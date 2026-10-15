#!/usr/bin/env python3
"""Compare every candidate word with original Apple ARM64 instructions.

Requires existing clang/lld and qemu-aarch64-static. No hardware access.
The oracle's operands come from a separate fixture bit parser and explicitly
specified synthetic host resource records, never from the candidate builder.
"""
from pathlib import Path
import argparse, hashlib, json, re, struct, subprocess, sys
from fixture_bits import parse

HERE = Path(__file__).resolve().parent
OLD_ROOT = Path('/home/zz8011/Documents/Codex/2026-10-13/task-6')
ap = argparse.ArgumentParser()
ap.add_argument('--candidate', type=Path, default=HERE.parent)
ap.add_argument('--out', type=Path, default=HERE.parent/'build/oracle')
ap.add_argument('--kernelcache', type=Path, default=OLD_ROOT/'evidence/a1599-usb-reenum-20261013/firmware-12.5.8-16H88/kernelcache.release.ipad4bm.decompressed')
ap.add_argument('--fixture', type=Path, default=OLD_ROOT/'evidence/a1599-init-experiment-20261014/fixtures/red32x32-idr.h264')
a = ap.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
def sha(b): return hashlib.sha256(b).hexdigest()
assert sha(a.kernelcache.read_bytes()) == '76269e79b4715783b95407acbb54df2369116c76709a03f24e0fe95924199eb9'
fixture = a.fixture.read_bytes()
assert sha(fixture) == '839967e0bd3c887029c3a8517f3f5ed0aea57b0a567ac9ac700a85f92458af31'
s,p,t,frameonly,mbaff,field,initial,delta,db,alpha,beta,stype = parse(fixture)
def fields(v): return {x['name']:x['value'] for x in v.fields}
sf,pf,tf = fields(s),fields(p),fields(t)
inputs = bytearray(0x620)
struct.pack_into('<HH', inputs, 0, sf['width_mbs_minus1'], sf['height_map_units_minus1'])
# P+6: original bridge 869304c..8693344. bit11 is host producer mode,
# explicitly set by Apple; baseline chroma_format_idc defaults to 1.
p6 = field | (mbaff << 1) | (1 << 4) | (bool(tf['nal_header'] & 0x60) << 6)
p6 |= (pf['constrained_intra'] << 7) | (pf['weighted_pred_flag'] << 8)
p6 |= (pf['weighted_bipred_idc'] << 9) | (1 << 11) | (frameonly << 12)
struct.pack_into('<H', inputs, 6, p6)
inputs[0xa8] = initial & 255
inputs[0xd8] = sf['direct_8x8_inference_flag']
inputs[0x216] = stype % 5
# Original bridge 8692f08..8692f0c supplies both unweighted denominators=5.
inputs[0x217:0x219] = bytes([5,5])
inputs[0x21b:0x21d] = bytes([alpha & 255,beta & 255])
inputs[0x55f] = delta & 255; inputs[0x563] = db
nal = re.split(b'\x00\x00\x00?\x01', fixture)[-1]
struct.pack_into('<I', inputs, 0x20c, len(nal))
struct.pack_into('<H', inputs, 0x214, t.p)
resources = [0x50000000,0x51000000,0x52000024,0x53000000,0x54000000,0x55000000,0x56000000,0]
struct.pack_into('<8I', inputs, 0x600, *resources)
(a.out/'fixture-fields.json').write_text(json.dumps({'sps':s.fields,'pps':p.fields,'slice':t.fields},indent=2)+'\n')

