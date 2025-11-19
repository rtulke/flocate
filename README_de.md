# Flocate

[English](README.md)

## Überblick

Dieses Repository ergänzt [flocate](https://flocate.sesse.net/) um forensische
Funktionen. Flocate basiert auf dem ursprünglichen
[plocate-Projekt](https://github.com/plocate/plocate); viele Ideen und große
Teile der Implementierung stammen von dort.
Neben der ursprünglichen, schnellen locate(1)-Implementierung kann
`updatedb` jetzt pro Datei Metadaten (Modus, Besitz, Zeitstempel, Größe sowie
optionale Hashes) erfassen und pro Lauf eine Änderungshistorie speichern. Das
Tool `flocate-showdiff` spielt diese Historie ab, vergleicht zwei Datenbanken
oder prüft eine Datenbank gegen ein Live-Dateisystem.

## Wichtige Funktionen

- Optionales Hashing über `--metadata-hash=xxh64|sha256`
  (bzw. `METADATA_HASH` in `/etc/updatedb.conf`) ohne den normalen locate-Flow
  auszubremsen.
- Behalten Sie die letzten `N` Läufe in der Datenbank mit
  `--history-depth=N`/`HISTORY_DEPTH`. Jeder Lauf erhält einen Marker, damit er
  gezielt getrimmt oder ausgewertet werden kann.
- Modi von `flocate-showdiff`:
  - `flocate-showdiff --history DB`: gibt die aufgezeichneten Add/Remove/Modify-
    Ereignisse der neuesten Läufe aus.
  - `flocate-showdiff ALT_DB NEU_DB`: vergleicht zwei Snapshots.
  - `flocate-showdiff --live /mnt/root DB`: vergleicht Dateien unter dem
    angegebenen Root mit einer Datenbank und berechnet Metadaten/Hashes erneut.
- Für alle Werkzeuge stehen groff-Manpages bereit: `flocate(1)`, `updatedb(8)`,
  `flocate-build(8)` und `flocate-showdiff(1)`.

## Bauen und Installieren

Abhängigkeiten: C++17-Compiler, Meson ≥ 0.61, Ninja, libzstd sowie optional
liburing.

```sh
# einmal konfigurieren (optional MESON_ARGS="--prefix=/opt/flocate")
make config

# bauen
make

# Tests ausführen
make test

# installieren / wieder entfernen
sudo make install
sudo make uninstall
```

Die Makefile-Ziele kapseln Meson, daher kann `make config` jederzeit für neue
`MESON_ARGS` genutzt werden.

## Besonderheiten von updatedb

- Mit `updatedb --metadata-hash=sha256` werden reguläre Dateien vor dem
  Serialisieren gehasht.
- Mit `updatedb --history-depth=3` (oder `HISTORY_DEPTH="3"` in
  `/etc/updatedb.conf`) bleiben die drei jüngsten Läufe in der Datenbank
  erhalten.
- Konfigurationsblöcke in der Datenbank sorgen dafür, dass inkompatible
  Änderungen automatisch einen Neuaufbau auslösen.

## flocate-showdiff verwenden

```
# interne Historie inspizieren
flocate-showdiff --history /var/lib/flocate/flocate.db

# zwei Snapshots vergleichen
flocate-showdiff /tmp/alt.db /tmp/neu.db

# Datenbank mit Live-Dateisystem vergleichen (z. B. eingehängtes Backup)
flocate-showdiff --live /mnt/backup /var/lib/flocate/flocate.db
```

Die Ausgabe zeigt die betroffenen Pfade sowie alte/neue Metadaten (Hashwerte
inklusive, sofern verfügbar).

## CLI-Referenz

### `flocate`

| Option | Beschreibung |
| --- | --- |
| `-A`, `--all` | Wird nur für die Kompatibilität zu mlocate akzeptiert. |
| `-b`, `--basename` | Nur den Dateinamen (ohne Verzeichnisse) vergleichen. |
| `-c`, `--count` | Keine einzelnen Pfade ausgeben, sondern nur die Gesamtsumme. |
| `-d`, `--database DBPFAD` | Zusätzliche Datenbanken durchsuchen (auch als durch Doppelpunkte getrennte Liste). |
| `-e`, `--existing` | Nur Einträge melden, die beim Lookup noch existieren. |
| `-i`, `--ignore-case` | Groß-/Kleinschreibung ignorieren (langsamer, eingeschränkte Unicode-Unterstützung). |
| `-l`, `--limit ANZAHL` | Nach `ANZAHL` Treffern abbrechen; begrenzt auch `--count`. |
| `-N`, `--literal` | Pfade ohne Shell-Quoting ausgeben. |
| `-0`, `--null` | Treffer mit einem NUL-Zeichen statt mit Zeilenumbrüchen trennen. |
| `-r`, `--regexp` | Muster als POSIX-Basisreguläre Ausdrücke interpretieren (erzwingt linearen Scan). |
| `--regex` | Muster als POSIX-erweiterte reguläre Ausdrücke interpretieren. |
| `-w`, `--wholename` | Den kompletten Pfadnamen abgleichen (Standard, solange `-b` nicht gesetzt ist). |
| `--help` | Kurze Hilfe anzeigen. |
| `--version`, `-V` | Versions- und Lizenztext ausgeben. |

### `updatedb`

| Option | Beschreibung |
| --- | --- |
| `-f`, `--add-prunefs FS` | Die durch Leerzeichen getrennten Dateisysteme aus `FS` zu `PRUNEFS` hinzufügen. |
| `-n`, `--add-prunenames NAMEN` | Die Namen zu `PRUNENAMES` hinzufügen. |
| `-e`, `--add-prunepaths PFADLISTE` | Die Pfade zu `PRUNEPATHS` hinzufügen. |
| `--add-single-prunepath PFAD` | Genau einen Pfad (auch mit Leerzeichen) zu `PRUNEPATHS` hinzufügen. |
| `-U`, `--database-root PFAD` | Nur den Teilbaum unter `PFAD` indizieren. |
| `--debug-pruning` | Entscheidungen zur Auslassung auf stderr protokollieren. |
| `-h`, `--help` | Hilfe anzeigen. |
| `-o`, `--output DATEI` | Die Datenbank in `DATEI` schreiben statt in die Voreinstellung. |
| `--prune-bind-mounts FLAG` | `PRUNE_BIND_MOUNTS` überschreiben (`yes`/`no`). |
| `--prunefs FS` | `PRUNEFS` vollständig ersetzen. |
| `--prunenames NAMEN` | `PRUNENAMES` vollständig ersetzen. |
| `--prunepaths PFADLISTE` | `PRUNEPATHS` vollständig ersetzen. |
| `--metadata-hash ALGO` | Reguläre Dateien mit `none`, `xxh64` oder `sha256` hashen. |
| `--history-depth N` | Metadaten/Historie für die letzten `N` Läufe speichern (`0` deaktiviert das Feature). |
| `-l`, `--require-visibility FLAG` | Festlegen, ob locate später Sichtbarkeitsprüfungen erzwingt. |
| `-v`, `--verbose` | Jeden gefundenen Pfad sofort ausgeben. |
| `-V`, `--version` | Versions- und Lizenztext ausgeben. |

### `flocate-build`

| Option | Beschreibung |
| --- | --- |
| `-b`, `--block-size ZAHL` | `ZAHL` Dateinamen pro Block komprimieren (Standard 32). |
| `-p`, `--plaintext` | Die Eingabe als zeilenbasierte Textliste statt als mlocate-DB behandeln. |
| `-l`, `--require-visibility FLAG` | Das Sichtbarkeitsflag in der erzeugten Datenbank setzen. |
| `--help` | Hilfe anzeigen. |
| `--version`, `-V` | Versions-/Lizenzangaben ausgeben. |

### `flocate-showdiff`

| Option | Beschreibung |
| --- | --- |
| `--history DB` | Die eingebettete Historie aus `DB` abspielen. |
| `ALT_DB NEU_DB` | Positionsargumente, die den Snapshot-Vergleich aktivieren. |
| `--live ROOT DB` | `DB` mit dem Live-Dateisystem unter `ROOT` vergleichen. |
| `--added-only` | Nur ADDED-Ereignisse anzeigen. |
| `--removed-only` | Nur REMOVED-Ereignisse anzeigen. |
| `--modified-only` | Nur MODIFIED-Ereignisse anzeigen. |
| `--help` | Hilfe anzeigen. |
| `--version`, `-V` | Versions-/Lizenzangaben ausgeben. |

## Konfiguration und Dokumentation

- `/etc/updatedb.conf` versteht neben den klassischen Pruning-Optionen jetzt
  auch `METADATA_HASH` und `HISTORY_DEPTH`.
- Die installierten Manpages liefern die vollständige Referenz:
  `flocate(1)`, `updatedb(8)`, `flocate-build(8)` und `flocate-showdiff(1)`.

## Mitmachen

Details zu Arbeitsabläufen finden sich in `AGENTS.md`; der langfristige Plan ist
in `FORENSICS_PROGRESS.md` bzw. `FORENSICS_HISTORY.md` dokumentiert. Beiträge
und Issue-Reports sind jederzeit willkommen!
