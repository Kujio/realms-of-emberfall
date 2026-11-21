## Headless client tests for the model and animation pipeline. Run:
##   godot --headless --path client --script res://tests/test_runner.gd
## (after `godot --headless --path client --import` so the GLB files are imported). Exit code 0 = pass.
extends SceneTree

const AnimState := preload("res://scripts/world/anim_state.gd")
const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const CharacterVisual := preload("res://scripts/world/character_visual.gd")
const ModelPreview := preload("res://scripts/ui/model_preview.gd")

var _checks: int = 0
var _failures: Array[String] = []
var _section: String = ""


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	_test_anim_state_locomotion()
	_test_anim_state_priorities()
	_test_anim_state_fallbacks()
	_test_speed_matching()
	_test_player_look()
	await _test_model_loads_and_animates()
	await _test_quadruped_fallbacks()
	await _test_missing_model_uses_placeholder()
	await _test_model_swap_is_data_only()
	await _test_third_party_models()
	await _test_facing()
	await _test_preview()
	VisualLibrary.override_data(null)
	print("client tests: %d checks, %d failed" % [_checks, _failures.size()])
	for f: String in _failures:
		printerr("  FAIL " + f)
	quit(0 if _failures.is_empty() else 1)


func section(name: String) -> void:
	_section = name


func check(cond: bool, what: String) -> void:
	_checks += 1
	if not cond:
		_failures.append("%s: %s" % [_section, what])


func near(a: float, b: float, eps: float = 0.01) -> bool:
	return absf(a - b) <= eps


func _full_machine() -> AnimState:
	var a: AnimState = AnimState.new()
	for s: String in ["idle", "combat_idle", "walk", "run", "attack1", "attack2", "cast", "hit", "death", "sit"]:
		a.available[s] = true
	return a


# ------------------------------------------------------------------------------ AnimState

func _test_anim_state_locomotion() -> void:
	section("locomotion")
	var a: AnimState = _full_machine()
	check(a.update(0.05) == "idle", "starts idle")
	a.speed = 0.3
	check(a.update(0.05) == "idle", "0.3 m/s is still idle (hysteresis)")
	a.speed = 1.5
	check(a.update(0.05) == "walk", "1.5 m/s walks")
	a.speed = 0.25
	check(a.update(0.05) == "walk", "0.25 m/s keeps walking once moving")
	a.speed = 6.0
	check(a.update(0.05) == "run", "6 m/s runs")
	a.speed = 3.1
	check(a.update(0.05) == "run", "just under the threshold keeps running")
	a.speed = 2.0
	check(a.update(0.05) == "walk", "slowing down walks again")
	a.backward = true
	check(a.update(0.05) == "walk_back", "moving backwards asks for walk_back")
	check(a.resolve("walk_back") == "walk", "walk_back falls back to walk")
	a.backward = false
	a.speed = 0.0
	a.in_combat = true
	check(a.update(0.05) == "combat_idle", "standing in combat uses combat_idle")


func _test_anim_state_priorities() -> void:
	section("priorities")
	var a: AnimState = _full_machine()
	check(a.trigger_attack(), "an attack starts")
	check(a.update(0.05) == "attack1", "first swing is attack1")
	check(not a.trigger_attack(), "a double attack in the same tick does not restart the swing")
	check(not a.trigger_hit(), "a hit never interrupts a swing")
	a.update(0.7)
	check(a.update(0.05) == "idle", "the swing ends")
	check(a.trigger_attack(), "second swing")
	check(a.update(0.05) == "attack2", "swings alternate")
	a.update(1.0)
	a.speed = 5.0
	a.update(0.05)
	check(not a.trigger_hit(), "no flinch while running")
	a.speed = 0.0
	a.update(0.05)
	check(a.trigger_hit(), "a flinch while standing")
	check(a.update(0.05) == "hit", "hit plays")
	a.update(1.0)
	a.casting = true
	check(a.update(0.05) == "cast", "casting loops cast")
	check(not a.trigger_hit(), "no flinch while casting")
	a.casting = false
	a.sitting = true
	check(a.update(0.05) == "sit", "sitting")
	check(not a.trigger_attack(), "no swings while sitting")
	a.sitting = false
	a.stunned = true
	check(not a.trigger_attack(), "no swings while stunned")
	check(a.update(0.05) == "idle", "stunned stands still")
	a.stunned = false
	a.update(1.0)
	check(a.trigger_attack(), "a swing")
	a.update(0.05)
	a.stunned = true
	check(a.update(0.05) == "idle", "a stun cuts the swing short")
	a.stunned = false
	a.set_dead(true, false)
	check(a.update(0.05) == "death", "death wins over everything")
	check(not a.death_is_instant(), "dying in front of us animates")
	var b: AnimState = _full_machine()
	b.set_dead(true, true)
	check(b.death_is_instant(), "already dead when we arrived: show the final pose")
	b.set_dead(false, false)
	check(b.update(0.05) == "idle", "revived")