def run(cmd): return subprocess.run(list(map(str,cmd)),check=True,capture_output=True,timeout=30)
oracle = a.out/'apple_emit_oracle'
run(['clang','--target=aarch64-linux-gnu','-nostdlib','-static','-fuse-ld=lld','-Wl,-Ttext=0x1000000','-fno-builtin','-fno-stack-protector','-O1',HERE/'apple_emit_oracle.c','-o',oracle])
def replay(name, data):
    path = a.out/(name+'.input'); path.write_bytes(data)
    r=run(['qemu-aarch64-static',oracle,a.kernelcache,path])
    (a.out/(name+'.bin')).write_bytes(r.stdout); (a.out/(name+'.log')).write_bytes(r.stderr)
    assert len(r.stdout)==544, (name,len(r.stdout))
    state={k:int(v,16) for k,v in re.findall(r'(C534|C538|B28)=([0-9a-f]+)',r.stderr.decode())}
    assert state['C538']==state['B28'],state
    return r.stdout,state
expected,state = replay('progressive', inputs)
assert state == {'C534':2,'C538':1,'B28':1}
# Five raw branch paths, including both P4 polarities in progressive mode.
for name,low,p4,mode,meta in [('progressive_p4_high',p6,128,2,1),('mbaff',p6|2,0,3,1),('field_top',p6|1,0,0,2),('field_bottom',p6|1,128,1,3)]:
    b=bytearray(inputs);struct.pack_into('<H',b,6,low);b[4]=p4
    _,state=replay(name,b)
    assert state=={'C534':mode,'C538':meta,'B28':meta},(name,state)
# The current host record's previous mode is not a first-I-slice command input.
b=bytearray(inputs);struct.pack_into('<I',b,0x61c,3)
assert replay('previous_B28_3',b)[0] == expected
# QP is independent of H+4; perturb fixture-derived delta and execute raw code.
b=bytearray(inputs);b[0x55f]=(delta+1)&255
qp,_=replay('qp_plus_one',b)
qw=struct.unpack('<136I',qp);ew=struct.unpack('<136I',expected)
assert [i for i in range(136) if qw[i]!=ew[i]]==[51,61]
assert all(qw[i]-ew[i]==1<<10 for i in (51,61))
host=a.out/'test_decode_stream'
run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',a.candidate/'host-regressions/test_decode_stream.c',a.candidate/'a1599_fw_decode_stream.c','-o',host])
run([host,a.out/'candidate.bin'])
candidate=(a.out/'candidate.bin').read_bytes()
assert len(candidate)==len(expected)
cw=struct.unpack('<136I',candidate)
diff=[{'word':i,'byte_offset':hex(i*4),'candidate':f'{cw[i]:08x}','apple':f'{ew[i]:08x}'} for i in range(136) if cw[i]!=ew[i]]
(a.out/'comparison.json').write_text(json.dumps({'compared_words':136,'differences':diff,'apple_sha256':sha(expected),'candidate_sha256':sha(candidate)},indent=2)+'\n')
(a.out/'all-136-words.tsv').write_text('word\tbyte_offset\tcandidate\tapple\tequal\n'+''.join(f'{i}\t0x{i*4:03x}\t{cw[i]:08x}\t{ew[i]:08x}\t{cw[i]==ew[i]}\n' for i in range(136)))
print('Raw producer replay: 5 picture paths, metadata store, B28 carryover, QP perturbation: PASS')
print(json.dumps({'compared_words':136,'differences':diff},indent=2))
if diff: sys.exit(1)
print('PASS: all 136 candidate words equal the independent original-instruction oracle.')
actual=bytearray(inputs)
struct.pack_into('<8I',actual,0x600,0x40012000,0x40013000,0x40014000,0x40015000,0x40019000,0x40029000,0x4002a000,0)
ae,_=replay('actual-layout-apple',actual)
dump=a.out/'dump_actual_layout'
run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',HERE/'dump_actual_layout.c',a.candidate/'a1599_fw_decode_stream.c','-o',dump])
ac=run([dump]).stdout
(a.out/'actual-layout-candidate.bin').write_bytes(ac)
assert ac==ae
print('PASS: actual candidate resource layout, all 544 bytes equal original producer; SHA256='+sha(ac))
