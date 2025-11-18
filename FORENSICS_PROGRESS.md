# Forensics Extension Tracker

## Current Objective
Augment plocate with optional forensic metadata capture, historical change tracking, and a high-speed diff tool (`showdiff`) that compares snapshots or the live filesystem.

## Milestones
1. **Metadata schema & compatibility** (in progress)  
   - Fields per entry: `mode`, `uid`, `gid`, `size`, `mtime`, `ctime`, `atime`, optional `hash_type + hash_value`.  
   - Store metadata in a dedicated stream with varint encoding + Zstd blocks; baseline retention only unless `--history-depth` > 0.  
   - Header additions (tentative `max_version = 3`):  
     - `metadata_length_bytes/metadata_offset_bytes`  
     - `history_length_bytes/history_offset_bytes`  
     - `metadata_flags` bitmask (enablement, hash algo id, record width).  
   - History log concept: append `(docid, change_kind, old_meta_digest, new_meta_digest, timestamp)` entries per run when enabled.
   - Status: FileRecords now flow end-to-end and the header reserves offsets/flags for the future metadata/history blocks; history layout still open.
2. **updatedb enhancements** (completed)  
   - Collect additional metadata during scans, compute hashes when enabled, and pass structured records through `DatabaseReceiver`.  
   - Produce baseline snapshots plus per-run change events.
   - Status: `updatedb` now attaches `mode/uid/gid/size` plus `mtime/ctime/atime` to each `FileRecord`; hashing and change logs still pending.
3. **Database format updates** (in progress)  
   - Extend `EncodingCorpus` to serialize metadata streams and an append-only history log with Zstd compression.  
   - Update `db.h` with offsets/lengths for metadata/history and ensure legacy paths remain untouched when feature is off.
   - Status: Metadata blocks are encoded and written (with `metadata_flags`), history log still to come.
4. **Runtime & diff tooling** (pending)  
   - Implement `showdiff` to compare two DBs or DB vs. live FS, surfacing added/removed/modified entries using metadata.  
   - Add plocate CLI toggles to read/display metadata only on demand.
5. **Docs, tests, benchmarks** (pending)  
   - Document workflows (`updatedb --metadata --history-depth`, `showdiff old.db new.db`).  
   - Add automated tests validating metadata accuracy and diff output; benchmark to confirm negligible impact when metadata disabled.

Progress will be updated here as milestones are completed or refined.
