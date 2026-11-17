"""Check offline locator baselines and in-memory mutations of installed x64 DLLs.

Repeat --bin for each known platform bin directory. frame.dll is required;
frntend.dll is checked where installed. No binaries are copied or modified.
"""

import argparse
from copy import copy
from pathlib import Path
import re
import runpy
import struct


def address(record, field):
    return int(record[field], 16)


def baseline(find, image):
    hits = list(find(image))
    if len(hits) != 1:
        raise ValueError(f"{image.path}: expected one {find.__name__}, found {len(hits)}")
    return hits[0]


def reject(find, image, mutations):
    for name, offset, replacement in mutations:
        variant = copy(image)
        variant.data = image.data[:offset] + replacement + image.data[offset + len(replacement):]
        if variant.data == image.data:
            raise ValueError(f"{image.path}: mutation did not change bytes: {name}")
        if list(find(variant)):
            raise ValueError(f"{image.path}: {find.__name__} accepted {name}")
    return len(mutations)


def patch(image, name, rva, replacement):
    return name, image.offset(rva), replacement


def redirect(image, name, operand, target):
    return patch(image, name, operand, struct.pack("<i", target - operand - 4))


def clip_unwind(image, name, start, end):
    _, size, offset = image.sections[b".pdata"]
    pdata = image.data[offset:offset + size]
    for index, (begin, finish, _) in enumerate(struct.iter_unpack("<III", pdata[:size - size % 12])):
        if begin == start and end < finish:
            return name, offset + index * 12 + 4, struct.pack("<I", end)
    raise ValueError(f"{image.path}: cannot clip unwind range at 0x{start:X}")


def verify_dock(find, image):
    hit = baseline(find, image)
    updater = address(hit, "function")
    last_call = address(hit["style_calls"], "0xB0")
    mutations = [
        clip_unwind(image, "clipped final style call", updater, last_call + 5),
        patch(image, "wrong style slot", address(hit["style_calls"], "0x80") + 2, b"\x88"),
    ]
    if hit["layout"] == "8.3":
        mutations.append(patch(image, "wrong flag field", address(hit, "flag_write") + 2, b"\x77"))
    else:
        table, = hit["tables"]
        vtable = address(table, "vtable")
        setter = address(table, "flag_setter")
        predicate = address(table, "active_predicate")
        updater_va = struct.pack("<Q", image.image_base + updater)
        predicate_va = struct.pack("<Q", image.image_base + predicate)
        mutations.extend([
            patch(image, "null predicate", vtable + 0xF8, bytes(8)),
            patch(image, "predicate points to updater", vtable + 0xF8, updater_va),
            patch(image, "wrong predicate flag field", predicate + 6, b"\x41"),
            patch(image, "setter points to updater", vtable + 0x40, updater_va),
            patch(image, "wrong setter flag field", setter + 5, b"\x41"),
            patch(image, "updater points to predicate", vtable + 0xF0, predicate_va),
            clip_unwind(image, "clipped predicate entry fragment", predicate, predicate + 21),
        ])
    return reject(find, image, mutations)


def instruction_operand(code, start, end, pattern):
    """Return the location of the sole captured operand in an anchor window."""
    match, = re.compile(pattern, re.DOTALL).finditer(code, start, end)
    return match.start(1)


