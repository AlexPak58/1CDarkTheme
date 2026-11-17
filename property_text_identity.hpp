#pragma once
#include "property_paint_identity.hpp"

namespace propertyTextControl {
using namespace propertyDescription;
inline constexpr DWORD windowTypeRva = 0xF3C18, contextTypeRva = 0xF3B30;
struct Identity {
    DWORD controlId{}, managerRva{}, managerViewRva{};
    bool operator==(const Identity&) const = default;
};
template <typename Reader = GuardedReader>
Outcome inspectText(ULONG_PTR frame, ULONG_PTR text, ULONG_PTR window, Identity& result,
                    const Reader& read = {}, Stage* stage = nullptr)
{
    result = {};
    setStage(stage, Stage::WindowTables);
    if (!frame || !text || !window) return Outcome::ReadFailure;
    ULONG_PTR table{}, contextTable{}, anchor{}, root{}, rootTable{}, viewTable{}, drawTable{}, back{};
    if (!read.read(window, 0, table) || !read.read(window, 0x190, contextTable) ||
        !read.read(window, 8, anchor)) return Outcome::ReadFailure;
    if (!matchesRva(text, table, windowTypeRva) || !matchesRva(text, contextTable, contextTypeRva) || anchor <= 0x100)
        return Outcome::Mismatch;
    if (!read.imageTable(text, table, 0x80) || !read.imageTable(text, contextTable, 8)) return Outcome::ReadFailure;
    root = anchor - 0x100;
    ULONG_PTR method{};
    if (!read.read(table, 0x78, method) || !read.read(root, 0, rootTable) ||
        !read.read(root, 0x100, viewTable) || !read.read(root, 0x600, drawTable) ||
        !read.read(root, 0x750, back)) return Outcome::ReadFailure;
    if (!matchesRva(text, method, 0xDC0E5) || !matchesRva(frame, rootTable, 0xA65200) ||
        !matchesRva(frame, viewTable, 0xA64B78) || !matchesRva(frame, drawTable, 0xA64200) ||
        !matchesOffset(window, 0x190, back)) return Outcome::Mismatch;
    if (!read.imageTable(frame, rootTable, 8) || !read.imageTable(frame, viewTable, 8) ||
        !read.imageTable(frame, drawTable, 8)) return Outcome::ReadFailure;
    setStage(stage, Stage::Site);
    ULONG_PTR site{}, control{}, view{}, context{}, siteTable{}, idTable{}, getter{}, form{};
    if (!read.read(root, 8, site) || !read.read(root, 0x288, control) ||
        !read.read(root, 0x290, view) || !read.read(root, 0x298, context) ||
        !read.read(site, 0, siteTable) || !read.read(control, 0, idTable)) return Outcome::ReadFailure;
    if (!matchesOffset(site, 0x18, control) || !matchesOffset(site, 0x20, view) || !matchesOffset(site, 0xA0, context) ||
        !matchesRva(frame, siteTable, siteTypeRva) || !matchesRva(frame, idTable, idTypeRva)) return Outcome::Mismatch;
    setStage(stage, Stage::ControlId);
    Identity output{};
    if (!read.imageTable(frame, siteTable, 8) || !read.imageTable(frame, idTable, 0x20) ||
        !read.read(idTable, 0x18, getter) || !read.read(control, 0x114, output.controlId) ||
        !read.read(site, 8, form)) return Outcome::ReadFailure;
    if (!matchesRva(frame, getter, 0x7C1670) || output.controlId != 11) return Outcome::Mismatch;
    propertyDescription::Identity family{};
    family.controlId = output.controlId;
    const auto outcome = inspectFamily(read, frame, form, family, stage, true);
    if (outcome != Outcome::Accepted) return outcome;
    setStage(stage, Stage::FamilyId);
    const auto registered = propertyPaint::registeredControl(read, form, site, output.controlId, true);
    if (registered != Outcome::Accepted) return registered;
    output.managerRva = family.managerRva;
    output.managerViewRva = family.managerViewRva;
    result = output;
    return Outcome::Accepted;
}
} // namespace propertyTextControl
