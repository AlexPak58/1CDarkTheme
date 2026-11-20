"""Read-only admission of Enterprise .2130 Cairo imports and geometry routes."""
from pathlib import Path
import re
import runpy
import sys

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'C:\Program Files\1cv8\8.3.27.2130\bin')
Image = runpy.run_path(str(root / 'compare-platform.py'))['Image']
source = (root / 'enterprise.cpp').read_text('utf-8')
platform = (root / 'platform.hpp').read_text('utf-8')
images = {name: Image(binary / name) for name in ('1cv8.exe', '1cv8c.exe', 'grphcs.dll', 'cairo.dll', 'WebKit.dll')}
for symbol, name in [('exeHash', '1cv8.exe'), ('thinClientHash', '1cv8c.exe'),
                     ('graphicsHash', 'grphcs.dll'), ('cairoHash', 'cairo.dll')]:
    expected = re.search(symbol + r' =\s*L"([A-F0-9]+)"', platform)[1]
    assert images[name].sha256().upper() == expected, name
webkit_hash = 'D8AC7240222CC741794308A92FB01D2AF04E9134ABB1215666999D52703F026C'
assert webkit_hash in source and images['WebKit.dll'].sha256().upper() == webkit_hash
routes = {
    'grphcs.dll': {'cairo_set_source_rgb': 0xC7B60, 'cairo_set_source_rgba': 0xC7C20,
                   'cairo_set_source': 0xC7B08, 'cairo_pattern_create_for_surface': 0xC7BE0},
    'WebKit.dll': {'cairo_set_source_rgb': 0x11DB4B0, 'cairo_set_source_rgba': 0x11DB328,
                   'cairo_set_source': 0x11DB5F0},
}
for name, entries in routes.items():
    imports = {(dll, function): rva for dll, function, rva in images[name].imports()}
    for function, rva in entries.items():
        assert imports['cairo.dll', function] == rva, (name, function)
        assert f'0x{rva:X}' in source, (name, function, 'source')
graphics = images['grphcs.dll']
windows = {
    0x1371E: 'FF15E4430B00',
    0x13727: 'FF15BB430B00',
    0x7444: 'FF15D6070C00488B4D30FF153C060C00',
    0xAE4D8: 'E823050000', 0xAE581: 'E87A040000',
    0xAEA05: '574883EC3080B9D900000000',
    0xAEA53: 'FF15C7910100',
    0xAEAB6: '488B7424484883C4305F48FF2599900100',
    0xABDED: 'FF1575BC0100',
}
for rva, expected in windows.items():
    body = bytes.fromhex(expected)
    start = graphics.offset(rva)
    assert graphics.data[start:start + len(body)] == body, hex(rva)
webkit = images['WebKit.dll']
assert '0x1109F0' in source
for rva, expected in {
    0x110946: '488D05A3000000',  # address used by class registration
    0x1109F0: '40555356574154415541564157488BEC4883EC68',
    0x1111EB: '4181FF1A0300000F85F9FAFFFF498BCDE8B075FCFF4885C00F84EBFAFFFF488BCBE82FA1FFFF',
    0x10B441: '488BBBB00000004889B3B0000000',
    0x10B479: '488BBBC00000004889B3C0000000',
}.items():
    body = bytes.fromhex(expected)
    start = webkit.offset(rva)
    assert webkit.data[start:start + len(body)] == body, hex(rva)
print('Enterprise native admission PASS: 5 hashes, 7 Cairo imports, 14 instruction windows')
