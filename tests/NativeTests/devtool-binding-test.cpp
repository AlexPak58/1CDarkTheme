#include "list_identity.hpp"
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
const HMODULE frameHandle = reinterpret_cast<HMODULE>(ULONG_PTR{0x10000000});
HMODULE devtoolHandle{};
constexpr wchar_t expectedPath[] = L"X:\\platform\\devtool.dll";
constexpr DWORD pinFlags = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;

bool mapped{};
bool foreignBasename{};
bool wrongPath{};
bool wrongHash{};
bool wrongCode{};
bool wrongHeader{};
bool readFails{};
bool smallImage{};
bool invalidSlot{};
bool moduleRetained{};
bool pinFails{};
HANDLE pinFile{};
unsigned hashCalls{};
unsigned codeHashes{};
unsigned pinCalls{};

void check(bool ok, const char* reason)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

std::wstring fixturePath(HMODULE module)
{
    if (module == frameHandle) return L"X:\\platform\\frame.dll";
    check(module == devtoolHandle && moduleRetained, "mapped path read while retained");
    return wrongPath ? L"X:\\other\\devtool.dll" : expectedPath;
}

BOOL WINAPI fixtureModule(DWORD flags, LPCWSTR key, HMODULE* result)
{
    if (flags == pinFlags) {
        check(key == reinterpret_cast<LPCWSTR>(devtoolHandle) && moduleRetained,
              "pin by exact address while retained");
        check(pinFile && GetFileType(pinFile) == FILE_TYPE_DISK, "file reference remains held through pin");
        ++pinCalls;
        if (pinFails) return FALSE;
    } else {
        check(flags == 0, "temporary lookup retains module");
        const std::wstring_view name(key);
        check(name == expectedPath, "lookup only the exact platform path");
        if (!mapped || foreignBasename) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return FALSE;
        }
        check(!moduleRetained, "temporary module reference acquired exactly once per attempt");
        moduleRetained = true;
    }
    *result = devtoolHandle;
    return TRUE;
}

BOOL WINAPI fixtureFree(HMODULE module)
{
    check(module == devtoolHandle && moduleRetained, "release the temporary module reference");
    moduleRetained = false;
    return TRUE;
}

HANDLE WINAPI fixtureOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES,
                          DWORD disposition, DWORD flags, HANDLE)
{
    check(std::wstring_view(path) == expectedPath && moduleRetained, "open only the verified platform path");
    check(access == GENERIC_READ && share == FILE_SHARE_READ && disposition == OPEN_EXISTING,
          "deny shared writes and deletion during validation");
    if (readFails) return INVALID_HANDLE_VALUE;
    pinFile = CreateFileW(modulePath(nullptr).c_str(), access, share, nullptr, disposition, flags, nullptr);
    return pinFile;
}

BOOL WINAPI fixtureImage(HANDLE process, HMODULE module, LPMODULEINFO info, DWORD size)
{
    check(process == GetCurrentProcess() && module == devtoolHandle && size == sizeof(*info),
          "resolve candidate image size");
    info->lpBaseOfDll = devtoolHandle;
    info->SizeOfImage = smallImage ? 0x1000 : 0x261000;
    return TRUE;
}

SIZE_T WINAPI fixtureQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
NTSTATUS WINAPI fixtureHash(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG);
}

#define modulePath fixturePath
#define GetModuleHandleExW fixtureModule
#define FreeLibrary fixtureFree
#define CreateFileW fixtureOpen
#define K32GetModuleInformation fixtureImage
#define VirtualQuery fixtureQuery
#define BCryptHash fixtureHash
#include "devtool_binding.hpp"
#undef BCryptHash
#undef VirtualQuery
#undef K32GetModuleInformation
#undef CreateFileW
#undef FreeLibrary
#undef GetModuleHandleExW
#undef modulePath

