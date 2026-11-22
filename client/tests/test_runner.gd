## Headless client tests for the model and animation pipeline and the class HUD. Run:
##   godot --headless --path client --script res://tests/test_runner.gd
## (after `godot --headless --path client --import` so the GLB files are imported). Exit code 0 = pass.
extends SceneTree

const AnimState := preload("res://scripts/world/anim_state.gd")
const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const CharacterVisual := preload("res://scripts/world/character_visual.gd")
const ModelPreview := preload("res://scripts/ui/model_preview.gd")
const ClassUi := preload("res://scripts/ui/class_ui.gd")
const RemoteEntity := preload("res://scripts/world/remote_entity.gd")
const ItemDb := preload("res://scripts/ui/item_db.gd")
const ItemUi := preload("res://scripts/ui/item_ui.gd")
const ZoneGeom := preload("res://scripts/world/zone_geom.gd")
const DungeonGen := preload("res://scripts/world/dungeon_gen.gd")
const ZoneData := preload("res://scripts/world/zone_data.gd")
const ZoneBuilder := preload("res://scripts/world/zone_builder.gd")

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
	_test_every_race_has_a_look()
	await _test_class_ui()
	await _test_entity_flags2()
	_test_item_db()
	await _test_item_ui()
	_test_equipment_visuals()
	_test_world_geometry()
	_test_zone_data()
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


# ------------------------------------------------------------------------------ Phase 4

func _test_every_race_has_a_look() -> void:
	section("races")
	VisualLibrary.override_data(null)
	var races: Array = (VisualLibrary.data().get("races", {}) as Dictionary).keys()
	check(races.size() >= 6 and races.size() <= 8, "six to eight races have looks (%d)" % races.size())
	for race: Variant in races:
		for g: int in [0, 1]:
			var l: Dictionary = VisualLibrary.player_look(str(race), g, "warrior")
			check(str((l["model"] as Dictionary).get("scene", "")) != "", "%s %d has a real model" % [str(race), g])
	var small: Dictionary = VisualLibrary.player_look("tinkerling", 0, "wizard")
	var big: Dictionary = VisualLibrary.player_look("morrok", 1, "warrior")
	check((small["scale"] as Vector3).y < 0.7 and (big["scale"] as Vector3).y > 1.15, "tinkerlings are tiny and morrok huge")


func _test_class_ui() -> void:
	section("class ui")
	var ui: ClassUi = ClassUi.new()
	root.add_child(ui)
	await process_frame
	check(not (ui.get("_pet_win") as Control).visible, "no pet, no pet window")
	ui.set_pet({"pet_id": 7, "name": "Kael's ember servant", "level": 20, "hp_pct": 60, "mode": 1, "target_id": 3, "charmed": false, "charm_ms": 0})
	check((ui.get("_pet_win") as Control).visible, "a pet shows the window")
	check(str((ui.get("_pet_state") as Label).text).begins_with("Guarding, attacking"), "mode and activity shown")
	var orders: Array[int] = []
	ui.pet_command.connect(func(o: int) -> void: orders.append(o))
	for b: Node in (ui.get("_pet_win") as Node).find_children("*", "Button", true, false):
		(b as Button).pressed.emit()
	check(orders == [0, 1, 2, 3, 4], "the five pet buttons send attack, follow, guard, back off, dismiss")
	ui.set_pet({"pet_id": 9, "name": "a gnoll", "level": 12, "hp_pct": 100, "mode": 0, "target_id": 0, "charmed": true, "charm_ms": 42000})
	check(str((ui.get("_pet_name") as Label).text).contains("charmed"), "charmed pets say so")
	check(str((ui.get("_pet_state") as Label).text).contains("42s"), "with the time left")
	ui.set_pet({"pet_id": 0, "name": "", "level": 0, "hp_pct": 0, "mode": 0, "target_id": 0, "charmed": false, "charm_ms": 0})
	check(not (ui.get("_pet_win") as Control).visible, "losing the pet hides the window")
	ui.set_song({"song_id": "hymn_of_mending", "name": "Hymn of Mending", "pulse_ms": 6000, "target_id": 1})
	check((ui.get("_song_panel") as Control).visible and str((ui.get("_song_label") as Label).text).contains("Hymn of Mending"), "song indicator")
	ui.set_song({"song_id": "", "name": "", "pulse_ms": 0, "target_id": 0})
	check(not (ui.get("_song_panel") as Control).visible, "silence hides it")
	ui.set_skills({"announce": false, "skills": [{"id": "backstab", "name": "Backstab", "value": 40, "cap": 55}]})
	ui.set_skills({"announce": true, "skills": [{"id": "backstab", "name": "Backstab", "value": 41, "cap": 55}]})
	check(int((ui.skills[0] as Dictionary)["value"]) == 41, "a skill-up updates the list")
	ui.toggle_skills()
	await process_frame
	check(ui.any_window_open(), "J opens the skills window")
	var cmds: int = 0
	for n: Node in (ui.get("_skills_list") as Node).get_children():
		if n is Button:
			cmds += 1
	check(cmds == ClassUi.COMMANDS.size(), "every hotbar command is listed")
	check(ClassUi.is_command("cmd:pet_attack") and ClassUi.command_label("cmd:pet_attack") == "Pet: Attack", "commands have labels")
	check(not ClassUi.is_command("rising_kick"), "abilities are not commands")
	ui.close_windows()
	check(not ui.any_window_open(), "Esc closes it")
	ui.queue_free()


