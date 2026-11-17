#include "thumb_surface_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace {
void* iatPage{};
inline constexpr ULONG_PTR fixtureIatRva = 0xC7BE0;
bool poisonQuery{};
LPCVOID deniedQuery{}, watchedQuery{};
size_t watchedQueries{};

SIZE_T WINAPI fixtureQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION region, SIZE_T size)
{
    if (address == watchedQuery) ++watchedQueries;
    if (address == deniedQuery) return 0;
    const auto result = VirtualQuery(address, region, size);
    if (result && iatPage && address == iatPage) {
        region->Type = MEM_IMAGE;
        region->AllocationBase = reinterpret_cast<PVOID>(reinterpret_cast<ULONG_PTR>(iatPage) - fixtureIatRva);
    }
    if (poisonQuery) SetLastError(0xB024);
    return result;
}
}

#define VirtualQuery fixtureQuery
#ifndef THEME_THUMB_SURFACE_PROBE
#define THEME_THUMB_SURFACE_PROBE
#endif
#include "thumb_surface_capture.hpp"
#undef VirtualQuery

namespace {
using namespace thumb_surface_capture;
static_assert(fixtureIatRva == scroller_png_hook::iatRva);

void check(bool okay, const char* reason)
{
    if (!okay) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}

template<size_t N> ULONG_PTR ptr(const std::array<ULONG_PTR, N>& storage)
{ return reinterpret_cast<ULONG_PTR>(storage.data()); }

template<class T, size_t N> void put(std::array<ULONG_PTR, N>& storage, size_t offset, T value)
{
    check(offset <= sizeof(storage) && sizeof(value) <= sizeof(storage) - offset, "fixture bounds");
    std::memcpy(reinterpret_cast<BYTE*>(storage.data()) + offset, &value, sizeof(value));
}

const DWORD cairoImageBackend{};
struct Surface {
    ULONG_PTR backend{reinterpret_cast<ULONG_PTR>(&cairoImageBackend)};
    // The synthetic getter results follow padding for the hash-gated native layout checks.
    std::array<ULONG_PTR, (0x188 - 8) / 8> nativeFields{};
    int status{}, type{}, format{}, width{14}, height{16}, stride{64};
    double sx{1.25}, sy{1}, ox{-2}, oy{8};
};

template<int Surface::*Member> int metric(const void* p) { return static_cast<const Surface*>(p)->*Member; }
void scale(const void* p, double* x, double* y)
{ const auto& s = *static_cast<const Surface*>(p); *x = s.sx; *y = s.sy; }
void offset(const void* p, double* x, double* y)
{ const auto& s = *static_cast<const Surface*>(p); *x = s.ox; *y = s.oy; }
int fault(const void*) { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); return 0; }
int expireInGetter(const void*) { deadline.store(0); return 0; }
int zero(const void*) { return 0; }
bool imageGetterCalled{};
int markImageGetter(const void*) { imageGetterCalled = true; return 0; }

struct Fixture {
    std::array<ULONG_PTR, 0xB50 / 8> stack{};
    std::array<ULONG_PTR, 0x110 / 8> scroller{};
    std::array<ULONG_PTR, 0xC0 / 8> window{};
    std::array<ULONG_PTR, 0x38 / 8> component{};
    std::array<ULONG_PTR, 2> drawingObject{};
    ULONG_PTR cacheWitness{}, rasterInterface{};
    std::unique_ptr<std::array<ULONG_PTR, 0x40000 / 8>> wbase =
        std::make_unique<std::array<ULONG_PTR, 0x40000 / 8>>();
    std::unique_ptr<std::array<ULONG_PTR, 0x15000 / 8>> imageUi =
        std::make_unique<std::array<ULONG_PTR, 0x15000 / 8>>();
    Images routes{0x10000000, 0x20000000, ptr(*wbase), ptr(*imageUi)};
    scroller_png::CairoApi api{metric<&Surface::status>, metric<&Surface::type>, metric<&Surface::format>,
        metric<&Surface::width>, metric<&Surface::height>, metric<&Surface::stride>, scale, offset};
    scroller_png_hook::Binding binding{routes.frame, routes.graphics, routes.wbase, 0, api, nullptr, nullptr};
    Surface surface;

