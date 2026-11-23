"""Art for the zone props (Phase 9): which models draw each prop, per biome.

tools/world_props.py turns each entry into a prop scene. The scene keeps the prop's collision (so the
server's geometry never changes) and holds one child per variant; at run time the prop keeps one
variant, picked from its position (client/scripts/world/prop_variants.gd).

SKINS maps "prop" (every zone) or "prop__biome" (zones of that biome or hub city: the theme's
"biome") to a list of variants. A variant is a list of parts, each
    (model, scale, (x, y, z), yaw degrees)
where model names a file in client/assets/env (the Polyfork kits, CC BY 4.0, see its LICENSE.txt) and
scale is a number or (sx, sy, sz); or a dict {"parts": [...], "tint": "#rrggbb"}. Scales fit each
model over the collision footprint of the prop it draws.

DRAW gives a prop's draw distance in metres (0 = always drawn); the graphics settings scale it.
"""
from __future__ import annotations


def one(model: str, scale, yaw: float = 0.0, pos=(0, 0, 0)) -> list:
    return [(model, scale, pos, yaw)]


def tinted(parts: list, tint: str) -> dict:
    return {"parts": parts, "tint": tint}


def row(model: str, scale, n: int, step: float, axis: str = "x", yaw: float = 0.0) -> list:
    """n copies along an axis, centred (walls, fences, piers)."""
    start = -step * (n - 1) / 2
    return [(model, scale, (start + i * step, 0, 0) if axis == "x" else (0, 0, start + i * step), yaw) for i in range(n)]


ASH = "#8a8580"
FROST = "#dfe8f2"
RED = "#d08a60"
MOSS = "#a3b08a"

OAKS = [one("broadleaf_oak", 1.3), one("maple_tree", 1.35), one("birch_tree", 1.3)]
PINES = [one("tall_pine_tree", 1.1), one("young_pine", 2.6)]
SPRUCE = [one("snow_laden_spruce", 1.6), one("snow_laden_spruce", 1.35, 40)]
DEAD = [one("dead_tree", 1.25), one("bare_mountain_rowan", 1.2)]
SWAMP_DEAD = [one("twisted_dead_oak", 0.85), one("dead_tree", 1.25)]
ASH_DEAD = [tinted(one("dead_tree", 1.25), ASH), tinted(one("twisted_dead_oak", 0.85), ASH)]
ROCKS = [one("medium_rock", 3.3), one("large_boulder", 1.5), one("boulder_field_cluster", 1.35)]
BOULDERS = [one("rock_outcrop", 2.6), one("large_coastal_boulder", 2.7)]

