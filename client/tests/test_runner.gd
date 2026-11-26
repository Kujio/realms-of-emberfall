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
const PacketCrypto := preload("res://scripts/net/packet_crypto.gd")
const NetClient := preload("res://scripts/net/net_client.gd")
const ProtoLib := preload("res://scripts/net/protocol.gd")
const CraftUi := preload("res://scripts/ui/craft_ui.gd")

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
	_test_surfaces()
	await _test_world_ui()
	await _test_world_map()
	await _test_craft_ui()
	await _test_chat_window()
	await _test_social_ui()
	_test_keybinds()
	_test_server_probe()
	_test_sound_events()
	_test_disconnect_text()
	_test_packet_crypto()
	_test_net_client_sealing()
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
	# Phase 15: a raid mark shows above the name, in the mark's colour, and comes off again.
	var tag: Label3D = r.get("_tag") as Label3D
	r.set_raid_mark(2)
	check(tag.text.begins_with("+ 2 +") and tag.text.contains("Mira"), "a mark tops the name tag: %s" % tag.text)
	check(tag.modulate == RemoteEntity.MARK_COLORS[1], "in the mark's colour")
	r.set_raid_mark(0)
	check(tag.text == "Mira", "unmarked again: %s" % tag.text)
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


## The world's surfaces (Phase 11): textures, the ground past a zone's edge, water, sky and dungeon blocks.
func _test_surfaces() -> void:
	section("surfaces")
	var Surfaces := preload("res://scripts/world/surfaces.gd")
	var EnvironmentArt := preload("res://scripts/world/environment_art.gd")
	# Every texture a biome names is bundled.
	var named: Dictionary = {"macro": true, "clouds": true, "water_normal": true}
	var env: Dictionary = EnvironmentArt.data()
	var blocks: Array = [env.get("default", {})]
	blocks.append_array((env.get("biomes", {}) as Dictionary).values())
	for b: Variant in blocks:
		for key: String in ["ground", "dungeon"]:
			var blk: Variant = (b as Dictionary).get(key)
			if blk is Dictionary:
				for layer: String in ["base", "high", "slope", "shore", "floor", "wall", "wall_top"]:
					if (blk as Dictionary).has(layer):
						named[str((blk as Dictionary)[layer])] = true
	for t: String in named:
		check(Surfaces.texture(t) != null, "texture %s is bundled" % t)
	check(named.size() >= 10, "biomes use a range of textures")

	var meadow: ZoneData = ZoneData.new()
	meadow.load_file("emberglade")
	var shore: ZoneData = ZoneData.new()
	shore.load_file("duskmere_shore")
	var bog: ZoneData = ZoneData.new()
	bog.load_file("gloomfen_bog")
	# Water comes from the zone theme.
	check(Surfaces.water(meadow).is_empty(), "the meadow has no water")
	var sw: Dictionary = Surfaces.water(shore)
	check(not sw.is_empty() and is_equal_approx(float(sw["level"]), -0.6), "the shore has its water level")
	check((sw["sea"] as Array) == ["-z"], "and the sea lies past its -z edge")
	var bw: Dictionary = Surfaces.water(bog)
	var pooled: bool = false
	for h: PackedFloat32Array in bog.geom.hills:
		if bog.geom.height(h[0], h[1]) < float(bw.get("level", 0.0)) - 0.2:
			pooled = true
	check(pooled, "the bog has hollows below its water: pools")

	# The skirt: the zone's own height inside, hills rising outside, the sea side sinking.
	var no_sea: Array[String] = []
	var b: Rect2 = meadow.bounds
	var inside: Vector2 = b.get_center() + Vector2(37.0, -51.0)
	check(is_equal_approx(Surfaces.skirt_height(meadow, inside.x, inside.y, 25.0, no_sea, 0.0), meadow.geom.height(inside.x, inside.y)),
		"inside the bounds the skirt is the zone's terrain")
	var edge: float = meadow.geom.height(b.end.x, 0.0)
	var near: float = Surfaces.skirt_height(meadow, b.end.x + 1.0, 0.0, 25.0, no_sea, 0.0)
	var mid: float = Surfaces.skirt_height(meadow, b.end.x + 150.0, 0.0, 25.0, no_sea, 0.0)
	var far: float = Surfaces.skirt_height(meadow, b.end.x + Surfaces.SKIRT, 0.0, 25.0, no_sea, 0.0)
	check(absf(near - edge) < 0.05, "the skirt meets the zone's edge without a step")
	check(mid > edge and far > mid and far - edge > 25.0 * 0.4, "and rises into hills away from it")
	var sea: Array[String] = ["-z"]
	var level: float = float(sw["level"])
	var shelf: float = Surfaces.skirt_height(shore, 0.0, shore.bounds.position.y - Surfaces.SEA_SHELF, 20.0, sea, level)
	check(shelf < level - Surfaces.SEA_DEPTH + 0.01, "a sea side sinks under the water")
	check(Surfaces.skirt_height(shore, 0.0, shore.bounds.end.y + 150.0, 20.0, sea, level) > level, "the other sides stay dry")
	# Past the corner of a sea side and a land side, the coast bends: no cliff along the diagonal.
	var cx: float = shore.bounds.end.x
	var cz: float = shore.bounds.position.y
	var steepest: float = 0.0
	for dd: float in [20.0, 60.0, 120.0, 200.0]:
		var a: float = Surfaces.skirt_height(shore, cx + dd + 1.0, cz - dd, 20.0, sea, level)
		var c: float = Surfaces.skirt_height(shore, cx + dd, cz - dd - 1.0, 20.0, sea, level)
		steepest = maxf(steepest, absf(a - c))
	check(steepest < 1.0, "a sea corner has no cliff (%.2f m across 1.4 m)" % steepest)
	# The terrain grid keeps the pre-skirt samples inside the bounds, even when the size is not a whole
	# number of steps (a 899 m side on a 5.625 m grid still ends on 899), and reaches SKIRT outside.
	for span: Array in [[-450.0, 450.0, 5.625], [0.0, 899.0, 5.625], [-400.0, 400.0, 5.0], [10.0, 33.0, 4.0]]:
		var lo: float = span[0]
		var hi: float = span[1]
		var stp: float = span[2]
		var g: PackedFloat32Array = Surfaces.grid_coords(lo, hi, stp)
		var old: PackedFloat32Array = PackedFloat32Array()
		for i: int in range(int(ceilf((hi - lo) / stp)) + 1):
			old.append(minf(lo + float(i) * stp, hi))
		var at: int = g.find(lo)
		var same: bool = at >= 0 and at + old.size() <= g.size()
		for i: int in range(old.size()):
			same = same and is_equal_approx(g[at + i], old[i])
		var rising: bool = true
		for i: int in range(1, g.size()):
			rising = rising and g[i] > g[i - 1]
		check(same and rising, "grid %s..%s: the in-bounds samples are unchanged" % [lo, hi])
		check(g[0] <= lo - Surfaces.SKIRT and g[g.size() - 1] >= hi + Surfaces.SKIRT, "grid %s..%s: the skirt reaches out" % [lo, hi])
	# Zones that draw no skirt use their own terrain height (the lineup camera).
	var hub: ZoneData = ZoneData.new()
	hub.load_file("emberhold")
	check(is_equal_approx(Surfaces.ground_height(hub, hub.bounds.end.x + 100.0, 0.0), hub.geom.height(hub.bounds.end.x + 100.0, 0.0)),
		"a hub has no skirt height")
	# Dungeon wall tops follow the wall's scale, floors their own.
	var mine: ZoneData = ZoneData.new()
	mine.load_file("cinderdeep_mines")
	var dm: Array[ShaderMaterial] = Surfaces.dungeon_materials(mine)
	check(is_equal_approx(float(dm[1].get_shader_parameter("top_scale")), float(dm[1].get_shader_parameter("side_scale"))),
		"wall tops use the wall scale")
	check(not is_equal_approx(float(dm[0].get_shader_parameter("top_scale")), float(dm[1].get_shader_parameter("top_scale"))),
		"and the mine's floor keeps its own")

	# Materials.
	var gm: ShaderMaterial = Surfaces.ground_material(bog, bog.geom.base, 3.0)
	check(gm.shader != null and gm.get_shader_parameter("base_tex") != null, "the ground material has its textures")
	check(is_equal_approx(float(gm.get_shader_parameter("water_level")), float(bw["level"])), "and knows the water level")
	var mm: ShaderMaterial = Surfaces.ground_material(meadow, 0.0, 3.0)
	check(float(mm.get_shader_parameter("water_level")) < -1000.0, "a dry zone has no shore band")
	var ground: Color = meadow.color("ground", Color.BLACK)
	var tint: Color = mm.get_shader_parameter("base_tint")
	check(tint.get_luminance() < ground.get_luminance() and tint.get_luminance() > 0.0, "the theme colour is darkened for the screen")

	# Built zones: terrain past the edge, water where the theme has it, textured sky and dungeons.
	for zid: String in ["emberglade", "duskmere_shore", "gloomfen_bog", "hollowmere_crypts"]:
		var z: ZoneData = ZoneData.new()
		z.load_file(zid)
		var root: Node3D = Node3D.new()
		get_root().add_child(root)
		ZoneBuilder.build(root, z)
		var we: WorldEnvironment = root.get_node_or_null("Environment") as WorldEnvironment
		if z.kind == "dungeon":
			var floor_mmi: MultiMeshInstance3D = root.get_node_or_null("Floor") as MultiMeshInstance3D
			var walls: MultiMeshInstance3D = root.get_node_or_null("Walls") as MultiMeshInstance3D
			check(floor_mmi != null and floor_mmi.material_override is ShaderMaterial, "%s: textured floor" % zid)
			check(walls != null and walls.material_override is ShaderMaterial, "%s: textured walls" % zid)
		else:
			check(we != null and we.environment.sky != null and we.environment.sky.sky_material is ShaderMaterial, "%s: the cloud sky" % zid)
			var terrain: MeshInstance3D = root.get_node_or_null("Terrain") as MeshInstance3D
			check(terrain != null and terrain.material_override is ShaderMaterial, "%s: textured terrain" % zid)
			if terrain != null:
				var box: AABB = terrain.get_aabb()
				check(box.position.x <= z.bounds.position.x - Surfaces.SKIRT + 1.0 and box.end.z >= z.bounds.end.y + Surfaces.SKIRT - 1.0,
					"%s: the ground reaches past the zone's edge" % zid)
			check((root.get_node_or_null("Water") != null) == not Surfaces.water(z).is_empty(), "%s: water only where the theme has it" % zid)
		root.free()


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
	# Phase 12: a level gained with the map open recolours the world map at once.
	var lvl: Array = [12]
	ui.set("level_source", func() -> int: return int(lvl[0]))
	ui.call("refresh_level")
	check(int((ui.get("world_map") as Control).get("level")) == 12, "the open world map takes the level")
	lvl[0] = 30
	ui.call("refresh_level")
	check(int((ui.get("world_map") as Control).get("level")) == 30, "and follows a level-up")
	ui.call("toggle_journal")
	await process_frame
	check(bool(ui.call("windows_on_screen")), "the map, journal, minimap and tracker all lie inside the viewport: %s" % str(ui.call("offscreen")))
	check(bool(ui.call("any_window_open")), "open windows are known to the HUD")
	ui.call("close_windows")
	check(not bool(ui.call("any_window_open")), "Escape closes them")
	screen.queue_free()