    void route(DWORD draw = 0x19AF5, DWORD componentOffset = 0x40, bool tree = false)
    {
        stack.fill(0); scroller.fill(0); window.fill(0); component.fill(0); drawingObject.fill(0);
        const auto wb = routes.wbase, ga = routes.graphics, iu = routes.imageUi;
        const auto layout = ptr(scroller) + 0xA8;
        const auto slotOffset = 0xC8 + (componentOffset - 0x40) / 2;
        const auto rectOffset = 0xA8 + componentOffset;
        put(stack, 0, ga + 0x13482);
        put(stack, 0x260, ga + 0xAE2A2);
        put(stack, 0x360, ga + 0xB6B09);
        put(stack, 0x420, iu + 0x63BB);
        put(stack, 0x490, iu + 0xAE67);
        put(stack, 0x660, iu + 0x90AB);
        put(stack, drawFrame, wb + draw);
        put(stack, paintFrame, wb + 0x194BE);
        put(stack, ownerFrame, wb + 0x1E9E0);
        put(stack, ownerFrame + 0x90, wb + 0x1D5D9);
        put(stack, paintFrame + 0x70, ULONG_PTR{0x99000});
        put(stack, paintFrame - 0x30, ULONG_PTR{0x99000});
        put(stack, 0x648, ULONG_PTR{0x99000});
        put(stack, drawFrame + 0x78, ULONG_PTR{0x1234567}); // Read as VDC only in the stale research note.
        put(stack, drawFrame + 0x80, ptr(drawingObject) + 8);
        put(stack, 0x638, ptr(drawingObject));
        rasterInterface = iu + 0x13D08;
        put(stack, 0x500, reinterpret_cast<ULONG_PTR>(&rasterInterface));
        put(stack, ownerFrame - 0x38, ptr(scroller));
        put(stack, ownerFrame - 0x40, layout);
        put(stack, ownerFrame - 0x20, static_cast<ULONG_PTR>(componentOffset));
        put(stack, ownerFrame - 0x10, ptr(scroller) + slotOffset);
        put(stack, ownerFrame - 0x18, ptr(scroller) + rectOffset);
        put(stack, paintFrame - 0x10, ptr(scroller) + 0x28);
        put(stack, paintFrame - 0x18, ptr(component));
        put(stack, paintFrame + 0xD8, RECT{-29, 5, -17, 20});
        put(stack, paintFrame + 0xEA, BYTE{1});
        const auto tableOffset = tree ? 0x3CD00 : 0x3CA78;
        put(scroller, 0, wb + tableOffset);
        put(scroller, 0x20, ptr(window));
        put(scroller, slotOffset, ptr(component));
        put(scroller, rectOffset, RECT{-30, 3, -16, 44});
        put(window, 0, routes.frame + (tree ? 0xA885D0 : 0xABEDB0));
        put(window, 0xB8, ptr(scroller));
        put(*wbase, tableOffset + 0x58, wb + 0x1D600);
        put(*wbase, tableOffset + 0xC0, wb + (tree ? 0x1EFE0 : 0x1E860));
        put(component, 0, wb + 0x3D348);
        put(component, 8, (componentOffset - 0x40) / 0x10);
        put(component, 0x28, reinterpret_cast<ULONG_PTR>(&cacheWitness));
        put(*wbase, 0x3D348 + 0x10, wb + 0x19290);
        put(*wbase, 0x3D348 + 0x30, wb + 0x1B930);
        put(*wbase, 0x3D348 + 0x38, wb + 0x1B960);
        put(drawingObject, 8, iu + 0x14218);
        put(*imageUi, 0x14218 + 0x18, iu + 0x8FE0);
        put(*imageUi, 0x13D08 + 0x18, iu + 0x6340);
        put(*imageUi, 0x14090 + 0x18, iu + 0x8FE0);
        put(*imageUi, 0x13C80 + 0x18, iu + 0x6340);
        state(1, 1, 0, 0, 0);
    }

    void state(BYTE enabled, BYTE interactive, DWORD hot, DWORD pressed, DWORD value)
    {
        put(scroller, 0x98, enabled);
        put(stack, paintFrame + 0x28, interactive);
        put(stack, ownerFrame + 0x28, interactive);
        put(component, 0xC, hot);
        put(component, 0x10, pressed);
        put(component, 0x30, value);
    }

    Stage locate(ThumbSurfaceIdentity& id)
    {
        CacheObservation cache;
        return locatePaint(ptr(stack), routes, id, cache);
    }

    void expect(Stage expected, const char* name)
    {
        ThumbSurfaceIdentity id{};
        check(locate(id) == expected, name);
    }
};

