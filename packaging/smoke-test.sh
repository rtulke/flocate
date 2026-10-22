#!/usr/bin/env bash
# Installs a flocate .deb the way an end user would (apt-get install, so the
# declared dependencies have to resolve) and checks that the result works:
# permissions, alternatives, a baseline run, a query as an unprivileged user,
# a second run that records changes, the live diff, and a clean purge.
# Meant to run as root in a throwaway container of the target distro.
set -euxo pipefail
PKG=${1:?usage: smoke-test.sh /path/to/flocate_*.deb}
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq --no-install-recommends "$PKG"
dpkg -s flocate | grep -q '^Status: install ok installed'

for bin in /usr/bin/flocate /usr/sbin/flocate-updatedb /usr/sbin/flocate-build /usr/sbin/flocate-showdiff; do
    test -x "$bin"
done
[ "$(stat -c '%a %U %G' /usr/bin/flocate)" = "2755 root flocate" ]
getent group flocate
test -f /etc/updatedb.conf
[ "$(readlink -f /usr/bin/locate)" = /usr/bin/flocate ]
[ "$(readlink -f /usr/bin/updatedb)" = /usr/sbin/flocate-updatedb ]
dpkg -L flocate | grep -q 'systemd/system/flocate-updatedb\.service$'
dpkg -L flocate | grep -q 'systemd/system/flocate-updatedb\.timer$'
dpkg -L flocate | grep -qx /usr/share/man/man1/flocate.1.gz
dpkg -L flocate | grep -qx /usr/share/man/man8/flocate-updatedb.8.gz
dpkg -L flocate | grep -qx /usr/share/man/man5/updatedb.conf.5.gz
dpkg -L flocate | grep -qx /usr/share/man/man1/flocate-showdiff.1.gz
dpkg -L flocate | grep -qx /usr/share/man/man8/flocate-build.8.gz

flocate --version | grep -q '^flocate '
flocate-build --version | grep -q '^flocate-build '
flocate-showdiff --version | grep -q '^flocate-showdiff '
flocate-updatedb --version | grep -q '^updatedb (flocate) '
locate --version | grep -q '^flocate '
updatedb --version | grep -q '^updatedb (flocate) '

# Baseline run with the packaged configuration plus hashing and history.
printf 'METADATA_HASH="xxh64"\nHISTORY_DEPTH="5"\n' >> /etc/updatedb.conf
mkdir -p /srv/smoke
echo one > /srv/smoke/a.txt
flocate-updatedb
[ "$(stat -c '%U %G' /var/lib/flocate/flocate.db)" = "root flocate" ]
[ "$(flocate -r '^/srv/smoke/a\.txt$')" = /srv/smoke/a.txt ]
flocate-showdiff --history /var/lib/flocate/flocate.db | grep -q baseline

# The database is group-readable only; the setgid query tool makes it usable.
useradd -m smoketester
[ "$(su smoketester -s /bin/sh -c "flocate -c -r '^/srv/smoke/a\.txt$'")" = 1 ]
if su smoketester -s /bin/sh -c 'cat /var/lib/flocate/flocate.db >/dev/null 2>&1'; then
    echo "database must not be world-readable" >&2
    exit 1
fi

# A second run records what changed in between.
sleep 4
echo two >> /srv/smoke/a.txt
echo new > /srv/smoke/b.txt
flocate-updatedb
HIST=$(flocate-showdiff --history /var/lib/flocate/flocate.db)
grep -q '^\[MODIFIED\] /srv/smoke/a\.txt' <<<"$HIST"
grep -q '^\[ADDED\] /srv/smoke/b\.txt' <<<"$HIST"
flocate-showdiff --json --history /var/lib/flocate/flocate.db | grep -q '"path":"/srv/smoke/b.txt"'

# Live diff against the running system.
echo three > /srv/smoke/c.txt
flocate-showdiff --live / --added-only /var/lib/flocate/flocate.db | grep -q '^\[ADDED\] /srv/smoke/c\.txt'

# Removal leaves nothing behind.
apt-get purge -y -qq flocate >/dev/null
! test -e /usr/bin/flocate
! test -e /var/lib/flocate
! test -e /etc/updatedb.conf
! update-alternatives --query locate >/dev/null 2>&1
! dpkg-statoverride --list /usr/bin/flocate >/dev/null 2>&1

echo "OK: $PKG installs, indexes, records history, diffs live and purges cleanly"
