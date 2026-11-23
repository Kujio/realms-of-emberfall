# How to add a character or mob model

Every character, creature and held item is drawn from data. Adding or swapping a model is a
content edit plus dropping files into the client. You never change code. This page covers the
file format, where files go, the JSON entries, the animation map, attachment points, parts and
tints, and how to check your work.

## The pieces

| File | What it holds |
|---|---|
| `client/assets/models/<pack>/*.glb` | The model files (glTF 2.0 binary, or `.gltf` with its files beside it) |
| `content/models.json` → `models` | One entry per model id: scene path, scale, nameplate height, collision radius, facing, placeholder, parts, attachment points, default tints |
| `content/models.json` → `animation_maps` | Per rig or pack: standard state → clip name, plus ground speeds |
| `content/models.json` → `item_visuals` | Meshes that attach to a slot (weapons, shields, a bow on the back) |
| `content/appearance.json` | Which model each race and gender uses, with tints and parts; what each class wears and holds by default |
| `content/items.json` → `visual` | The item visual an item shows when held |
| `content/mobs.json`, `content/npcs.json` → `model` | The model id of each mob type and NPC |
| `client/data/visuals.json` | Generated client copy of the above. Never edit it by hand |

After any edit:

```sh
python tools/sync_client_content.py   # regenerate client/data/visuals.json (CI fails if stale)
python tools/validate_content.py      # opens every GLB and checks clips, bones, parts and materials
```

The server loads `models.json` and `appearance.json` too. It fails fast on unknown model ids, and it
uses `collision_radius` for mob melee reach.

## 1. The model file

- **Format:** glTF 2.0, preferably `.glb`. Godot imports it at export time; commit the `.glb` and the
  `.glb.import` file Godot creates beside it (not the `*_albedo.png` textures Godot extracts on
  import; they are git-ignored and regenerated).
- **FBX or OBJ only?** Convert with Godot itself, so the validator can check the result:
  `tools/convert_to_glb.sh <godot> <out_dir> <files...>` (textures next to the sources are bound to
  materials the importer left untextured, then embedded). Godot's FBX importer renames bones with
  dots (`Foot.L` becomes `Foot_L`), so read bone names from the converted file.
- **Orientation and units:** Y up, 1 unit = 1 m, feet at y = 0. Most packs face +Z; set
  `"yaw_offset_deg": 180` for those (the client's forward is -Z). A model that already faces -Z uses 0.
- **Skeleton:** one skinned skeleton. Bone names can be anything, because the JSON maps them.
- **Animations:** the clips go in the same file (Godot imports them into an `AnimationPlayer`).
  Clip names can be anything; the animation map translates them. Loop flags in the file are
  ignored: the client loops idle, walk, run, cast, sit, swim and fall, and plays everything else
  once, unless the map overrides it with `"loop"`. Godot drops a `_Loop`/`-Loop`/`_Cycle` suffix
  from clip names on import (`Walk_Loop` becomes `Walk`). A map may use either spelling, and the
  validator and the client both accept it.
- **Parts:** optional meshes the JSON can show or hide (hair styles, helmets, a robe). Each one must be its
  own mesh node with a unique name.
- **Materials:** name them. Tints refer to material names (`Skin`, `Cloth`, `Hair`...), and a
  white or neutral base colour tints best.

Put a pack under `client/assets/models/<pack_name>/` (lower case, no spaces), keeping its license
file beside it, and add the pack to `ASSETS.md`.

To list a file's clips, bones, mesh nodes and materials:

```sh
python tools/inspect_model.py client/assets/models/emberfall_basic/humanoid_male.glb
```

## 2. The model entry (`content/models.json` → `models`)

```json
{
  "id": "humanoid_male",
  "scene": "res://assets/models/emberfall_basic/humanoid_male.glb",
  "scale": 1.0,
  "nameplate_height": 2.15,
  "collision_radius": 0.5,
  "yaw_offset_deg": 180,
  "animations": "emberfall_humanoid",
  "placeholder": {"shape": "upright", "color": "#c9a27e"},
  "parts": {"hair": ["HairShort", "HairLong"], "head": ["Helm", "Hood"]},
  "default_parts": {"hair": "HairShort"},
  "tints": {"Cloth": "#5b4a3a"},
  "attachments": {
    "main_hand": {"bone": "hand_R", "position": [0.0, -0.06, 0.0]},
    "off_hand": {"bone": "forearm_L", "position": [0.05, -0.1, 0.0], "rotation_deg": [0, 0, 0]},
    "back": "chest"
  }
}
```

| Field | Meaning |
|---|---|
| `id` | `[a-z0-9_]`, referenced by mobs, NPCs and `appearance.json` |
| `scene` | `res://` path to the `.glb`/`.gltf`. `""` = no model yet: the placeholder shape is drawn |
| `scale` | Multiplies the entity's own scale (a mob's `scale` in `mobs.json`) |
| `nameplate_height` | Name tag height in metres at entity scale 1 |
| `collision_radius` | Body radius in metres at entity scale 1. Clicking uses it, and so does the server: a mob's melee reach is `melee_range + collision_radius × scale` |
| `yaw_offset_deg` | Turns the model to the client's forward (-Z): 180 for +Z-facing packs |
| `animations` | Animation map id (required when `scene` is set) |
| `placeholder` | `shape` (`upright` or `low`) and `color`, used when the scene is empty or fails to load |
| `parts` | Groups of alternative mesh nodes. In each group at most one is visible; nodes in no group are always visible |
| `default_parts` | The default choice per group (`""` = none) |
| `tints` | Material name (or `"*"` for every material not named) → `#rrggbb` |
| `attachments` | Attachment point → `{bone, position, rotation_deg}` (or just the bone name). Standard points: `main_hand`, `off_hand`, `back`, `head`; you may add others (the starter models add `off_hand_held` for an off-hand weapon held in the hand rather than strapped to the forearm) |

