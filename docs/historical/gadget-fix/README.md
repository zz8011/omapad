# A1599 bootdiag-v2 gadget link fix

This package is prepared for review only. It has not been uploaded or booted on the iPad.

The user-provided photo from the prior bootdiag-v2 run shows Alpine reaching `sysinit complete` on the A7 hardware, after the gadget function link failed. That proves the older bootdiag-v2 image completed sysinit on the device; it does not verify this newly prepared gadget-fix package on hardware.

## Finding

The bootdiag-v2 `/sbin/setup-gadget` loop created `acm.usb0`, then ran:

```sh
ln -s "../../functions/$fn" "$cfg/configs/c.1/$fn"
```

PID 1 and its sysinit shell run with current directory `/`; the script does not `cd` to the gadget directory. Configfs resolves the symlink source relative to the calling process's current directory. Thus the old source resolves as `/functions/acm.usb0`, which is absent. In isolated QEMU using the target kernel's configfs and ACM implementation, BusyBox `ln` returned 1 and printed `No such file or directory` (Linux `ENOENT`, errno 2). The same configfs accepted the absolute target `$cfg/functions/acm.usb0`.

The observed fallback was skipped because the failed link called `die function-link ...`, and `die` logs then exits. The shell-only fallback was only in the later `bind_udc` failure branch, which was never reached.

## Change

Only `sbin/setup-gadget` changes in the cpio archive. Link sources now use the absolute configfs function path. Link-creation failure shares the shell-only recovery path with a three-function UDC bind failure: remove any partial functions/links, create and link one ACM function, bind, and verify `ttyGS0`. The immutable 9,553,530-byte m1n1/kernel/DTB/bootargs prefix is unchanged.

Rebuild with:

```sh
python3 build_gadget_fix.py
```

The builder verifies both input hashes, the bundle boundary and immutable-prefix hash, and that every cpio entry other than `sbin/setup-gadget` remains byte-for-byte and metadata-identical.

## Offline evidence

- A host-side control-flow regression test first failed against the old script at the `function-link` exit; after the change it passes through cleanup, shell-only bind, and `ttyGS0` verification.
- An isolated QEMU system test booted a temporary copy of the target kernel build with PL011 console and `USB_DUMMY_HCD`; the dummy UDC existed only inside QEMU. The old relative symlink failed with the error above, and the absolute target succeeded.
- With the fixed script, the same QEMU configfs flow created all three ACM functions, bound `dummy_udc.0`, and reached `stage=verify-tty-nodes ok`.
- A second QEMU run injected a one-time failure on the second `ln`. The fixed script logged `trying shell-only ACM fallback`, removed the partial setup, bound one ACM function, and reached `stage=verify-ttyGS0 ok shell-only fallback`.
- These QEMU checks validate Linux configfs and local gadget-function setup. They do not demonstrate A1599 USB enumeration, device-side ACM enumeration, or remote access.

The QEMU test kernel, initramfs, dummy UDC, and all test filesystem changes were isolated in `/tmp`. No USB hardware was touched, no reboot/DFU/exploit/upload was performed, and the source bundle was not modified.

## Prepared artifacts

- Rootfs: `out/a1599-alpine-rootfs-gadget-fix.cpio.gz`
- Bundle: `out/a1599-alpine-m1n1-linux-gadget-fix.bundle`
- Rootfs SHA-256: `cbf49193f14c61acbb379a6964915f997c7ff7865ae9633853f7eba641164a4b` (4,272,268 bytes)
- Bundle SHA-256: `a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074` (13,825,798 bytes)
- Preserved prefix: 9,553,530 bytes, SHA-256 `93ae225e1f25c2cdc0de508ef11471b4acb9e3b32b26b0270b7df378e7e7084c`

## Fixed launcher

`launch/alpine_dfu_wrapper.py` is copied from the existing fail-closed bootdiag-v2 launcher. Its only source changes pin `BUNDLE` to the gadget-fix artifact above and set `BUNDLE_SHA256` to its matching hash. The pre-fix launcher and bootdiag-v2 package remain unchanged; their bundle hash remains `0516c3fe8226c9ac7858d0cc5d8df09941d1a5e86f6c01dd4621b971fa870106`.

An import-only `validate_artifacts()` check read and hash-verified checkra1n, Pongo, pongoterm, and the new bundle successfully. The fixed launcher has not been executed; it will require the expected DFU/Pongo gates before its upload flow.
