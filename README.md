# Flocate

[Deutsch](README_de.md)

## Overview

This tree extends [flocate](https://flocate.sesse.net/) with forensic
instrumentation. Flocate is derived from the original
[plocate](https://github.com/plocate/plocate) project; the core ideas and much
of the implementation remain thanks to that upstream.
In addition to the original fast locate(1) implementation,
`updatedb` can now capture per-file metadata (mode, ownership, timestamps, size
and optional hashes) and persist per-run history logs. The companion tool
`flocate-showdiff` lets you replay that history, diff two databases, or compare a
database against a live filesystem tree.

## Key Features

- Optional hashing via `--metadata-hash=xxh64|sha256` (or `METADATA_HASH` in
  `/etc/updatedb.conf`) without compromising the main locate workflow.
- Retain the latest `N` runs in the database by setting
  `--history-depth=N`/`HISTORY_DEPTH`. Each run is tagged with a marker so it
  can be trimmed or replayed precisely.
- `flocate-showdiff` modes:
  - `flocate-showdiff --history DB`: print the recorded add/remove/modify events
    for the newest runs.
  - `flocate-showdiff OLD_DB NEW_DB`: diff two snapshots.
  - `flocate-showdiff --live /mnt/root DB`: compare on-disk files (under the
    specified root) against a database, recomputing metadata and hashes on the fly.
- Everything ships with groff man pages (`flocate(1)`, `updatedb(8)`,
  `flocate-build(8)`, `flocate-showdiff(1)`).

## Building and Installing

Dependencies: a C++17 compiler, Meson ≥ 0.61, Ninja, libzstd, and optional
liburing.

For Debian GNU/Linux based Systems

```sh
# install meson and ninja
sudo apt install meson ninja-build cmake cmake-data pkg-config libzstd-dev liburing-dev 
```

```sh
# configure once (pass MESON_ARGS="--prefix=/opt/flocate" if needed)
make config

# build
make

# run the test suite
make test

# install / uninstall
sudo groupadd flocate
sudo ninja -C build install
# sudo make install
# sudo make uninstall
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

## flocate-showdiff Usage

```
# Inspect the built-in history log
flocate-showdiff --history /var/lib/flocate/flocate.db

# Compare two snapshots
flocate-showdiff /tmp/old.db /tmp/new.db

# Compare a database against a live tree (e.g., mounted backup)
flocate-showdiff --live /mnt/backup /var/lib/flocate/flocate.db
```

Outputs list the affected paths as well as old/new metadata (including hashes
when available).

## Command-line Reference

### `flocate`

| Option short | Option long                   | Description |
| :---         | :---                          | :--- |
| `-A`         | `--all`                       | Ignored for mlocate compatibility. |
| `-b`         | `--basename`                  | Match against the filename component only. |
| `-c`         | `--count`                     | Suppress individual paths and print the total at the end. |
| `-d`         | `--database DBPATH`           | Add one or more databases to search (colon-delimited list is accepted). |
| `-e`         | `--existing`                  | Report only entries that still exist at lookup time. |
| `-i`         | `--ignore-case`               | Case-insensitive search (slower and limited Unicode folding). |
| `-l`         | `--limit LIMIT`               | Stop after `LIMIT` matches; `--count` is capped at the same value. |
| `-N`         | `--literal`                   | Output raw paths without shell-style escaping. |
| `-0`         | `--null`                      | Separate matches with NUL instead of newlines. |
| `-r`         | `--regexp`                    | Treat patterns as POSIX basic regular expressions (forces linear scan). |
|              | `--regex`                     | Treat patterns as POSIX extended regular expressions. |
| `-w`         | `--wholename`                 | Match against the full path (default unless `-b` was given earlier). |
|              | `--help`                      | Show usage information. |
| `-V`         | `--version`                   | Print version/license information. |

### `updatedb`

| Option short | Option long                   | Description |
| ---          | ---                           | ---         |
| `-f`         | `--add-prunefs FS`            | Append whitespace-separated filesystems in `FS` to `PRUNEFS`. |
| `-n`         | `--add-prunenames NAMES`      | Append whitespace-separated directory names to `PRUNENAMES`. |
| `-e`         | `--add-prunepaths PATHS`      | Append whitespace-separated paths to `PRUNEPATHS`. |
|              | `--add-single-prunepath PATH` | Append a single path (even with spaces) to `PRUNEPATHS`. |
| `-U`         | `--database-root PATH`        | Restrict scanning to `PATH`. |
|              | `--debug-pruning`             | Emit verbose pruning diagnostics on stderr. |
| `-h`         | `--help`                      | Display usage information. |
| `-o`         | `--output FILE`               | Write the database to `FILE` instead of the default. |
|              | `--prune-bind-mounts FLAG`    | Override `PRUNE_BIND_MOUNTS` (`yes`/`no`). |
|              | `--prunefs FS`                | Override `PRUNEFS` entirely. |
|              | `--prunenames NAMES`          | Override `PRUNENAMES` entirely. |
|              | `--prunepaths PATHS`          | Override `PRUNEPATHS` entirely. |
|              | `--metadata-hash ALGO`        | Hash regular files with `none`, `xxh64`, or `sha256`. |
|              | `--history-depth N`           | Keep metadata/history for the newest `N` runs (0 disables history). |
| `-l`         | `--require-visibility FLAG`   | Toggle permission filtering in the generated database. |
| `-v`         | `--verbose`                   | Print each path as it is discovered. |
| `-V`         | `--version`                   | Print version/license information. |

### `flocate-build`

| Option short | Option long                   | Description |
| ---          | ---                           | ---         |
| `-b`         | `--block-size SIZE`           | Compress `SIZE` filenames per posting-list block (default 32). |
| `-p`         | `--plaintext`                 | Treat the input as newline-delimited plain text instead of an mlocate DB. |
| `-l`         | `--require-visibility FLAG`   | Set the “require visibility” flag in the generated database. |
|              | `--help`                      | Show usage information. |
| `-V`         | `--version`                   | Print version/license information. |

### `flocate-showdiff`

| Option short | Option long                   | Description |
| ---          | ---                           | ---         |
|              | `--history DB`                | Replay the per-run history embedded in `DB`. |
|              | `OLD_DB NEW_DB`               | Positional arguments that trigger snapshot diff mode. |
|              | `--live ROOT DB`              | Compare `DB` against a live filesystem rooted at `ROOT`. |
|              | `--added-only`                | Filter the output to ADDED events. |
|              | `--removed-only`              | Filter the output to REMOVED events. |
|              | `--modified-only`             | Filter the output to MODIFIED events. |
|              | `--help`                      | Show usage information. |
| `-V`         | `--version`                   | Print version/license information. |

## Configuration and Documentation

- `/etc/updatedb.conf` understands the new `METADATA_HASH` and `HISTORY_DEPTH`
  variables in addition to the classic pruning knobs.
- Refer to the installed man pages for the canonical CLI reference:
  `flocate(1)`, `updatedb(8)`, `flocate-build(8)`, and `flocate-showdiff(1)`.