namespace {
using namespace devtool_detail;

SIZE_T WINAPI fixtureQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size)
{
    const auto rva = reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(devtoolHandle);
    check(size == sizeof(*info), "query only fixture image ranges");
    info->AllocationBase = devtoolHandle;
    info->BaseAddress = const_cast<PVOID>(address);
    info->State = MEM_COMMIT;
    info->Type = MEM_IMAGE;
    info->Protect = PAGE_READONLY;
    switch (rva) {
    case 0: info->RegionSize = headerSize; break;
    case textRva: info->RegionSize = textSize; info->Protect = PAGE_EXECUTE_READ; break;
    case globalSearchRowsRva:
        info->RegionSize = sizeof(globalSearchMethods);
        if (invalidSlot) info->Protect = PAGE_READWRITE;
        break;
    default: check(false, "verify only the two authorized vtables, header and text");
    }
    return size;
}

NTSTATUS WINAPI fixtureHash(BCRYPT_ALG_HANDLE algorithm, PUCHAR, ULONG, PUCHAR bytes, ULONG length,
                            PUCHAR digest, ULONG size)
{
    check(algorithm == BCRYPT_SHA256_ALG_HANDLE && bytes && length && size == expectedDigest.size() &&
          moduleRetained && pinFile && GetFileType(pinFile) == FILE_TYPE_DISK, "hash the held file mapping");
    const auto base = reinterpret_cast<ULONG_PTR>(devtoolHandle);
    const auto address = reinterpret_cast<ULONG_PTR>(bytes);
    const auto* expected = &expectedDigest;
    bool mismatch = wrongHash;
    if (length == headerSize) {
        ULONG_PTR imageBase{};
        std::memcpy(&imageBase, bytes + imageBaseHeaderOffset, sizeof(imageBase));
        check(address != base && imageBase == preferredImageBase, "hash checked ASLR-normalized header copy");
        expected = &headerDigest;
        mismatch = wrongHeader;
    } else if (address == base + textRva) {
        check(length == textSize, "hash checked full mapped text");
        ++codeHashes;
        expected = &textDigest;
        mismatch = wrongCode;
    } else {
        ++hashCalls;
    }
    std::copy(expected->begin(), expected->end(), digest);
    if (mismatch) digest[0] ^= 1;
    return 0;
}

struct Fixture {
    SRWLOCK lock = SRWLOCK_INIT;
    Binding binding;
    ULONGLONG now{100};
    PTP_TIMER_CALLBACK callback{};
    void* context{};
    bool armed{};
    bool createFails{};
    unsigned creates{};
    unsigned schedules{};
    unsigned attempts{};
    unsigned redraws{};
    ULONGLONG delay{};
    DWORD coalesce{};
    void (*duringInspect)(){};

    explicit Fixture(const BindingOps& ops) : binding(lock, ops) {}
};

Fixture* current{};
const PTP_TIMER fakeTimer = reinterpret_cast<PTP_TIMER>(ULONG_PTR{0x9999});

PTP_TIMER WINAPI fixtureCreateTimer(PTP_TIMER_CALLBACK callback, void* context, PTP_CALLBACK_ENVIRON)
{
    ++current->creates;
    if (current->createFails) return nullptr;
    current->callback = callback;
    current->context = context;
    return fakeTimer;
}

VOID WINAPI fixtureSetTimer(PTP_TIMER timer, PFILETIME time, DWORD period, DWORD window)
{
    check(timer == fakeTimer && !period, "use one timer and one-shot scheduling");
    current->armed = time != nullptr;
    if (!time) return;
    ++current->schedules;
    ULARGE_INTEGER due{};
    due.LowPart = time->dwLowDateTime;
    due.HighPart = time->dwHighDateTime;
    const auto delayTicks = 0 - due.QuadPart;
    check(delayTicks && delayTicks <= 10000000, "schedule immediate first check or bounded retry");
    current->delay = delayTicks / 10000;
    current->coalesce = window;
}

ULONGLONG WINAPI fixtureTick() { return current->now; }

