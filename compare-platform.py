"""Compare x64 1C rendering binaries offline. Matches are not verified hooks."""

import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import struct
import uuid


REFERENCE_HASHES = {
    "frame.dll": "d2f4367c1f4de24747440a4cf963961585bdf0d854a3c324741f912db123f2d7",
    "grphcs.dll": "125c00c01e7780704ad3e2159170dd7e1a05ef8cf9b2ed32b0b30cb535c4a563",
}
PROPERTY_PAINT_RETURN = 0x4F60ED
SEARCH_LABEL_RENDER_RETURN = 0x5BD070
SEARCH_LABEL_CLASS_ID = uuid.UUID("4f2defd2-5243-4166-b7f2-3e7ffc5f6394").bytes_le
SEARCH_GRID_CLASS_ID = uuid.UUID("2dd8e24f-ea53-4aab-a45e-a973eb59375f").bytes_le
CONTROL_PROPERTIES_ID = uuid.UUID("d3ac3bd1-b55a-11d4-a8bb-00c0df47968e").bytes_le
SEARCH_RESULTS_TOOLBAR_CLASS_ID = uuid.UUID("12735fb0-15fd-11d4-9403-008048da11f9").bytes_le
SEARCH_LABEL_LAYOUTS = {
    (0xC20, 0xC38): (0x218, 0x110),
    (0xC68, 0xC80): (0x238, 0x120),
}
SEARCH_LABEL_RENDER_85 = bytes.fromhex("FF 90 38 02 00 00 41 80 BF 0A 0C 00 00 00")
DROPDOWN_DESCRIPTOR = 0xA8A7A0
DROPDOWN_DISPATCH_PREFIX = bytes.fromhex(
    "48 83 EC 48 48 8B 01 C7 44 24 30 64 00 00 00 "
    "C7 44 24 28 02 00 00 00 C7 44 24 20 01 00 00 00 FF 90"
)
SITES = {
    "frame.dll": {
        "rowFill": 0x303454,
        "workspaceFill": 0x9518EC,
        "treeTabFill": 0x5D4B1E,
        "treeTabText": 0x5D4104,
    },
    "grphcs.dll": {
        "emptyFill": 0xAEE4E,
        "labelColor": 0x22D7,
        "toolbarEdgeStroke": 0xAEDA3,
    },
}
CAIRO_COLORS = {"cairo_set_source_rgb", "cairo_set_source_rgba"}
# cmp [rdi+60h],4; mov edx,eax; not edx; mov rcx,rdi; cmovne edx,eax; call rel32
FILL_PREFIX = bytes.fromhex("83 7F 60 04 8B D0 F7 D2 48 8B CF 0F 45 D0 E8")
# Glyph run: load the packed color address, then call the converter before setting the font.
# The converter extracts blue from bits 0–7 in 8.3 and 16–23 in 8.5.
TEXT_COLOR_LAYOUTS = (
    ("AARRGGBB", bytes.fromhex("49 8D 51 4C 48 8B CD E8"), 0x9C, 0x60,
     bytes.fromhex("C1 E8 18 66 0F 6E E0 41 0F B6 C0 F3 0F E6 E4 66 0F 6E D8")),
    ("AABBGGRR", bytes.fromhex("4A 8D 14 08 48 8B CE E8"), 0x72, 0x61,
     bytes.fromhex("C1 E8 18 66 0F 6E E0 41 8B C0 C1 E8 10 0F B6 C0 F3 0F E6 E4 66 0F 6E D8")),
)
FRAME_COLOR_PATTERNS = {
    "k83MenuTextCOLORREF": "48 8B 05 ?? ?? ?? ?? BB FF FF FF 00 8B 30 74 ??",
    "k83MenuSelectBackColor": "48 8B 15 ?? ?? ?? ?? 48 8B 00 FF 90 ?? 02 00 00 8B D8",
    "k83FocusWindowColor": "48 8B 15 ?? ?? ?? ?? 48 8B CB FF 90 40 02 00 00 0F B6 D0",
    "k83TabsBorderCOLORREF": "48 8B 05 ?? ?? ?? ?? 84 D2 75 ?? 8B 08 EB ??",
}


class Image:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        pe = self.read("I", 0x3C)
        if self.data[:2] != b"MZ" or self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: expected a PE image")
        machine, section_count = self.read("HH", pe + 4)
        optional_size = self.read("H", pe + 20)
        optional = pe + 24
        if machine != 0x8664 or self.read("H", optional) != 0x20B:
            raise ValueError(f"{path}: expected an x64 PE32+ image")
        if optional_size < 128 or self.read("I", optional + 108) < 2:
            raise ValueError(f"{path}: missing PE import directory entry")
        self.image_base = self.read("Q", optional + 24)
        self.import_rva, self.import_size = self.read("II", optional + 120)
        self.sections = {}
        for index in range(section_count):
            start = optional + optional_size + 40 * index
            name = self.data[start:start + 8].rstrip(b"\0")
            rva, size, offset = self.read("III", start + 12)
            if size and offset + size > len(self.data):
                raise ValueError(f"{path}: truncated PE section {name!r}")
            self.sections[name] = (rva, size, offset)

    def read(self, fmt, offset):
        values = struct.unpack_from("<" + fmt, self.data, offset)
        return values[0] if len(values) == 1 else values

    def offset(self, rva):
        for start, size, offset in self.sections.values():
            if start <= rva < start + size:
                return offset + rva - start
        raise ValueError(f"{self.path}: RVA 0x{rva:X} is outside file-backed sections")

    def text(self):
        if b".text" not in self.sections:
            raise ValueError(f"{self.path}: no .text section")
        rva, size, offset = self.sections[b".text"]
        return rva, self.data[offset:offset + size]

    def function_ranges(self):
        _, size, offset = self.sections.get(b".pdata", (0, 0, 0))
        for begin, end, _ in struct.iter_unpack("<III", self.data[offset:offset + size - size % 12]):
            if begin < end:
                yield begin, end

    def sha256(self):
        return hashlib.sha256(self.data).hexdigest()

    def name_at(self, rva):
        offset = self.offset(rva)
        end = self.data.index(b"\0", offset)
        return self.data[offset:end].decode("ascii")

    def imports(self):
        for relative in range(0, self.import_size - 19, 20):
            descriptor = self.offset(self.import_rva + relative)
            names, _, _, dll_rva, iat = self.read("IIIII", descriptor)
            if not dll_rva:
                return
            dll = self.name_at(dll_rva).lower()
            index = 0
            while (entry := self.read("Q", self.offset((names or iat) + 8 * index))) != 0:
                if entry < (1 << 63):
                    yield dll, self.name_at(entry + 2), iat + 8 * index
                index += 1


