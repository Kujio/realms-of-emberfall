## Command-line driven end-to-end test of the real client UI flow. Enabled with:
##   EmberfallClient.exe -- --autotest --server HOST:PORT --user U --pass P --char Name
##        [--race R --class C --city C] [--walk SECONDS] [--say TEXT] [--expect-peers N]
##        [--screenshot PATH] [--result PATH] [--timeout SECONDS] [--linger SECONDS]
##        [--reset-pins] [--expect-error TEXT]
##        [--fight N] [--camp X,Z] [--die-at X,Z] [--death-screenshot PATH] [--show-windows]
##        [--caster N] [--pet N] [--bard N] [--gender 0|1] [--expect-anims a,b,c] [--expect-models] [--preview-screenshot PATH]
## --fight N: target, chase, and kill N level-appropriate mobs near --camp (auto-attack plus
## hotbar slot 2), looting every corpse. --die-at: then walk there, pick a fight far above our
## level, die, press Respawn, walk back to the corpse, and loot it.
## --caster N (Cleric or Wizard): find the class trainer, buy the level-1 scroll we lack, scribe
## it, buff ourselves, memorize the new spell, then kill and loot N mobs with spells (healing
## ourselves when hurt), standing still while casting.
## --pet N (a pet class, e.g. Magician): summon the pet (gem 1), then kill N mobs by sending the pet in
## and casting gem 2. --bard N: kill N mobs in melee while singing the gem-2 song at them.
## Certificate pins go to user://autotest_known_servers.cfg (--reset-pins clears it first).
## With --expect-error the run passes only if login fails with a message containing TEXT.
## --expect-anims: the run fails unless every listed animation state (idle, run, attack1, cast,
## death, ...) was played by some character. --expect-models: the run fails if our own character
## or every mob fell back to a placeholder. --preview-screenshot: capture the create screen's model preview.
## --camera-distance M: hold the follow camera at M metres (closer screenshots).
## --items: the item economy (kit, vendor buy/equip/sell, food, bank, auction board); see _items_run.
## --zone-walk: Phase 6 zone travel: from Emberhold out of the east gate into the Emberglade, into the
## Rattlebone Warren's cave mouth, and back out of the warren's exit; fails on a layout mismatch.
## --quest: Phase 6 quests through the real UI: walk to Captain Brack in Emberhold, talk (T), take his
## first quest from the dialogue window, see it in the journal and tracker, open the map, abandon it.
## --social lead|join --partner NAME [--guild NAME]: Phase 7 groups, guilds and chat between two clients run
## together (see _social_run).
## Registers (or logs in), creates the character if needed, enters the world, walks in a
## circle, optionally chats and screenshots, writes a JSON result, camps, and quits.
extends Node

const CharacterVisual := preload("res://scripts/world/character_visual.gd")

var opts: Dictionary = {}
var _state: String = "start"
var _state_time: float = 0.0
var _total_time: float = 0.0
var _tried_login: bool = false
var _result: Dictionary = {"ok": false, "log": []}
var _said: bool = false
var _done: bool = false
var _world_start_ms: int = 0
var _create_submitted: bool = false
var _phase: String = ""
var _target: int = 0
var _loot_corpse: int = 0
var _loot_step: int = 0
var _loot_t: float = 0.0
var _looted_before: int = 0
var _next_kick: float = 0.0
var _next_attack: float = 0.0
var _died_with: int = 0
var _respawn_clicked: bool = false
var _recovered: bool = false
var _kills_needed: int = 0
var _looted_kills: int = 0
# Caster run (--caster N).
var _caster_step_name: String = ""
var _step_t: float = 0.0
var _npcs_tried: Array[int] = []
var _new_spell: String = ""
var _book_before: int = 0
var _max_target_effects: int = 0
var _next_cast: float = 0.0
var _bought_item: String = ""
var _loot_wanted_at: float = -1.0
# Pet and Bard runs (--pet N, --bard N) reuse the caster fight with these modes.
var _mode: String = "caster"
var _next_pet: float = 0.0


func _ready() -> void:
	opts = _parse_args(OS.get_cmdline_user_args())
	Session.known_servers_path = "user://autotest_known_servers.cfg"
	if str(opts["reset-pins"]) == "true":
		DirAccess.remove_absolute(ProjectSettings.globalize_path(Session.known_servers_path))
	Session.status_changed.connect(_on_status)
	Session.auth_succeeded.connect(func() -> void: _goto("char_select"))
	Session.entered_world.connect(func(_i: Dictionary) -> void: _goto("in_world"))
	Session.returned_to_select.connect(func() -> void: _goto("camped"))
	Session.char_create_finished.connect(_on_created)
	Session.trust_requested.connect(_on_trust_requested)
	_log("autotest: server=%s user=%s char=%s" % [opts["server"], opts["user"], opts["char"]])
	_goto("login")


func _parse_args(args: PackedStringArray) -> Dictionary:
	var o: Dictionary = {
		"server": "127.0.0.1:7777", "user": "tester", "pass": "testpassword", "char": "Tester",
		"race": "highlander", "class": "warrior", "city": "emberhold", "walk": "8", "say": "",
		"expect-peers": "0", "screenshot": "", "result": "", "timeout": "60", "linger": "0",
		"reset-pins": "false", "expect-error": "", "fight": "0", "camp": "50,0", "die-at": "",
		"death-screenshot": "", "show-windows": "false", "caster": "0", "pet": "0", "bard": "0", "gender": "0", "expect-anims": "",
		"expect-models": "false", "preview-screenshot": "", "camera-distance": "", "items": "false", "zone-walk": "false", "quest": "false",
		"social": "", "partner": "", "guild": "",
	}
	var i: int = 0
	while i < args.size():
		var a: String = args[i]
		if a.begins_with("--") and a != "--autotest":
			var key: String = a.substr(2)
			if i + 1 < args.size() and not args[i + 1].begins_with("--"):
				o[key] = args[i + 1]
				i += 1
			else:
				o[key] = "true"
		i += 1
	return o


func _log(line: String) -> void:
	print(line)
	var lines: Array = _result["log"]
	lines.append(line)


func _goto(state: String) -> void:
	_state = state
	_state_time = 0.0
	_log("autotest: -> " + state)
	match state:
		"login":
			_screen().call("autotest_login", str(opts["server"]), str(opts["user"]), str(opts["pass"]), true)
		"in_world":
			_world_start_ms = Time.get_ticks_msec()
			var world: Node = _screen()
			if str(opts["show-windows"]) == "true":
				var hud: Node = world.get("hud")
				# Casters see their spellbook; everyone else their abilities.
				if int(opts["caster"]) > 0:
					(hud.get("spells") as Node).call("toggle_spellbook")
				else:
					hud.call("toggle_abilities")
				hud.call("toggle_inventory")
			_kills_needed = int(opts["fight"])
			if str(opts["social"]) != "":
				_phase = "social"
				_items_goto("settle")
				_log("autotest: social run (%s) with %s" % [opts["social"], opts["partner"]])
				return
			if str(opts["quest"]) == "true":
				_phase = "quest"
				_items_goto("settle")
				_log("autotest: quest run: talk, accept, journal, map, abandon")
				return
			if str(opts["zone-walk"]) == "true":
				_phase = "zones"
				_zone_leg = 0
				_log("autotest: zone walk: emberhold -> emberglade -> rattlebone_warren -> emberglade")
				return
			if str(opts["items"]) == "true":
				_phase = "items"
				_items_goto("settle")
				_log("autotest: items run: kit, vendor, equip, sell, consume, bank, auction")
				return
			if int(opts["pet"]) > 0 or int(opts["bard"]) > 0:
				_mode = "pet" if int(opts["pet"]) > 0 else "bard"
				_kills_needed = int(opts["pet"]) if _mode == "pet" else int(opts["bard"])
				_phase = "caster"
				_caster_step_name = "settle"
				_log("autotest: %s run: %d kills" % [_mode, _kills_needed])
				return
			if int(opts["caster"]) > 0:
				_kills_needed = int(opts["caster"])
				_phase = "caster"
				_caster_step_name = "settle"
				_log("autotest: caster run: train, buff, memorize, then %d kills with spells" % _kills_needed)
				return
			if _kills_needed > 0 or str(opts["die-at"]) != "":
				_phase = "fight" if _kills_needed > 0 else "die"
				_log("autotest: combat run: fight %d, die-at '%s'" % [_kills_needed, opts["die-at"]])
			else:
				world.get("local_player").set("autopilot", true)


