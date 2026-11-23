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
const KeybindsLib := preload("res://scripts/keybinds.gd")
const Probe := preload("res://scripts/net/server_probe.gd")
const AudioLib := preload("res://scripts/audio.gd")
const UiLib := preload("res://scripts/ui/ui.gd")

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
	await _test_animal_models()
	await _test_effects()
	_test_icons_and_theme()
	await _test_credits()
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
	await _test_world_ui()
	await _test_chat_window()
	await _test_social_ui()
	_test_keybinds()
	_test_server_probe()
	_test_sound_events()
	_test_disconnect_text()
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
	# Outfit models (Phase 9): with nothing worn, the class's armour type picks the outfit.
	var l: Dictionary = VisualLibrary.player_look("highlander", 1, "warrior")
	check(str((l["model"] as Dictionary).get("id")) == "polyfork_human_warrior", "a highlander warrior wears the plate outfit")
	check(not (l["tints"] as Dictionary).has("Cloth") and not (l["parts"] as Dictionary).has("shoulders"), "outfit models skip class tints and parts")
	check(str(l["parts"].get("held")) == "", "the model's own weapons are hidden")
	check(str((l["held"]["main_hand"] as Dictionary).get("id")) == "sword_basic", "warriors hold a sword")
	check(str((l["held"]["off_hand"] as Dictionary).get("id")) == "shield_round", "and a shield")
	var wiz: Dictionary = VisualLibrary.player_look("highlander", 1, "wizard")
	check(str((wiz["model"] as Dictionary).get("id")) == "polyfork_human_priest", "a wizard wears the cloth outfit")
	var none: Dictionary = {"main_hand": "", "off_hand": "", "ranged": "", "chest": "", "head": ""}
	var robed: Dictionary = none.duplicate()
	robed["chest"] = "cloth_t1_chest"
	var rl: Dictionary = VisualLibrary.player_look("highlander", 1, "warrior", robed)
	check(str((rl["model"] as Dictionary).get("id")) == "polyfork_human_priest", "a worn cloth chest piece puts on the cloth outfit")
	check(not rl.has("outfit_tint"), "first-tier armour is untinted")
	var steel: Dictionary = none.duplicate()
	steel["chest"] = "plate_t5_chest"
	var sl: Dictionary = VisualLibrary.player_look("highlander", 1, "warrior", steel)
	check(sl.get("outfit_tint") is Color, "higher-tier armour tints the outfit")
	var helmed: Dictionary = none.duplicate()
	helmed["head"] = "plate_t1_head"
	check(str(VisualLibrary.player_look("highlander", 0, "warrior", helmed)["parts"].get("helm")) == "garment-helmet", "a worn helm shows the helmet")
	check(str(VisualLibrary.player_look("highlander", 0, "warrior", none)["parts"].get("helm")) == "", "bare-headed, no helmet")
	check(str(VisualLibrary.player_look("highlander", 0, "warrior")["parts"].get("helm")) == "", "previews show the face")
	var v: Dictionary = VisualLibrary.player_look("vaelith", 0, "wizard")
	check(str((v["model"] as Dictionary).get("id")) == "polyfork_elf_mage", "vaelith female wizard")
	var d: Dictionary = VisualLibrary.player_look("deepkin", 1, "cleric")
	check(float((d["model"] as Dictionary).get("nameplate_height")) < 1.8, "deepkin are short")
	var dusk: Dictionary = VisualLibrary.player_look("duskborn", 1, "necromancer")
	check((dusk["tints"] as Dictionary).has("body"), "duskborn skin is tinted")
	# The class-styled path (models dressed by class tints and parts) still works for such models.
	VisualLibrary.override_data(_classic_data())
	var c: Dictionary = VisualLibrary.player_look("highlander", 1, "warrior")
	check(str((c["model"] as Dictionary).get("id")) == "humanoid_male", "a class-styled model")
	check((c["tints"] as Dictionary).has("Skin"), "race tints the skin")
	check((c["tints"]["Cloth"] as Color).is_equal_approx(Color.html("#7a2a20")), "class tints the cloth")
	check(str(c["parts"].get("beard")) == "Beard", "race part: beard")
	check(str(c["parts"].get("shoulders")) == "Pauldrons", "class part: pauldrons")
	VisualLibrary.override_data(null)
	var u: Dictionary = VisualLibrary.player_look("no_such_race", 0, "no_such_class")
	check(str((u["model"] as Dictionary).get("id")) == "humanoid", "unknown race falls back to the generic humanoid")
	check(str((VisualLibrary.mob_look("not_a_model")["model"] as Dictionary).get("scene")) == "", "unknown model id is a placeholder")


## The bundled data with the highlander male drawn by the class-styled humanoid model (as before Phase 9).
func _classic_data() -> Dictionary:
	var data: Dictionary = (VisualLibrary.data() as Dictionary).duplicate(true)
	(data["races"]["highlander"] as Dictionary)["male"] = {"model": "humanoid_male", "scale": 1.0,
		"tints": {"Skin": "#dcae88", "Hair": "#5a3820"}, "parts": {"hair": "HairShort", "beard": "Beard"}}
	return data


func _add(look: Dictionary, scale: float = 1.0) -> CharacterVisual:
	var cv: CharacterVisual = CharacterVisual.new()
	cv.build(look, scale)
	root.add_child(cv)
	return cv


