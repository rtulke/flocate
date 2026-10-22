# Flocate - [F]orensic plocate

[German Version](README_de.md)

## Overview

flocate extends [plocate](https://plocate.sesse.net/) with forensic tooling.
The fast trigram index and the `flocate` query tool are plocate's; on top of
that, flocate's `updatedb` records per-file metadata (mode, owner, size,
mtime/ctime/atime and an optional content hash) and keeps a change history of
the most recent runs inside the database. The companion tool
`flocate-showdiff` replays that history, compares two databases, or compares a
database with a live file system tree such as a mounted image or backup.

## Key features

- Metadata for every indexed entry, with optional hashing via
  `--metadata-hash=xxh64|sha256` (or `METADATA_HASH` in `/etc/updatedb.conf`).
  Digests are stored in canonical form, so they match `xxhsum` and `sha256sum`.
- Cheap daily runs: a file is only re-read and re-hashed when its size, mtime or
  ctime changed; `--rehash-all` forces a complete pass. Pseudo file systems
  (`proc`, `sysfs`, ...) are indexed but never hashed, `--hash-max-size`
  caps the size of files that are.
- Change history of the newest `N` runs (`--history-depth=N` / `HISTORY_DEPTH`),
  each run delimited by a marker. The first run records a baseline instead of
  one event per file.
- Access times are stored but ignored by default (`--compare-atime` /
  `COMPARE_ATIME` turns them on), because on `relatime` mounts every reader
  changes them. Hashing itself never touches the atime (`O_NOATIME`).
- `flocate-showdiff` modes: `--history DB`, `OLD_DB NEW_DB`, `--live ROOT DB`;
  filters, `--json` for scripts, and a `changed:` line naming the fields that
  differ.
- Database records are byte-order independent, so a database can be examined
  on another machine.
- groff man pages for `flocate(1)`, `updatedb(8)`, `updatedb.conf(5)`,
  `flocate-build(8)` and `flocate-showdiff(1)`, and an end-to-end test suite.

## Installing from packages

