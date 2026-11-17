#include "png_surface_probe.hpp"
#include <cstring>

namespace {
constexpr ULONG_PTR fakeGraphics = 0x180000000, fakeCairo = 0x181000000;
DWORD fixtureProtection = PAGE_READONLY;
bool wrongPointer{}, wrongAllocation{};
SIZE_T WINAPI fixtureQuery(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    const auto current = reinterpret_cast<ULONG_PTR>(address);
    if (current == fakeGraphics + pngSurfaceIatRva || current == fakeCairo + pngSurfaceOriginalRva) {
        const bool slot = current == fakeGraphics + pngSurfaceIatRva;
        *region = {};
        region->AllocationBase = reinterpret_cast<LPVOID>(slot ? fakeGraphics + (wrongAllocation ? 4096 : 0) : fakeCairo);
        region->BaseAddress = const_cast<LPVOID>(address);
        region->RegionSize = 4096;
        region->State = MEM_COMMIT;
        region->Type = MEM_IMAGE;
        region->Protect = slot ? fixtureProtection : PAGE_EXECUTE_READ;
        return sizeof(*region);
    }
    return VirtualQueryEx(process, address, region, length);
}
BOOL WINAPI fixtureRead(HANDLE process, LPCVOID address, LPVOID output, SIZE_T length, SIZE_T* sizeRead)
{
    if (reinterpret_cast<ULONG_PTR>(address) == fakeGraphics + pngSurfaceIatRva && length == sizeof(ULONG_PTR)) {
        const auto pointer = fakeCairo + pngSurfaceOriginalRva + (wrongPointer ? 1 : 0);
        std::memcpy(output, &pointer, length);
        *sizeRead = length;
        return TRUE;
    }
    return ReadProcessMemory(process, address, output, length, sizeRead);
}
}
#define VirtualQueryEx fixtureQuery
#define ReadProcessMemory fixtureRead
#define PNG_SURFACE_LAUNCHER_TEST
#include "png_surface_probe_launcher.cpp"
#undef ReadProcessMemory
#undef VirtualQueryEx