def reference_windows(reference_dir):
    signatures = {}
    for module, addresses in SITES.items():
        image = Image(reference_dir / module)
        if image.sha256() != REFERENCE_HASHES[module]:
            raise ValueError(f"{image.path}: expected the 8.3.27.2325 reference SHA-256")
        rva, code = image.text()
        signatures[module] = {name: code[site - rva - 8:site - rva + 16]
                              for name, site in addresses.items()}
        if module == "frame.dll":
            offset = PROPERTY_PAINT_RETURN - rva
            signatures[module]["property_paint"] = code[offset:offset + 24]
            offset = SEARCH_LABEL_RENDER_RETURN - rva - 12
            signatures[module]["search_label_render"] = code[offset:offset + 24]
            signatures[module]["dropdown_descriptors"] = dropdown_descriptors(image, DROPDOWN_DESCRIPTOR)
            if signatures[module]["dropdown_descriptors"] is None:
                raise ValueError(f"{image.path}: missing reference dropdown descriptors")
    return signatures


def fill_candidates(rva, code, rgba_calls):
    for match in re.finditer(re.escape(FILL_PREFIX) + b".{4}", code, re.S):
        callee = rva + match.end() + struct.unpack_from("<i", code, match.end() - 4)[0]
        # This layout has an indirect call to the RGBA import 0x53 bytes into the callee.
        rgba_call = callee + 0x53
        if rgba_call in rgba_calls:
            yield {"site": f"0x{rva + match.start() + 11:X}",
                   "callee": f"0x{callee:X}", "rgba_call": f"0x{rgba_call:X}"}


def spreadsheet_rectangle_cairo_candidates(rva, code, cairo_imports):
    # The rectangle dispatch forwards x/y/width and loads height from its caller's stack.
    dispatch = re.compile(rb"\x89\x44\x24\x20\x44\x8b\x8c\x24[\x80\x90]\x00\x00\x00"
                          rb"\x44\x8b\xc6\x8b\xd5\xe8(.{4})", re.S)
    for match in dispatch.finditer(code):
        helper = match.end() + struct.unpack("<i", match[1])[0]
        rectangle = helper + 0xDD
        fill_call = rectangle + 0x1A
        if helper < 0 or fill_call + 5 > len(code) or code[rectangle:rectangle + 2] != b"\xff\x15":
            continue
        slot = rva + rectangle + 6 + struct.unpack_from("<i", code, rectangle + 2)[0]
        if cairo_imports.get(slot) != "cairo_rectangle" or code[fill_call] != 0xE8:
            continue
        fill = fill_call + 5 + struct.unpack_from("<i", code, fill_call + 1)[0]
        color_call = fill + 0x71
        if (fill < 0 or color_call + 5 > len(code) or
                code[color_call - 6:color_call + 1] != b"\x48\x8b\xcf\x0f\x45\xd0\xe8"):
            continue
        converter = color_call + 5 + struct.unpack_from("<i", code, color_call + 1)[0]
        rgb_jump = converter + 0xC0
        mask_call = converter + 0x53
        if (converter < 0 or rgb_jump + 7 > len(code) or
                code[rgb_jump:rgb_jump + 3] != b"\x48\xff\x25" or
                code[mask_call:mask_call + 2] != b"\xff\x15"):
            continue
        rgb_slot = rva + rgb_jump + 7 + struct.unpack_from("<i", code, rgb_jump + 3)[0]
        rgba_slot = rva + mask_call + 6 + struct.unpack_from("<i", code, mask_call + 2)[0]
        if (cairo_imports.get(rgb_slot) != "cairo_set_source_rgb" or
                cairo_imports.get(rgba_slot) != "cairo_set_source_rgba"):
            continue
        yield {
            "rectangle_iat": f"0x{slot:X}",
            "rectangle_return": f"0x{rva + rectangle + 6:X}",
            "dispatch_return": f"0x{rva + match.end():X}",
            "fill_return": f"0x{rva + fill_call + 5:X}",
            "solid_color_return": f"0x{rva + color_call + 5:X}",
            "solid_mask_return": f"0x{rva + mask_call + 6:X}",
        }


def text_color_candidates(image, rva, code, rgba_calls, font_calls, fill_calls):
    ranges = list(image.function_ranges())
    # The two callers step over original glyph runs and temporary fragments, respectively.
    run_calls = []
    for match in re.finditer(rb"(?=(\xe8(.{4})\x48\x83(?:\xc3\x60|\xc7\x68)))", code, re.S):
        caller = rva + match.start()
        target = caller + 5 + struct.unpack("<i", match[2])[0]
        run_calls.append((caller, target, match[1][-1]))
    for packing, prefix, call_offset, rgba_offset, channel_bytes in TEXT_COLOR_LAYOUTS:
        for match in re.finditer(re.escape(prefix) + b"(.{4})", code, re.S):
            caller = rva + match.end() - 5
            converter = caller + 5 + struct.unpack("<i", match[1])[0]
            rgba_call = converter + rgba_offset
            renderer = caller - call_offset
            if rgba_call not in rgba_calls or caller + 0x15 not in font_calls:
                continue
            if not code.startswith(channel_bytes, converter + 0x12 - rva):
                continue
            if not any(begin == renderer and caller + 0x15 < end for begin, end in ranges):
                continue
            enclosing = {}
            for site, target, stride in run_calls:
                if target != renderer:
                    continue
                for bounds in ranges:
                    if bounds[0] <= site < bounds[1]:
                        enclosing.setdefault(bounds, {}).setdefault(stride, []).append(site)
            for (begin, end), sites in enclosing.items():
                if set(sites) != {0x60, 0x68}:
                    continue
                backgrounds = sorted(site for site in rgba_calls
                                     if begin <= site < end and site + 9 in fill_calls)
                yield {
                    "packing_candidate": packing,
                    "glyph_rgba_call": f"0x{rgba_call:X}",
                    "glyph_color_return": f"0x{caller + 5:X}",
                    "range_renderer": f"0x{begin:X}",
                    "original_run_calls": [f"0x{site:X}" for site in sites[0x60]],
                    "fragment_run_calls": [f"0x{site:X}" for site in sites[0x68]],
                    "range_background_rgba_calls": [f"0x{site:X}" for site in backgrounds],
                }


