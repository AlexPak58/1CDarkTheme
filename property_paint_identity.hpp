#pragma once

#include "property_description_identity.hpp"

namespace propertyPaint {
using namespace propertyDescription;

struct SiteObservation {
    DWORD siteTableRva{}, idTableRva{}, imageFlags{}, linked18{};
    bool operator==(const SiteObservation&) const = default;
};

template <typename Reader>
DWORD imageRva(const Reader& read, ULONG_PTR frame, ULONG_PTR table, DWORD bit, DWORD& flags)
{
    if (table < frame || table - frame > MAXDWORD ||
        !read.imageTable(frame, table, sizeof(ULONG_PTR))) return 0;
    flags |= bit;
    return static_cast<DWORD>(table - frame);
}

// At the upper paint call, draw+68 has not yet been assigned for this call.
// Match the current argument instead; an absent brush does not change identity.
template <typename Reader = GuardedReader>
Outcome inspectPaint(ULONG_PTR frame, ULONG_PTR window, ULONG_PTR currentBrushSource,
                     Identity& id, bool& brushPresent, const Reader& read = {}, Stage* stage = nullptr,
                     SiteObservation* rejectedSite = nullptr)
{
    setStage(stage, Stage::WindowTables);
    id = {};
    brushPresent = false;
    if (rejectedSite) *rejectedSite = {};
    ULONG_PTR windowDraw{};
    if (!frame || !addOffset(window, 0x160, windowDraw)) return Outcome::ReadFailure;
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
    ULONG_PTR owner{}, ownerWindow{}, ownerTable{}, brush{};
    if (!read.read(windowDraw, 0x260, owner) || !read.read(owner, 0x970, ownerWindow) ||
        !read.read(owner, 0x2E0, ownerTable) ||
        !read.read(owner, 0x5C8, id.rawColor) || !read.read(owner, 0x5E8, brush) ||
        !ownerWindow || !currentBrushSource) return Outcome::ReadFailure;
    brushPresent = brush != 0;
    if (ownerWindow != window || !matchesRva(frame, ownerTable, ownerTypeRva) ||
        !matchesOffset(owner, 0x5C8, currentBrushSource)) return Outcome::Mismatch;
    if (!read.imageTable(frame, ownerTable, sizeof(ULONG_PTR))) return Outcome::ReadFailure;

    setStage(stage, Stage::Site);
    ULONG_PTR site{}, control{}, siteTable{}, idTable{}, form{}, method{};
    if (!read.read(owner, 0x2A8, site) || !read.read(owner, 0x528, control) ||
        !read.read(site, 0, siteTable) || !read.read(control, 0, idTable)) return Outcome::ReadFailure;
    if (!matchesOffset(site, 0x18, control) || !matchesRva(frame, siteTable, siteTypeRva) ||
        !matchesRva(frame, idTable, idTypeRva)) {
        // Diagnostic-only provenance. Never turn an unknown table into acceptance.
        if (rejectedSite) {
            rejectedSite->linked18 = matchesOffset(site, 0x18, control) ? 1 : 0;
            rejectedSite->siteTableRva = imageRva(read, frame, siteTable, 1, rejectedSite->imageFlags);
            rejectedSite->idTableRva = imageRva(read, frame, idTable, 2, rejectedSite->imageFlags);
        }
        return Outcome::Mismatch;
    }
    setStage(stage, Stage::ControlId);
    if (!read.imageTable(frame, siteTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, idTable, 0x18 + sizeof(ULONG_PTR)) ||
        !read.read(idTable, 0x18, method) || !read.read(control, 0x114, id.controlId) ||
        !read.read(site, 8, form)) return Outcome::ReadFailure;
    if (!matchesRva(frame, method, 0x7C1670) || (id.controlId != 8 && id.controlId != 10))
        return Outcome::Mismatch;
    return inspectFamily(read, frame, form, id, stage);
}

} // namespace propertyPaint
