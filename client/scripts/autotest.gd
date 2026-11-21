## Command-line driven end-to-end test of the real client UI flow. Enabled with:
##   EmberfallClient.exe -- --autotest --server HOST:PORT --user U --pass P --char Name
##        [--race R --class C --city C] [--walk SECONDS] [--say TEXT] [--expect-peers N]
##        [--screenshot PATH] [--result PATH] [--timeout SECONDS] [--linger SECONDS]
##        [--reset-pins] [--expect-error TEXT]
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
		"reset-pins": "false", "expect-error": "",
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
	var expected: int = int(opts["expect-peers"])
	var ok: bool = seen.size() >= expected and int(_result["snapshots"]) > 0 and int(_result["last_ack"]) > 0
	if str(opts["expect-error"]) != "":
		# A negative test must not pass just because the happy path worked.
		ok = false
		_result["error"] = "expected login to fail with '%s', but it succeeded" % opts["expect-error"]
	_result["ok"] = ok
	_log("autotest: saw %s; snapshots=%d corrections=%d acked=%d/%d" % [
		str(seen.keys()), int(_result["snapshots"]), int(_result["corrections"]), int(_result["last_ack"]), int(_result["inputs_sent"])])
	if str(opts["screenshot"]) != "" and DisplayServer.get_name() != "headless":
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
