#pragma once

#include "platform.hpp"

// Verified on the hash-gated 8.3.27.2325 x64 frame/frntend pair.
inline constexpr auto listFrontendHash =
    L"1E309B705B04DC8A85DC99212368AEF848BCD70AB1BD9F1DB1F09E6A9452531E";

enum class ListKind { Unknown, MetadataTree, Dropdown, SearchResults };

inline ListKind classifyListControl(ULONG_PTR control, ULONG_PTR frame, ULONG_PTR frontend)
{
    if (!control || control % alignof(ULONG_PTR) || !frame || !frontend) return ListKind::Unknown;
    __try {
        const auto table = *reinterpret_cast<const ULONG_PTR*>(control);
        const bool standalone = table == frame + 0xA8A278;
        const bool aggregated = table == frame + 0xA88AC0;
        if (!standalone && !aggregated) return ListKind::Unknown;

        // The grid retains these two interfaces of the results data provider.
        // Checking the live provider avoids caching a destructible grid address.
        const auto provider = *reinterpret_cast<const ULONG_PTR*>(control + 0x7B0);
        const auto rows = *reinterpret_cast<const ULONG_PTR*>(control + 0x7F0);
        if (provider && rows == provider + 0x18 &&
            *reinterpret_cast<const ULONG_PTR*>(provider) == frontend + 0xEB8850 &&
            *reinterpret_cast<const ULONG_PTR*>(rows) == frontend + 0xEB88F8)
            return ListKind::SearchResults;

        if (aggregated) {
            const auto owner = *reinterpret_cast<const ULONG_PTR*>(control + 0x270);
            if (owner && *reinterpret_cast<const ULONG_PTR*>(owner) == frontend + 0xEEC598)
                return ListKind::MetadataTree;
        } else {
            const auto site = *reinterpret_cast<const ULONG_PTR*>(control + 8);
            if (site && *reinterpret_cast<const ULONG_PTR*>(site) == frame + 0xA61258)
                return ListKind::Dropdown;
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return ListKind::Unknown;
    }
    return ListKind::Unknown;
}
