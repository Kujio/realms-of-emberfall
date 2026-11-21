## Animation state machine for one character, driven only by what the server tells us (movement
## speed from snapshots, the dead/sitting/stunned flags, combat state, casts, and combat events).
## Pure logic, no nodes: it decides which *standard* state should play (idle, walk, run, attack1,
## cast, ...) and at what speed. CharacterVisual maps the state to a clip through the model's
## animation map, and asks resolve() for a fallback when the model lacks a clip.
extends RefCounted

## Standard states. Animation maps in content/models.json map these names to clips.
const STATES: Array[String] = ["idle", "combat_idle", "walk", "walk_back", "run", "attack1", "attack2", "cast",
	"hit", "death", "sit", "swim", "fall"]
const LOOPING: Array[String] = ["idle", "combat_idle", "walk", "walk_back", "run", "cast", "sit", "swim", "fall"]
## When a model has no clip for a state, try these in order ("" in the result = nothing fits).
const FALLBACKS: Dictionary = {
	"combat_idle": ["idle"],
	"walk": ["run", "idle"],
	"walk_back": ["walk", "run", "idle"],
	"run": ["walk", "idle"],
	"attack1": ["attack2"],
	"attack2": ["attack1"],
	"cast": ["idle"],
	"swim": ["run", "walk", "idle"],
	"fall": ["idle"],
	"sit": [],
	"death": [],
	"hit": [],
	"idle": [],
}

const IDLE_TO_MOVE: float = 0.35  # m/s: start walking above this
const MOVE_TO_IDLE: float = 0.2   # m/s: stop below this
const ONESHOT_DEFAULT: float = 0.6
const ATTACK_MIN_GAP: float = 0.25  # double attacks arrive in the same tick; don't restart the swing

## Clips this model provides, by standard state (set from the animation map).
var available: Dictionary = {}
var walk_run_threshold: float = 3.2

var dead: bool = false
var sitting: bool = false
var stunned: bool = false  # stunned or mezzed: no locomotion or swings
var in_combat: bool = false
var casting: bool = false
var speed: float = 0.0
var backward: bool = false

var _loco: String = "idle"
## Bumped on every one-shot trigger, so a repeated swing restarts its clip.
var oneshot_serial: int = 0
var _oneshot: String = ""
var _oneshot_left: float = 0.0
var _since_attack: float = 99.0
var _next_attack: int = 0
var _state: String = "idle"
var _started_dead: bool = false


## Standard state -> the state actually played on this model ("" when none fits).
func resolve(state: String) -> String:
	if available.is_empty():
		return ""
	if available.has(state):
		return state
	for alt: String in FALLBACKS.get(state, []):
		if available.has(alt):
			return alt
	return ""


func set_dead(is_dead: bool, instant: bool) -> void:
	if is_dead and not dead:
		_started_dead = instant
		_oneshot = ""
		casting = false
	dead = is_dead


## True when the death pose should be shown already finished (we arrived after the death).
func death_is_instant() -> bool:
	return _started_dead


## Melee swing (or ability). Alternates attack1/attack2 unless a variant is forced (1 or 2).
func trigger_attack(variant: int = 0, duration: float = ONESHOT_DEFAULT) -> bool:
	if dead or sitting or stunned or _since_attack < ATTACK_MIN_GAP:
		return false
	var v: int = variant
	if v == 0:
		v = 1 + _next_attack
		_next_attack = 1 - _next_attack
	_oneshot = "attack%d" % v
	oneshot_serial += 1
	_oneshot_left = duration
	_since_attack = 0.0
	return true


## A hit reaction. Never interrupts a swing, a cast, or movement.
func trigger_hit(duration: float = 0.4) -> bool:
	if dead or sitting or casting or speed > IDLE_TO_MOVE or _oneshot.begins_with("attack"):
		return false
	_oneshot = "hit"
	oneshot_serial += 1
	_oneshot_left = duration
	return true


## A short release gesture after an instant (or just-finished) spell.
func trigger_cast_release(duration: float = 0.5) -> bool:
	if dead or sitting or stunned:
		return false
	_oneshot = "cast"
	oneshot_serial += 1
	_oneshot_left = duration
	return true


## Tell the machine how long the one-shot that just started really lasts (the clip's length).
func set_oneshot_length(seconds: float) -> void:
	if _oneshot != "":
		_oneshot_left = maxf(0.05, seconds)


func oneshot() -> String:
	return _oneshot


## Advance timers and return the standard state that should play now.
func update(delta: float) -> String:
	_since_attack += delta
	if (stunned or sitting) and _oneshot != "":
		_oneshot = ""  # a stun (or sitting down) cuts a swing or flinch short
	if _oneshot != "":
		_oneshot_left -= delta
		if _oneshot_left <= 0.0:
			_oneshot = ""
	_update_locomotion()
	if dead:
		_state = "death"
	elif sitting:
		_state = "sit"
	elif _oneshot != "" and not (casting and _oneshot == "hit"):
		_state = _oneshot
	elif casting:
		_state = "cast"
	elif stunned:
		_state = "combat_idle" if in_combat else "idle"
	else:
		_state = _loco
	return _state


func state() -> String:
	return _state


func _update_locomotion() -> void:
	var moving: bool = speed > (MOVE_TO_IDLE if _loco != "idle" and _loco != "combat_idle" else IDLE_TO_MOVE)
	if stunned or not moving:
		_loco = "combat_idle" if in_combat else "idle"
		return
	if backward:
		_loco = "walk_back"
		return
	var run_at: float = walk_run_threshold * (0.9 if _loco == "run" else 1.1)
	_loco = "run" if speed > run_at else "walk"


## Playback speed for locomotion clips so feet match the ground: actual speed over the clip's
## ground speed (scaled by the model's size). Other states play at 1x.
func speed_scale(played: String, ground_speed: float, size: float) -> float:
	if ground_speed <= 0.0 or not (played in ["walk", "walk_back", "run", "swim"]):
		return 1.0
	var s: float = speed / maxf(0.05, ground_speed * maxf(0.1, size))
	s = clampf(s, 0.4, 2.5)
	# A model without a walk_back clip walks backwards by playing walk (or run) in reverse.
	return -s if backward and played != "walk_back" else s
