#!/usr/bin/env python3
"""List what a glTF/GLB file contains: animation clips, skeleton bones, mesh nodes and materials.

Use it when writing an animation map, attachment points, part groups or tints for a new model
(see docs/MODELS.md). Standard library only.

Usage: python tools/inspect_model.py client/assets/models/<pack>/<file>.glb [more files...]
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_checks import godot_clip_name, read_gltf  # noqa: E402


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    status = 0
    for arg in sys.argv[1:]:
        try:
            info = read_gltf(Path(arg))
        except (OSError, ValueError) as e:
            print(f"{arg}: cannot read: {e}", file=sys.stderr)
            status = 1
            continue
        print(arg)
        renamed = {a: godot_clip_name(a) for a in info.animations if godot_clip_name(a) != a}
        for title, names in (("clips", info.animations), ("bones", info.joints), ("mesh nodes", info.meshes),
                             ("materials", info.materials)):
            print(f"  {title} ({len(names)}): {', '.join(sorted(names)) or '-'}")
        if renamed:
            print(f"  note: Godot imports {len(renamed)} clip(s) without the loop suffix (and loops them): "
                  + ", ".join(f"{k} -> {v}" for k, v in sorted(renamed.items())))
    return status


if __name__ == "__main__":
    sys.exit(main())
