#pragma once

#include "list_identity.hpp"
#include <atomic>
#include <cstring>
#include <psapi.h>

namespace devtool_detail {

enum class Result { Retry, Unsupported, Bound };

struct Attempt {
    Result result{Result::Retry};
    ULONG_PTR base{};
};

inline constexpr wchar_t moduleName[] = L"devtool.dll";
inline constexpr std::array<UCHAR, 32> expectedDigest{
    0x0E, 0xB4, 0x0B, 0x39, 0xEB, 0x95, 0xA4, 0x49, 0xDA, 0x96, 0x4B, 0x53, 0x78, 0xB2, 0x0B, 0xFC,
    0x91, 0x6F, 0x77, 0x51, 0x0E, 0xE5, 0x33, 0xA4, 0xE0, 0xFA, 0x6E, 0xB6, 0xE5, 0xB1, 0x4D, 0x7B
};

// Fixed ranges from the exact hash-gated image. Its .text has no base relocations.
inline constexpr DWORD expectedImageSize = 0x261000;
inline constexpr ULONG_PTR textRva = 0x1000;
inline constexpr ULONG headerSize = 0x400;
inline constexpr ULONG textSize = 0x195A48;
inline constexpr ULONG_PTR preferredImageBase = 0x180000000;
inline constexpr size_t imageBaseHeaderOffset =
    0x128 + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase);
inline constexpr std::array<UCHAR, 32> headerDigest{
    0x2E, 0x1C, 0x90, 0xA9, 0xDA, 0x8E, 0x6C, 0x39, 0x57, 0xB4, 0xE5, 0xAE, 0x88, 0x99, 0x9A, 0xE3,
    0x00, 0x4F, 0xB7, 0xB3, 0xED, 0x15, 0x8A, 0x48, 0xDD, 0x7C, 0xAE, 0x6A, 0x31, 0x79, 0x02, 0xAA
};
inline constexpr std::array<UCHAR, 32> textDigest{
    0xDE, 0x1E, 0x5A, 0x2C, 0x4E, 0xEB, 0x6A, 0x6F, 0x5F, 0xEC, 0x5F, 0x6C, 0x78, 0x6A, 0x8E, 0x14,
    0xBC, 0x01, 0x7E, 0xA0, 0x57, 0xC7, 0x8F, 0xDB, 0x04, 0xA5, 0xF2, 0xB3, 0x20, 0x7D, 0xE4, 0xFB
};
// The rows table starts this contiguous run of 57 DIR64 code pointers.
// The provider table starts four entries into the run.
inline constexpr std::array<ULONG_PTR, 57> globalSearchMethods{
    0x8AE8C, 0x8AE38, 0x8AED4, 0x80960, 0x8AEF8, 0x8AEB0, 0x8AD00, 0x82E0,
    0x802C0, 0x80340, 0x80410, 0x8660, 0x8AD48, 0x8ACF4, 0x8ADA8, 0x806C0,
    0x806E0, 0x806F0, 0x8ADCC, 0x8AD84, 0x8AE2C, 0x812E0, 0x812F0, 0x81300,
    0x81420, 0x81430, 0x81600, 0x81D50, 0x82220, 0x82290, 0x8AE68, 0x8AE20,
    0x8AEBC, 0x80B00, 0x810B0, 0x81100, 0x812B0, 0x86940, 0x86990, 0x86960,
    0x82B60, 0x82E10, 0x82E40, 0x83290, 0x83340, 0x835B0, 0x86B30, 0x8AD0C,
    0x85F40, 0x85F10, 0x85E80, 0x8AD90, 0xBB70, 0x16882C, 0x16882C, 0x16882C,
    0x8AF50
};
static_assert(globalSearchRowsRva + 4 * sizeof(ULONG_PTR) == globalSearchProviderRva);

struct ModuleRef {
    HMODULE handle{};
    ~ModuleRef() { if (handle) FreeLibrary(handle); }
};

