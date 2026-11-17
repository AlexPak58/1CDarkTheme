#define wmain launcherEntry
#include "launcher.cpp"
#undef wmain
#include <thread>

void check(bool success, const char* details)
{
    if (!success) {
        std::fprintf(stderr, "FAIL: %s\n", details);
        std::exit(1);
    }
}

int wmain(int argc, wchar_t** argv)
{
    check(argc == 3, "paths to verified and legacy DLL expected");
    check(isDesignerCommandLine(L"\"C:\\Program Files\\1cv8.exe\" DESIGNER /S\"server\\test\""),
          "explicit designer mode after quoted image");
    check(isDesignerCommandLine(L"1cv8.exe \"Designer\" /F\"C:\\test fit\""), "quoted designer mode");
    check(!isDesignerCommandLine(L"1cv8.exe ENTERPRISE /F\"DESIGNER\""), "Enterprise is rejected");
    check(!isDesignerCommandLine(L"1cv8.exe DESIGNER-extra"), "prefix is not designer");
    check(!isDesignerCommandLine(L" DESIGNER"), "empty argv0 does not qualify");
    ULONGLONG parsed{};
    DWORD parsedPid{};
    check(parseDecimal(L"18446744073709551615", parsed) && parsed == ~0ULL, "maximum FILETIME parsed");
    check(!parseDecimal(L"18446744073709551616", parsed) && !parseDecimal(L"-1", parsed), "reject overflow and sign");
    check(!parseDecimal(L"4294967296", parsedPid), "reject overflow PID");

    const auto pid = GetCurrentProcessId();
    FILETIME created{}, exited{}, kernel{}, user{};
    check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "read test process times");
    const auto creation = fileTimeValue(created);
    const Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    check(checkProcessLifetime(query, creation) == ERROR_SUCCESS, "exact process identity");
    check(checkProcessLifetime(query, creation + 1) == ERROR_PROCESS_ABORTED, "reject reused lifetime");
    std::wstring commandLine;
    check(readProcessCommandLine(query, commandLine) == ERROR_SUCCESS && commandLine == GetCommandLineW(),
          "counted command line via restricted query handle");
    check(attachToDesigner(pid, creation + 1, ThemeProfile{}) == 4, " CLI stale status");
    check(attachToDesigner(pid, creation, ThemeProfile{}) == 5, "CLI rejects other image");
    ULONG_PTR consoleSetter{};
    check(loadLaunchedTheme(query, pid, L"missing-tree-theme.dll", consoleSetter, "SetTreeTheme") ==
              ERROR_NOT_READY && !consoleSetter, "console enable defers before opening the DLL");

    {
        ThemeProcessLock owner;
        check(owner.acquire(pid, creation) == ERROR_SUCCESS, "acquire target mutex");
        std::thread contender([&] {
            ThemeProcessLock same, different;
            check(same.acquire(pid, creation, 0) == WAIT_TIMEOUT, "same lifetime serializes");
            check(different.acquire(pid, creation + 1, 0) == ERROR_SUCCESS, "new lifetime is isolated");
        });
        contender.join();
    }
    {
        ThemeProcessLock exitedOwner;
        std::thread abnormal([&] {
            check(exitedOwner.acquire(pid, creation) == ERROR_SUCCESS, "acquire before owner exits");
            // Keep the handle open after the owner thread exits, as another launcher might.
        });
        abnormal.join();
        ThemeProcessLock observer;
        check(observer.acquire(pid, creation, 0) == WAIT_ABANDONED, "interrupted operation is uncertain");
    }

    const std::wstring imagePath = argv[1];
    const std::wstring legacyPath = argv[2];
    const HMODULE loaded = LoadLibraryW(imagePath.c_str());
    check(loaded != nullptr, "load production fixture only into this test process");
    AttachImage image(imagePath);
    check(image.validate(), "production image has attach ABI and executable setter");
    AttachModules modules;
    check(snapshotAttachModules(pid, imagePath, modules) == ERROR_SUCCESS &&
          modules.ownBase == reinterpret_cast<ULONG_PTR>(loaded) && !modules.competingDll, "matching mapped path");
    check(image.remoteSetter(GetCurrentProcess(), modules) ==
          reinterpret_cast<ULONG_PTR>(GetProcAddress(loaded, "SetThemeProfile")), "mapped header and export validation");
    --modules.ownSize;
    check(!image.remoteSetter(GetCurrentProcess(), modules), "different mapped image size refused");

    const HMODULE legacy = LoadLibraryW(legacyPath.c_str());
    check(legacy != nullptr && legacy != loaded && GetProcAddress(legacy, "SetThemeProfile"), "load competing legacy fixture");
    AttachImage oldImage(legacyPath);
    check(!oldImage.validate(), "legacy marker absent");
    check(snapshotAttachModules(pid, imagePath, modules) == ERROR_SUCCESS && modules.competingDll,
          "same basename at a different path fails closed");
    check(snapshotAttachModules(pid, legacyPath, modules) == ERROR_SUCCESS &&
          modules.ownBase == reinterpret_cast<ULONG_PTR>(legacy) &&
          !image.remoteSetter(GetCurrentProcess(), modules), "production ABI rejects mapped legacy image");
    FreeLibrary(legacy);
    FreeLibrary(loaded);
    std::puts("PASS: lifetime, bounded mode query, CLI gates, mapped DLL ABI and competing basename");
}