func _test_entity_flags2() -> void:
	section("entity flags2")
	var spawn: Dictionary = {"entity_id": 5, "kind": 0, "name": "Mira", "race": "highlander", "class_id": "enchanter", "level": 30, "x": 0.0,
		"y": 0.0, "z": 0.0, "yaw": 0.0, "hp_pct": 100, "flags": 0, "scale": 1.0, "gender": 0, "flags2": 0, "model": "", "owner_id": 0}
	var r: RemoteEntity = RemoteEntity.new()
	r.setup(spawn, 0.0)
	root.add_child(r)
	await process_frame
	var first: Node = r.visual
	r.push_state(0.1, Vector3.ZERO, 0.0, 100, 0, RemoteEntity.FLAG2_FEIGNED)
	check(r.visual.anim.dead, "a feigning player lies as if dead")
	check(r.is_alive(), "but is not dead")
	r.push_state(0.2, Vector3.ZERO, 0.0, 100, 0, RemoteEntity.FLAG2_HIDDEN)
	check(not r.visual.anim.dead, "standing up again")
	var ghost: bool = true
	for n: Node in r.visual.find_children("*", "GeometryInstance3D", true, false):
		ghost = ghost and (n as GeometryInstance3D).transparency > 0.5
	check(ghost, "a hidden player is drawn faintly")
	var wolf: Dictionary = spawn.duplicate()
	wolf["model"] = "wolf"
	r.refresh(wolf)
	check(r.visual != first and str((r.look()["model"] as Dictionary).get("id")) == "wolf", "an illusion rebuilds the body as the creature")
	var deep: Dictionary = spawn.duplicate()
	deep["race"] = "deepkin"
	r.refresh(deep)
	check(str((r.look()["model"] as Dictionary).get("id")) == "humanoid_female" and (r.look()["scale"] as Vector3).y < 0.9, "a race illusion looks like that race")
	r.queue_free()
	var pet_spawn: Dictionary = spawn.duplicate()
	pet_spawn.merge({"entity_id": 6, "kind": 1, "name": "Kael's ember servant", "race": "imp", "class_id": "", "flags2": RemoteEntity.FLAG2_PET, "owner_id": 2}, true)
	var pet: RemoteEntity = RemoteEntity.new()
	pet.setup(pet_spawn, 0.0)
	root.add_child(pet)
	await process_frame
	check(pet.is_pet() and pet.owner_id == 2, "pets know their owner")
	check(pet._tag_color().is_equal_approx(Color(0.6, 1.0, 0.85)), "and wear a friendly name colour")
	pet.queue_free()


