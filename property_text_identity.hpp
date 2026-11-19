#pragma once
#include "property_paint_identity.hpp"

namespace propertyTextControl {
using namespace propertyDescription;
inline constexpr DWORD windowTypeRva = 0xF3C18, contextTypeRva = 0xF3B30;
// Numeric diagnostics never retain a heap pointer or an absolute image address.
enum class Module : DWORD { Other, Frame, Text, Wbase, Count };
struct ImageRange { ULONG_PTR base{}; DWORD size{}; };
using ImageRanges = std::array<ImageRange, 3>;
struct TableToken {
    Module module{};
    DWORD rva{};
    bool operator==(const TableToken&) const = default;
};
inline TableToken tableToken(ULONG_PTR pointer, const ImageRanges& images)
{
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (image.base && image.size && pointer >= image.base && pointer - image.base < image.size)
            return {static_cast<Module>(i + 1), static_cast<DWORD>(pointer - image.base)};
    }
    return {};
}
enum class Reason : DWORD {
    None, WindowTable, ContextTable, Anchor, Method, RootTable, RootView, RootDraw, Back,
    ReadInput, ReadWindow, ReadContext, ReadAnchor, ImageWindow, ImageContext,
    ReadMethod, ReadRoot, ReadRootView, ReadRootDraw, ReadBack,
    ImageRoot, ImageRootView, ImageRootDraw, Count
};
struct Diagnostic {
    Reason reason{};
    TableToken windowTable{};
    bool operator==(const Diagnostic&) const = default;
};
struct Identity {
    DWORD controlId{}, managerRva{}, managerViewRva{};
    bool operator==(const Identity&) const = default;
};
template <typename Reader = GuardedReader>
Outcome inspectText(ULONG_PTR frame, ULONG_PTR text, ULONG_PTR window, Identity& result,
                    const Reader& read = {}, Stage* stage = nullptr,
                    Diagnostic* diagnostic = nullptr, const ImageRanges& images = {})
{
    result = {};
    setStage(stage, Stage::WindowTables);
    if (diagnostic) *diagnostic = {};
    const auto reject = [&](Outcome outcome, Reason reason) {
        if (diagnostic) diagnostic->reason = reason;
        return outcome;
    };
    if (!frame || !text || !window) return reject(Outcome::ReadFailure, Reason::ReadInput);
    ULONG_PTR table{}, contextTable{}, anchor{}, root{}, rootTable{}, viewTable{}, drawTable{}, back{};
    if (!read.read(window, 0, table)) return reject(Outcome::ReadFailure, Reason::ReadWindow);
    if (diagnostic) diagnostic->windowTable = tableToken(table, images);
    if (!read.read(window, 0x190, contextTable)) return reject(Outcome::ReadFailure, Reason::ReadContext);
    if (!read.read(window, 8, anchor)) return reject(Outcome::ReadFailure, Reason::ReadAnchor);
    if (!matchesRva(text, table, windowTypeRva)) return reject(Outcome::Mismatch, Reason::WindowTable);
    if (!matchesRva(text, contextTable, contextTypeRva)) return reject(Outcome::Mismatch, Reason::ContextTable);
    if (anchor <= 0x100) return reject(Outcome::Mismatch, Reason::Anchor);
    if (!read.imageTable(text, table, 0x80)) return reject(Outcome::ReadFailure, Reason::ImageWindow);
    if (!read.imageTable(text, contextTable, 8)) return reject(Outcome::ReadFailure, Reason::ImageContext);
    root = anchor - 0x100;
    ULONG_PTR method{};
    if (!read.read(table, 0x78, method)) return reject(Outcome::ReadFailure, Reason::ReadMethod);
    if (!read.read(root, 0, rootTable)) return reject(Outcome::ReadFailure, Reason::ReadRoot);
    if (!read.read(root, 0x100, viewTable)) return reject(Outcome::ReadFailure, Reason::ReadRootView);
    if (!read.read(root, 0x600, drawTable)) return reject(Outcome::ReadFailure, Reason::ReadRootDraw);
    if (!read.read(root, 0x750, back)) return reject(Outcome::ReadFailure, Reason::ReadBack);
    if (!matchesRva(text, method, 0xDC0E5)) return reject(Outcome::Mismatch, Reason::Method);
    if (!matchesRva(frame, rootTable, 0xA65200)) return reject(Outcome::Mismatch, Reason::RootTable);
    if (!matchesRva(frame, viewTable, 0xA64B78)) return reject(Outcome::Mismatch, Reason::RootView);
    if (!matchesRva(frame, drawTable, 0xA64200)) return reject(Outcome::Mismatch, Reason::RootDraw);
    if (!matchesOffset(window, 0x190, back)) return reject(Outcome::Mismatch, Reason::Back);
    if (!read.imageTable(frame, rootTable, 8)) return reject(Outcome::ReadFailure, Reason::ImageRoot);
    if (!read.imageTable(frame, viewTable, 8)) return reject(Outcome::ReadFailure, Reason::ImageRootView);
    if (!read.imageTable(frame, drawTable, 8)) return reject(Outcome::ReadFailure, Reason::ImageRootDraw);
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