## The drawn world map (Phase 12): every place has a cell, roads join them, a route is found, and a
## click picks the place under the pointer.
func _test_world_map() -> void:
	section("world map")
	var WorldMap: GDScript = load("res://scripts/ui/world_map.gd")
	var world: Variant = JSON.parse_string(FileAccess.get_file_as_string("res://data/world.json"))
	check(world is Dictionary, "the world index is bundled")
	var map: Control = WorldMap.new()
	map.call("set_world", world)
	# Level bands, as the con colours: inside the band white, above you yellow then red, below blue,
	# green and grey.
	check(int(WorldMap.band_con(5, 1, 8)) == 3, "inside the band: white")
	check(int(WorldMap.band_con(5, 6, 10)) == 4 and int(WorldMap.band_con(5, 8, 15)) == 5, "above you: yellow, then red")
	check(int(WorldMap.band_con(10, 1, 8)) == 2 and int(WorldMap.band_con(14, 1, 8)) == 1 and int(WorldMap.band_con(40, 1, 8)) == 0,
		"below you: blue, green, grey")
	# Every place but the test zone is on the map; no two cities or zones share a cell.
	var cells: Dictionary = {}
	var placed: int = 0
	for z: Variant in (world as Dictionary)["zones"]:
		var zd: Dictionary = z as Dictionary
		var id: String = str(zd["id"])
		if str(zd["kind"]) == "test":
			check(map.call("cell_of", id) == null, "the test zone is not on the map")
			continue
		var c: Variant = map.call("cell_of", id)
		check(c != null, "%s is on the map" % id)
		placed += 1
		if str(zd["kind"]) != "dungeon":
			check(not cells.has(c), "%s has a cell of its own" % id)
			cells[c] = id
	check(placed >= 26, "every city, zone and dungeon is placed")
	# Roads go both ways, and a route leads from the first city to the far north and into a dungeon.
	check((map.call("neighbours", "emberhold") as PackedStringArray).has("emberglade"), "a city's road comes from its neighbour's zone line")
	var way: PackedStringArray = map.call("path", "emberhold", "stormcrown_peaks")
	check(way.size() >= 6 and way[0] == "emberhold" and way[way.size() - 1] == "stormcrown_peaks", "a route to the Stormcrown Peaks: %s" % str(way))
	var into: PackedStringArray = map.call("path", "emberhold", "rattlebone_warren")
	check(into == PackedStringArray(["emberhold", "emberglade", "rattlebone_warren"]), "and into the Rattlebone Warren")
	map.call("set_current", "emberhold", 3)
	map.call("select", "saltmarsh_fens")
	var text: String = str(map.call("details", "saltmarsh_fens"))
	check(text.contains("The way there: Emberhold > The Emberglade > The Saltmarsh Fens"), "the details give the way there: %s" % text)
	check(text.contains("Dungeons: Mirefang Hollow"), "and the zone's dungeons")
	check(str(map.call("details", "emberhold")).contains("You are here."), "where you are")
	# Drawn in a window: clicking a box picks its zone, a diamond its dungeon.
	map.size = Vector2(700, 420)
	get_root().add_child(map)
	map.queue_redraw()
	await process_frame
	await process_frame
	var boxes: Dictionary = map.get("_boxes")
	check(boxes.has("duskmere_shore") and boxes.has("sunken_bastion"), "zones and dungeons are drawn")
	var picked: Array = []
	map.connect("picked", func(id: String) -> void: picked.append(id))
	for id: String in ["duskmere_shore", "sunken_bastion"]:
		var ev: InputEventMouseButton = InputEventMouseButton.new()
		ev.button_index = MOUSE_BUTTON_LEFT
		ev.pressed = true
		ev.position = (boxes[id] as Rect2).get_center()
		map.call("_gui_input", ev)
	check(picked == ["duskmere_shore", "sunken_bastion"], "a click picks the zone, or the dungeon inside it: %s" % str(picked))
	map.queue_free()


