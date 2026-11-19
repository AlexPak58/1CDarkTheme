#pragma once

#include "platform.hpp"

#include <cstring>

namespace propertyDescription {

inline constexpr DWORD windowTypeRva = 0xB21098;
inline constexpr DWORD windowDrawTypeRva = 0xB213A8;
inline constexpr DWORD ownerTypeRva = 0xB1EFB0;
inline constexpr DWORD siteTypeRva = 0xB0AC10;
inline constexpr DWORD idTypeRva = 0xB0AC30;
inline constexpr DWORD formTypeRva = 0xB0CBC8;
inline constexpr DWORD formViewTypeRva = 0xB0C2E8;
inline constexpr DWORD outerTypeRva = 0xB35CE8;
inline constexpr DWORD shellTypeRva = 0xB35D38;
inline constexpr std::array<std::array<DWORD, 2>, 2> managerTypes{{
    {0xAB8480, 0xAB8510}, {0xAB7688, 0xAB7710}
}};

inline bool containsBytes(const MEMORY_BASIC_INFORMATION& region, ULONG_PTR address, SIZE_T size)
{
    const auto base = reinterpret_cast<ULONG_PTR>(region.BaseAddress);
    return address >= base && address - base <= region.RegionSize &&
        size <= region.RegionSize - (address - base);
}

inline int readFaultFilter(DWORD error)
{
    return error == EXCEPTION_ACCESS_VIOLATION || error == EXCEPTION_IN_PAGE_ERROR ||
        error == EXCEPTION_GUARD_PAGE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

inline bool guardedCopy(void* destination, const void* source, SIZE_T size)
{
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (readFaultFilter(GetExceptionCode())) {
        return false;
    }
}

inline bool addOffset(ULONG_PTR base, ULONG_PTR offset, ULONG_PTR& address)
{
    if (!base || offset > (std::numeric_limits<ULONG_PTR>::max)() - base) return false;
    address = base + offset;
    return true;
}

inline bool readableMemory(DWORD protection)
{
    switch (protection) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

struct GuardedReader {
    template <typename T>
    bool read(ULONG_PTR base, ULONG_PTR offset, T& value) const
    {
        ULONG_PTR address{};
        MEMORY_BASIC_INFORMATION region{};
        return addOffset(base, offset, address) && address % alignof(T) == 0 &&
            VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) &&
            region.State == MEM_COMMIT && readableMemory(region.Protect) &&
            containsBytes(region, address, sizeof(value)) &&
            guardedCopy(&value, reinterpret_cast<LPCVOID>(address), sizeof(value));
    }

    bool imageTable(ULONG_PTR frame, ULONG_PTR table, SIZE_T length) const
    {
        MEMORY_BASIC_INFORMATION region{};
        return table >= frame &&
            VirtualQuery(reinterpret_cast<LPCVOID>(table), &region, sizeof(region)) &&
            region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.Protect == PAGE_READONLY &&
            reinterpret_cast<ULONG_PTR>(region.AllocationBase) == frame &&
            containsBytes(region, table, length);
    }
};

enum class Kind : DWORD { Description = 1, List, ForeignId10 };
enum class Outcome { Accepted, Mismatch, ReadFailure };
enum class Stage : DWORD {
    Input, Stack, Returns, RectAddress, RectValues, WindowTables, DrawMethod,
    Owner, Site, ControlId, FormTables, OuterTables, ShellLinks, ManagerTables,
    ManagerLinks, FamilyId, Count
};

inline void setStage(Stage* output, Stage stage) { if (output) *output = stage; }

struct Identity {
    Kind kind{};
    DWORD controlId{}, managerRva{}, managerViewRva{}, rawColor{};
    bool operator==(const Identity&) const = default;
};

inline bool matchesRva(ULONG_PTR frame, ULONG_PTR address, DWORD rva)
{
    return address >= frame && address - frame == rva;
}

inline bool matchesOffset(ULONG_PTR base, ULONG_PTR offset, ULONG_PTR value)
{
    ULONG_PTR expected{};
    return addOffset(base, offset, expected) && expected == value;
}

// A table from this exact image is evidence of an explicit mismatch.
// Missing/unreadable links and inconsistent backlinks are kept separate.
template <typename Reader>
Outcome inspectFamily(const Reader& read, ULONG_PTR frame, ULONG_PTR form, Identity& id, Stage* stage = nullptr,
                      bool allowText11 = false)
{
    setStage(stage, Stage::FormTables);
    if (!form) return Outcome::ReadFailure;
    ULONG_PTR formTable{}, formViewTable{};
    if (!read.read(form, 0, formTable) || !read.read(form, 0x28, formViewTable) ||
        !read.imageTable(frame, formTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, formViewTable, sizeof(ULONG_PTR))) return Outcome::ReadFailure;
    const auto foreign = [&] {
        if (id.controlId != 10) return Outcome::Mismatch;
        id.kind = Kind::ForeignId10;
        return Outcome::Accepted;
    };
    if (!matchesRva(frame, formTable, formTypeRva) || !matchesRva(frame, formViewTable, formViewTypeRva))
        return foreign();

    setStage(stage, Stage::OuterTables);
    ULONG_PTR outer{}, outerTable{}, shellTable{}, shellForm{}, shellFormView{};
    if (!read.read(form, 0x4A0, outer) || outer <= 0x10) return Outcome::ReadFailure;
    const auto shell = outer - 0x10;
    if (!read.read(outer, 0, outerTable) || !read.read(shell, 0, shellTable) ||
        !read.imageTable(frame, outerTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, shellTable, sizeof(ULONG_PTR))) return Outcome::ReadFailure;
    if (!matchesRva(frame, outerTable, outerTypeRva) || !matchesRva(frame, shellTable, shellTypeRva))
        return foreign();
    setStage(stage, Stage::ShellLinks);
    if (!read.read(shell, 0x28, shellForm) || !read.read(shell, 0x30, shellFormView) ||
        !shellForm || !shellFormView) return Outcome::ReadFailure;
    if (!matchesOffset(shellForm, 0x18, form) || !matchesOffset(form, 0x28, shellFormView))
        return Outcome::Mismatch;

    setStage(stage, Stage::ManagerTables);
    ULONG_PTR manager{}, shellManagerView{}, managerTable{}, managerViewTable{};
    if (!read.read(shell, 0x70, manager) || !read.read(shell, 0x18, shellManagerView) ||
        !shellManagerView || !read.read(manager, 0, managerTable) ||
        !read.read(manager, 8, managerViewTable) ||
        !read.imageTable(frame, managerTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, managerViewTable, sizeof(ULONG_PTR))) return Outcome::ReadFailure;
    if (!matchesOffset(manager, 8, shellManagerView)) return Outcome::Mismatch;
    const auto firstRoot = matchesRva(frame, managerTable, managerTypes[0][0]);
    const auto firstView = matchesRva(frame, managerViewTable, managerTypes[0][1]);
    const auto secondRoot = matchesRva(frame, managerTable, managerTypes[1][0]);
    const auto secondView = matchesRva(frame, managerViewTable, managerTypes[1][1]);
    const bool first = firstRoot && firstView;
    const bool second = secondRoot && secondView;
    if (!first && !second) {
        if (firstRoot || firstView || secondRoot || secondView) return Outcome::Mismatch;
        return foreign();
    }

    setStage(stage, Stage::ManagerLinks);
    ULONG_PTR managerHost{}, managerShell{}, managerFormView{};
    if (!read.read(manager, 0x28, managerHost) || !read.read(manager, 0x30, managerShell) ||
        !read.read(manager, 0x38, managerFormView) || !managerHost || !managerShell || !managerFormView)
        return Outcome::ReadFailure;
    if (!matchesOffset(managerHost, 0x18, shell) || managerShell != shell || managerFormView != shellFormView)
        return Outcome::Mismatch;
    setStage(stage, Stage::FamilyId);
    if (id.controlId != 8 && id.controlId != 10 && !(allowText11 && id.controlId == 11))
        return Outcome::Mismatch;
    id.kind = id.controlId == 10 ? Kind::Description : Kind::List;
    id.managerRva = static_cast<DWORD>(managerTable - frame);
    id.managerViewRva = static_cast<DWORD>(managerViewTable - frame);
    return Outcome::Accepted;
}

template <typename Reader = GuardedReader>
Outcome inspect(ULONG_PTR frame, ULONG_PTR windowDraw, Identity& id, const Reader& read = {}, Stage* stage = nullptr)
{
    setStage(stage, Stage::WindowTables);
    id = {};
    if (!frame || windowDraw <= 0x160) return Outcome::ReadFailure;
    const auto window = windowDraw - 0x160;
    ULONG_PTR windowTable{}, drawTable{};
    if (!read.read(window, 0, windowTable) || !read.read(windowDraw, 0, drawTable)) return Outcome::ReadFailure;
    if (!matchesRva(frame, windowTable, windowTypeRva) || !matchesRva(frame, drawTable, windowDrawTypeRva))
        return Outcome::Mismatch;
    if (!read.imageTable(frame, windowTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, drawTable, 0x120 + sizeof(ULONG_PTR))) return Outcome::ReadFailure;
    setStage(stage, Stage::DrawMethod);
    ULONG_PTR drawMethod{};
    if (!read.read(drawTable, 0x120, drawMethod)) return Outcome::ReadFailure;
    if (!matchesRva(frame, drawMethod, 0x5D3230)) return Outcome::Mismatch;

    setStage(stage, Stage::Owner);
    ULONG_PTR owner{}, ownerWindow{}, ownerTable{}, brushSource{}, brush{};
    if (!read.read(windowDraw, 0x260, owner) || !read.read(owner, 0x970, ownerWindow) ||
        !read.read(owner, 0x2E0, ownerTable) || !read.read(windowDraw, 0x68, brushSource) ||
        !read.read(owner, 0x5C8, id.rawColor) || !read.read(owner, 0x5E8, brush) ||
        !ownerWindow || !brushSource || !brush) return Outcome::ReadFailure;
    if (ownerWindow != window || !matchesRva(frame, ownerTable, ownerTypeRva) ||
        !matchesOffset(owner, 0x5C8, brushSource)) return Outcome::Mismatch;
    if (!read.imageTable(frame, ownerTable, sizeof(ULONG_PTR))) return Outcome::ReadFailure;

    setStage(stage, Stage::Site);
    ULONG_PTR site{}, control{}, siteTable{}, idTable{}, form{}, method{};
    if (!read.read(owner, 0x2A8, site) || !read.read(owner, 0x528, control) ||
        !read.read(site, 0, siteTable) || !read.read(control, 0, idTable)) return Outcome::ReadFailure;
    if (!matchesOffset(site, 0x18, control) || !matchesRva(frame, siteTable, siteTypeRva) ||
        !matchesRva(frame, idTable, idTypeRva)) return Outcome::Mismatch;
    setStage(stage, Stage::ControlId);
    if (!read.imageTable(frame, siteTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, idTable, 0x18 + sizeof(ULONG_PTR)) ||
        !read.read(idTable, 0x18, method) || !read.read(control, 0x114, id.controlId) ||
        !read.read(site, 8, form)) return Outcome::ReadFailure;
    if (!matchesRva(frame, method, 0x7C1670) || (id.controlId != 8 && id.controlId != 10))
        return Outcome::Mismatch;
    return inspectFamily(read, frame, form, id, stage);
}

} // namespace propertyDescription
