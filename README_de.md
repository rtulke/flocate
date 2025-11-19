# Flocate - [F]orensic plocate

[English Version](README.md)

## Überblick

Dieses Repository erweitert [plocate](https://plocate.sesse.net/) um forensische
Werkzeuge. `flocate` entstand aus dem ursprünglichen
[plocate-Projekt](https://plocate.sesse.net/); Kernideen und große Teile der
Implementierung stammen von dort. Zusätzlich zur schnellen plocate-
Implementierung kann flocates `updatedb` nun pro Datei Metadaten (Modus, Besitz,
Zeitstempel, Größe und optionale Hashes) sammeln und pro Lauf eine Historie
speichern. Mit dem Begleittool `flocate-showdiff` lassen sich diese
Änderungslogs abspielen, zwei Datenbanken vergleichen oder eine Datenbank gegen
ein Live-Dateisystem prüfen.

## Wichtige Funktionen

- Optionales Hashing über `--metadata-hash=xxh64|sha256`
  (bzw. `METADATA_HASH` in `/etc/updatedb.conf`) ohne den normalen locate-Flow
  auszubremsen.
- Behalte die letzten `N` Läufe mittels `--history-depth=N` /
  `HISTORY_DEPTH`. Jeder Lauf wird markiert, sodass er gezielt beschnitten oder
  erneut abgespielt werden kann.
- Modi von `flocate-showdiff`:
  - `flocate-showdiff --history DB`: gibt aufgezeichnete Add/Remove/Modify-
    Ereignisse der jüngsten Läufe aus.
  - `flocate-showdiff ALT_DB NEU_DB`: vergleicht zwei Snapshots.
  - `flocate-showdiff --live /mnt/root DB`: vergleicht Dateien unter dem
    angegebenen Root mit einer Datenbank und berechnet Metadaten/Hashes neu.
- Alle Tools besitzen groff-Manpages (`flocate(1)`, `updatedb(8)`,
  `flocate-build(8)`, `flocate-showdiff(1)`).
- Die Projektstruktur wurde neu organisiert.

## Bauen und Installieren

Abhängigkeiten: C++17-Compiler, Meson ≥ 0.61, Ninja, libzstd und optional
liburing.

Vorbereitung der Abhängigkeiten für Debian/Ubuntu/Kali/Raspberry Pi OS etc.:

```sh
# Meson, Ninja & Co. installieren
sudo apt install meson ninja-build cmake cmake-data pkg-config libzstd-dev liburing-dev git
```

Stelle sicher, dass keine anderen Suchwerkzeuge wie locate, mlocate oder
plocate installiert sind. Falls doch, entferne sie – siehe auch den Abschnitt
[Troubleshooting](#troubleshooting).

```sh
sudo dpkg -l '*locate'
sudo apt remove locate   # bzw. plocate, mlocate
```

Repository klonen:

```sh
mkdir -p ~/dev
cd ~/dev
git clone https://github.com/rtulke/flocate.git
```

Flocate bauen (beliebige Linux-Distribution):

```sh
cd ~/dev/flocate

# einmalig konfigurieren (optional: MESON_ARGS="--prefix=/opt/flocate")
make config

# bauen
make

# systemweit installieren
sudo groupadd flocate
sudo make install   # oder: sudo ninja -C build install

# deinstallieren
sudo make uninstall
```

Alle Ziele kapseln Meson, daher kannst du `make config` jederzeit mit neuen
`MESON_ARGS` aufrufen.

## updatedb-Highlights

- `updatedb --metadata-hash=sha256` hasht reguläre Dateien, bevor die Metadaten
  serialisiert werden.
- `updatedb --history-depth=3` (oder `HISTORY_DEPTH="3"` in
  `/etc/updatedb.conf`) behält die letzten drei Läufe in der Datenbank.
- Konfigurationsblöcke im Header sorgen dafür, dass inkompatible Änderungen
  automatisch einen Neuaufbau erzwingen.

## flocate-showdiff verwenden

```
# interne Historie ansehen
flocate-showdiff --history /var/lib/flocate/flocate.db

# zwei Snapshots vergleichen
flocate-showdiff /tmp/alt.db /tmp/neu.db

# Datenbank gegen Live-System prüfen (z. B. eingehängtes Backup)
flocate-showdiff --live /mnt/backup /var/lib/flocate/flocate.db
```

Die Ausgabe zeigt betroffene Pfade sowie alte/neue Metadaten (Hashes inklusive,
falls vorhanden).

## CLI-Referenz

### `flocate`

| Option kurz | Option lang               | Beschreibung |
| :---        | :---                      | :--- |
| `-A`        | `--all`                   | Nur für mlocate-Kompatibilität relevant. |
| `-b`        | `--basename`              | Nur den Dateinamen (ohne Verzeichnisse) vergleichen. |
| `-c`        | `--count`                 | Keine einzelnen Pfade ausgeben, nur die Summe. |
| `-d`        | `--database DBPFAD`       | Weitere Datenbanken durchsuchen (auch kolonseparierte Liste). |
| `-e`        | `--existing`              | Nur Einträge melden, die beim Lookup noch existieren. |
| `-i`        | `--ignore-case`           | Groß-/Kleinschreibung ignorieren (langsamer, eingeschränkte Unicode-Unterstützung). |
| `-l`        | `--limit ANZAHL`          | Nach `ANZAHL` Treffern abbrechen; begrenzt auch `--count`. |
| `-N`        | `--literal`               | Pfade ohne Shell-Quoting ausgeben. |
| `-0`        | `--null`                  | Treffer mit NUL statt Zeilenumbruch trennen. |
| `-r`        | `--regexp`                | Muster als POSIX-Basisregex interpretieren (erzwingt linearen Scan). |
|             | `--regex`                 | Muster als POSIX-erweiterte Regex interpretieren. |
| `-w`        | `--wholename`             | Gesamten Pfad abgleichen (Standard, solange `-b` fehlt). |
|             | `--help`                  | Hilfe anzeigen. |
| `-V`        | `--version`               | Versions- und Lizenztext ausgeben. |

### `updatedb`

| Option kurz | Option lang                   | Beschreibung |
| :---        | :---                          | :--- |
| `-f`        | `--add-prunefs FS`            | Dateisysteme aus `FS` (durch Leerzeichen getrennt) zu `PRUNEFS` hinzufügen. |
| `-n`        | `--add-prunenames NAMEN`      | Namen zu `PRUNENAMES` hinzufügen. |
| `-e`        | `--add-prunepaths PFADE`      | Pfade zu `PRUNEPATHS` hinzufügen. |
|             | `--add-single-prunepath PFAD` | Einzelnen Pfad (auch mit Leerzeichen) zu `PRUNEPATHS` hinzufügen. |
| `-U`        | `--database-root PFAD`        | Scan auf den Teilbaum unter `PFAD` begrenzen. |
|             | `--debug-pruning`             | Entscheidungen zu ausgelassenen Pfaden auf stderr protokollieren. |
| `-h`        | `--help`                      | Hilfe anzeigen. |
| `-o`        | `--output DATEI`              | Datenbank in `DATEI` schreiben. |
|             | `--prune-bind-mounts FLAG`    | `PRUNE_BIND_MOUNTS` überschreiben (`yes`/`no`). |
|             | `--prunefs FS`                | `PRUNEFS` komplett ersetzen. |
|             | `--prunenames NAMEN`          | `PRUNENAMES` komplett ersetzen. |
|             | `--prunepaths PFADE`          | `PRUNEPATHS` komplett ersetzen. |
|             | `--metadata-hash ALGO`        | Reguläre Dateien mit `none`, `xxh64` oder `sha256` hashen. |
|             | `--history-depth N`           | Metadaten/Historie für die letzten `N` Läufe speichern (`0` deaktiviert). |
| `-l`        | `--require-visibility FLAG`   | Sichtbarkeitsprüfung durch locate aktivieren/deaktivieren. |
| `-v`        | `--verbose`                   | Jeden gefundenen Pfad sofort ausgeben. |
| `-V`        | `--version`                   | Versions-/Lizenztext ausgeben. |

### `flocate-build`

| Option kurz | Option lang               | Beschreibung |
| :---        | :---                      | :--- |
| `-b`        | `--block-size ANZAHL`     | `ANZAHL` Dateinamen pro Posting-Block komprimieren (Standard 32). |
| `-p`        | `--plaintext`             | Eingabe als zeilenbasierten Text statt als mlocate-DB behandeln. |
| `-l`        | `--require-visibility FLAG` | Sichtbarkeitsflag in der erzeugten Datenbank setzen. |
|             | `--help`                  | Hilfe anzeigen. |
| `-V`        | `--version`               | Versions-/Lizenztext ausgeben. |

### `flocate-showdiff`

| Option kurz | Option lang         | Beschreibung |
| :---        | :---                | :--- |
|             | `--history DB`      | Eingebettete Historie aus `DB` abspielen. |
|             | `ALT_DB NEU_DB`     | Positionsargumente für den Snapshot-Vergleich. |
|             | `--live ROOT DB`    | `DB` mit dem Live-Dateisystem unter `ROOT` vergleichen. |
|             | `--added-only`      | Nur ADDED-Ereignisse anzeigen. |
|             | `--removed-only`    | Nur REMOVED-Ereignisse anzeigen. |
|             | `--modified-only`   | Nur MODIFIED-Ereignisse anzeigen. |
|             | `--help`            | Hilfe anzeigen. |
| `-V`        | `--version`         | Versions-/Lizenztext ausgeben. |

## Konfiguration und Dokumentation

- `/etc/updatedb.conf` versteht neben den klassischen Pruning-Optionen auch
  `METADATA_HASH` und `HISTORY_DEPTH`.

## Troubleshooting

### Systemweite Installation (Konflikte mit locate/mlocate/plocate)

```sh
sudo dpkg -l | grep locate
```

oder

```sh
ls -lat /usr/bin/locate
ls -lat /etc/alternatives/locate
ls -lat /usr/bin/plocate
```

und analog

```sh
ls -lat /usr/bin/updatedb
ls -lat /etc/alternatives/updatedb
ls -lat /usr/sbin/updatedb.plocate
```

Dies zeigt Symlink-Ketten wie `/usr/bin/locate -> /etc/alternatives/locate ->
/usr/bin/plocate` bzw. `/usr/bin/updatedb -> … -> /usr/sbin/updatedb.plocate`.
Solche Installationen können unser `updatedb` stören – flocate wird nach
`/usr/local/bin` bzw. `/usr/local/sbin` installiert.

Auf eigenes Risiko kannst du Symlinks direkt auf flocate setzen:

```sh
sudo ln -fs /usr/local/sbin/updatedb /usr/bin/updatedb
sudo ln -fs /usr/local/bin/flocate /usr/bin/locate
```

Empfehlung: deinstalliere nicht benötigte locate-Varianten komplett.

```sh
sudo apt remove locate   # bzw. plocate, mlocate
```

## Referenzen

- Siehe die installierten Manpages für die vollständige Referenz:
  `flocate(1)`, `updatedb(8)`, `flocate-build(8)` und `flocate-showdiff(1)`.