func _screen() -> Node:
	return get_parent().call("current_screen")


func _on_status(text: String, is_error: bool) -> void:
	if not is_error:
		return
	_log("autotest: status error: " + text)
	if text.find("taken") >= 0 and not _tried_login:
		_tried_login = true
		_screen().call_deferred("autotest_login", str(opts["server"]), str(opts["user"]), str(opts["pass"]), false)
	elif _state == "login":
		_finish(false, "login failed: " + text)


func _on_trust_requested(server: String, fingerprint: String) -> void:
	_log("autotest: trusting %s with fingerprint %s" % [server, fingerprint])
	_result["server_fingerprint"] = fingerprint
	Session.call_deferred("trust_server", true)


func _on_created(ok: bool, message: String) -> void:
	_log("autotest: create result ok=%s %s" % [ok, message])
	if not ok:
		_finish(false, "character creation failed: " + message)


func _process(delta: float) -> void:
	if _done:
		return
	_state_time += delta
	_total_time += delta
	if _state == "in_world" and _total_time >= _next_model_note:
		_next_model_note = _total_time + 1.0
		var w: Node = _screen()
		if w != null and w.name == "World":
			_note_mob_models(w)
	if _total_time > float(opts["timeout"]):
		_finish(false, "timed out in state " + _state)
		return
	match _state:
		"char_select":
			var screen: Node = _screen()
			if screen.name != "CharSelectScreen" or _state_time < 0.3:
				return
			var id: int = int(screen.call("find_character", str(opts["char"])))
			if id >= 0:
				_note_preview("select_preview", screen)
				_log("autotest: entering world as %s (id %d)" % [opts["char"], id])
				Session.enter_world(id)
				_goto("entering")
			elif _state_time > 1.0:
				_log("autotest: creating %s" % opts["char"])
				screen.emit_signal("create_requested")
				_goto("creating")
		"creating":
			var screen: Node = _screen()
			if not _create_submitted and screen.name == "CharCreateScreen" and _state_time > 0.2:
				_create_submitted = true
				screen.call("autotest_create", str(opts["char"]), str(opts["race"]), str(opts["class"]), str(opts["city"]),
					int(opts["gender"]))
				_note_preview("create_preview", screen)
				if str(opts["preview-screenshot"]) != "":
					_screenshot_now(str(opts["preview-screenshot"]))
			elif screen.name == "CharSelectScreen":
				_goto("char_select")
		"in_world":
			if str(opts["camera-distance"]) != "":
				var rig: Node = _screen().get("_camera_rig") as Node
				if rig != null:
					rig.set("distance", clampf(float(opts["camera-distance"]), 2.5, 25.0))
			if _phase != "":
				_combat_step()
				return
			var walk: float = float(opts["walk"])
			if not _said and str(opts["say"]) != "" and _state_time > walk * 0.5:
				_said = true
				_screen().get("chat").call("say", str(opts["say"]))
			if _state_time >= walk:
				_screen().get("local_player").set("autopilot", false)
				_capture_and_report()
		"camped":
			Session.quit_session("autotest finished")
			_quit()


# ------------------------------------------------------------------------------ combat run

static func _parse_point(text: String) -> Vector3:
	var parts: PackedStringArray = text.split(",")
	if parts.size() != 2:
		return Vector3.ZERO
	return Vector3(parts[0].to_float(), 0.0, parts[1].to_float())


func _go(lp: Node3D, point: Vector3, stop: float) -> void:
	lp.set("goto_point", point)
	lp.set("goto_stop", stop)
	lp.set("goto_active", true)


func _combat_step() -> void:
	var world: Node = _screen()
	if world == null or world.name != "World":
		return
	var lp: Node3D = world.get("local_player")
	var stats: Dictionary = world.get("stats")
	var remotes: Dictionary = world.get("remotes")
	var hud: Node = world.get("hud")
	var my_level: int = int(world.get("my_level"))
	var dead: bool = (int(stats.get("flags", 0)) & 1) != 0

	if _phase == "report":
		return
	if _phase == "caster":
		_caster_step(world, lp, stats, remotes, hud)
		return
	if _phase == "items":
		_items_run(world, lp)
		return
	if _phase == "zones":
		_zones_step(world, lp)
		return
	if _phase == "quest":
		_quest_run(world, lp)
		return
	if _phase == "social":
		_social_run(world)
		return
	if dead:
		lp.set("goto_active", false)
		if not _respawn_clicked and bool(hud.call("is_death_shown")):
			_respawn_clicked = true
			_log("autotest: died to %s (lost %d XP); pressing Respawn" % [
				str((world.get("last_death") as Dictionary).get("killer", "?")), int((world.get("last_death") as Dictionary).get("xp_lost", 0))])
			_screenshot_now(str(opts["death-screenshot"]))
			world.call_deferred("respawn")
		return
	if _respawn_clicked and _phase == "die":
		_phase = "recover"
		_log("autotest: respawned; walking back to the corpse at %s" % str(world.get("death_position")))

	# Looting in progress (mob corpses during the fight, our own corpse after dying).
	if _loot_corpse != 0:
		if not remotes.has(_loot_corpse):
			if _phase == "recover":
				_go(lp, world.get("death_position"), 2.0)  # walk until the corpse comes into view
				return
			if _loot_step >= 2:
				_finish_loot(world)  # an emptied mob corpse vanishes as soon as it is looted
			else:
				_loot_corpse = 0
			return
		var c: Node3D = remotes[_loot_corpse]
		var d: float = c.position.distance_to(lp.position)
		if d > 3.0:
			_go(lp, c.position, 2.0)
			return
		lp.set("goto_active", false)
		_loot_step_run(world, hud)
		return

	if _phase == "recover":
		# The exact corpse from PlayerDied: older (emptied) corpses of ours may lie nearby.
		var corpse: int = int((world.get("last_death") as Dictionary).get("corpse_id", 0))
		if corpse != 0 and remotes.has(corpse):
			_loot_corpse = corpse
			_loot_step = 0
		else:
			_go(lp, world.get("death_position"), 2.0)
		return

	var kills: int = int(world.get("kills"))
	if _phase == "fight" and _looted_kills >= _kills_needed:
		_phase = "die" if str(opts["die-at"]) != "" else "done"
		_died_with = (hud.get("inventory_items") as Array).size()
		_log("autotest: %d kills looted; inventory holds %d stacks" % [_looted_kills, _died_with])
	if _phase == "done":
		lp.set("goto_active", false)
		_capture_and_report()
		return

	# Fighting: keep the current target, or pick a fresh one.
	if _target != 0 and (not remotes.has(_target) or not bool((remotes[_target] as Node3D).call("is_alive"))):
		if _phase == "fight" and kills > int(_result.get("kills_seen", 0)):
			_result["kills_seen"] = kills
			var corpse: int = int(world.call("nearest", 2, func(r: Node3D) -> bool: return not str(r.get("display_name")).begins_with(str(lp.get("display_name"))), 12.0))
			if corpse != 0:
				_loot_corpse = corpse
				_loot_step = 0
		_target = 0
		return
	if _target == 0:
		var want_strong: bool = _phase == "die"
		_target = int(world.call("nearest", 1, func(r: Node3D) -> bool:
			var lvl: int = int(r.get("level"))
			var fresh: bool = bool(r.call("is_alive")) and int(r.get("hp_pct")) >= 100
			return fresh and (lvl >= my_level + 5 if want_strong else lvl <= my_level), 70.0))
		if _target != 0:
			world.call("set_target", _target)
			_log("autotest: engaging %s (level %d)" % [str((remotes[_target] as Node3D).get("display_name")), int((remotes[_target] as Node3D).get("level"))])
		else:
			_go(lp, _parse_point(str(opts["die-at"] if want_strong else opts["camp"])), 6.0)
		return
	var m: Node3D = remotes[_target]
	var dist: float = m.position.distance_to(lp.position)
	_go(lp, m.position, 2.0)
	if dist < 6.0 and not bool(stats.get("auto_attack", false)) and int(world.get("target_id")) == _target and _state_time >= _next_attack:
		world.call("use_slot", 0)  # slot 1: Attack toggle
		_next_attack = _state_time + 0.75  # wait for PlayerStats to confirm before asking again
	if dist < 4.0 and _state_time >= _next_kick:
		world.call("use_slot", 1)  # slot 2: Rising Kick
		_next_kick = _state_time + 6.5
	if str(opts["screenshot"]) != "" and not _result.has("combat_screenshot") and dist < 4.0 and int(world.get("combat_events")) > 6:
		_result["combat_screenshot"] = true
		_screenshot_now(str(opts["screenshot"]))


