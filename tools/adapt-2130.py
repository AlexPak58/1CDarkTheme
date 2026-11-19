"""Regenerate the guarded .2130 native profile from the pinned upstream source.
No guessed global-search devtool or experimental thumb identity is admitted.
"""
from pathlib import Path
import hashlib, json, os, re, struct, subprocess, sys, types
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
BIN = Path(r'C:\Program Files\1cv8\8.3.27.2130\bin')
UPSTREAM = 'b412b20'
def upstream(name):
    return subprocess.check_output(['git','show',f'{UPSTREAM}:{name}'],cwd=ROOT).decode('utf-8')
module = types.ModuleType('platform_compare')
exec(upstream('compare-platform.py'), module.__dict__)
images = {name: module.Image(BIN / name) for name in ('1cv8.exe','1cv8c.exe','frame.dll','grphcs.dll','wbase83.dll','cairo.dll','frntend.dll','devtool.dll')}
def data(name, rva, size):
    image = images[name]; offset = image.offset(rva)
    return image.data[offset:offset+size]
def digest(name): return images[name].sha256().upper()
def write(name, content): (ROOT / name).write_text(content,encoding='utf-8')
def replace_hex(content, mapping):
    return re.sub(r'0x([0-9A-Fa-f]+)', lambda m: '0x%X' % mapping.get(int(m[1],16),int(m[1],16)),content)
FRAME_DATA = {
    0xABEDB0:0xABF108, 0xB20900:0xB213A8,
  0xAD3D38:0xAD36D8,0xACD3B8:0xACCD58,
  0xB22710:0xB224A8,0xB22788:0xB22620,0xB23F20:0xB23EA8,0xB23848:0xB23578,
  0xAFE280:0xAFDE70,0xAFE100:0xAFE110,0xAFE088:0xAFE098,
  0xAFDE60:0xAFE5C0,0xAFE768:0xAFE548,
  0xAFEB48:0xAFEB18,0xAFE950:0xAFE8D0,0xAFEA50:0xAFEA18,0xAFE9D0:0xAFE998,
  0xB2FC08:0xB2FA68,0xB2F0B8:0xB2FE10,0xB2EFF8:0xB2FD50,
}
# Frame paint ABI for the .27.2130 family: code moves by 30h while data tables
# are derived independently from target constructor LEA/store chains.
def frame_rva(value):
    return FRAME_DATA.get(value,value-0x30 if 0x200000 < value < 0xA00000 else value)
def graphics_rva(value): return value-0x8D0 if 0x70000 < value < 0xC0000 else value
checks = []
for rva, expected in [(0x303424,'448bc6'),(0x9518BC,'448bc0'),(0x5D4AEE,'8bd8'),(0x5D40D4,'448bc0')]:
    assert data('frame.dll',rva,len(bytes.fromhex(expected))) == bytes.fromhex(expected)
    checks.append({'module':'frame.dll','rva':hex(rva),'bytes':expected})
for rva, expected in [(0xAE57E,'0f45d0'),(0x22D7,'418bc0'),(0xAE4D3,'8bd7')]:
    assert data('grphcs.dll',rva,len(bytes.fromhex(expected))) == bytes.fromhex(expected)
    checks.append({'module':'grphcs.dll','rva':hex(rva),'bytes':expected})
# Constructor assignments establish interface identity, independently of code
# deltas or unrelated tables that share an implementation.
for site, table in [(0x740ECF,0xAFDE70),(0x740EEF,0xAFE098),(0x740EFA,0xAFE110),
 (0x73FDBF,0xAFEB18),(0x73FDDF,0xAFE548),(0x73FDEA,0xAFE5C0),
 (0x8B6248,0xB23EA8),(0x8B62B5,0xB23578),
 (0x54C4AF,0xABF108),(0x91238C,0xB2FA68),(0x9123A4,0xB2FE10),(0x9123C0,0xB2FD50)]:
    raw=data('frame.dll',site,16)
    assert raw[:2]==bytes.fromhex('488d') and raw[2]&0xC7==5 and site+7+struct.unpack_from('<i',raw,3)[0]==table,(hex(site),hex(table))
    checks.append({'module':'frame.dll','constructor_lea':hex(site),'table':hex(table),'bytes':raw.hex()})
# Verify exact vtable methods and the new constructor-derived interface tables.
for table, slot, method in [(0xAFE098,0x18,0x73B3E0),(0xAFE548,0x18,0x73B860),
 (0xAFEB18,0x38,0x73BC00),(0xAFE8D0,0x38,0x73BDC0),(0xAFEA18,0x38,0x73C0B0),(0xAFE998,0x38,0x73C250),
 (0xB2FD50,0x18,0x92AF00),(0xB224A8,0x38,0x8BA9C0),(0xB22620,0x38,0x8BC3C0)]:
    assert struct.unpack('<Q',data('frame.dll',table+slot,8))[0] == images['frame.dll'].image_base+method,(hex(table),hex(slot))
    checks.append({'module':'frame.dll','table':hex(table),'slot':hex(slot),'method':hex(method)})