def frame_color_candidates(image, rva, code):
    slots = {}
    for dll, symbol, slot in image.imports():
        if dll in {"core83.dll", "core85.dll"} and symbol.startswith("?"):
            slots[symbol[1:].split("@", 1)[0]] = slot
    result = {}
    for name, pattern in FRAME_COLOR_PATTERNS.items():
        slot = slots.get(name)
        candidates = []
        if slot is not None:
            window = b"".join(b"." if byte == "??" else re.escape(bytes.fromhex(byte))
                              for byte in pattern.split())
            for match in re.finditer(b"(?=" + window + b")", code, re.S):
                offset = match.start()
                displacement = struct.unpack_from("<i", code, offset + 3)[0]
                if rva + offset + 7 + displacement == slot:
                    candidates.append(f"0x{rva + offset:X}")
        result[name] = {"iat": None if slot is None else f"0x{slot:X}",
                        "load_candidates": candidates}
    return result


def property_palette_candidates(image, rva, code, anchor):
    imports = [(dll, slot) for dll, name, slot in image.imports()
               if dll in {"wbase83.dll", "wbase85.dll"} and name.startswith("?fillSolidRect@wbase@@")]
    if len(imports) != 1:
        return
    dll, slot = imports[0]
    if dll == "wbase83.dll":
        background_field, selection_field, text_slot = 0x37C, 0x390, 0x210
        filter_suffix = bytes.fromhex("4C 8B 75 E0 8B 75 84 3B B7 40 03 00 00")
    else:
        background_field, selection_field, text_slot = 0x3AC, 0x3C0, 0x230
        filter_suffix = bytes.fromhex("4C 8B 75 F0 8B 45 84 3B 87 70 03 00 00")

    text_call = b"\x49\x8b\xd4\xff\x90" + struct.pack("<I", text_slot)
    filter_call = b"\x41\xff\xd6" + filter_suffix
    fields = (background_field, selection_field, background_field)
    ranges = dict(image.function_ranges())
    for paint in re.finditer(re.escape(anchor), code):
        offset = paint.start()
        if offset < 5 or code[offset - 5] != 0xE8:
            continue
        paint_return = rva + offset
        renderer = paint_return + struct.unpack_from("<i", code, offset - 4)[0]
        end = ranges.get(renderer, 0)
        if not rva <= renderer < end <= rva + len(code):
            continue
        body = code[renderer - rva:end - rva]
        fills = [match.end() for match in re.finditer(rb"\xff\x15(.{4})", body, re.S)
                 if renderer + match.end() + struct.unpack("<i", match[1])[0] == slot]
        if len(fills) != 4 or fills[1] < 23:
            continue
        if any(not body.startswith(b"\x44\x8b\x87" + struct.pack("<I", field), call_return - 23)
               for call_return, field in zip(fills[1:], fields)):
            continue

        labels = [match.end() for match in re.finditer(re.escape(text_call), body)]
        filtered = [match.start() + 3 for match in re.finditer(re.escape(filter_call), body)]
        if len(labels) != 2 or len(filtered) != 1:
            continue
        if not fills[2] < labels[0] < filtered[0] < fills[3] < labels[1]:
            continue
        yield {
            "paint_return": f"0x{paint_return:X}",
            "renderer": f"0x{renderer:X}",
            "fill_returns": {name: f"0x{renderer + site:X}" for name, site in
                             zip(("row", "selection", "group"), fills[1:])},
            "label_returns": {
                "row": f"0x{renderer + labels[0]:X}",
                "group": f"0x{renderer + labels[1]:X}",
                "filtered": f"0x{renderer + filtered[0]:X}",
            },
            "background_field": f"0x{background_field:X}",
            "selection_field": f"0x{selection_field:X}",
            "label_slot": f"0x{text_slot:X}",
        }


def dropdown_descriptors(image, rva):
    start, size, offset = image.sections.get(b".rdata", (0, 0, 0))
    if not start <= rva <= start + size - 7 * 24:
        return None
    entries = []
    for index in range(7):
        pointer, field, flag = image.read("QQQ", offset + rva - start + index * 24)
        identifier = pointer - image.image_base
        if not start <= identifier <= start + size - 16:
            return None
        begin = offset + identifier - start
        entries.append((image.data[begin:begin + 16], field, flag))
    return entries


def dropdown_candidates(image, rva, code, descriptors):
    start, size, offset = image.sections.get(b".rdata", (0, 0, 0))
    rdata = image.data[offset:offset + size]
    ranges = dict(image.function_ranges())
    # A constructor writes the primary table to [rbx]; a reset writes it to [rcx].
    constructors = {}
    for match in re.finditer(rb"\x48\x8d\x05(.{4})\x48\x89\x03", code, re.S):
        table = rva + match.start() + 7 + struct.unpack("<i", match[1])[0]
        if start <= table < start + size:
            constructors.setdefault(table, []).append(rva + match.start())

    for match in re.finditer(rb"\x4d\x8b\xc8\x4c\x8b\xc2\x48\x8d\x15(.{4})\xe9(.{4})", code, re.S):
        method = rva + match.start()
        destination = match.end() + struct.unpack("<i", match[2])[0]
        if not 0 <= destination < len(code):
            continue
        descriptor = method + 13 + struct.unpack("<i", match[1])[0]
        if dropdown_descriptors(image, descriptor) != descriptors:
            continue
        pointer = struct.pack("<Q", image.image_base + method)
        for reference in re.finditer(re.escape(pointer), rdata):
            table = start + reference.start()
            if table % 8 or table not in constructors:
                continue
            for slot, allocation in ((0x258, 0x88), (0x270, 0xC8)):
                if reference.start() + slot + 8 > len(rdata):
                    continue
                renderer = image.read("Q", offset + reference.start() + slot) - image.image_base
                prologue = bytes.fromhex("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC")
                prologue += struct.pack("<I", allocation)
                if renderer < rva or renderer not in ranges or not code.startswith(prologue, renderer - rva):
                    continue
                dispatch = DROPDOWN_DISPATCH_PREFIX + struct.pack("<I", slot)
                returns = [f"0x{rva + call.end():X}" for call in re.finditer(re.escape(dispatch), code)
                           if ranges.get(rva + call.start(), 0) > rva + call.end()]
                if returns:
                    yield {
                        "descriptor_method": f"0x{method:X}",
                        "descriptor_array": f"0x{descriptor:X}",
                        "vtable": f"0x{table:X}",
                        "constructor_references": [f"0x{site:X}" for site in constructors[table]],
                        "paint_slot": f"0x{slot:X}",
                        "paint_method": f"0x{renderer:X}",
                        "stack_allocation": f"0x{allocation:X}",
                        "dispatch_returns": returns,
                    }