func _caster_goto(step: String) -> void:
	_caster_step_name = step
	_step_t = _state_time
	_log("autotest: caster -> " + step)


func _caster_step(world: Node, lp: Node3D, stats: Dictionary, remotes: Dictionary, hud: Node) -> void:
	var spells: Node = hud.get("spells")
	var since: float = _state_time - _step_t
	var gems: Array = spells.get("gem_ids")
	match _caster_step_name:
		"settle":
			if since > 1.5 and bool(spells.call("has_spells")):
				_caster_goto("summon" if _mode == "pet" else ("fight" if _mode == "bard" else "find_trainer"))
		"summon":
			var ps: Dictionary = world.get("pet_status")
			if not ps.is_empty() and int(ps["pet_id"]) != 0:
				_result["pet"] = str(ps["name"])
				_log("autotest: our pet is %s" % str(ps["name"]))
				_caster_goto("fight")
			elif since > 40.0:
				_finish(false, "our pet never answered the call")
			elif not bool(spells.get("casting")) and _state_time >= _next_cast:
				world.call("cast_gem", 0)
				_next_cast = _state_time + 8.0
		"find_trainer":
			var npc: int = int(world.call("nearest", 3, func(r: Node3D) -> bool: return not _npcs_tried.has(int(r.get("entity_id"))), 80.0))
			if npc == 0:
				_finish(false, "no trainer for our class nearby (tried %d)" % _npcs_tried.size())
				return
			var n: Node3D = remotes[npc]
			if n.position.distance_to(lp.position) > 4.0:
				_go(lp, n.position, 3.0)
				return
			lp.set("goto_active", false)
			_npcs_tried.append(npc)
			world.set("last_trainer", {})
			world.call("set_target", npc)
			world.call_deferred("open_trainer")
			_caster_goto("await_trainer")
		"await_trainer":
			var t: Dictionary = world.get("last_trainer")
			if not t.is_empty():
				_result["trainer"] = str(t["name"])
				_log("autotest: %s offers %d scrolls" % [str(t["name"]), (t["entries"] as Array).size()])
				_caster_goto("buy")
			elif since > 2.0:
				_caster_goto("find_trainer")  # not our class's trainer; try the next one
		"buy":
			var t: Dictionary = world.get("last_trainer")
			var coin: int = int(hud.get("inventory_coin"))
			for e: Dictionary in t["entries"]:
				if not bool(e["known"]) and int(e["level"]) <= int(world.get("my_level")) and int(e["price"]) <= coin:
					_new_spell = str(e["spell_id"])
					_bought_item = str(e["item_id"])
					_book_before = (spells.get("spell_order") as Array).size()
					_log("autotest: buying %s for %d copper" % [str(e["name"]), int(e["price"])])
					world.call("trainer_buy", int(t["npc_id"]), str(e["item_id"]))
					_caster_goto("scribe")
					return
			_finish(false, "nothing affordable to buy (coin %d)" % coin)
		"scribe":
			for it: Dictionary in hud.get("inventory_items"):
				if str(it.get("item_id", "")) == _bought_item:  # the scroll we bought, not any scroll
					world.call("scribe", int(it["slot"]))
					_caster_goto("await_scribe")
					return
			if since > 3.0:
				_finish(false, "the scroll never reached our pack")
		"await_scribe":
			if (spells.get("spell_order") as Array).has(_new_spell):
				_result["scribed"] = _new_spell
				spells.call("close_trainer")
				_caster_goto("buff")
			elif since > 3.0:
				_finish(false, "scribing %s did not add it to the spellbook" % _new_spell)
		"buff":
			# Gem 2 holds our starter buff: cast it on ourselves, then replace it with the new spell.
			if not bool(spells.get("casting")) and since > 0.3 and _state_time >= _next_cast:
				world.call("set_target", int(world.get("my_id")))
				world.call("cast_gem", 1)
				_next_cast = _state_time + 1.0
			var buff_id: String = str(gems[1])
			if buff_id != "" and bool(spells.call("has_self_buff", buff_id)):
				_result["self_buff"] = buff_id
				_caster_goto("memorize")
			elif since > 12.0:
				_finish(false, "our buff %s never showed in the tray" % buff_id)
		"memorize":
			if since < 0.2:
				return
			if str(gems[1]) != _new_spell:
				if since < 0.5:
					world.call("memorize", 1, _new_spell)
				elif since > 4.0:
					_finish(false, "memorizing %s failed" % _new_spell)
				return
			if float((spells.call("gem_lock", 1) as Array)[0]) <= 0.0:
				_result["memorized"] = _new_spell
				_caster_goto("fight")
		"fight":
			_caster_fight(world, lp, stats, remotes, hud, spells)