# ------------------------------------------------------------------------------ Phase 7: chat and social windows

func _test_craft_ui() -> void:
	section("craft ui")
	# The bundled recipes and the server's rules, mirrored for display.
	var smithing: Array = CraftUi.recipes_for("smithing")
	check(smithing.size() > 100, "the forge's recipes are bundled")
	check(int((smithing[0] as Dictionary)["trivial"]) <= int((smithing[smithing.size() - 1] as Dictionary)["trivial"]), "easiest first")
	check(str(((CraftUi.recipe("copper_bar")["result"]) as Dictionary)["item"]) == "copper_bar", "a recipe by id")
	check(str(CraftUi.tradeskill_def("tailoring").get("station", "")) == "loom", "a tradeskill's station")
	check(CraftUi.difficulty(30, 20) == CraftUi.TRIVIAL and CraftUi.difficulty(10, 20) == CraftUi.EASY, "grey at the trivial, green close below")
	check(CraftUi.difficulty(0, 40) == CraftUi.FAIR and CraftUi.difficulty(0, 90) == CraftUi.HARD, "yellow, then red, further off")
	check(near(CraftUi.success_chance(20, 20), 0.95) and near(CraftUi.success_chance(10, 20), 0.83) and near(CraftUi.success_chance(0, 290), 0.05),
		"the success chance matches the server's")
	var counts: Dictionary = CraftUi.pack_counts([{"slot": 0x0100, "item_id": "copper_ore", "count": 3}, {"slot": 0x1000, "item_id": "copper_ore", "count": 2},
		{"slot": 0x0200, "item_id": "copper_ore", "count": 50}, {"slot": 0x000F, "item_id": "training_sword", "count": 1}])
	check(int(counts.get("copper_ore", 0)) == 5 and not counts.has("training_sword"), "the pack counts its bags, never the bank or worn gear")

	var ui: Control = CraftUi.new()
	var sent: Array = []
	ui.connect("combine", func(npc: int, rid: String, req: int) -> void: sent.append([npc, rid, req]))
	root.add_child(ui)
	await process_frame
	ui.call("set_craft_skills", {"announce": false, "skills": [{"id": "smithing", "name": "Smithing", "value": 10, "cap": 300},
		{"id": "tailoring", "name": "Tailoring", "value": 0, "cap": 300}]})
	ui.call("set_inventory", {"items": [{"slot": 0x0100, "item_id": "copper_ore", "count": 3}, {"slot": 0x0101, "item_id": "forge_coal", "count": 1},
		{"slot": 0x0200, "item_id": "copper_bar", "count": 40}]})
	check(not ui.call("any_window_open"), "closed until a station opens it")
	ui.call("show_station", {"npc_id": 5, "name": "Forge", "tradeskill": "smithing"})
	await process_frame
	check(ui.call("any_window_open"), "a station opens the window")
	check(str((ui.get("_title") as Label).text) == "Forge - Smithing", "titled by station and tradeskill")
	check(str((ui.get("_skill_label") as Label).text) == "Your Smithing: 10 / 300", "with your skill")
	check((ui.call("visible_recipes") as Array).size() == smithing.size(), "every smithing recipe is listed")
	ui.set("only_makeable", true)
	var makeable: Array = ui.call("visible_recipes")
	check(makeable.size() == 1 and str((makeable[0] as Dictionary)["id"]) == "copper_bar", "only the bar can be made from ore and coal (the bank's bars do not count)")
	ui.set("only_makeable", false)
	ui.set("search_text", "coif")
	var coifs: Array = ui.call("visible_recipes")
	check(coifs.size() == 8 and str(ItemDb.name_of(str(((coifs[0] as Dictionary)["result"] as Dictionary)["item"]))).contains("Coif"), "searching by name")
	ui.set("search_text", "")
	ui.call("set_craft_skills", {"announce": true, "skills": [{"id": "smithing", "name": "Smithing", "value": 25, "cap": 300}]})
	ui.set("hide_trivial", true)
	check(not (ui.call("visible_recipes") as Array).any(func(r: Dictionary) -> bool: return str(r["id"]) == "copper_bar"), "trivial recipes can be hidden")
	check(int((ui.get("skills") as Dictionary)["tailoring"]["value"]) == 0, "a skill-up leaves the other tradeskills alone")
	ui.set("hide_trivial", false)

	ui.call("select", "copper_bar")
	var text: String = str(ui.call("describe", "copper_bar"))
	check(text.contains("1 x Copper Ore (you have 3)") and text.contains("Trivial at 10"), "the recipe's needs against the pack: %s" % text)
	check(not (ui.get("_combine_btn") as Button).disabled, "Combine is ready")
	ui.call("start_combine", 2)
	check(sent.size() == 1 and int((sent[0] as Array)[0]) == 5 and str((sent[0] as Array)[1]) == "copper_bar", "Combine sends the station and recipe")
	ui.call("start_combine", 1)
	check(sent.size() == 1, "one combine at a time")
	ui.call("on_result", {"recipe_id": "copper_bar", "success": true, "made": 1, "request": int((sent[0] as Array)[2])})
	check(str((ui.get("_status") as Label).text) == "Made Copper Bar.", "the result is reported")
	ui.call("_process", 0.0)
	check(sent.size() == 1 and (ui.get("_combine_btn") as Button).disabled, "the next waits out the server's pause, and so does Combine")
	ui.set("_ready_at_ms", 0)  # the pause between combines has passed
	ui.call("_process", 0.0)
	check(sent.size() == 2, "a run of combines sends the next one")
	check(int((sent[1] as Array)[2]) != int((sent[0] as Array)[2]), "each combine has its own number")
	ui.call("on_result", {"recipe_id": "copper_bar", "success": false, "made": 0, "request": int((sent[1] as Array)[2])})
	check(str((ui.get("_status") as Label).text) == "Failed to make Copper Bar.", "and a failure too")
	check(int(ui.get("repeat_left")) == 0, "the run is over")
	# A combine asked for during the pause (by hand, not a run) is held until the pause ends, not sent early.
	ui.call("start_combine", 1)
	check(sent.size() == 2 and int(ui.get("repeat_left")) == 1, "a manual combine waits out the pause too")
	ui.set("_ready_at_ms", 0)
	ui.call("_process", 0.0)
	check(sent.size() == 3 and (ui.get("_combine_btn") as Button).disabled, "and goes once it ends")
	ui.call("on_result", {"recipe_id": "copper_bar", "success": true, "made": 1, "request": int((sent[2] as Array)[2])})
	ui.set("_ready_at_ms", 0)
	ui.call("_process", 0.0)
	check(not (ui.get("_combine_btn") as Button).disabled, "Combine is ready again after the pause")
	# A late answer to an older combine (the window closed and reopened meanwhile) changes nothing, even for
	# the same recipe at the same station.
	ui.call("start_combine", 2)
	var before: int = sent.size()
	var mine: int = int((sent[before - 1] as Array)[2])
	ui.call("on_result", {"recipe_id": "copper_bar", "success": true, "made": 1, "request": int((sent[1] as Array)[2])})
	check(bool(ui.get("_waiting")) and int(ui.get("repeat_left")) == 1, "a stale result is ignored")
	ui.call("on_result", {"recipe_id": "copper_bar", "success": true, "made": 1, "request": mine})
	check(not bool(ui.get("_waiting")), "its own result is not")
	ui.call("select", "chain_t1_head")
	check(int(ui.get("repeat_left")) == 0 and sent.size() == before, "choosing another recipe ends the run")
	# Closing the window with a combine in flight keeps the server's pause: reopening at once sends nothing early.
	ui.set("_ready_at_ms", 0)
	ui.call("select", "copper_bar")
	ui.call("start_combine", 1)
	var flying: int = sent.size()
	check(flying == before + 1, "a combine goes out")
	ui.call("show_station", {"npc_id": 0, "name": "", "tradeskill": ""})
	ui.call("show_station", {"npc_id": 5, "name": "Forge", "tradeskill": "smithing"})
	ui.call("select", "copper_bar")
	ui.call("start_combine", 1)
	ui.call("_process", 0.0)
	check(sent.size() == flying and (ui.get("_combine_btn") as Button).disabled, "a combine in flight when the window closed still holds the next one back")
	# Another station opens while it is still in flight, and the local pause runs out before its result comes.
	ui.call("show_station", {"npc_id": 6, "name": "Loom", "tradeskill": "tailoring"})
	ui.call("show_station", {"npc_id": 5, "name": "Forge", "tradeskill": "smithing"})
	ui.call("select", "copper_bar")
	ui.set("_ready_at_ms", 0)
	ui.call("start_combine", 1)
	ui.call("_process", 0.0)
	check(sent.size() == flying and bool(ui.get("_waiting")), "a slow result holds the next combine back past the pause and a change of station")
	(ui.get("_status") as Label).text = ""
	ui.call("show_station", {"npc_id": 6, "name": "Loom", "tradeskill": "tailoring"})
	ui.call("on_result", {"recipe_id": "copper_bar", "success": true, "made": 1, "request": int((sent[flying - 1] as Array)[2])})
	check(not bool(ui.get("_waiting")) and int(ui.get("_ready_at_ms")) > Time.get_ticks_msec(), "and its result, however late, ends the wait and starts the pause")
	check(str((ui.get("_status") as Label).text) == "", "but the Forge's result is not shown as the Loom's")
	ui.call("show_station", {"npc_id": 5, "name": "Forge", "tradeskill": "smithing"})
	ui.call("select", "copper_bar")
	# A refusal is answered: it ends the wait at once, starts no pause and ends a run.
	ui.set("_ready_at_ms", 0)
	ui.call("start_combine", 3)
	check(sent.size() == flying + 1, "the next combine goes once the result is in")
	ui.call("on_result", {"recipe_id": "copper_bar", "success": false, "made": 0, "request": int((sent[flying] as Array)[2]), "refused": true})
	check(not bool(ui.get("_waiting")) and int(ui.get("_ready_at_ms")) <= Time.get_ticks_msec() and int(ui.get("repeat_left")) == 0,
		"a refused combine frees the window at once, with no pause, and ends the run")
	check(str((ui.get("_status") as Label).text) == "Not combined (see chat).", "and says so")
	# A combine whose request is merely slow keeps holding the window, however long: only its answer frees it.
	ui.call("start_combine", 1)
	check(sent.size() == flying + 2, "a combine right after a refusal")
	ui.set("clock_offset_ms", 120000)  # two minutes on, far past the old 10 s timeout
	ui.call("_process", 0.0)
	ui.call("start_combine", 1)
	ui.call("_process", 0.0)
	check(bool(ui.get("_waiting")) and sent.size() == flying + 2 and (ui.get("_combine_btn") as Button).disabled,
		"a request still undelivered after two minutes holds the next one back")
	ui.call("on_result", {"recipe_id": "copper_bar", "success": true, "made": 1, "request": int((sent[flying + 1] as Array)[2])})
	check(not bool(ui.get("_waiting")) and int(ui.get("_ready_at_ms")) > int(ui.call("_now")), "until its answer comes, which starts the pause")
	ui.set("clock_offset_ms", 0)
	ui.call("close_windows")
	ui.set("_ready_at_ms", 0)
	ui.call("show_station", {"npc_id": 5, "name": "Forge", "tradeskill": "smithing"})
	var coif: String = str((coifs[0] as Dictionary)["id"])
	ui.call("select", coif)
	check(str(ui.call("describe", coif)).contains("Smithing Hammer  - not in your pack") and (ui.get("_combine_btn") as Button).disabled,
		"a missing tool is named and Combine waits")
	ui.call("show_station", {"npc_id": 0, "name": "", "tradeskill": ""})
	check(not ui.call("any_window_open"), "walking away closes it")
	ui.queue_free()
	# The skills window lists the tradeskills under the combat skills.
	var cu: ClassUi = ClassUi.new()
	root.add_child(cu)
	await process_frame
	cu.set_craft_skills({"announce": false, "skills": [{"id": "smithing", "name": "Smithing", "value": 12, "cap": 300}]})
	cu.set_craft_skills({"announce": true, "skills": [{"id": "smithing", "name": "Smithing", "value": 13, "cap": 300}]})
	cu.toggle_skills()
	await process_frame
	var found: bool = false
	for n: Node in (cu.get("_skills_list") as Node).find_children("*", "Label", true, false):
		if (n as Label).text == "13 / 300":
			found = true
	check(found, "a tradeskill's value in the skills window")
	cu.queue_free()


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
	# Phase 14: the raid channel, its prefix and its tab.
	chat.call("say", "/rs pull on three")
	sent = chat.get("sent")
	check(int(sent.back()["channel"]) == 11 and str(sent.back()["text"]) == "pull on three", "/rs speaks to the raid")
	chat.call("add_message", 11, "Hilde", "ready")
	chat.call("add_message", 11, "", "Garrick's group has joined the raid.")
	check(str(chat.call("tab_text", "Raid")).contains("Hilde tells the raid, 'ready'"), "raid tab: " + str(chat.call("tab_text", "Raid")))
	check(str(chat.call("tab_text", "Raid")).contains("has joined the raid"), "raid notices go to the raid tab")
	check(not str(chat.call("tab_text", "Group")).contains("Hilde"), "the group tab leaves raid lines out")
	chat.call("show_tab_named", "Raid")
	chat.call("say", "go")
	sent = chat.get("sent")
	check(int(sent.back()["channel"]) == 11, "the raid tab speaks to the raid")
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
	# Phase 12: a member who left the world keeps a greyed row without bars.
	var with_offline: Array = members.duplicate(true)
	(with_offline[2] as Dictionary)["online"] = false
	(with_offline[2] as Dictionary)["entity_id"] = 0
	ui.call("set_group", {"group_id": 3, "leader": "Aldric", "loot_rule": 0, "members": with_offline})
	await process_frame
	var offline_row: Control = (ui.get("_group_rows") as Control).get_child(2) as Control
	var head: Label = offline_row.get_child(0).get_child(0) as Label
	check(head.text.contains("(offline)"), "an offline member is marked: %s" % head.text)
	check(offline_row.get_child(0).get_child_count() == 1, "and has no bars")
	check(int(ui.call("member_entity", 1)) == 0, "F3 cannot target someone offline")
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
	# Phase 14: the raid window. Aldric leads group 1 and the raid; three groups, one member offline.
	ui.call("set_group", {"group_id": 3, "leader": "Aldric", "loot_rule": 0, "members": members})
	var raiders: Array = []
	var raid_names: Array = ["Aldric", "Brenna", "Garrick", "Hilde", "Ivo", "Juna", "Kestrel"]
	var raid_groups: Array = [1, 1, 2, 2, 3, 3, 3]
	for i: int in raid_names.size():
		raiders.append({"group": raid_groups[i], "name": raid_names[i], "class_id": "cleric", "level": 55, "entity_id": 200 + i,
			"hp_pct": 90, "group_leader": i in [0, 2, 4], "dead": i == 5, "online": i != 6})
	check(not bool(ui.call("raid_visible")), "the raid window waits for a raid")
	ui.call("set_raid", {"raid_id": 4, "leader": "Aldric", "loot_rule": 0, "max_groups": 12, "members": raiders})
	await process_frame
	check(bool(ui.call("raid_visible")), "joining a raid opens the raid window")
	check((ui.get("_raid_grid") as Control).get_child_count() == 3, "one box per group")
	check(str((ui.get("_raid_title") as Label).text).contains("3 groups, 7 members"), "the title counts them: %s" % (ui.get("_raid_title") as Label).text)
	var rb: Dictionary = ui.get("_raid_buttons")
	check(not (rb["invite"] as Button).disabled and not (rb["disband"] as Button).disabled, "the raid leader invites and disbands")
	check((rb["kick"] as Button).disabled, "removing a group needs someone chosen")
	ui.call("select_raider", "Hilde")
	check(not (rb["kick"] as Button).disabled, "...then it is ready")
	(rb["kick"] as Button).emit_signal("pressed")
	sent = ui.get("sent")
	check(str(sent.back()) == "raidkick Hilde", "remove names the chosen raider: %s" % str(sent.back()))
	(rb["leader"] as Button).emit_signal("pressed")
	sent = ui.get("sent")
	check(str(sent.back()) == "raidleader Hilde", "make leader too")
	(ui.get("_raid_loot") as OptionButton).item_selected.emit(1)
	sent = ui.get("sent")
	check(str(sent.back()) == "raidloot ng", "the loot rule")
	var box3: Control = (ui.get("_raid_grid") as Control).get_child(2) as Control
	var offline_label: Label = box3.get_child(0).get_child(3).get_child(0).get_child(0) as Label
	check(offline_label.text.contains("offline"), "an offline raider is marked: %s" % offline_label.text)
	# Another member's view: no leader tools, and only a group leader takes the group out.
	ui.call("set_raid", {"raid_id": 4, "leader": "Garrick", "loot_rule": 1, "max_groups": 12, "members": raiders})
	check((rb["invite"] as Button).disabled and (rb["disband"] as Button).disabled, "a raider who does not lead cannot invite or disband")
	check(not (rb["leave"] as Button).disabled, "Aldric still leads his group, so he can take it out")
	check((ui.get("_raid_loot") as OptionButton).selected == 1, "the loot rule shows")
	# Phase 15: the main assist and marks. Aldric is the main assist here: he marks, but cannot name one.
	ui.call("set_raid", {"raid_id": 4, "leader": "Garrick", "loot_rule": 1, "max_groups": 12, "members": raiders,
		"main_assist": "Aldric", "marks": [{"slot": 1, "entity_id": 900}]})
	check(str((ui.get("_raid_title") as Label).text).contains("main assist Aldric"), "the title names the main assist")
	check((rb["assist"] as Button).disabled, "only the raid leader names the main assist")
	check(not (rb["mark"] as Button).disabled and not (rb["unmark"] as Button).disabled, "the main assist marks targets")
	check((rb["assist_ma"] as Button).disabled, "the main assist has nobody to assist")
	(rb["mark"] as Button).emit_signal("pressed")
	sent = ui.get("sent")
	check(str(sent.back()) == "mark 2", "a new mark takes the next free number: %s" % str(sent.back()))
	ui.call("set_raid", {"raid_id": 4, "leader": "Aldric", "loot_rule": 1, "max_groups": 12, "members": raiders,
		"main_assist": "Hilde", "marks": []})
	ui.call("select_raider", "Hilde")
	check(not (rb["assist"] as Button).disabled and (rb["unmark"] as Button).disabled, "the leader names one; no marks to clear")
	(rb["assist"] as Button).emit_signal("pressed")
	sent = ui.get("sent")
	check(str(sent.back()) == "raidassist Hilde", "main assist names the chosen raider")
	(rb["assist_ma"] as Button).emit_signal("pressed")
	sent = ui.get("sent")
	check(str(sent.back()) == "assist", "Assist follows the main assist's target")
	ui.call("set_raid", {"raid_id": 4, "leader": "Garrick", "loot_rule": 1, "max_groups": 12, "members": raiders})
	ui.call("show_invite", {"kind": 2, "from_name": "Garrick", "guild_name": "", "expires_ms": 60000})
	check(str((ui.get("_invite_text") as Label).text).contains("invites your group to join a raid"), "a raid invitation pops up")
	ui.call("answer_invite", true)
	sent = ui.get("sent")
	check(str(sent.back()) == "raidaccept", "accepting a raid invitation")
	await process_frame
	check(bool(ui.call("windows_on_screen")), "a raid of three groups fits on screen")
	var twelve: Array = []
	for g: int in 12:
		for k: int in 6:
			twelve.append({"group": g + 1, "name": "R%d_%d" % [g, k], "class_id": "warrior", "level": 60, "entity_id": 0,
				"hp_pct": 100, "group_leader": k == 0, "dead": false, "online": true})
	ui.call("set_raid", {"raid_id": 4, "leader": "R0_0", "loot_rule": 0, "max_groups": 12, "members": twelve})
	await process_frame
	check((ui.get("_raid_grid") as Control).get_child_count() == 12, "a full raid: twelve groups")
	check(bool(ui.call("windows_on_screen")), "a full raid of 72 still fits on screen (the groups scroll)")
	var raid_rect: Rect2 = (ui.get("_raid_win") as Control).get_global_rect()
	check(raid_rect.end.y <= 460.0, "the raid window stays above the chat window (bottom at %.0f)" % raid_rect.end.y)
	ui.call("set_raid", {"raid_id": 0, "leader": "", "loot_rule": 0, "max_groups": 12, "members": []})
	check((ui.get("_raid_grid") as Control).get_child_count() == 1, "out of the raid: a hint how to form one")
	await process_frame
	check(bool(ui.call("windows_on_screen")), "the social windows lie inside a 1280x720 screen")
	ui.call("close_windows")
	check(not bool(ui.call("any_window_open")), "Escape closes them")
	screen.queue_free()
	await _test_master_loot()


