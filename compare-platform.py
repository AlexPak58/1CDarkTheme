"""Compare x64 1C rendering binaries offline. Matches are not verified hooks."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


REFERENCE_HASHES = {
    "frame.dll": "d2f4367c1f4de24747440a4cf963961585bdf0d854a3c324741f912db123f2d7",
    "grphcs.dll": "125c00c01e7780704ad3e2159170dd7e1a05ef8cf9b2ed32b0b30cb535c4a563",
}
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
    return signatures


def fill_candidates(rva, code, rgba_calls):
    for match in re.finditer(re.escape(FILL_PREFIX) + b".{4}", code, re.S):
        callee = rva + match.end() + struct.unpack_from("<i", code, match.end() - 4)[0]
        # This layout has an indirect call to the RGBA import 0x53 bytes into the callee.
        rgba_call = callee + 0x53
        if rgba_call in rgba_calls:
            yield {"site": f"0x{rva + match.start() + 11:X}",
                   "callee": f"0x{callee:X}", "rgba_call": f"0x{rgba_call:X}"}


def compare(directory, signatures):
    result = {"directory": str(directory), "clients": [], "modules": {}}
    for name in ("1cv8.exe", "1cv8c.exe"):
        if (directory / name).is_file():
            image = Image(directory / name)
            result["clients"].append({"name": name, "sha256": image.sha256()})
    for name, sites in signatures.items():
        image = Image(directory / name)
        rva, code = image.text()
        summary = {"sha256": image.sha256(), "exact_windows": {}}
        for site, signature in sites.items():
            # Lookahead includes overlapping matches, preserving ambiguity.
            matches = re.finditer(b"(?=" + re.escape(signature) + b")", code)
            summary["exact_windows"][site] = [f"0x{rva + match.start() + 8:X}" for match in matches]
        if name == "grphcs.dll":
            imports = {slot: symbol for dll, symbol, slot in image.imports()
                       if dll == "cairo.dll" and symbol in CAIRO_COLORS}
            summary["cairo_imports"] = {symbol: f"0x{slot:X}" for slot, symbol in imports.items()}
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
                for symbol, addresses in calls.items()
            }
            rgba_calls = set(calls.get("cairo_set_source_rgba", []))
            summary["fill_rgba_candidates"] = list(fill_candidates(rva, code, rgba_calls))
        result["modules"][name] = summary
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