def registered_factories(image, rva, code, class_id):
    data_rva, data_size, data_offset = image.sections.get(b".data", (0, 0, 0))
    ro_rva, ro_size, ro_offset = image.sections.get(b".rdata", (0, 0, 0))
    data = image.data[data_offset:data_offset + data_size]
    ro_data = image.data[ro_offset:ro_offset + ro_size]
    factory_prefix = b"\x48\x85\xc9\x0f\x84"
    for identifier in re.finditer(re.escape(class_id), ro_data):
        address = image.image_base + ro_rva + identifier.start()
        for entry in re.finditer(re.escape(struct.pack("<Q", address)), data):
            record = data_rva + entry.start()
            if record % 8 or entry.start() + 0x20 > len(data):
                continue
            factory = struct.unpack_from("<Q", data, entry.start() + 0x18)[0] - image.image_base
            begin = factory - rva
            if not 0 <= begin <= len(code) - 14 or not code.startswith(factory_prefix, begin):
                continue
            if code[begin + 9] != 0xE9:
                continue
            targets = [factory + end + struct.unpack_from("<i", code, begin + end - 4)[0]
                       for end in (9, 14)]
            yield record, factory, targets


def search_label_factories(image, rva, code):
    ro_rva, ro_size, ro_offset = image.sections.get(b".rdata", (0, 0, 0))
    ro_data = image.data[ro_offset:ro_offset + ro_size]
    constructor_layouts = ((0x54, 0x73, b"\x48\x89\x03"), (0x67, 0x9D, b"\x48\x89\x43\x18"))
    ranges = dict(image.function_ranges())
    for _, factory, targets in registered_factories(image, rva, code, SEARCH_LABEL_CLASS_ID):
        sizes, tables, initializers = [], [], []
        for target, (size_offset, table_offset, store) in zip(targets, constructor_layouts):
            allocation = target + size_offset - rva
            load = target + table_offset - rva
            store_end = load + 7 + len(store)
            if not 0 <= allocation <= load - 10 or store_end > len(code):
                break
            if ranges.get(target, 0) < rva + store_end:
                break
            if (code[allocation] != 0xB9 or code[load - 5] != 0xE8
                    or code[load:load + 3] != b"\x48\x8d\x05"
                    or not code.startswith(store, load + 7)):
                break
            table = rva + load + 7 + struct.unpack_from("<i", code, load + 3)[0]
            if table % 8 or not ro_rva <= table <= ro_rva + ro_size - 8:
                break
            first_method = struct.unpack_from("<Q", ro_data, table - ro_rva)[0] - image.image_base
            if not rva <= first_method < rva + len(code):
                break
            tables.append(table)
            sizes.append(struct.unpack_from("<I", code, allocation + 1)[0])
            initializers.append(rva + load + struct.unpack_from("<i", code, load - 4)[0])
        if tuple(sizes) in SEARCH_LABEL_LAYOUTS and initializers[0] == initializers[1]:
            yield factory, tables, tuple(sizes)


def search_grid_window_colors(image, rva, code, ranges, imports, vtable):
    primary_setters = {
        "?setPrimaryBkColor@Window@wbase@@QEAAXK@Z": "background_call",
        "?setPrimaryTextColor@Window@wbase@@QEAAXK@Z": "text_call",
    }
    palette_setters = {
        "?prepare_window_colors@wbase@@YAXPEAKPEAVIPresentationService@core@@PEAVIV8Style@3@H@Z":
            "prepare_call",
        "?setWindowColors@Window@wbase@@QEAAXPEBKH@Z": "apply_call",
    }
    ro_rva, ro_size, ro_offset = image.sections[b".rdata"]
    for slot, symbols in ((0x100, primary_setters), (0x108, palette_setters)):
        entry = vtable + slot
        if not ro_rva <= entry <= ro_rva + ro_size - 8:
            continue
        method = image.read("Q", ro_offset + entry - ro_rva) - image.image_base
        end = ranges.get(method, 0)
        if not rva <= method < end <= rva + len(code):
            continue
        updater = method
        if slot == 0x108:
            delegate = re.search(rb"\x48\x8b\xf9\xe8(.{4})", code[method - rva:end - rva], re.S)
            if delegate is None:
                continue
            updater = method + delegate.end() + struct.unpack("<i", delegate[1])[0]
            end = ranges.get(updater, 0)
            if not rva <= updater < end <= rva + len(code):
                continue
        calls = {}
        for call in re.finditer(rb"\xff\x15(.{4})", code[updater - rva:end - rva], re.S):
            target = updater + call.end() + struct.unpack("<i", call[1])[0]
            key = symbols.get(imports.get(target))
            if key:
                calls.setdefault(key, []).append(updater + call.start())
        if set(calls) == set(symbols.values()) and all(len(sites) == 1 for sites in calls.values()):
            return {
                "method_slot": f"0x{slot:X}",
                "method": f"0x{method:X}",
                "updater": f"0x{updater:X}",
                **{key: f"0x{sites[0]:X}" for key, sites in calls.items()},
            }
    return None