## Phase 14: the master looter's loot window has a Give menu of who was at the kill (not themselves).
func _test_master_loot() -> void:
	section("master loot")
	var ItemUiScript: GDScript = load("res://scripts/ui/item_ui.gd")
	var ui: Control = ItemUiScript.new()
	get_root().add_child(ui)
	await process_frame
	ui.set("my_name", "Aldric")
	var given: Array = []
	ui.connect("loot_give", func(i: int, to: String) -> void: given.append([i, to]))
	var w: Dictionary = {"corpse_id": 9, "name": "the Obsidian Tyrant's corpse", "coin": 0, "master": true,
		"party": ["Aldric", "Hilde", "Brenna"],
		"items": [{"index": 0, "item_id": "plate_t1_chest", "count": 1, "owner": "", "mine": true},
			{"index": 1, "item_id": "rat_whisker", "count": 2, "owner": "", "mine": true}]}
	ui.call("show_loot", w)
	await process_frame
	var menus: Array = (ui.get("_loot_list") as Node).find_children("*", "MenuButton", true, false)
	check(menus.size() == 2, "a Give menu on each item")
	var pop: PopupMenu = (menus[0] as MenuButton).get_popup()
	check(pop.item_count == 2 and pop.get_item_text(0) == "Brenna" and pop.get_item_text(1) == "Hilde", "who was there, sorted, without you")
	pop.index_pressed.emit(1)
	check(given.size() == 1 and int(given[0][0]) == 0 and str(given[0][1]) == "Hilde", "giving names the item and the raider: %s" % str(given))
	w["master"] = false
	ui.call("show_loot", w)
	await process_frame
	check((ui.get("_loot_list") as Node).find_children("*", "MenuButton", true, false).is_empty(), "no Give menu for anyone else")
	ui.queue_free()


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
	# Phase 14: Y became the raid window's default; a player who had already put the guild window on Y keeps it.
	var older: Dictionary = KeybindsLib.from_config({"guild_window": "%d" % KEY_Y})
	check(Array(older["guild_window"]) == [KEY_Y], "a saved key keeps its action over a new default")
	check(not Array(older["raid_window"]).has(KEY_Y), "...and the new action does without it")
	check(Array(KeybindsLib.defaults()["raid_window"]) == [KEY_Y], "Y opens the raid window by default")
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


