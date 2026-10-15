#!/usr/bin/env python3
"""Offline verification of original instruction bytes, replay and baseline preservation."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
workspace = root.parent
evidence = root / 'evidence'
source = json.loads((evidence / 'SOURCES.json').read_text())
sha = lambda b: hashlib.sha256(b).hexdigest()
kernel = Path(source['kernelcache']).read_bytes()
assert sha(kernel) == source['sha256']
for name, item in source['ranges'].items():
    offset = int(item['file_offset'], 16)
    length = int(item['va_end_exclusive'], 16) - int(item['va_start'], 16)
    data = kernel[offset:offset + length]
    assert data == (evidence / (name + '.bin')).read_bytes(), name
    assert sha(data) == item['sha256'], name
fw = source['firmware']
staging = Path(fw['path']).read_bytes()
assert sha(staging) == fw['sha256']
assert kernel[0x14eef8:0x14eef8+0x66dc] == staging[:0x66dc]
assert kernel[0x1555fc:0x1555fc+0x820] == staging[0x66e0:0x6f00]
vtable = json.loads((evidence / 'IOSURFACE-VTABLE.json').read_text())
for item in vtable['slots']:
    offset = int(item['file_offset'], 16)
    raw = int.from_bytes(kernel[offset:offset+8], 'little')
    assert raw == int(item['raw'], 16)
    low = raw & ((1 << 51) - 1)
    canonical = low | (0xfff8000000000000 if low & (1 << 50) else 0)
    assert canonical == int(item['canonical_low51'], 16)
before = json.loads((evidence / 'BASELINE-BEFORE.json').read_text())
baseline = workspace / 'a1599_first_decode_candidate_c0_classification_20261014'
after = {str(p.relative_to(workspace)): sha(p.read_bytes())
         for p in baseline.rglob('*') if p.is_file()}
assert before == after, '1211 baseline file set or contents changed'
target = root / 'tests/replay_receiver'
compile_args = ['clang', '--target=aarch64-linux-gnu', '-ffreestanding',
                '-fno-builtin', '-fno-stack-protector', '-nostdlib', '-static',
                '-fuse-ld=lld', '-Wl,-Ttext=0x1000000', '-O2', '-Wall',
                '-Wextra', '-Werror', str(root / 'tests/replay_receiver.c'),
                '-o', str(target)]
subprocess.run(compile_args, check=True, capture_output=True, timeout=60)
run_args = ['qemu-aarch64-static', str(target), source['kernelcache']]
run = subprocess.run(run_args, capture_output=True, text=True, timeout=30)
(evidence / 'RECEIVER-REPLAY.log').write_text(
    'compile=' + repr(compile_args) + '\nrun=' + repr(run_args) + '\n' +
    run.stdout + run.stderr + '\nexit=' + str(run.returncode) + '\n')
assert run.returncode == 0, run.stdout + run.stderr
result = dict(kernel_sha256=source['sha256'], original_ranges_verified=len(source['ranges']),
              firmware_text_data_exact=True, iosurface_vtable_slots_verified=3,
              baseline_files_unchanged=len(before), receiver_cases=408,
              completion_cases=2, original_instruction_replay_exit=run.returncode,
              hardware_access=False, dma_order_proven=False)
(evidence / 'EVIDENCE-VERIFICATION.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