# ------------------------------------------------------------------------------ Phase 5: items

func _test_item_db() -> void:
	section("item db")
	check(ItemDb.item("plate_t1_chest").get("armor", "") == "plate", "the synced item data loads")
	check((ItemDb.data()["items"] as Dictionary).size() >= 600, "600+ items in the client copy")
	check(ItemDb.code(ItemDb.CONT_GENERAL, 3) == 0x0103, "slot codes match the server")
	check(ItemDb.is_bag_content(ItemDb.code(ItemDb.CONT_GENERAL_BAG + 2, 0)), "bag contents")
	check(ItemDb.bag_parent(ItemDb.code(ItemDb.CONT_BANK_BAG + 4, 1)) == ItemDb.code(ItemDb.CONT_BANK, 4), "bag parent in the bank")
	check(ItemDb.in_bank(ItemDb.code(ItemDb.CONT_BANK, 0)) and not ItemDb.in_bank(ItemDb.code(ItemDb.CONT_GENERAL, 0)), "bank slots")
	check(ItemDb.equip_slots_of(ItemDb.item("mystic_finger_t1")) == [10, 11], "rings fit both fingers")
	check(ItemDb.fits(ItemDb.item("longsword_t1"), ItemDb.SECONDARY), "one-handers fit the off hand")
	check(not ItemDb.fits(ItemDb.item("greatsword_t1"), ItemDb.SECONDARY), "two-handers do not")
	var tip: String = ItemDb.tooltip("plate_t5_chest", 1)
	check(tip.contains("Plate armor") and tip.contains("AC +") and tip.contains("Requires level 30"), "tooltips show type, AC and level")
	var cmp: Array[String] = ItemDb.compare_lines(ItemDb.item("plate_t5_chest"), ItemDb.item("plate_t1_chest"))
	check(cmp.size() > 0 and cmp[0].contains("+") and cmp[0].contains("AC"), "comparison shows the AC gain first")
	var wtip: String = ItemDb.tooltip("longsword_t10", 1, ItemDb.item("longsword_t1"))
	check(wtip.contains("Damage/sec") and wtip.contains("Compared to"), "weapon tooltips compare damage per second")
	check(ItemDb.tooltip("greater_draught", 3).contains("Use:"), "consumables say what they do")
	check(ItemDb.comparison_item(ItemDb.item("plate_t1_chest"), {5: "plate_t2_chest"}).get("id", "") == "plate_t2_chest", "compares with the worn chest")
	check(ItemDb.comparison_item(ItemDb.item("mystic_finger_t1"), {10: "mystic_finger_t2"}).is_empty(), "an empty finger means nothing to compare")
	check(ItemUi.parse_coin("2p 5g 3s 1c") == 2531 and ItemUi.parse_coin("250") == 250 and ItemUi.parse_coin("1.5p") == 1500, "coin parsing")
	check(ItemUi.parse_coin("2p 5gg") == -1 and ItemUi.parse_coin("-3") == -1 and ItemUi.parse_coin("abc") == -1 and ItemUi.parse_coin("") == 0,
		"malformed amounts are rejected whole, not partly read")
	check(ItemDb.format_coin(2531) == "2p 5g 3s 1c", "coin formatting")


