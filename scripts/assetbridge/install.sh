#!/bin/sh
# Build this fork and install it into ~/.local, menu entry and launcher included.
# Run it again after a git pull; it is an update as much as an install.
#
#   ./scripts/assetbridge/install.sh              build, then install
#   ./scripts/assetbridge/install.sh --no-build   install what is already built
#
# Override any of these in the environment: BUILD_DIR PREFIX BINDIR PRESET
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
: "${PRESET:=system-release}"
: "${BUILD_DIR:=$(dirname "$SRC")/build/$PRESET}"
: "${PREFIX:=$HOME/.local/opt/Sourcetrail}"
: "${BINDIR:=$HOME/.local/bin}"

if [ "$1" != "--no-build" ]; then
    # The Rust indexer is a separate cargo crate — CMake does not build it, and
    # without it the project indexes to an empty database without complaining.
    echo "==> cargo build --release (rust_indexer)"
    cargo build --release --manifest-path "$SRC/rust_indexer/Cargo.toml"

    # The three language packages the preset turns on are all off here: this fork
    # indexes Rust, Python and TypeScript, so libclang, the JDK and Maven are dead
    # weight and pull in half a gigabyte of build dependencies.
    echo "==> cmake --preset $PRESET"
    cd "$SRC"
    cmake --preset "$PRESET" \
        -DBUILD_CXX_LANGUAGE_PACKAGE=OFF \
        -DBUILD_JAVA_LANGUAGE_PACKAGE=OFF \
        -DBUILD_UNIT_TESTS_PACKAGE=OFF
    cmake --build "$BUILD_DIR"
fi

[ -x "$BUILD_DIR/app/Sourcetrail" ] || {
    echo "install: no build in $BUILD_DIR — drop --no-build" >&2
    exit 1
}

echo "==> installing to $PREFIX/app"
mkdir -p "$PREFIX/app" "$BINDIR"
for item in "$BUILD_DIR"/app/*; do
    name=$(basename "$item")
    # user/ holds the settings of an existing installation, never overwrite it
    [ "$name" = user ] && [ -e "$PREFIX/app/user" ] && continue
    # A running Sourcetrail holds its own binary open: writing over it fails with
    # ETXTBSY. Landing beside it and renaming into place does not.
    cp -r "$item" "$PREFIX/app/$name.new"
    rm -rf "$PREFIX/app/$name"
    mv "$PREFIX/app/$name.new" "$PREFIX/app/$name"
done

echo "==> launcher $BINDIR/sourcetrail-assetbridge"
cp "$HERE/sourcetrail-assetbridge" "$BINDIR/sourcetrail-assetbridge"
chmod +x "$BINDIR/sourcetrail-assetbridge"

# A menu entry runs without a login shell, so ~/.local/bin is not on its PATH.
# The Exec line therefore has to be absolute.
apps="$HOME/.local/share/applications"
icons="$HOME/.local/share/icons/hicolor"
echo "==> menu entry $apps/sourcetrail.desktop"
mkdir -p "$apps"
sed "s|@EXEC@|$BINDIR/sourcetrail-assetbridge|" "$HERE/sourcetrail.desktop" \
    > "$apps/sourcetrail.desktop"

# The logo is 1024 square and hicolor's index.theme stops at 512, so a file
# dropped into a 1024x1024 directory is never looked up. Scale it if there is
# anything to scale with, otherwise fall back to pixmaps, which is searched
# whatever the size.
logo="$SRC/src/resources/icon/logo_1024_1024.png"
if command -v magick >/dev/null || command -v convert >/dev/null; then
    resize=$(command -v magick || command -v convert)
    mkdir -p "$icons/256x256/apps"
    "$resize" "$logo" -resize 256x256 "$icons/256x256/apps/sourcetrail.png"
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -qf "$icons" 2>/dev/null || true
else
    mkdir -p "$HOME/.local/share/pixmaps"
    cp "$logo" "$HOME/.local/share/pixmaps/sourcetrail.png"
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" || true

echo
echo "Fertig. Starten: sourcetrail-assetbridge"
case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo "Achtung: $BINDIR liegt nicht im PATH." ;;
esac
echo "Das Projektfile enthaelt absolute Pfade — siehe INSTALL.md, Abschnitt 'Zweiter Rechner'."
