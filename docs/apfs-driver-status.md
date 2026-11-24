# APFS driver bring-up (B2L)

## Safety boundary

This branch contains a read-only host parser, not a mountable filesystem.
Do not format, migrate, or boot the device's USERDATA with it. TWRP only needs
to flash artifacts; the eventual XNU filesystem provider must understand APFS.

## Implemented parser layers

- Bounded NXSB probe with APFS Fletcher-64 complemented checksum validation.
- Checkpoint descriptor-ring discovery for contiguous and fragmented layouts;
  fragmented layouts use a bounded fixed-KV B-tree lookup of physical ranges.
- Checkpoint-map validation and multi-block chain aggregation, including
  object type, checksum, XID, LAST flag, entry count and container range checks.
- Newest internally consistent NXSB selection, with matching container UUID,
  block geometry and XID; extracts container OMAP physical address,
  spaceman/reaper OIDs and volume OIDs.
- Checkpoint-map chain lookup to resolve container metadata OIDs such as
  spaceman and reaper into physical block ranges.
- Container OMAP object, key and value decoding.
- Fixed-KV OMAP B-tree lookup with XID-aware branch selection, deleted-value
  reporting, key ordering, checksum/offset/range validation, cycle detection
  and a 32-node depth limit.
- End-to-end container OMAP object -> B-tree -> virtual OID resolution helper.
- Synthetic fixtures for contiguous/fragmented descriptor layouts,
  checkpoint-map validation and lookup, OMAP leaf/branch traversal, and
  container OMAP resolution.
- ASan/UBSan host test scripts are wired into a separate Linux CI job.

## Deliberate limits

This is host-side parser code and is not linked into XNU. B-tree traversal
currently accepts fixed-KV nodes only. Variable-KV trees, snapshots, encrypted
objects and unknown feature combinations are not supported. Fragmented
checkpoint-area and OMAP tree roots are expected as physical block addresses;
the exact address semantics must still be confirmed against independent real
APFS images. No device mount, APFS write path or USERDATA migration has been
attempted. CI has not yet completed for the current branch head.

## Next work

1. Validate against independent APFS images and a real iOS 26.6.2 container.
2. Add variable-KV node support and a stricter unsupported-feature policy.
3. Resolve all container metadata and volume superblocks, then each volume OMAP.
4. Implement filesystem B-tree traversal and file data extents.
5. Build a read-only XNU VFS provider and validate against generated APFS images.
6. Implement iOS metadata semantics and app-install validation.
7. Implement writable APFS only after COW, checkpoint publication, flush
   ordering and crash-recovery tests.

## References

- Apple File System Reference:
  https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf
- libfsapfs format research:
  https://github.com/libyal/libfsapfs/blob/main/documentation/Apple%20File%20System%20(APFS).asciidoc
