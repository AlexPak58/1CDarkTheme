#include "png_surface_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
LPVOID iatPage{};
bool fakeImage{true};
unsigned deniedRestore{};
unsigned deniedWritable{};
unsigned writableAttempts{};
bool faultNextCas{};
PVOID foreignOnWritable{};

SIZE_T WINAPI fixtureQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T size)
{
    const auto result = VirtualQuery(address, region, size);
    if (result && address == iatPage && fakeImage) region->Type = MEM_IMAGE;
    return result;
}

BOOL WINAPI fixtureProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous)
{
    if (address == iatPage && protection == PAGE_READWRITE) {
        ++writableAttempts;
        if (deniedWritable) {
            --deniedWritable;
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }
    if (address == iatPage && protection == PAGE_READONLY && deniedRestore) {
        --deniedRestore;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const auto result = VirtualProtect(address, size, protection, previous);
    if (result && address == iatPage && protection == PAGE_READWRITE && foreignOnWritable)
        *static_cast<PVOID*>(address) = foreignOnWritable;
    if (result && address == iatPage && protection == PAGE_READWRITE && faultNextCas) {
        faultNextCas = false;
        DWORD ignored{};
        if (!VirtualProtect(address, size, PAGE_READONLY, &ignored)) std::abort();
    }
    return result;
}
}

#define VirtualQuery fixtureQuery
#define VirtualProtect fixtureProtect
#include "png_surface_probe.cpp"
#undef VirtualProtect
#undef VirtualQuery

namespace {
void check(bool okay, const char* reason)
{
    if (!okay) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}

struct Image {
    int status{}, type{}, format{}, width{14}, height{16}, stride{64};
    double scaleX{1.25}, scaleY{1}, offsetX{-2}, offsetY{8};
};

unsigned creates{};
DWORD callerError{};
void* receivedSurface{};
ULONG_PTR fakePattern{};

void* original(void* surface)
{
    ++creates;
    receivedSurface = surface;
    callerError = GetLastError();
    SetLastError(0xB026);
    return &fakePattern;
}

template<int Image::*Member>
int getInt(const void* surface)
{
    return static_cast<const Image*>(surface)->*Member;
}

void getScale(const void* pointer, double* x, double* y)
{
    const auto& surface = *static_cast<const Image*>(pointer);
    *x = surface.scaleX;
    *y = surface.scaleY;
}

void getOffset(const void* pointer, double* x, double* y)
{
    const auto& surface = *static_cast<const Image*>(pointer);
    *x = surface.offsetX;
    *y = surface.offsetY;
}

template<class T, size_t N>
void put(std::array<ULONG_PTR, N>& storage, size_t offset, T value)
{
    check(offset <= sizeof(storage) && sizeof(value) <= sizeof(storage) - offset, "fixture bounds");
    std::memcpy(reinterpret_cast<BYTE*>(storage.data()) + offset, &value, sizeof(value));
}

template<size_t N>
ULONG_PTR ptr(const std::array<ULONG_PTR, N>& storage)
{
    return reinterpret_cast<ULONG_PTR>(storage.data());
}

struct Fixture {
    static constexpr ULONG_PTR c = 0x4C0, e = 0x550, trackS = 0x610, arrowS = 0x6A0;
    std::array<ULONG_PTR, 0x800 / 8> stack{};
    std::array<ULONG_PTR, 0x110 / 8> scroller{};
    std::array<ULONG_PTR, 0xC0 / 8> window{};
    std::array<ULONG_PTR, 0x18 / 8> component{};
    std::unique_ptr<std::array<ULONG_PTR, 0x40000 / 8>> wbase =
        std::make_unique<std::array<ULONG_PTR, 0x40000 / 8>>();
    ProbeBindings binding{0x10000000, 0x20000000, ptr(*wbase),
        {original, getInt<&Image::status>, getInt<&Image::type>, getInt<&Image::format>,
         getInt<&Image::width>, getInt<&Image::height>, getInt<&Image::stride>, getScale, getOffset}};
    Image image;

    void route(DWORD part, bool tree, DWORD offset)
    {
        stack.fill(0);
        const auto wb = binding.wbase;
        const auto ga = binding.graphics;
        const auto layout = ptr(scroller) + 0xA8;
        const auto slotOffset = 0xA8 + 0x20 + (offset - 0x40) / 2;
        const auto rectOffset = 0xA8 + offset;
        const bool track = part == 3;
        const auto s = track ? trackS : arrowS;
        put(stack, 0, ga + 0x13482);
        put(stack, 0x260, ga + 0x4911);
        put(stack, 0x430, ga + 0x91BA);
        put(stack, c, wb + (track ? 0x19410 : 0x1963F));
        if (!track) put(stack, e, wb + (part == 0 ? 0x19452 : 0x1948B));
        put(stack, track ? c - 8 : e - 0x10, ptr(component));
        put(stack, s, wb + 0x1E9E0);
        put(stack, s + 0x90, wb + 0x1D5D9);
        put(stack, s + 0x28, BYTE{1});
        put(stack, s - 0x38, ptr(scroller));
        put(stack, s - 0x40, layout);
        put(stack, s - 0x20, static_cast<ULONG_PTR>(offset));
        put(stack, s - 0x10, ptr(scroller) + slotOffset);
        put(stack, s - 0x18, ptr(scroller) + rectOffset);
        put(scroller, 0, wb + (tree ? 0x3CD00 : 0x3CA78));
        put(scroller, 0x20, ptr(window));
        put(scroller, slotOffset, ptr(component));
        put(scroller, rectOffset, RECT{-30, 3, -16, 44});
        put(window, 0, binding.frame + (tree ? 0xA885D0 : 0xABEDB0));
        put(window, 0xB8, ptr(scroller));
        put(*wbase, 0x3CA78 + 0x58, wb + 0x1D600);
        put(*wbase, 0x3CA78 + 0xC0, wb + 0x1E860);
        put(*wbase, 0x3CD00 + 0x58, wb + 0x1D600);
        put(*wbase, 0x3CD00 + 0xC0, wb + 0x1EFE0);
        put(component, 0, wb + 0x3D348);
        put(component, 8, (offset - 0x40) / 0x10);
        put(component, 0xC, DWORD{2});
        put(component, 0x10, DWORD{5});
        put(*wbase, 0x3D348 + 0x10, wb + 0x19290);
        put(*wbase, 0x3D348 + 0x30, wb + 0x1B930);
    }

    void expect(Stage stage, const char* reason)
    {
        PngSurfaceIdentity id{};
        check(locatePaint(ptr(stack), binding, id) == stage, reason);
    }
};

void testRoutes(Fixture& f)
{
    for (DWORD part : {0UL, 1UL, 3UL}) {
        for (DWORD offset : {0x40UL, 0x50UL}) {
            f.route(part, false, offset);
            PngSurfaceIdentity id{};
            check(locatePaint(ptr(f.stack), f.binding, id) == Stage::Accepted &&
                  id.part == part && id.componentOffset == offset && id.left == -30 && id.bottom == 44 &&
                  id.orientation == (offset - 0x40) / 0x10 && id.hotId == 2 && id.pressedId == 5 &&
                  id.windowTableRva == 0xABEDB0 && id.scrollerTableRva == 0x3CA78, "editor tracks and arrows");
        }
    }
    f.route(3, true, 0x40);
    f.expect(Stage::Accepted, "tree vertical track");
    f.route(1, true, 0x40);
    f.expect(Stage::Accepted, "tree vertical arrow");
    f.route(3, true, 0x50);
    f.expect(Stage::Owner, "tree horizontal denied");
    f.route(3, false, 0x40);
    ++f.stack[0];
    f.expect(Stage::OtherCall, "unrelated Cairo caller");
    for (size_t slot : {0x260, 0x430}) {
        f.route(3, false, 0x40);
        ++f.stack[slot / 8];
        f.expect(Stage::GraphicsChain, "graphics return mismatch");
    }
    f.route(3, false, 0x40);
    put(f.stack, Fixture::c, f.binding.wbase + 0x19859);
    f.expect(Stage::PaintRoute, "offscreen thumb stage excluded");
    f.route(0, false, 0x40);
    ++f.stack[Fixture::e / 8];
    f.expect(Stage::PaintRoute, "unknown arrow paint");
    for (size_t slot : {Fixture::trackS, Fixture::trackS + 0x90, Fixture::trackS + 0x28}) {
        f.route(3, false, 0x40);
        ++f.stack[slot / 8];
        f.expect(Stage::PaintRoute, "outer paint return or flag mismatch");
    }
    for (size_t offset : {0x38, 0x40, 0x20, 0x10, 0x18}) {
        f.route(3, false, 0x40);
        ++f.stack[(Fixture::trackS - offset) / 8];
        f.expect(Stage::Owner, "outer stack ownership mismatch");
    }
    for (size_t offset : {0, 0xB8}) {
        f.route(3, true, 0x40);
        ++f.window[offset / 8];
        f.expect(Stage::Owner, "window table and backlink must match");
    }
    for (size_t offset : {0x58, 0xC0}) {
        f.route(3, false, 0x40);
        ++(*f.wbase)[(0x3CA78 + offset) / 8];
        f.expect(Stage::Owner, "scroller methods must match");
    }
    f.route(3, false, 0x40);
    put(f.scroller, 0xE8, RECT{0, 0, 0, 0});
    f.expect(Stage::Owner, "empty rectangle rejected");
    for (size_t offset : {Fixture::c - 8, Fixture::e - 0x10}) {
        f.route(offset == Fixture::c - 8 ? 3 : 0, false, 0x40);
        ++f.stack[offset / 8];
        f.expect(Stage::Component, "component must be in the slot");
    }
    for (size_t offset : {0x00, 0x08, 0x0C, 0x10}) {
        f.route(3, false, 0x40);
        put(f.component, offset, DWORD{MAXDWORD});
        f.expect(Stage::Component, "component type and numeric IDs constrained");
    }
    for (size_t offset : {0x10, 0x30}) {
        f.route(3, true, 0x40);
        ++(*f.wbase)[(0x3D348 + offset) / 8];
        f.expect(Stage::Component, "component paint and getter must match");
    }
    PngSurfaceIdentity id{};
    check(locatePaint(panel_detail::limit - 7, f.binding, id) == Stage::ReadFailure, "out-of-stack address rejected");
}

void testMetadataAndCollection(Fixture& f)
{
    PngSurfaceIdentity id{};
    const auto measure = [&] { return readSurface(f.binding.cairo, &f.image, id); };
    check(measure() == Stage::Accepted && id.width == 14 && id.height == 16 && id.stride == 64 &&
          id.format == 0 && id.deviceScaleX == 1.25 && id.deviceScaleY == 1 &&
          id.deviceOffsetX == -2 && id.deviceOffsetY == 8, "reads image metadata without a pixel buffer");
    f.image.status = 2;
    check(measure() == Stage::SurfaceError, "Cairo errors rejected");
    f.image = {};
    f.image.type = 2;
    check(measure() == Stage::SurfaceType, "non-image rejected");
    f.image = {};
    f.image.stride = 52;
    check(measure() == Stage::InvalidMetrics, "minimum stride enforced");
    f.image.stride = 57;
    check(measure() == Stage::InvalidMetrics, "alignment enforced");
    f.image = {};
    f.image.scaleX = (std::numeric_limits<double>::infinity)();
    check(measure() == Stage::InvalidMetrics, "non-finite scale rejected");
    f.image.scaleX = 0;
    check(measure() == Stage::InvalidMetrics, "zero scale rejected");
    f.image = {};
    f.image.offsetX = (std::numeric_limits<double>::quiet_NaN)();
    check(measure() == Stage::InvalidMetrics, "NaN offset rejected");
    f.image = {};
    f.image.format = 6;
    check(measure() == Stage::InvalidMetrics, "unknown image format has explicit status");

    f.image = {};
    f.route(3, false, 0x50);
    record(ptr(f.stack), &f.image, f.binding);
    record(ptr(f.stack), &f.image, f.binding);
    check(observations.sampleCount == 1 && observations.samples[0].calls == 2, "identical groups aggregated");
    for (size_t i = 1; i <= pngSurfaceCapacity; ++i) {
        f.image.offsetX = static_cast<double>(i);
        record(ptr(f.stack), &f.image, f.binding);
    }
    PngSurfaceTrace trace{};
    check(copySamples(trace) == ERROR_SUCCESS && trace.sampleCount == pngSurfaceCapacity && trace.capacityDrops == 1 &&
          trace.stages[static_cast<size_t>(Stage::Accepted)] == pngSurfaceCapacity + 2,
          "bounded distinct groups and accepted totals");
    observations = {};
}

void testWrapper(Fixture& f)
{
    bindings = f.binding;
    published.store(&bindings);
    DWORD expectedCalls{};
    const auto call = [&] {
        SetLastError(0xB025);
        const auto result = createPattern(&f.image);
        const auto outgoing = GetLastError();
        check(result == &fakePattern && ++expectedCalls == creates && receivedSurface == &f.image &&
              callerError == 0xB025 && outgoing == 0xB026, "single original call, same argument, result and LastError");
    };
    deadline.store(GetTickCount64() + pngSurfaceCaptureMs);
    call();
    AcquireSRWLockExclusive(&collectorLock);
    call();
    ReleaseSRWLockExclusive(&collectorLock);
    PngSurfaceTrace closed{};
    deadline.store(0);
    check(copySamples(closed) == ERROR_SUCCESS && closed.stages[static_cast<size_t>(Stage::OtherCall)] == 1 &&
          closed.busyDrops == 1, "active observation and bounded contention");
    call();
    check(observations.stages[static_cast<size_t>(Stage::OtherCall)] == 1 && busyDrops.load() == 1,
          "late call remains passthrough");
}

void testTransaction()
{
    iatPage = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(iatPage != nullptr, "allocate synthetic IAT page");
    auto* const slot = static_cast<PVOID*>(iatPage);
    const auto real = reinterpret_cast<PVOID>(&original);
    const auto wrapper = reinterpret_cast<PVOID>(&createPattern);
    auto* const foreign = reinterpret_cast<PVOID>(ULONG_PTR{0x778899});
    *slot = real;
    DWORD protection{};
    check(VirtualProtect(iatPage, sizeof(PVOID), PAGE_READONLY, &protection), "fixture RO IAT");
    IatPatch patch{reinterpret_cast<ULONG_PTR>(iatPage), slot, real};
    fakeImage = false;
    check(changeSlot(patch, true) == ERROR_INVALID_STATE, "private page cannot be an IAT");
    fakeImage = true;
    auto misaligned = patch;
    misaligned.slot = reinterpret_cast<PVOID volatile*>(reinterpret_cast<ULONG_PTR>(iatPage) + 1);
    check(changeSlot(misaligned, true) == ERROR_INVALID_STATE, "unaligned slot denied");
    check(changeSlot(patch, true) == ERROR_SUCCESS && patch.installed && verifyIat(patch, wrapper, protection) &&
          protection == PAGE_READONLY, "one CAS installs and restores page protection");
    check(changeSlot(patch, false) == ERROR_SUCCESS && !patch.installed && verifyIat(patch, real, protection),
          "one CAS removes and restores page protection");

    PngSurfaceTrace cleanup{};
    check(changeSlot(patch, true) == ERROR_SUCCESS, "install for cleanup retry");
    deadline.store(GetTickCount64() + pngSurfaceCaptureMs);
    deniedWritable = 1;
    writableAttempts = 0;
    AcquireSRWLockExclusive(&collectorLock);
    finishPatch(patch, cleanup);
    check(!deadline.load() && writableAttempts == 2 && !patch.installed && !cleanup.cleanupError &&
          cleanup.snapshotError == ERROR_ACCESS_DENIED && cleanup.finalProtection == PAGE_READONLY &&
          verifyIat(patch, real, protection), "transient writable failure recovered with the collector still locked");
    check(copySamples(cleanup) == ERROR_TIMEOUT, "snapshot wait is bounded independently of teardown");
    ReleaseSRWLockExclusive(&collectorLock);

    check(changeSlot(patch, true) == ERROR_SUCCESS, "install for persistent cleanup fault");
    cleanup = {};
    deniedWritable = 3;
    writableAttempts = 0;
    finishPatch(patch, cleanup);
    check(writableAttempts == 2 && deniedWritable == 1 && patch.installed &&
          cleanup.snapshotError == ERROR_ACCESS_DENIED && cleanup.cleanupError == ERROR_ACCESS_DENIED &&
          verifyIat(patch, wrapper, protection), "persistent refusal leaves passive wrapper with a bounded ambiguity");
    deniedWritable = 0;
    check(changeSlot(patch, false) == ERROR_SUCCESS, "reset persistent-fault fixture");

    check(changeSlot(patch, true) == ERROR_SUCCESS, "install for unwind-time CAS fault");
    cleanup = {};
    faultNextCas = true;
    writableAttempts = 0;
    finishPatch(patch, cleanup);
    check(writableAttempts == 2 && !patch.installed && !cleanup.cleanupError &&
          cleanup.snapshotError == ERROR_NOACCESS && verifyIat(patch, real, protection),
          "CAS fault recovered without losing first error");

    check(changeSlot(patch, true) == ERROR_SUCCESS, "install for a foreign CAS during retry");
    cleanup = {};
    deniedWritable = 1;
    writableAttempts = 0;
    foreignOnWritable = foreign;
    finishPatch(patch, cleanup);
    check(writableAttempts == 2 && !patch.installed && cleanup.snapshotError == ERROR_ACCESS_DENIED &&
          cleanup.cleanupError == ERROR_INVALID_DATA && verifyIat(patch, foreign, protection),
          "retry leaves foreign pointer intact and reports the final CAS conflict");

    foreignOnWritable = foreign;
    check(changeSlot(patch, true) == ERROR_INVALID_DATA && !patch.installed && verifyIat(patch, foreign, protection),
          "install conflict preserves foreign pointer");
    foreignOnWritable = real;
    check(changeSlot(patch, true) == ERROR_SUCCESS, "install after isolated fixture reset");
    foreignOnWritable = foreign;
    check(changeSlot(patch, false) == ERROR_INVALID_DATA && !patch.installed && verifyIat(patch, foreign, protection),
          "uninstall conflict preserves foreign pointer");
    foreignOnWritable = real;
    deniedRestore = 1;
    check(changeSlot(patch, true) == ERROR_ACCESS_DENIED && !patch.installed && verifyIat(patch, real, protection),
          "install restore failure rolls back slot and recovers protection");
    check(changeSlot(patch, true) == ERROR_SUCCESS, "reinstall fixture for unhook restore failure");
    foreignOnWritable = nullptr;
    deniedRestore = 1;
    check(changeSlot(patch, false) == ERROR_ACCESS_DENIED && !patch.installed && verifyIat(patch, real, protection),
          "unhook restore failure never reinstalls wrapper");
    deniedRestore = 2;
    check(changeSlot(patch, true) == ERROR_ACCESS_DENIED && !patch.installed && !verifyIat(patch, real, protection) &&
          protection == PAGE_READWRITE && *slot == real, "failed retry leaves explicit protection ambiguity");
    check(VirtualFree(iatPage, 0, MEM_RELEASE) != FALSE, "release synthetic IAT page");
    iatPage = nullptr;
}

void testAbi()
{
    PngSurfaceTrace trace{};
    check(ScrollerPngSurfaceProbeAbiV1(nullptr) == 1 && pngSurfaceCaptureMs == 60000, "ABI1 and one minute");
    trace.version = 2;
    check(RunScrollerPngSurfaceProbeV1(&trace) == ERROR_INVALID_PARAMETER && !attempted, "reject wrong version");
    trace.version = pngSurfaceProbeVersion;
    trace.size = sizeof(DWORD) * 2;
    check(RunScrollerPngSurfaceProbeV1(&trace) == ERROR_INVALID_PARAMETER && !attempted, "reject short header");
    trace.size = sizeof(trace);
    auto* const page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(page != nullptr, "truncated payload fixture");
    std::memcpy(page, &trace, sizeof(DWORD) * 2);
    check(RunScrollerPngSurfaceProbeV1(page) == ERROR_INVALID_PARAMETER && !attempted, "reject truncated allocation");
    check(VirtualFree(page, 0, MEM_RELEASE) != FALSE, "release truncated payload");
    attempted = true;
    check(RunScrollerPngSurfaceProbeV1(&trace) == ERROR_ALREADY_EXISTS && trace.patchError == ERROR_ALREADY_EXISTS,
          "attempt cannot rearm");
}
}

int main()
{
    Fixture fixture;
    testAbi();
    testRoutes(fixture);
    testMetadataAndCollection(fixture);
    testWrapper(fixture);
    testTransaction();
    std::puts("PASS: PNG routes and metadata, bounded observation, transparent wrapper, IAT CAS and restoration");
}