func _test_model_loads_and_animates() -> void:
	section("outfit model")
	VisualLibrary.override_data(null)
	var pf: CharacterVisual = _add(VisualLibrary.player_look("highlander", 1, "warrior"))
	await process_frame
	check(not pf.is_placeholder and pf.skeleton() != null, "the outfit GLB loads with a skeleton")
	for s: String in ["idle", "combat_idle", "walk", "run", "attack1", "attack2", "cast", "hit", "death", "sit"]:
		check(pf.has_clip_for(s), "outfit clip mapped for " + s)
	var pba: BoneAttachment3D = pf.attached.get("main_hand") as BoneAttachment3D
	check(pba != null and pba.bone_name == "RightHand", "the sword follows the right hand")
	var pinst: Node3D = pf.model_instance()
	var own_sword: Node3D = pinst.find_child("arming-sword-body", true, false) as Node3D if pinst != null else null
	var body_mesh: Node3D = pinst.find_child("body", true, false) as Node3D if pinst != null else null
	check(own_sword != null and not own_sword.visible, "the model's own sword is hidden")
	check(body_mesh != null and body_mesh.visible, "the body is shown")
	var vc: bool = false
	for n: Node in pinst.find_children("*", "MeshInstance3D", true, false):
		var o: BaseMaterial3D = (n as MeshInstance3D).get_surface_override_material(0) as BaseMaterial3D
		vc = vc or (o != null and o.vertex_color_use_as_albedo)
	check(vc, "vertex colours are its albedo")
	pf.queue_free()
	var plate: Dictionary = {"main_hand": "", "off_hand": "", "ranged": "", "chest": "plate_t5_chest", "head": ""}
	var tinted_pf: CharacterVisual = _add(VisualLibrary.player_look("highlander", 1, "warrior", plate))
	await process_frame
	var tl: Color = tinted_pf.look["outfit_tint"]
	var garment: MeshInstance3D = tinted_pf.model_instance().find_child("garment-tunic", true, false) as MeshInstance3D
	var hair: MeshInstance3D = tinted_pf.model_instance().find_child("garment-hair", true, false) as MeshInstance3D
	check(garment != null and hair != null, "the outfit has a tunic and hair to tint")
	var gm: BaseMaterial3D = garment.get_surface_override_material(0) as BaseMaterial3D if garment != null else null
	var hm: BaseMaterial3D = hair.get_surface_override_material(0) as BaseMaterial3D if hair != null else null
	check(gm != null and gm.albedo_color.is_equal_approx(tl), "the tier tint colours the garments")
	check(hm != null and not hm.albedo_color.is_equal_approx(tl), "but not the hair")
	tinted_pf.queue_free()
	section("humanoid model")
	VisualLibrary.override_data(_classic_data())
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
	VisualLibrary.override_data(null)


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
	VisualLibrary.override_data(null)
	var data: Dictionary = (VisualLibrary.data() as Dictionary).duplicate(true)
	(data["models"]["polyfork_human_warrior"] as Dictionary)["scene"] = "res://assets/models/missing/nope.glb"
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
	(data["races"]["highlander"] as Dictionary)["female"] = {"model": "wolf"}
	VisualLibrary.override_data(data)
	var cv: CharacterVisual = _add(VisualLibrary.player_look("highlander", 0, "monk"))
	await process_frame
	check(str(cv.model_def.get("id")) == "wolf", "a JSON edit swaps the model")
	check(not cv.is_placeholder and cv.has_clip_for("run"), "and it animates with its own map")
	cv.queue_free()
	VisualLibrary.override_data(null)


func _test_third_party_models() -> void:
	section("mob models")
	VisualLibrary.override_data(null)
	# Polyfork monsters carry the 11 retargeted clips the game plays.
	var knight: CharacterVisual = _add(VisualLibrary.mob_look("pf_dark_knight"), 1.1)
	await process_frame
	check(not knight.is_placeholder, "the revenant knight loads")
	for st: String in ["idle", "combat_idle", "walk", "run", "attack1", "attack2", "cast", "hit", "death", "sit"]:
		check(knight.has_clip_for(st), "pf_dark_knight maps " + st)
	var golem: CharacterVisual = _add(VisualLibrary.mob_look("pf_furnace_golem"), 1.6)
	await process_frame
	check(not golem.is_placeholder and golem.has_clip_for("attack1"), "the lava golem loads and attacks")
	var trainer: CharacterVisual = _add(VisualLibrary.mob_look("pf_staff_acolyte"))
	await process_frame
	check(not trainer.is_placeholder and trainer.has_clip_for("cast"), "the acolyte can cast")
	for cv: CharacterVisual in [knight, golem, trainer]:
		cv.queue_free()


