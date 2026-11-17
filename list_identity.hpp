#pragma once

#include "platform.hpp"

// Verified on the hash-gated 8.3.27.2325 x64 frame/frntend pair.
inline constexpr auto listFrontendHash =
    L"1E309B705B04DC8A85DC99212368AEF848BCD70AB1BD9F1DB1F09E6A9452531E";
inline constexpr ULONG_PTR globalSearchProviderRva = 0x1A1308;
inline constexpr ULONG_PTR globalSearchRowsRva = 0x1A12E8;
inline constexpr ULONG_PTR globalSearchSiteRva = 0xB20900;

enum class ListKind { Unknown, MetadataTree, Dropdown, SearchResults };

namespace list_detail {
constexpr auto addressLimit = (std::numeric_limits<ULONG_PTR>::max)();

inline bool readable(DWORD protection)
{
    switch (protection) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool readAt(ULONG_PTR address, ULONG_PTR& value)
{
    __try {
        value = *reinterpret_cast<const ULONG_PTR*>(address);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

class PointerReader {
    ULONG_PTR start_{};
    SIZE_T size_{};

    bool contains(ULONG_PTR address) const
    {
        return address >= start_ && address - start_ < size_ &&
            sizeof(ULONG_PTR) <= size_ - (address - start_);
    }

public:
    bool read(ULONG_PTR address, ULONG_PTR& value)
    {
        if (!address || address % alignof(ULONG_PTR)) return false;
        if (!contains(address)) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) ||
                region.State != MEM_COMMIT || !readable(region.Protect)) return false;
            start_ = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
            size_ = region.RegionSize;
            if (!contains(address)) return false;
        }
        // The region is local to the current classification. Protection can
        // change after VirtualQuery, so the dereference still needs SEH.
        return readAt(address, value);
    }
};

inline ListKind classify(ULONG_PTR control, ULONG_PTR frame, ULONG_PTR frontend, ULONG_PTR devtool)
{
    if (!control || control > addressLimit - 0x7F0 ||
        !frame || frame > addressLimit - globalSearchSiteRva ||
        !frontend || frontend > addressLimit - 0xEEC598) return ListKind::Unknown;
    PointerReader reader;
    ULONG_PTR table{};
    if (!reader.read(control, table)) return ListKind::Unknown;
    const bool standalone = table == frame + 0xA8A278;
    const bool aggregated = table == frame + 0xA88AC0;
    if (!standalone && !aggregated) return ListKind::Unknown;

    // The grid retains these two interfaces. Inspect the current provider
    // instead of keeping the address of a destructible grid.
    ULONG_PTR provider{}, rows{};
    if (!reader.read(control + 0x7B0, provider) || !reader.read(control + 0x7F0, rows))
        return ListKind::Unknown;
    bool globalPair{};
    if (provider && provider <= addressLimit - 0x18 && rows == provider + 0x18) {
        ULONG_PTR providerTable{};
        if (!reader.read(provider, providerTable)) return ListKind::Unknown;
        const bool metadataProvider = providerTable == frontend + 0xEB8850;
        const bool globalProvider = standalone && devtool && devtool <= addressLimit - globalSearchProviderRva &&
            providerTable == devtool + globalSearchProviderRva;
        if (metadataProvider || globalProvider) {
            ULONG_PTR rowsTable{};
            if (!reader.read(rows, rowsTable)) return ListKind::Unknown;
            if (metadataProvider && rowsTable == frontend + 0xEB88F8) return ListKind::SearchResults;
            globalPair = globalProvider && rowsTable == devtool + globalSearchRowsRva;
        }
    }
    ULONG_PTR context{}, contextTable{};
    if (!reader.read(control + (aggregated ? 0x270 : 8), context) ||
        !reader.read(context, contextTable)) return ListKind::Unknown;
    if (aggregated && contextTable == frontend + 0xEEC598) return ListKind::MetadataTree;
    if (standalone && globalPair && contextTable == frame + globalSearchSiteRva) return ListKind::SearchResults;
    if (standalone && contextTable == frame + 0xA61258) return ListKind::Dropdown;
    return ListKind::Unknown;
}
} // namespace list_detail

inline ListKind classifyListControl(ULONG_PTR control, ULONG_PTR frame, ULONG_PTR frontend,
                                    ULONG_PTR devtool = 0)
{
    const auto lastError = GetLastError();
    const auto kind = list_detail::classify(control, frame, frontend, devtool);
    SetLastError(lastError);
    return kind;
}