func _caster_fight(world: Node, lp: Node3D, stats: Dictionary, remotes: Dictionary, hud: Node, spells: Node) -> void:
	var dead: bool = (int(stats.get("flags", 0)) & 1) != 0
	if dead:
		_finish(false, "died during the caster run")
		return
	_max_target_effects = maxi(_max_target_effects, (spells.get("target_buffs") as Array).size())
	if _loot_corpse != 0:
		if not remotes.has(_loot_corpse):
			if _loot_step >= 2:
				_finish_loot(world)
			else:
				_loot_corpse = 0
			return
		var c: Node3D = remotes[_loot_corpse]
		if c.position.distance_to(lp.position) > 3.0:
			_go(lp, c.position, 2.0)
			return
		lp.set("goto_active", false)
		_loot_step_run(world, hud)
		return
	if _looted_kills >= _kills_needed:
		lp.set("goto_active", false)
		_phase = "report"
		_result["casts_completed"] = spells.get("casts_completed")
		_result["casts_interrupted"] = spells.get("casts_interrupted")
		_result["spells_landed"] = world.get("spells_landed")
		_result["resists"] = world.get("resists")
		_result["self_buffs"] = (spells.get("self_buffs") as Array).map(func(b: Dictionary) -> String: return str(b["spell_id"]))
		_result["max_target_effects"] = _max_target_effects
		_result["gems"] = spells.get("gem_ids")
		_result["pet_status"] = world.get("pet_status")
		_result["pet_commands"] = world.get("pet_commands_sent")
		_result["songs_started"] = world.get("songs_started")
		_capture_and_report()
		return
	var kills: int = int(world.get("kills"))
	if _target != 0 and (not remotes.has(_target) or not bool((remotes[_target] as Node3D).call("is_alive"))):
		if kills > int(_result.get("kills_seen", 0)):
			_result["kills_seen"] = kills
			_loot_wanted_at = _state_time
		_target = 0
		return
	if _loot_wanted_at >= 0.0:
		# Our kill can lie anywhere within casting range; keep looking until its corpse appears.
		var corpse: int = int(world.call("nearest", 2, func(r: Node3D) -> bool: return not str(r.get("display_name")).begins_with(str(lp.get("display_name"))), 75.0))
		if corpse != 0:
			_loot_corpse = corpse
			_loot_step = 0
			_loot_wanted_at = -1.0
		elif _state_time - _loot_wanted_at > 3.0:
			_loot_wanted_at = -1.0
		return
	if _target == 0:
		var my_level: int = int(world.get("my_level"))
		_target = int(world.call("nearest", 1, func(r: Node3D) -> bool:
			return bool(r.call("is_alive")) and int(r.get("hp_pct")) >= 100 and int(r.get("level")) <= my_level, 70.0))
		if _target != 0:
			world.call("set_target", _target)
			_log("autotest: casting at %s (level %d)" % [str((remotes[_target] as Node3D).get("display_name")), int((remotes[_target] as Node3D).get("level"))])
		else:
			_go(lp, _parse_point(str(opts["camp"])), 6.0)
		return
	var m: Node3D = remotes[_target]
	var dist: float = m.position.distance_to(lp.position)
	if int(world.get("target_id")) != _target:
		world.call("set_target", _target)
		return
	if _mode == "bard":
		# Bards fight in melee and sing while they move.
		if dist > 3.5:
			_go(lp, m.position, 2.5)
		else:
			lp.set("goto_active", false)
		if dist < 5.0 and not bool(stats.get("auto_attack", false)) and _state_time >= _next_attack:
			world.call("toggle_auto_attack")
			_next_attack = _state_time + 0.75
		var singing: String = str((world.get("song") as Dictionary).get("song_id", ""))
		if dist < 30.0 and singing != str(spells.get("gem_ids")[1]) and _state_time >= _next_cast:
			world.call("cast_gem", 1)
			_next_cast = _state_time + 2.0
		return
	if _mode == "pet":
		var ps: Dictionary = world.get("pet_status")
		if ps.is_empty() or int(ps["pet_id"]) == 0:
			_caster_goto("summon")  # it died: call another
			return
		if int(ps["target_id"]) != _target and _state_time >= _next_pet:
			world.call("send_pet_command", 0)
			_next_pet = _state_time + 2.0
	# Close to casting range, then stand still: moving interrupts a spell.
	if dist > 40.0 and not bool(spells.get("casting")):
		_go(lp, m.position, 30.0)
		return
	lp.set("goto_active", false)
	if dist < 5.0 and not bool(stats.get("auto_attack", false)) and _state_time >= _next_attack:
		world.call("use_slot", 0)
		_next_attack = _state_time + 0.75
	if str(opts["screenshot"]) != "" and not _result.has("combat_screenshot") and bool(spells.get("casting")) and int(world.get("combat_events")) > 4:
		_result["combat_screenshot"] = true
		_screenshot_now(str(opts["screenshot"]))
	if bool(spells.get("casting")) or _state_time < _next_cast:
		return
	var hp: float = float(stats.get("hp", 1)) / maxf(1.0, float(stats.get("max_hp", 1)))
	var healer: bool = str(opts["class"]) == "cleric"
	var gem: int = 1
	if healer and hp < 0.5:
		gem = 0  # Minor Mending: the server lands a heal on us when a mob is targeted
	elif not healer and float((spells.call("gem_lock", 1) as Array)[0]) > 0.0:
		gem = 0
	if float((spells.call("gem_lock", gem) as Array)[0]) <= 0.0:
		world.call("cast_gem", gem)
		_next_cast = _state_time + 0.4


## Target the corpse, open it, take everything (or close an empty one), and wait for the
## server to confirm the window closed.
func _loot_step_run(world: Node, hud: Node) -> void:
	match _loot_step:
		0:
			_looted_before = int(world.get("corpses_looted"))
			world.call("set_target", _loot_corpse)
			_loot_t = _state_time
			_loot_step = 1
		1:
			if _state_time - _loot_t > 0.3:
				world.call("loot_target")
				_loot_t = _state_time
				_loot_step = 2
		2:
			if bool(hud.call("is_loot_open")):
				if (hud.get("loot_items") as Array).is_empty():
					world.call("loot_close")
				else:
					world.call("loot_take", 255)
				_loot_t = _state_time
				_loot_step = 3
			elif _state_time - _loot_t > 3.0:
				_log("autotest: loot window never opened (%s)" % str(world.get("last_loot_closed")))
				_finish_loot(world)
		3:
			if int(world.get("corpses_looted")) > _looted_before:
				_finish_loot(world)
			elif bool(hud.call("is_loot_open")) and (hud.get("loot_items") as Array).is_empty() and _state_time - _loot_t > 0.3:
				world.call("loot_close")  # a player corpse stays open, empty, until we close it
				_loot_t = _state_time
			elif _state_time - _loot_t > 4.0:
				_log("autotest: loot did not complete")
				_finish_loot(world)


func _finish_loot(world: Node) -> void:
	var looted: int = int(world.get("corpses_looted"))
	if _phase == "recover":
		var hud: Node = world.get("hud")
		var now_items: int = (hud.get("inventory_items") as Array).size()
		_recovered = looted > _looted_before
		_log("autotest: corpse recovered=%s; inventory %d stacks (had %d before dying)" % [_recovered, now_items, _died_with])
		_result["recovered_items"] = now_items
		_phase = "done"
	else:
		_looted_kills = looted
		_log("autotest: looted corpse (%d/%d)" % [_looted_kills, _kills_needed])
	_loot_corpse = 0
	_loot_step = 0


# ------------------------------------------------------------------------------ quests (Phase 6)

const QUEST_ID: String = "emberglade_vermin"


func _dialog_has(wui: Node, key: String) -> bool:
	if not bool(wui.call("dialog_visible")):
		return false
	for o: Variant in wui.call("dialog_options"):
		if str((o as Dictionary)["key"]) == key:
			return true
	return false


func _journal_has(world: Node, qid: String) -> bool:
	var j: Dictionary = world.get("quest_journal")
	for q: Variant in j.get("quests", []):
		if str((q as Dictionary)["id"]) == qid:
			return true
	return false