func _test_animal_models() -> void:
	section("animal models")
	VisualLibrary.override_data(null)
	var wolf: CharacterVisual = _add(VisualLibrary.mob_look("pf_wolf"), 0.9)
	await process_frame
	check(not wolf.is_placeholder, "the wolf loads")
	for s: String in ["idle", "walk", "run"]:
		check(wolf.has_clip_for(s), "wolf maps " + s)
	check(not wolf.has_clip_for("attack1") and not wolf.has_clip_for("death"), "no attack or death clip")
	var body: Node3D = wolf.get_node("Body") as Node3D
	wolf.step(0.05, 0.0, false, 0.0)
	var rest: float = body.position.z
	wolf.anim.trigger_attack(1)
	for i: int in range(4):
		wolf.step(0.05, 0.0, false, 0.0)
	check(body.position.z < rest - 0.05, "a swing with no clip lunges forward (%.2f)" % body.position.z)
	# A tiny model scaled up a lot (a 10 cm tarantula) lunges by its drawn size, not its scale factor.
	var spider: CharacterVisual = _add(VisualLibrary.mob_look("pf_tarantula"), 1.0)
	await process_frame
	var sbody: Node3D = spider.get_node("Body") as Node3D
	spider.step(0.05, 0.0, false, 0.0)
	spider.anim.trigger_attack(1)
	var deepest: float = 0.0
	for i: int in range(10):
		spider.step(0.05, 0.0, false, 0.0)
		deepest = minf(deepest, sbody.position.z)
	check(deepest < -0.02 and deepest > -0.6, "a tarantula's lunge stays within its body (%.2f m)" % deepest)
	spider.queue_free()
	wolf.anim.set_dead(true, false)
	for i: int in range(30):
		wolf.step(0.05, 0.0, false, 0.0)
	check(absf(body.rotation.z) > 1.4, "a dead animal lies on its side")
	var crow: CharacterVisual = _add(VisualLibrary.mob_look("pf_crow"))
	await process_frame
	var players: Array[Node] = crow.model_instance().find_children("*", "AnimationPlayer", true, false) if crow.model_instance() != null else []
	check(not players.is_empty(), "the crow has an animation player")
	if not players.is_empty():
		var player: AnimationPlayer = players[0] as AnimationPlayer
		for i: int in range(12):
			crow.step(0.05, 6.0, false, 0.0)
		check(player.current_animation == "Fly", "a running bird flies (got %s)" % player.current_animation)
	# A new body (a changed outfit) carries on what the old one was doing.
	var before: CharacterVisual = _add(VisualLibrary.player_look("highlander", 1, "wizard"))
	before.anim.casting = true
	before.anim.sitting = true
	var after: CharacterVisual = _add(VisualLibrary.player_look("highlander", 1, "warrior"))
	after.carry_state_from(before)
	check(after.anim.casting and after.anim.sitting, "a swapped body keeps casting and sitting")
	for cv: CharacterVisual in [wolf, crow, before, after]:
		cv.queue_free()


## Spell effects come from effects.json: events, spell overrides, projectiles, lasting states, screen cues.
func _test_effects() -> void:
	section("effects")
	const Effects := preload("res://scripts/world/effects.gd")
	const ScreenCues := preload("res://scripts/ui/screen_cues.gd")
	check(not Effects.effect("hit").is_empty(), "the effect library loads")
	check(Effects.impact_for("heal", "Lay Light") == "heal", "a heal event plays the heal effect")
	check(Effects.projectile_for("Ember Dart") == "fire_bolt", "a spell override picks its projectile")
	check(Effects.projectile_for("Lay Light") == "", "heals do not fly")
	var fx: Effects = Effects.new()
	root.add_child(fx)
	var a: Node3D = Node3D.new()
	var b: Node3D = Node3D.new()
	root.add_child(a)
	root.add_child(b)
	b.position = Vector3(0.0, 0.0, 10.0)
	fx.play_event("spell_hit", a, b, 2, "Ember Dart")
	check(int(fx.played.get("fire_bolt", 0)) == 1, "a nuke at another target flies a projectile first")
	await create_timer(0.9).timeout  # a 10 m flight at 18 m/s
	check(int(fx.played.get("fire_hit", 0)) == 1, "and its impact plays when it arrives")
	# A projectile homes on a target that moves during its flight.
	var orbs_before: int = fx.get_child_count()
	fx.play_event("spell_hit", a, b, 2, "Ember Dart")
	var orb: Node3D = fx.get_child(fx.get_child_count() - 1) as Node3D if fx.get_child_count() > orbs_before else null
	b.position = Vector3(6.0, 0.0, 10.0)
	var near: float = 99.0
	var until: int = Time.get_ticks_msec() + 900
	while Time.get_ticks_msec() < until:
		await process_frame
		if orb != null and is_instance_valid(orb):
			near = minf(near, Vector2(orb.global_position.x, orb.global_position.z).distance_to(Vector2(6.0, 10.0)))
	check(near < 1.0, "a projectile lands on a target that moved (%.2f m off)" % near)
	b.position = Vector3(0.0, 0.0, 10.0)
	# A spell's own impact wins over its projectile's.
	var overrides: Dictionary = Effects.data()["spells"] as Dictionary
	overrides["Test Bolt"] = {"projectile": "fire_bolt", "impact": "drain_hit"}
	fx.play_event("spell_hit", a, b, 2, "Test Bolt")
	await create_timer(0.9).timeout
	check(int(fx.played.get("drain_hit", 0)) == 1 and int(fx.played.get("fire_hit", 0)) == 2, "a spell's impact override plays where its projectile lands")
	overrides.erase("Test Bolt")
	fx.play_event("heal", a, a, 0, "Lay Light")
	check(int(fx.played.get("heal", 0)) == 1, "a self-heal plays in place")
	fx.set_states(b, {"stunned": true, "rooted": true})
	check(b.get_node_or_null("FxState_stunned") != null and b.get_node_or_null("FxState_rooted") != null, "lasting states show markers")
	var spun: Node3D = b.get_node("FxState_stunned").get_child(0) as Node3D
	await create_timer(0.3).timeout
	check(absf(spun.rotation.y) > 0.1, "and their markers spin")
	fx.set_states(b, {"rooted": true})
	await process_frame
	check(b.get_node_or_null("FxState_stunned") == null and b.get_node_or_null("FxState_rooted") != null, "and clear when the state ends")
	fx.set_states(b, {})
	fx.set_states(b, {"rooted": true})  # ends and comes back in one frame
	await process_frame
	check(b.get_node_or_null("FxState_rooted") != null, "a state that comes back in the same frame keeps its marker")
	fx.cast(a, 1, "", true)
	fx.cast(a, 1, "", false)
	fx.cast(a, 1, "", true)  # a cast ends and the next starts in one frame
	fx.cast(a, 1, "", false)
	await process_frame
	check(a.get_node_or_null("FxCast") == null, "the next cast's end stops its motes")
	fx.cast(a, 1, "", true)
	check(a.get_node_or_null("FxCast") != null, "casting shows motes")
	fx.cast(a, 0, "", false)
	var cues: ScreenCues = ScreenCues.new()
	root.add_child(cues)
	cues.update_state(0.1, {})
	await process_frame
	check(cues.showing == "low_health", "low health shows the red vignette")
	cues.update_state(0.9, {"mezzed": true})
	await process_frame
	check(cues.showing == "mezzed", "being mezzed tints the screen")
	for n: Node in [fx, a, b, cues]:
		n.queue_free()


