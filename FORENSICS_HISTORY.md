# Forensic Hash & History Plan

## Hash Collection Strategy
- **Scope**: Hash regular files (symlinks and special files store size/metadata only). Default algorithm `xxHash64` for speed; optional `SHA-256` when `--metadata-hash=sha256` is supplied.
- **Configuration**:
  - `conf_metadata_hash` enum (`none`, `xxh64`, `sha256`).
  - CLI flag `--metadata-hash=<algo>`; `updatedb.conf` entry mirrors it.
  - Hashing disabled by default to preserve current performance.
- **Implementation**:
  - Reuse `MetadataHashKind` in `FileMetadata`.
  - Add helper `compute_file_hash(int dirfd, const string &name, MetadataHashKind algo)` to avoid extra path joins.
  - Hash stored inline in metadata stream (already serialized), truncated to 8 bytes for xxHash64 and 32 bytes for SHA-256.
  - Error handling: fallback to `HashKind::None` with warning; never abort `updatedb`.

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