struct ReadFileRef {
    HANDLE handle{INVALID_HANDLE_VALUE};
    ~ReadFileRef() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

inline bool equalPath(const std::wstring& first, const std::wstring& second)
{
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline bool hashView(const void* bytes, ULONG size, std::array<UCHAR, 32>& digest)
{
    __try {
        return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                          static_cast<PUCHAR>(const_cast<void*>(bytes)), size,
                          digest.data(), static_cast<ULONG>(digest.size())) == 0;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

inline bool hashFile(HANDLE file, std::array<UCHAR, 32>& digest)
{
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
        length.QuadPart > (std::numeric_limits<ULONG>::max)())
        return false;
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) return false;
    const auto bytes = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    const bool hashed = bytes && hashView(bytes, static_cast<ULONG>(length.QuadPart), digest);
    if (bytes) UnmapViewOfFile(bytes);
    CloseHandle(mapping);
    return hashed;
}

inline bool hashMappedHeaders(HMODULE module, std::array<UCHAR, 32>& digest)
{
    std::array<BYTE, headerSize> header{};
    __try {
        std::memcpy(header.data(), module, header.size());
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    // Windows can write the ASLR base into the image header when making a
    // SEC_IMAGE section. Hash the original preferred value in this copy only.
    std::memcpy(header.data() + imageBaseHeaderOffset, &preferredImageBase, sizeof(preferredImageBase));
    return hashView(header.data(), headerSize, digest);
}

inline bool validImageRange(HMODULE module, ULONG_PTR rva, SIZE_T length, DWORD protection)
{
    const auto base = reinterpret_cast<ULONG_PTR>(module);
    if (rva >= expectedImageSize || length > expectedImageSize - rva ||
        rva > (std::numeric_limits<ULONG_PTR>::max)() - base) return false;
    const auto address = base + rva;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
        region.AllocationBase != module || region.Type != MEM_IMAGE ||
        region.State != MEM_COMMIT || region.Protect != protection) return false;
    const auto start = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return address >= start && address - start < region.RegionSize &&
        length <= region.RegionSize - (address - start);
}

inline bool matchesMethods(ULONG_PTR base)
{
    const auto* table = reinterpret_cast<const ULONG_PTR*>(base + globalSearchRowsRva);
    __try {
        for (size_t i = 0; i < globalSearchMethods.size(); ++i) {
            if (table[i] != base + globalSearchMethods[i]) return false;
        }
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

inline Result verifyMappedCode(HMODULE module)
{
    MODULEINFO info{};
    if (!K32GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return Result::Retry;
    const auto base = reinterpret_cast<ULONG_PTR>(module);
    if (info.lpBaseOfDll != module || info.SizeOfImage != expectedImageSize ||
        base > (std::numeric_limits<ULONG_PTR>::max)() - expectedImageSize ||
        !validImageRange(module, 0, headerSize, PAGE_READONLY) ||
        !validImageRange(module, textRva, textSize, PAGE_EXECUTE_READ) ||
        !validImageRange(module, globalSearchRowsRva, sizeof(globalSearchMethods), PAGE_READONLY))
        return Result::Unsupported;

    // Hashing the backing path does not identify an older mapping left behind
    // after a file replacement. Verify the live code and required tables too.
    std::array<UCHAR, 32> digest{};
    if (!hashMappedHeaders(module, digest)) return Result::Retry;
    if (digest != headerDigest) return Result::Unsupported;
    if (!hashView(reinterpret_cast<LPCVOID>(base + textRva), textSize, digest)) return Result::Retry;
    if (digest != textDigest || !matchesMethods(base)) return Result::Unsupported;
    return Result::Bound;
}

inline Attempt inspectLoadedModule(HMODULE frame) noexcept
{
    try {
        if (!frame) return {};
        auto expected = modulePath(frame);
        if (expected.empty()) return {};
        const auto separator = expected.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return {Result::Unsupported};
        expected.replace(separator + 1, std::wstring::npos, moduleName);

        ModuleRef candidate;
        if (!GetModuleHandleExW(0, expected.c_str(), &candidate.handle)) return {};
        const auto actual = modulePath(candidate.handle);
        if (actual.empty()) return {};
        if (!equalPath(actual, expected)) return {Result::Unsupported};

        // Retain the mapped module and deny writers/deletion of its backing path
        // from hash validation through the successful, exact-address pin.
        ReadFileRef file{CreateFileW(actual.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.handle == INVALID_HANDLE_VALUE) return {};
        std::array<UCHAR, 32> digest{};
        if (!hashFile(file.handle, digest)) return {};
        if (digest != expectedDigest) return {Result::Unsupported};

        const auto mappedCode = verifyMappedCode(candidate.handle);
        if (mappedCode != Result::Bound) return {mappedCode};

        HMODULE pinned{};
        constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
        if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(candidate.handle), &pinned)) return {};
        if (pinned != candidate.handle) return {Result::Unsupported};
        return {Result::Bound, reinterpret_cast<ULONG_PTR>(pinned)};
    } catch (...) {
        // The optional observer must not unwind through the Windows thread pool.
        return {};
    }
}

struct BindingOps {
    decltype(&CreateThreadpoolTimer) createTimer;
    decltype(&SetThreadpoolTimer) setTimer;
    decltype(&GetTickCount64) tick;
    Attempt (*inspect)(HMODULE) noexcept;
    void (*redraw)() noexcept;
};

// The supplied lock also protects publication of the active profile. The timer
// and callback context belong to the already-pinned theme DLL until process exit.
class Binding {
    SRWLOCK& lock_;
    const BindingOps& ops_;
    std::atomic<ULONG_PTR> base_{};
    HMODULE frame_{};
    PTP_TIMER timer_{};
    ULONGLONG due_{};
    bool enabled_{};
    bool pending_{};
    bool running_{};
    bool unsupported_{};

    static void CALLBACK timerCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept
    {
        static_cast<Binding*>(context)->checkLoadedModule();
    }

    void arm(ULONGLONG delay) noexcept
    {
        // Thread-pool relative due times are in 100 ns units. Use -1 for "now".
        ULARGE_INTEGER relative{};
        relative.QuadPart = 0 - (delay ? delay * 10000 : 1);
        FILETIME time{relative.LowPart, relative.HighPart};
        ops_.setTimer(timer_, &time, 0, delay ? 250 : 0);
    }

    void schedule(DWORD delay) noexcept
    {
        due_ = ops_.tick() + delay;
        pending_ = true;
        arm(delay);
    }

    HMODULE beginCheck() noexcept
    {
        AcquireSRWLockExclusive(&lock_);
        HMODULE frame{};
        if (pending_) {
            const auto now = ops_.tick();
            if (now < due_) {
                arm(due_ - now);
            } else {
                pending_ = false;
                running_ = true;
                frame = frame_;
                ops_.setTimer(timer_, nullptr, 0, 0);
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return frame;
    }

    void checkLoadedModule() noexcept
    {
        const auto frame = beginCheck();
        if (!frame) return;

        const auto attempt = ops_.inspect(frame);
        AcquireSRWLockExclusive(&lock_);
        running_ = false;
        switch (attempt.result) {
        case Result::Bound:
            base_.store(attempt.base, std::memory_order_release);
            if (enabled_) ops_.redraw();
            break;
        case Result::Unsupported:
            unsupported_ = true;
            break;
        case Result::Retry:
            if (enabled_) schedule(1000);
            break;
        }
        ReleaseSRWLockExclusive(&lock_);
    }

public:
    constexpr Binding(SRWLOCK& lock, const BindingOps& ops) noexcept : lock_(lock), ops_(ops) {}

    ULONG_PTR base() const noexcept
    {
        return base_.load(std::memory_order_acquire);
    }

    // Called with the profile lock after publishing a successfully applied profile.
    void setEnabledLocked(bool shouldObserve, HMODULE frame) noexcept
    {
        enabled_ = shouldObserve;
        if (!enabled_) {
            if (pending_) {
                pending_ = false;
                ops_.setTimer(timer_, nullptr, 0, 0);
            }
            return;
        }
        if (!frame || unsupported_ || base() || pending_ || running_) return;
        frame_ = frame;
        if (!timer_) timer_ = ops_.createTimer(timerCallback, this, nullptr);
        if (timer_) schedule(0);
    }
};

static_assert(std::atomic<ULONG_PTR>::is_always_lock_free);

} // namespace devtool_detail