# ------------------------------------------------------------------------------ Phase 10: sealed packets

func _kat_key() -> PackedByteArray:
	var k: PackedByteArray = PackedByteArray()
	for i: int in 32:
		k.append(i)
	return k


## The same vectors as server/tests/test_packet_crypto.cpp, made with the openssl command line.
func _test_packet_crypto() -> void:
	section("packet crypto")
	var key: PackedByteArray = _kat_key()
	check(PacketCrypto.label(true, 0) == "emberfall/1 c2s 0" and PacketCrypto.label(false, 1) == "emberfall/1 s2c 1", "labels")
	check(PacketCrypto.derive_key(key, "emberfall/1 c2s 0 enc").hex_encode() == "0a229cdfd35745c76c8d05bf49a1ce7d13aa36c8316896b0490ff3a7a7168faf", "enc key vector")
	check(PacketCrypto.derive_key(key, "emberfall/1 c2s 0 mac").hex_encode() == "f638384a71d33f2a118fa4c53f24cf652f172fe90236095d1575b59bb52cf872", "mac key vector")
	var client: PacketCrypto = PacketCrypto.new(key, true)
	for i: int in 5:
		client.seal(0, "x".to_utf8_buffer())
	var text: String = "Realms of Emberfall: a sealed packet, three AES blocks."
	var kat: String = "05000000000000000633fef70c7f895ee140f068cf133c6d74845e0362215c85f7bfa3201360183de2a6804d0e84e8d724a819aef9fdcb8645e4dc62788fc688ccbed464cfabad93c310c0a0a34544"
	check(client.seal(0, text.to_utf8_buffer()).hex_encode() == kat, "sealed packet vector")
	var server: PacketCrypto = PacketCrypto.new(key, false)
	check(server.seal(1, PackedByteArray()).hex_encode() == "000000000000000038c8bf9e16be6291a6d68fc442997504", "empty packet vector")
	# The vector opens on a receiver that has had counters 0..4, and not on a fresh one (the reliable
	# channel takes only the next counter).
	var fresh: PacketCrypto = PacketCrypto.new(key, false)
	check(int(fresh.open(0, kat.hex_decode())[0]) == PacketCrypto.Open.REPLAY, "reliable: counter 5 is not the next one")
	var sender: PacketCrypto = PacketCrypto.new(key, true)
	for i: int in 5:
		fresh.open(0, sender.seal(0, "x".to_utf8_buffer()))
	var r: Array = fresh.open(0, kat.hex_decode())
	check(int(r[0]) == PacketCrypto.Open.OK and (r[1] as PackedByteArray).get_string_from_utf8() == text, "the vector opens")

	# Round trips both ways on both channels, every length up to a few blocks (the XOR tail paths).
	key = Crypto.new().generate_random_bytes(32)
	client = PacketCrypto.new(key, true)
	server = PacketCrypto.new(key, false)
	var all_ok: bool = true
	for ch: int in 2:
		for n: int in 70:
			var msg: PackedByteArray = PackedByteArray()
			for i: int in n:
				msg.append((i * 37 + n) & 0xff)
			var sealed: PackedByteArray = client.seal(ch, msg)
			var back: Array = server.open(ch, sealed)
			all_ok = all_ok and sealed.size() == n + PacketCrypto.OVERHEAD and int(back[0]) == PacketCrypto.Open.OK and back[1] == msg
			back = client.open(ch, server.seal(ch, msg))
			all_ok = all_ok and int(back[0]) == PacketCrypto.Open.OK and back[1] == msg
	check(all_ok, "round trips, 0..69 bytes, both ways, both channels")

	var good: PackedByteArray = client.seal(0, "hello world".to_utf8_buffer())
	var tamper_caught: bool = true
	for i: int in good.size():
		var bad: PackedByteArray = good.duplicate()
		bad[i] = bad[i] ^ 1
		tamper_caught = tamper_caught and int(server.open(0, bad)[0]) == PacketCrypto.Open.BAD_TAG
	check(tamper_caught, "any flipped bit is refused")
	check(int(server.open(0, good.slice(0, 23))[0]) == PacketCrypto.Open.MALFORMED, "too short")
	check(int(server.open(1, good)[0]) == PacketCrypto.Open.BAD_TAG, "another channel's keys")
	check(int(client.open(0, good)[0]) == PacketCrypto.Open.BAD_TAG, "reflected at its sender")
	check(int(server.open(0, good)[0]) == PacketCrypto.Open.OK, "the real packet opens")
	check(int(server.open(0, good)[0]) == PacketCrypto.Open.REPLAY, "reliable: each counter once")
	var u1: PackedByteArray = client.seal(1, "u1".to_utf8_buffer())
	var u2: PackedByteArray = client.seal(1, "u2".to_utf8_buffer())
	check(int(server.open(1, u2)[0]) == PacketCrypto.Open.OK, "unreliable: a lost packet is fine")
	check(int(server.open(1, u1)[0]) == PacketCrypto.Open.REPLAY, "unreliable: nothing older than the newest")