## Every spell, ability and item resolves to an icon file; the theme builds.
func _test_icons_and_theme() -> void:
	section("icons and theme")
	const Icons := preload("res://scripts/ui/icons.gd")
	const GameTheme := preload("res://scripts/ui/game_theme.gd")
	var Effects := preload("res://scripts/world/effects.gd")
	var EnvironmentArt := preload("res://scripts/world/environment_art.gd")
	var missing: Array[String] = []
	for spell_name: Variant in (Effects.data().get("by_name", {}) as Dictionary).keys():
		var icon: Array = Icons.for_spell(str(spell_name))
		if icon.is_empty() or Icons.texture(str(icon[0])) == null:
			missing.append(str(spell_name))
	check(missing.is_empty(), "every spell and ability has an icon (missing: %s)" % ", ".join(missing.slice(0, 5)))
	var missing_items: Array[String] = []
	for id: Variant in (ItemDb.data().get("items", {}) as Dictionary).keys():
		var icon: Array = Icons.for_item(ItemDb.item(str(id)))
		if Icons.texture(str(icon[0])) == null:
			missing_items.append(str(id))
	check(missing_items.is_empty(), "every item has an icon (missing: %s)" % ", ".join(missing_items.slice(0, 5)))
	check(str(Icons.for_item(ItemDb.item("staff_t3"))[0]) == "staff" and str(Icons.for_item(ItemDb.item("cloth_t1_chest"))[0]) == "chest", "staves and chest pieces get their own icons")
	check(str(Icons.for_spell("Ember Dart")[0]) == "nuke", "a nuke gets the bolt")
	# Graphics quality scales the art systems.
	var st: Node = root.get_node_or_null("/root/Settings")
	if st != null:
		var before: String = str(st.get("quality"))
		st.set("quality", "low")
		st.call("apply_quality")
		check(is_equal_approx(EnvironmentArt.density_scale, 0.0) and Effects.density < 0.5, "Low turns clutter off and thins effects")
		st.set("quality", "high")
		st.call("apply_quality")
		check(is_equal_approx(EnvironmentArt.density_scale, 1.0) and is_equal_approx(Effects.density, 1.0), "High restores them")
		st.set("quality", before)
		st.call("apply_quality")
	var t: Theme = GameTheme.build()
	check(t.has_stylebox("panel", "PanelContainer") and t.has_stylebox("normal", "Button"), "the UI theme builds panels and buttons")


## The Credits screen lists every asset row of ASSETS.md, each Polyfork model, and the licence texts.
func _test_credits() -> void:
	section("credits")
	const CreditsWindow := preload("res://scripts/ui/credits_window.gd")
	var d: Dictionary = CreditsWindow.data()
	check((d.get("assets", []) as Array).size() >= 20, "the asset list loads (%d rows)" % (d.get("assets", []) as Array).size())
	check((d.get("model_credits", []) as Array).size() >= 190, "every Polyfork model has a credit line (%d)" % (d.get("model_credits", []) as Array).size())
	var lic: Dictionary = d.get("licenses", {}) as Dictionary
	check(lic.has("CC-BY-4.0") and lic.has("CC0-1.0") and lic.has("Godot-LICENSE") and str(lic["CC-BY-4.0"]).contains("Attribution 4.0"), "the licence texts are bundled")
	var w: CreditsWindow = CreditsWindow.new()
	root.add_child(w)
	await process_frame
	check(w.get_child_count() > 0, "the credits window builds")
	w.queue_free()


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
	var height: Callable = func(lk: Dictionary) -> float: return float((lk["model"] as Dictionary).get("nameplate_height")) * (lk["scale"] as Vector3).y
	check(float(height.call(small)) < 1.0 and float(height.call(big)) > 2.0, "tinkerlings are tiny and morrok huge (%.2f, %.2f)" % [height.call(small), height.call(big)])


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
	check(str((r.look()["model"] as Dictionary).get("id")).begins_with("polyfork_dwarf"), "a race illusion looks like that race")
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
		"main_hand": "mace_t2", "off_hand": "", "ranged": "", "chest": "leather_t1_chest", "head": ""}, 0.0)
	check(str((r.look()["model"] as Dictionary).get("id")) == "polyfork_human_assassin", "a spawned player wears its chest piece's outfit")
	check((r.look()["held"] as Dictionary).has("main_hand"), "a spawned player holds its worn weapon")
	r.set_equipment({"main_hand": "", "off_hand": "buckler_t1", "ranged": "", "chest": "", "head": ""})
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
	# A wall against the zone's edge never traps a body (the server's movement test does the same).
	var edge: ZoneGeom = ZoneGeom.new()
	edge.boxes.append(PackedFloat32Array([0.5, 5.0, 0.5, 5.0, 0.0]))
	var q: Vector3 = Vector3(1.5, 0.0, 5.0)
	var trapped: bool = false
	for i: int in range(20):
		q = edge.step(q, -1.0, 0.0, 7.0, 0.05, Rect2(0, 0, 10, 10), 0.4)
		trapped = trapped or edge.overlaps(q.x, q.z, 0.4)
	check(not trapped, "the edge clamp never pushes into a wall")
	var r: Vector3 = Vector3(0.25, 0.0, 5.0)
	for i: int in range(20):
		r = edge.step(r, 1.0, 0.0, 7.0, 0.05, Rect2(0, 0, 10, 10), 0.4)
	check(r.x > 1.4 and not edge.overlaps(r.x, r.z, 0.4), "walks out of a wall at the edge")


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