func _test_anim_state_fallbacks() -> void:
	section("fallbacks")
	var a: AnimState = AnimState.new()
	check(a.resolve("run") == "", "no clips at all resolves to nothing (placeholder motion)")
	a.available = {"idle": true, "walk": true, "attack1": true}
	check(a.resolve("run") == "walk", "run falls back to walk")
	check(a.resolve("attack2") == "attack1", "attack2 falls back to attack1")
	check(a.resolve("combat_idle") == "idle", "combat_idle falls back to idle")
	check(a.resolve("cast") == "idle", "cast falls back to idle")
	check(a.resolve("hit") == "", "a missing hit reaction is skipped")
	check(a.resolve("death") == "", "a missing death is procedural")
	a.available = {"idle": true, "run": true}
	check(a.resolve("walk") == "run", "walk falls back to run")


func _test_speed_matching() -> void:
	section("speed matching")
	var a: AnimState = _full_machine()
	a.speed = 7.0
	check(near(a.speed_scale("run", 5.2, 1.0), 7.0 / 5.2), "run plays faster at 7 m/s")
	check(near(a.speed_scale("run", 5.2, 2.0), 7.0 / 10.4), "a model twice the size strides twice as far")
	a.speed = 1.0
	check(near(a.speed_scale("run", 5.2, 1.0), 0.4), "clamped at 0.4x")
	check(near(a.speed_scale("idle", 5.2, 1.0), 1.0), "idle is not speed matched")
	a.backward = true
	a.speed = 1.7
	check(a.speed_scale("walk", 1.7, 1.0) < 0.0, "walking backwards plays walk in reverse")
	check(a.speed_scale("walk_back", 1.7, 1.0) > 0.0, "a real walk_back clip plays forwards")


# ------------------------------------------------------------------------------ looks

func _test_player_look() -> void:
	section("player look")
	VisualLibrary.override_data(null)
	var l: Dictionary = VisualLibrary.player_look("highlander", 1, "warrior")
	check(str((l["model"] as Dictionary).get("id")) == "humanoid_male", "highlander male uses humanoid_male")
	check((l["tints"] as Dictionary).has("Skin"), "race tints the skin")
	check((l["tints"]["Cloth"] as Color).is_equal_approx(Color.html("#7a2a20")), "class tints the cloth")
	check(str(l["parts"].get("beard")) == "Beard", "race part: beard")
	check(str(l["parts"].get("shoulders")) == "Pauldrons", "class part: pauldrons")
	check(str((l["held"]["main_hand"] as Dictionary).get("id")) == "sword_basic", "warriors hold a sword")
	check(str((l["held"]["off_hand"] as Dictionary).get("id")) == "shield_round", "and a shield")
	var v: Dictionary = VisualLibrary.player_look("vaelith", 0, "wizard")
	check(str((v["model"] as Dictionary).get("id")) == "humanoid_female", "vaelith female")
	check((v["scale"] as Vector3).y > 1.0 and (v["scale"] as Vector3).x < 1.0, "vaelith are tall and slender")
	check(str(v["parts"].get("head")) == "WizardHat", "wizards wear the hat")
	check(str(v["parts"].get("ears")) == "EarsPointed", "vaelith ears")
	var d: Dictionary = VisualLibrary.player_look("deepkin", 1, "cleric")
	check((d["scale"] as Vector3).y < 0.9, "deepkin are short")
	var u: Dictionary = VisualLibrary.player_look("no_such_race", 0, "no_such_class")
	check(str((u["model"] as Dictionary).get("id")) == "humanoid", "unknown race falls back to the generic humanoid")
	check(str((VisualLibrary.mob_look("not_a_model")["model"] as Dictionary).get("scene")) == "", "unknown model id is a placeholder")


func _add(look: Dictionary, scale: float = 1.0) -> CharacterVisual:
	var cv: CharacterVisual = CharacterVisual.new()
	cv.build(look, scale)
	root.add_child(cv)
	return cv


