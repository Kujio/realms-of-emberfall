## Autoload "Audio" (Phase 8): plays named sound events from res://data/sounds.json on the mixer buses
## the Settings autoload creates. A small pool of players is reused; a missing event or file is silent.
extends Node

const SOUNDS_PATH: String = "res://data/sounds.json"
const POOL_SIZE: int = 12

var _events: Dictionary = {}
var _streams: Dictionary = {}
var _pool: Array[AudioStreamPlayer] = []
var _next: int = 0
var _loops: Dictionary = {}  # event -> AudioStreamPlayer
var played: int = 0  # for tests and the autotest driver


func _ready() -> void:
	_events = load_events(SOUNDS_PATH)
	for i: int in POOL_SIZE:
		var p: AudioStreamPlayer = AudioStreamPlayer.new()
		add_child(p)
		_pool.append(p)


static func load_events(path: String) -> Dictionary:
	var f: FileAccess = FileAccess.open(path, FileAccess.READ)
	if f == null:
		return {}
	var parsed: Variant = JSON.parse_string(f.get_as_text())
	if parsed is Dictionary and (parsed as Dictionary).get("events") is Dictionary:
		return (parsed as Dictionary)["events"]
	return {}


func has_event(event: String) -> bool:
	return _events.has(event)


func _stream(event: String) -> AudioStream:
	if _streams.has(event):
		return _streams[event]
	var def: Dictionary = _events.get(event, {})
	var path: String = str(def.get("file", ""))
	var s: AudioStream = null
	if path != "" and ResourceLoader.exists(path):
		s = load(path) as AudioStream
		var wav: AudioStreamWAV = s as AudioStreamWAV
		if wav != null and bool(def.get("loop", false)):
			wav = wav.duplicate() as AudioStreamWAV
			wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
			wav.loop_begin = 0
			wav.loop_end = int(wav.get_length() * float(wav.mix_rate))
			s = wav
	_streams[event] = s
	return s


## One-shot sound. `pitch_jitter` varies repeated sounds (hits) slightly.
func play(event: String, pitch_jitter: float = 0.0) -> void:
	var s: AudioStream = _stream(event)
	if s == null or _pool.is_empty():
		return
	var def: Dictionary = _events[event]
	var p: AudioStreamPlayer = _pool[_next]
	_next = (_next + 1) % _pool.size()
	p.stream = s
	p.bus = str(def.get("bus", "SFX"))
	p.volume_db = float(def.get("volume_db", 0.0))
	p.pitch_scale = 1.0 + randf_range(-pitch_jitter, pitch_jitter)
	p.play()
	played += 1


## A looping sound (ambience) until stop_loop(); starting it again is a no-op.
func play_loop(event: String) -> void:
	if _loops.has(event):
		return
	var s: AudioStream = _stream(event)
	if s == null:
		return
	var def: Dictionary = _events[event]
	var p: AudioStreamPlayer = AudioStreamPlayer.new()
	p.stream = s
	p.bus = str(def.get("bus", "Music"))
	p.volume_db = float(def.get("volume_db", 0.0))
	add_child(p)
	p.play()
	_loops[event] = p


func stop_loop(event: String) -> void:
	var p: AudioStreamPlayer = _loops.get(event)
	if p != null:
		p.queue_free()
	_loops.erase(event)
