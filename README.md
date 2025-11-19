# Flocate - [F]orensic plocate

[Deutsch](README_de.md)

## Overview

This repository extends [plocate](https://plocate.sesse.net/) with forensic tools. `flocate` originated from the original [plocate](https://plocate.sesse.net/) project; the core ideas and most of the implementation have been preserved thanks to this upstream project. In addition to the original fast plocate implementation, flocate's updatedb can now collect metadata per file (mode, ownership, timestamp, size, and optional hashes) and store history logs per run. 

With the companion tool flocate-showdiff, you can play back this history, compare two databases, or compare a database with a live file system tree.

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
- Restructured file structure

## Building and Installing

Dependencies: a C++17 compiler, Meson ≥ 0.61, Ninja, libzstd, and optional
liburing.

Prepare Dependecies for Debian GNU/Linux based Systems (ubuntu, kali, raspberryOS, mint,...)

```sh
# install meson and ninja
sudo apt install meson ninja-build cmake cmake-data pkg-config libzstd-dev liburing-dev git
```

Cloning Repository
```sh
mkdir ~/dev/
cd ~/dev/
git clone https://github.com/rtulke/flocate.git
```

Compile and Build flocate (all Linux Distrisbutions)
```sh
# jump into the cloned local repository
cd ~/dev/flocate

# configure once (pass MESON_ARGS="--prefix=/opt/flocate" if needed)
make config

# build
make

# install systemwide
sudo groupadd flocate
sudo ninja -C build install
sudo make install

# uninstall 
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
| :---         | :---                          | :---        |
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
| :---         | :---                          | :---        |
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
| :---         | :---                          | :---        |
| `-b`         | `--block-size SIZE`           | Compress `SIZE` filenames per posting-list block (default 32). |
| `-p`         | `--plaintext`                 | Treat the input as newline-delimited plain text instead of an mlocate DB. |
| `-l`         | `--require-visibility FLAG`   | Set the “require visibility” flag in the generated database. |
|              | `--help`                      | Show usage information. |
| `-V`         | `--version`                   | Print version/license information. |

### `flocate-showdiff`

| Option short | Option long                   | Description |
| :---         | :---                          | :---        |
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

## Troubleshooting

### System-wide installation (updatedb conflicts with locate, mlocate or plocate)

```sh
sudo dpkg -l |grep locate

ii  locate                         4.9.0-4                                          amd64        maintain and query an index of a directory tree
ii  mlocate                        1.1.18-1                                         all          transitional dummy package
ii  plocate                        1.1.18-1                                         amd64        much faster locate
```
or

```sh
ls -lat /usr/bin/locate
root 24 28. Sep 2019  /usr/bin/locate -> /etc/alternatives/locate

ls -lat /etc/alternatives/locate
lrwxrwxrwx 1 root root 16 22. Nov 2023  /etc/alternatives/locate -> /usr/bin/plocate

ls -lat /usr/bin/plocate
-rwxr-sr-x 1 root plocate 314760 28. Jan 2023  /usr/bin/plocate
```

and

```sh
ls -lat /usr/bin/updatedb
lrwxrwxrwx 1 root root 26 28. Sep 2019  /usr/bin/updatedb -> /etc/alternatives/updatedb

ls -lat /etc/alternatives/updatedb
lrwxrwxrwx 1 root root 26 22. Nov 2023  /etc/alternatives/updatedb -> /usr/sbin/updatedb.plocate

ls -lat /usr/sbin/updatedb.plocate
-rwxr-xr-x 1 root root 114096 28. Jan 2023  /usr/sbin/updatedb.plocate
```

Means this is a symlink 
- for locate from `/usr/bin/locate` to `/etc/alternatives/locate` to `/usr/bin/plocate`
- for updatedb from `/usr/bin/updatedb` to `/etc/alternatives/updatedb` to `/usr/sbin/updatedb.plocate`

Ensure that no other search tools are installed, as this may cause problems with updatedb, since our flocate is installed in `/usr/local/bin` and updatedb comes in `/usr/local/sbin`.

At your own risk you can try creating a symlink as follows:

for updatedb

```sh
ln -fs /usr/local/sbin/updatedb /usr/bin/updatedb
```

for flocate 

```sh
ln -fs /usr/local/bin/floacte /usr/bin/locate
```

Otherwise, if plocate, mlocate, or locate are not needed, remove the installed locate tool. 

```sh
apt remove locate      # or plocate, mlocate
```

## References

- Refer to the installed man pages for the canonical CLI reference:
  `flocate(1)`, `updatedb(8)`, `flocate-build(8)`, and `flocate-showdiff(1)`.
