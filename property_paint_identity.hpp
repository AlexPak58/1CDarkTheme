#pragma once

#include "property_description_identity.hpp"

namespace propertyPaint {
using namespace propertyDescription;

struct SiteObservation {
    DWORD siteTableRva{}, idTableRva{}, imageFlags{}, linked18{};
    bool operator==(const SiteObservation&) const = default;
};

inline constexpr DWORD proxySiteTypeRva = 0xB20900;
inline constexpr DWORD proxyIdTypeRva = 0xB208A0;
inline constexpr DWORD proxyHostTypeRva = 0xB20DC0;
inline constexpr DWORD ownerNativeTypeRva = 0xB1FD88;

// Mirror the form's unsigned lower_bound without invoking platform code.
// Attached controls are not necessarily registered yet. Require this exact W.
template <typename Reader>
Outcome registeredControl(const Reader& read, ULONG_PTR form, ULONG_PTR site, DWORD id)
{
    if (site < 8 || (id != 8 && id != 10)) return Outcome::Mismatch;
    ULONG_PTR head{}, node{}, candidate{};
    BYTE sentinel{};
    if (!read.read(form, 8, head) || !head || !read.read(head, 0x19, sentinel) ||
        !read.read(head, 8, node)) return Outcome::ReadFailure;
    if (!sentinel) return Outcome::Mismatch;
    std::array<ULONG_PTR, 64> visited{};
    size_t count{};
    for (;;) {
        if (!node || !read.read(node, 0x19, sentinel)) return Outcome::ReadFailure;
        if (sentinel) {
            if (node != head) return Outcome::Mismatch;
            break;
        }
        if (count == visited.size()) return Outcome::Mismatch;
        for (size_t index = 0; index < count; ++index)
            if (visited[index] == node) return Outcome::Mismatch;
        visited[count++] = node;
        DWORD key{};
        if (!read.read(node, 0x20, key)) return Outcome::ReadFailure;
        const auto nextOffset = key < id ? 0x10 : 0;
        if (key >= id) candidate = node;
        if (!read.read(node, nextOffset, node)) return Outcome::ReadFailure;
    }
    if (!candidate) return Outcome::Mismatch;
    DWORD key{};
    ULONG_PTR wrapper{};
    if (!read.read(candidate, 0x20, key) || !read.read(candidate, 0x28, wrapper))
        return Outcome::ReadFailure;
    return key == id && wrapper == site - 8 ? Outcome::Accepted : Outcome::Mismatch;
}

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
    const auto originalSite = site, originalControl = control;
    const auto originalSiteTable = siteTable, originalIdTable = idTable;
    const auto rejectSite = [&] {
        // Keep the ABI2 diagnostic tied to the original owner interfaces.
        if (rejectedSite) {
            rejectedSite->linked18 = matchesOffset(originalSite, 0x18, originalControl) ? 1 : 0;
            rejectedSite->siteTableRva = imageRva(read, frame, originalSiteTable, 1, rejectedSite->imageFlags);
            rejectedSite->idTableRva = imageRva(read, frame, originalIdTable, 2, rejectedSite->imageFlags);
        }
        return Outcome::Mismatch;
    };
    if (matchesRva(frame, siteTable, proxySiteTypeRva) &&
        matchesRva(frame, idTable, proxyIdTypeRva)) {
        if (!matchesOffset(site, 8, control)) return rejectSite();
        if (!read.imageTable(frame, siteTable, 0x20) || !read.imageTable(frame, idTable, 0x20))
            return Outcome::ReadFailure;
        ULONG_PTR siteGetter{}, idGetter{}, hostTable{}, nativeTable{}, nativeOwner{}, propertyOwner{};
        ULONG_PTR delegateSite{}, delegateId{}, wrapperHost{};
        if (!read.read(siteTable, 0x18, siteGetter) || !read.read(idTable, 0x18, idGetter) ||
            !read.read(site, 0x40, hostTable) || !read.read(owner, 0x2A0, nativeTable) ||
            !read.read(site, 0x1C0, nativeOwner) || !read.read(site, 0x1D0, propertyOwner) ||
            !read.read(site, 0x228, delegateSite) || !read.read(site, 0x230, delegateId) ||
            !nativeOwner || !propertyOwner || !delegateSite || !delegateId)
            return Outcome::ReadFailure;
        if (!matchesRva(frame, siteGetter, 0x8A7A80) || !matchesRva(frame, idGetter, 0x8A7AA0) ||
            !matchesRva(frame, hostTable, proxyHostTypeRva) ||
            !matchesRva(frame, nativeTable, ownerNativeTypeRva) ||
            !matchesOffset(owner, 0x2A0, nativeOwner) || !matchesOffset(owner, 0x2E0, propertyOwner) ||
            !matchesOffset(delegateSite, 0x18, delegateId)) return rejectSite();
        if (!read.imageTable(frame, hostTable, sizeof(ULONG_PTR)) ||
            !read.imageTable(frame, nativeTable, sizeof(ULONG_PTR)) ||
            !read.read(delegateSite, 0x10, wrapperHost) ||
            !read.read(delegateSite, 0, siteTable) || !read.read(delegateId, 0, idTable))
            return Outcome::ReadFailure;
        if (!matchesOffset(site, 0x40, wrapperHost)) return rejectSite();
        // Exactly one known proxy. Nested/unknown delegates fail the canonical checks.
        site = delegateSite;
        control = delegateId;
    }
    if (!matchesOffset(site, 0x18, control) || !matchesRva(frame, siteTable, siteTypeRva) ||
        !matchesRva(frame, idTable, idTypeRva)) {
        return rejectSite();
    }
    setStage(stage, Stage::ControlId);
    if (!read.imageTable(frame, siteTable, sizeof(ULONG_PTR)) ||
        !read.imageTable(frame, idTable, 0x18 + sizeof(ULONG_PTR)) ||
        !read.read(idTable, 0x18, method) || !read.read(control, 0x114, id.controlId) ||
        !read.read(site, 8, form)) return Outcome::ReadFailure;
    if (!matchesRva(frame, method, 0x7C1670) || (id.controlId != 8 && id.controlId != 10))
        return Outcome::Mismatch;
    const auto family = inspectFamily(read, frame, form, id, stage);
    if (family != Outcome::Accepted || id.kind == Kind::ForeignId10) return family;
    setStage(stage, Stage::FamilyId);
    return registeredControl(read, form, site, id.controlId);
}

} // namespace propertyPaint