## NetClient's receive rule: unsealed only for a refused login before the first sealed packet.
func _test_net_client_sealing() -> void:
	section("net client sealing")
	var key: PackedByteArray = Crypto.new().generate_random_bytes(32)
	var net: Node = NetClient.new()
	net.enable_encryption(key)
	var server: PacketCrypto = PacketCrypto.new(key, false)
	var refused: PackedByteArray = ProtoLib.encode_auth_result({"ok": false, "message": "expired"})
	check(net._unseal(refused, 0) == refused, "a refused AuthResult may come unsealed")
	var status: PackedByteArray = ProtoLib.encode_server_status({"motd": "hi", "players_online": 1, "max_players": 10, "server_version": "x"})
	check(net._unseal(status, 0) == status, "so may a ServerStatus that ENet resent late")
	var fake_ok: PackedByteArray = ProtoLib.encode_auth_result({"ok": true, "message": "welcome"})
	check(net._unseal(fake_ok, 1).is_empty(), "an unsealed success is refused")
	check(int(net.rejected_in) == 1, "and counted")
	check(net._unseal(server.seal(0, fake_ok), 0) == fake_ok, "a sealed AuthResult opens")
	check(net.is_encrypted(), "encrypted from the first sealed packet")
	var kick: PackedByteArray = ProtoLib.encode_kick({"reason": "spoofed"})
	check(net._unseal(kick, 1).is_empty(), "after that, nothing unsealed")
	check(net._unseal(status, 1).is_empty(), "not even a ServerStatus")
	net.free()
	# On the reliable channel a refused packet can only be tampering: the connection closes.
	var net2: Node = NetClient.new()
	var reasons: Array[String] = []
	net2.disconnected.connect(func(r: String) -> void: reasons.append(r))
	net2.enable_encryption(key)
	check(net2._unseal(fake_ok, 0).is_empty(), "an unsealed success on the reliable channel is refused")
	check(net2._crypto == null, "the connection is closed (no cipher left)")
	check(reasons.size() == 1, "and the player is told")
	# Failed unreliable packets are dropped until there are too many, as on the server.
	var net3: Node = NetClient.new()
	var reasons3: Array[String] = []
	net3.disconnected.connect(func(r: String) -> void: reasons3.append(r))
	net3.enable_encryption(key)
	var junk: PackedByteArray = PackedByteArray()
	junk.resize(40)
	for i: int in NetClient.MAX_UNRELIABLE_FAILURES - 1:
		net3._unseal(junk, 1)
	check(net3._crypto != null and reasons3.is_empty(), "a few bad unreliable packets are only dropped")
	net3._unseal(junk, 1)
	check(net3._crypto == null and reasons3.size() == 1, "too many close the connection")
	# A sealed reliable packet that could not be sent used up its counter: the connection closes.
	var net4: Node = NetClient.new()
	var reasons4: Array[String] = []
	net4.disconnected.connect(func(r: String) -> void: reasons4.append(r))
	net4.enable_encryption(key)
	net4._on_send_failed(1)
	check(net4._crypto != null and reasons4.is_empty(), "a lost unreliable send is fine")
	net4._on_send_failed(0)
	check(net4._crypto == null and reasons4.size() == 1, "a lost reliable send closes the connection")
	net2.free()
	net3.free()
	net4.free()