func _quest_run(world: Node, lp: Node3D) -> void:
	var wui: Node = (world.get("hud") as Node).get("world_ui") as Node
	var since: float = _state_time - _items_t
	if since > 40.0 and _items_step != "done":
		_finish(false, "quest run stuck at step '%s'" % _items_step)
		return
	match _items_step:
		"settle":
			if since > 1.5:
				_items_goto("walk")
		"walk":
			_walk_to_npc(world, lp, "questgiver", "Brack", "dialog")
		"dialog":
			if _dialog_has(wui, "offer:" + QUEST_ID):
				_result["quest_offered"] = QUEST_ID
				wui.call("choose", "offer:" + QUEST_ID)
				_items_goto("offer")
		"offer":
			if _dialog_has(wui, "accept:" + QUEST_ID):
				wui.call("choose", "accept:" + QUEST_ID)
				_items_goto("accepted")
		"accepted":
			if _journal_has(world, QUEST_ID):
				_result["quest_accepted"] = QUEST_ID
				_log("autotest: quest in the journal: " + QUEST_ID)
				wui.call("close_dialog")
				wui.call("toggle_journal")
				wui.call("toggle_map")
				_items_goto("windows")
		"windows":
			if since > 1.0:
				_result["journal_shown"] = bool(wui.call("journal_visible"))
				_result["map_shown"] = bool(wui.call("map_visible"))
				_result["minimap"] = (wui.get("_minimap") as Control).visible
				_result["tracker_lines"] = (wui.get("_tracker") as Control).get_child_count()
				# A headless run has a 64x64 dummy display: placement is only meaningful on a real one.
				_result["windows_on_screen"] = DisplayServer.get_name() == "headless" or bool(wui.call("windows_on_screen"))
				if not bool(_result["windows_on_screen"]):
					_log("autotest: off screen: %s" % str(wui.call("offscreen")))
				if str(opts["screenshot"]) != "" and DisplayServer.get_name() != "headless":
					# The last drawn frame, now: the windows have been open for a second.
					# (Its own file: the end-of-run report screenshots to --screenshot itself.)
					var shot: String = str(opts["screenshot"]).get_basename() + "_windows.png"
					var img: Image = get_viewport().get_texture().get_image()
					_log("autotest: screenshot %s (%s)" % [shot, error_string(img.save_png(shot))])
				_items_goto("shot")
		"shot":
			if since > 1.0:  # the screenshot is taken with the windows still open
				wui.emit_signal("quest_abandoned", QUEST_ID)
				_items_goto("abandon")
		"abandon":
			if not _journal_has(world, QUEST_ID):
				_result["quest_abandoned"] = QUEST_ID
				wui.call("close_windows")
				_items_goto("done")
				_phase = "report"
				_capture_and_report()


# ------------------------------------------------------------------------------ zone walk (Phase 6)

var _zone_leg: int = 0
var _zones_seen: Array[String] = []
var _layout_bad: bool = false
var _leg_started: float = 0.0
var _zone_world: Node = null
var _zone_corrections: Dictionary = {}  # zone visit index -> corrections seen there
var _shot_done: Dictionary = {}


## Legs: [zone, point or "" (the zone's exit line)]. Walking onto a line moves us; each new World is a
## new zone, which must be the next one expected.
const ZONE_LEGS: Array = [
	["emberhold", Vector3(150.0, 0.0, 0.0)], ["emberhold", Vector3(197.0, 0.0, 0.0)],
	["emberglade", Vector3(0.0, 0.0, 0.0)], ["emberglade", Vector3(250.0, 0.0, -178.0)], ["emberglade", Vector3(250.0, 0.0, -204.0)],
	["rattlebone_warren", null],
]


func _zones_step(world: Node, lp: Node3D) -> void:
	var zone: Variant = world.get("zone")
	if zone == null:
		return
	var zid: String = str((zone as RefCounted).get("id"))
	if world != _zone_world:
		_zone_world = world
		_zones_seen.append(zid)
		_leg_started = _total_time
		if bool(world.get("layout_mismatch")):
			_layout_bad = true
			_log("autotest: layout mismatch in " + zid)
		_log("autotest: now in %s (zones so far: %s)" % [zid, ", ".join(_zones_seen)])
		# Skip legs for zones we have left.
		while _zone_leg < ZONE_LEGS.size() and str(ZONE_LEGS[_zone_leg][0]) != zid:
			_zone_leg += 1
		if _zones_seen.size() >= 4:
			# Walk a few steps in the last zone so its inputs are acknowledged, then report.
			_go(lp, lp.position + Vector3(0.0, 0.0, 12.0), 0.0)
	_zone_corrections[_zones_seen.size() - 1] = int(world.get("corrections_received"))
	# --shots DIR: a screenshot in each zone, a moment after arriving (Windows visual checks).
	if str(opts.get("shots", "")) != "" and not _shot_done.has(_zones_seen.size()) and _total_time - _leg_started > 2.5:
		_shot_done[_zones_seen.size()] = true
		_screenshot_now(str(opts["shots"]).path_join("%d_%s.png" % [_zones_seen.size(), zid]))
	if _zones_seen.size() >= 4:
		if _total_time - _leg_started > 3.0:
			lp.set("goto_active", false)
			_phase = "report"
			var total: int = 0
			for c: int in _zone_corrections.values():
				total += c
			_result["zone_corrections"] = total
			_capture_and_report()
		return
	if _total_time - _leg_started > 150.0:
		_finish(false, "zone walk stuck in %s at leg %d" % [zid, _zone_leg])
		return
	if _zone_leg >= ZONE_LEGS.size():
		# Back the way we came: the warren's way out sends us to the Emberglade.
		if zid == "emberglade" and _zones_seen.size() >= 3:
			return
		return
	var leg: Array = ZONE_LEGS[_zone_leg]
	if str(leg[0]) != zid:
		return
	var point: Vector3
	if leg[1] == null and _total_time - _leg_started < 3.5:
		lp.set("goto_active", false)  # a moment in the dungeon (and its screenshot) before leaving
		return
	if leg[1] == null:
		var lines: Array = (zone as RefCounted).call("zone_lines")
		if lines.is_empty():
			_finish(false, zid + " has no zone line")
			return
		var ld: Dictionary = lines[0]
		point = Vector3(float(ld["x"]), 0.0, float(ld["z"]))
	else:
		point = leg[1]
	var to: Vector3 = point - lp.position
	to.y = 0.0
	if to.length() < 1.0 and leg[1] != null:
		_zone_leg += 1
		_leg_started = _total_time
		return
	_go(lp, point, 0.0)


func _screenshot_now(path: String) -> void:
	if path == "" or DisplayServer.get_name() == "headless":
		return
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	var err: Error = img.save_png(path)
	_log("autotest: screenshot %s (%s)" % [path, error_string(err)])


