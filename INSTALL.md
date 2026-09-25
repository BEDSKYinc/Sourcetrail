# Installation

Dieser Fork indiziert **Rust, Python und TypeScript**. Die C++-, Java- und
Test-Pakete des Originals sind aus, das spart libclang, JDK und Maven.

## Abhaengigkeiten

Arch:

    sudo pacman -S --needed cmake ninja gcc qt6-base qt6-svg sqlite tinyxml boost python rustup

Debian/Ubuntu: `scripts/install-system-build-dependencies.sh` liegt bei, ist aber
vom Original und installiert auch die Pakete fuer C++ und Java, die hier nicht
gebraucht werden.

Dazu eine Rust-Toolchain (`rustup default stable`) und, fuer den Chat in der
Landkarte, das eingeloggte [Claude CLI](https://claude.com/claude-code)
(`claude`, einmal `/login`).

## Bauen und installieren

    git clone -b rust-indexer https://github.com/BEDSKYinc/Sourcetrail.git
    cd Sourcetrail
    ./scripts/assetbridge/install.sh

Das Skript baut den Rust-Indexer mit cargo, konfiguriert CMake mit dem Preset
`system-release`, baut, und legt alles nach `~/.local/opt/Sourcetrail/app`. Dazu
kommen der Starter `~/.local/bin/sourcetrail-assetbridge` und ein Menueintrag.

Nach einem `git pull` dasselbe Skript nochmal. `--no-build` ueberspringt den
Bau und installiert nur.

Das Build-Verzeichnis liegt **neben** dem Quellbaum, nicht darin:
`../build/system-release`. Das kommt aus `CMakePresets.json`.

## Zweiter Rechner

Der Index selbst liegt in einem eigenen, privaten Repo:

    git clone https://github.com/BEDSKYinc/asset-bridge-index.git

Darin steht die Projektdefinition mit den Beschreibungen und der
Pipeline-Reihenfolge — die `.srctrldb` nicht, die wird neu gebaut.

**Die Falle:** `AssetBridge.srctrlprj` enthaelt absolute Pfade, und zwar
zweierlei — die indizierten Quellordner unter `~/Dokumente/Asset Bridge`, und
die drei Indexer-Kommandos unter `~/surcetai/Sourcetrail`. Stimmt davon etwas
nicht, indiziert Sourcetrail ins Leere und meldet keinen Fehler, sondern ein
leeres Projekt. Entweder die Pfade im Projektfile anpassen, oder die beiden
Checkouts genauso ablegen:

    ~/Dokumente/Asset Bridge          das indizierte Projekt
    ~/surcetai/Sourcetrail            dieser Fork
    ~/surcetai/asset-bridge-index     Index und Notizen

Danach einmal voll indizieren:

    sourcetrail-assetbridge --index -f

Der Starter macht vorher ein `git pull` im Asset-Bridge-Checkout. Liegt der
woanders, setze `ASSETBRIDGE_REPO`, `ASSETBRIDGE_PROJECT` oder
`SOURCETRAIL_APP` in `~/.profile`.

## Was die Landkarte zusaetzlich liest

Neben dem Projektfile, alle optional und alle im Index-Repo:

| Datei | Wirkung, wenn sie fehlt |
|---|---|
| `codemap-notes.json` | Alle Dateien ohne Beschreibung |
| `codemap-stages.json` | Feature-Ansicht faellt auf die berechnete Schichtung zurueck statt auf Import-oben-Export-unten |
| `codemap-layout.json` | Keine verschobenen Knoten, Gruppierung startet auf Verzeichnis |