Several model ids can share one scene with different tints, parts and scale. The starter set does
this: `skeleton`, `giant`, `golem`, `imp`, `treant` and `wraith` all reuse the humanoid rig, and
`rat`, `wolf`, `boar` and `drake` the quadruped rig.

## 3. The animation map (`content/models.json` → `animation_maps`)

The client's animation state machine (`client/scripts/world/anim_state.gd`) asks for **standard
states**. The map translates them to clip names in your file:

```json
{
  "id": "my_pack_humanoid",
  "clips": {
    "idle": {"clip": "Idle_Loop"},
    "combat_idle": {"clip": "Sword_Idle"},
    "walk": {"clip": "Walk_Loop", "ground_speed": 1.6},
    "run": {"clip": "Jog_Fwd_Loop", "ground_speed": 4.8},
    "attack1": {"clip": "Sword_Attack"},
    "attack2": {"clip": "Punch_Cross"},
    "cast": {"clip": "Spell_Simple_Idle_Loop"},
    "hit": {"clip": "Hit_Chest"},
    "death": {"clip": "Death01"},
    "sit": {"clip": "Sitting_Idle_Loop", "loop": true}
  },
  "walk_run_threshold": 3.0,
  "blend": 0.15
}
```

The clip names above are examples. Use the names your file actually contains; the validator rejects
any that do not exist.

**Standard states and what drives them** (all from server state; the client never invents gameplay):

| State | When |
|---|---|
| `idle` / `combat_idle` | Standing; `combat_idle` while the server's in-combat flag is set |
| `walk`, `run` | Ground speed from snapshots (players: from prediction). `walk_run_threshold` (m/s) splits them, with hysteresis |
| `walk_back` | Moving against your facing |
| `attack1`, `attack2` | A melee swing (`CombatEvent` hit/miss/dodge/parry by this entity). Swings alternate; abilities use `attack2` |
| `cast` | Between `CastStart` and `CastEnd`, and briefly for instant spells |
| `hit` | Taking damage while standing (never interrupts a swing, a cast or movement) |
| `death` | The dead flag, or a corpse. Arriving after the death shows the final pose |
| `sit` | The sitting flag |
| `swim`, `fall` | Reserved (no swimming or falling yet) |

**Missing clips degrade gracefully.** When a state has no clip, the client tries these in order:

