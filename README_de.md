# Flocate - [F]orensic plocate

[English Version](README.md)

## Überblick

flocate erweitert [plocate](https://plocate.sesse.net/) um forensische
Werkzeuge. Der schnelle Trigramm-Index und das Suchwerkzeug `flocate` stammen
von plocate; zusätzlich erfasst flocates `updatedb` pro Datei Metadaten (Modus,
Besitzer, Größe, mtime/ctime/atime und optional einen Inhalts-Hash) und führt
in der Datenbank eine Änderungshistorie der letzten Läufe. Das Begleitwerkzeug
`flocate-showdiff` spielt diese Historie ab, vergleicht zwei Datenbanken oder
eine Datenbank mit einem live eingehängten Dateisystem (z. B. einem Image oder
Backup).

## Hauptfunktionen

- Metadaten zu jedem indizierten Eintrag, optional mit Hash über
  `--metadata-hash=xxh64|sha256` (bzw. `METADATA_HASH` in `/etc/updatedb.conf`).
  Hashes werden in kanonischer Form gespeichert und stimmen mit `xxhsum` und
  `sha256sum` überein.
- Günstige tägliche Läufe: Eine Datei wird nur neu gelesen und gehasht, wenn
  sich Größe, mtime oder ctime geändert haben; `--rehash-all` erzwingt einen
  vollständigen Durchlauf. Pseudo-Dateisysteme (`proc`, `sysfs`, …) werden
  indiziert, aber nie gehasht; `--hash-max-size` begrenzt die Dateigröße.
- Änderungshistorie der letzten `N` Läufe (`--history-depth=N` /
  `HISTORY_DEPTH`), jeder Lauf durch einen Marker abgegrenzt. Der erste Lauf
  wird als Baseline aufgezeichnet statt mit einem Ereignis pro Datei.
- Zugriffszeiten werden gespeichert, aber standardmäßig nicht verglichen
  (`--compare-atime` / `COMPARE_ATIME` schaltet das ein), weil auf
  `relatime`-Mounts jeder Lesezugriff sie verändert. Das Hashen selbst
  verändert die atime nicht (`O_NOATIME`).
- `flocate-showdiff`-Modi: `--history DB`, `OLD_DB NEW_DB`, `--live ROOT DB`;
  Filter, `--json` für Skripte und eine `changed:`-Zeile mit den abweichenden
  Feldern.
- Datenbank-Einträge sind byteorder-unabhängig, eine Datenbank kann also auf
  einem anderen Rechner ausgewertet werden.
- groff-Manpages für `flocate(1)`, `updatedb(8)`, `updatedb.conf(5)`,
  `flocate-build(8)` und `flocate-showdiff(1)` sowie eine End-to-End-Testsuite.

## Installation aus Paketen

Fertige `.deb`-Pakete für Debian 12/13 und Ubuntu 24.04/26.04, jeweils für
amd64 und arm64, hängen an jedem
[Release](https://github.com/rtulke/flocate/releases); `latest` ist ein
laufend aktualisierter Build von `main`. Das Paket für die eigene Distribution
und Architektur installieren:

```sh
sudo apt install ./flocate_*_<debian12|debian13|ubuntu2404|ubuntu2604>_<amd64|arm64>.deb
```

Das Paket ersetzt locate, mlocate und plocate (apt entfernt sie), legt die
Gruppe `flocate` an, setzt das setgid-Bit auf `/usr/bin/flocate`, registriert
`locate` und `updatedb` über das Alternatives-System (das Programm selbst heißt
`/usr/sbin/flocate-updatedb`), installiert `/etc/updatedb.conf` mit den
üblichen Pruning-Vorgaben und aktiviert den täglichen `flocate-updatedb.timer`,
der direkt nach der Installation auch einmal läuft. `sudo apt purge flocate`
entfernt auch die Datenbank.

Pakete selbst bauen:

```sh
packaging/build-deb-native.sh   # auf einem Debian/Ubuntu-System -> dist/flocate_<version>_<distro>_<arch>.deb
packaging/build-deb.sh          # alle vier Ziele, jedes in seinem eigenen Docker/Podman-Container
packaging/test-install.sh       # jedes Paket in einem frischen Container installieren und testen
```

Ein einfaches `dpkg-buildpackage -us -uc -b` im Checkout funktioniert ebenfalls.
Die Version stammt aus `meson.build` und muss zu `debian/changelog` passen;
Builds, die nicht auf dem passenden Tag `v<version>` stehen, erhalten den
Zusatz `+git<datum>.<commit>`. Der GitHub-Workflow `packages.yml` baut und
testet alle Pakete bei jedem Push und veröffentlicht sie auf der Releases-Seite;
`tests.yml` führt die Testsuite aus.

## Bauen und installieren

Abhängigkeiten: C++17-Compiler, Meson ≥ 0.61, Ninja, libzstd, optional
liburing. flocate läuft ausschließlich unter Linux.

Debian/Ubuntu und Derivate:

```sh
sudo apt install meson ninja-build cmake cmake-data pkg-config libzstd-dev liburing-dev git
```

Beim Bauen aus den Quellen darf keine andere `locate`-Implementierung (locate,
mlocate, plocate) installiert sein; siehe [Fehlerbehebung](#fehlerbehebung).
Das `.deb`-Paket kümmert sich selbst darum.

```sh
sudo dpkg -l '*locate*'
sudo apt remove locate   # oder plocate, mlocate
```

Klonen, bauen, testen, installieren:

```sh
git clone https://github.com/rtulke/flocate.git
cd flocate

make config          # meson setup build; bei Bedarf MESON_ARGS="--prefix=/opt/flocate"
make                 # bauen
make test            # optional: End-to-End-Testsuite gegen den Build ausführen
sudo groupadd flocate
sudo make install    # oder: sudo ninja -C build install
sudo make uninstall  # wieder entfernen
```

Alle Targets kapseln Meson; nach Änderung von `MESON_ARGS` erneut `make config`
ausführen. Meson-Optionen (`meson_options.txt`): `locategroup`, `dbpath`,
`updatedb_progname`, `install_systemd`, `systemunitdir`, `install_cron`.

## Verwendung im Alltag

### 1. Einmal konfigurieren

Die forensischen Einstellungen gehören neben die üblichen Pruning-Variablen in
`/etc/updatedb.conf`. Der systemd-Timer ruft `updatedb` ohne Argumente auf,
diese Datei bestimmt also die Produktivläufe.

```sh
PRUNE_BIND_MOUNTS="yes"
PRUNEFS="NFS afs autofs cifs fuse.sshfs proc sysfs tmpfs ..."
PRUNEPATHS="/tmp /var/spool /media /var/tmp"
PRUNENAMES=".git .hg .svn"

METADATA_HASH="sha256"     # none | xxh64 | sha256
HISTORY_DEPTH="14"         # zwei Wochen tägliche Läufe behalten; 0 schaltet die Historie ab
COMPARE_ATIME="no"         # "yes" protokolliert auch reine atime-Änderungen
HASH_MAX_SIZE="2G"         # größere Dateien nicht hashen; 0 = keine Grenze
```

Baseline erzeugen und den täglichen Timer aktivieren:

```sh
sudo updatedb                                 # erster Lauf: Vollscan, als Baseline aufgezeichnet
sudo systemctl enable --now flocate-updatedb.timer
```

### 2. Täglich: Was hat sich geändert?

```sh
# Alles, was die letzten Läufe aufgezeichnet haben, nach Lauf gruppiert
sudo flocate-showdiff --history /var/lib/flocate/flocate.db

# Nur Änderungen, maschinenlesbar (ein JSON-Objekt pro Zeile)
sudo flocate-showdiff --history --modified-only --json /var/lib/flocate/flocate.db

# Dateien suchen wie mit locate
flocate '*.pem'
flocate -i -b 'authorized_keys'
```

Ein MODIFIED-Ereignis zeigt alte und neue Attribute sowie eine `changed:`-Zeile
wie `changed: size,mtime,ctime,hash`. Änderungen an `mode`, `uid` oder `gid`
erscheinen nur mit `ctime`; ein geänderter `hash` ohne geänderte `mtime`
deutet auf einen nach dem Bearbeiten zurückgesetzten Zeitstempel hin.

### 3. Snapshots aufbewahren und vergleichen

Die Datenbank ist ein in sich geschlossener Snapshot. Eine tägliche Kopie
erlaubt Zeitpunktvergleiche unabhängig von `HISTORY_DEPTH`:

```sh
sudo cp /var/lib/flocate/flocate.db /var/backups/flocate-$(date +%F).db
sudo flocate-showdiff /var/backups/flocate-2026-10-01.db /var/lib/flocate/flocate.db
```

### 4. Backup, Image oder laufendes System prüfen

`--live ROOT` durchläuft den Baum, für den die Datenbank erstellt wurde,
unterhalb von `ROOT`, berechnet Metadaten und Hashes neu und meldet
Abweichungen. Hashes werden nur für Dateien neu berechnet, für die die
Datenbank einen Hash hat; Dateien oberhalb der `HASH_MAX_SIZE` des Scans
werden also über ihre übrigen Attribute verglichen. Neue Dateien werden bis
`--hash-max-size` gehasht (Standard: kein Limit):

```sh
sudo flocate-showdiff --live /mnt/image  /var/backups/flocate-2026-10-01.db   # eingehängtes Image
sudo flocate-showdiff --live /           /var/lib/flocate/flocate.db           # laufendes System
```

Die in der Datenbank gespeicherte Pruning-Konfiguration (`PRUNEFS`,
`PRUNENAMES`, `PRUNEPATHS`) wird berücksichtigt; Bind-Mounts werden in diesem
Modus nicht erkannt.

### 5. Gelegentlich

```sh
sudo updatedb --rehash-all          # jede Datei neu lesen statt Größe/mtime/ctime zu vertrauen
sudo updatedb -v --debug-pruning    # zeigen, was gescannt, ausgelassen und nicht gehasht wird
```

Eine Änderung von `METADATA_HASH` oder einer Pruning-Variable macht den
nächsten Lauf zu einem Vollaufbau, der als neue Baseline aufgezeichnet wird.

## Ausgabe von flocate-showdiff

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

Mit `--json` wird jedes Ereignis zu einem Objekt mit den Schlüsseln `kind`,
`path`, `time`, `old`, `new` und `changed`; Lauf-Marker haben `kind: "RUN"`,
`time` und `baseline`.

## Kommandozeilen-Referenz

### `flocate`

| Option kurz | Option lang                   | Beschreibung |
| :---        | :---                          | :---         |
| `-A`        | `--all`                       | Wird aus Kompatibilität zu mlocate ignoriert. |
| `-b`        | `--basename`                  | Nur gegen den Dateinamen (ohne Pfad) matchen. |
| `-c`        | `--count`                     | Keine Pfade ausgeben, nur die Gesamtanzahl. |
| `-d`        | `--database DBPATH`           | Eine oder mehrere Datenbanken durchsuchen (Doppelpunkt-getrennte Liste). |
| `-e`        | `--existing`                  | Nur Einträge melden, die zum Abfragezeitpunkt noch existieren. |
| `-i`        | `--ignore-case`               | Groß-/Kleinschreibung ignorieren (langsamer, eingeschränktes Unicode-Folding). |
| `-l`        | `--limit LIMIT`               | Nach `LIMIT` Treffern abbrechen; `--count` wird ebenso begrenzt. |
| `-N`        | `--literal`                   | Pfade ohne Shell-Escaping ausgeben (Escaping erfolgt nur auf einem Terminal). |
| `-0`        | `--null`                      | Treffer mit NUL statt Zeilenumbruch trennen. |
| `-r`        | `--regexp`                    | Muster als POSIX Basic Regular Expressions interpretieren (linearer Scan). |
|             | `--regex`                     | Muster als POSIX Extended Regular Expressions interpretieren. |
| `-w`        | `--wholename`                 | Gegen den vollständigen Pfad matchen (Standard, sofern nicht `-b`). |
|             | `--help`                      | Hilfe anzeigen. |
| `-V`        | `--version`                   | Versions-/Lizenzinformationen ausgeben. |

### `updatedb`

| Option kurz | Option lang                   | Beschreibung |
| :---        | :---                          | :---         |
| `-f`        | `--add-prunefs FS`            | Durch Leerzeichen getrennte Dateisysteme an `PRUNEFS` anhängen. |
| `-n`        | `--add-prunenames NAMES`      | Verzeichnisnamen an `PRUNENAMES` anhängen. |
| `-e`        | `--add-prunepaths PATHS`      | Pfade an `PRUNEPATHS` anhängen. |
|             | `--add-single-prunepath PATH` | Einen einzelnen Pfad (auch mit Leerzeichen) an `PRUNEPATHS` anhängen. |
| `-U`        | `--database-root PATH`        | Scan auf `PATH` beschränken. |
|             | `--debug-pruning`             | Ausführliche Pruning- und Hashing-Diagnose auf stderr. |
| `-h`        | `--help`                      | Hilfe anzeigen. |
| `-o`        | `--output FILE`               | Datenbank nach `FILE` statt an den Standardort schreiben. |
| `-b`        | `--block-size SIZE`           | `SIZE` Dateinamen pro Block komprimieren (Standard 32). |
|             | `--prune-bind-mounts FLAG`    | `PRUNE_BIND_MOUNTS` überschreiben (`yes`/`no`). |
|             | `--prunefs FS`                | `PRUNEFS` vollständig überschreiben. |
|             | `--prunenames NAMES`          | `PRUNENAMES` vollständig überschreiben. |
|             | `--prunepaths PATHS`          | `PRUNEPATHS` vollständig überschreiben. |
|             | `--metadata-hash ALGO`        | Reguläre Dateien mit `none`, `xxh64` oder `sha256` hashen. |
|             | `--history-depth N`           | Metadaten/Historie für die letzten `N` Läufe speichern (`0` deaktiviert). |
|             | `--compare-atime FLAG`        | Reine atime-Änderungen als MODIFIED protokollieren (`yes`/`no`, Standard `no`). |
|             | `--hash-max-size SIZE`        | Dateien größer als `SIZE` nicht hashen (Suffix `K`/`M`/`G`/`T`; 0 = keine Grenze). |
|             | `--rehash-all`                | Jede Datei neu hashen, auch wenn Größe, mtime und ctime unverändert sind. |
| `-l`        | `--require-visibility FLAG`   | Rechteprüfung in der erzeugten Datenbank ein-/ausschalten. |
| `-v`        | `--verbose`                   | Jeden gefundenen Pfad ausgeben. |
| `-V`        | `--version`                   | Versions-/Lizenzinformationen ausgeben. |

### `flocate-build`

| Option kurz | Option lang                   | Beschreibung |
| :---        | :---                          | :---         |
| `-b`        | `--block-size SIZE`           | `SIZE` Dateinamen pro Posting-List-Block komprimieren (Standard 32). |
| `-p`        | `--plaintext`                 | Eingabe als zeilenweise Textliste statt mlocate-DB behandeln. |
| `-l`        | `--require-visibility FLAG`   | Das „require visibility“-Flag in der erzeugten Datenbank setzen. |
|             | `--help`                      | Hilfe anzeigen. |
| `-V`        | `--version`                   | Versions-/Lizenzinformationen ausgeben. |

Von `flocate-build` erzeugte Datenbanken enthalten keine Metadaten und keine
Historie.

### `flocate-showdiff`

| Option kurz | Option lang         | Beschreibung |
| :---        | :---                | :---         |
|             | `--history DB`      | Die in `DB` eingebettete Historie pro Lauf abspielen (auch Standard bei einem einzelnen DB-Argument). |
|             | `OLD_DB NEW_DB`     | Positionsargumente für den Snapshot-Vergleich. |
|             | `--live ROOT DB`    | `DB` mit dem Live-Baum unter `ROOT` vergleichen (der Scan-Root der Datenbank wird angehängt). |
|             | `--added-only`      | Nur ADDED-Ereignisse anzeigen. |
|             | `--removed-only`    | Nur REMOVED-Ereignisse anzeigen. |
|             | `--modified-only`   | Nur MODIFIED-Ereignisse anzeigen. |
|             | `--compare-atime`   | Reine atime-Unterschiede im Diff-/Live-Modus als Änderung werten. |
|             | `--hash-max-size SIZE` | Live-Modus: neue Dateien größer als `SIZE` nicht hashen (Suffixe `K`/`M`/`G`/`T`, Standard: kein Limit). Bekannte Dateien werden genau dann gehasht, wenn die Datenbank einen Hash für sie hat. |
|             | `--json`            | Ein JSON-Objekt pro Ereignis statt des Textformats ausgeben. |
|             | `--help`            | Hilfe anzeigen. |
| `-V`        | `--version`         | Versions-/Lizenzinformationen ausgeben. |

## Konfigurationsdatei

`/etc/updatedb.conf` versteht neben den klassischen Pruning-Variablen
`PRUNE_BIND_MOUNTS`, `PRUNEFS`, `PRUNENAMES` und `PRUNEPATHS`:

| Variable        | Werte                                   | Standard |
| :---            | :---                                    | :---     |
| `METADATA_HASH` | `none`, `xxh64`, `sha256`               | `none`   |
| `HISTORY_DEPTH` | Anzahl zu behaltender Läufe, `0` = aus  | `1`      |
| `COMPARE_ATIME` | `yes` / `no`                            | `no`     |
| `HASH_MAX_SIZE` | Bytes, Suffix `K`/`M`/`G`/`T`, `0` = keine Grenze | `0` |

Ohne die Datei wird kein Dateisystem ausgelassen; mit aktiviertem Hashing ist
das zwar sicher (Pseudo-Dateisysteme werden nie gehasht), aber langsam. Eine
`PRUNEFS`-Liste sollte immer gesetzt sein.

## Tests

`tests/run-tests.sh` legt temporäre Bäume an und prüft jede Option der vier
Werkzeuge, darunter Hash-Werte gegen `sha256sum` und die XXH64-Referenzvektoren,
die Historien-Semantik, Pruning und die drei Modi von `flocate-showdiff`. Die
Suite läuft als normaler Benutzer und braucht etwa eine Minute, überwiegend
Wartezeit, bis Verzeichnis-Zeitstempel „alt genug“ sind (`updatedb` scannt
Verzeichnisse neu, die vor weniger als drei Sekunden geändert wurden).

```sh
make test                       # meson test -C build
tests/run-tests.sh build        # dasselbe mit Ausgabe je Prüfung
FLOCATE_TEST_SETTLE=4 tests/run-tests.sh build
```

## Fehlerbehebung

### Systemweite Installation (flocates updatedb kollidiert mit locate, mlocate oder plocate)

```sh
sudo dpkg -l | grep locate

ii  locate                         4.9.0-4       amd64        maintain and query an index of a directory tree
ii  mlocate                        1.1.18-1      all          transitional dummy package
ii  plocate                        1.1.18-1      amd64        much faster locate
```

oder

```sh
ls -la /usr/bin/locate /etc/alternatives/locate /usr/bin/plocate
ls -la /usr/bin/updatedb /etc/alternatives/updatedb /usr/sbin/updatedb.plocate
```

Zeigen `/usr/bin/locate` und `/usr/bin/updatedb` über `/etc/alternatives` auf
plocate, überdecken die Distributionswerkzeuge ein aus den Quellen gebautes
flocate, das nach `/usr/local/bin` und `/usr/local/sbin` installiert wird (das
`.deb`-Paket kollidiert mit diesen Paketen und kann nicht überdeckt werden).
Entweder das andere Paket entfernen

```sh
sudo apt remove locate      # oder plocate, mlocate
```

oder auf eigenes Risiko die Symlinks auf flocate zeigen lassen:

```sh
sudo ln -fs /usr/local/sbin/updatedb /usr/bin/updatedb
sudo ln -fs /usr/local/bin/flocate /usr/bin/locate
```

### `flocate-showdiff` meldet, die Datenbank werde nicht unterstützt

Datenbanken aus flocate 1.2.0 verwenden ein älteres Metadaten-Layout. Einmal
`updatedb` ausführen; die Datenbank wird neu aufgebaut (als Baseline
aufgezeichnet).

### Jede Datei erscheint als MODIFIED

Prüfen, ob `COMPARE_ATIME="yes"` auf einem `relatime`-Mount gesetzt ist oder ob
zwischen den verglichenen Läufen der Hash-Algorithmus bzw. die
Pruning-Konfiguration geändert wurde (eine Konfigurationsänderung löst einen
Vollaufbau aus).

## Referenzen

- Installierte Manpages: `flocate(1)`, `updatedb(8)`, `updatedb.conf(5)`,
  `flocate-build(8)`, `flocate-showdiff(1)`.
- Upstream plocate: <https://plocate.sesse.net/>, <https://github.com/plocate/plocate>.
