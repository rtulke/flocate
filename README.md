# Plocate Forensics Extensions

[Deutsch](README_de.md)

## Overview

This tree extends [plocate](https://plocate.sesse.net/) with forensic
instrumentation. In addition to the original fast locate(1) implementation,
`updatedb` can now capture per-file metadata (mode, ownership, timestamps, size
and optional hashes) and persist per-run history logs. The companion tool
`plocate-showdiff` lets you replay that history, diff two databases, or compare a
database against a live filesystem tree.

## Key Features

- Optional hashing via `--metadata-hash=xxh64|sha256` (or `METADATA_HASH` in
  `/etc/updatedb.conf`) without compromising the main locate workflow.
- Retain the latest `N` runs in the database by setting
  `--history-depth=N`/`HISTORY_DEPTH`. Each run is tagged with a marker so it
  can be trimmed or replayed precisely.
- `plocate-showdiff` modes:
  - `plocate-showdiff --history DB`: print the recorded add/remove/modify events
    for the newest runs.
  - `plocate-showdiff OLD_DB NEW_DB`: diff two snapshots.
  - `plocate-showdiff --live /mnt/root DB`: compare on-disk files (under the
    specified root) against a database, recomputing metadata and hashes on the fly.
- Everything ships with groff man pages (`plocate(1)`, `updatedb(8)`,
  `plocate-build(8)`, `plocate-showdiff(1)`).

## Building and Installing

Dependencies: a C++17 compiler, Meson ≥ 0.61, Ninja, libzstd, and optional
liburing.

```sh
# configure once (pass MESON_ARGS="--prefix=/opt/plocate" if needed)
make config

# build
make

# run the test suite
make test

# install / uninstall
sudo make install
sudo make uninstall
```

All targets wrap Meson, so you can reconfigure with `make config` whenever you
adjust `MESON_ARGS`.

## updatedb Highlights

- Use `updatedb --metadata-hash=sha256` to hash regular files before serialising
  their metadata.
- Use `updatedb --history-depth=3` (or set `HISTORY_DEPTH="3"` in
  `/etc/updatedb.conf`) to keep the newest three runs inside the database.
- Configuration snippets in the database header ensure incompatible changes
  trigger an automatic rebuild.

## plocate-showdiff Usage

```
# Inspect the built-in history log
plocate-showdiff --history /var/lib/plocate/plocate.db

# Compare two snapshots
plocate-showdiff /tmp/old.db /tmp/new.db

# Compare a database against a live tree (e.g., mounted backup)
plocate-showdiff --live /mnt/backup /var/lib/plocate/plocate.db
```

Outputs list the affected paths as well as old/new metadata (including hashes
when available).

## Configuration and Documentation

- `/etc/updatedb.conf` understands the new `METADATA_HASH` and `HISTORY_DEPTH`
  variables in addition to the classic pruning knobs.
- Refer to the installed man pages for the canonical CLI reference:
  `plocate(1)`, `updatedb(8)`, `plocate-build(8)`, and `plocate-showdiff(1)`.

## Contributing

See `AGENTS.md` for repository guidelines and `FORENSICS_PROGRESS.md` /
`FORENSICS_HISTORY.md` for the long-term roadmap. Patches and issue reports are
very welcome!