func _capture_and_report() -> void:
	_state = "reporting"
	var world: Node = _screen()
	var lp: Node3D = world.get("local_player")
	var seen: Dictionary = world.get("seen_names")
	var chat_lines: Array = world.get("chat").get("lines")
	_result["seen_players"] = seen.keys()
	_result["visible_now"] = (world.get("remotes") as Dictionary).size()
	_result["snapshots"] = world.get("snapshots_received")
	_result["corrections"] = world.get("corrections_received")
	_result["chat"] = chat_lines
	_result["position"] = [lp.position.x, lp.position.z]
	_result["inputs_sent"] = lp.get("seq")
	_result["last_ack"] = lp.get("last_ack")
	_result["net_malformed_in"] = Net.malformed_in
	_result["in_world_seconds"] = float(Time.get_ticks_msec() - _world_start_ms) / 1000.0
	_result["inputs_per_second"] = float(lp.get("seq")) / maxf(0.001, float(_result["in_world_seconds"]))
	_result["kills"] = world.get("kills")
	_result["deaths"] = world.get("deaths")
	_result["corpses_looted"] = world.get("corpses_looted")
	_result["level"] = world.get("my_level")
	_result["level_ups"] = world.get("level_ups")
	_result["combat_events"] = world.get("combat_events")
	_result["abilities_used"] = world.get("abilities_used")
	var stats: Dictionary = world.get("stats")
	_result["xp_into_level"] = stats.get("xp", 0)
	_result["hp"] = [stats.get("hp", 0), stats.get("max_hp", 0)]
	var hud: Node = world.get("hud")
	_result["inventory_stacks"] = (hud.get("inventory_items") as Array).size()
	_result["coin"] = hud.get("inventory_coin")
	var combat_lines: Array = world.get("chat").get("combat_lines")
	_result["combat_log_sample"] = combat_lines.slice(maxi(0, combat_lines.size() - 12))
	var expected: int = int(opts["expect-peers"])
	var ok: bool = seen.size() >= expected and int(_result["snapshots"]) > 0 and int(_result["last_ack"]) > 0
	if _kills_needed > 0:
		ok = ok and int(_result["kills"]) >= _kills_needed and _looted_kills >= _kills_needed and int(_result["abilities_used"]) > 0
	if int(opts["caster"]) > 0:
		ok = ok and _result.has("scribed") and _result.has("memorized") and _result.has("self_buff") \
			and int(_result.get("casts_completed", 0)) >= _kills_needed
	if int(opts["pet"]) > 0:
		ok = ok and _result.has("pet") and int(_result.get("pet_commands", 0)) > 0 and _looted_kills >= _kills_needed
	if int(opts["bard"]) > 0:
		ok = ok and int(_result.get("songs_started", 0)) > 0 and _looted_kills >= _kills_needed
	if str(opts["quest"]) == "true":
		ok = ok and _result.has("quest_offered") and _result.has("quest_accepted") and bool(_result.get("journal_shown", false)) \
			and bool(_result.get("map_shown", false)) and bool(_result.get("minimap", false)) and int(_result.get("tracker_lines", 0)) >= 1 \
			and bool(_result.get("windows_on_screen", false)) and _result.has("quest_abandoned")
	if str(opts["social"]) != "":
		ok = ok and int(_result.get("group_members_max", 0)) >= 2 and int(_result.get("guild_members_max", 0)) >= 2 \
			and bool(_result.get("heard_group", false)) and bool(_result.get("heard_guild", false)) \
			and bool(_result.get("windows_on_screen", false)) and bool(_result.get("windows_open", false))
		if str(opts["social"]) == "lead":
			ok = ok and bool(_result.get("who_saw_partner", false))
	if str(opts["zone-walk"]) == "true":
		ok = ok and _zones_seen == ["emberhold", "emberglade", "rattlebone_warren", "emberglade"] and not _layout_bad \
			and int(_result.get("zone_corrections", 99)) <= 2
		_result["zones_seen"] = _zones_seen
		_result["layout_mismatch"] = _layout_bad
	if str(opts["items"]) == "true":
		ok = ok and _result.has("equipped") and _result.has("sold") and _result.has("banked") and _result.has("withdrew")
		ok = ok and _result.has("listed") and _result.has("auction_cancelled") and _result.has("consumed")
	if str(opts["die-at"]) != "":
		ok = ok and int(_result["deaths"]) >= 1 and _recovered and (_died_with == 0 or int(_result["recovered_items"]) >= _died_with)
	_result["visuals"] = _visual_report()
	var visual_problem: String = _visual_problems()
	if visual_problem != "":
		ok = false
		_result["error"] = visual_problem
		_log("autotest: " + visual_problem)
	if str(opts["expect-error"]) != "":
		# A negative test must not pass just because the happy path worked.
		ok = false
		_result["error"] = "expected login to fail with '%s', but it succeeded" % opts["expect-error"]
	_result["ok"] = ok
	_log("autotest: saw %s; snapshots=%d corrections=%d acked=%d/%d" % [
		str(seen.keys()), int(_result["snapshots"]), int(_result["corrections"]), int(_result["last_ack"]), int(_result["inputs_sent"])])
	if str(opts["screenshot"]) != "" and DisplayServer.get_name() != "headless" and not _result.has("combat_screenshot"):
		await RenderingServer.frame_post_draw
		var img: Image = get_viewport().get_texture().get_image()
		var err: Error = img.save_png(str(opts["screenshot"]))
		_log("autotest: screenshot %s (%s)" % [opts["screenshot"], error_string(err)])
	if not _write_result():
		_result["ok"] = false
	var linger: float = float(opts["linger"])
	if linger > 0.0:
		await get_tree().create_timer(linger).timeout
	Session.logout()
	_state = "camping"


## What a character screen's model preview shows (race/gender/class, and whether a real model loaded).
func _note_preview(key: String, screen: Node) -> void:
	var preview: Node = screen.get("preview") as Node
	if preview == null:
		return
	var v: Node = preview.get("visual") as Node
	_result[key] = {"shown": str(preview.get("shown_key")), "model": v != null and not bool(v.get("is_placeholder"))}


## Every mob model seen during the run -> whether it loaded a real model (a camp emptied by earlier
## runs must not make the report look as if mobs never had models).
var _mob_models_seen: Dictionary = {}
var _next_model_note: float = 0.0


func _note_mob_models(world: Node) -> void:
	for r: Node in (world.get("remotes") as Dictionary).values():
		var rv: Node = r.get("visual") as Node
		if rv != null and int(r.get("kind")) == 1:
			_mob_models_seen[str(r.get("model"))] = bool(_mob_models_seen.get(str(r.get("model")), false)) or not bool(rv.get("is_placeholder"))


## Models and animation states seen during the run (every character, ours included).
func _visual_report() -> Dictionary:
	var report: Dictionary = {"stats": CharacterVisual.stats.duplicate(), "states": CharacterVisual.states_seen.keys()}
	var world: Node = _screen()
	if world != null and world.name == "World":
		var lp: Node = world.get("local_player") as Node
		var v: Node = lp.get("visual") as Node if lp != null else null
		if v != null:
			report["me"] = {"model": str((v.get("model_def") as Dictionary).get("id", "")), "placeholder": bool(v.get("is_placeholder")),
				"held": (v.get("attached") as Dictionary).keys(), "playing": str(v.get("playing"))}
		_note_mob_models(world)
		report["mob_models"] = _mob_models_seen
	return report


## The model/animation expectations from --expect-anims and --expect-models ("" when met).
func _visual_problems() -> String:
	var v: Dictionary = _result.get("visuals", {})
	var missing: Array[String] = []
	for s: String in str(opts["expect-anims"]).split(",", false):
		if not (v.get("states", []) as Array).has(s.strip_edges()):
			missing.append(s.strip_edges())
	if not missing.is_empty():
		return "animation states never played: " + ", ".join(missing)
	if str(opts["expect-models"]) == "true":
		if int((v.get("stats", {}) as Dictionary).get("models", 0)) == 0:
			return "no character loaded a real model"
		if bool((v.get("me", {}) as Dictionary).get("placeholder", true)) and v.has("me"):
			return "our own character is a placeholder"
		var mobs: Dictionary = v.get("mob_models", {})
		if not mobs.is_empty() and not mobs.values().has(true):
			return "every mob in view is a placeholder"
	return ""


## Returns false if a requested result file could not be written (which fails the run).
func _write_result() -> bool:
	_result["visuals"] = _visual_report()
	var path: String = str(opts["result"])
	if path == "":
		return true
	var f: FileAccess = FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_error("autotest: cannot write %s (%s)" % [path, error_string(FileAccess.get_open_error())])
		return false
	f.store_string(JSON.stringify(_result, "  "))
	f.close()
	return true