for table, slot, method in [(0x3CA78,0xC0,0x1E860),(0x3CD00,0xC0,0x1EFE0),
 (0x3CF98,0x10,0x17400),(0x3D348,0x10,0x19290),(0x3D348,0x30,0x1B930)]:
    assert struct.unpack('<Q',data('wbase83.dll',table+slot,8))[0] == images['wbase83.dll'].image_base+method,(hex(table),hex(slot))
    checks.append({'module':'wbase83.dll','table':hex(table),'slot':hex(slot),'method':hex(method)})
# The new panel hooks are instruction-displacing: preserve the entire new
# target 16-byte guard, not a mask with unchecked RIP displacements.
panel = upstream('panel_headers.hpp')
patch_pattern = re.compile(r'\{(0x[0-9A-F]+), \{([^}]+)\}\}')
def patch(match):
    original = int(match[1],16); target = frame_rva(original)
    old = bytes(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]+)',match[2]))
    actual = data('frame.dll',target,16)
    assert actual[:3] == old[:3],(hex(original),hex(target))
    checks.append({'module':'frame.dll','rva':hex(target),'bytes':actual.hex(),'upstream':hex(original)})
    return '{0x%X, {%s}}' % (target,','.join('0x%02X'%x for x in actual))
panel = patch_pattern.sub(patch,panel)
# Replace references separately; newly produced target patch RVAs must not move again.
head, tail = panel.split('inline constexpr ULONG_PTR limit',1)
nums = set(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{5,})',tail))
panel = head+'inline constexpr ULONG_PTR limit'+replace_hex(tail,{v:frame_rva(v) for v in nums if v<0xC00000})
panel=panel.replace('8.3.27.2325','8.3.27.2130')
write('panel_headers.hpp',panel)
# Existing base-theme guards and addresses retain prior verified port.
theme = upstream('theme.cpp')
theme = re.sub(r'(frameBase \+ )(0x[0-9A-Fa-f]+)',lambda m:m[1]+'0x%X'%frame_rva(int(m[2],16)),theme)
theme = re.sub(r'(graphicsBase \+ )(0x[0-9A-Fa-f]+)',lambda m:m[1]+'0x%X'%graphics_rva(int(m[2],16)),theme)
for name in ('treeTabVtable','windowTabPaintReturn','windowPanelPaintReturn','propertyPaintReturn','propertyLinkVtable','sharedListPaintReturn'):
    theme=re.sub(r'('+name+r' = )(0x[0-9A-Fa-f]+)',lambda m:m[1]+'0x%X'%frame_rva(int(m[2],16)),theme)
theme=theme.replace('(shortcut ? 0x73499F : 0x73480F)','(shortcut ? 0x73496F : 0x7347DF)')
write('theme.cpp',theme)
for name in ('dock_splitters.hpp','scroller_corner.hpp','scroller_png.hpp','thumb_paint.hpp'):
    content=upstream(name)
    content=re.sub(r'((?:frame|b\.frame) \+ )(0x[0-9A-Fa-f]+)',lambda m:m[1]+'0x%X'%frame_rva(int(m[2],16)),content)
    content=re.sub(r'((?:graphics|b\.graphics) \+ )(0x[0-9A-Fa-f]+)',lambda m:m[1]+'0x%X'%graphics_rva(int(m[2],16)),content)
    # Conditional graphics-return array in the corner classifier.
    if name=='scroller_corner.hpp':
        content=replace_hex(content,{v:graphics_rva(v) for v in (0xAC84F,0xAF1DB,0xB46BF,0xB3121)})
    if name in ('scroller_corner.hpp','scroller_png.hpp','thumb_paint.hpp'): content=content.replace('0xABEDB0','0xABF108')
    if name=='dock_splitters.hpp': content=content.replace('limit - 0xB2FC08','limit - 0xB2FE10')
    write(name,content.replace('8.3.27.2325','8.3.27.2130'))
platform=upstream('platform.hpp')
for name,file in [('exeHash','1cv8.exe'),('frameHash','frame.dll'),('graphicsHash','grphcs.dll'),('wbaseHash','wbase83.dll'),('cairoHash','cairo.dll')]:
    platform=re.sub(r'('+name+r' =\s*L")[A-F0-9]+',lambda m:m[1]+digest(file),platform)