Prebuilt `.deb` packages for Debian 12/13 and Ubuntu 24.04/26.04, each for
amd64 and arm64, are attached to every
[release](https://github.com/rtulke/flocate/releases); `latest` is a rolling
build of `main`. Take the one for your distribution and architecture:

```sh
sudo apt install ./flocate_*_<debian12|debian13|ubuntu2404|ubuntu2604>_<amd64|arm64>.deb
```

The package replaces locate, mlocate and plocate (apt removes them), creates
the `flocate` group, makes `/usr/bin/flocate` setgid, registers `locate` and
`updatedb` through the alternatives system (the binary itself is
`/usr/sbin/flocate-updatedb`), installs `/etc/updatedb.conf` with the usual
pruning defaults and enables the daily `flocate-updatedb.timer`, which also
runs once right after installation. `sudo apt purge flocate` removes the
database as well.

Building the packages yourself:

```sh
packaging/build-deb-native.sh   # on a Debian/Ubuntu host -> dist/flocate_<version>_<distro>_<arch>.deb
packaging/build-deb.sh          # all four targets, each in its own Docker/Podman container
packaging/test-install.sh       # install and smoke-test each package in a fresh container
```

A plain `dpkg-buildpackage -us -uc -b` in the checkout works too. The version
comes from `meson.build` and must match `debian/changelog`; builds that are not
on the matching `v<version>` tag get a `+git<commit-timestamp>.<commit>` suffix (UTC, to the second, so later commits always sort as newer). The GitHub
workflow `packages.yml` builds and smoke-tests all packages on every push and
publishes them to the Releases page; `tests.yml` runs the test suite.

## Building and installing

Dependencies: a C++17 compiler, Meson ≥ 0.61, Ninja, libzstd and optionally
liburing. flocate is Linux-only.

Debian/Ubuntu and derivatives:

```sh
sudo apt install meson ninja-build cmake cmake-data pkg-config libzstd-dev liburing-dev git
```

When building from source, make sure no other `locate` implementation (locate,
mlocate, plocate) is installed; see [Troubleshooting](#troubleshooting). The
`.deb` package takes care of this itself.

```sh
sudo dpkg -l '*locate*'
sudo apt remove locate   # or plocate, mlocate
```

Clone, build, test and install:

```sh
git clone https://github.com/rtulke/flocate.git
cd flocate

make config          # meson setup build; pass MESON_ARGS="--prefix=/opt/flocate" if needed
make                 # build
make test            # optional: run the end-to-end test suite against the build
sudo groupadd flocate
sudo make install    # or: sudo ninja -C build install
sudo make uninstall  # remove again
```

All targets wrap Meson; rerun `make config` whenever you change `MESON_ARGS`.
Meson options (`meson_options.txt`): `locategroup`, `dbpath`,
`updatedb_progname`, `install_systemd`, `systemunitdir`, `install_cron`.

## Everyday use

### 1. Configure once

Put the forensic settings into `/etc/updatedb.conf` next to the usual pruning
variables. The systemd timer runs `updatedb` without arguments, so this file is
what production runs use.

```sh
PRUNE_BIND_MOUNTS="yes"
PRUNEFS="NFS afs autofs cifs fuse.sshfs proc sysfs tmpfs ..."
PRUNEPATHS="/tmp /var/spool /media /var/tmp"
PRUNENAMES=".git .hg .svn"

METADATA_HASH="sha256"     # none | xxh64 | sha256
HISTORY_DEPTH="14"         # keep two weeks of daily runs; 0 disables the history
COMPARE_ATIME="no"         # "yes" also records atime-only changes
HASH_MAX_SIZE="2G"         # skip hashing files larger than this; 0 = no limit
```

Create the baseline and enable the daily timer:

```sh
sudo updatedb                                 # first run: full scan, recorded as baseline
sudo systemctl enable --now flocate-updatedb.timer
```

### 2. Daily: what changed?

```sh
# Everything the last runs recorded, grouped per run
sudo flocate-showdiff --history /var/lib/flocate/flocate.db

# Only modifications, machine readable (one JSON object per line)
sudo flocate-showdiff --history --modified-only --json /var/lib/flocate/flocate.db

# Find files as with locate
flocate '*.pem'
flocate -i -b 'authorized_keys'
```

A MODIFIED event prints the old and new attributes and a `changed:` line such
as `changed: size,mtime,ctime,hash`. Changes of `mode`, `uid` or `gid` show up
with `ctime` only; a changed `hash` without a changed `mtime` points at a
timestamp that was reset after editing.

### 3. Keep snapshots and compare them

The database is a self-contained snapshot. Copying it once a day gives you
point-in-time comparisons that are independent of `HISTORY_DEPTH`:

```sh
sudo cp /var/lib/flocate/flocate.db /var/backups/flocate-$(date +%F).db
sudo flocate-showdiff /var/backups/flocate-2026-10-01.db /var/lib/flocate/flocate.db
```

### 4. Verify a backup, image or the running system

`--live ROOT` walks the tree the database was built for, appended to `ROOT`,
recomputes metadata and hashes and reports what differs. Hashes are only
recomputed for files the database has a digest for, so files above the
`HASH_MAX_SIZE` of the scan are compared by their other attributes; new files
are hashed up to `--hash-max-size` (default: no limit):

```sh
sudo flocate-showdiff --live /mnt/image  /var/backups/flocate-2026-10-01.db   # mounted image
sudo flocate-showdiff --live /           /var/lib/flocate/flocate.db           # running system
```

The pruning configuration stored in the database (`PRUNEFS`, `PRUNENAMES`,
`PRUNEPATHS`) is honoured; bind mounts are not detected in this mode.

### 5. Occasionally

```sh
sudo updatedb --rehash-all          # re-read every file instead of trusting size/mtime/ctime
sudo updatedb -v --debug-pruning    # see what is scanned, pruned and not hashed
```

Changing `METADATA_HASH` or any pruning variable makes the next run a full
rebuild that is recorded as a new baseline.

## flocate-showdiff output

```
=== run 2026-10-22 02:08:34 +0200 (baseline: full scan, no per-file events) ===
=== run 2026-10-23 02:08:38 +0200 ===
[MODIFIED] /etc/ssh/sshd_config
  old: mode=100644 uid=0 gid=0 size=3223 mtime=1792627709.973508801 ctime=... atime=... hash=sha256:...
  new: mode=100644 uid=0 gid=0 size=3251 mtime=1792627714.197516230 ctime=... atime=... hash=sha256:...
  changed: size,mtime,ctime,hash
[ADDED] /root/.ssh/authorized_keys
  new: mode=100600 uid=0 gid=0 size=412 ...
[REMOVED] /var/log/auth.log.1
  old: mode=100640 uid=0 gid=4 size=98123 ...
```

With `--json` every event becomes one object with the keys `kind`, `path`,
`time`, `old`, `new` and `changed`; run markers have `kind: "RUN"`, `time` and
`baseline`.

## Command-line reference

### `flocate`

| Option short | Option long                   | Description |
| :---         | :---                          | :---        |
| `-A`         | `--all`                       | Ignored for mlocate compatibility. |
| `-b`         | `--basename`                  | Match against the filename component only. |
| `-c`         | `--count`                     | Suppress individual paths and print the total at the end. |
| `-d`         | `--database DBPATH`           | Add one or more databases to search (colon-delimited list is accepted). |
| `-e`         | `--existing`                  | Report only entries that still exist at lookup time. |
| `-i`         | `--ignore-case`               | Case-insensitive search (slower and limited Unicode folding). |
| `-l`         | `--limit LIMIT`               | Stop after `LIMIT` matches; `--count` is capped at the same value. |
| `-N`         | `--literal`                   | Output raw paths without shell-style escaping (escaping only happens on a terminal). |
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
|              | `--debug-pruning`             | Emit verbose pruning and hashing diagnostics on stderr. |
| `-h`         | `--help`                      | Display usage information. |
| `-o`         | `--output FILE`               | Write the database to `FILE` instead of the default. |
| `-b`         | `--block-size SIZE`           | Compress `SIZE` filenames per block (default 32). |
|              | `--prune-bind-mounts FLAG`    | Override `PRUNE_BIND_MOUNTS` (`yes`/`no`). |
|              | `--prunefs FS`                | Override `PRUNEFS` entirely. |
|              | `--prunenames NAMES`          | Override `PRUNENAMES` entirely. |
|              | `--prunepaths PATHS`          | Override `PRUNEPATHS` entirely. |
|              | `--metadata-hash ALGO`        | Hash regular files with `none`, `xxh64`, or `sha256`. |
|              | `--history-depth N`           | Keep metadata/history for the newest `N` runs (0 disables history). |
|              | `--compare-atime FLAG`        | Record atime-only changes as MODIFIED (`yes`/`no`, default `no`). |
|              | `--hash-max-size SIZE`        | Do not hash files larger than `SIZE` (suffix `K`/`M`/`G`/`T`; 0 = no limit). |
|              | `--rehash-all`                | Re-hash every file even if size, mtime and ctime are unchanged. |
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

Databases produced by `flocate-build` contain no metadata or history.

### `flocate-showdiff`

| Option short | Option long                   | Description |
| :---         | :---                          | :---        |
|              | `--history DB`                | Replay the per-run history embedded in `DB` (also the default with a single DB argument). |
|              | `OLD_DB NEW_DB`               | Positional arguments that trigger snapshot diff mode. |
|              | `--live ROOT DB`              | Compare `DB` against the live tree below `ROOT` (the database's scan root is appended). |
|              | `--added-only`                | Filter the output to ADDED events. |
|              | `--removed-only`              | Filter the output to REMOVED events. |
|              | `--modified-only`             | Filter the output to MODIFIED events. |
|              | `--compare-atime`             | Treat atime-only differences as modifications in diff/live mode. |
|              | `--hash-max-size SIZE`        | Live mode: do not hash new files larger than `SIZE` (`K`/`M`/`G`/`T` suffixes, default no limit). Known files are hashed exactly when the database has a digest for them. |
|              | `--json`                      | Print one JSON object per event instead of the text format. |
|              | `--help`                      | Show usage information. |
| `-V`         | `--version`                   | Print version/license information. |

## Configuration file

`/etc/updatedb.conf` understands, in addition to the classic pruning variables
`PRUNE_BIND_MOUNTS`, `PRUNEFS`, `PRUNENAMES` and `PRUNEPATHS`:

| Variable        | Values                              | Default |
| :---            | :---                                | :---    |
| `METADATA_HASH` | `none`, `xxh64`, `sha256`           | `none`  |
| `HISTORY_DEPTH` | number of runs to keep, `0` = off   | `1`     |
| `COMPARE_ATIME` | `yes` / `no`                        | `no`    |
| `HASH_MAX_SIZE` | bytes, suffix `K`/`M`/`G`/`T`, `0` = no limit | `0` |

Without the file no file system is pruned; with hashing enabled this is safe
(pseudo file systems are never hashed) but slow. Ship a `PRUNEFS` list.

## Testing

`tests/run-tests.sh` builds temporary trees and exercises every option of the
four tools, including hash values against `sha256sum` and the XXH64 reference
vectors, history semantics, pruning and the three `flocate-showdiff` modes. It
runs as an unprivileged user and takes about a minute, most of it waiting for
directory timestamps to settle (`updatedb` rescans directories modified less
than three seconds ago).

```sh
make test                       # meson test -C build
tests/run-tests.sh build        # same, with per-check output
FLOCATE_TEST_SETTLE=4 tests/run-tests.sh build
```

## Troubleshooting

### System-wide installation (flocate's updatedb conflicts with locate, mlocate or plocate)

```sh
sudo dpkg -l | grep locate

ii  locate                         4.9.0-4       amd64        maintain and query an index of a directory tree
ii  mlocate                        1.1.18-1      all          transitional dummy package
ii  plocate                        1.1.18-1      amd64        much faster locate
```

or

```sh
ls -la /usr/bin/locate /etc/alternatives/locate /usr/bin/plocate
ls -la /usr/bin/updatedb /etc/alternatives/updatedb /usr/sbin/updatedb.plocate
```

If `/usr/bin/locate` and `/usr/bin/updatedb` point through `/etc/alternatives`
to plocate, the distribution's tools shadow a source-built flocate, which
installs to `/usr/local/bin` and `/usr/local/sbin` (the `.deb` package conflicts
with those packages and cannot be shadowed). Either remove the other package

```sh
sudo apt remove locate      # or plocate, mlocate
```

or, at your own risk, point the symlinks at flocate:

```sh
sudo ln -fs /usr/local/sbin/updatedb /usr/bin/updatedb
sudo ln -fs /usr/local/bin/flocate /usr/bin/locate
```

### `flocate-showdiff` says the database is not supported

Databases written by flocate 1.2.0 use an older metadata layout. Run
`updatedb` once; it rebuilds the database (recorded as baseline).

### Every file shows up as MODIFIED

Check whether `COMPARE_ATIME="yes"` is set on a `relatime` mount, or whether
the hash algorithm or pruning configuration was changed between the runs you
compare (a configuration change triggers a full rebuild).

## References

- Installed man pages: `flocate(1)`, `updatedb(8)`, `updatedb.conf(5)`,
  `flocate-build(8)`, `flocate-showdiff(1)`.
- Upstream plocate: <https://plocate.sesse.net/>, <https://github.com/plocate/plocate>.