func _test_model_loads_and_animates() -> void:
	section("humanoid model")
	VisualLibrary.override_data(null)
	var cv: CharacterVisual = _add(VisualLibrary.player_look("highlander", 1, "warrior"))
	await process_frame
	check(not cv.is_placeholder, "the GLB loads")
	check(cv.skeleton() != null, "it has a skeleton")
	for s: String in ["idle", "combat_idle", "walk", "run", "attack1", "attack2", "cast", "hit", "death", "sit"]:
		check(cv.has_clip_for(s), "clip mapped for " + s)
	check(cv.attached.has("main_hand") and cv.attached.has("off_hand"), "sword and shield attached")
	var ba: BoneAttachment3D = cv.attached.get("main_hand") as BoneAttachment3D
	check(ba != null and ba.bone_name == "hand_R", "the sword follows the right hand bone")
	check(ba != null and ba.get_parent() == cv.skeleton(), "attachments live under the skeleton")
	var inst: Node3D = cv.model_instance()
	check((inst.find_child("Beard", true, false) as Node3D).visible, "beard shown")
	check(not (inst.find_child("HairLong", true, false) as Node3D).visible, "other hair hidden")
	check(not (inst.find_child("WizardHat", true, false) as Node3D).visible, "wizard hat hidden on a warrior")
	check((inst.find_child("Pauldrons", true, false) as Node3D).visible, "warrior pauldrons shown")
	var tinted: bool = false
	for n: Node in inst.find_children("*", "MeshInstance3D", true, false):
		var mi: MeshInstance3D = n as MeshInstance3D
		for i: int in range(mi.mesh.get_surface_count()):
			var m: Material = mi.mesh.surface_get_material(i)
			if m != null and m.resource_name == "Cloth":
				var o: BaseMaterial3D = mi.get_surface_override_material(i) as BaseMaterial3D
				tinted = o != null and o.albedo_color.is_equal_approx(Color.html("#7a2a20"))
	check(tinted, "the Cloth material carries the class tint")
	var player: AnimationPlayer = cv.model_instance().find_children("*", "AnimationPlayer", true, false)[0] as AnimationPlayer
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Idle", "standing plays Idle (got %s)" % player.current_animation)
	for i: int in range(12):
		cv.step(0.05, 6.0, false, 0.0)
	check(player.current_animation == "Run", "6 m/s plays Run (got %s)" % player.current_animation)
	check(near(player.speed_scale, 6.0 / 3.6, 0.05), "Run speed matched to 6 m/s (got %.2f)" % player.speed_scale)
	for i: int in range(12):
		cv.step(0.05, 0.0, false, 0.0)
	cv.anim.trigger_attack(1)
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Attack1", "a swing plays Attack1 (got %s)" % player.current_animation)
	cv.step(0.3, 0.0, false, 0.0)
	player.advance(0.3)
	check(player.current_animation == "Attack1" and player.current_animation_position > 0.1, "the first swing is under way")
	check(cv.anim.trigger_attack(1), "a second swing is accepted")
	cv.step(0.0, 0.0, false, 0.0)
	check(player.current_animation_position < 0.1, "a second swing of the same clip starts from the top")
	cv.anim.casting = true
	cv.step(1.0, 0.0, false, 0.0)
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Cast", "casting plays Cast (got %s)" % player.current_animation)
	cv.anim.casting = false
	cv.anim.sitting = true
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Sit", "sitting plays Sit")
	cv.anim.sitting = false
	cv.anim.set_dead(true, false)
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Death", "dying plays Death")
	check(player.get_animation("Death").loop_mode == Animation.LOOP_NONE, "death does not loop")
	check(player.get_animation("Run").loop_mode == Animation.LOOP_LINEAR, "run loops")
	cv.queue_free()


func _test_quadruped_fallbacks() -> void:
	section("quadruped")
	var cv: CharacterVisual = _add(VisualLibrary.mob_look("wolf"), 1.0)
	await process_frame
	check(not cv.is_placeholder, "wolf loads the quadruped GLB")
	var player: AnimationPlayer = cv.model_instance().find_children("*", "AnimationPlayer", true, false)[0] as AnimationPlayer
	cv.anim.casting = true
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Idle", "a beast without a cast clip idles (got %s)" % player.current_animation)
	cv.anim.casting = false
	cv.anim.trigger_attack(2)
	cv.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Attack", "attack2 falls back to the only attack (got %s)" % player.current_animation)
	cv.anim.sitting = true
	for i: int in range(20):
		cv.step(0.05, 0.0, false, 0.0)
	check(cv.get_node("Body").scale.y < 1.3 * 0.8, "no sit clip: the body squats procedurally")
	check(not (cv.model_instance().find_child("Tusks", true, false) as Node3D).visible, "wolves have no tusks")
	var boar: CharacterVisual = _add(VisualLibrary.mob_look("boar"), 1.0)
	check((boar.model_instance().find_child("Tusks", true, false) as Node3D).visible, "boars do (same mesh, other parts)")
	cv.queue_free()
	boar.queue_free()


