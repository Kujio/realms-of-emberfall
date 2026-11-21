#!/usr/bin/env bash
# Downloads the official Godot editor (Linux x86_64) and export templates, verifies them
# against the release's SHA512-SUMS.txt, and installs the templates where Godot looks for them.
# Usage: tools/scripts/install_godot.sh [install_dir] [version]
set -euo pipefail
DEST=${1:-$HOME/godot}
VERSION=${2:-4.7.2}
BASE="https://github.com/godotengine/godot/releases/download/${VERSION}-stable"
EDITOR="Godot_v${VERSION}-stable_linux.x86_64"
TEMPLATES="Godot_v${VERSION}-stable_export_templates.tpz"
TPL_DIR="$HOME/.local/share/godot/export_templates/${VERSION}.stable"

mkdir -p "$DEST"
cd "$DEST"
curl -fsSLO "$BASE/SHA512-SUMS.txt"
curl -fsSLO "$BASE/${EDITOR}.zip"
curl -fsSLO "$BASE/${TEMPLATES}"
grep -E " (${EDITOR}\.zip|${TEMPLATES})$" SHA512-SUMS.txt | sha512sum -c -
unzip -oq "${EDITOR}.zip"
ln -sf "$DEST/$EDITOR" "$DEST/godot"
mkdir -p "$TPL_DIR"
rm -rf tpl && unzip -oq "$TEMPLATES" -d tpl && cp -r tpl/templates/* "$TPL_DIR/"
rm -rf tpl "$TEMPLATES" "${EDITOR}.zip"
"$DEST/godot" --version