void require(bool okay, const char* message)
{
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
LPVOID lastPayload{};
DWORD callError{};
DWORD fixtureCall(HANDLE, ULONG_PTR, LPVOID payload, DWORD& result)
{
    lastPayload = payload;
    if (callError) return callError;
    static_cast<PngSurfaceTrace*>(payload)->elapsedMs = pngSurfaceCaptureMs;
    result = ERROR_NOT_READY;
    return ERROR_SUCCESS;
}
DWORD WINAPI fixtureWorker(LPVOID payload)
{
    static_cast<PngSurfaceTrace*>(payload)->elapsedMs = pngSurfaceCaptureMs;
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv)
{
    require(argc == 3, "expected probe and production DLL paths");
    wchar_t executable[] = L"png-surface-probe-run.exe", command[] = L"--png-surface-probe";
    wchar_t other[] = L"--attach", zero[] = L"0", one[] = L"1", overflow[] = L"4294967296";
    wchar_t* args[]{executable, command, zero, one};
    require(pngSurfaceCommand(1, args) == 2, "empty CLI cannot launch platform");
    require(pngSurfaceCommand(4, args) == 2, "zero PID rejected");
    args[1] = other; args[2] = one;
    require(pngSurfaceCommand(4, args) == 2, "production attach never dispatched");
    args[1] = command; args[2] = overflow;
    require(pngSurfaceCommand(4, args) == 2, "PID overflow rejected");
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "get lifetime");
    require(pngSurfaceToDesigner(GetCurrentProcessId(), fileTimeValue(created) + 1) == 4, "wrong lifetime rejected");
    require(pngSurfaceToDesigner(GetCurrentProcessId(), fileTimeValue(created)) == 5, "non-platform target rejected");

    const ProbeModule graphics{fakeGraphics, 0x100000, L"grphcs.dll", L""};
    const ProbeModule cairo{fakeCairo, 0x100000, L"cairo.dll", L""};
    DWORD protection{};
    require(originalPngIat(GetCurrentProcess(), graphics, cairo, protection) && protection == PAGE_READONLY,
            "exact original image IAT admitted");
    wrongPointer = true;
    require(!originalPngIat(GetCurrentProcess(), graphics, cairo, protection), "foreign pointer rejected");
    wrongPointer = false; wrongAllocation = true;
    require(!originalPngIat(GetCurrentProcess(), graphics, cairo, protection), "foreign image region rejected");
    wrongAllocation = false; fixtureProtection = PAGE_READWRITE;
    require(!originalPngIat(GetCurrentProcess(), graphics, cairo, protection), "mutable IAT preflight rejected");
    fixtureProtection = PAGE_READONLY | PAGE_GUARD;
    require(!originalPngIat(GetCurrentProcess(), graphics, cairo, protection), "guard page rejected");
    fixtureProtection = PAGE_READONLY;

    PngSurfaceTrace trace{};
    auto call = callPngSurfaceProbe(GetCurrentProcess(), 0, trace, fixtureCall);
    require(call.received && call.payloadReleased && !call.transportError && call.workerResult == ERROR_NOT_READY,
            "completed worker payload received and released");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(lastPayload, &region, sizeof(region)) && region.State == MEM_FREE, "payload released");
    callError = WAIT_TIMEOUT;
    call = callPngSurfaceProbe(GetCurrentProcess(), 0, trace, fixtureCall);
    require(!call.received && !call.payloadReleased && call.transportError == WAIT_TIMEOUT &&
            VirtualQuery(lastPayload, &region, sizeof(region)) && region.State == MEM_COMMIT,
            "ambiguous worker retains payload without terminate/retry");
    require(VirtualFreeEx(GetCurrentProcess(), lastPayload, 0, MEM_RELEASE), "free simulated timeout fixture");
    callError = ERROR_ACCESS_DENIED;
    call = callPngSurfaceProbe(GetCurrentProcess(), 0, trace, fixtureCall);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "failed worker creation releases unused payload");
    call = callPngSurfaceProbe(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(&fixtureWorker), trace);
    require(call.received && call.payloadReleased && !call.transportError && !call.workerResult, "real local worker");
    trace.originalProtection = trace.finalProtection = PAGE_READONLY;
    require(!pngSurfaceReplyError(trace, call, PAGE_READONLY), "clean empty capture is valid but not complete");
    trace.cleanupError = ERROR_WRITE_FAULT;
    call.transportError = ERROR_ACCESS_DENIED;
    require(pngSurfaceReplyError(trace, call, PAGE_READONLY) == 7, "cleanup uncertainty wins over payload error");
    trace.cleanupError = 0; call.transportError = 0;
    trace.finalProtection = PAGE_READWRITE;
    require(pngSurfaceReplyError(trace, call, PAGE_READONLY) == 7, "protection drift rejected");
    trace.finalProtection = PAGE_READONLY;
    trace.sampleCount = 1; trace.stages[static_cast<size_t>(PngSurfaceStage::Accepted)] = 2;
    auto& sample = trace.samples[0];
    sample.calls = 2;
    auto& id = sample.identity;
    id = {0xABEDB0, 0x3CA78, 0x40, 3, 0, 0, 0, 0, 0, 0, 17, 400, 0, 0, 17, 4, 68, 0, 1, 1, 0, 0};
    require(validPngTrace(trace), "valid numeric surface capture accepted");
    id.width = 32768; require(!validPngTrace(trace), "oversized dimensions rejected"); id.width = 17;
    id.deviceScaleX = std::numeric_limits<double>::quiet_NaN();
    require(!validPngTrace(trace), "NaN rejected before JSON"); id.deviceScaleX = 1;
    id.stride = 64; require(!validPngTrace(trace), "short stride rejected"); id.stride = 68;
    id.windowTableRva = 0xA885D0;
    require(!validPngTrace(trace), "wrong Window/scroller pair rejected"); id.windowTableRva = 0xABEDB0;
    id.part = 2; require(!validPngTrace(trace), "thumb not falsely attributed"); id.part = 3;
    trace.capacityDrops = 1; require(!validPngTrace(trace), "inconsistent loss counts rejected");
    ++trace.stages[static_cast<size_t>(PngSurfaceStage::Accepted)];
    require(!validPngTrace(trace), "capacity loss before full buffer is impossible");
    trace.capacityDrops = 0;
    trace.sampleCount = static_cast<DWORD>(pngSurfaceCapacity);
    for (DWORD i = 1; i < trace.sampleCount; ++i) {
        trace.samples[i] = sample;
        trace.samples[i].calls = 1;
        trace.samples[i].identity.right += static_cast<LONG>(i);
    }
    trace.stages[static_cast<size_t>(PngSurfaceStage::Accepted)] = pngSurfaceCapacity + 2;
    trace.capacityDrops = 1;
    require(validPngTrace(trace), "full buffer can account for capacity loss");
    trace.version = 0; require(!validPngTrace(trace), "wrong ABI rejected");

    const HMODULE probe = LoadLibraryW(argv[1]), production = LoadLibraryW(argv[2]);
    require(probe && production, "fixture DLLs load in test process");
    AttachImage probeImage(argv[1]), productionImage(argv[2]);
    require(probeImage.validate(pngSurfaceRunExport, pngSurfaceAbiExport), "probe ABI accepted");
    require(!probeImage.validate(), "probe cannot satisfy production ABI");
    require(!productionImage.validate(pngSurfaceRunExport, pngSurfaceAbiExport), "production cannot satisfy probe ABI");
    require(probeImage.validate(pngSurfaceRunExport, pngSurfaceAbiExport), "restore validator");
    AttachModules mapped;
    require(!snapshotAttachModules(GetCurrentProcessId(), argv[1], mapped, pngSurfaceDllName) &&
            mapped.ownBase == reinterpret_cast<ULONG_PTR>(probe) && !mapped.competingDll &&
            probeImage.remoteSetter(GetCurrentProcess(), mapped) ==
                reinterpret_cast<ULONG_PTR>(GetProcAddress(probe, pngSurfaceRunExport)), "mapped export checked");
    FreeLibrary(probe); FreeLibrary(production);
    std::puts("PASS: PNG probe exclusive CLI, IAT pre/post, metadata bounds, ABI and payload lifetime");
}
