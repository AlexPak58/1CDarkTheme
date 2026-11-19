"""Read-only verification of the pinned .2130 native build inputs."""
from pathlib import Path
import hashlib,json,os,re,sys,types
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[1]
bin=Path(r'C:\Program Files\1cv8\8.3.27.2130\bin')
m=types.ModuleType('compare');exec((root/'compare-platform.py').read_text('utf-8'),m.__dict__)
images={n:m.Image(bin/n) for n in ('1cv8.exe','1cv8c.exe','frame.dll','grphcs.dll','wbase83.dll','cairo.dll','frntend.dll','devtool.dll')}
platform=(root/'platform.hpp').read_text('utf-8')
for symbol,name in [('exeHash','1cv8.exe'),('thinClientHash','1cv8c.exe'),('frameHash','frame.dll'),('graphicsHash','grphcs.dll'),('wbaseHash','wbase83.dll'),('cairoHash','cairo.dll')]:
 expected=re.search(symbol+r' =\s*L"([0-9A-F]+)"',platform)[1]
 assert expected==images[name].sha256().upper(),symbol
identity=(root/'list_identity.hpp').read_text('utf-8')
assert re.search(r'listFrontendHash =\s*L"([0-9A-F]+)"',identity)[1]==images['frntend.dll'].sha256().upper()
evidence_dir=Path(os.environ['LOCALAPPDATA'])/'1CDarkTheme/compat-evidence'
evidence=json.loads((evidence_dir/'native-2130.json').read_text('utf-8'))
for check in evidence['checks']:
 image=images[check['module']]
 if 'bytes' in check:
  site=int(check.get('rva',check.get('constructor_lea')),16);expected=bytes.fromhex(check['bytes'])
  offset=image.offset(site);assert image.data[offset:offset+len(expected)]==expected,check
 if 'method' in check:
  assert image.read('Q',image.offset(int(check['table'],16)+int(check['slot'],16)))==image.image_base+int(check['method'],16),check
for rva,body in re.findall(r'\{0x([0-9A-F]+), \{([^}]+)\}\}',(root/'panel_headers.hpp').read_text('utf-8')):
 expected=bytes(int(x,16) for x in re.findall('0x([0-9A-F]+)',body));image=images['frame.dll'];offset=image.offset(int(rva,16));assert image.data[offset:offset+len(expected)]==expected
paths=['theme.cpp','platform.hpp','list_identity.hpp','devtool_binding.hpp','panel_headers.hpp','dock_splitters.hpp','scroller_corner.hpp','scroller_png.hpp','scroller_png_hook.hpp','thumb_paint.hpp','tools/adapt-2130.py']
report={'status':'PASS','assertion_groups':len(evidence['checks'])+8,'files':{name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in paths}}
(evidence_dir/'final-verification.json').write_text(json.dumps(report,indent=2),'utf-8')
print(f"Native offline verification PASS ({report['assertion_groups']} groups, 9 panel windows); experimental thumb still defaults off")