func _finish(ok: bool, reason: String) -> void:
	var expected: String = str(opts["expect-error"])
	if not ok and expected != "" and reason.find(expected) >= 0:
		ok = true
		reason = "failed as expected: " + reason
	_result["ok"] = ok
	_result["error"] = reason
	_log("autotest: FINISHED ok=%s %s" % [ok, reason])
	if not _write_result():
		_result["ok"] = false
	Session.quit_session(reason)
	_quit()


func _quit() -> void:
	if _done:
		return
	_done = true
	var ok: bool = bool(_result.get("ok", false))
	_log("autotest: exiting with %s" % ("PASS" if ok else "FAIL"))
	get_tree().quit(0 if ok else 1)


# ------------------------------------------------------------------------------ items run (--items)

## --items: the Phase 5 economy through the real UI code. Check the starting kit is worn, walk to the
## novice outfitter, buy a chest piece our class can wear, equip it (AC must rise), sell food back,
## eat and drink, bank an item and some coin and take them back, then list an item on the auction
## board and cancel the listing.
const ARMOR_OF: Dictionary = {"warrior": "plate", "paladin": "plate", "shadow_knight": "plate", "cleric": "plate", "ranger": "chain",
	"bard": "chain", "rogue": "chain", "shaman": "chain", "monk": "leather", "druid": "leather", "wizard": "cloth", "magician": "cloth",
	"necromancer": "cloth", "enchanter": "cloth"}
var _items_step: String = ""
var _items_t: float = 0.0
var _items_npc: int = 0
var _items_mark: Dictionary = {}


func _items_goto(step: String) -> void:
	_items_step = step
	_items_t = _state_time
	_log("autotest: items -> " + step)


func _items_ui(world: Node) -> Node:
	return (world.get("hud") as Node).get("items") as Node


func _walk_to_npc(world: Node, lp: Node3D, role: String, name_part: String, next: String) -> void:
	var remotes: Dictionary = world.get("remotes")
	var npc: int = int(world.call("nearest", 3, func(r: Node3D) -> bool:
		return str(r.get("class_id")) == role and (name_part == "" or str(r.get("display_name")).contains(name_part)), 120.0))
	if npc == 0:
		if _state_time - _items_t > 8.0:
			var seen: Array[String] = []
			for r: Node3D in remotes.values():
				if int(r.get("kind")) == 3:
					seen.append("%s<%s>" % [str(r.get("display_name")), str(r.get("class_id"))])
			_finish(false, "no %s '%s' in view (npcs: %s)" % [role, name_part, ", ".join(seen)])
		_go(lp, Vector3(0, 0, 0), 2.0)
		return
	var n: Node3D = remotes[npc]
	if n.position.distance_to(lp.position) > 6.0:
		_go(lp, n.position, 4.0)
		return
	lp.set("goto_active", false)
	_items_npc = npc
	world.call("set_target", npc)
	world.call_deferred("interact")
	_items_goto(next)


func _items_run(world: Node, lp: Node3D) -> void:
	var ui: Node = _items_ui(world)
	var since: float = _state_time - _items_t
	var coin: int = int(ui.get("coin"))
	if since > 25.0 and _items_step != "done":
		_finish(false, "items run stuck at step '%s'" % _items_step)
		return
	match _items_step:
		"settle":
			if since > 1.5 and not (world.get("sheet") as Dictionary).is_empty():
				var worn: Dictionary = ui.call("worn_by_index")
				_result["kit_worn"] = worn.values()
				_log("autotest: starting kit worn: %s" % str(worn.values()))
				if worn.is_empty() and str(opts["class"]) != "monk":
					_finish(false, "the starting kit is not worn")
					return
				_items_goto("to_vendor")
		"to_vendor":
			_walk_to_npc(world, lp, "vendor", "Tamsin", "await_vendor")
		"await_vendor":
			if not (world.get("last_vendor") as Dictionary).is_empty():
				_result["vendor"] = str((world.get("last_vendor") as Dictionary)["name"])
				_items_goto("buy")
		"buy":
			var want: String = "%s_t1_chest" % str(ARMOR_OF.get(str(opts["class"]), "cloth"))
			_items_mark = {"item": want, "coin": coin, "ac": int((world.get("sheet") as Dictionary).get("ac", 0))}
			world.call("vendor_buy", _items_npc, want, 1, false)
			_items_goto("await_buy")
		"await_buy":
			if int(ui.call("count_of", str(_items_mark["item"]))) > 0:
				_result["bought"] = {"item": _items_mark["item"], "paid": int(_items_mark["coin"]) - coin}
				_log("autotest: bought %s for %d copper" % [str(_items_mark["item"]), int(_items_mark["coin"]) - coin])
				world.call("use_item", int(ui.call("find_item", str(_items_mark["item"]), false)))
				_items_goto("await_equip")
		"await_equip":
			var ac: int = int((world.get("sheet") as Dictionary).get("ac", 0))
			if (ui.call("worn_by_index") as Dictionary).get(5, "") == str(_items_mark["item"]) and ac > int(_items_mark["ac"]):
				_result["equipped"] = {"item": _items_mark["item"], "ac_before": _items_mark["ac"], "ac_after": ac}
				_log("autotest: equipped %s: AC %d -> %d" % [str(_items_mark["item"]), int(_items_mark["ac"]), ac])
				_items_mark = {"coin": coin, "food": int(ui.call("count_of", "trail_rations"))}
				world.call("vendor_sell", _items_npc, int(ui.call("find_item", "trail_rations", false)), 2)
				_items_goto("await_sell")
		"await_sell":
			if int(ui.call("count_of", "trail_rations")) == int(_items_mark["food"]) - 2 and coin > int(_items_mark["coin"]):
				_result["sold"] = {"item": "trail_rations", "count": 2, "earned": coin - int(_items_mark["coin"])}
				_log("autotest: sold 2 trail rations for %d copper" % (coin - int(_items_mark["coin"])))
				_items_mark = {"water": int(ui.call("count_of", "water_flask"))}
				world.call("use_item", int(ui.call("find_item", "water_flask", false)))
				_items_goto("await_drink")
		"await_drink":
			if int(ui.call("count_of", "water_flask")) == int(_items_mark["water"]) - 1 and bool((world.get("hud") as Node).get("spells").call("has_self_buff", "fx_water_flask")):
				_result["consumed"] = "water_flask"
				_items_goto("to_bank")
		"to_bank":
			_walk_to_npc(world, lp, "banker", "", "await_bank")
		"await_bank":
			if bool(world.get("bank_open")):
				_items_mark = {"code": int(ui.call("find_item", "trail_rations", false)), "coin": coin}
				world.call("move_item", int(_items_mark["code"]), 0x0200, 0)
				world.call("bank_coin", true, 5)
				_items_goto("await_deposit")
		"await_deposit":
			var items: Dictionary = ui.get("items")
			if items.has(0x0200) and str((items[0x0200] as Dictionary)["item_id"]) == "trail_rations" and int(ui.get("bank_coin_amount")) == 5:
				_result["banked"] = {"item": "trail_rations", "coin": 5}
				_log("autotest: banked trail rations and 5 copper")
				world.call("move_item", 0x0200, int(_items_mark["code"]), 0)
				world.call("bank_coin", false, 5)
				_items_goto("await_withdraw")
		"await_withdraw":
			var items2: Dictionary = ui.get("items")
			if not items2.has(0x0200) and int(ui.get("bank_coin_amount")) == 0 and coin == int(_items_mark["coin"]):
				_result["withdrew"] = true
				_items_goto("to_auction")
		"to_auction":
			_walk_to_npc(world, lp, "auctioneer", "", "await_auction")
		"await_auction":
			if int(world.get("auction_npc")) != 0 and not (world.get("last_auction_results") as Dictionary).is_empty():
				_items_mark = {"coin": coin, "food": int(ui.call("count_of", "trail_rations"))}
				world.call("auction_post", int(world.get("auction_npc")), int(ui.call("find_item", "trail_rations", false)), "trail_rations", 40)
				_items_goto("await_post")
		"await_post":
			var r: Dictionary = world.get("last_auction_results")
			if bool(r.get("mine", false)) and (r["entries"] as Array).size() == 1 and int(ui.call("count_of", "trail_rations")) == 0:
				var e: Dictionary = (r["entries"] as Array)[0]
				_result["listed"] = {"item": str(e["item_id"]), "price": int(e["price"]), "fee": int(_items_mark["coin"]) - coin}
				_log("autotest: listed %s for %d (fee %d)" % [str(e["item_id"]), int(e["price"]), int(_items_mark["coin"]) - coin])
				if not bool(ui.call("is_inventory_open")):
					(world.get("hud") as Node).call("toggle_inventory")
				world.call("auction_cancel", int(world.get("auction_npc")), int(e["id"]))
				_items_goto("await_cancel")
		"await_cancel":
			if int(ui.call("count_of", "trail_rations")) == int(_items_mark["food"]):
				_result["auction_cancelled"] = true
				_log("autotest: listing cancelled, the rations are back")
				_items_goto("done")
		"done":
			if since > 0.5:
				_phase = "report"
				_capture_and_report()