void testKnownThumbSurfaces()
{
    ThumbSurfaceIdentity id{};
    id.windowTableRva = 0xABEDB0; id.scrollerTableRva = 0x3CA78;
    id.enabled = id.interactive = 1;
    const DWORD vertical[]{0x19AF5, 0x19B78, 0x19BFD, 0x19ED0};
    const DWORD horizontal[]{0x19FAD, 0x1A030, 0x1A0B5, 0x19ED0};
    const int vRgb[]{5, 1, 5, 6}, hRgb[]{5, 1, 5, 5}, vArgb[]{6, 1, 6, 7};
    for (size_t i = 0; i < 4; ++i) {
        id.drawReturnRva = vertical[i];
        check(thumb_paint::knownSurface(id, 1, 16, vRgb[i]) &&
              !thumb_paint::knownSurface(id, 0, 20, vArgb[i]), "editor RGB24 observed; editor ARGB32 pending");
        id.windowTableRva = 0xA885D0; id.scrollerTableRva = 0x3CD00;
        check(thumb_paint::knownSurface(id, 0, 20, vArgb[i]) &&
              thumb_paint::knownSurface(id, 1, 16, vRgb[i]) &&
              !thumb_paint::knownSurface(id, 0, 21, vArgb[i]) &&
              !thumb_paint::knownSurface(id, 1, 16, vRgb[i] + 1), "both tree formats require exact part dimensions");
        id.windowTableRva = 0xABEDB0; id.scrollerTableRva = 0x3CA78;
        id.orientation = 1; id.drawReturnRva = horizontal[i];
        check(thumb_paint::knownSurface(id, 1, hRgb[i], 16) &&
              !thumb_paint::knownSurface(id, 1, 16, hRgb[i]), "horizontal editor RGB24 part geometry");
        id.orientation = 0;
    }
    id.drawReturnRva = vertical[0];
    id.enabled = 0;
    check(!thumb_paint::knownSurface(id, 1, 16, 5), "disabled thumb awaits live coverage");
    id.enabled = 1; id.interactive = 0;
    check(!thumb_paint::knownSurface(id, 1, 16, 5), "noninteractive thumb awaits live coverage");
    id.interactive = 1;
    check(!thumb_paint::knownSurface(id, 2, 16, 5), "unconfirmed image format rejected");
    id.drawReturnRva = 0x19410;
    check(!thumb_paint::knownSurface(id, 1, 16, 5), "track return not a thumb part");
}
void testRoutes(Fixture& f)
{
    for (const auto draw : resizedReturns) {
        const bool vertical = draw <= 0x19ED0;
        const DWORD componentOffset = vertical ? 0x40 : 0x50;
        f.route(draw, componentOffset);
        ThumbSurfaceIdentity id{};
        check(f.locate(id) == Stage::Accepted && id.drawReturnRva == draw &&
            id.componentOffset == componentOffset && id.orientation == (vertical ? 0UL : 1UL) &&
            id.windowTableRva == 0xABEDB0 && id.scrollerTableRva == 0x3CA78 && id.left == -30 &&
            id.thumbLeft == -29 && id.thumbBottom == 20, "all seven editor draws and both coordinate sets");
        if (vertical) {
            f.route(draw, 0x40, true);
            check(f.locate(id) == Stage::Accepted && id.windowTableRva == 0xA885D0 &&
                id.scrollerTableRva == 0x3CD00, "vertical tree draws");
        }
    }
    f.route(0x19ED0, 0x50); f.expect(Stage::Accepted, "shared centered draw can be horizontal");
    f.route(0x1A0B5, 0x40); f.expect(Stage::Orientation, "horizontal draw on vertical owner rejected");
    f.route(0x1A0B5, 0x50, true); f.expect(Stage::Owner, "horizontal tree rejected");
    f.route(); ++f.stack[0]; f.expect(Stage::OtherCall, "unrelated caller");
    for (size_t slot : {0x260, 0x360, 0x420, 0x490, 0x660}) {
        f.route(); ++f.stack[slot / 8]; f.expect(Stage::GraphicsChain, "graphics chain must be exact");
    }
    f.route(); put(f.stack, 0x260, f.routes.graphics + 0x4911); f.expect(Stage::GraphicsChain, "reject staging blit");
    const std::pair<ULONG_PTR, Stage> returns[]{{drawFrame, Stage::DrawReturn}, {paintFrame, Stage::PaintReturn},
        {ownerFrame, Stage::OwnerReturn}, {ownerFrame + 0x90, Stage::OuterReturn}};
    for (const auto& [slot, rejection] : returns) {
        f.route(); ++f.stack[slot / 8]; f.expect(rejection, "reject exact mismatched call frame");
    }
    f.route(); put(f.stack, drawFrame, f.routes.imageUi + 0x7BD0);
    f.expect(Stage::DelegatedDraw, "count the unconfirmed extra imageui return");
    put(f.stack, 0x7F0, f.routes.wbase + 0x19AF5);
    put(f.stack, 0x960, f.routes.wbase + 0x194BE);
    put(f.stack, 0xAB0, f.routes.wbase + 0x1E9E0);
    put(f.stack, 0xB40, f.routes.wbase + 0x1D5D9);
    f.expect(Stage::DelegatedFrames, "count confirmed shifted returns without accepting them");
    for (const size_t slot : {0x7F0, 0x960, 0xAB0, 0xB40}) {
        ++f.stack[slot / 8];
        f.expect(Stage::DelegatedDraw, "all shifted returns must match to count a confirmed chain");
        --f.stack[slot / 8];
    }
    for (const size_t slot : {paintFrame + 0x70, paintFrame - 0x30, ULONG_PTR{0x648}}) {
        f.route(); ++f.stack[slot / 8]; f.expect(Stage::Vdc, "VDC copies must match");
    }
    f.route(); put(f.stack, 0x478, f.stack[0x648 / 8]); put(f.stack, 0x648, ULONG_PTR{0});
    f.expect(Stage::Vdc, "VDC at the raster return's old offset cannot replace the AC50 saved RSI");
    f.route(); put(f.stack, paintFrame + 0xEA, BYTE{0}); f.expect(Stage::ThumbPresent, "absent thumb rejected");
    f.route(); put(f.stack, paintFrame + 0xD8, RECT{}); f.expect(Stage::Owner, "empty thumb geometry rejected");
    for (const size_t slot : {ownerFrame - 0x38, ownerFrame - 0x40, ownerFrame - 0x20, ownerFrame - 0x10,
                              ownerFrame - 0x18, paintFrame - 0x10}) {
        f.route(); ++f.stack[slot / 8]; f.expect(Stage::Owner, "stack owner relations must match");
    }
    f.route(); ++f.window[0xB8 / 8]; f.expect(Stage::Owner, "owner backlink must match");
    f.route(); ++(*f.wbase)[(0x3CA78 + 0xC0) / 8]; f.expect(Stage::Owner, "owner dispatch must match");
    f.route(); ++f.stack[(paintFrame - 0x18) / 8]; f.expect(Stage::Component, "component must be in owner slot");
    f.route(); ++(*f.wbase)[(0x3D348 + 0x38) / 8]; f.expect(Stage::Component, "thumb metrics dispatch must match");
    f.route(); put(f.component, 0xC, DWORD{6}); f.expect(Stage::Component, "hot ID limited to known values");
    for (const size_t slot : {drawFrame + 0x80, ULONG_PTR{0x638}}) {
        f.route(); ++f.stack[slot / 8]; f.expect(Stage::Cache, "drawing interface and wrapper must match");
    }
    f.route(); put(f.stack, 0x468, f.stack[0x638 / 8]); put(f.stack, 0x638, ULONG_PTR{0});
    f.expect(Stage::Cache, "drawing wrapper belongs to the AC50 frame");
    f.route(); put(f.stack, 0x330, f.stack[0x500 / 8]); put(f.stack, 0x500, ULONG_PTR{0});
    f.expect(Stage::Cache, "raster interface belongs to the AC50 frame");
    f.route(); put(f.stack, 0x500, f.routes.imageUi + 0x13D08);
    f.expect(Stage::Cache, "raster table itself cannot stand in for its interface instance");
    f.route(); f.rasterInterface = f.routes.imageUi + 0x13D10; f.expect(Stage::Cache, "raster interface table must match");
    f.route(); put(f.stack, 0x500, ULONG_PTR{0}); f.expect(Stage::Cache, "null raster interface rejected");
    f.route(); put(f.stack, 0x500, ULONG_PTR{1}); f.expect(Stage::ReadFailure, "invalid raster interface rejected");
    f.route(); put(f.component, 0x28, ULONG_PTR{0}); f.expect(Stage::Cache, "empty cache rejected");
    f.route(); f.expect(Stage::Accepted, "drawing wrapper may differ from component cache");
    f.route(); put(f.drawingObject, 8, f.routes.imageUi + 0x13EB0); f.expect(Stage::Cache, "unresized class rejected");
    f.route(); ++(*f.imageUi)[(0x14218 + 0x18) / 8]; f.expect(Stage::Cache, "QI method must match");
    f.route(); ++(*f.imageUi)[(0x13D08 + 0x18) / 8]; f.expect(Stage::Cache, "raster method must match");

    f.route(); f.state(0, 1, 5, 5, 1); f.expect(Stage::Accepted, "disabled overrides pressed and hot");
    f.state(1, 0, 5, 5, 0); f.expect(Stage::Accepted, "noninteractive overrides pressed and hot");
    f.state(1, 1, 5, 5, 3); f.expect(Stage::Accepted, "pressed overrides hot");
    f.state(1, 1, 5, 0, 2); f.expect(Stage::Accepted, "hot cache");
    f.state(1, 1, 5, 5, 2); f.expect(Stage::State, "reject stale hover cache while pressed");
    f.state(1, 1, 0, 0, 4); f.expect(Stage::State, "reject unknown cache state");
    f.state(1, 1, 0, 0, 0); put(f.stack, ownerFrame + 0x28, BYTE{0});
    f.expect(Stage::State, "incoming interaction byte must match");
    ThumbSurfaceIdentity id{};
    CacheObservation cache;
    check(locatePaint(1, f.routes, id, cache) == Stage::ReadFailure && cache.gate == CacheCounter::Count,
        "unreadable stack denied before cache diagnostics");
}

