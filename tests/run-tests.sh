#!/bin/bash
# End-to-end tests for flocate, updatedb, flocate-build and flocate-showdiff.
#
# Usage: tests/run-tests.sh BUILDDIR [UPDATEDB_PROGNAME]
# Runs as an unprivileged user against temporary trees; no installation,
# no root and no /etc/updatedb.conf dependency (all pruning options are
# passed explicitly). Directory timestamps younger than three seconds are
# treated as "still changing" by updatedb, hence the settle() pauses.

set -u

BUILDDIR=${1:?build directory required}
UPDATEDB=$BUILDDIR/${2:-updatedb}
FLOCATE=$BUILDDIR/flocate
BUILD=$BUILDDIR/flocate-build
SHOWDIFF=$BUILDDIR/flocate-showdiff
SETTLE=${FLOCATE_TEST_SETTLE:-4}

for bin in "$UPDATEDB" "$FLOCATE" "$BUILD" "$SHOWDIFF"; do
	[ -x "$bin" ] || { echo "missing binary: $bin" >&2; exit 99; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/flocate-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

passed=0
failed=0
pass() { passed=$((passed + 1)); echo "ok   - $1"; }
fail() { failed=$((failed + 1)); echo "FAIL - $1"; [ $# -gt 1 ] && printf '       %s\n' "${@:2}"; }

# expect_eq NAME EXPECTED ACTUAL
expect_eq() {
	if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected: $2" "actual:   $3"; fi
}
# expect_contains NAME NEEDLE HAYSTACK (fixed string)
expect_contains() {
	if printf '%s' "$3" | grep -qF -- "$2"; then pass "$1"; else fail "$1" "missing: $2" "in: $(printf '%s' "$3" | head -c 600)"; fi
}
expect_not_contains() {
	if printf '%s' "$3" | grep -qF -- "$2"; then fail "$1" "unexpected: $2" "in: $(printf '%s' "$3" | head -c 600)"; else pass "$1"; fi
}
# expect_matches NAME REGEX HAYSTACK (extended regex)
expect_matches() {
	if printf '%s' "$3" | grep -qE -- "$2"; then pass "$1"; else fail "$1" "no match: $2" "in: $(printf '%s' "$3" | head -c 600)"; fi
}
expect_no_match() {
	if printf '%s' "$3" | grep -qE -- "$2"; then fail "$1" "unexpected match: $2" "in: $(printf '%s' "$3" | head -c 600)"; else pass "$1"; fi
}
# expect_status NAME EXPECTED_STATUS cmd...
expect_status() {
	local name=$1 want=$2; shift 2
	"$@" >/dev/null 2>&1
	local got=$?
	if [ "$got" = "$want" ]; then pass "$name"; else fail "$name" "exit status $got, expected $want: $*"; fi
}

settle() { sleep "$SETTLE"; }

# updatedb with the pruning configuration pinned, so the host's
# /etc/updatedb.conf cannot interfere.
upd() {
	local db=$1 tree=$2; shift 2
	"$UPDATEDB" -o "$db" -U "$tree" -l no --prunepaths "" --prunenames "" --prunefs "" --prune-bind-mounts no "$@"
}
count() { "$FLOCATE" -d "$1" -c "$2"; }
hist() { "$SHOWDIFF" --history "$1"; }
events() { hist "$1" | grep -c "^\[$2\]"; }
runs() { hist "$1" | grep -c '^=== run'; }

make_tree() {
	local t=$1
	mkdir -p "$t/a/sub" "$t/b" "$t/c/deep/er"
	for i in 1 2 3; do
		echo "a$i" >"$t/a/f$i.txt"
		echo "s$i" >"$t/a/sub/s$i.txt"
		echo "b$i" >"$t/b/g$i.dat"
	done
	echo "deep" >"$t/c/deep/er/leaf"
	printf 'q"uote\\back\ttab' >"$t/b/odd\"name"
	ln -s f1.txt "$t/a/link"
}

# ---------------------------------------------------------------- help / version
section() { echo "# $1"; }

section "help and version output"
expect_contains "updatedb --help lists forensic options" "--metadata-hash" "$("$UPDATEDB" --help 2>&1)"
expect_contains "updatedb --help lists --compare-atime" "--compare-atime" "$("$UPDATEDB" --help)"
expect_contains "updatedb --help lists --hash-max-size" "--hash-max-size" "$("$UPDATEDB" --help)"
expect_contains "updatedb --help lists --rehash-all" "--rehash-all" "$("$UPDATEDB" --help)"
expect_status "updatedb -h exits 0" 0 "$UPDATEDB" -h
expect_contains "updatedb --version" "updatedb" "$("$UPDATEDB" --version)"
expect_status "updatedb -V exits 0" 0 "$UPDATEDB" -V
expect_contains "flocate --help" "Usage: flocate" "$("$FLOCATE" --help 2>&1)"
expect_contains "flocate -V" "flocate" "$("$FLOCATE" -V)"
expect_contains "flocate-build --help" "plaintext" "$("$BUILD" --help 2>&1)"
expect_contains "flocate-build --version" "flocate-build" "$("$BUILD" --version)"
expect_contains "flocate-showdiff --help" "--json" "$("$SHOWDIFF" --help 2>&1)"
VER=$("$SHOWDIFF" --version | head -1)
expect_contains "flocate-showdiff --version carries package version" "flocate-showdiff " "$VER"
expect_not_contains "flocate-showdiff --version is not hard-coded" "1.1.24-dev" "$VER"
expect_eq "flocate-showdiff -V equals --version" "$VER" "$("$SHOWDIFF" -V | head -1)"

# ---------------------------------------------------------------- argument validation
section "argument validation"
T=$WORK/val; mkdir -p "$T"
expect_status "updatedb rejects --metadata-hash bogus" 1 upd "$WORK/val.db" "$T" --metadata-hash bogus
expect_status "updatedb rejects --history-depth -1" 1 upd "$WORK/val.db" "$T" --history-depth -1
expect_status "updatedb rejects --history-depth abc" 1 upd "$WORK/val.db" "$T" --history-depth abc
expect_status "updatedb rejects --compare-atime maybe" 1 upd "$WORK/val.db" "$T" --compare-atime maybe
expect_status "updatedb rejects --hash-max-size 12X" 1 upd "$WORK/val.db" "$T" --hash-max-size 12X
expect_status "updatedb rejects --block-size 0" 1 upd "$WORK/val.db" "$T" -b 0
expect_status "updatedb rejects --require-visibility maybe" 1 "$UPDATEDB" -o "$WORK/val.db" -U "$T" -l maybe
expect_status "updatedb rejects --database-root /nonexistent" 1 "$UPDATEDB" -o "$WORK/val.db" -U /nonexistent -l no
expect_status "updatedb rejects --prune-bind-mounts maybe" 1 upd "$WORK/val.db" "$T" --prune-bind-mounts maybe
expect_status "updatedb rejects stray operand" 1 upd "$WORK/val.db" "$T" stray
expect_status "updatedb rejects duplicate --output" 1 "$UPDATEDB" -o "$WORK/val.db" -o "$WORK/val2.db" -U "$T" -l no
expect_status "flocate-showdiff rejects unknown option" 1 "$SHOWDIFF" --bogus "$WORK/val.db"
expect_status "flocate-showdiff without arguments" 1 "$SHOWDIFF"
expect_status "flocate-showdiff --live without root" 1 "$SHOWDIFF" --live
expect_status "flocate-showdiff --history with two databases" 1 "$SHOWDIFF" --history a b
expect_status "flocate-showdiff --live with two databases" 1 "$SHOWDIFF" --live / a b
expect_status "flocate-build rejects --block-size 0" 1 "$BUILD" -b 0 -p /dev/null "$WORK/val.db"
expect_status "flocate without pattern" 1 "$FLOCATE" -d "$WORK/none.db"

# ---------------------------------------------------------------- basic indexing and flocate
section "indexing and flocate query options"
T=$WORK/t1; make_tree "$T"; settle
DB=$WORK/t1.db
expect_status "updatedb baseline run" 0 upd "$DB" "$T" --metadata-hash xxh64 --history-depth 3
EXPECTED=$(find "$T" -mindepth 1 | wc -l)
expect_eq "database holds every entry" "$EXPECTED" "$(count "$DB" "$T/")"
expect_eq "flocate -c with substring" "3" "$(count "$DB" ".dat")"
expect_eq "pattern matches directory components by default" "4" "$(count "$DB" "sub")"
expect_eq "flocate -b matches basename only" "1" "$("$FLOCATE" -d "$DB" -b -c "sub")"
expect_eq "flocate -i case-insensitive" "3" "$("$FLOCATE" -d "$DB" -i -c ".DAT")"
expect_eq "flocate -r basic regex" "3" "$("$FLOCATE" -d "$DB" -r -c 'g[0-9]\.dat$')"
expect_eq "flocate --regex extended regex" "3" "$("$FLOCATE" -d "$DB" --regex -c 'g[0-9]+\.dat$')"
expect_eq "flocate -l limits output" "1" "$("$FLOCATE" -d "$DB" -l 1 ".txt" | wc -l | tr -d ' ')"
expect_eq "flocate -0 separates with NUL" "3" "$("$FLOCATE" -d "$DB" -0 ".dat" | tr -cd '\0' | wc -c | tr -d ' ')"
expect_eq "flocate prints names unescaped when not on a tty" "$T/b/odd\"name" "$("$FLOCATE" -d "$DB" odd)"
expect_eq "flocate -N prints literally" "$T/b/odd\"name" "$("$FLOCATE" -d "$DB" -N odd)"
expect_eq "flocate -w whole-name match" "1" "$("$FLOCATE" -d "$DB" -w -c "a/sub/s1")"
expect_eq "flocate -A is accepted" "3" "$("$FLOCATE" -d "$DB" -A -c ".dat")"
rm "$T/b/g2.dat"
expect_eq "flocate -e reports only existing files" "2" "$("$FLOCATE" -d "$DB" -e -c ".dat")"
expect_eq "flocate -d with colon-separated list searches both" "6" "$("$FLOCATE" -d "$DB:$DB" -c ".dat")"
expect_status "flocate on missing database fails" 1 "$FLOCATE" -d "$WORK/missing.db" x
settle

section "updatedb -v and small block size"
OUT=$(upd "$WORK/t1v.db" "$T" -v -b 4 2>&1)
expect_contains "updatedb -v prints discovered paths" "$T/a/f1.txt" "$OUT"
expect_eq "block size 4 database is complete" "$(find "$T" -mindepth 1 | wc -l)" "$(count "$WORK/t1v.db" "$T/")"
expect_eq "flocate -c on block size 4 database" "2" "$(count "$WORK/t1v.db" ".dat")"

# ---------------------------------------------------------------- history semantics
section "history: baseline, no-change runs, change kinds"
expect_eq "baseline run records one run" "1" "$(runs "$DB")"
expect_contains "baseline run is marked" "baseline" "$(hist "$DB")"
expect_eq "baseline run has no ADDED events" "0" "$(events "$DB" ADDED)"
upd "$DB" "$T" --metadata-hash xxh64 --history-depth 3   # g2.dat was removed above
expect_eq "second run detects removal" "1" "$(events "$DB" REMOVED)"
expect_eq "no spurious ADDED after removal run" "0" "$(events "$DB" ADDED)"
expect_eq "no spurious MODIFIED after removal run" "1" "$(events "$DB" MODIFIED)"   # the directory b itself
upd "$DB" "$T" --metadata-hash xxh64 --history-depth 3
expect_eq "unchanged rerun adds no events" "1" "$(events "$DB" REMOVED)"
expect_eq "unchanged rerun adds no MODIFIED" "1" "$(events "$DB" MODIFIED)"
expect_eq "three runs recorded" "3" "$(runs "$DB")"
echo "changed" >>"$T/a/f1.txt"; chmod 600 "$T/a/f2.txt"; echo new >"$T/b/new.dat"; settle
upd "$DB" "$T" --metadata-hash xxh64 --history-depth 3
H=$(hist "$DB")
expect_contains "content change reports size,mtime,ctime,hash" "changed: size,mtime,ctime,hash" "$H"
expect_contains "mode change reports mode,ctime" "changed: mode,ctime" "$H"
expect_contains "new file is ADDED" "[ADDED] $T/b/new.dat" "$H"
expect_eq "history depth 3 trims the oldest run" "3" "$(runs "$DB")"
expect_not_contains "trimmed history no longer holds the baseline marker" "baseline" "$H"
expect_contains "old metadata line printed" "  old: mode=100644" "$H"
expect_contains "ctime and atime are printed" "ctime=" "$H"

section "history depth 0 and 1"
DB0=$WORK/d0.db
upd "$DB0" "$T" --history-depth 0; upd "$DB0" "$T" --history-depth 0
expect_contains "history-depth 0 stores no log" "no history log" "$("$SHOWDIFF" --history "$DB0" 2>&1)"
DB1=$WORK/d1.db
upd "$DB1" "$T" --history-depth 1; upd "$DB1" "$T" --history-depth 1; upd "$DB1" "$T" --history-depth 1
expect_eq "history-depth 1 keeps exactly one run" "1" "$(runs "$DB1")"

section "configuration change forces a baseline rebuild"
upd "$DB" "$T" --metadata-hash sha256 --history-depth 3
expect_contains "switching hash algorithm records a baseline" "baseline" "$(hist "$DB" | tail -1)"

# ---------------------------------------------------------------- hashing
section "hash values"
HT=$WORK/hash; mkdir -p "$HT"
: >"$HT/empty"; printf 'a' >"$HT/a"; printf 'abc' >"$HT/abc"; head -c 100000 /dev/urandom >"$HT/random"; settle
HDB=$WORK/hash.db
upd "$HDB" "$HT" --metadata-hash xxh64 --history-depth 1
# Make a copy of the database, touch nothing, and diff against a rebuilt one to read the hashes.
cp "$HDB" "$WORK/hash-old.db"; echo "x" >"$HT/newfile"; settle
upd "$HDB" "$HT" --metadata-hash xxh64 --history-depth 1
hash_of() { "$SHOWDIFF" --json "$WORK/hash-old.db" "$HDB" | grep "\"path\":\"$1\"" | sed -n 's/.*"hex":"\([0-9a-f]*\)".*/\1/p' | head -1; }
# Only differences are listed, so compare DB against an empty one instead:
EMPTYT=$WORK/empty; mkdir -p "$EMPTYT"; settle; EDB=$WORK/empty.db; upd "$EDB" "$EMPTYT" --metadata-hash xxh64 --history-depth 1
J=$("$SHOWDIFF" --json "$EDB" "$HDB")
hex_for() { printf '%s\n' "$J" | grep "\"path\":\"$HT/$1\"" | sed -n 's/.*"hex":"\([0-9a-f]*\)".*/\1/p'; }
expect_eq "xxh64 of empty file matches reference" "ef46db3751d8e999" "$(hex_for empty)"
expect_eq "xxh64 of 'a' matches reference" "d24ec4f1a98c6e5b" "$(hex_for a)"
expect_eq "xxh64 of 'abc' matches reference" "44bc2cf5ad770999" "$(hex_for abc)"
if command -v xxhsum >/dev/null 2>&1; then
	expect_eq "xxh64 of random data matches xxhsum" "$(xxhsum -H64 "$HT/random" | cut -d' ' -f1)" "$(hex_for random)"
fi
SDB=$WORK/sha.db
upd "$SDB" "$HT" --metadata-hash sha256 --history-depth 1
J=$("$SHOWDIFF" --json "$EDB" "$SDB")
expect_eq "sha256 matches sha256sum" "$(sha256sum "$HT/random" | cut -d' ' -f1)" "$(hex_for random)"
expect_eq "sha256 of empty file" "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" "$(hex_for empty)"
expect_contains "hash kind is reported in JSON" '"kind":"sha256"' "$J"
NDB=$WORK/nohash.db
upd "$NDB" "$HT" --metadata-hash none --history-depth 1
expect_not_contains "--metadata-hash none stores no hashes" '"hex"' "$("$SHOWDIFF" --json "$EDB" "$NDB")"

section "hash reuse, --rehash-all, --hash-max-size"
BIG=$WORK/big; mkdir -p "$BIG"; head -c 1000 /dev/urandom >"$BIG/small"; head -c 3000 /dev/urandom >"$BIG/large"; settle
BDB=$WORK/big.db
upd "$BDB" "$BIG" --metadata-hash sha256 --history-depth 5 --hash-max-size 2K
J=$("$SHOWDIFF" --json "$EDB" "$BDB")
expect_matches "file below --hash-max-size is hashed" "\"path\":\"$BIG/small\".*\"hex\"" "$J"
expect_no_match "file above --hash-max-size has no hash" "\"path\":\"$BIG/large\".*\"hex\"" "$J"
upd "$BDB" "$BIG" --metadata-hash sha256 --history-depth 5 --hash-max-size 1M
J2=$("$SHOWDIFF" --json "$EDB" "$BDB")
expect_matches "raising the limit hashes the large file on the next run" "\"path\":\"$BIG/large\".*\"hex\"" "$J2"
expect_eq "gaining a hash is recorded as a modification" "1" "$(events "$BDB" MODIFIED)"
expect_contains "gaining a hash lists only the hash field" "changed: hash" "$(hist "$BDB")"
upd "$BDB" "$BIG" --metadata-hash sha256 --history-depth 5 --hash-max-size 1M
expect_eq "unchanged files reuse their hash without events" "1" "$(events "$BDB" MODIFIED)"
upd "$BDB" "$BIG" --metadata-hash sha256 --history-depth 5 --hash-max-size 1M --rehash-all
expect_eq "--rehash-all recomputes identical hashes" "1" "$(events "$BDB" MODIFIED)"
expect_eq "--rehash-all keeps database complete" "2" "$(count "$BDB" "$BIG/")"

section "pseudo filesystems are indexed but never hashed"
if [ -d /proc/sys/kernel ]; then
	OUT=$(upd "$WORK/proc.db" /proc/sys/kernel --metadata-hash xxh64 --history-depth 0 --debug-pruning 2>&1)
	expect_contains "updatedb refuses to hash below /proc" "Not hashing below" "$OUT"
	expect_contains "/proc entries are still indexed" "/proc/sys/kernel/hostname" "$("$FLOCATE" -d "$WORK/proc.db" hostname)"
fi

# ---------------------------------------------------------------- atime handling
section "atime handling"
AT=$WORK/atime; mkdir -p "$AT"; echo one >"$AT/one"; echo two >"$AT/two"; settle
ADB=$WORK/atime.db
upd "$ADB" "$AT" --metadata-hash xxh64 --history-depth 5
A1=$(stat -c %X "$AT/one")
expect_eq "hashing does not update atime (O_NOATIME)" "$A1" "$(stat -c %X "$AT/one")"
cat "$AT/one" >/dev/null   # a reader bumps atime under relatime (atime == mtime right after creation)
if [ "$(stat -c %X "$AT/one")" != "$A1" ]; then
	settle; upd "$ADB" "$AT" --metadata-hash xxh64 --history-depth 5
	expect_eq "atime-only change is ignored by default" "0" "$(events "$ADB" MODIFIED)"
	cat "$AT/two" >/dev/null; settle
	upd "$ADB" "$AT" --metadata-hash xxh64 --history-depth 5 --compare-atime yes
	expect_eq "--compare-atime yes records atime-only change" "1" "$(events "$ADB" MODIFIED)"
	expect_contains "atime-only change lists only atime" "changed: atime" "$(hist "$ADB")"
	upd "$ADB" "$AT" --metadata-hash xxh64 --history-depth 5 --compare-atime no
	expect_eq "--compare-atime no is accepted and adds nothing" "1" "$(events "$ADB" MODIFIED)"
else
	echo "skip - filesystem does not update atime on read (noatime); atime tests skipped"
fi

# ---------------------------------------------------------------- pruning options
section "pruning options"
PT=$WORK/prune; mkdir -p "$PT/keep" "$PT/skipname/inner" "$PT/skippath/inner" "$PT/also"
echo k >"$PT/keep/k"; echo s >"$PT/skipname/inner/s"; echo p >"$PT/skippath/inner/p"; echo a >"$PT/also/a"; settle
PDB=$WORK/prune.db
pupd() { "$UPDATEDB" -o "$PDB" -U "$PT" -l no --history-depth 0 "$@"; }
pupd --prunefs "" --prune-bind-mounts no --prunenames "skipname" --prunepaths "$PT/skippath"
expect_eq "--prunenames lists the directory itself" "1" "$(count "$PDB" "$PT/skipname")"
expect_eq "--prunenames does not descend" "0" "$(count "$PDB" "$PT/skipname/")"
expect_eq "--prunepaths does not descend" "0" "$(count "$PDB" "$PT/skippath/")"
expect_eq "unpruned content is indexed" "1" "$(count "$PDB" "$PT/keep/k")"
pupd --prunefs "" --prune-bind-mounts no --add-prunenames "skipname" --add-single-prunepath "$PT/also"
expect_eq "--add-prunenames prunes" "0" "$(count "$PDB" "$PT/skipname/")"
expect_eq "--add-single-prunepath prunes" "0" "$(count "$PDB" "$PT/also/")"
expect_eq "--add-single-prunepath keeps other paths" "1" "$(count "$PDB" "$PT/skippath/inner/p")"
pupd --prunefs "" --prune-bind-mounts no --add-prunepaths "$PT/skippath $PT/also"
expect_eq "--add-prunepaths prunes both" "0" "$(count "$PDB" "inner/p")"
OUT=$(pupd --prune-bind-mounts no --prunefs "nfs cifs" --add-prunefs "fuse.sshfs" --debug-pruning 2>&1)
expect_contains "--prunefs/--add-prunefs end up in the configuration block" "FUSE.SSHFS" "$OUT"
expect_contains "--debug-pruning dumps the configuration block" "conf_block:" "$OUT"
expect_status "--prune-bind-mounts yes is accepted" 0 pupd --prunefs "" --prune-bind-mounts yes
expect_status "--debug (undocumented) is accepted" 0 upd "$PDB" "$PT" --history-depth 0 -D
# Database used by the live-mode pruning test below: prunenames + prunepaths.
PRUNEDB=$WORK/prune-live.db
"$UPDATEDB" -o "$PRUNEDB" -U "$PT" -l no --history-depth 0 --prunefs "" --prune-bind-mounts no --prunenames "skipname" --prunepaths "$PT/skippath"

# ---------------------------------------------------------------- flocate-build
section "flocate-build"
LIST=$WORK/list.txt
printf '/x/one\n/x/two/three\n/y/four\n' >"$LIST"
expect_status "flocate-build --plaintext" 0 "$BUILD" -p "$LIST" "$WORK/build.db"
expect_eq "visibility check hides paths that do not exist" "0" "$("$FLOCATE" -d "$WORK/build.db" -c /x/)"
expect_status "flocate-build --plaintext -l no" 0 "$BUILD" -p -l no "$LIST" "$WORK/build-novis.db"
expect_eq "built database answers queries" "2" "$("$FLOCATE" -d "$WORK/build-novis.db" -c /x/)"
expect_status "flocate-build -b 2 -l no" 0 "$BUILD" -p -b 2 -l no "$LIST" "$WORK/build2.db"
expect_eq "flocate-build with block size 2" "3" "$("$FLOCATE" -d "$WORK/build2.db" -c /)"
expect_contains "flocate-build database has no metadata" "does not contain metadata" "$("$SHOWDIFF" "$WORK/build.db" "$WORK/build2.db" 2>&1)"

# ---------------------------------------------------------------- flocate-showdiff modes
section "flocate-showdiff: history, diff, live, filters, json"
expect_eq "single database argument implies --history" "$(hist "$DB")" "$("$SHOWDIFF" "$DB")"
expect_eq "--added-only filters" "0" "$("$SHOWDIFF" --history --added-only "$DB" | grep -c '^\[MODIFIED\]')"
expect_eq "--modified-only filters" "0" "$("$SHOWDIFF" --history --modified-only "$DB" | grep -c '^\[ADDED\]')"
expect_eq "--removed-only shows removals" "$(events "$DB" REMOVED)" "$("$SHOWDIFF" --history --removed-only "$DB" | grep -c '^\[REMOVED\]')"
expect_eq "two filters combine" "$(( $(events "$DB" ADDED) + $(events "$DB" REMOVED) ))" "$("$SHOWDIFF" --history --added-only --removed-only "$DB" | grep -c '^\[')"
if command -v python3 >/dev/null 2>&1; then
	N=$("$SHOWDIFF" --history --json "$DB" | python3 -c 'import sys,json
n=0
for line in sys.stdin:
    json.loads(line); n+=1
print(n)' 2>&1)
	expect_eq "--json emits one valid JSON object per line" "$(hist "$DB" | grep -c '^\[\|^===')" "$N"
	ODD=$("$SHOWDIFF" --json "$EDB" "$WORK/t1v.db" | grep odd | python3 -c 'import sys,json; print(json.loads(sys.stdin.readline())["path"])')
	expect_eq "--json escapes quotes and backslashes in paths" "$T/b/odd\"name" "$ODD"
fi
expect_contains "--json run marker" '"kind":"RUN"' "$("$SHOWDIFF" --history --json "$DB")"

OLD=$WORK/diff-old.db; NEW=$WORK/diff-new.db
DT=$WORK/diff; mkdir -p "$DT"; echo 1 >"$DT/same"; echo 1 >"$DT/changes"; echo 1 >"$DT/goes"; settle
upd "$OLD" "$DT" --metadata-hash xxh64 --history-depth 0
echo 22 >"$DT/changes"; rm "$DT/goes"; echo n >"$DT/comes"; settle
upd "$NEW" "$DT" --metadata-hash xxh64 --history-depth 0
D=$("$SHOWDIFF" "$OLD" "$NEW")
expect_contains "diff mode: MODIFIED" "[MODIFIED] $DT/changes" "$D"
expect_contains "diff mode: REMOVED" "[REMOVED] $DT/goes" "$D"
expect_contains "diff mode: ADDED" "[ADDED] $DT/comes" "$D"
expect_not_contains "diff mode: unchanged file not listed" "$DT/same" "$D"
expect_contains "diff mode reports hash change" "changed: size,mtime,ctime,hash" "$D"
expect_contains "diff of identical databases" "No differences" "$("$SHOWDIFF" "$NEW" "$NEW")"
expect_eq "diff --added-only" "1" "$("$SHOWDIFF" --added-only "$OLD" "$NEW" | grep -c '^\[')"
cat "$DT/same" >/dev/null; settle
NEW2=$WORK/diff-new2.db; upd "$NEW2" "$DT" --metadata-hash xxh64 --history-depth 0
if [ "$(stat -c %X "$DT/same")" != "$(stat -c %Y "$DT/same")" ]; then
	expect_contains "diff mode ignores atime by default" "No differences" "$("$SHOWDIFF" "$NEW" "$NEW2")"
	expect_contains "diff --compare-atime reports atime" "changed: atime" "$("$SHOWDIFF" --compare-atime "$NEW" "$NEW2")"
fi

section "flocate-showdiff --live"
L=$("$SHOWDIFF" --live / "$NEW2")
expect_contains "live mode against the real tree: no differences" "No differences" "$L"
MIR=$WORK/mirror; mkdir -p "$MIR$DT"; cp -a "$DT/." "$MIR$DT/"
echo more >>"$DT/changes"; rm "$DT/comes"; echo l >"$DT/livenew"
L=$("$SHOWDIFF" --live / "$NEW2")
expect_contains "live mode: MODIFIED" "[MODIFIED] $DT/changes" "$L"
expect_contains "live mode: REMOVED" "[REMOVED] $DT/comes" "$L"
expect_contains "live mode: ADDED" "[ADDED] $DT/livenew" "$L"
expect_no_match "live mode stays inside the indexed tree" "^\[ADDED\] (/tmp|$WORK|$WORK/diff)$" "$L"
expect_eq "live --modified-only" "0" "$("$SHOWDIFF" --live / --modified-only "$NEW2" | grep -c '^\[ADDED\]')"
L=$("$SHOWDIFF" --live "$MIR" "$NEW2")
expect_not_contains "live mode with ROOT: nothing REMOVED in a complete copy" "[REMOVED]" "$L"
expect_not_contains "live mode with ROOT: nothing ADDED in a complete copy" "[ADDED]" "$L"
expect_not_contains "live mode with ROOT reports database paths, not mirror paths" "$MIR" "$L"
mkdir -p "$PT/skipname/inner/x"; echo p >"$PT/skippath/inner/new"; echo k >"$PT/keep/new"
L=$("$SHOWDIFF" --live / "$PRUNEDB")
expect_contains "live mode reports additions in unpruned directories" "[ADDED] $PT/keep/new" "$L"
expect_not_contains "live mode honours PRUNENAMES from the database" "skipname/inner" "$L"
expect_not_contains "live mode honours PRUNEPATHS from the database" "skippath/inner" "$L"
expect_status "live mode with missing root fails" 1 "$SHOWDIFF" --live /nonexistent "$NEW2"

section "flocate-showdiff rejects damaged input"
SIZE=$(stat -c %s "$NEW")
head -c $((SIZE / 2)) "$NEW" >"$WORK/trunc.db"
expect_status "truncated database in history mode" 1 "$SHOWDIFF" --history "$WORK/trunc.db"
expect_status "truncated database in diff mode" 1 "$SHOWDIFF" "$WORK/trunc.db" "$NEW"
expect_status "truncated database in live mode" 1 "$SHOWDIFF" --live / "$WORK/trunc.db"
expect_status "random file is not a database" 1 "$SHOWDIFF" --history "$LIST"
expect_status "missing database file" 1 "$SHOWDIFF" --history "$WORK/missing.db"

echo
echo "# $passed passed, $failed failed"
[ "$failed" -eq 0 ]
