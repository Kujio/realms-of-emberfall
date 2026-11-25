## Rebindable keys (Phase 8): the actions, their default keys, and the rules for changing them. Pure
## data and functions (no scene tree), so the test runner checks them directly; the Settings autoload
## applies the result to Godot's InputMap and saves it.
class_name Keybinds
extends RefCounted

## [action, label, default physical keys (the first is the one players rebind)]
const ACTIONS: Array = [
	["move_forward", "Move forward", [KEY_W, KEY_UP]],
	["move_back", "Move back", [KEY_S, KEY_DOWN]],
	["turn_left", "Turn left (strafe while mouselooking)", [KEY_A, KEY_LEFT]],
	["turn_right", "Turn right (strafe while mouselooking)", [KEY_D, KEY_RIGHT]],
	["strafe_left", "Strafe left", [KEY_Q]],
	["strafe_right", "Strafe right", [KEY_E]],
	["target_next", "Next target (Shift: previous)", [KEY_TAB]],
	["target_self", "Target yourself", [KEY_F1]],
	["group_1", "Target group member 1", [KEY_F2]],
	["group_2", "Target group member 2", [KEY_F3]],
	["group_3", "Target group member 3", [KEY_F4]],
	["group_4", "Target group member 4", [KEY_F5]],
	["group_5", "Target group member 5", [KEY_F6]],
	["auto_attack", "Auto-attack", [KEY_R]],
	["consider", "Consider target", [KEY_C]],
	["interact", "Talk / interact", [KEY_T]],
	["loot", "Loot", [KEY_L]],
	["sit", "Sit / stand", [KEY_X]],
	["inventory", "Inventory", [KEY_I]],
	["abilities", "Abilities", [KEY_K]],
	["skills", "Skills", [KEY_J]],
	["spellbook", "Spellbook", [KEY_B]],
	["group_window", "Group", [KEY_P]],
	["raid_window", "Raid", [KEY_Y]],
	["guild_window", "Guild", [KEY_G]],
	["friends_window", "Friends and ignore", [KEY_O]],
	["map", "Map", [KEY_M]],
	["journal", "Quest journal", [KEY_U]],
	["hotbar_1", "Hotbar 1", [KEY_1]],
	["hotbar_2", "Hotbar 2", [KEY_2]],
	["hotbar_3", "Hotbar 3", [KEY_3]],
	["hotbar_4", "Hotbar 4", [KEY_4]],
	["hotbar_5", "Hotbar 5", [KEY_5]],
	["hotbar_6", "Hotbar 6", [KEY_6]],
	["hotbar_7", "Hotbar 7", [KEY_7]],
	["hotbar_8", "Hotbar 8", [KEY_8]],
	["hotbar_9", "Hotbar 9", [KEY_9]],
	["hotbar_10", "Hotbar 10", [KEY_0]],
]

## Keys that are never rebindable: they open chat, close windows and leave menus.
const RESERVED: Array[int] = [KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER, KEY_SLASH]


static func action_ids() -> Array[String]:
	var out: Array[String] = []
	for a: Array in ACTIONS:
		out.append(str(a[0]))
	return out


static func label_of(action: String) -> String:
	for a: Array in ACTIONS:
		if str(a[0]) == action:
			return str(a[1])
	return action


## action -> Array[int] of physical keycodes.
static func defaults() -> Dictionary:
	var out: Dictionary = {}
	for a: Array in ACTIONS:
		var keys: Array[int] = []
		for k: int in a[2]:
			keys.append(k)
		out[str(a[0])] = keys
	return out


## Binds `key` as `action`'s primary key. Whatever else used that key loses it (keys are never shared),
## and the action's old primary key goes free. Returns the actions that changed (empty if refused).
static func rebind(binds: Dictionary, action: String, key: int) -> Array[String]:
	var changed: Array[String] = []
	if not binds.has(action) or key == KEY_NONE or RESERVED.has(key):
		return changed
	for other: String in binds.keys():
		if other == action:
			continue
		var keys: Array = binds[other]
		if keys.has(key):
			keys.erase(key)
			changed.append(other)
	var mine: Array = binds[action]
	mine.erase(key)
	if mine.is_empty():
		mine.append(key)
	else:
		mine[0] = key
	changed.append(action)
	return changed


## Saved form: action -> comma-separated keycodes (only actions that differ from the defaults).
static func to_config(binds: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	var d: Dictionary = defaults()
	for action: String in binds.keys():
		var keys: Array = binds[action]
		if keys != Array(d.get(action, [])):
			var parts: PackedStringArray = []
			for k: int in keys:
				parts.append(str(k))
			out[action] = ",".join(parts)
	return out


## Reads the saved form over the defaults, ignoring unknown actions, bad keycodes and reserved keys.
static func from_config(saved: Dictionary) -> Dictionary:
	var binds: Dictionary = defaults()
	for action: String in saved.keys():
		if not binds.has(action):
			continue
		var keys: Array[int] = []
		for part: String in str(saved[action]).split(",", false):
			if part.is_valid_int():
				var k: int = part.to_int()
				if k > 0 and not RESERVED.has(k) and not keys.has(k):
					keys.append(k)
		binds[action] = keys
	# A key claimed by two actions stays with the one the player saved it for (so a key someone chose
	# keeps working when a new version gives that key to a new action by default), else with the first.
	var seen: Dictionary = {}
	for saved_first: bool in [true, false]:
		for a: Array in ACTIONS:
			var action: String = str(a[0])
			if saved.has(action) != saved_first:
				continue
			var keys: Array = binds[action]
			for k: int in keys.duplicate():
				if seen.has(k):
					keys.erase(k)
				else:
					seen[k] = action
	return binds


static func key_name(keys: Array) -> String:
	if keys.is_empty():
		return "(unbound)"
	var parts: PackedStringArray = []
	for k: int in keys:
		parts.append(OS.get_keycode_string(k))
	return " / ".join(parts)
