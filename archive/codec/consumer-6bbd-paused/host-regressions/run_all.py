#!/usr/bin/env python3
"""Host-only regression runner; builds/runs no kernel or device operations."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build/host-regressions'
out.mkdir(parents=True, exist_ok=True)
commands = []
for source in sorted((root / 'host-regressions').glob('test_*.c')):
    target = out / source.stem
    extra = []
    if source.stem in ('test_c0_ack', 'test_decode_diagnostics', 'test_decode_submit_mock'):
        extra.append(root.parent / 'id81_host_contract/id81_contract.c')
    if source.stem == 'test_decode_stream':
        extra.append(root / 'a1599_fw_decode_stream.c')
    commands += [['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                  source, *extra, '-o', target], [target]]
commands += [[sys.executable, root / 'host-regressions/check_contract.py']]
commands += [[sys.executable, p] for p in sorted((root / 'host-regressions').glob('test_*.py'))]
failed = []
with (root / 'build/HOST-REGRESSIONS.log').open('w') as log:
    for args in commands:
        argv = list(map(str, args))
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        log.write(f'$ {argv!r}\nexit={proc.returncode}\n{proc.stdout}{proc.stderr}\n')
        log.flush()
        if proc.returncode:
            failed.append(argv)
            print('FAIL', argv, proc.stdout, proc.stderr)
        else:
            print('PASS', Path(argv[0]).name, Path(argv[-1]).name)
print(f'{len(commands)} commands; {len(failed)} failed')
sys.exit(bool(failed))
