## Command-line driven end-to-end test of the real client UI flow. Enabled with:
##   EmberfallClient.exe -- --autotest --server HOST:PORT --user U --pass P --char Name
##        [--race R --class C --city C] [--walk SECONDS] [--say TEXT] [--expect-peers N]
##        [--screenshot PATH] [--result PATH] [--timeout SECONDS] [--linger SECONDS]
##        [--reset-pins] [--expect-error TEXT]
##        [--fight N] [--camp X,Z] [--die-at X,Z] [--death-screenshot PATH] [--show-windows]
## --fight N: target, chase, and kill N level-appropriate mobs near --camp (auto-attack plus
## hotbar slot 2), looting every corpse. --die-at: then walk there, pick a fight far above our
## level, die, press Respawn, walk back to the corpse, and loot it.
## Certificate pins go to user://autotest_known_servers.cfg (--reset-pins clears it first).
## With --expect-error the run passes only if login fails with a message containing TEXT.
## Registers (or logs in), creates the character if needed, enters the world, walks in a
## circle, optionally chats and screenshots, writes a JSON result, camps, and quits.
extends Node

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
		"death-screenshot": "", "show-windows": "false",
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
				hud.call("toggle_abilities")
				hud.call("toggle_inventory")
			_kills_needed = int(opts["fight"])
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
				screen.call("autotest_create", str(opts["char"]), str(opts["race"]), str(opts["class"]), str(opts["city"]))
			elif screen.name == "CharSelectScreen":
				_goto("char_select")
		"in_world":
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
	if str(opts["die-at"]) != "":
		ok = ok and int(_result["deaths"]) >= 1 and _recovered and (_died_with == 0 or int(_result["recovered_items"]) >= _died_with)
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


## Returns false if a requested result file could not be written (which fails the run).
func _write_result() -> bool:
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
