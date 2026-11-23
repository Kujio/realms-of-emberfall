## Autoload "Audio" (Phase 8): plays named sound events from res://data/sounds.json on the mixer buses
## the Settings autoload creates. A small pool of players is reused; a missing event or file is silent.
## Phase 9: an event may list several files (one is picked each time) and a pitch; MP3 and Ogg loops;
## a music and an ambience channel that crossfade, chosen per zone (zone id, then biome) from the same
## file (content/audio.json); footsteps per surface; cast and impact sounds per resist school; and the
## boss names, so boss fights get their own music.
extends Node

const SOUNDS_PATH: String = "res://data/sounds.json"
const POOL_SIZE: int = 12

const FADE_S: float = 2.0
const SCHOOLS: Array[String] = ["none", "magic", "fire", "cold", "poison", "disease"]

var _doc: Dictionary = {}
var _events: Dictionary = {}
## The looping event each channel ("music", "ambience") plays now ("" = silent), and its player.
var channel_event: Dictionary = {"music": "", "ambience": ""}
var _channel_player: Dictionary = {}
var _streams: Dictionary = {}
var _pool: Array[AudioStreamPlayer] = []
var _next: int = 0
var _loops: Dictionary = {}  # event -> AudioStreamPlayer
var played: int = 0  # for tests and the autotest driver


func _ready() -> void:
	_doc = load_doc(SOUNDS_PATH)
	_events = _doc.get("events", {}) as Dictionary
	for i: int in POOL_SIZE:
		var p: AudioStreamPlayer = AudioStreamPlayer.new()
		add_child(p)
		_pool.append(p)


static func load_doc(path: String) -> Dictionary:
	var f: FileAccess = FileAccess.open(path, FileAccess.READ)
	if f == null:
		return {}
	var parsed: Variant = JSON.parse_string(f.get_as_text())
	if parsed is Dictionary and (parsed as Dictionary).get("events") is Dictionary:
		return parsed as Dictionary
	return {}


static func load_events(path: String) -> Dictionary:
	return load_doc(path).get("events", {}) as Dictionary


func has_event(event: String) -> bool:
	return _events.has(event)


## One of the event's files (a random one when it lists several), loaded once and set to loop when the
## event loops.
func _stream(event: String) -> AudioStream:
	var def: Dictionary = _events.get(event, {})
	var files: Array = def.get("files", []) as Array
	var path: String = str(files[randi() % files.size()]) if not files.is_empty() else str(def.get("file", ""))
	var key: String = event + "|" + path
	if _streams.has(key):
		return _streams[key]
	var s: AudioStream = null
	if path != "" and ResourceLoader.exists(path):
		s = load(path) as AudioStream
		if bool(def.get("loop", false)) and s != null:
			s = s.duplicate() as AudioStream
			if s is AudioStreamWAV:
				var wav: AudioStreamWAV = s as AudioStreamWAV
				wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
				wav.loop_begin = 0
				wav.loop_end = int(wav.get_length() * float(wav.mix_rate))
			elif s is AudioStreamMP3:
				(s as AudioStreamMP3).loop = true
			elif s is AudioStreamOggVorbis:
				(s as AudioStreamOggVorbis).loop = true
	_streams[key] = s
	return s


## One-shot sound. `pitch_jitter` varies repeated sounds (hits) slightly; `pitch` shifts it (schools).
func play(event: String, pitch_jitter: float = 0.0, pitch: float = 1.0) -> void:
	var s: AudioStream = _stream(event)
	if s == null or _pool.is_empty():
		return
	var def: Dictionary = _events[event]
	var p: AudioStreamPlayer = _pool[_next]
	_next = (_next + 1) % _pool.size()
	p.stream = s
	p.bus = str(def.get("bus", "SFX"))
	p.volume_db = float(def.get("volume_db", 0.0))
	p.pitch_scale = float(def.get("pitch", 1.0)) * pitch * (1.0 + randf_range(-pitch_jitter, pitch_jitter))
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


# ------------------------------------------------------------------------------ music and ambience

## Plays `event` on a channel ("music" or "ambience"), fading the old one out and the new one in;
## the same event again is a no-op, "" fades the channel out.
func set_channel(channel: String, event: String) -> void:
	if str(channel_event.get(channel, "")) == event:
		return
	channel_event[channel] = event
	var old: AudioStreamPlayer = _channel_player.get(channel) as AudioStreamPlayer
	if old != null and is_instance_valid(old):
		var out: Tween = old.create_tween()
		out.tween_property(old, "volume_db", -60.0, FADE_S)
		out.tween_callback(old.queue_free)
	_channel_player.erase(channel)
	if event == "":
		return
	var s: AudioStream = _stream(event)
	if s == null:
		return
	var def: Dictionary = _events[event]
	var p: AudioStreamPlayer = AudioStreamPlayer.new()
	p.stream = s
	p.bus = str(def.get("bus", "Music"))
	p.volume_db = -60.0
	add_child(p)
	p.play()
	var target: float = float(def.get("volume_db", 0.0))
	p.create_tween().tween_property(p, "volume_db", target, FADE_S)
	_channel_player[channel] = p


## The music or ambience event a zone plays: by zone id, then by biome (a list picks one by the zone's
## id, so neighbouring zones of a biome vary), then the default.
func zone_event(kind: String, zone_id: String, biome: String) -> String:
	var sec: Dictionary = _doc.get(kind, {}) as Dictionary
	var v: Variant = (sec.get("zones", {}) as Dictionary).get(zone_id)
	if v == null:
		v = (sec.get("biomes", {}) as Dictionary).get(biome, sec.get("default", ""))
	if v is Array:
		var a: Array = v as Array
		return str(a[absi(hash(zone_id)) % a.size()]) if not a.is_empty() else ""
	return str(v)


## 0 = not a boss, 1 = a boss, 2 = a raid boss (by mob name).
func boss_kind(mob_name: String) -> int:
	var b: Variant = (_doc.get("bosses", {}) as Dictionary).get(mob_name)
	return 0 if b == null else (2 if bool(b) else 1)


## The music for a fight: a boss's or raid boss's, the combat music for a mob `level_gap` above us,
## or "" (the zone's own music).
func fight_music(mob_name: String, level_gap: int) -> String:
	match boss_kind(mob_name):
		2:
			return str(_doc.get("raid_boss_music", ""))
		1:
			return str(_doc.get("boss_music", ""))
	var cm: Dictionary = _doc.get("combat_music", {}) as Dictionary
	if not cm.is_empty() and level_gap >= int(cm.get("min_level_gap", 99)):
		return str(cm.get("event", ""))
	return ""


## The footstep event for a biome's surface, and the distance between steps.
func step_event(biome: String) -> String:
	var fs: Dictionary = _doc.get("footsteps", {}) as Dictionary
	var surface: String = str((fs.get("biomes", {}) as Dictionary).get(biome, fs.get("default", "stone")))
	return str((fs.get("surfaces", {}) as Dictionary).get(surface, ""))


func step_metres() -> float:
	return float((_doc.get("footsteps", {}) as Dictionary).get("step_metres", 1.7))


## A spell's cast or impact sound for its resist school (index 0..5), pitched per school.
func play_school(which: String, school: int) -> void:
	var sc: Dictionary = (_doc.get("schools", {}) as Dictionary).get(SCHOOLS[clampi(school, 0, SCHOOLS.size() - 1)], {}) as Dictionary
	var ev: String = str(sc.get(which, "spell_cast" if which == "cast" else "spell_land"))
	play(ev, 0.05, float(sc.get("pitch", 1.0)))

