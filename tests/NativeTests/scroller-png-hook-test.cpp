#include "platform.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void* iatPage{};
DWORD failRestore{};
void* foreign{};
SIZE_T WINAPI fixtureQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION out, SIZE_T size)
{
    const auto result = VirtualQuery(address, out, size);
    if (result && address == iatPage) {
        out->Type = MEM_IMAGE;
        out->AllocationBase = reinterpret_cast<LPVOID>(reinterpret_cast<ULONG_PTR>(iatPage) - 0xC7BE0);
    }
    return result;
}
BOOL WINAPI fixtureProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD old)
{
    if (address == iatPage && protection == PAGE_READONLY && failRestore) {
        --failRestore; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    const auto result = VirtualProtect(address, size, protection, old);
    if (result && address == iatPage && protection == PAGE_READWRITE && foreign)
        *static_cast<PVOID*>(address) = foreign;
    return result;
}
}
#define VirtualQuery fixtureQuery
#define VirtualProtect fixtureProtect
#include "scroller_png_hook.hpp"
#undef VirtualProtect
#undef VirtualQuery

void check(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
namespace {
struct Surface {
    int status{}, type{}, format{1}, width{16}, height{21}, stride{64};
    unsigned references{1};
    std::array<DWORD, 1120> pixels{};
};
Surface source, copy;
bool denyCreate{};
unsigned originalCalls{}, clones{}, destroys{};
void* chosen{};
DWORD incomingOriginalError{};
DWORD patternToken{};
template<int Surface::*Field> int integer(const void* p) { return static_cast<const Surface*>(p)->*Field; }
void scale(const void*, double* x, double* y) { *x = *y = 1; }
void offset(const void*, double* x, double* y) { *x = *y = 0; }
void setPair(void*, double, double) { SetLastError(891); }
void flush(void*) { SetLastError(888); }
void dirty(void*) { SetLastError(889); }
void destroy(void* p) { ++destroys; --static_cast<Surface*>(p)->references; SetLastError(890); }
BYTE* data(void* p) { return reinterpret_cast<BYTE*>(static_cast<Surface*>(p)->pixels.data()); }
void* create(int format, int width, int height)
{
    ++clones;
    if (denyCreate) return nullptr;
    copy = {}; copy.format = format; copy.width = width; copy.height = height; copy.stride = width * 4;
    return &copy;
}
void* original(void* p)
{
    ++originalCalls; chosen = p; incomingOriginalError = GetLastError();
    ++static_cast<Surface*>(p)->references;
    SetLastError(0xBABE);
    return &patternToken;
}
scroller_png_hook::Binding fixtureBinding()
{
    scroller_png_hook::Binding state;
    state.original = original;
    state.api = {integer<&Surface::status>, integer<&Surface::type>, integer<&Surface::format>,
        integer<&Surface::width>, integer<&Surface::height>, integer<&Surface::stride>, scale, offset,
        flush, destroy, dirty, data, create};
    return state;
}
void testPattern()
{
    auto state = fixtureBinding();
    ThemeProfile profile;
    profile.zones = enableScrollerPng;
    const scroller_png::Identity arrow{0, 0, 0, 0};
    source.pixels.fill(0xFAF9E6);
    source.pixels[150] = 0x404040;
    const auto before = source.pixels;
    SetLastError(1234);
    check(scroller_png_hook::patternForSurface(&source, state, &profile, &arrow) == &patternToken,
          "original pattern result returned");
    check(originalCalls == 1 && clones == 1 && destroys == 1 && chosen == &copy &&
          incomingOriginalError == 1234 && GetLastError() == 0xBABE, "one create, clone release and LastError preserved");
    check(source.pixels == before && source.references == 1 && copy.references == 1,
          "source unchanged and pattern retains clone after local ref released");
    check(copy.pixels[150] == profile.colors[scrollerArrowGlyph], "themed pixels reach original Cairo call");
    destroy(&copy); check(!copy.references, "pattern final release owns remaining clone ref");
    profile.zones = 0;
    SetLastError(4567);
    scroller_png_hook::patternForSurface(&source, state, &profile, &arrow);
    check(originalCalls == 2 && clones == 1 && chosen == &source && incomingOriginalError == 4567 &&
          GetLastError() == 0xBABE, "disabled zone forwards original without cloning");
    destroy(&source);
    profile.zones = enableScrollerPng; denyCreate = true;
    SetLastError(321);
    scroller_png_hook::patternForSurface(&source, state, &profile, &arrow);
    check(originalCalls == 3 && clones == 2 && chosen == &source && incomingOriginalError == 321 &&
          GetLastError() == 0xBABE && source.pixels == before, "allocation failure falls back with one original call");
    destroy(&source); denyCreate = false;
    std::atomic<const ThemeProfile*> active{&profile}; state.profile = &active;
    scroller_png_hook::published.store(&state);
    SetLastError(555);
    scroller_png_hook::createPattern(&source); // The test caller is not the platform PNG route.
    check(originalCalls == 4 && clones == 2 && chosen == &source && incomingOriginalError == 555 &&
          GetLastError() == 0xBABE, "unmatched live wrapper is transparent");
    destroy(&source);
    profile.zones = 0;
    scroller_png_hook::createPattern(&source);
    check(originalCalls == 5 && clones == 2 && chosen == &source, "inactive wrapper remains safe for late callbacks");
    destroy(&source);
    scroller_png_hook::published.store(nullptr);
}
void testThumbPattern()
{
    source = {}; copy = {};
    source.format = 0; source.width = 20; source.height = 6; source.stride = 80;
    source.pixels.fill(0xFFFAFAFA);
    const auto before = source.pixels;
    auto state = fixtureBinding();
    scroller_png_hook::ThumbBinding thumb{{}, {state.api, setPair, setPair}};
    ThemeProfile profile;
    profile.zones = enableScrollerThumb;
    ThumbSurfaceIdentity id{};
    id.windowTableRva = 0xA885D0; id.scrollerTableRva = 0x3CD00;
    id.drawReturnRva = 0x19AF5; id.enabled = id.interactive = 1;
    originalCalls = clones = destroys = 0;
    for (DWORD cacheState : {0UL, 2UL, 3UL}) {
        id.cacheState = cacheState;
        id.hotId = cacheState == 2 ? 5 : 0;
        id.pressedId = cacheState == 3 ? 5 : 0;
        const auto callsBefore = originalCalls;
        SetLastError(1234);
        check(scroller_png_hook::patternForThumb(&source, state, thumb, &profile, id) == &patternToken,
              "thumb wrapper returns original result");
        const auto color = cacheState == 3 ? scrollerThumbPressed :
                           cacheState == 2 ? scrollerThumbHover : scrollerThumbBackground;
        check(originalCalls == callsBefore + 1 && chosen == &copy && copy.references == 1 && source.references == 1 &&
              incomingOriginalError == 1234 && GetLastError() == 0xBABE &&
              copy.pixels[0] == (0xFF000000 | profile.colors[color]) && source.pixels == before,
              "thumb states use separate colors, preserve source and LastError, retain clone in pattern");
        destroy(&copy);
    }
    const auto copies = clones;
    const auto fallback = [&] {
        const auto callsBefore = originalCalls;
        SetLastError(7654);
        scroller_png_hook::patternForThumb(&source, state, thumb, &profile, id);
        check(originalCalls == callsBefore + 1 && chosen == &source && incomingOriginalError == 7654 &&
              GetLastError() == 0xBABE && source.pixels == before, "thumb rejection passes original exactly once");
        destroy(&source);
    };
    profile.zones = enableScrollerPng; fallback();
    profile.zones = enableScrollerThumb; id.enabled = 0; fallback();
    id.enabled = 1; id.interactive = 0; fallback();
    id.interactive = 1; source.width = 21; fallback(); source.width = 20;
    id.windowTableRva = 0xABF108; id.scrollerTableRva = 0x3CA78; fallback();
    check(clones == copies, "disabled/uncovered thumb layouts do not allocate");
    id.windowTableRva = 0xA885D0; id.scrollerTableRva = 0x3CD00;
    denyCreate = true; fallback(); denyCreate = false;
    check(clones == copies + 1, "failed thumb allocation falls back safely");
    std::atomic<const ThemeProfile*> active{&profile}; state.profile = &active;
    scroller_png_hook::published.store(&state);
    scroller_png_hook::publishedThumb.store(&thumb);
    SetLastError(4422);
    scroller_png_hook::createPattern(&source);
    check(chosen == &source && clones == copies + 1 && incomingOriginalError == 4422 && GetLastError() == 0xBABE,
          "unrelated call remains unchanged with only thumb zone enabled");
    destroy(&source);
    scroller_png_hook::publishedThumb.store(nullptr);
    scroller_png_hook::published.store(nullptr);
}
void testIat()
{
    iatPage = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(iatPage != nullptr, "allocate isolated IAT fixture");
    auto state = fixtureBinding();
    state.graphics = reinterpret_cast<ULONG_PTR>(iatPage) - scroller_png_hook::iatRva;
    const auto initial = reinterpret_cast<PVOID>(state.original);
    const auto wrapper = reinterpret_cast<PVOID>(&scroller_png_hook::createPattern);
    auto reset = [&] {
        DWORD ignored{};
        check(VirtualProtect(iatPage, 8, PAGE_READWRITE, &ignored) != FALSE, "reset write access");
        *static_cast<PVOID*>(iatPage) = initial;
        check(VirtualProtect(iatPage, 8, PAGE_READONLY, &ignored) != FALSE, "reset read-only access");
        failRestore = 0; foreign = nullptr;
    };
    reset();
    check(!scroller_png_hook::installSlot(state) && scroller_png_hook::slotEquals(state, wrapper),
          "atomic installation restores read-only protection");
    check(scroller_png_hook::installSlot(state) == ERROR_INVALID_DATA, "double installation refused");
    reset(); failRestore = 1;
    check(scroller_png_hook::installSlot(state) == ERROR_ACCESS_DENIED &&
          scroller_png_hook::slotEquals(state, initial), "restore failure rolls back owned pointer and retries protection");
    reset(); failRestore = 2;
    check(scroller_png_hook::installSlot(state) == themeStateUncertain &&
          *static_cast<PVOID*>(iatPage) == initial, "persistent restoration failure returns uncertainty");
    reset(); foreign = reinterpret_cast<PVOID>(ULONG_PTR{0x12345678});
    check(scroller_png_hook::installSlot(state) == ERROR_INVALID_DATA &&
          scroller_png_hook::slotEquals(state, foreign), "CAS conflict leaves foreign pointer and protection intact");
    reset(); foreign = reinterpret_cast<PVOID>(ULONG_PTR{0x12345678}); failRestore = 1;
    check(scroller_png_hook::installSlot(state) == ERROR_INVALID_DATA &&
          scroller_png_hook::slotEquals(state, foreign), "restore retry does not overwrite competing pointer");
    foreign = nullptr;
    check(VirtualFree(iatPage, 0, MEM_RELEASE) != FALSE, "free isolated IAT fixture");
    iatPage = nullptr;
}
}
int main()
{
    testPattern(); testThumbPattern(); testIat();
    std::puts("PASS: scoped static/thumb PNG wrappers, Cairo reference lifetime, fallback, LastError and IAT CAS rollback");
}
