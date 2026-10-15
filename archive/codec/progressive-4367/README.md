# A1599 progressive-mode offline candidate

Built successor to frozen M2H candidate c97fb883…fb2. Only three production
constants changed, all in `a1599_fw_decode_stream.c`. This has not been tested
on hardware and does not establish the cause of HW_PANIC.

See [RESULT-20261014.md](RESULT-20261014.md) for source instruction addresses,
complete command comparison, test scope, build command, hashes and limitations.

Module: `build/a1599_fw_candidate.ko`.

SHA-256: `4367df89658bdbcee535129f768180c2fc239baf1eaae87896079a2061687b4c`.

Run the independent offline regression with:

```sh
python host-regressions/test_apple_emitter.py
```

The test reads the original kernelcache and fixture at the documented local
evidence paths, or paths supplied with `--kernelcache` and `--fixture`.
It executes only a bounded original-code producer in QEMU userspace; no device
access or module loading is involved. Shared source dependency is the adjacent
`../id81_host_contract/` directory.