def search_grid_alternating_palette(rva, code, ranges, after_resolve, method_end):
    tail = code[after_resolve - rva:method_end - rva]
    # Immediately after resolving the Color, cache eax on the grid receiver.
    cache = re.match(rb"(?:\xba\x06\x00\x00\x00)?\x41\x89\x86(.{4})", tail, re.S)
    if cache is None:
        return None
    # The refresh finishes by rebuilding the palette of a separate object.
    update_calls = re.finditer(rb"\x49\x8b\x8e(.{4})\xe8(.{4})\xb0\x01", tail, re.S)
    white, alternating = ((0x70, rb"\x41\x89\x47\x74") if cache.start(1) == 3
                          else (0x7C, rb"\x41\x89\x87\x80\x00\x00\x00"))
    pair = re.compile(
        rb"\x41\xc7\x47" + re.escape(bytes((white,))) + rb"\xff\xff\xff\x00\x8b\x81"
        + re.escape(cache[1]) + alternating,
    )
    for call in update_calls:
        updater = after_resolve + call.end(2) + struct.unpack("<i", call[2])[0]
        updater_end = ranges.get(updater, 0)
        if not rva <= updater < updater_end <= rva + len(code):
            continue
        matches = list(pair.finditer(code, updater - rva, updater_end - rva))
        if len(matches) != 2:
            continue
        return {
            "cache_offset": f"0x{struct.unpack('<I', cache[1])[0]:X}",
            "cache_write": f"0x{after_resolve + cache.start(1) - 3:X}",
            "palette_receiver_offset": f"0x{struct.unpack('<I', call[1])[0]:X}",
            "palette_updater": f"0x{updater:X}",
            "palette_call": f"0x{after_resolve + call.start(2) - 1:X}",
            "fixed_white_writes": [f"0x{rva + match.start():X}" for match in matches],
            "white_offset": f"0x{white:X}",
            "alternating_offset": f"0x{white + 4:X}",
        }
    return None


def search_grid_alternating_dispatch(image, rva, code, ranges, palette, color_offset):
    if palette is None:
        return None
    receiver = re.escape(struct.pack("<I", int(palette["palette_receiver_offset"], 16)))
    alternate = int(palette["alternating_offset"], 16)
    if alternate == 0x74:
        read = rb"\x49\x8b\x87" + receiver + rb"\x8b\x48\x74"
        color_address = rb"\x4d\x8d\x87" + re.escape(color_offset)
        query = rb"\x4c\x8d\x44\x24\x70\x48\x8d\x15(.{4})\x49\x8b\xcc\xff\x10"
    else:
        read = rb"\x49\x8b\x86" + receiver + rb"\x8b\x90\x80\x00\x00\x00"
        color_address = rb"\x4d\x8d\x86" + re.escape(color_offset)
        query = rb"\x48\x8d\x55.\x48\x8d\x4d\x90\xe8(.{4})\x90\x48\x8b\x4d\x90"
    dispatch = (rb"\x83\xf8\x02\x75(?P<branch>.)\x48\x8d\x8d.{4}\xff\x15.{4}"
                rb"(?:\x89\x9d.{4}|\xc7\x85.{4}\x00\x00\x00\x00)"
                rb"(?P<read>" + read + rb").{20,100}?\xeb\x07"
                rb"(?P<fallback>" + color_address + rb").{7,24}?\xba\x04\x00\x00\x00\xff\x50\x48")
    ro_rva, ro_size, ro_offset = image.sections[b".rdata"]
    starts = sorted(ranges)
    query_pattern = re.compile(query, re.S)
    candidates = []
    for match in re.finditer(dispatch, code, re.S):
        if match.end("branch") + struct.unpack("<b", match["branch"])[0] != match.start("fallback"):
            continue
        method_index = bisect_right(starts, rva + match.start()) - 1
        if method_index < 0 or rva + match.end() > ranges[starts[method_index]]:
            continue
        query_start = max(starts[method_index] - rva, match.start("read") - 0xD0)
        for lookup in query_pattern.finditer(code, query_start, match.start()):
            address = rva + lookup.end(1) + struct.unpack("<i", lookup[1])[0]
            helper = None
            query_call = rva + lookup.end() - 2
            if alternate == 0x80:
                query_call = rva + lookup.end(1) - 5
                helper = address
                end = ranges.get(helper, 0)
                if not rva <= helper < end <= rva + len(code):
                    continue
                body = code[helper - rva:min(end, helper + 0x40) - rva]
                load = re.search(rb"\x48\x8d\x15(.{4})\x4c\x8b\xc3\xff\x10", body, re.S)
                if load is None:
                    continue
                address = helper + load.end(1) + struct.unpack("<i", load[1])[0]
            if not ro_rva <= address <= ro_rva + ro_size - len(CONTROL_PROPERTIES_ID):
                continue
            if not image.data.startswith(CONTROL_PROPERTIES_ID, ro_offset + address - ro_rva):
                continue
            candidates.append({
                "palette_read": f"0x{rva + match.start('read'):X}",
                "query": f"0x{query_call:X}",
                "query_helper": f"0x{helper:X}" if helper else None,
                "property": 4,
                "slot": "0x48",
                "dispatch": f"0x{rva + match.end() - 3:X}",
            })
    return candidates[0] if len(candidates) == 1 else None