def verify_counter(find, image):
    hit = baseline(find, image)
    initializer, = hit["label_initializers"]
    handler = address(hit, "handler")
    formatter = address(hit, "formatter_function")
    descriptor = address(hit, "command_descriptor")
    rva, code = image.text()

    def operand(start, end, pattern):
        return rva + instruction_operand(code, start - rva, end - rva, pattern)

    def target(field):
        return field + 4 + image.read("i", image.offset(field))

    test = address(hit, "command_test")
    high = operand(test, test + 50, rb"\x48\x33\x0d(.{4})")
    skip = operand(test, test + 55, rb"\x84\xc0(?:\x74|\x0f\x84)(.)")
    skip_size = 1 if code[skip - rva - 1] == 0x74 else 4
    branch = skip + skip_size
    branch_end = branch + int.from_bytes(code[skip - rva:branch - rva], "little", signed=True)
    loader = address(initializer, "resource_reference")
    load = operand(loader, loader + 42, rb"\xff\x15(.{4})")
    assign = operand(load + 4, loader + 42, rb"\x88\x00\x00\x00\x48\x8d(?:\x55.|\x54\x24.)\xe8(.{4})")
    fmt = address(hit, "format_call") + 2
    setter = address(hit, "text_setter_call")
    copy_setup = rb"(?:\x49\x8b\xd7\x48\x8d\x4d.|\x48\x8b\xd5\x48\x8d\x4c\x24.)\xe8"
    copy_call = operand(max(formatter, fmt - 128), fmt, copy_setup + rb"(.{4})")
    core_dll = next(dll for dll, _, slot in image.imports() if slot == target(load))
    for relative in range(0, image.import_size - 19, 20):
        dll_name = image.read("I", image.offset(image.import_rva + relative) + 12)
        if dll_name and image.name_at(dll_name).lower() == core_dll:
            break
    else:
        raise ValueError(f"{image.path}: cannot locate the core import descriptor")

    mutations = [
        patch(image, "wrong resource key", address(initializer, "resource_name_rva"), b"!"),
        patch(image, "wrong GUID", descriptor, b"\0"),
        patch(image, "wrong ID", descriptor + 16, b"\2"),
        redirect(image, "wrong GUID high reference", high, descriptor),
        patch(image, "backward branch", skip, b"\xff" * skip_size),
        patch(image, "shortened branch", skip, b"\1" + bytes(skip_size - 1)),
        redirect(image, "wrong formatter import", fmt, target(load)),
        redirect(image, "wrong loader import", load, target(fmt)),
        patch(image, "wrong core DLL name", dll_name, b"!"),
        patch(image, "wrong command UI slot", setter + 2, b"\x48"),
        patch(image, "missing label copy", copy_call - 1, b"\x90" * 5),
        patch(image, "jump over branch at entry", branch, b"\xe9" + struct.pack("<i", branch_end - branch - 5)),
        patch(image, "missing loader assignment", assign - 1, b"\x90" * 5),
        redirect(image, "loader assignment points to handler", assign, handler),
        redirect(image, "formatter copy points to handler", copy_call, handler),
        clip_unwind(image, "clipped label initializer", address(initializer, "function"), address(initializer, "function") + 1),
        clip_unwind(image, "clipped handler", handler, handler + 1),
    ]
    if formatter != handler:
        mutations.append(clip_unwind(image, "clipped formatter", formatter, formatter + 1))
    if hit["helper_call"]:
        mutations.append(redirect(image, "helper call past entry", address(hit, "helper_call") + 1, formatter + 1))
    return reject(find, image, mutations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", dest="directories", type=Path, required=True, action="append")
    args = parser.parse_args()
    compare = runpy.run_path(str(Path(__file__).resolve().parents[2] / "compare-platform.py"))
    image = compare["Image"]
    counter_total = dock_total = 0
    for directory in args.directories:
        try:
            dock_count = verify_dock(compare["dock_title_style_candidates"], image(directory / "frame.dll"))
            frontend = directory / "frntend.dll"
            counter_count = verify_counter(compare["search_counter_candidates"], image(frontend)) if frontend.is_file() else 0
        except (OSError, ValueError) as error:
            parser.exit(1, f"{directory}: {error}\n")
        counter_total += counter_count
        dock_total += dock_count
        counter_status = f"1 candidate, {counter_count} rejected mutations" if counter_count else "frntend.dll absent"
        print(f"{directory}: dock 1 candidate, {dock_count} rejected mutations; counter {counter_status}")
    print(f"Total rejected: {counter_total} counter, {dock_total} dock")


if __name__ == "__main__":
    main()