platform=platform.replace('inline std::wstring modulePath',f'inline constexpr auto thinClientHash = L"{digest("1cv8c.exe")}";\n\ninline std::wstring modulePath')
write('platform.hpp',platform)
# The exact .2130 Cairo export is independently resolved in the PE export table.
im=images['cairo.dll'];pe=im.read('I',0x3C);export=im.read('I',pe+24+112);off=im.offset(export)
count=im.read('I',off+24);functions,names,ordinals=im.read('III',off+28)
pattern_rva=None
for i in range(count):
    name=im.name_at(im.read('I',im.offset(names)+i*4))
    if name=='cairo_pattern_create_for_surface':
        index=im.read('H',im.offset(ordinals)+i*2);pattern_rva=im.read('I',im.offset(functions)+index*4)
assert pattern_rva
hook=upstream('scroller_png_hook.hpp')
hook=re.sub(r'originalRva = 0x[0-9A-F]+',f'originalRva = 0x{pattern_rva:X}',hook)
write('scroller_png_hook.hpp',hook)
# Global search: target retains all 57 interface method RVAs, verified before
# admitting its exact on-disk and mapped-image hashes.
dev = images['devtool.dll']
binding = upstream('devtool_binding.hpp')
methods = binding.split('globalSearchMethods{',1)[1].split('};',1)[0]
methods = [int(value,16) for value in re.findall(r'0x([0-9A-F]+)',methods)]
assert len(methods) == 57
for index, method in enumerate(methods):
    assert dev.read('Q',dev.offset(0x1A12E8 + index*8)) == dev.image_base + method
pe = dev.read('I',0x3C)
assert dev.read('I',pe+24+56)==0x261000
assert dev.read('I',pe+24+dev.read('H',pe+20)+8)==0x195A48
for name,raw in [('expectedDigest',bytes.fromhex(digest('devtool.dll'))),
 ('headerDigest',hashlib.sha256(dev.data[:0x400]).digest()),
 ('textDigest',hashlib.sha256(data('devtool.dll',0x1000,0x195A48)).digest())]:
    body=', '.join('0x%02X'%value for value in raw)
    binding=re.sub(r'('+name+r'\{)[^}]+',lambda match:match[1]+'\n    '+body+'\n',binding)
binding=binding.replace('0x128 + offsetof', '0x%X + offsetof'%pe)
write('devtool_binding.hpp',binding)
checks.append({'module':'devtool.dll','vtable_run':'0x1A12E8','method_count':57,'pe_offset':hex(pe)})
# Live-owned test observations are backed by independent target constructors:
# tree context, metadata-search provider, rows=provider+18h.
FRONTEND_DATA={0xEB8850:0xD62048,0xEB88F8:0xD61F70,0xEEC598:0xD95480}
for site,table in [(0x283F57,0xD62048),(0x283F78,0xD61F70),(0x65B42F,0xD95480)]:
    raw=data('frntend.dll',site,16)
    assert raw[:3]==bytes.fromhex('488d05') and site+7+struct.unpack_from('<i',raw,3)[0]==table
    checks.append({'module':'frntend.dll','constructor_lea':hex(site),'table':hex(table),'bytes':raw.hex()})
identity=replace_hex(upstream('list_identity.hpp'),FRONTEND_DATA)
identity=identity.replace('globalSearchSiteRva = 0xB20900','globalSearchSiteRva = 0xB213A8')
assert images['frame.dll'].read('Q',images['frame.dll'].offset(0xB213A8+0x18)) == images['frame.dll'].image_base+0x8A7A50
checks.append({'module':'frame.dll','global_search_site':'0xB213A8','table':'0xB213A8','slot':'0x18','method':'0x8A7A50','live_provider':'devtool+1A1308'})
identity=re.sub(r'(listFrontendHash =\s*L")[A-F0-9]+',lambda match:match[1]+digest('frntend.dll'),identity)
write('list_identity.hpp',identity.replace('8.3.27.2325','8.3.27.2130'))
# frontend and devtool identity port remains separate: hash changes without the
# matching tables must never be treated as proof of compatibility.
evidence=Path(os.environ['LOCALAPPDATA'])/'1CDarkTheme/compat-evidence'
evidence.mkdir(parents=True,exist_ok=True)
(evidence/'native-2130.json').write_text(json.dumps({'upstream':UPSTREAM,'module_hashes':{n:digest(n) for n in images},'checks':checks,'frame_data_mapping':{hex(k):hex(v) for k,v in FRAME_DATA.items()},'frontend_data_mapping':{hex(k):hex(v) for k,v in FRONTEND_DATA.items()},'cairo_pattern_create_rva':hex(pattern_rva)},indent=2),encoding='utf-8')
print(f'{len(checks)} native instruction/vtable assertions passed; evidence: {evidence / "native-2130.json"}')