func _test_item_ui() -> void:
	section("item ui")
	var ui: ItemUi = ItemUi.new()
	root.add_child(ui)
	await process_frame
	var inv: Dictionary = {"coin": 1234, "bank_coin": 50, "bank_slots": 16, "items": [
		{"slot": 0x000F, "item_id": "training_sword", "count": 1, "expires_ms": 0},
		{"slot": 0x0100, "item_id": "small_sack", "count": 1, "expires_ms": 0},
		{"slot": 0x1003, "item_id": "trail_rations", "count": 5, "expires_ms": 0},
		{"slot": 0x0101, "item_id": "plate_t1_chest", "count": 1, "expires_ms": 0},
	]}
	ui.set_inventory(inv)
	await process_frame
	check(ui.count_of("trail_rations") == 5, "inventory parsed by slot code")
	check(ui.free_pack_slot("rat_whisker") == 0x0102, "the first empty pack slot")
	check(ui.free_pack_slot("cloth_pouch") == 0x0102, "bags only go in pack slots")
	var bag_rows: int = (ui.get("_bags") as Node).get_child_count()
	check(bag_rows == 1, "an open bag gets its own row")
	var moves: Array = []
	ui.move_item.connect(func(f: int, t: int, n: int) -> void: moves.append([f, t, n]))
	var uses: Array[int] = []
	ui.use_item.connect(func(c: int) -> void: uses.append(c))
	var sells: Array = []
	ui.vendor_sell.connect(func(npc: int, c: int, n: int) -> void: sells.append([npc, c, n]))
	# Find the pack square holding the chest and drop it on the chest square of the paper doll.
	var chest_slot: Control = null
	var doll_chest: Control = null
	for n: Node in ui.find_children("*", "Panel", true, false):
		if n.get("code") == 0x0101 and n.get("role") == "inv":
			chest_slot = n as Control
		if n.get("code") == 0x0005 and n.get("role") == "equip":
			doll_chest = n as Control
	check(chest_slot != null and doll_chest != null, "pack and paper doll squares exist")
	if chest_slot != null and doll_chest != null:
		var data: Variant = chest_slot.call("_get_drag_data", Vector2.ZERO)
		check(data is Dictionary and int((data as Dictionary)["code"]) == 0x0101, "dragging a pack item carries its slot")
		check(bool(doll_chest.call("_can_drop_data", Vector2.ZERO, data)), "the paper doll accepts it")
		doll_chest.call("_drop_data", Vector2.ZERO, data)
		check(moves.size() == 1 and int(moves[0][0]) == 0x0101 and int(moves[0][1]) == 0x0005, "dropping sends a move")
		check(str(doll_chest.get("tooltip_text")) == "Chest", "an empty doll square names its slot")
		check(str(chest_slot.get("tooltip_text")).contains("Compared to nothing worn"), "pack tooltips compare with what is worn")
		chest_slot.call("_gui_input", _right_click())
		check(uses == [0x0101], "right-click uses (equips) an item")
		ui.show_vendor({"npc_id": 42, "name": "Tamsin", "greeting": "Hello", "sell_permille": 250,
			"entries": [{"item_id": "longsword_t1", "price": 20, "stock": -1, "buyback": false}, {"item_id": "rat_whisker", "price": 2, "stock": 3, "buyback": true}]})
		check(ui.is_vendor_open(), "vendor window opens")
		chest_slot.call("_gui_input", _right_click())
		check(sells.size() == 1 and int(sells[0][0]) == 42 and int(sells[0][1]) == 0x0101, "right-click sells while a vendor is open")
		var buys: Array = []
		ui.vendor_buy.connect(func(npc: int, id: String, n: int, bb: bool) -> void: buys.append([npc, id, n, bb]))
		for b: Node in (ui.get("_vendor_list") as Node).find_children("*", "Button", true, false):
			(b as Button).pressed.emit()
		check(buys.size() == 2 and str(buys[0][1]) == "longsword_t1" and bool(buys[1][3]), "buy buttons, including buyback")
	ui.show_trade({"trade_id": 1, "partner_id": 7, "partner_name": "Bob", "mine": [{"index": 2, "item_id": "trail_rations", "count": 5, "expires_ms": 0}],
		"my_coin": 10, "my_accept": false, "theirs": [{"index": 0, "item_id": "longsword_t2", "count": 1, "expires_ms": 0}], "their_coin": 0, "their_accept": true})
	check(ui.is_trading(), "trade window opens")
	check(str(((ui.get("_trade_theirs") as Node).get_child(0)).get("item_id")) == "longsword_t2", "their offer is shown")
	check(str((ui.get("_trade_status") as Label).text).contains("(accepted)"), "acceptance is shown")
	ui.close_trade({"trade_id": 1, "completed": true, "message": "The trade is complete."})
	check(not ui.is_trading(), "trade window closes")
	var rolled: Array = []
	ui.roll_choice.connect(func(r: int, c: int) -> void: rolled.append([r, c]))
	ui.show_roll({"roll_id": 9, "corpse_id": 3, "item_id": "plate_t2_chest", "count": 1, "expires_ms": 30000, "can_need": false})
	await process_frame
	var need_disabled: bool = false
	for b: Node in (ui.get("_rolls_box") as Node).find_children("*", "Button", true, false):
		if (b as Button).text == "Need":
			need_disabled = (b as Button).disabled
		if (b as Button).text == "Greed":
			(b as Button).pressed.emit()
	check(need_disabled, "need is disabled when you cannot use it")
	check(rolled == [[9, 1]], "greed answers the roll")
	ui.show_loot({"corpse_id": 5, "name": "a rat's corpse", "coin": 0, "items": [{"index": 0, "item_id": "rat_whisker", "count": 2, "owner": "Bob", "mine": false}]})
	check(ui.is_loot_open() and ui.loot_corpse() == 5, "loot window")
	var take_disabled: bool = false
	for b: Node in (ui.get("_loot_list") as Node).find_children("*", "Button", true, false):
		take_disabled = (b as Button).disabled
	check(take_disabled, "items reserved for someone else cannot be taken")
	ui.queue_free()


