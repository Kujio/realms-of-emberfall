#!/usr/bin/env bash
# Convert model files Godot can import (FBX, OBJ, ...) to GLB, so packs that ship without glTF can
# join the glTF pipeline (validated by tools/validate_content.py). Uses a throwaway Godot project, so
# nothing is imported into the client. Textures next to each source are bound to materials that the
# importer left untextured (see tools/godot_convert/convert.gd).
#
# Usage: tools/convert_to_glb.sh <godot> <out_dir> <source file> [more source files...]
#   Put each source's textures (PNG) in the same folder as the source.
# Example (Quaternius RPG Characters, after unzipping the pack into its own folder):
#   tools/convert_to_glb.sh godot client/assets/models/quaternius_rpg_characters \
#     "RPG Characters - Nov 2020/FBX/Warrior.fbx" "RPG Characters - Nov 2020/FBX/Rogue.fbx"
#   (copy RPG Characters - Nov 2020/Textures/*.png next to the FBX files first)
# Output names are lower-cased source names: Warrior.fbx -> warrior.glb.
set -euo pipefail
if [ $# -lt 3 ]; then
  sed -n '2,13p' "$0" >&2
  exit 2
fi
GODOT=$1
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)

# Two sources must never write the same GLB (Knight.fbx and knight.fbx in different folders).
declare -A SEEN=()
for src in "$@"; do
  [ -f "$src" ] || { echo "no such file: $src" >&2; exit 2; }
  base=$(basename "$src")
  out=$(echo "${base%.*}" | tr '[:upper:]' '[:lower:]').glb
  if [ -n "${SEEN[$out]:-}" ]; then
    echo "both ${SEEN[$out]} and $src would write $out; rename one" >&2
    exit 2
  fi
  SEEN[$out]=$src
done

PROJ=$(mktemp -d)
trap 'rm -rf "$PROJ"' EXIT
printf 'config_version=5\n[application]\nconfig/name="convert"\n' > "$PROJ/project.godot"
cp "$HERE/godot_convert/convert.gd" "$PROJ/"
# Each source gets its own folder with its own textures, so two packs' Body.png never collide.
ARGS=()
i=0
for src in "$@"; do
  i=$((i + 1))
  stage="$PROJ/src$i"
  mkdir -p "$stage"
  base=$(basename "$src")
  cp "$src" "$stage/$base"
  cp "$(dirname "$src")"/*.png "$stage/" 2>/dev/null || true
  out=$(echo "${base%.*}" | tr '[:upper:]' '[:lower:]').glb
  ARGS+=("res://src$i/$base" "$OUT/$out")
done
LOG="$PROJ/import.log"
if ! "$GODOT" --headless --path "$PROJ" --import > "$LOG" 2>&1; then
  echo "Godot import reported errors:" >&2
  cat "$LOG" >&2
fi
if ! "$GODOT" --headless --path "$PROJ" --script res://convert.gd -- "${ARGS[@]}"; then
  echo "conversion failed; Godot's import log follows:" >&2
  cat "$LOG" >&2
  exit 1
fi