SKINS: dict[str, list] = {
    # ------------------------------------------------------------------ wilds
    "tree_oak": OAKS,
    "tree_oak__tundra": [one("windblown_larch", 1.0)],
    "tree_oak__peaks": [one("windblown_larch", 1.0)],
    "tree_oak__bog": SWAMP_DEAD,
    "tree_oak__marsh": SWAMP_DEAD,
    "tree_oak__ashland": ASH_DEAD,
    "tree_oak__wastes": ASH_DEAD,
    "tree_oak__badlands": [one("dead_tree", 1.3)],
    "tree_pine": PINES,
    "tree_pine__tundra": SPRUCE,
    "tree_pine__peaks": SPRUCE,
    "tree_pine__mountain": SPRUCE,
    "tree_dead": DEAD,
    "tree_dead__bog": SWAMP_DEAD,
    "tree_dead__marsh": SWAMP_DEAD,
    "tree_dead__ashland": ASH_DEAD,
    "tree_dead__wastes": ASH_DEAD,
    "tree_dead__tundra": [tinted(one("bare_mountain_rowan", 1.2), FROST)],
    "tree_dead__peaks": [tinted(one("bare_mountain_rowan", 1.2), FROST)],
    "rock": ROCKS,
    "rock__forest": ROCKS[:2] + [one("fallen_log", 1.1, 30), one("tree_stump", 3.0)],
    "rock__pinewood": ROCKS[:2] + [one("fallen_log", 1.1, 60), one("tree_stump", 3.0)],
    "rock__badlands": [tinted(one("rock_spire", 1.6), RED), tinted(one("rock_outcrop", 1.3), RED)],
    "rock__wastes": [one("obsidian_fang_rock", 1.4), one("smouldering_ash_heap", 1.7)],
    "rock__ashland": [tinted(one("medium_rock", 3.3), ASH), one("smouldering_ash_heap", 1.7)],
    "rock__tundra": [tinted(one("boulder_field_cluster", 1.35), FROST), one("stone_cairn", 1.6)],
    "rock__peaks": [tinted(one("scree_heap", 1.35), FROST), tinted(one("boulder_field_cluster", 1.35), FROST)],
    "rock__shore": [one("large_coastal_boulder", 1.4), one("driftwood_log", 1.3, 70)],
    "rock__steppe": [one("stone_cairn", 1.8), one("medium_rock", 3.3)],
    "rock__bog": [tinted(one("medium_rock", 3.3), MOSS), one("fallen_log", 1.1, 20), one("exposed_tree_roots", 1.3)],
    "rock__marsh": [tinted(one("medium_rock", 3.3), MOSS), one("fallen_log", 1.1, 80)],
    "boulder": BOULDERS,
    "boulder__badlands": [tinted(one("rock_outcrop", 2.6), RED), tinted(one("rock_spire", 2.5), RED)],
    "boulder__shore": [one("sea_stack", 2.0), one("large_coastal_boulder", 2.7)],
    "boulder__wastes": [one("obsidian_fang_rock", 2.5), one("geode_boulder", 3.5)],
    "boulder__tundra": [tinted(one("rock_outcrop", 2.6), FROST)],
    "boulder__peaks": [tinted(one("rock_outcrop", 2.6), FROST)],
    "boulder__ashland": [tinted(one("rock_outcrop", 2.6), ASH)],
    "reeds": [one("reed_bed", 1.2), [("cattail_reed", 1.5, (-0.4, 0, 0), 0), ("cattail_reed", 1.3, (0.3, 0, 0.3), 40),
                                     ("cattail_reed", 1.6, (0.1, 0, -0.4), 90)]],
    "reeds__shore": [one("marram_grass_tuft", 1.6)],
    "reeds__tundra": [one("dwarf_pine_mat", 1.0)],
    "mushroom": [one("toadstool", 8.0), one("toadstool", 6.5, 50)],
    "crystal": [one("lair_crystal_column", 1.6)],
    "crystal__tundra": [tinted(one("lair_crystal_column", 1.6), "#cfe4ff")],
    "ruin_pillar": [one("broken_stone_column", 1.0)],
    "ruin_wall": [one("ruined_lair_wall", (2.5, 0.75, 1.1))],
    "tent": [one("canvas_wall_tent", (1.0, 1.0, 1.25))],
    "cave_mouth": [one("cave_overhang_arch", 0.55)],
    "brazier": [one("flame_brazier", 1.8)],
    "brazier__lair": [one("dragonfire_brazier", 1.2)],
    "brazier__citadel": [one("dragonfire_brazier", 1.2)],
    "dock": [row("pier_deck_section", (1.5, 1, 1), 7, 3.0, "z")],
    "boat": [one("small_fishing_boat", 1.75)],
    # ------------------------------------------------------------------ towns (Emberhold is "town",
    # Thornwood "grove", Cragmouth "mountain")
    "well": [one("village_well", 1.4)],
    "lamp_post": [one("lamp_post", 1.8)],
    "lamp_post__grove": [one("lantern_post", 1.7)],
    "crates": [[("wooden_crate", 1.6, (0, 0, 0), 0), ("cargo_crate", 1.0, (0.2, 0.98, 0.1), 20)], one("wooden_barrel", 1.6)],
    "fence": [row("wooden_fence_section", 1.0, 4, 2.0)],
    "fence__grove": [row("palisade_fence_section", (1.11, 1.0, 1.0), 3, 2.67)],
    "fence__mountain": [row("low_stone_wall", 1.0, 4, 2.0)],
    "market_stall": [one("market_stall", 2.0)],
    "forge": [[("blacksmith_forge", 1.6, (0, 0, 0), 0), ("anvil", 1.6, (3.2, 0, 2.6), 30)]],
    "house_small": [one("thatched_cottage", 1.35), one("timber_barn", 1.2, 90)],
    "house_small__grove": [one("log_cabin", 1.15, 90)],
    "house_small__mountain": [one("mountain_refuge_hut", (1.35, 1.35, 1.0))],
    "house_large": [one("two_story_house", 1.6, 90), one("village_tavern", 0.93)],
    "house_large__grove": [one("log_cabin", 1.6, 90)],
    "house_large__mountain": [one("alpine_chalet", 1.2)],
    "wall_segment": [row("wall_module", (2.0, 2.0, 6.25), 5, 4.0)],
    "wall_segment__grove": [row("palisade_fence_section", (1.67, 4.3, 8.7), 5, 4.0)],
    "wall_tower": [one("watchtower", 1.45)],
    "gate_arch": [one("village_gatehouse", (2.25, 1.4, 1.5))],
    "bank_hall": [one("two_story_house", (2.1, 1.5, 2.1), 90)],
    "bank_hall__grove": [one("log_cabin", (2.3, 1.8, 2.0), 90)],
    "bank_hall__mountain": [one("alpine_chalet", 1.6)],
    "temple": [one("stone_chapel", (3.1, 1.5, 2.8))],
}

# Metres: clutter fades first, trees later, buildings and landmarks never.
DRAW: dict[str, float] = {
    "rock": 220.0, "reeds": 140.0, "mushroom": 180.0, "crystal": 260.0, "tree_oak": 380.0, "tree_pine": 380.0,
    "tree_dead": 320.0, "boulder": 420.0, "ruin_pillar": 300.0, "crates": 160.0, "fence": 220.0, "lamp_post": 200.0,
    "tent": 260.0,
}