func _right_click() -> InputEventMouseButton:
	var e: InputEventMouseButton = InputEventMouseButton.new()
	e.button_index = MOUSE_BUTTON_RIGHT
	e.pressed = true
	return e


func _test_equipment_visuals() -> void:
	section("equipment visuals")
	var held: Dictionary = VisualLibrary.equipment_held({"main_hand": "longsword_t3", "off_hand": "dagger_t2", "ranged": "shortbow_t1"})
	check(held.has("main_hand") and held.has("off_hand") and held.has("back"), "worn weapons become held visuals")
	check(str((held["off_hand"] as Dictionary).get("attach_point", "")) == "off_hand", "a main-hand mesh in the off hand attaches there")
	var bare: Dictionary = VisualLibrary.player_look("highlander", 0, "warrior", {"main_hand": "", "off_hand": "", "ranged": ""})
	check((bare["held"] as Dictionary).is_empty(), "nothing worn, nothing held (no class training gear)")
	var classic: Dictionary = VisualLibrary.player_look("highlander", 0, "warrior")
	check(not (classic["held"] as Dictionary).is_empty(), "previews keep the class look")
	var r: RemoteEntity = RemoteEntity.new()
	r.setup({"entity_id": 77, "kind": 0, "name": "Armed", "race": "highlander", "class_id": "warrior", "level": 5, "x": 0.0, "y": 0.0, "z": 0.0,
		"yaw": 0.0, "hp_pct": 100, "flags": 0, "scale": 1.0, "gender": 0, "flags2": 0, "model": "", "owner_id": 0,
		"main_hand": "mace_t2", "off_hand": "", "ranged": ""}, 0.0)
	check((r.look()["held"] as Dictionary).has("main_hand"), "a spawned player holds its worn weapon")
	r.set_equipment({"main_hand": "", "off_hand": "buckler_t1", "ranged": ""})
	check((r.look()["held"] as Dictionary).has("off_hand") and not (r.look()["held"] as Dictionary).has("main_hand"), "equipment changes update the look")
	r.free()


# ------------------------------------------------------------------------------ Phase 6: world

