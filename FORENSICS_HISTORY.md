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
- **Purpose**: Track adds/removals/metadata changes per updatedb run for consumption by `showdiff`.
- **Storage**:
  - Append-only log after metadata stream: entries `struct { uint32_t docid; uint8_t change; dir_time timestamp; uint64_t prev_meta_offset; uint64_t new_meta_offset; }`.
  - Change kinds: 0=added, 1=deleted, 2=modified (metadata delta), 3=hash-mismatch.
  - Log compressed with Zstd in 4 KiB chunks, similar to metadata.
  - Header already reserves `history_offset/length`.
- **Retention**:
  - CLI flag `--history-depth=<n>` controlling number of logs (rolling window). Depth=0 disables history, default 0.
  - Each updatedb run writes a new log chunk tagged with run timestamp; optional compaction merges older entries.
  - External `showdiff` reads baseline metadata + history logs to compute deltas between timestamps.

## showdiff Consumption
- Stream metadata sequentially using the new reader in `ExistingDB`.
- Build `showdiff <old.db> <new.db>` to walk both DBs in lockstep; hashes provide fast equality checks, history log accelerates incremental comparisons.
- Future extension: `showdiff --with-live /path/db` to compare DB metadata against `stat()` calls for drift detection.
