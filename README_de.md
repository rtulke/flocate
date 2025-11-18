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

## Konfiguration und Dokumentation

- `/etc/updatedb.conf` versteht neben den klassischen Pruning-Optionen jetzt
  auch `METADATA_HASH` und `HISTORY_DEPTH`.
- Die installierten Manpages liefern die vollständige Referenz:
  `flocate(1)`, `updatedb(8)`, `flocate-build(8)` und `flocate-showdiff(1)`.

## Mitmachen

Details zu Arbeitsabläufen finden sich in `AGENTS.md`; der langfristige Plan ist
in `FORENSICS_PROGRESS.md` bzw. `FORENSICS_HISTORY.md` dokumentiert. Beiträge
und Issue-Reports sind jederzeit willkommen!