void testMetadata(Fixture& f)
{
    ThumbSurfaceIdentity id{};
    const auto read = [&] { return readSurface(f.api, &f.surface, id); };
    check(read() == Stage::Accepted && id.format == 0 && id.width == 14 && id.height == 16 && id.stride == 64 &&
        id.deviceScaleX == 1.25 && id.deviceOffsetX == -2, "only Cairo metadata read");
    f.surface.status = 2; check(read() == Stage::SurfaceError, "Cairo error denied");
    f.surface = {}; f.surface.type = 2; check(read() == Stage::SurfaceType, "non-image denied");
    f.surface = {}; f.surface.format = 6; check(read() == Stage::InvalidMetrics, "unknown format denied");
    f.surface = {}; f.surface.stride = 52; check(read() == Stage::InvalidMetrics, "undersize stride denied");
    f.surface.stride = 57; check(read() == Stage::InvalidMetrics, "unaligned stride denied");
    f.surface = {}; f.surface.sx = 0; check(read() == Stage::InvalidMetrics, "zero scale denied");
    f.surface.sx = (std::numeric_limits<double>::infinity)();
    check(read() == Stage::InvalidMetrics, "infinite scale denied");
    f.surface = {}; f.surface.oy = (std::numeric_limits<double>::quiet_NaN)();
    check(read() == Stage::InvalidMetrics, "NaN offset denied");
    f.surface = {};
    auto faulting = f.api;
    faulting.height = fault;
    check(readSurface(faulting, &f.surface, id) == Stage::ReadFailure, "access fault contained around getters");

    void* guard = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
    check(guard != nullptr, "allocate guard fixture");
    check(readSurface(f.api, guard, id) == Stage::ReadFailure, "guarded surface denied without getters");
    f.route();
    put(f.scroller, 0x20, reinterpret_cast<ULONG_PTR>(guard));
    f.expect(Stage::ReadFailure, "guarded owner denied");
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(guard, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "guard not consumed");
    f.surface.backend = reinterpret_cast<ULONG_PTR>(guard);
    check(read() == Stage::ReadFailure, "guarded backend denied before image getter");
    f.surface = {};
    check(VirtualQuery(guard, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "backend guard not consumed");
    check(VirtualFree(guard, 0, MEM_RELEASE), "free guard fixture");

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const auto pageSize = sys.dwPageSize;
    auto* split = static_cast<BYTE*>(VirtualAlloc(nullptr, 2ULL * pageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(split != nullptr, "allocate split surface fixture");
    DWORD previous{};
    check(VirtualProtect(split + pageSize, pageSize, PAGE_READWRITE | PAGE_GUARD, &previous), "guard second page");
    auto* truncated = split + pageSize - 0x100;
    std::memcpy(truncated, &f.surface.backend, sizeof(f.surface.backend));
    auto fake = f.api;
    fake.status = zero; fake.type = zero; fake.format = markImageGetter;
    check(readSurface(fake, truncated, id) == Stage::ReadFailure && !imageGetterCalled,
        "reject known guard in image getter span before invoking accessor");
    check(VirtualQuery(split + pageSize, &region, sizeof(region)) && (region.Protect & PAGE_GUARD), "getter-span guard intact");
    check(VirtualFree(split, 0, MEM_RELEASE), "free split fixture");
}

ULONGLONG hits(const ThumbSurfaceTrace& t, Stage stage) { return t.stages[static_cast<size_t>(stage)]; }

void testCacheDiagnostics(Fixture& f)
{
    using Counter = CacheCounter;
    const auto iu = f.routes.imageUi;
    images = f.routes;
    armedBinding = &f.binding;
    f.binding.api.status = markImageGetter;
    deadline.store(GetTickCount64() + thumbSurfaceCaptureMs);
    const auto expect = [&](Stage stage, Counter gate, Counter methods, const char* reason) {
        const auto previous = observations;
        imageGetterCalled = false;
        observe(ptr(f.stack), &f.surface, f.binding);
        check(hits(observations, stage) == hits(previous, stage) + 1 &&
            imageGetterCalled == (stage == Stage::Accepted), reason);
        auto expectedCounters = previous.cacheCounters;
        if (gate != Counter::Count) ++expectedCounters[static_cast<size_t>(gate)];
        if (methods != Counter::Count) ++expectedCounters[static_cast<size_t>(methods)];
        check(observations.cacheCounters == expectedCounters, reason);
    };
    const auto tables = [&](ULONG_PTR draw, ULONG_PTR raster) {
        f.route();
        put(f.drawingObject, 8, iu + draw);
        f.rasterInterface = iu + raster;
    };

    tables(0x14218, 0x13D08);
    expect(Stage::Accepted, Counter::OriginalPair, Counter::MethodsMatch, "original known pair reaches Cairo");
    tables(0x14090, 0x13D08);
    expect(Stage::Cache, Counter::AlternateDrawPair, Counter::MethodsMatch, "alternate draw pair matches methods but never reaches Cairo");
    tables(0x14218, 0x13C80);
    expect(Stage::Cache, Counter::AlternateRasterPair, Counter::MethodsMatch, "alternate raster pair matches methods but never reaches Cairo");
    tables(0x14090, 0x13C80);
    expect(Stage::Accepted, Counter::AlternateBothPair, Counter::MethodsMatch, "confirmed aggregate pair reaches Cairo");
    f.state(1, 1, 0, 0, 2);
    expect(Stage::State, Counter::AlternateBothPair, Counter::MethodsMatch, "aggregate pair still requires matching cache state");
    tables(0x14090, 0x13C80);
    put(f.stack, paintFrame + 0xD8, RECT{});
    expect(Stage::Owner, Counter::AlternateBothPair, Counter::MethodsMatch, "aggregate pair still requires valid thumb geometry");

    auto* guard = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD));
    check(guard != nullptr, "allocate unknown-table guard fixture");
    f.route();
    put(f.drawingObject, 8, reinterpret_cast<ULONG_PTR>(guard));
    watchedQuery = guard + 0x18;
    expect(Stage::Cache, Counter::UnknownPair, Counter::Count, "unknown pair is classified without reading its table");
    check(!watchedQueries, "unknown table method slot was not queried");
    watchedQuery = nullptr;
    check(VirtualFree(guard, 0, MEM_RELEASE), "free unknown-table guard fixture");

    f.route();
    put(f.component, 0x28, ULONG_PTR{0});
    expect(Stage::Cache, Counter::NullCache, Counter::Count, "null cache separated");
    f.route();
    put(f.stack, drawFrame + 0x80, ULONG_PTR{8});
    expect(Stage::Cache, Counter::InvalidDrawQi, Counter::Count, "invalid draw QI separated");
    f.route();
    ++f.stack[0x638 / 8];
    expect(Stage::Cache, Counter::DrawingObjectMismatch, Counter::Count, "wrong drawing object separated");
    f.route();
    put(f.stack, 0x500, ULONG_PTR{0});
    expect(Stage::Cache, Counter::NullRasterQi, Counter::Count, "null raster QI separated");
    f.route();
    put(f.stack, 0x500, ULONG_PTR{1});
    expect(Stage::ReadFailure, Counter::TablesReadFailure, Counter::Count, "guarded table read failure separated");
    panel_detail::Reader reader;
    CacheObservation cache;
    check(checkCache(reader, ptr(f.stack), 0, iu, cache) == Stage::ReadFailure &&
        cache.gate == Counter::InputsReadFailure && cache.methods == Counter::Count, "guarded input read failure separated");

    f.route();
    deniedQuery = reinterpret_cast<LPCVOID>(iu + 0x14218 + 0x18);
    expect(Stage::ReadFailure, Counter::OriginalPair, Counter::MethodsReadFailure, "original method read failure keeps ReadFailure");
    tables(0x14090, 0x13C80);
    deniedQuery = reinterpret_cast<LPCVOID>(iu + 0x14090 + 0x18);
    expect(Stage::ReadFailure, Counter::AlternateBothPair, Counter::MethodsReadFailure, "aggregate method read failure reports ReadFailure");
    tables(0x14090, 0x13D08);
    expect(Stage::Cache, Counter::AlternateDrawPair, Counter::MethodsReadFailure, "mixed method read failure keeps Cache");
    deniedQuery = nullptr;

    f.route();
    ++(*f.imageUi)[(0x14218 + 0x18) / 8];
    expect(Stage::Cache, Counter::OriginalPair, Counter::WrongDrawMethod, "wrong draw method separated");
    f.route();
    ++(*f.imageUi)[(0x13D08 + 0x18) / 8];
    expect(Stage::Cache, Counter::OriginalPair, Counter::WrongRasterMethod, "wrong raster method separated");
    ++(*f.imageUi)[(0x14218 + 0x18) / 8];
    expect(Stage::Cache, Counter::OriginalPair, Counter::WrongBothMethods, "both wrong methods separated");
    tables(0x14090, 0x13C80);
    ++(*f.imageUi)[(0x13C80 + 0x18) / 8];
    expect(Stage::Cache, Counter::AlternateBothPair, Counter::WrongRasterMethod, "aggregate pair still requires exact methods");

    tables(0x14090, 0x13C80);
    ++f.window[0xB8 / 8];
    expect(Stage::Owner, Counter::Count, Counter::Count, "owner must match before pair classification");
    tables(0x14090, 0x13C80);
    ++f.stack[(paintFrame - 0x18) / 8];
    expect(Stage::Component, Counter::Count, Counter::Count, "component must match before pair classification");

    deadline.store(0);
    ThumbSurfaceTrace snapshot;
    check(copySamples(snapshot) == ERROR_SUCCESS && snapshot.cacheCounters == observations.cacheCounters &&
        snapshot.sampleCount == 1 && snapshot.samples[0].calls == 2 &&
        snapshot.samples[0].identity.width == f.surface.width && snapshot.samples[0].identity.height == f.surface.height &&
        snapshot.samples[0].identity.stride == f.surface.stride, "snapshot aggregates metadata from both admitted pairs");
    observations = {};
    f.binding.api.status = f.api.status;
}

void testCompactGrouping(Fixture& f)
{
    observations = {};
    busyDrops.store(0);
    f.route();
    f.surface = {};
    images = f.routes;
    armedBinding = &f.binding;
    deadline.store(GetTickCount64() + thumbSurfaceCaptureMs);
    const auto collect = [&] { observe(ptr(f.stack), &f.surface, f.binding); };
    ThumbSurfaceGeometry minimum{}, maximum{};
    ThumbSurfaceIdentity first{};
    for (LONG i = 0; i < 600; ++i) {
        const LONG x = i == 598 ? (std::numeric_limits<LONG>::min)() :
                       i == 599 ? (std::numeric_limits<LONG>::max)() - 100 : i * 13 - 4000;
        const LONG y = i == 598 ? (std::numeric_limits<LONG>::max)() - 100 :
                       i == 599 ? (std::numeric_limits<LONG>::min)() : 3000 - i * 11;
        put(f.scroller, 0xE8, RECT{x, y, x + 20, y + 50});
        put(f.stack, paintFrame + 0xD8, RECT{x + 1, y + 2, x + 10, y + 15});
        ThumbSurfaceIdentity id{};
        check(f.locate(id) == Stage::Accepted && readSurface(f.api, &f.surface, id) == Stage::Accepted,
            "position fixture passes unchanged geometry and Cairo admission");
        const auto geometry = thumbSurfaceGeometry(id);
        if (!i) { first = id; minimum = maximum = geometry; }
        for (size_t coordinate = 0; coordinate < geometry.size(); ++coordinate) {
            if (geometry[coordinate] < minimum[coordinate]) minimum[coordinate] = geometry[coordinate];
            if (geometry[coordinate] > maximum[coordinate]) maximum[coordinate] = geometry[coordinate];
        }
        collect();
    }
    check(observations.sampleCount == 1 && observations.samples[0].calls == 600 &&
        !observations.capacityDrops && observations.samples[0].identity == first &&
        observations.samples[0].geometryMin == minimum && observations.samples[0].geometryMax == maximum,
        "hundreds of valid positions compact without loss and retain first identity and signed coordinate extrema");
    put(f.stack, paintFrame + 0xD8, RECT{0, 0, 0, 2});
    collect();
    check(observations.sampleCount == 1 && observations.samples[0].calls == 600 && hits(observations, Stage::Owner) == 1,
        "invalid geometry rejected before grouping");
    deadline.store(0);

    // Exercise the actual collector key for every non-coordinate identity field.
    // These synthetic one-field changes test key completeness, independently of route admission.
    observations = {};
    const CacheObservation cache{};
    record(Stage::Accepted, first, cache);
    const auto split = [&](const ThumbSurfaceIdentity& id) {
        const auto before = observations.sampleCount;
        check(!sameThumbSurfaceGroup(first, id), "semantic field remains in group key");
        record(Stage::Accepted, id, cache);
        check(observations.sampleCount == before + 1, "changed semantic field creates a collector group");
    };
    for (auto field : {&ThumbSurfaceIdentity::windowTableRva, &ThumbSurfaceIdentity::scrollerTableRva,
         &ThumbSurfaceIdentity::componentOffset, &ThumbSurfaceIdentity::drawReturnRva, &ThumbSurfaceIdentity::orientation,
         &ThumbSurfaceIdentity::hotId, &ThumbSurfaceIdentity::pressedId, &ThumbSurfaceIdentity::cacheState,
         &ThumbSurfaceIdentity::enabled, &ThumbSurfaceIdentity::interactive, &ThumbSurfaceIdentity::reserved1,
         &ThumbSurfaceIdentity::reserved2}) {
        auto id = first; ++(id.*field); split(id);
    }
    for (auto field : {&ThumbSurfaceIdentity::surfaceType, &ThumbSurfaceIdentity::format, &ThumbSurfaceIdentity::width,
         &ThumbSurfaceIdentity::height, &ThumbSurfaceIdentity::stride, &ThumbSurfaceIdentity::status}) {
        auto id = first; ++(id.*field); split(id);
    }
    for (auto field : {&ThumbSurfaceIdentity::deviceScaleX, &ThumbSurfaceIdentity::deviceScaleY,
         &ThumbSurfaceIdentity::deviceOffsetX, &ThumbSurfaceIdentity::deviceOffsetY}) {
        auto id = first; id.*field += 0.5; split(id);
    }
    observations = {};
}

void testCapture(Fixture& f)
{
    f.route();
    images = f.routes;
    armedBinding = &f.binding;
    deadline.store(GetTickCount64() + thumbSurfaceCaptureMs);
    const auto collect = [&] { observe(ptr(f.stack), &f.surface, f.binding); };
    collect(); collect();
    auto foreign = f.binding;
    observe(ptr(f.stack), &f.surface, foreign);
    for (size_t i = 1; i <= thumbSurfaceCapacity; ++i) {
        f.surface.ox = static_cast<double>(i);
        collect();
    }
    // Full capacity still permits an existing key at a new absolute position.
    f.surface.ox = -2;
    put(f.scroller, 0xE8, RECT{100, 200, 114, 241});
    put(f.stack, paintFrame + 0xD8, RECT{101, 202, 113, 217});
    collect();
    AcquireSRWLockExclusive(&collectorLock);
    collect();
    ThumbSurfaceTrace closed{};
    check(copySamples(closed) == ERROR_TIMEOUT && !closed.sampleCount, "snapshot waits at most its budget and stays empty");
    ReleaseSRWLockExclusive(&collectorLock);

    // The absolute deadline closes even if the run thread has not cleared it.
    deadline.store(GetTickCount64());
    collect();
    check(copySamples(closed) == ERROR_SUCCESS && closed.sampleCount == thumbSurfaceCapacity &&
        closed.samples[0].calls == 3 && closed.samples[0].geometryMax[0] == 100 && closed.capacityDrops == 1 && closed.busyDrops == 1 &&
        hits(closed, Stage::Accepted) == thumbSurfaceCapacity + 3, "expiry and coherent bounded aggregation");
    deadline.store(GetTickCount64() + thumbSurfaceCaptureMs);
    f.binding.api.status = expireInGetter;
    collect();
    f.binding.api.status = f.api.status;
    check(copySamples(closed) == ERROR_SUCCESS && hits(closed, Stage::Accepted) == thumbSurfaceCapacity + 3,
        "deadline closure while reading prevents blind late recording");
}

void testRunBoundaries()
{
    ThumbSurfaceTrace trace{};
    trace.version = 4;
    check(run(&trace) == ERROR_INVALID_PARAMETER && !attempted && !deadline.load(), "legacy v4 ABI does not consume one-shot");
    trace.version = 3;
    check(run(&trace) == ERROR_INVALID_PARAMETER && !attempted && !deadline.load(), "legacy v3 ABI does not consume one-shot");
    trace.version = 2;
    trace.size = 18640;
    check(run(&trace) == ERROR_INVALID_PARAMETER && !attempted && !deadline.load(), "legacy v2 ABI does not consume one-shot");
    trace = {};
    trace.size = 18640;
    check(run(&trace) == ERROR_INVALID_PARAMETER && !attempted, "legacy v2 buffer size rejected");
    trace = {};
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(page != nullptr, "allocate undersized buffer");
    std::memcpy(page, &trace, sizeof(DWORD) * 2);
    check(run(page) == ERROR_INVALID_PARAMETER && !attempted, "short allocation rejected even with valid header");
    check(VirtualFree(page, 0, MEM_RELEASE), "free short allocation");
    if (!IsDebuggerPresent()) {
        check(run(&trace) == ERROR_NOT_READY && trace.startError == ERROR_NOT_READY && attempted && !deadline.load(),
            "no published binding; reports and remains closed");
        check(run(&trace) == ERROR_ALREADY_EXISTS && trace.startError == ERROR_ALREADY_EXISTS && !deadline.load(),
            "second call cannot rearm");
    }
    trace = {};
    trace.snapshotError = ERROR_TIMEOUT;
    check(resultOf(trace) == ERROR_TIMEOUT, "snapshot failure reported");
    trace.endError = ERROR_INVALID_DATA;
    check(resultOf(trace) == ERROR_INVALID_DATA, "lost target invariant takes precedence over incomplete snapshot");
}

void testBindingStatus()
{
    iatPage = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(iatPage != nullptr, "allocate synthetic IAT");
    auto* slot = static_cast<PVOID*>(iatPage);
    *slot = reinterpret_cast<PVOID>(&scroller_png_hook::createPattern);
    DWORD old{};
    check(VirtualProtect(iatPage, sizeof(*slot), PAGE_READONLY, &old), "fixture readonly IAT");
    ThemeProfile off{};
    off.zones = 0;
    ThemeProfile on{};
    on.zones = enableScrollerPng;
    std::atomic<const ThemeProfile*> profile{&on};
    scroller_png_hook::Binding binding{};
    binding.profile = &profile;
    binding.graphics = reinterpret_cast<ULONG_PTR>(iatPage) - scroller_png_hook::iatRva;
    check(bindingStatus(binding) == ERROR_INVALID_STATE, "target requires current published binding");
    scroller_png_hook::published.store(&binding);
    check(bindingStatus(binding) == ERROR_SUCCESS, "exact current IAT with zone enabled accepted");
    profile.store(&off);
    check(bindingStatus(binding) == ERROR_NOT_READY, "zone must remain enabled");
    profile.store(&on);
    check(VirtualProtect(iatPage, sizeof(*slot), PAGE_READWRITE, &old), "temporarily open fixture IAT");
    check(bindingStatus(binding) == ERROR_INVALID_DATA, "changed protection rejected");
    *slot = nullptr;
    check(VirtualProtect(iatPage, sizeof(*slot), PAGE_READONLY, &old), "restore fixture IAT protection");
    check(bindingStatus(binding) == ERROR_INVALID_DATA, "foreign slot rejected");
    scroller_png_hook::published.store(nullptr);
    check(VirtualFree(iatPage, 0, MEM_RELEASE), "free synthetic IAT");
    iatPage = nullptr;
}

void* forwarded{};
size_t forwardedCount{};
DWORD forwardedError{};
ULONG_PTR resultObject{};

void* original(void* surface)
{
    forwarded = surface;
    ++forwardedCount;
    forwardedError = GetLastError();
    SetLastError(0xB026);
    return &resultObject;
}

void testDiagnosticWrapper(Fixture& f)
{
    ThemeProfile off{};
    off.zones = 0;
    std::atomic<const ThemeProfile*> profile{&off};
    auto binding = f.binding;
    binding.original = original;
    binding.profile = &profile;
    scroller_png_hook::published.store(&binding);
    armedBinding = &binding;
    images = f.routes;
    size_t expectedCount{};
    const auto call = [&] {
        SetLastError(0xB025);
        const auto result = scroller_png_hook::createPattern(&f.surface);
        const auto error = GetLastError();
        ++expectedCount;
        check(result == &resultObject && forwarded == &f.surface && forwardedCount == expectedCount &&
            forwardedError == 0xB025 && error == 0xB026 && profile.load() == &off && !off.zones,
            "diagnostic wrapper forwards once with original argument, result, profile and LastError");
    };
    deadline.store(0);
    call();
    deadline.store(GetTickCount64());
    call();
    deadline.store(GetTickCount64() + thumbSurfaceCaptureMs);
    const auto before = hits(observations, Stage::OtherCall);
    poisonQuery = true;
    call();
    poisonQuery = false;
    deadline.store(0);
    check(hits(observations, Stage::OtherCall) == before + 1, "active observer sees unrelated original frame");
    scroller_png_hook::published.store(nullptr);
    armedBinding = nullptr;
}
}

int main()
{
    Fixture f;
    testRunBoundaries();
    testKnownThumbSurfaces();
    testRoutes(f);
    testMetadata(f);
    testCacheDiagnostics(f);
    testCompactGrouping(f);
    testCapture(f);
    testBindingStatus();
    testDiagnosticWrapper(f);
    std::puts("PASS: thumb route, owner, cache, state, metadata, and bounded time/capacity");
}
