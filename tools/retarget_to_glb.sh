#!/usr/bin/env bash
# Gives characters on one humanoid rig the animation clips of another, and writes them as GLB (Phase 9:
# Polyfork characters get the CC0 Quaternius Universal Animation Library clips). Runs Godot headless in a
# throwaway project, so nothing is imported into the client. See tools/godot_convert/retarget.gd.
#
# Usage: tools/retarget_to_glb.sh <godot> <spec.json> <out_dir> <source glbs...> -- <target glbs...>
#   The spec names its sources by file name; each listed source file is copied in under that name.
#   Output names are the targets' names (elf-hunter-base.glb -> <out_dir>/elf-hunter-base.glb).
set -euo pipefail
if [ $# -lt 5 ]; then
  sed -n '2,8p' "$0" >&2
  exit 2
fi
GODOT=$1
SPEC=$2
OUT=$(mkdir -p "$3" && cd "$3" && pwd)
shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ=$(mktemp -d)
trap 'rm -rf "$PROJ"' EXIT
printf 'config_version=5\n[application]\nconfig/name="retarget"\n' > "$PROJ/project.godot"
cp "$HERE/godot_convert/retarget.gd" "$PROJ/"
mkdir -p "$PROJ/src" "$PROJ/tgt"
while [ $# -gt 0 ] && [ "$1" != "--" ]; do cp "$1" "$PROJ/src/"; shift; done
[ "${1:-}" = "--" ] || { echo "missing -- before the targets" >&2; exit 2; }
shift
TARGETS=()
for t in "$@"; do cp "$t" "$PROJ/tgt/"; TARGETS+=("$(basename "$t")"); done
cp "$SPEC" "$PROJ/spec.json"
"$GODOT" --headless --path "$PROJ" --import > "$PROJ/import.log" 2>&1 || { cat "$PROJ/import.log" >&2; exit 1; }
fail=0
for t in "${TARGETS[@]}"; do
  "$GODOT" --headless --path "$PROJ" --script res://retarget.gd -- "res://tgt/$t" "$OUT/$t" "$PROJ/spec.json" || fail=1
done
exit $fail
