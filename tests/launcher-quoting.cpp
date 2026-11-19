#define wmain themeLauncherEntry
#include "../launcher.cpp"
#undef wmain
#include <shellapi.h>
#include <cstdio>
int wmain() {
    for (const auto* value : {L"C:\\Bases\\Test\\", L"C:\\Data with spaces\\Base\\", L"C:\\", L"server\\ref", L"", L"a\\\"b"}) {
        const auto command = std::wstring(L"test.exe ") + quoteCommandArgument(value);
        int count{}; auto* args=CommandLineToArgvW(command.c_str(),&count);
        if (!args || count!=2 || std::wstring(args[1])!=value) return 1;
        LocalFree(args);
    }
    std::puts("Windows target argument round-trip: PASS");return 0;
}