## The world HUD (Phase 6f): dialogue, journal and tracker, factions, map views.
func _test_world_ui() -> void:
	section("world ui")
	var WorldUi: GDScript = load("res://scripts/ui/world_ui.gd")
	var screen: Control = Control.new()  # a 1280x720 HUD to lay the windows out in
	screen.size = Vector2(1280, 720)
	get_root().add_child(screen)
	var ui: Control = WorldUi.new()
	screen.add_child(ui)
	await process_frame
	var chosen: Array = []
	ui.connect("dialog_chosen", func(npc: int, key: String) -> void: chosen.append([npc, key]))
	ui.call("show_dialog", {"npc_id": 7, "name": "Captain", "text": "Hello.", "options": [{"key": "offer:q1", "label": "Rats (level 2)"}]})
	check(bool(ui.call("dialog_visible")), "dialogue opens")
	ui.call("choose", "offer:q1")
	check(chosen.size() == 1 and int(chosen[0][0]) == 7 and str(chosen[0][1]) == "offer:q1", "a choice is sent back with its key")
	var q: Dictionary = {"id": "q1", "name": "Rats", "level": 2, "zone": "emberglade", "step": 0, "steps": 1, "text": "Slay 8 in The Emberglade",
		"have": 3, "need": 8, "ready": false, "epic": false, "turn_in": "Captain"}
	check(WorldUi.quest_line(q) == "Slay 8 in The Emberglade (3/8)", "journal line shows progress")
	ui.call("set_journal", {"quests": [q], "completed": 4})
	await process_frame
	check((ui.get("_tracker") as Control).get_child_count() == 1, "the tracker lists the quest")
	ui.call("set_factions", [{"id": "emberhold_watch", "name": "The Emberhold Watch", "standing": 320, "regard": 3}])
	await process_frame
	check((ui.get("_faction_list") as Control).get_child_count() == 1, "the factions tab lists standings")
	var z: RefCounted = ZoneData.new()
	z.load_file("rattlebone_warren")
	ui.call("set_map_source", func() -> Dictionary:
		return {"pos": Vector3.ZERO, "yaw": 0.0, "dots": [[Vector3(5, 0, 5), Color.RED]], "lines": z.call("zone_lines"), "bounds": z.get("bounds"),
			"zone": "Rattlebone Warren", "walls": [], "circles": [],
			"tiles": {"cell": z.get("dungeon").get("cell"), "origin": z.get("dungeon").get("origin"), "w": z.get("dungeon").get("w"), "solid": z.get("dungeon").get("solid")}},
		"rattlebone_warren")
	ui.call("toggle_map")
	await process_frame
	await process_frame
	check(bool(ui.call("map_visible")), "the map opens and draws")
	ui.call("toggle_journal")
	await process_frame
	check(bool(ui.call("windows_on_screen")), "the map, journal, minimap and tracker all lie inside the viewport: %s" % str(ui.call("offscreen")))
	check(bool(ui.call("any_window_open")), "open windows are known to the HUD")
	ui.call("close_windows")
	check(not bool(ui.call("any_window_open")), "Escape closes them")
	screen.queue_free()


# ------------------------------------------------------------------------------ Phase 7: chat and social windows