Attempt fixtureInspect(HMODULE frame) noexcept
{
    check(TryAcquireSRWLockExclusive(&current->lock) != FALSE, "module verification outside state lock");
    ReleaseSRWLockExclusive(&current->lock);
    ++current->attempts;
    if (const auto during = current->duringInspect) {
        current->duringInspect = nullptr;
        during();
    }
    return inspectLoadedModule(frame);
}

void fixtureRedraw() noexcept { ++current->redraws; }
constexpr BindingOps ops{fixtureCreateTimer, fixtureSetTimer, fixtureTick, fixtureInspect, fixtureRedraw};

void setEnabled(bool enabled)
{
    AcquireSRWLockExclusive(&current->lock);
    current->binding.setEnabledLocked(enabled, frameHandle);
    ReleaseSRWLockExclusive(&current->lock);
}

void fire()
{
    check(current->callback != nullptr, "timer callback registered");
    current->callback(nullptr, current->context, fakeTimer);
}

void reset()
{
    check(!moduleRetained, "every attempt released the temporary module");
    mapped = foreignBasename = wrongPath = wrongHash = wrongCode = wrongHeader = false;
    readFails = smallImage = invalidSlot = pinFails = false;
    pinFile = nullptr;
    hashCalls = codeHashes = pinCalls = 0;
}

void testLateBinding()
{
    Fixture fixture(ops);
    current = &fixture;
    setEnabled(true);
    check(fixture.armed && fixture.creates == 1 && fixture.delay == 0 && !fixture.attempts,
          "first enabled profile posts initial verification");
    fire();
    check(fixture.armed && fixture.delay == 1000 && fixture.coalesce == 250 && !fixture.binding.base() &&
          fixture.attempts == 1 && !hashCalls && !pinCalls, "missing module retries once per second");
    fire(); // A previously queued callback may arrive before the next due time.
    check(fixture.attempts == 1 && fixture.armed, "early queued callback cannot start another lookup");
    mapped = foreignBasename = true;
    fixture.now += 1000;
    fire();
    check(fixture.attempts == 2 && fixture.armed && !hashCalls && !pinCalls,
          "foreign basename does not stop waiting for the expected platform path");
    foreignBasename = false;
    readFails = true;
    fixture.now += 1000;
    fire();
    check(fixture.attempts == 3 && fixture.armed && !hashCalls && !pinCalls,
          "retryable read failure remains pending without binding");
    readFails = false;
    fixture.now += 1000;
    fire();
    check(fixture.binding.base() == reinterpret_cast<ULONG_PTR>(devtoolHandle) && !fixture.armed &&
          fixture.attempts == 4 && hashCalls == 1 && codeHashes == 1 && pinCalls == 1 && fixture.redraws == 1,
          "late exact image is hashed, pinned and published once");
    setEnabled(false);
    setEnabled(true);
    fire();
    check(fixture.creates == 1 && fixture.attempts == 4 && hashCalls == 1 && codeHashes == 1 && pinCalls == 1 &&
          fixture.redraws == 1 && !fixture.armed, "verified mapping is reused without lookup or redraw polling");
}

void testUnsupported(bool pathMismatch)
{
    reset();
    Fixture fixture(ops);
    current = &fixture;
    mapped = true;
    wrongPath = pathMismatch;
    wrongHash = !pathMismatch;
    setEnabled(true);
    fire();
    check(!fixture.armed && !fixture.binding.base() && fixture.attempts == 1 && !pinCalls && !fixture.redraws &&
          hashCalls == (pathMismatch ? 0u : 1u),
          "mismatched path or digest fails closed without pinning");
    wrongPath = wrongHash = false;
    setEnabled(false);
    setEnabled(true);
    fire();
    check(fixture.attempts == 1 && !fixture.armed, "unsupported image remains terminal for process lifetime");
}