def search_grid_candidates(image, rva, code):
    ranges = dict(image.function_ranges())
    imports = {slot: symbol for _, symbol, slot in image.imports()}
    ro_rva, ro_size, ro_offset = image.sections.get(b".rdata", (0, 0, 0))
    # mov ecx, size; call [iat] ... lea rcx,[rbx+18h]; call ctor; set vtable
    factory_pattern = re.compile(
        rb"\xb9(.{4})\xff\x15(.{4}).{0,64}\x48\x8d\x4b\x18"
        rb"\xe8(.{4})\x48\x8d\x05(.{4})\x48\x89\x43\x18", re.S)
    # Call Color(CompositeID), then assign it to the constructor receiver's field.
    assign_pattern = re.compile(
        rb"\xff\x15(.{4})\x48\x8b\xd0\x48\x8d\x8e(.{4})\xff\x15(.{4})", re.S)
    # Resolve the field through a virtual call, with kInvalidV8StyleEntryId in r9.
    resolve_pattern = re.compile(
        rb"\x48\x8b\x1d(.{4})\x48\x8b\xb8(.{4})\x49\x8b\x46\x60\xff\x50\x40"
        rb"\x49\x8d\x96(.{4})\x4c\x8b\xcb\x4c\x8b\xc0"
        rb"(?:\x49\x8b\xcc|\x49\x8b\xcf)\xff\xd7", re.S)
    for record, factory, (_, target) in registered_factories(image, rva, code, SEARCH_GRID_CLASS_ID):
        if not rva <= target < ranges.get(target, 0) <= rva + len(code):
            continue
        for setup in factory_pattern.finditer(code, target - rva, ranges[target] - rva):
            allocator = rva + setup.end(2) + struct.unpack("<i", setup[2])[0]
            if imports.get(allocator) != "?nuke_new_nothrow@@YAPEAX_K@Z":
                continue
            constructor = rva + setup.end(3) + struct.unpack("<i", setup[3])[0]
            vtable = rva + setup.end(4) + struct.unpack("<i", setup[4])[0]
            if not rva <= constructor < ranges.get(constructor, 0) <= rva + len(code):
                continue
            body = code[constructor - rva:ranges[constructor] - rva]
            for assignment in assign_pattern.finditer(body):
                color_import = constructor + assignment.end(1) + struct.unpack("<i", assignment[1])[0]
                assign_import = constructor + assignment.end(3) + struct.unpack("<i", assignment[3])[0]
                if (imports.get(color_import) != "??0Color@core@@QEAA@AEBVCompositeID@1@@Z"
                        or imports.get(assign_import) != "??4Color@core@@QEAAAEAU01@AEBU01@@Z"):
                    continue
                setup_bytes = body[max(0, assignment.start() - 40):assignment.start()]
                if not any(opcode + b"\xf3\xff\xff\xff" in setup_bytes
                           for opcode in (b"\xc7\x45\xf7", b"\xba")):
                    continue
                for method_slot in (0x1C0, 0x1D0):
                    entry = vtable + method_slot
                    if vtable % 8 or not ro_rva <= vtable <= entry <= ro_rva + ro_size - 8:
                        continue
                    method = image.read("Q", ro_offset + entry - ro_rva) - image.image_base
                    if not rva <= method < ranges.get(method, 0) <= rva + len(code):
                        continue
                    for resolve in resolve_pattern.finditer(code, method - rva, ranges[method] - rva):
                        style_import = rva + resolve.end(1) + struct.unpack("<i", resolve[1])[0]
                        if (resolve[3] != assignment[2] or imports.get(style_import)
                                != "?kInvalidV8StyleEntryId@core@@3VCompositeID@1@B"):
                            continue
                        palette = search_grid_alternating_palette(
                            rva, code, ranges, rva + resolve.end(), ranges[method])
                        yield {
                            "registration": f"0x{record:X}",
                            "factory": f"0x{factory:X}",
                            "nonzero_branch": f"0x{target:X}",
                            "constructor": f"0x{constructor:X}",
                            "vtable": f"0x{vtable:X}",
                            "allocation_size": struct.unpack("<I", setup[1])[0],
                            "field_offset": f"0x{struct.unpack('<I', assignment[2])[0]:X}",
                            "style_key": -13,
                            "method_slot": f"0x{method_slot:X}",
                            "resolver_slot": f"0x{struct.unpack('<I', resolve[2])[0]:X}",
                            "field_load": f"0x{rva + resolve.start(3) - 3:X}",
                            "window_colors": search_grid_window_colors(
                                image, rva, code, ranges, imports, vtable),
                            "alternating_palette": palette,
                            "alternating_dispatch": search_grid_alternating_dispatch(
                                image, rva, code, ranges, palette, assignment[2]),
                        }


def search_label_candidates(image, rva, code, reference_render):
    ranges = list(image.function_ranges())
    for factory, tables, sizes in search_label_factories(image, rva, code):
        text_slot, color_slot = SEARCH_LABEL_LAYOUTS[sizes]
        # The 8.3 window straddles the call return. In 8.5 it starts at the call.
        signature, return_offset = ((reference_render, 12) if text_slot == 0x218
                                    else (SEARCH_LABEL_RENDER_85, 6))
        text_call = b"\xff\x90" + struct.pack("<I", text_slot)
        color_call = b"\xff\x90" + struct.pack("<I", color_slot)
        for match in re.finditer(re.escape(signature), code):
            normal_return = match.start() + return_offset
            if not code.startswith(text_call, normal_return - len(text_call)):
                continue
            call_rva = rva + normal_return - len(text_call)
            for begin, end in ranges:
                if not begin <= call_rva < end <= rva + len(code):
                    continue
                body = code[begin - rva:end - rva]
                returns = [begin + site.end() for site in re.finditer(re.escape(text_call), body)]
                if len(returns) != 3 or returns[-1] != rva + normal_return:
                    continue
                if (body.count(color_call) != 4 or bytes.fromhex("E0 E0 E0 00") not in body
                        or bytes.fromhex("54 54 54 00") not in body):
                    continue
                yield {
                    "factory": f"0x{factory:X}",
                    "search_label_vtable": f"0x{tables[0]:X}",
                    "open_link_vtable": f"0x{tables[1]:X}",
                    "allocation_sizes": [f"0x{size:X}" for size in sizes],
                    "renderer_range_start": f"0x{begin:X}",
                    "normal_text_return": f"0x{returns[-1]:X}",
                    "text_returns": [f"0x{return_rva:X}" for return_rva in returns],
                    "graphics_text_slot": f"0x{text_slot:X}",
                    "graphics_color_slot": f"0x{color_slot:X}",
                }


