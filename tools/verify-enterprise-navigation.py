"""Verify navigation and subsystem skins against installed .2130 resources.

Optional --fixtures exports the verified navigation and subsystem PNGs for tests.
"""
import argparse
import hashlib
import pathlib
import re
import struct
import zlib


def decode(data, rounding=127):
    assert rounding in (0, 127)
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, compressed = 8, b''
    while pos < len(data):
        length = struct.unpack_from('>I', data, pos)[0]
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        assert zlib.crc32(kind + chunk) & 0xffffffff == struct.unpack_from('>I', data, pos + 8 + length)[0]
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', chunk)
            assert (depth, color, compression, filtering, interlace) == (8, 6, 0, 0, 0)
        if kind == b'IDAT':
            compressed += chunk
        pos += length + 12
    raw, previous, result = zlib.decompress(compressed), bytearray(width * 4), bytearray()
    assert len(raw) == (width * 4 + 1) * height
    for y in range(height):
        start = y * (width * 4 + 1)
        mode, row = raw[start], bytearray(raw[start + 1:start + 1 + width * 4])
        assert mode <= 4
        for x in range(len(row)):
            a, b, c = row[x - 4] if x >= 4 else 0, previous[x], previous[x - 4] if x >= 4 else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            predictor = (0, a, b, (a + b) // 2, a if pa <= pb and pa <= pc else b if pb <= pc else c)[mode]
            row[x] = (row[x] + predictor) & 255
        for x in range(0, len(row), 4):
            r, g, b, a = row[x:x + 4]
            result.extend(((b * a + rounding) // 255, (g * a + rounding) // 255, (r * a + rounding) // 255, a))
        previous = row
    return width, height, bytes(result)


def assets(path, pattern=r'tl(?:Back|Fwd)(?:Disabled)?_(?:85|100|125|150|175|200|300|400)\.png', count=32):
    data, pos, found = path.read_bytes(), 0, {}
    while True:
        pos = data.find(b'PK\x03\x04', pos)
        if pos < 0:
            break
        header = struct.unpack_from('<I5H3I2H', data, pos)
        name = data[pos + 30:pos + 30 + header[9]].decode('utf-8', 'replace')
        if re.fullmatch(pattern, name):
            assert name not in found and not header[2] & 8
            start = pos + 30 + header[9] + header[10]
            png = data[start:start + header[7]]
            if header[3] == 8:
                png = zlib.decompress(png, -15)
            else:
                assert header[3] == 0
            assert len(png) == header[8] and zlib.crc32(png) & 0xffffffff == header[6]
            found[name] = (png, *decode(png))
        pos += 4
    assert len(found) == count, len(found)
    return found


def tile_records(resource):
    records = set()
    for family, count, tiles in [('check_83l', 8, 21), ('ncpager-83', 8, 16)]:
        pattern = rf'{family}_(?:85|100|125|150|175|200|300|400)\.png'
        for name, (png, width, height, rounded) in sorted(assets(resource, pattern, count).items()):
            assert width % tiles == 0, name
            tile = width // tiles
            for image in (rounded, decode(png, 0)[2]):
                for index in range(tiles):
                    pixels = b''.join(image[(y * width + index * tile) * 4:
                                            (y * width + (index + 1) * tile) * 4]
                                      for y in range(height))
                    if not any(pixels):  # Cairo may use the same empty tile elsewhere.
                        continue
                    digest = hashlib.sha256(pixels).hexdigest()
                    records.add((tile, height, digest, family == 'check_83l'))
    return ''.join(f'    {{{w}, {h}, "{digest}", {str(checkbox).lower()}}},\n'
                   for w, h, digest, checkbox in sorted(records))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resource', type=pathlib.Path, default=pathlib.Path(r'C:\Program Files\1cv8\8.3.27.2130\bin\mngcore_root.res'))
    parser.add_argument('--emit', action='store_true')
    parser.add_argument('--emit-tiles', action='store_true')
    parser.add_argument('--fixtures', type=pathlib.Path)
    args = parser.parse_args()
    tile_source = tile_records(args.resource.with_name('core83_root.res'))
    if args.emit_tiles:
        print(tile_source, end='')
        raise SystemExit(0)
    tiles_header = pathlib.Path(__file__).resolve().parents[1] / 'enterprise_control_tiles.inc'
    assert tiles_header.read_text('ascii') == tile_source, 'Control tile fingerprints are stale'
    print(f'Checkbox and pager tiles: {len(tile_source.splitlines())} verified fingerprints')
    entries = assets(args.resource)
    subsystem_icons = assets(args.resource,
                            r'defTopSubsystem(?:_16)?_(?:85|100|125|150|175|200|300|400)\.png', 16)
    if args.fixtures:
        assert not args.fixtures.exists(), 'Use a new external fixture directory'
        assert not args.fixtures.resolve().is_relative_to(pathlib.Path(__file__).resolve().parents[1])
        args.fixtures.mkdir(parents=True)
    header = pathlib.Path(__file__).resolve().parents[1] / 'enterprise_navigation.hpp'
    source = header.read_text('utf-8') if header.exists() else ''
    if not args.emit:
        checkboxes = assets(args.resource.with_name('core83_root.res'),
                            r'(?:checked|unchecked|discheck|check_83l)_(?:85|100|125|150|175|200|300|400)\.png', 32)
        for name, (png, width, height, pixels) in checkboxes.items():
            for variant, decoded in [('rounded', pixels), ('floor', decode(png, 0)[2])]:
                digest = hashlib.sha256(decoded).hexdigest()
                assert f'{{{width}, {height}, "{digest}", true}}, // {name} {variant}' in source, (name, variant)
        print('Checkbox skins: 32 platform resource CRCs, 64 decoded hashes verified')
    for name, (png, width, height, pixels) in sorted((entries | subsystem_icons).items()):
        for variant, decoded in [('rounded', pixels), ('floor', decode(png, 0)[2])]:
            digest = hashlib.sha256(decoded).hexdigest()
            kind = ', false, Skin::subsystem' if name in subsystem_icons else ''
            record = f'    {{{width}, {height}, "{digest}"{kind}}}, // {name} {variant}'
            if args.emit:
                print(record)
            else:
                assert record in source, (name, variant)
            # The two observed .2130 resource-pipeline surfaces are exactly
            # reproduced by floor premultiplication, rather than Cairo's round.
            if variant == 'floor' and name == 'tlBack_100.png':
                assert digest == 'cd839a613c99e3d670e287f19fe85f36cc68038faf2531f5009319e0b8a9d9c5'
            if variant == 'floor' and name == 'tlFwdDisabled_100.png':
                assert digest == 'd4bede7273084e1e7d864e51dad9f965363b40f9f7caba0965db477ff5fcfe6e'
            changed = bytearray(decoded)
            changed[0] ^= 1
            assert hashlib.sha256(changed).hexdigest() != digest
        if args.fixtures:
            (args.fixtures / name).write_bytes(png)
            (args.fixtures / (name + '.floorbgra')).write_bytes(decode(png, 0)[2])
    if not args.emit:
        print('PASS: 48 navigation and subsystem resource CRCs, 96 rounded/floor BGRA digests and mutation rejections')
