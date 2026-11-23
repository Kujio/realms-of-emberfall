# Art still needed

What the game borrows or fakes today, most visible first. When a real asset arrives, list it in
[ASSETS.md](../ASSETS.md), map it in `content/appearance.json` or `content/models.json`, and strike it here.

## Performance

Characters have no lower-detail meshes, so distant ones only animate less often and stop casting
shadows. Medium holds about 33 FPS in a 230-character crowd on integrated graphics; a mid-range
discrete GPU was not available to measure.

## Audio

Music, ambience, footsteps and most effects are CC0 packs (ASSETS.md). Still missing: distinct cast
and impact sounds per school (one pair of sounds is pitched per school), mob voices (the RPG Sound
Pack has some that are not wired up), ambience for towns, shores and plains (they keep a generated
pad), snow footsteps, and a login theme.

## UI

The theme and icons are generated (game_theme.gd, tools/gen_icons.py). Still missing: unique icons per
spell and item (each category shares one glyph), a display font with character (Godot's default font
is used), textured window frames, and a designed login and character-select backdrop.

## Spell effects

Effects are procedural shapes in the spell's colour (content/effects.json). Still missing: unique
effects for signature spells (only the fire bolts are distinct), area-effect rings sized to the
spell's radius, weapon trails, and textured sprites (fire, smoke, lightning).

## Environments

Every zone prop, hub building, ground clutter and dungeon decoration now uses Polyfork kit parts.
Still placeholder or plain:

| Missing | Stand-in today |
|---|---|
| Ground textures | Vertex-coloured terrain shaded by height and noise |
| Dungeon walls and floors | Shaded blocks in the biome's colours; a Dungeon & Crypt kit is in production at Polyfork |
| Water (bog, marsh, shore, lakes) | None: the ground is drawn at its height; lily pads and reeds suggest it |
| Waystones and zone-line gateways | Primitive shapes with a glow |
| Sky | A procedural gradient per zone; no clouds or skyboxes |
| Snow ground | Plain white; the tundra reads very bright |
| Distinct landmark buildings (the bank hall, the temple) | Stretched village houses and the stone chapel |

## Creatures

Beasts, insects, birds and drakes use Polyfork's animals (CC BY 4.0), which come with idle, walk and
run clips only (plus fly, crawl and a fire breath on some). Attacks, hits and deaths are procedural
(a lunge, a flinch, rolling onto the side). Still missing:

| Missing | Stand-in today |
|---|---|
| Snakes (six vipers, adders, asps and cobras, among them Hissra the Coiled) | The low placeholder shape; Polyfork has no snake |
| The fen basilisk and the bog serpent | The crocodile and the stone centipede |
| Attack, hit and death clips for every animal | Procedural motion |
| A harpy (the cliff harpy) and a hawk (the thunderhawk) | The crypt bat and a tinted owl |
| A heron (the marsh heron) | The herring gull |
| Distinct drakes per element (basalt, storm, cinder) | Four Polyfork dragons, two of them shared |
| Birds that fly above the ground | Birds walk, and run with their flying clip at ground level |

## Mobs and NPCs drawn by a stand-in

| Missing | Stand-in today |
|---|---|
| Ghouls | The Bloated Drowned Seaman, tinted green |
| Frost and storm giants | The Ember-Cracked Troll, tinted |
| Ogres | The Orc Brute |
| Moss and storm golems | The Mud Colossus and Cathedral Statue, tinted |
| Imps and fire sprites | The Magma-Cracked Goblin |
| Female soulbinders, healers and druids | Mirwen and the Forest Witch, shared by many NPCs |
| A few class trainers and epic quest givers | Player outfit models (the same as players of their city's races) |

The old procedural models (skeleton, giant, imp, treant, wraith, rat, wolf, boar, drake; about 0.5 MB)
are no longer used by any mob, NPC or illusion; only the generic humanoid stays in use, for restored
player corpses. The Quaternius RPG Characters and mannequin were removed from the build.

## Player characters (Phase 9)

Every race, gender and armour type has a Polyfork model (CC BY 4.0), but several combinations borrow
another race's or gender's model. `content/appearance.json` lists each race's `outfits`.

| Missing | Stand-in today |
|---|---|
| Female Morrok (orc), all armour types | The male orc models (Warrior, Hunter, Berserker) |
| Female Tinkerling (gnome), all armour types | Elf Mage (cloth, leather) and Human Knight (chain, plate), scaled to gnome height |
| Female Hollowfolk (halfling), all armour types | Human Assassin (cloth, leather) and Human Knight (chain, plate), scaled to halfling height |
| Female Deepkin (dwarf) leather, chain and plate | The Dwarf Priest in every armour type |
| Female Highlander and Frostmane cloth | Elf Mage (Highlander) and Human Assassin (Frostmane) |
| Female Vaelith and Duskborn leather, chain and plate | Human Assassin and Human Knight, slightly slimmed |
| Male Vaelith and Duskborn cloth | Human Mage, slightly slimmed (the hood hides the ears) |
| Male Highlander leather | Human Mage (coat and hood) |
| Male Hollowfolk plate | Gnome Warrior scaled up |
| Male Deepkin cloth | Dwarf Berserker |
| Frostmane (both genders) | Human models scaled up 12% with pale skin; no race of its own |
| Duskborn (both genders) | Elf and human models with a grey skin tint |
| Male human plate | Human Warrior (chainmail); no plate-armoured man |
| Gnome and Dwarf Knight heads | The Dwarf Knight has no scalp under its helmet, so its helmet always shows |

Polyfork's creator can make these as variations (a gender swap, other outfits), but downloading
variations needs a paid Polyfork plan. Any CC0 or CC-BY rigged humanoid with a Mixamo-named skeleton
retargets the same way (`tools/retarget_to_glb.sh`).

Armour tiers are shown only as a tint over the outfit (`appearance.json` → `armor_tiers`); there are
no distinct meshes per tier, and gloves, boots, shoulders and the like do not change the look.