def spreadsheet_view_grid_candidates(image):
    rva, code = image.text()
    ro_rva, ro_size, ro_offset = image.sections.get(b".rdata", (0, 0, 0))
    rodata = image.data[ro_offset:ro_offset + ro_size]
    names = {}
    for name in ("viewSettings", "showGrid", "showHeaders", "showPageBreaks"):
        encoded = (name + "\0").encode("utf-16le")
        for match in re.finditer(re.escape(encoded), rodata):
            names[ro_rva + match.start()] = name

    ranges = sorted(image.function_ranges())
    starts = [begin for begin, _ in ranges]
    references = {}
    lea_pattern = rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})"
    for match in re.finditer(lea_pattern, code, re.DOTALL):
        target = rva + match.end() + struct.unpack("<i", match[1])[0]
        name = names.get(target)
        if name is None:
            continue
        address = rva + match.start()
        index = bisect_right(starts, address) - 1
        if index >= 0 and address < ranges[index][1]:
            references.setdefault(starts[index], {}).setdefault(name, []).append(address)

    for begin, sites in references.items():
        if not {"showGrid", "showHeaders", "showPageBreaks"} <= sites.keys():
            continue
        start, end = max(sites["showGrid"]), min(sites["showHeaders"])
        window = code[start - rva:end - rva]
        if "viewSettings" in sites:
            # Serialize the byte at +0x30 using the writer's boolean method.
            role = "serialize"
            marker = bytes.fromhex("41 0F B6 56 30 48 8B CE FF 50 58")
            view_references = [min(sites["viewSettings"])]
        else:
            # The native XML reader is called after its caller matches viewSettings.
            role = "deserialize"
            marker = bytes.fromhex("41 88 45 30 E9")
            view_references = []
            for other_sites in references.values():
                for address in other_sites.get("viewSettings", []):
                    call = address + 0x3C
                    offset = call - rva
                    if code[offset:offset + 1] == b"\xe8" and offset + 5 <= len(code):
                        target = call + 5 + struct.unpack_from("<i", code, offset + 1)[0]
                        if target == begin:
                            view_references.append(address)
        for address in view_references:
            for match in re.finditer(re.escape(marker), window):
                yield {
                    "role": role,
                    "function": f"0x{begin:X}",
                    "show_grid_access": f"0x{start + match.start():X}",
                    "view_settings_reference": f"0x{address:X}",
                }


def spreadsheet_outline_dispatch_candidates(image):
    code_rva, code = image.text()
    data_rva, data_size, data_offset = image.sections[b".rdata"]
    data = image.data[data_offset:data_offset + data_size]
    slot = 0x80
    color_load = bytes.fromhex("8B B4 24 F0 00 00 00")
    pen_color_store = bytes.fromhex("89 74 24 6C")
    table_stores = {}
    for match in re.finditer(rb"\x48\x8d\x05(.{4})(?:\x48\x89[\x01\x03]|\x49\x89[\x04\x07])", code, re.DOTALL):
        lea = code_rva + match.start()
        table = lea + 7 + struct.unpack("<i", match[1])[0]
        table_stores.setdefault(table, []).append(f"0x{lea:X}")

    for begin, end in image.function_ranges():
        offset = begin - code_rva
        if offset < 0 or end - begin < 0xC6:
            continue
        if (code[offset + 0x35:offset + 0x3C] != color_load or
                code[offset + 0xC2:offset + 0xC6] != pen_color_store):
            continue
        pointer = struct.pack("<Q", image.image_base + begin)
        tables = []
        for match in re.finditer(re.escape(pointer), data):
            table = data_rva + match.start() - slot
            if table % 8 == 0 and table in table_stores:
                tables.append({
                    "vtable": f"0x{table:X}",
                    "table_loads": table_stores[table],
                })
        if tables:
            yield {"handler": f"0x{begin:X}", "slot": f"0x{slot:X}", "tables": tables}


def spreadsheet_grid_strip_candidates(image):
    rva, code = image.text()
    # The setter copies ShowGrid from view settings into this+disp32.
    # Its this pointer addresses the +0x240 subobject of the view.
    setter = rb"\x41\x0f\xb6\x45\x30\x88\x86(.{4})\x41\x0f\xb6\x45\x32"
    for write in re.finditer(setter, code, re.DOTALL):
        flag = struct.unpack("<I", write[1])[0] + 0x240
        guard = (rb"\x48\x8b\x47(?P<view>[\x40\x68])\x80\xb8"
                 + re.escape(struct.pack("<I", flag))
                 + rb"\x00\x0f\x84(?P<skip>.{4})")
        # The horizontal call passes height=1 and the vertical call width=1.
        # Both pass PATCOPY and use the same field in the paint object.
        strips = re.compile(
            guard
            + rb".{19}\x48\x8b\x0d(?P<h_cache>.{4}).{11}"
              rb"\x48\x8b\x57(?P<context>[\x50\x78]).{8}"
              rb"\xc7\x44\x24\x30\x21\x00\xf0\x00\x41\xff\xc8"
              rb"\xc7\x44\x24\x28\x01\x00\x00\x00\x89\x44\x24\x20"
              rb"(?P<h_call>\x41\xff\x92(?P<slot>\xe8\x01|\x08\x02)\x00\x00)"
              rb".{19}\x48\x8b\x0d(?P<v_cache>.{4}).{10}"
              rb"\x48\x8b\x57(?P=context).{8}"
              rb"\xc7\x44\x24\x30\x21\x00\xf0\x00\x41\xff\xc8"
              rb"\x89\x44\x24\x28\xc7\x44\x24\x20\x01\x00\x00\x00"
              rb"(?P<v_call>\x41\xff\x92(?P=slot)\x00\x00)",
            re.DOTALL,
        )
        for match in strips.finditer(code):
            horizontal_cache = match.end("h_cache") + struct.unpack("<i", match["h_cache"])[0]
            vertical_cache = match.end("v_cache") + struct.unpack("<i", match["v_cache"])[0]
            skip_target = match.end("skip") + struct.unpack("<i", match["skip"])[0]
            if horizontal_cache != vertical_cache or skip_target != match.end("v_call"):
                continue
            yield {
                "show_grid_write": f"0x{rva + write.start() + 5:X}",
                "show_grid_check": f"0x{rva + match.start() + 4:X}",
                "flag_offset": f"0x{flag:X}",
                "view_offset": f"0x{match['view'][0]:X}",
                "context_offset": f"0x{match['context'][0]:X}",
                "graphics_cache": f"0x{rva + horizontal_cache:X}",
                "graphics_slot": f"0x{struct.unpack('<H', match['slot'])[0]:X}",
                "horizontal_return": f"0x{rva + match.end('h_call'):X}",
                "vertical_return": f"0x{rva + match.end('v_call'):X}",
            }