void testDisableDuringLookup(bool present, bool resume)
{
    reset();
    Fixture fixture(ops);
    current = &fixture;
    mapped = present;
    setEnabled(true);
    fixture.duringInspect = resume ? +[] { setEnabled(false); setEnabled(true); fire(); } :
                                    +[] { setEnabled(false); fire(); };
    fire();
    check(fixture.attempts == 1 && fixture.creates == 1 && fixture.armed == (!present && resume) &&
          fixture.redraws == (present && resume ? 1u : 0u) &&
          (!!fixture.binding.base() == present),
          "disable and re-enable during lookup neither duplicate work nor rearm or redraw disabled state");
    if (!resume) {
        setEnabled(true);
        check(fixture.armed == !present, "re-enable reuses completed binding or schedules pending work");
    }
}

void testCancelledTimer()
{
    reset();
    Fixture fixture(ops);
    current = &fixture;
    fixture.createFails = true;
    setEnabled(true);
    check(!fixture.armed && fixture.creates == 1, "timer creation failure leaves optional binding idle");
    fixture.createFails = false;
    setEnabled(true);
    check(fixture.armed && fixture.creates == 2, "later successful apply may retry timer creation");
    setEnabled(false);
    fire();
    check(!fixture.armed && !fixture.attempts, "disable cancels pending work and ignores queued callback");
    setEnabled(true);
    check(fixture.armed && fixture.creates == 2, "re-enable reuses the one timer");
}

void testImageChecks()
{
    reset();
    mapped = true;
    smallImage = true;
    check(inspectLoadedModule(frameHandle).result == Result::Unsupported && !pinCalls,
          "vtable RVAs must fit mapped image");
    smallImage = false;
    invalidSlot = true;
    check(inspectLoadedModule(frameHandle).result == Result::Unsupported && !pinCalls,
          "both vtables must map immutable image data");
    invalidSlot = false;
    wrongHeader = true;
    check(inspectLoadedModule(frameHandle).result == Result::Unsupported && !pinCalls && !codeHashes,
          "approved disk digest cannot hide different mapped PE headers");
    wrongHeader = false;
    wrongCode = true;
    check(inspectLoadedModule(frameHandle).result == Result::Unsupported && !pinCalls && codeHashes == 1,
          "approved disk digest cannot hide different mapped executable code");
    wrongCode = false;
    const auto base = reinterpret_cast<ULONG_PTR>(devtoolHandle);
    auto* const rowTable = reinterpret_cast<ULONG_PTR*>(base + globalSearchRowsRva);
    rowTable[2] ^= 8;
    check(inspectLoadedModule(frameHandle).result == Result::Unsupported && !pinCalls && codeHashes == 2,
          "approved disk and code digests cannot hide different mapped vtable entries");
    rowTable[2] ^= 8;
    pinFails = true;
    check(inspectLoadedModule(frameHandle).result == Result::Retry && pinCalls == 1 && !moduleRetained,
          "failed pin conveys no base and releases temporary retention");
}

void allocateFixtureImage()
{
    devtoolHandle = static_cast<HMODULE>(
        VirtualAlloc(nullptr, expectedImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(devtoolHandle != nullptr, "allocate synthetic image");
    const auto base = reinterpret_cast<ULONG_PTR>(devtoolHandle);
    auto* const header = reinterpret_cast<BYTE*>(base);
    std::memcpy(header + imageBaseHeaderOffset, &base, sizeof(base));
    auto* table = reinterpret_cast<ULONG_PTR*>(base + globalSearchRowsRva);
    for (size_t i = 0; i < globalSearchMethods.size(); ++i)
        table[i] = base + globalSearchMethods[i];
}
}

int main()
{
    allocateFixtureImage();
    testLateBinding();
    testUnsupported(true);
    testUnsupported(false);
    testDisableDuringLookup(true, false);
    testDisableDuringLookup(true, true);
    testDisableDuringLookup(false, false);
    testDisableDuringLookup(false, true);
    testCancelledTimer();
    testImageChecks();
    reset();
    check(VirtualFree(devtoolHandle, 0, MEM_RELEASE) != FALSE, "release synthetic image");
    std::puts("PASS: optional devtool path, SHA, mapped RVAs, retention, one-shot cadence, disable races and reuse");
}