func _test_chat_window() -> void:
	section("chat window")
	var ChatWindow: GDScript = load("res://scripts/ui/chat_window.gd")
	var chat: PanelContainer = ChatWindow.new()
	chat.set("offline", true)
	chat.size = Vector2(380, 240)
	get_root().add_child(chat)
	await process_frame
	# Whatever this machine's saved filters say, the test starts with every channel shown (offline: nothing is saved).
	for f: Variant in ChatWindow.get_script_constant_map()["FILTERABLE"]:
		chat.call("set_filter", int((f as Array)[0]), true)
	chat.set("my_name", "Aldric")
	var cmds: Array = []
	chat.connect("command", func(n: String, a: String) -> void: cmds.append([n, a]))
	chat.call("say", "hello there")
	chat.call("say", "/ooc anyone selling bread?")
	chat.call("say", "/tell Brenna meet at the gate")
	chat.call("say", "/g pull")
	chat.call("say", "/gu evening all")
	chat.call("say", "/me waves.")
	chat.call("say", "/invite Brenna")
	var sent: Array = chat.get("sent")
	check(sent.size() == 6, "six chat lines sent (%d)" % sent.size())
	if sent.size() == 6:
		check(int(sent[0]["channel"]) == 0, "plain text is said")
		check(int(sent[1]["channel"]) == 4 and str(sent[1]["text"]) == "anyone selling bread?", "/ooc goes out of character")
		check(int(sent[2]["channel"]) == 5 and str(sent[2]["target"]) == "Brenna" and str(sent[2]["text"]) == "meet at the gate", "/tell NAME text")
		check(int(sent[3]["channel"]) == 7, "/g is the group")
		check(int(sent[4]["channel"]) == 8, "/gu is the guild")
		check(int(sent[5]["channel"]) == 9, "/me emotes")
	check(cmds.size() == 1 and str(cmds[0][0]) == "invite" and str(cmds[0][1]) == "Brenna", "other slash lines are commands")
	# Incoming lines read naturally and land in their tabs; the All tab honours its filters.
	chat.call("add_message", 5, "Brenna", "on my way")
	check(str(chat.get("last_tell_from")) == "Brenna", "a tell remembers who to reply to")
	chat.call("say", "/r coming")
	sent = chat.get("sent")
	check(int(sent.back()["channel"]) == 5 and str(sent.back()["target"]) == "Brenna", "/r replies to the last tell")
	(chat.get("_input") as LineEdit).release_focus()
	chat.set("last_tell_from", "")
	chat.call("focus_input")  # typing before anyone has sent a tell...
	chat.call("add_message", 5, "Brenna", "on my way")  # ...one arrives
	chat.call("say", "/r oops")
	check(str(sent.back()["text"]) != "oops", "a reply started with nobody to answer is not sent to whoever writes next")
	(chat.get("_input") as LineEdit).release_focus()
	chat.call("focus_input")  # typing a reply to Brenna...
	chat.call("add_message", 5, "Corvin", "psst")  # ...when Corvin's tell arrives
	chat.call("say", "/r still for Brenna")
	sent = chat.get("sent")
	check(str(sent.back()["target"]) == "Brenna", "a tell arriving mid-reply does not redirect it")
	(chat.get("_input") as LineEdit).release_focus()
	check(str(chat.call("reply_target")) == "Corvin", "the next reply goes to the newest sender")
	chat.call("add_message", 7, "Corvin", "inc now")
	chat.call("add_message", 8, "", "Guild message of the day: Raid at dusk")
	chat.call("add_message", 6, "Brenna", "coming")
	check(str(chat.call("tab_text", "Group")).contains("Corvin tells the group"), "group tab: " + str(chat.call("tab_text", "Group")))
	check(str(chat.call("tab_text", "Guild")).contains("Raid at dusk"), "guild notices go to the guild tab")
	check(str(chat.call("tab_text", "Tells")).contains("Brenna tells you"), "tells tab has incoming tells")
	check(str(chat.call("tab_text", "Tells")).contains("You told Brenna"), "...and the echo of yours")
	check(not str(chat.call("tab_text", "Chat")).contains("inc now"), "the chat tab leaves group lines out")
	chat.call("set_filter", 4, false)
	chat.call("add_message", 4, "Spammer", "wts stuff")
	check(not str(chat.call("tab_text", "All")).contains("wts stuff"), "a filtered channel stays off the All tab")
	check(str(chat.call("tab_text", "Chat")).contains("wts stuff"), "...but its own tab still has it")
	chat.call("add_message", 3, "Aldric", "over here")
	check(str(chat.call("tab_text", "All")).contains("You shout"), "your own shout reads as yours")
	# On the Group tab, plain text goes to the group.
	chat.call("show_tab_named", "Group")
	chat.call("say", "ready")
	sent = chat.get("sent")
	check(int(sent.back()["channel"]) == 7, "the group tab speaks to the group")
	chat.queue_free()