def search_form_loaders(image):
    rva, code = image.text()
    ro_rva, ro_size, ro_offset = image.sections.get(b".rdata", (0, 0, 0))
    ro_data = image.data[ro_offset:ro_offset + ro_size]
    functions = sorted(image.function_ranges())
    starts = [start for start, _ in functions]
    form_slot = bytes.fromhex("4C 8B 81 C8 01 00 00 48 8B D3 48 8B C8 41 FF D0")
    names_by_address = {
        ro_rva + hit.start(): name
        for name in ("fnrresults.f", "fnrtreesearch.f")
        for hit in re.finditer(re.escape((name + "\0").encode("utf-16-le")), ro_data)
    }
    seen = set()
    for ref in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})",
                           code, re.DOTALL):
        address = rva + ref.end() + struct.unpack("<i", ref[1])[0]
        name = names_by_address.get(address)
        site = rva + ref.start()
        index = bisect_right(starts, site) - 1
        if name is None or index < 0:
            continue
        start, end = functions[index]
        if site >= end or (name, start) in seen:
            continue
        call = code.find(form_slot, ref.end(), end - rva)
        if call < 0:
            continue
        seen.add((name, start))
        yield {
            "resource": name,
            "resource_name_rva": f"0x{address:X}",
            "resource_reference": f"0x{site:X}",
            "function": f"0x{start:X}",
            "window_call": f"0x{rva + call + len(form_slot) - 3:X}",
        }


def compare(directory, signatures):
    result = {"directory": str(directory), "clients": [], "modules": {}}
    for name in ("1cv8.exe", "1cv8c.exe"):
        if (directory / name).is_file():
            image = Image(directory / name)
            result["clients"].append({"name": name, "sha256": image.sha256()})
    for name, windows in signatures.items():
        image = Image(directory / name)
        rva, code = image.text()
        summary = {"sha256": image.sha256(), "exact_windows": {}}
        for site in SITES[name]:
            # Lookahead includes overlapping matches, preserving ambiguity.
            matches = re.finditer(b"(?=" + re.escape(windows[site]) + b")", code)
            summary["exact_windows"][site] = [f"0x{rva + match.start() + 8:X}" for match in matches]
        if name == "frame.dll":
            summary["frame_color_candidates"] = frame_color_candidates(image, rva, code)
            summary["property_palette_candidates"] = list(property_palette_candidates(
                image, rva, code, windows["property_paint"]))
            summary["dropdown_candidates"] = list(dropdown_candidates(
                image, rva, code, windows["dropdown_descriptors"]))
            summary["search_label_candidates"] = list(search_label_candidates(
                image, rva, code, windows["search_label_render"]))
            summary["search_grid_candidates"] = list(search_grid_candidates(image, rva, code))
            summary["search_results_toolbar_factories"] = [
                {
                    "registry_record": f"0x{record:X}",
                    "factory": f"0x{factory:X}",
                    "zero_argument_target": f"0x{targets[0]:X}",
                    "nonzero_argument_target": f"0x{targets[1]:X}",
                }
                for record, factory, targets in registered_factories(
                    image, rva, code, SEARCH_RESULTS_TOOLBAR_CLASS_ID)
            ]
        elif name == "grphcs.dll":
            cairo_imports = {slot: symbol for dll, symbol, slot in image.imports() if dll == "cairo.dll"}
            imports = {slot: symbol for slot, symbol in cairo_imports.items()
                       if symbol in CAIRO_COLORS | {"cairo_set_scaled_font", "cairo_fill"}}
            summary["spreadsheet_rectangle_cairo_candidates"] = list(
                spreadsheet_rectangle_cairo_candidates(rva, code, cairo_imports))
            summary["cairo_imports"] = {symbol: f"0x{slot:X}" for slot, symbol in imports.items()
                                        if symbol in CAIRO_COLORS}
            # Opcode candidates only. Confirm instruction boundaries with a disassembler.
            calls = {symbol: [] for symbol in imports.values()}
            for match in re.finditer(b"(?=\xff\x15)", code):
                offset = match.start()
                if offset + 6 > len(code):
                    continue
                displacement = struct.unpack_from("<i", code, offset + 2)[0]
                symbol = imports.get(rva + offset + 6 + displacement)
                if symbol:
                    calls[symbol].append(rva + offset)
            summary["cairo_call_candidates"] = {
                symbol: [f"0x{address:X}" for address in addresses]
                for symbol, addresses in calls.items() if symbol in CAIRO_COLORS
            }
            rgba_calls = set(calls.get("cairo_set_source_rgba", []))
            summary["fill_rgba_candidates"] = list(fill_candidates(rva, code, rgba_calls))
            summary["text_color_candidates"] = list(text_color_candidates(
                image, rva, code, rgba_calls, set(calls.get("cairo_set_scaled_font", [])),
                set(calls.get("cairo_fill", []))))
        result["modules"][name] = summary
    path = directory / "moxel.dll"
    if path.is_file():
        image = Image(path)
        result["modules"]["moxel.dll"] = {
            "sha256": image.sha256(),
            "spreadsheet_view_grid_candidates": list(spreadsheet_view_grid_candidates(image)),
        }
    path = directory / "moxelui.dll"
    if path.is_file():
        image = Image(path)
        result["modules"]["moxelui.dll"] = {
            "sha256": image.sha256(),
            "spreadsheet_grid_strip_candidates": list(spreadsheet_grid_strip_candidates(image)),
            "spreadsheet_outline_dispatch_candidates": list(spreadsheet_outline_dispatch_candidates(image)),
        }
    path = directory / "frntend.dll"
    if path.is_file():
        image = Image(path)
        result["modules"]["frntend.dll"] = {
            "sha256": image.sha256(),
            "search_form_loaders": list(search_form_loaders(image)),
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_bin", type=Path, help="8.3.27.2325 x64 bin directory")
    parser.add_argument("candidate_bin", nargs="+", type=Path, help="x64 bin directories to compare")
    arguments = parser.parse_args()
    try:
        signatures = reference_windows(arguments.reference_bin)
        results = [compare(directory, signatures) for directory in arguments.candidate_bin]
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