| State | Falls back to |
|---|---|
| `combat_idle` | `idle` |
| `walk` | `run`, `idle` |
| `walk_back` | `walk` (played in reverse), `run`, `idle` |
| `run` | `walk`, `idle` |
| `attack1` ↔ `attack2` | each other |
| `cast`, `fall` | `idle` |
| `swim` | `run`, `walk`, `idle` |
| `hit` | skipped (keep doing whatever you were doing) |
| `death`, `sit` | a procedural pose: the body lies down or squats |

A model with no animations at all, and every placeholder, uses that procedural motion for
everything: a bob while moving, a lunge for swings, a flinch, lying down.

**Avoiding foot sliding:** set `ground_speed` on `walk` and `run` to the speed (m/s) at which the
clip's feet match the ground at 1x. The client plays locomotion at `actual speed / (ground_speed ×
model size)`, clamped to 0.4x-2.5x, so a giant's long strides and a rat's quick steps both stay planted.
Measure it rather than guessing. In a scratch Godot project containing the GLB, run:

```sh
godot --headless --path <scratch project> --script res://measure.gd -- res://model.glb <left foot bone> <right foot bone> Walk Run
```

(`measure.gd` is `tools/godot_convert/measure.gd`.) It prints the bone height span (for
`nameplate_height`) and two estimates per clip: the planted-foot speed and a stride-range speed.
For walks they agree. For runs with a flight phase, the truth lies between them, nearer the
stride-range figure scaled up by about 1.4.

## 4. Characters: races, genders and classes (`content/appearance.json`)

```json
"races": [
  {"race": "deepkin",
   "female": {"model": "humanoid_female", "scale": [1.12, 0.8, 1.12], "tints": {"Skin": "#c99a78"}, "parts": {"hair": "HairTopknot"}},
   "male":   {"model": "humanoid_male",   "scale": [1.15, 0.78, 1.15], "parts": {"beard": "Beard"}}}
],
"classes": [
  {"class": "wizard", "tints": {"Cloth": "#2d3f8f", "Trim": "#d9b44a"}, "parts": {"legs": "Robe", "head": "WizardHat"},
   "held": {"main_hand": "training_staff"}}
]
```

- Every race needs both genders (`female`, `male`), and the server checks this at startup.
- `scale` is a number or `[x, y, z]` (0.3-3), so one rig covers stocky and slender races.
- Layers apply in order: the model's defaults, then race and gender, then class. A later layer
  wins for the same material or part group. Races usually tint `Skin` and `Hair` and pick hair,
  beards and ears; classes tint `Cloth`, `Pants` and `Trim` and pick headgear, robes and capes.
  That is how 8 races × 14 classes share two body models.
- `held` lists the items a class carries until real equipment arrives (Phase 5), by slot. Each item
  needs a `visual` whose slot matches.

### Outfit models (Phase 9)

Models that come dressed (the Polyfork characters) set `"class_style": false` in their model entry,
so class tints and parts do not apply. A race and gender then lists an outfit per armour type:

```json
"male": {"model": "polyfork_human_warrior", "scale": 1.0, "outfits": {
    "cloth": {"model": "polyfork_human_priest"},
    "leather": {"model": "polyfork_human_mage"},
    "chain": {"model": "polyfork_human_warrior"},
    "plate": {"model": "polyfork_human_warrior"}
}}
```

- The worn chest piece's `armor` type picks the outfit; with no chest piece, the class's own armour
  type does (`classes.json`). An outfit's fields replace the gender's (`model`, `scale`, `tints`,
  `parts`); fields it leaves out come from the gender.
- `armor_tiers` (in `appearance.json`) tints the outfit by the chest piece's `req_level` (and `epic`
  rarity). The tint applies to mesh nodes starting with `prefix`, except those in `keep` (hair, beards).
- A model's `helm` part group shows only while a head item is worn; its `held` group (the model's
  own weapons) stays hidden so equipment shows instead.
- `stride` scales the animation map's ground speeds for the model's leg length, and an attachment
  point's `scale` shrinks held items for a small model built at its true size.
- Polyfork models arrive with their own clips. `tools/retarget_to_glb.sh` replaces them with the
  Quaternius clips; see the header of `tools/godot_convert/retarget.gd`, then measure with
  `tools/godot_convert/measure.gd`.
- Check every race and armour type without a server: `EmberfallClient.exe -- --lineup DIR` saves
  lineups, armour tiers and close-ups of held items into DIR.

## 5. Held items and attachment points

```json
"item_visuals": [
  {"id": "sword_basic", "scene": "res://assets/models/emberfall_basic/sword.glb", "slot": "main_hand"},
  {"id": "dagger_offhand", "scene": "res://assets/models/emberfall_basic/dagger.glb", "slot": "off_hand", "attach_point": "off_hand_held"},
  {"id": "staff_basic", "scene": "res://assets/models/emberfall_basic/staff.glb", "slot": "main_hand", "position": [0, 0, 0.03]}
]
```

- `slot` is `main_hand`, `off_hand`, `back` or `head`. `attach_point` (default: the slot) names
  the model attachment point to use.
- Optional `position`, `rotation_deg` and `scale` adjust the mesh relative to the attachment point.
  The model's attachment entry adjusts the point relative to the bone.
- **Authoring convention:** put the grip at the origin, with blades pointing along +Z (forward from
  a hanging hand). Shields face +X (outward from the left forearm); staffs and bows run along Y.
- An item names its visual in `items.json` (`"visual": "sword_basic"`).
- If a model lacks the attachment point (or the bone), the item is skipped with a warning.
  Placeholders hold items at fixed points, so the pipeline is visible even without art.

## 6. Mobs and NPCs

Which model each mob (by name), pet and NPC (by id) uses is set in `tools/model_picks.py`; rerun the
generators (`gen_mobs.py`, `gen_classes.py`, `gen_world.py`, `gen_quests.py`, `gen_items.py`) after
editing it (they write `mobs.json`, `pets.json` and `npcs.json`; do not edit those by hand). A
model's `scale` sets its height before the mob's own `scale` multiplies it. A mob type with no art yet can point at a model entry whose
`scene` is `""`: it draws the placeholder shape until a file is added.

## 7. Checking your work

1. `python tools/sync_client_content.py && python tools/validate_content.py`
2. `godot --headless --path client --import`, then the client tests:
   `godot --headless --path client --script res://tests/test_runner.gd`