## The dungeon generator and the collision rule must match the server's (server/tests/test_world.cpp
## pins the same numbers).
func _test_world_geometry() -> void:
	section("world geometry")
	check(DungeonGen.xorshift_first(1) == 270369, "xorshift32 from 1 matches the server")
	var d: DungeonGen = DungeonGen.new()
	d.seed_value = 12345
	d.rooms_wanted = 10
	d.generate()
	check(d.rooms.size() == 10, "exact room count")
	check(d.hash_value() == 3734157024, "golden dungeon hash matches the server (got %d)" % d.hash_value())
	var g: ZoneGeom = ZoneGeom.new()
	g.circles.append(PackedFloat32Array([0.0, 0.0, 2.0]))
	check(near(g.resolve(Vector2(1.0, 0.0), 0.5).x, 2.5, 0.001), "pushed out of a circle")
	var b: ZoneGeom = ZoneGeom.new()
	b.boxes.append(PackedFloat32Array([0.0, 0.0, 5.0, 1.0, PI / 2.0]))
	var out: Vector2 = b.resolve(Vector2(0.5, 3.0), 0.4)
	check(near(out.x, 1.4, 0.001) and near(out.y, 3.0, 0.001), "out of a rotated box through its nearest face")
	check(b.overlaps(0.0, 4.0, 0.4) and not b.overlaps(4.0, 0.0, 0.4), "rotated box overlap")
	var t: ZoneGeom = ZoneGeom.new()
	t.tile_w = 3
	t.tile_h = 3
	t.tile_cell = 2.0
	t.tiles = PackedByteArray([1, 1, 1, 1, 0, 1, 1, 1, 1])
	check(t.resolve(Vector2(2.1, 3.0), 0.4).x >= 2.4 - 0.0001, "out of a wall tile")
	check(t.overlaps(-5.0, -5.0, 0.1), "outside the tile grid is wall")
	# The same slide along a wall as the server's movement test.
	var w: ZoneGeom = ZoneGeom.new()
	w.boxes.append(PackedFloat32Array([5.0, 0.0, 1.0, 10.0, 0.0]))
	w.hills.append(PackedFloat32Array([0.0, 0.0, 50.0, 4.0]))
	var p: Vector3 = Vector3.ZERO
	for i: int in range(40):
		p = w.step(p, 0.8, 0.6, 7.0, 0.05, Rect2(-100, -100, 200, 200), 0.4)
	check(p.x <= 3.6 + 0.001 and p.z > 5.0, "slides along a wall")
	check(near(p.y, w.height(p.x, p.z), 0.0001), "follows the terrain")


## Every bundled zone loads, its geometry is sane, and dungeons build their rooms.
func _test_zone_data() -> void:
	section("zone data")
	var files: PackedStringArray = DirAccess.get_files_at(ZoneData.DATA_DIR)
	var n: int = 0
	for f: String in files:
		if not f.ends_with(".json"):
			continue
		n += 1
		var z: ZoneData = ZoneData.new()
		z.load_file(f.get_basename())
		check(z.loaded, "%s loads" % f)
		check(z.layout_hash() != 0, "%s has a layout hash" % f)
		check(z.bounds.size.x > 0.0 and z.bounds.size.y > 0.0, "%s has bounds" % f)
		if z.kind == "dungeon":
			check(z.dungeon != null and not z.dungeon.rooms.is_empty(), "%s builds its rooms" % f)
			for l: Variant in z.zone_lines():
				var ld: Dictionary = l as Dictionary
				check(not z.geom.overlaps(float(ld["x"]), float(ld["z"]), 0.3), "%s: its exit is on the floor" % f)
		for l2: Variant in z.zone_lines():
			check(FileAccess.file_exists(ZoneData.DATA_DIR.path_join(str((l2 as Dictionary)["to"]) + ".json")), "%s: line target bundled" % f)
		var root: Node3D = Node3D.new()
		get_root().add_child(root)
		ZoneBuilder.build(root, z)
		check(root.get_child_count() > 2, "%s builds visuals" % f)
		root.queue_free()
	check(n >= 4, "zones are bundled")
	var world: Variant = JSON.parse_string(FileAccess.get_file_as_string("res://data/world.json"))
	check(world is Dictionary and (world as Dictionary).has("zones"), "the world map index is bundled")
