# APFS bring-up status

This directory currently contains a **read-only host-side APFS parser prototype**.
It is not yet an XNU VFS driver and is not enabled in the S10+ boot path.

## Implemented and host-tested

- NXSB container probe and Fletcher-64 validation.
- Checkpoint descriptor discovery and checkpoint-map OID resolution.
- Fixed-KV container/volume OMAP decoding and bounded OID lookup.
- APSB volume-superblock decoding, including live and snapshot forms.
- Volume OMAP resolution of the virtual FSTREE root.
- Bounded variable-KV FSTREE traversal for an exact inode key.
- Decoding of the fixed 92-byte inode value prefix.
- Bounds checks, checksum checks, cycle/depth guards and synthetic ASan/UBSan tests.

## Not implemented

- Full directory-record iteration and Unicode-aware name lookup.
- Complete inode extended-field, dstream, xattr and file-extent decoding.
- File data reads, compression and clone handling.
- APFS encryption/keybag support, sealed-volume integrity verification,
  Fusion support and complete snapshot enumeration.
- Full APFS VFS integration in XNU, mount lifecycle, vnode operations,
  cache/coherency handling, and boot-time device integration.
- Validation against a real iOS APFS image and on-device testing.

The parser deliberately fails closed on unsupported or malformed structures.
Synthetic tests validate parser logic only; they do not establish compatibility
with a particular iOS build or target device.

## Host verification

From the repository root:

```sh
bash scripts/test-apfs-probe.sh
sh scripts/test-apfs-checkpoint.sh
git status --short
```

The checkpoint test script is compiled with AddressSanitizer and
UndefinedBehaviorSanitizer when supported by the host compiler.

**Do not flash an APFS-enabled kernel based on this prototype.** The parser is
not wired into XNU's VFS and has not been validated against a real device image.
Keep the existing HFS+ USERDATA unchanged until a separately reviewed migration
and rollback plan exists.
