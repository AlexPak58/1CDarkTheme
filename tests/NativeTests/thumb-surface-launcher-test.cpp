#include "thumb_surface_probe.hpp"
#include "png_surface_probe.hpp"
#include <cstring>
#include <utility>

namespace {
constexpr ULONG_PTR fakeGraphics = 0x180000000, fakeTheme = 0x181000000, fakeEntry = fakeTheme + 0x1200;
DWORD slotProtection = PAGE_READONLY;
DWORD codeProtection = PAGE_EXECUTE_READ;
ULONG_PTR hookPointer = fakeEntry;
bool wrongAllocation{}, shortRead{};

SIZE_T WINAPI fixtureQuery(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T length)
{
    const auto value = reinterpret_cast<ULONG_PTR>(address);
    const bool slot = value == fakeGraphics + pngSurfaceIatRva;
    if (!slot && (value < fakeTheme + 0x1000 || value >= fakeTheme + 0x2000))
        return VirtualQueryEx(process, address, region, length);
    *region = {};
    region->AllocationBase = reinterpret_cast<LPVOID>(slot ? fakeGraphics : fakeTheme + (wrongAllocation ? 0x1000 : 0));
    region->BaseAddress = const_cast<LPVOID>(address);
    region->RegionSize = 4096;
    region->State = MEM_COMMIT;
    region->Type = MEM_IMAGE;
    region->Protect = slot ? slotProtection : codeProtection;
    return sizeof(*region);
}

BOOL WINAPI fixtureRead(HANDLE process, LPCVOID address, LPVOID output, SIZE_T length, SIZE_T* read)
{
    if (reinterpret_cast<ULONG_PTR>(address) != fakeGraphics + pngSurfaceIatRva || length != sizeof(hookPointer))
        return ReadProcessMemory(process, address, output, length, read);
    std::memcpy(output, &hookPointer, length);
    *read = shortRead ? length - 1 : length;
    return TRUE;
}
} // namespace

#define VirtualQueryEx fixtureQuery
#define ReadProcessMemory fixtureRead
#define THUMB_SURFACE_LAUNCHER_TEST
#include "thumb_surface_probe_launcher.cpp"
#undef ReadProcessMemory
#undef VirtualQueryEx