# ------------------------------------------------------------------------------ social run (Phase 7)

## --social lead|join --partner NAME [--guild "Guild Name"]: two clients, run together. The leader finds the
## partner with /who, invites them into a group, founds a guild and invites them into it; both speak on the
## group and guild channels and must hear each other; each opens the group, guild and friends windows (and
## the leader the /who window) for a screenshot. The leader disbands the guild at the end, so the run repeats.
var _social_heard: Dictionary = {}
var _social_next: float = 0.0


func _chat_has(world: Node, needle: String) -> bool:
	for l: Variant in world.get("chat").get("lines"):
		if str(l).contains(needle):
			return true
	return false


func _social_run(world: Node) -> void:
	var hud: Node = world.get("hud") as Node
	var social: Node = hud.get("social") as Node
	var chat: Node = world.get("chat") as Node
	var role: String = str(opts["social"])
	var partner: String = str(opts["partner"])
	var guild_name: String = str(opts["guild"]) if str(opts["guild"]) != "" else "Autotest Fellows"
	var since: float = _state_time - _items_t
	var group: Dictionary = world.get("group_state")
	var guild: Dictionary = world.get("guild_info")
	var group_size: int = (group.get("members", []) as Array).size() if int(group.get("group_id", 0)) != 0 else 0
	var guild_size: int = (guild.get("members", []) as Array).size() if int(guild.get("guild_id", 0)) != 0 else 0
	_result["group_members_max"] = maxi(int(_result.get("group_members_max", 0)), group_size)
	_result["guild_members_max"] = maxi(int(_result.get("guild_members_max", 0)), guild_size)
	if since > 60.0 and _items_step != "done":
		_finish(false, "social run (%s) stuck at step '%s'" % [role, _items_step])
		return
	match _items_step:
		"settle":
			if since > 1.5:
				chat.call("say", "/ooc Hello from %s" % str(world.get("local_player").get("display_name")))
				_items_goto("who" if role == "lead" else "wait_group")
		# ---- leader
		"who":
			if _state_time >= _social_next:
				_social_next = _state_time + 2.0
				chat.call("say", "/who all")
			var who: Dictionary = world.get("who_results")
			for e: Variant in who.get("entries", []):
				if str((e as Dictionary)["name"]) == partner:
					_result["who_saw_partner"] = true
					chat.call("say", "/invite " + partner)
					_social_next = _state_time + 10.0
					_items_goto("grouping")
		"grouping":
			if group_size >= 2:
				_log("autotest: grouped with %s" % partner)
				chat.call("say", "/guildcreate " + guild_name)
				_items_goto("founding")
			elif _state_time >= _social_next:
				_social_next = _state_time + 10.0
				chat.call("say", "/invite " + partner)  # the partner may not have been in the world yet
		"founding":
			if str(guild.get("name", "")) != "":
				_result["guild_name"] = str(guild["name"])
				chat.call("say", "/guildinvite " + partner)
				_social_next = _state_time + 10.0
				_items_goto("recruiting")
		"recruiting":
			if guild_size >= 2:
				chat.call("say", "/g group check from the leader")
				chat.call("say", "/gu guild check from the leader")
				chat.call("say", "/friend " + partner)
				_items_goto("listening")
			elif _state_time >= _social_next:
				_social_next = _state_time + 10.0
				chat.call("say", "/guildinvite " + partner)
		# ---- joiner
		"wait_group":
			var inv: Dictionary = world.get("last_invite")
			if group_size >= 2:
				_items_goto("wait_guild")
			elif not inv.is_empty() and int(inv["kind"]) == 0 and bool(social.call("invite_visible")):
				_result["group_invite_from"] = str(inv["from_name"])
				social.call("answer_invite", true)  # the popup's Accept button
		"wait_guild":
			var inv: Dictionary = world.get("last_invite")
			if guild_size >= 2:
				_result["guild_name"] = str(guild["name"])
				chat.call("say", "/g ready from the member")
				chat.call("say", "/gu ready from the member")
				_items_goto("listening")
			elif not inv.is_empty() and int(inv["kind"]) == 1 and bool(social.call("invite_visible")):
				_result["guild_invite_from"] = str(inv["from_name"])
				social.call("answer_invite", true)
		# ---- both
		"listening":
			var want_group: String = "%s tells the group" % partner
			var want_guild: String = "%s tells the guild" % partner
			_result["heard_group"] = _chat_has(world, want_group)
			_result["heard_guild"] = _chat_has(world, want_guild)
			if bool(_result["heard_group"]) and bool(_result["heard_guild"]):
				if not bool(social.call("group_visible")):
					social.call("toggle_group")
				social.call("toggle_guild")
				social.call("toggle_friends")
				chat.call("show_tab_named", "Group")
				_items_goto("windows")
		"windows":
			if since > 1.5:
				var view_ok: bool = DisplayServer.get_name() == "headless" or bool(social.call("windows_on_screen"))
				_result["windows_on_screen"] = view_ok
				_result["windows_open"] = bool(social.call("group_visible")) and bool(social.call("guild_visible")) and bool(social.call("friends_visible"))
				if str(opts["screenshot"]) != "" and DisplayServer.get_name() != "headless":
					var shot: String = str(opts["screenshot"]).get_basename() + "_social.png"
					var img: Image = get_viewport().get_texture().get_image()
					_log("autotest: screenshot %s (%s)" % [shot, error_string(img.save_png(shot))])
				_items_goto("cleanup")
		"cleanup":
			# The leader waits for the member's screenshot before taking the guild down.
			if role == "lead" and since > 4.0:
				chat.call("say", "/guilddisband")
				chat.call("say", "/disband")
				_items_goto("done")
			elif role != "lead" and since > 0.5:
				_items_goto("done")
		"done":
			if since > 1.0:
				social.call("close_windows")
				_phase = "report"
				_capture_and_report()