3. Look at it: the character create screen shows a turning preview for every race, gender and
   class, and in the world `--autotest ... --expect-models --expect-anims idle,run,attack1` reports
   which states played.

## Swapping in a third-party pack (worked example)

This is how the Quaternius packs listed in `ASSETS.md` went in:

1. The Universal Animation Library ships a Godot GLB (a mannequin with 46 clips). It was copied
   into `client/assets/models/quaternius_animation_library/` with its license. An animation map
   `quaternius_ual` names its clips (`Idle_Loop`, `Jog_Fwd_Loop`, `Sword_Attack`, `Death01`, ...),
   and the `golem` model entry now points at it with a stone tint. No code changed.
2. The RPG Characters ship FBX, so `tools/convert_to_glb.sh` converted them. Their clips are named
   `CharacterArmature|Idle` and so on, and four animation maps cover the warrior, rogue, ranger and
   caster clip sets. New model ids `brigand`, `brigand_archer`, `cultist`, `hollow_knight`,
   `trainer_cleric` and `trainer_wizard` point at them, and the mobs and NPCs name those ids.
3. Ground speeds and heights were measured with `measure.gd`. Facing was checked from the geometry:
   the converted RPG characters face -Z (`yaw_offset_deg: 0`), and the mannequin faces +Z (180).

In general, to use a CC0 humanoid pack in place of the starter body:

1. Download the pack yourself from its official page, check its license allows use in a
   distributed game, and unzip its glTF/GLB files into `client/assets/models/<pack>/` with the
   license text. Record the pack in `ASSETS.md`.
2. List the file's clips, bones, meshes and materials with `python tools/inspect_model.py <file>`.
3. Add an `animation_maps` entry that maps the standard states to the pack's clip names, with
   `ground_speed` for walk and run.
4. Add a `models` entry with the scene path, `yaw_offset_deg` (usually 180), the bone names for
   `main_hand`, `off_hand`, `back` and `head`, any part groups, and tints on the pack's material
   names.
5. Point a race and gender at it in `appearance.json` (or a mob in `mobs.json`).
6. Run step 7 above. Nothing else changes.