namespace {
void require(bool passed, const char* description)
{
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

LPVOID savedPayload{};
ThumbWorkerResult fixtureResult{};
ThumbWorkerResult fixtureWorker(HANDLE, ULONG_PTR, LPVOID payload)
{
    savedPayload = payload;
    if (!fixtureResult.error) static_cast<ThumbSurfaceTrace*>(payload)->elapsedMs = thumbSurfaceCaptureMs;
    return fixtureResult;
}

DWORD WINAPI localWorker(LPVOID payload)
{
    static_cast<ThumbSurfaceTrace*>(payload)->elapsedMs = thumbSurfaceCaptureMs;
    return ERROR_SUCCESS;
}

void testCli()
{
    wchar_t exe[] = L"thumb-surface-probe-run.exe", command[] = L"--thumb-surface-probe", other[] = L"--attach";
    wchar_t zero[] = L"0", one[] = L"1", overflow[] = L"4294967296", timeOverflow[] = L"18446744073709551616";
    wchar_t digest[] = L"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    wchar_t* args[]{exe, command, zero, one, digest};
    require(thumbSurfaceCommand(4, args) == 2 && thumbSurfaceCommand(5, args) == 2, "requires exact command and nonzero PID");
    args[1] = other; args[2] = one;
    require(thumbSurfaceCommand(5, args) == 2, "cannot dispatch normal attach");
    args[1] = command; args[2] = overflow;
    require(thumbSurfaceCommand(5, args) == 2, "rejects PID overflow");
    args[2] = one; args[3] = timeOverflow;
    require(thumbSurfaceCommand(5, args) == 2, "rejects creation time overflow");
    args[3] = one; args[4] = digest + 1;
    require(thumbSurfaceCommand(5, args) == 2, "requires 64 hex characters");
    args[4] = digest; digest[63] = L'g';
    require(thumbSurfaceCommand(5, args) == 2, "rejects non-hex characters");
    digest[63] = L'9';
    Sha256Hex hash{};
    require(thumbExpectedHash(digest, hash) && hash[0] == L'A' && !hash[64], "normalizes hex digest");
    ULONGLONG creation{};
    require(!readProcessCreation(GetCurrentProcess(), creation), "fixture process creation");
    require(thumbSurfaceToDesigner(GetCurrentProcessId(), creation + 1, hash) == 4, "rejects stale process identity");
    require(thumbSurfaceToDesigner(GetCurrentProcessId(), creation, hash) == 5, "rejects non-Designer process");
}

void testIat()
{
    ThumbModules modules{};
    modules[ThumbGraphics] = {fakeGraphics, 0x100000, L"grphcs.dll"};
    modules[ThumbTheme] = {fakeTheme, 0x100000, L"tree-theme.dll"};
    const auto process = GetCurrentProcess();
    const auto before = thumbHookPointer(process, modules[ThumbGraphics], modules[ThumbTheme]);
    require(before == fakeEntry && thumbHookUnchanged(process, modules, before), "stable readonly owned IAT");
    hookPointer += 16;
    require(!thumbHookUnchanged(process, modules, before), "rejects even a different owned code pointer");
    hookPointer = fakeGraphics + pngSurfaceOriginalRva;
    require(!thumbHookUnchanged(process, modules, before), "rejects foreign target");
    hookPointer = fakeEntry; wrongAllocation = true;
    require(!thumbHookUnchanged(process, modules, before), "rejects foreign allocation base");
    wrongAllocation = false; slotProtection = PAGE_READWRITE;
    require(!thumbHookUnchanged(process, modules, before), "rejects writable slot");
    slotProtection = PAGE_READONLY | PAGE_GUARD;
    require(!thumbHookUnchanged(process, modules, before), "rejects guarded slot");
    slotProtection = PAGE_READONLY; codeProtection = PAGE_EXECUTE_READWRITE;
    require(!thumbHookUnchanged(process, modules, before), "rejects mutable hook code");
    codeProtection = PAGE_EXECUTE_READ; shortRead = true;
    require(!thumbHookUnchanged(process, modules, before), "rejects a partial pointer read");
    shortRead = false;
    require(sameThumbModules(modules, modules), "same modules admitted");
    auto replaced = modules;
    ++replaced[ThumbTheme].base;
    require(!sameThumbModules(modules, replaced), "rejects replacement module after capture");
}

void testPayload()
{
    const auto process = GetCurrentProcess();
    ThumbSurfaceTrace trace{};
    auto call = callThumbSurfaceProbe(process, 0, trace, fixtureWorker);
    require(call.received && call.payloadReleased && !call.transportError, "completed call copies and releases payload");
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(savedPayload, &region, sizeof(region)) && region.State == MEM_FREE, "completed payload is free");
    for (const auto error : {static_cast<DWORD>(WAIT_TIMEOUT), static_cast<DWORD>(ERROR_INVALID_HANDLE)}) {
        fixtureResult = {error, 0, true};
        call = callThumbSurfaceProbe(process, 0, trace, fixtureWorker);
        require(!call.received && !call.payloadReleased && call.transportError == error &&
                VirtualQuery(savedPayload, &region, sizeof(region)) && region.State == MEM_COMMIT,
                "inconclusive wait preserves worker memory");
        require(VirtualFreeEx(process, savedPayload, 0, MEM_RELEASE), "free simulated timeout fixture");
    }
    fixtureResult = {ERROR_ACCESS_DENIED};
    call = callThumbSurfaceProbe(process, 0, trace, fixtureWorker);
    require(!call.received && call.payloadReleased && call.transportError == ERROR_ACCESS_DENIED,
            "worker creation failure releases unused payload");
    call = callThumbSurfaceProbe(process, reinterpret_cast<ULONG_PTR>(&localWorker), trace);
    require(call.received && call.payloadReleased && !call.transportError && !call.worker.exitCode,
            "real local worker payload transfer");
    require(!thumbReplyError(trace, call), "empty capture is a valid incomplete result");
    --trace.elapsedMs;
    require(thumbReplyError(trace, call) == 7, "rejects short capture");
    trace.elapsedMs = thumbSurfaceCaptureMs;
    trace.endError = ERROR_INVALID_STATE;
    require(thumbReplyError(trace, call) == 7, "failed target postcondition is ambiguous");
    trace.endError = 0; trace.snapshotError = ERROR_TIMEOUT;
    require(thumbReplyError(trace, call) == 7, "failed target snapshot is ambiguous");
    trace.snapshotError = 0; trace.elapsedMs = 0; trace.startError = ERROR_NOT_READY;
    require(thumbReplyError(trace, call) == 7, "inconsistent worker and start error rejected");
    call.worker.exitCode = ERROR_NOT_READY;
    require(thumbReplyError(trace, call) == 1, "consistent pre-arm rejection returned as error");
}

void testTrace()
{
    ThumbSurfaceTrace trace{};
    using Stage = ThumbSurfaceStage;
    const std::array routeStages{
        std::pair{Stage::DrawReturn, "drawReturn"}, std::pair{Stage::DelegatedDraw, "delegatedDraw"},
        std::pair{Stage::DelegatedFrames, "delegatedFrames"}, std::pair{Stage::PaintReturn, "paintReturn"},
        std::pair{Stage::OwnerReturn, "ownerReturn"},
        std::pair{Stage::OuterReturn, "outerReturn"}, std::pair{Stage::Vdc, "vdc"},
        std::pair{Stage::Orientation, "orientation"}, std::pair{Stage::ThumbPresent, "thumbPresent"}
    };
    for (const auto& [stage, label] : routeStages) {
        const auto index = static_cast<size_t>(stage);
        require(std::string_view(thumbStageNames[index]) == label, "route rejection has the matching JSON key");
        trace.stages[index] = index + 1;
    }
    require(validThumbTrace(trace), "route rejection counters alone are valid without accepted samples");
    auto& accepted = trace.stages[static_cast<size_t>(ThumbSurfaceStage::Accepted)];
    auto& sample = trace.samples[0];
    auto& id = sample.identity;
    trace.sampleCount = 1; accepted = sample.calls = 2;
    id = {0xABEDB0, 0x3CA78, 0x50, 0x1A030, 1, 5, 0, 2, 1, 1, 0, 0,
          0, 0, 20, 50, 1, 2, 10, 15, 0, 1, 13, 17, 52, 0, 1, 1, 0, 0};
    require(validThumbTrace(trace), "valid horizontal image metadata accepted");
    id.enabled = 0; require(!validThumbTrace(trace), "cache state must match enabled and interactive flags"); id.enabled = 1;
    id.drawReturnRva = 0x19AF5; require(!validThumbTrace(trace), "piece must match orientation"); id.drawReturnRva = 0x1A030;
    id.deviceOffsetX = std::numeric_limits<double>::infinity();
    require(!validThumbTrace(trace), "rejects nonfinite fields before JSON"); id.deviceOffsetX = 0;
    id.stride = 48; require(!validThumbTrace(trace), "rejects insufficient stride"); id.stride = 52;
    id.surfaceType = 1; require(!validThumbTrace(trace), "only image surface admitted"); id.surfaceType = 0;
    id.scrollerTableRva = 0x3CD00; require(!validThumbTrace(trace), "rejects mismatched owners"); id.scrollerTableRva = 0x3CA78;
    trace.samples[1] = sample;
    trace.sampleCount = 2; accepted = 4;
    require(!validThumbTrace(trace), "rejects duplicate groups");
    trace.sampleCount = 1; accepted = 3; trace.capacityDrops = 1;
    require(!validThumbTrace(trace), "capacity drops require full buffer");
    for (DWORD i = 1; i < thumbSurfaceCapacity; ++i) {
        trace.samples[i] = sample;
        trace.samples[i].calls = 1;
        trace.samples[i].identity.right += static_cast<LONG>(i);
    }
    trace.sampleCount = static_cast<DWORD>(thumbSurfaceCapacity); accepted = thumbSurfaceCapacity + 2;
    require(validThumbTrace(trace), "full capture accounts for exact capacity drops");
    --accepted;
    require(!validThumbTrace(trace), "aggregate accepted counts must match rows and loss");
    ++accepted; trace.version = 1;
    require(!validThumbTrace(trace), "rejects version 1 payload");
    trace.version = thumbSurfaceProbeVersion;
    trace.size = 18576;
    require(!validThumbTrace(trace), "rejects version 1 payload size");
    trace.size = static_cast<DWORD>(sizeof(trace) - sizeof(trace.stages[0]));
    require(!validThumbTrace(trace), "rejects a payload missing one stage counter");

    ThumbCoverage coverage;
    const auto addPieces = [&](unsigned mask) {
        for (const auto draw : thumbDrawReturns) {
            if (!(thumbPiece(draw) & mask)) continue;
            id.drawReturnRva = draw;
            coverage.add(id);
        }
    };
    addPieces(thumbHorizontalPieces);
    id.orientation = 0; addPieces(thumbVerticalPieces);
    require(!coverage.complete(), "both editor axes alone are incomplete");
    id.scrollerTableRva = 0x3CD00; addPieces(thumbVerticalPieces);
    require(coverage.complete(), "all orientation-specific pieces complete without optional shared piece");
    addPieces(thumbSharedPiece);
    require(coverage.complete(), "optional shared piece does not invalidate complete coverage");
}

void testImages(const std::wstring& diagnostic, const std::wstring& normal)
{
    ThumbModules modules{};
    for (size_t i = 0; i < modules.size(); ++i) modules[i] = {1, 1, thumbModuleNames[i]};
    require(!validateThumbPlatformModules(modules, [](const std::wstring&, const wchar_t*) { return true; }),
            "all supported platform images present");
    require(validateThumbPlatformModules(modules, [](const std::wstring& name, const wchar_t*) { return name != L"imageui.dll"; }) ==
                ERROR_REVISION_MISMATCH, "imageui hash mandatory");
    modules[ThumbImage] = {};
    require(validateThumbPlatformModules(modules, [](const std::wstring&, const wchar_t*) { return true; }) ==
                ERROR_MOD_NOT_FOUND, "image module mandatory");

    Sha256Hex digest{};
    require(readSha256(diagnostic, digest) && matchesSha256(diagnostic, digest.data()) &&
            !matchesSha256(normal, digest.data()), "trusted diagnostic digest excludes ordinary production DLL");
    {
        AttachImage image(normal);
        require(image.validate() && !image.validate(thumbSurfaceRunExport, thumbSurfaceAbiExport),
                "ordinary production has v10 ABI without thumb worker");
    }
    {
        AttachImage image(diagnostic);
        ThumbModules loaded;
        require(!captureThumbModules(GetCurrentProcessId(), loaded) && thumbModulePath(loaded[ThumbTheme], diagnostic),
                "unique loaded diagnostic path resolves");
        const auto& theme = loaded[ThumbTheme];
        require(!thumbModulePath(theme, normal), "same basename at different path rejected");
        const auto mapped = GetModuleHandleW(diagnostic.c_str());
        const auto run = mapped ? reinterpret_cast<ULONG_PTR>(GetProcAddress(mapped, thumbSurfaceRunExport)) : 0;
        require(run && thumbMappedRun(GetCurrentProcess(), image, theme) == run,
                "mapped diagnostic v10 and probe v2 exports agree with trusted PE");
        require(!GetProcAddress(mapped, "RunThumbSurfaceProbeV1") && !GetProcAddress(mapped, "ThumbSurfaceProbeAbiV1"),
                "diagnostic DLL advertises only probe v2");
        auto wrong = theme;
        ++wrong.size;
        require(!thumbMappedRun(GetCurrentProcess(), image, wrong), "wrong mapped image size rejected");
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    require(argc == 3, "expected diagnostic and ordinary production DLL paths");
    testCli();
    testIat();
    testPayload();
    testTrace();
    testImages(std::filesystem::absolute(argv[1]).wstring(), std::filesystem::absolute(argv[2]).wstring());
    std::puts("PASS: thumb probe CLI, module identity, IAT pre/post, payload lifetime and bounded metadata");
}