func _test_missing_model_uses_placeholder() -> void:
	section("missing model")
	var data: Dictionary = (VisualLibrary.data() as Dictionary).duplicate(true)
	(data["models"]["humanoid_male"] as Dictionary)["scene"] = "res://assets/models/missing/nope.glb"
	VisualLibrary.override_data(data)
	var cv: CharacterVisual = _add(VisualLibrary.player_look("highlander", 1, "warrior"))
	await process_frame
	check(cv.is_placeholder, "a missing GLB falls back to the placeholder")
	check(cv.attached.has("main_hand"), "the placeholder still holds the sword")
	for i: int in range(10):
		cv.step(0.05, 6.0, false, 0.0)
	check(cv.playing == "run", "placeholders still run the state machine")
	cv.anim.set_dead(true, false)
	for i: int in range(30):
		cv.step(0.05, 0.0, false, 0.0)
	check(cv.get_node("Body").rotation.x < -1.4, "a dead placeholder lies down")
	cv.queue_free()
	VisualLibrary.override_data(null)


func _test_model_swap_is_data_only() -> void:
	section("swap by data")
	var data: Dictionary = (VisualLibrary.data() as Dictionary).duplicate(true)
	((data["races"]["highlander"] as Dictionary)["female"] as Dictionary)["model"] = "wolf"
	VisualLibrary.override_data(data)
	var cv: CharacterVisual = _add(VisualLibrary.player_look("highlander", 0, "monk"))
	await process_frame
	check(str(cv.model_def.get("id")) == "wolf", "a JSON edit swaps the model")
	check(not cv.is_placeholder and cv.has_clip_for("run"), "and it animates with its own map")
	cv.queue_free()
	VisualLibrary.override_data(null)


func _test_third_party_models() -> void:
	section("third-party models")
	VisualLibrary.override_data(null)
	# Converted from FBX (Quaternius RPG Characters): clip names carry the armature prefix.
	var knight: CharacterVisual = _add(VisualLibrary.mob_look("hollow_knight"), 1.1)
	await process_frame
	check(not knight.is_placeholder, "the converted RPG Warrior loads")
	for s: String in ["idle", "combat_idle", "walk", "run", "attack1", "attack2", "hit", "death"]:
		check(knight.has_clip_for(s), "hollow_knight maps " + s)
	check(not knight.has_clip_for("cast") and knight.anim.resolve("cast") == "idle", "no cast clip: falls back to idle")
	# The Universal Animation Library mannequin: map names with _Loop resolve to Godot's renamed clips.
	var golem: CharacterVisual = _add(VisualLibrary.mob_look("golem"), 1.6)
	await process_frame
	check(not golem.is_placeholder, "the UAL mannequin loads")
	for s: String in ["idle", "combat_idle", "walk", "run", "attack1", "attack2", "cast", "hit", "death", "sit", "swim", "fall"]:
		check(golem.has_clip_for(s), "golem maps " + s)
	var player: AnimationPlayer = golem.model_instance().find_children("*", "AnimationPlayer", true, false)[0] as AnimationPlayer
	golem.step(0.05, 0.0, false, 0.0)
	check(player.current_animation == "Idle", "Idle_Loop plays as Godot's 'Idle' (got %s)" % player.current_animation)
	check(player.get_animation("Idle").loop_mode == Animation.LOOP_LINEAR, "and loops")
	var trainer: CharacterVisual = _add(VisualLibrary.mob_look("trainer_wizard"))
	await process_frame
	check(not trainer.is_placeholder and trainer.has_clip_for("cast"), "the wizard trainer can cast")
	for cv: CharacterVisual in [knight, golem, trainer]:
		cv.queue_free()


func _test_facing() -> void:
	section("facing")
	var cv: CharacterVisual = _add(VisualLibrary.mob_look("rat"))
	await process_frame
	cv.step(0.05, 0.0, false, 0.0, NAN)
	check(near(cv.rotation.y, 0.0), "faces its root yaw")
	for i: int in range(5):
		cv.step(0.016, 0.0, false, 0.0, PI / 2.0)
	check(cv.rotation.y > 0.1 and cv.rotation.y < PI / 2.0 - 0.05, "turns toward a target smoothly, not instantly")
	for i: int in range(120):
		cv.step(0.016, 0.0, false, 0.0, PI / 2.0)
	check(near(cv.rotation.y, PI / 2.0, 0.02), "ends up facing the target")
	for i: int in range(120):
		cv.step(0.016, 4.0, false, 0.0, PI / 2.0)
	check(near(cv.rotation.y, 0.0, 0.02), "moving faces the way it moves, not the target")
	cv.queue_free()


func _test_preview() -> void:
	section("preview")
	var p: ModelPreview = ModelPreview.new()
	root.add_child(p)
	p.show_character("deepkin", 1, "cleric")
	await process_frame
	check(p.shown_key == "deepkin/1/cleric", "preview shows the choice")
	check(p.visual != null and not p.visual.is_placeholder, "with the real model")
	var first: CharacterVisual = p.visual
	p.show_character("deepkin", 1, "cleric")
	check(p.visual == first, "the same choice does not rebuild")
	p.show_character("deepkin", 0, "cleric")
	check(p.visual != first and p.shown_key == "deepkin/0/cleric", "changing gender rebuilds")
	p.queue_free()