func _test_social_ui() -> void:
	section("social ui")
	var SocialUi: GDScript = load("res://scripts/ui/social_ui.gd")
	var screen: Control = Control.new()
	screen.size = Vector2(1280, 720)
	get_root().add_child(screen)
	var ui: Control = SocialUi.new()
	screen.add_child(ui)
	await process_frame
	ui.set("my_name", "Aldric")
	var members: Array = []
	var names: Array = ["Aldric", "Brenna", "Corvin", "Dorian", "Elsbeth", "Fenwick"]
	for i: int in names.size():
		members.append({"name": names[i], "class_id": "warrior", "level": 10, "zone": "The Emberglade", "entity_id": 100 + i,
			"hp_pct": 100 - i * 10, "mana_pct": 255 if i % 2 == 0 else 50, "endurance_pct": 80, "dead": i == 5})
	ui.call("set_group", {"group_id": 3, "leader": "Aldric", "loot_rule": 1, "members": members})
	await process_frame
	check(bool(ui.call("group_visible")), "joining a group opens the group window")
	check((ui.get("_group_rows") as Control).get_child_count() == 6, "six member rows")
	check(int(ui.call("member_entity", 0)) == 101, "F2 targets the first other member")
	check(int(ui.call("member_entity", 4)) == 105, "F6 the fifth")
	check(int(ui.call("member_entity", 5)) == 0, "no seventh")
	var gb: Dictionary = ui.get("_group_buttons")
	check(not (gb["kick"] as Button).disabled, "the leader can kick")
	ui.call("select_member", "Corvin")
	(gb["kick"] as Button).emit_signal("pressed")
	var sent: Array = ui.get("sent")
	check(sent.size() > 0 and str(sent.back()) == "kick Corvin", "kick names the selected member: %s" % str(sent))
	ui.call("set_group", {"group_id": 3, "leader": "Brenna", "loot_rule": 0, "members": members})
	check((gb["kick"] as Button).disabled, "a member cannot kick")
	# Guild window: tools follow the rank's permissions.
	var ranks: Array = [{"name": "Leader", "permissions": 127}, {"name": "Officer", "permissions": 63}, {"name": "Member", "permissions": 33},
		{"name": "Recruit", "permissions": 32}]
	var roster: Array = [{"name": "Aldric", "rank": 3, "level": 10, "class_id": "warrior", "zone": "Emberhold", "online": true},
		{"name": "Brenna", "rank": 0, "level": 12, "class_id": "cleric", "zone": "", "online": false}]
	ui.call("set_guild", {"guild_id": 9, "name": "The Iron Vow", "motd": "Raid at dusk", "my_rank": 3, "ranks": ranks, "members": roster})
	ui.call("toggle_guild")
	await process_frame
	var gu: Dictionary = ui.get("_guild_buttons")
	check(str((ui.get("_guild_title") as Label).text).contains("The Iron Vow"), "the guild window names the guild")
	check(str((ui.get("_guild_motd") as Label).text).contains("Raid at dusk"), "...and shows the message of the day")
	check(not (ui.get("_guild_motd_edit") as Control).visible, "a recruit cannot set the message of the day")
	check((gu["invite"] as Button).disabled, "nor invite")
	check(not (gu["disband"] as Button).visible, "nor disband")
	ui.call("set_guild", {"guild_id": 9, "name": "The Iron Vow", "motd": "", "my_rank": 0, "ranks": ranks, "members": roster})
	check((ui.get("_guild_motd_edit") as Control).visible, "the leader can")
	check((gu["disband"] as Button).visible, "the leader can disband")
	check((ui.get("_guild_rows") as Control).get_child_count() == 2, "the roster lists both members")
	ui.call("set_guild", {"guild_id": 0, "name": "", "motd": "", "my_rank": 0, "ranks": ranks, "members": []})
	check((ui.get("_guild_create") as Control).visible, "out of a guild: found one")
	# Friends and who.
	ui.call("set_friends", {"friends": [{"name": "Brenna", "online": true, "level": 12, "class_id": "cleric", "zone": "Emberhold"},
		{"name": "Corvin", "online": false, "level": 0, "class_id": "", "zone": ""}], "ignored": ["Spammer"], "muted_channels": 16})
	check((ui.get("_friend_rows") as Control).get_child_count() == 2, "both friends listed")
	check((ui.get("_ignore_rows") as Control).get_child_count() == 1, "the ignore list")
	var ch: Dictionary = ui.get("_channel_checks")
	check(not (ch["ooc"] as CheckBox).button_pressed, "OOC (bit 4) shows as off")
	check((ch["say"] as CheckBox).button_pressed, "say is on")
	var many: Array = []
	for i: int in 64:
		many.append({"name": "Friend%c" % (65 + i % 26), "online": false, "level": 0, "class_id": "", "zone": ""})
	ui.call("set_friends", {"friends": many, "ignored": [], "muted_channels": 0})
	ui.call("toggle_friends")
	await process_frame
	check(bool(ui.call("windows_on_screen")), "sixty-four friends still fit on screen (the list scrolls)")
	ui.call("toggle_friends")
	ui.call("toggle_friends")
	ui.call("show_who", {"query": "all warrior", "total": 2, "entries": [
		{"name": "Aldric", "level": 10, "class_id": "warrior", "race": "highlander", "zone": "Emberhold", "guild": "The Iron Vow", "gm": false, "group_size": 6},
		{"name": "Dorian", "level": 11, "class_id": "warrior", "race": "deepkin", "zone": "Cragmouth", "guild": "", "gm": true, "group_size": 0}]})
	await process_frame
	check(bool(ui.call("who_visible")), "who results open their window")
	check((ui.get("_who_rows") as Control).get_child_count() == 2, "two who rows")
	# Invitations answer with the right command.
	ui.call("show_invite", {"kind": 1, "from_name": "Brenna", "guild_name": "The Iron Vow", "expires_ms": 60000})
	check(bool(ui.call("invite_visible")), "a guild invitation pops up")
	ui.call("answer_invite", true)
	sent = ui.get("sent")
	check(str(sent.back()) == "guildjoin", "accepting a guild invitation joins")
	ui.call("show_invite", {"kind": 0, "from_name": "Brenna", "guild_name": "", "expires_ms": 60000})
	ui.call("answer_invite", false)
	sent = ui.get("sent")
	check(str(sent.back()) == "decline", "declining a group invitation")
	await process_frame
	check(bool(ui.call("windows_on_screen")), "the social windows lie inside a 1280x720 screen")
	ui.call("close_windows")
	check(not bool(ui.call("any_window_open")), "Escape closes them")
	screen.queue_free()


# ------------------------------------------------------------------ Phase 8: settings, audio, login polish

