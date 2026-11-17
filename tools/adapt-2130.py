from pathlib import Path
import re,hashlib,importlib.util,sys
sys.dont_write_bytecode = True
root=Path(__file__).resolve().parents[1]
bin=Path(r'C:\Program Files\1cv8\8.3.27.2130\bin')
spec=importlib.util.spec_from_file_location('compare',root/'compare-platform.py');cmp=importlib.util.module_from_spec(spec);spec.loader.exec_module(cmp)
frame=cmp.Image(bin/'frame.dll'); graphics=cmp.Image(bin/'grphcs.dll')
# Confirm PE architecture and each instruction before producing the pinned build.
sites=[(frame,0x303424,'448bc6'),(graphics,0xAE57E,'0f45d0'),(graphics,0x22D7,'418bc0'),(frame,0x9518BC,'448bc0'),(frame,0x5D4AEE,'8bd8'),(frame,0x5D40D4,'448bc0'),(graphics,0xAE4D3,'8bd7')]
for im,rva,expected in sites:
 o=im.offset(rva);assert im.data[o:o+len(bytes.fromhex(expected))]==bytes.fromhex(expected),(im.path,rva)
# All frame paint call sites used by this build retain their layout (-0x30).
# Early frame code remains at the same RVA. Graphics paint code moved by -0x8D0.
import subprocess
t=subprocess.check_output(['git','show','46f590b:theme.cpp'],cwd=root).decode('utf-8')
def frame_ref(m):
 x=int(m[1],16);return 'frameBase + 0x%X'%(x-0x30 if 0x200000<x<0xA00000 else x)
def graphics_ref(m):
 x=int(m[1],16);return 'graphicsBase + 0x%X'%(x-0x8D0 if x>0x70000 else x)
t=re.sub(r'frameBase \+ (0x[0-9A-F]+)',frame_ref,t)
t=re.sub(r'graphicsBase \+ (0x[0-9A-F]+)',graphics_ref,t)
for name,old,new in [('treeTabVtable',0xAD3D38,0xAD36D8),('windowTabPaintReturn',0x8EFC3A,0x8EFC0A),('windowPanelPaintReturn',0x8EF5DD,0x8EF5AD),('propertyPaintReturn',0x4F60ED,0x4F60BD),('propertyLinkVtable',0xACD3B8,0xACCD58),('sharedListPaintReturn',0x8117F5,0x8117C5)]:
 t=t.replace(f'{name} = 0x{old:X}',f'{name} = 0x{new:X}')
t=t.replace('(shortcut ? 0x73499F : 0x73480F)','(shortcut ? 0x73496F : 0x7347DF)')
(root/'theme.cpp').write_text(t,'utf-8')
p=(root/'platform.hpp').read_text('utf-8')
for name,file in [('exeHash','1cv8.exe'),('frameHash','frame.dll'),('graphicsHash','grphcs.dll')]:
 h=hashlib.sha256((bin/file).read_bytes()).hexdigest().upper();p=re.sub(r'('+name+r' =\s*L")[A-F0-9]+',r'\g<1>'+h,p)
(root/'platform.hpp').write_text(p,'utf-8')
print('7 hook instruction checks passed; exact local binary hashes pinned.')
