# Forensic Hash & History Plan

## Hash Collection Strategy
- **Scope**: Hash regular files (symlinks and special files store size/metadata only). Default algorithm `xxh64` for speed; optional `sha256` via `--metadata-hash=sha256`.
- **Configuration**:
  - `conf_metadata_hash_kind` accepts `none`, `xxh64`, `sha256` from CLI or `METADATA_HASH` in `updatedb.conf`.
  - Hashing disabled by default to preserve current performance.
- **Implementation**:
  - `MetadataHashBuilder` streams either xxHash64 or SHA-256 over file contents read via `openat`.
  - Results live inside `FileMetadata.hash` (8 bytes for xxHash64, 32 bytes for SHA-256) and are serialized alongside the rest of the metadata stream.
  - Failures (permission errors, short reads) clear the hash back to `None` and optionally log via `conf_verbose`.

## History Log Layout
- **Purpose**: Track adds/removals/metadata changes per updatedb run for `showdiff`.
- **Storage**:
  - After metadata stream, we append a Zstd-compressed sequence of events. Each event stores `{uint8_t kind, dir_time event_time, uint32_t path_len, path bytes, encoded old metadata, encoded new metadata}`; metadata uses the same layout as the main stream (enabled flag + POSIX fields + hash).
  - Change kinds: 0 = added, 1 = removed, 2 = modified. Added events carry only the new metadata, removed events carry only the old metadata.
  - Header fields `history_offset_bytes/history_length_bytes` point at the compressed blob.
- **Retention**:
  - Currently we keep a single run's event log. Future work can extend this to a rolling history through multiple DBs or user-configurable retention (`--history-depth` placeholder).
  - External tools can replay logs chronologically to rebuild deltas without rescanning the filesystem.

## showdiff Consumption
- Stream metadata sequentially using the new reader in `ExistingDB`.
- Build `showdiff <old.db> <new.db>` to walk both DBs in lockstep; hashes provide fast equality checks, history log accelerates incremental comparisons.
- Future extension: `showdiff --with-live /path/db` to compare DB metadata against `stat()` calls for drift detection.