func _test_keybinds() -> void:
	section("keybinds")
	var d: Dictionary = KeybindsLib.defaults()
	check(d.size() == KeybindsLib.ACTIONS.size(), "every action has default keys")
	var seen: Dictionary = {}
	var clash: bool = false
	for action: String in d.keys():
		for k: int in d[action]:
			clash = clash or seen.has(k)
			seen[k] = action
	check(not clash, "no key is bound to two actions by default")
	check(not seen.has(KEY_ESCAPE) and not seen.has(KEY_ENTER), "reserved keys are not bindable")
	var binds: Dictionary = KeybindsLib.defaults()
	var changed: Array[String] = KeybindsLib.rebind(binds, "auto_attack", KEY_W)
	check(changed.has("move_forward") and changed.has("auto_attack"), "rebinding to a taken key reports both actions")
	check(Array(binds["auto_attack"])[0] == KEY_W and not Array(binds["move_forward"]).has(KEY_W), "the key moves; it is never shared")
	check(Array(binds["move_forward"]).has(KEY_UP), "the other action keeps its second key")
	check(KeybindsLib.rebind(binds, "auto_attack", KEY_ESCAPE).is_empty(), "Escape cannot be bound")
	check(KeybindsLib.rebind(binds, "no_such_action", KEY_Z).is_empty(), "unknown actions are refused")
	var saved: Dictionary = KeybindsLib.to_config(binds)
	check(saved.has("auto_attack") and not saved.has("inventory"), "only changed actions are saved")
	var restored: Dictionary = KeybindsLib.from_config(saved)
	check(Array(restored["auto_attack"]) == Array(binds["auto_attack"]), "saved bindings load back")
	var hand_edited: Dictionary = KeybindsLib.from_config({"loot": "%d" % KEY_I, "inventory": "x,%d" % KEY_ESCAPE, "bogus": "5"})
	check(Array(hand_edited["inventory"]).is_empty() or not Array(hand_edited["inventory"]).has(KEY_ESCAPE), "bad keycodes and reserved keys are dropped")
	var holders: int = 0
	for action: String in hand_edited.keys():
		if Array(hand_edited[action]).has(KEY_I):
			holders += 1
	check(holders == 1, "a key claimed twice in a hand-edited file stays with one action")
	check(KeybindsLib.key_name([]) == "(unbound)", "unbound actions read as unbound")


func _test_server_probe() -> void:
	section("server probe")
	var ok: Dictionary = Probe.parse({"ok": true, "message": "Welcome"}, {"motd": "Hello", "players_online": 3, "max_players": 200, "server_version": "0.1.0"})
	check(bool(ok["online"]) and bool(ok["version_ok"]) and str(ok["motd"]) == "Hello", "a status reply is parsed")
	var line: String = Probe.describe(ok)
	check(line.contains("3 players") and line.contains("200") and line.contains("0.1.0"), "the status line shows players and version: " + line)
	check(Probe.describe(Probe.parse({"ok": true}, {"players_online": 1})).contains("1 player"), "one player is singular")
	var old: Dictionary = Probe.parse({"ok": false, "message": "Please update your client."}, {})
	check(Probe.describe(old) == "Please update your client.", "a version mismatch shows the server's message")
	check(Probe.describe({"online": false, "message": "The server is not answering."}).begins_with("Offline"), "an unreachable server reads offline")


func _test_sound_events() -> void:
	section("sound events")
	var events: Dictionary = AudioLib.load_events("res://data/sounds.json")
	check(events.size() >= 15, "the sound list loads (%d events)" % events.size())
	for name: String in ["ui_click", "melee_hit", "spell_cast", "level_up", "death", "loot_coin", "tell", "ambience"]:
		check(events.has(name), "event %s is defined" % name)
	var missing: Array[String] = []
	for name: String in events.keys():
		var def: Dictionary = events[name]
		var files: Array = def.get("files", [def.get("file", "")]) as Array
		for f: Variant in files:
			if not ResourceLoader.exists(str(f)):
				missing.append("%s: %s" % [name, str(f)])
		check(["Music", "Ambient", "SFX", "UI"].has(str(def.get("bus", ""))), "%s plays on a known bus" % name)
	check(missing.is_empty(), "every sound file exists (%s)" % ", ".join(missing))
	# Phase 9: zone music and ambience by biome, boss music, footsteps per surface.
	var au: Node = root.get_node_or_null("/root/Audio")
	check(au != null, "the Audio autoload is running")
	if au != null:
		check(str(au.call("zone_event", "music", "emberhold", "town")) == "music_town", "Emberhold plays the town theme")
		check(str(au.call("zone_event", "music", "obsidian_citadel", "citadel")) == "music_choir", "a zone override wins")
		check(str(au.call("zone_event", "ambience", "gloomfen_bog", "bog")) == "amb_dark", "the bog has its own ambience")
		check(int(au.call("boss_kind", "The Emberwyrm")) == 2 and int(au.call("boss_kind", "The Rattle King")) == 1, "raid bosses and bosses are known")
		check(str(au.call("fight_music", "a grey wolf", 5)) == "music_battle" and str(au.call("fight_music", "a grey wolf", 0)) == "", "hard fights get battle music")
		check(str(au.call("step_event", "bog")) == "step_mud" and str(au.call("step_event", "town")) == "step_stone", "footsteps follow the ground")
		au.call("set_channel", "music", "music_town")
		check(str((au.get("channel_event") as Dictionary)["music"]) == "music_town", "music plays on its channel")
		au.call("set_channel", "music", "")


func _test_disconnect_text() -> void:
	section("disconnect text")
	check(UiLib.disconnect_text("") == "", "no dialog without a reason")
	check(UiLib.disconnect_text("Logged out.") == "", "no dialog for a chosen logout")
	check(UiLib.disconnect_text("Connection to the server was lost.").contains("log in again"), "a lost connection explains what to do")
	check(UiLib.disconnect_text("The server is shutting down. Your character has been saved.").contains("shutting down"), "server reasons are shown as sent")
