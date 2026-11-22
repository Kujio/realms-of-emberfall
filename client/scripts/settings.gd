## Autoload "Settings" (Phase 8): video, audio and key options, saved to user://options.cfg and applied
## at startup. The settings window edits these and calls apply_*/save().
extends Node

signal changed

const PATH: String = "user://options.cfg"
const WINDOW_MODES: Array[String] = ["windowed", "fullscreen", "exclusive"]
const RESOLUTIONS: Array[Vector2i] = [
	Vector2i(1280, 720), Vector2i(1366, 768), Vector2i(1600, 900), Vector2i(1920, 1080),
	Vector2i(2560, 1440), Vector2i(3840, 2160)]
const BUSES: Array[String] = ["Music", "SFX", "UI"]

var window_mode: String = "windowed"
var resolution: Vector2i = Vector2i(1280, 720)
var vsync: bool = true
## Linear 0..1 per bus ("Master" included).
var volumes: Dictionary = {"Master": 0.8, "Music": 0.5, "SFX": 0.8, "UI": 0.7}
var muted: bool = false
var binds: Dictionary = {}


func _ready() -> void:
	ensure_buses()
	load_options()
	apply_binds()
	apply_audio()
	apply_video()


## The mixer: Music, SFX and UI buses under Master (created once, at startup).
static func ensure_buses() -> void:
	for bus: String in BUSES:
		if AudioServer.get_bus_index(bus) == -1:
			AudioServer.add_bus()
			var idx: int = AudioServer.bus_count - 1
			AudioServer.set_bus_name(idx, bus)
			AudioServer.set_bus_send(idx, "Master")


func load_options() -> void:
	var cfg: ConfigFile = ConfigFile.new()
	var saved: Dictionary = {}
	if cfg.load(PATH) == OK:
		var mode: String = str(cfg.get_value("video", "window_mode", window_mode))
		window_mode = mode if WINDOW_MODES.has(mode) else "windowed"
		var res: Variant = cfg.get_value("video", "resolution", resolution)
		if res is Vector2i:
			resolution = res
		vsync = bool(cfg.get_value("video", "vsync", vsync))
		for bus: String in volumes.keys():
			volumes[bus] = clampf(float(cfg.get_value("audio", bus, volumes[bus])), 0.0, 1.0)
		muted = bool(cfg.get_value("audio", "muted", false))
		if cfg.has_section("keys"):
			for action: String in cfg.get_section_keys("keys"):
				saved[action] = cfg.get_value("keys", action, "")
	binds = Keybinds.from_config(saved)


func save() -> void:
	var cfg: ConfigFile = ConfigFile.new()
	cfg.set_value("video", "window_mode", window_mode)
	cfg.set_value("video", "resolution", resolution)
	cfg.set_value("video", "vsync", vsync)
	for bus: String in volumes.keys():
		cfg.set_value("audio", bus, volumes[bus])
	cfg.set_value("audio", "muted", muted)
	var keys: Dictionary = Keybinds.to_config(binds)
	for action: String in keys.keys():
		cfg.set_value("keys", action, keys[action])
	cfg.save(PATH)
	changed.emit()


func apply_video() -> void:
	if DisplayServer.get_name() == "headless":
		return
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_ENABLED if vsync else DisplayServer.VSYNC_DISABLED)
	match window_mode:
		"fullscreen":
			DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN)
		"exclusive":
			DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_EXCLUSIVE_FULLSCREEN)
		_:
			DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
			var screen: Vector2i = DisplayServer.screen_get_size()
			var size: Vector2i = Vector2i(mini(resolution.x, screen.x), mini(resolution.y, screen.y))
			DisplayServer.window_set_size(size)
			DisplayServer.window_set_position((screen - size) / 2)


static func linear_to_db_safe(v: float) -> float:
	return -80.0 if v <= 0.001 else linear_to_db(v)


func apply_audio() -> void:
	for bus: String in volumes.keys():
		var idx: int = AudioServer.get_bus_index(bus)
		if idx == -1:
			continue
		AudioServer.set_bus_volume_db(idx, linear_to_db_safe(float(volumes[bus])))
		AudioServer.set_bus_mute(idx, muted and bus == "Master")


## Mirrors `binds` into Godot's InputMap (physical keys, so layouts other than QWERTY keep positions).
func apply_binds() -> void:
	for action: String in Keybinds.action_ids():
		if InputMap.has_action(action):
			InputMap.action_erase_events(action)
		else:
			InputMap.add_action(action)
		for k: int in binds.get(action, []):
			var ev: InputEventKey = InputEventKey.new()
			ev.physical_keycode = k as Key
			InputMap.action_add_event(action, ev)


func rebind(action: String, key: int) -> bool:
	var changed_actions: Array[String] = Keybinds.rebind(binds, action, key)
	if changed_actions.is_empty():
		return false
	apply_binds()
	return true


func reset_binds() -> void:
	binds = Keybinds.defaults()
	apply_binds()
