## In-world scene: environment, entities (players, mobs, corpses, trainers), camera, combat and
## spell HUD, chat with the combat log, targeting, spell effects, and the in-game menu. Everything
## combat-related is an intent sent to the server; this script only presents what the server decides.
extends Node3D

const Proto := preload("res://scripts/net/protocol.gd")
const LocalPlayer := preload("res://scripts/world/local_player.gd")
const RemoteEntity := preload("res://scripts/world/remote_entity.gd")
const CameraRig := preload("res://scripts/world/camera_rig.gd")
const ChatWindow := preload("res://scripts/ui/chat_window.gd")
const Hud := preload("res://scripts/ui/hud.gd")
const UI := preload("res://scripts/ui/ui.gd")
const GemSlot := preload("res://scripts/ui/gem_slot.gd")
const ItemDb := preload("res://scripts/ui/item_db.gd")

## Remote entities are drawn this far behind the newest snapshot so there are always two to blend.
const INTERP_DELAY: float = 0.1
const TAB_RANGE: float = 60.0
const PICK_RADIUS_PX: float = 48.0
const FLAG_DEAD: int = 1
const FLAG_IN_COMBAT: int = 2
const FLAG_STUNNED: int = 8
const FLAG_SITTING: int = 4
const FLAG_FLEEING: int = 16
const FLAG_MEZZED: int = 64
const FLAG_ROOTED: int = 128
const GEM_PREFIX: String = "gem:"

# CombatEvent kinds (server/src/game/zone.hpp: CombatKind)
enum Ev {
	HIT, MISS, DODGE, PARRY, RIPOSTE, CRIT, ABILITY_HIT, ABILITY_MISS, TAUNT, TAUNT_FAIL, STUN, STUN_FAIL, HEAL, BUFF, BUFF_FADE, DEATH, EVADE,
	SPELL_HIT, RESIST, DOT_TICK, HOT_TICK, MEZ, MEZ_BREAK, ROOT, ROOT_BREAK, SNARE, SLOW, FEAR, DEBUFF, DISPEL, RESURRECT, TELEPORT,
	MANA_RESTORE, IMMUNE,
	DAMAGE_SHIELD, LIFETAP, DRAIN_TICK, CHARM, CHARM_BREAK, FEIGN, FEIGN_FAIL, HIDE, HIDE_FAIL, SUMMON, THREAT_DROP,
	ILLUSION, SONG_PULSE, PET_DEATH, BACKSTAB,
}
## PetCommand orders (server PetOrder) by /pet keyword.
const PET_ORDERS: Dictionary = {"attack": 0, "follow": 1, "guard": 2, "back": 3, "backoff": 3, "dismiss": 4}
const RESIST_NAMES: Array[String] = ["", "magic", "fire", "cold", "poison", "disease"]

var info: Dictionary = {}
var local_player: LocalPlayer
var remotes: Dictionary = {}  # entity_id -> RemoteEntity
var chat: ChatWindow
var hud: Hud
var snapshots_received: int = 0
var corrections_received: int = 0
var seen_names: Dictionary = {}  # player name -> true, everyone ever spawned
var my_id: int = 0
var my_level: int = 1
var stats: Dictionary = {}
var target_id: int = 0
var target_con: int = 3
var abilities: Array = []
# Counters the autotest reports.
var kills: int = 0
var deaths: int = 0
var corpses_looted: int = 0
var level_ups: int = 0
var combat_events: int = 0
var abilities_used: int = 0
var last_death: Dictionary = {}
var death_position: Vector3 = Vector3.ZERO
var last_loot_closed: String = ""
var spells_landed: int = 0
var resists: int = 0
var heals_received: int = 0
var last_trainer: Dictionary = {}
var pet_status: Dictionary = {}
var song: Dictionary = {}
var pet_commands_sent: int = 0
var songs_started: int = 0
# Phase 5: items. The autotest reads these.
var sheet: Dictionary = {}
var last_vendor: Dictionary = {}
var last_auction_results: Dictionary = {}
var last_trade_closed: Dictionary = {}
var bank_open: bool = false
var auction_npc: int = 0

var _tick_rate: int = 20
var _server_time: float = 0.0
var _has_time: bool = false
var _last_usec: int = 0
var _hud_label: Label
var _menu: PanelContainer
var _camera_rig: CameraRig
var _tab_index: int = -1
var _last_cast_end: Dictionary = {}  # entity id -> msec of its last CastEnd
var _recent_deaths: Array[Dictionary] = []


func _ready() -> void:
	info = Session.world_info
	my_id = int(info["entity_id"])
	_tick_rate = maxi(1, int(info["tick_rate"]))
	_server_time = float(info["server_tick"]) / float(_tick_rate)
	_build_environment()
	_build_ground()

	var me: Dictionary = _my_character()
	my_level = int(me.get("level", 1))
	local_player = LocalPlayer.new()
	local_player.name = "LocalPlayer"
	local_player.setup(info, str(me.get("name", Session.username)), str(me.get("class_id", "")), str(me.get("race", "")),
		int(me.get("gender", 0)))
	add_child(local_player)

	_camera_rig = CameraRig.new()
	_camera_rig.target = local_player
	add_child(_camera_rig)

	_build_hud()
	chat.my_name = local_player.display_name
	hud.my_name = local_player.display_name
	Session.world_message.connect(_on_world_message)


func _my_character() -> Dictionary:
	# EnterWorldResult does not repeat the name/class, so look it up from the character list.
	for c: Dictionary in Session.characters:
		if int(c["id"]) == Session.entering_char_id:
			return c
	return Session.characters[0] if Session.characters.size() > 0 else {}


func _process(delta: float) -> void:
	var now: int = Time.get_ticks_usec()
	if _last_usec != 0:
		_server_time += float(now - _last_usec) / 1000000.0
	_last_usec = now
	var render_time: float = _server_time - INTERP_DELAY
	for r: RemoteEntity in remotes.values():
		r.update_render(render_time)
		r.update_visual(delta)
	var flags: int = int(stats.get("flags", 0))
	local_player.set_state_flags((flags & FLAG_DEAD) != 0, (flags & FLAG_SITTING) != 0,
		(flags & (FLAG_STUNNED | FLAG_MEZZED)) != 0, (flags & FLAG_IN_COMBAT) != 0)
	local_player.frozen = (flags & (FLAG_DEAD | FLAG_STUNNED | FLAG_MEZZED | FLAG_ROOTED | FLAG_FLEEING)) != 0
	local_player.set_flags2(int(stats.get("flags2", 0)))
	local_player.input_enabled = not chat.is_typing() and not _menu.visible
	if target_id != 0 and remotes.has(target_id):
		hud.set_target_hp((remotes[target_id] as RemoteEntity).hp_pct)
	_hud_label.text = "%s   pos %.1f, %.1f   nearby: %d   ping: %d ms   fps: %d" % [
		str(info["zone_name"]), local_player.position.x, local_player.position.z, remotes.size(),
		Net.round_trip_ms(), Engine.get_frames_per_second()]


# ------------------------------------------------------------------------------ input

func _unhandled_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb != null and mb.pressed and mb.button_index == MOUSE_BUTTON_LEFT and not _menu.visible:
		var picked: int = _pick_at(mb.position)
		if picked != 0:
			set_target(picked)
			get_viewport().set_input_as_handled()
		return
	var k: InputEventKey = event as InputEventKey
	if k == null or not k.pressed or k.echo or chat.is_typing():
		return
	var handled: bool = true
	match k.keycode:
		KEY_ENTER, KEY_KP_ENTER:
			if not _menu.visible:
				chat.focus_input()
		KEY_SLASH:
			if not _menu.visible:
				chat.focus_input()
				handled = false  # let the "/" reach the input
		KEY_ESCAPE:
			if bool(hud.spells.get("casting")):
				stop_cast()
			elif hud.any_window_open():
				hud.close_windows()
			elif target_id != 0:
				set_target(0)
			else:
				_menu.visible = not _menu.visible
		KEY_TAB:
			cycle_target(k.shift_pressed)
		KEY_F1:
			set_target(my_id)
		KEY_R:
			toggle_auto_attack()
		KEY_K:
			hud.toggle_abilities()
		KEY_J:
			hud.class_ui.call("toggle_skills")
		KEY_B:
			hud.spells.call("toggle_spellbook")
		KEY_T:
			interact()
		KEY_P:
			hud.spells.call("toggle_group")
		KEY_I:
			hud.toggle_inventory()
		KEY_L:
			loot_target()
		KEY_C:
			consider()
		KEY_X:
			set_sitting(int(stats.get("flags", 0)) & FLAG_SITTING == 0)
		_:
			if k.keycode >= KEY_0 and k.keycode <= KEY_9 and not _menu.visible:
				var n: int = int(k.keycode - KEY_0)
				use_slot(9 if n == 0 else n - 1)
			else:
				handled = false
	if handled:
		get_viewport().set_input_as_handled()


func _pick_at(screen_pos: Vector2) -> int:
	var cam: Camera3D = get_viewport().get_camera_3d()
	if cam == null:
		return 0
	# A click within the normal radius of a body wins (the nearest such body). Only when the click
	# misses every body that way does a large body's wider radius (scaled by its collision radius) count.
	var best: int = 0
	var best_d: float = PICK_RADIUS_PX
	var wide: int = 0
	var wide_score: float = 1.0
	for r: RemoteEntity in remotes.values():
		var anchor: Vector3 = r.pick_point()
		if cam.is_position_behind(anchor):
			continue
		var d: float = cam.unproject_position(anchor).distance_to(screen_pos)
		if d < best_d:
			best_d = d
			best = r.entity_id
		var reach_px: float = PICK_RADIUS_PX * clampf(r.radius() / 0.5, 1.0, 4.0)
		if d < reach_px and d / reach_px < wide_score:
			wide_score = d / reach_px
			wide = r.entity_id
	return best if best != 0 else wide


# ------------------------------------------------------------------------------ intents (also used by the autotest)

func set_target(eid: int) -> void:
	if target_id != 0 and remotes.has(target_id):
		(remotes[target_id] as RemoteEntity).set_selected(false)
	if eid != target_id:
		hud.spells.call("clear_target_buffs")  # the server sends the new target's effects, if it has any
	target_id = eid
	if eid == 0:
		hud.clear_target()
	elif remotes.has(eid):
		(remotes[eid] as RemoteEntity).set_selected(true)
	Net.send(Proto.encode_set_target({"entity_id": eid}))


## Tab cycles living mobs by distance; Shift+Tab goes the other way.
func cycle_target(reverse: bool) -> void:
	var list: Array = []
	for r: RemoteEntity in remotes.values():
		if r.kind == RemoteEntity.KIND_MOB and r.is_alive() and not r.is_pet():  # Tab finds foes, not pets
			var d: float = r.position.distance_to(local_player.position)
			if d <= TAB_RANGE:
				list.append([d, r.entity_id])
	if list.is_empty():
		chat.add_system("No targets nearby.")
		return
	list.sort_custom(func(a: Array, b: Array) -> bool: return float(a[0]) < float(b[0]))
	var ids: Array = list.map(func(e: Array) -> int: return int(e[1]))
	var cur: int = ids.find(target_id)
	var next: int = 0 if cur < 0 else (cur + (-1 if reverse else 1) + ids.size()) % ids.size()
	set_target(int(ids[next]))


func toggle_auto_attack() -> void:
	Net.send(Proto.encode_auto_attack({"enabled": not bool(stats.get("auto_attack", false))}))


func use_slot(index: int) -> void:
	if index < 0 or index >= hud.hotbar_ids.size():
		return
	var id: String = hud.hotbar_ids[index]
	if id == Hud.ATTACK_ID:
		toggle_auto_attack()
	elif id.begins_with("cmd:"):
		run_hotbar_command(id)
	elif id.begins_with(GEM_PREFIX):
		cast_gem(id.substr(GEM_PREFIX.length()).to_int())
	elif id != "":
		use_ability(id)


func use_ability(id: String) -> void:
	Net.send(Proto.encode_use_ability({"ability_id": id, "target_id": target_id}))


## Casts the spell in a gem (0-based) at the current target; the server picks friend or foe.
func cast_gem(gem: int) -> void:
	if str(hud.spells.call("gem_spell", gem)) == "":
		chat.add_system("Spell gem %d is empty. Open your spellbook (B) and drag a spell onto it." % (gem + 1))
		return
	Net.send(Proto.encode_cast_spell({"gem": gem, "target_id": target_id}))


func stop_cast() -> void:
	Net.send(Proto.encode_stop_cast())


## Orders our pet (attack uses the current target).
func send_pet_command(order: int) -> void:
	pet_commands_sent += 1
	Net.send(Proto.encode_pet_command({"command": order, "target_id": target_id if order == 0 else 0}))


## A command dropped on the hotbar ("cmd:...", see class_ui.gd).
func run_hotbar_command(cmd: String) -> void:
	match cmd:
		"cmd:sit":
			set_sitting(int(stats.get("flags", 0)) & FLAG_SITTING == 0)
		"cmd:consider":
			consider()
		"cmd:loot":
			loot_target()
		"cmd:pet_attack":
			send_pet_command(0)
		"cmd:pet_follow":
			send_pet_command(1)
		"cmd:pet_guard":
			send_pet_command(2)
		"cmd:pet_back":
			send_pet_command(3)
		"cmd:pet_dismiss":
			send_pet_command(4)
		"cmd:stop":
			stop_cast()


func memorize(gem: int, spell_id: String) -> void:
	Net.send(Proto.encode_memorize_spell({"gem": gem, "spell_id": spell_id}))


func forget(gem: int) -> void:
	Net.send(Proto.encode_memorize_spell({"gem": gem, "spell_id": ""}))


func scribe(slot: int) -> void:
	Net.send(Proto.encode_scribe_spell({"slot": slot}))


func open_trainer() -> void:
	if target_id != 0 and remotes.has(target_id) and (remotes[target_id] as RemoteEntity).kind == RemoteEntity.KIND_NPC:
		Net.send(Proto.encode_trainer_open({"npc_id": target_id}))
	else:
		chat.add_system("Target a trainer to talk to them (click them, then press T).")


## Talk to the targeted NPC: trainers train, vendors trade, bankers open the bank, auctioneers the board.
## A targeted player is offered a trade instead.
func interact() -> void:
	if target_id == 0 or not remotes.has(target_id):
		chat.add_system("Target someone to talk to them (click them, then press T).")
		return
	var r: RemoteEntity = remotes[target_id]
	if r.kind == RemoteEntity.KIND_PLAYER:
		trade_request()
		return
	if r.kind != RemoteEntity.KIND_NPC:
		chat.add_system("%s has nothing to say." % _cap(r.display_name))
		return
	match r.class_id:
		"vendor":
			Net.send(Proto.encode_vendor_open({"npc_id": target_id}))
		"banker":
			Net.send(Proto.encode_bank_open({"npc_id": target_id}))
		"auctioneer":
			auction_npc = target_id
			Net.send(Proto.encode_auction_open({"npc_id": target_id}))
		_:
			Net.send(Proto.encode_trainer_open({"npc_id": target_id}))


# ------------------------------------------------------------------------------ items (Phase 5)

func move_item(from_code: int, to_code: int, count: int = 0) -> void:
	Net.send(Proto.encode_move_item({"from_slot": from_code, "to_slot": to_code, "count": count}))


func destroy_item(code: int, item_id: String) -> void:
	Net.send(Proto.encode_destroy_item({"slot": code, "item_id": item_id}))


func use_item(code: int) -> void:
	Net.send(Proto.encode_use_item({"slot": code, "target_id": target_id}))


func vendor_buy(npc_id: int, item_id: String, count: int = 1, buyback: bool = false) -> void:
	Net.send(Proto.encode_vendor_buy({"npc_id": npc_id, "item_id": item_id, "count": count, "buyback": buyback}))


func vendor_sell(npc_id: int, code: int, count: int = 0) -> void:
	Net.send(Proto.encode_vendor_sell({"npc_id": npc_id, "slot": code, "count": count}))


func bank_coin(deposit: bool, amount: int) -> void:
	if amount > 0:
		Net.send(Proto.encode_bank_coin({"deposit": deposit, "amount": amount}))


func trade_request() -> void:
	if target_id == 0 or not remotes.has(target_id) or (remotes[target_id] as RemoteEntity).kind != RemoteEntity.KIND_PLAYER:
		chat.add_system("Target another player to trade with them.")
		return
	Net.send(Proto.encode_trade_request({"target_id": target_id}))


func trade_respond(from_id: int, accept: bool) -> void:
	Net.send(Proto.encode_trade_respond({"from_id": from_id, "accept": accept}))


func trade_set_item(index: int, code: int) -> void:
	Net.send(Proto.encode_trade_set_item({"index": index, "slot": code}))


func trade_set_coin(coin: int) -> void:
	Net.send(Proto.encode_trade_set_coin({"coin": coin}))


## trade_id and revision name the offer the player saw: the server ignores an accept for one that changed.
func trade_accept(accept: bool, trade_id: int = 0, revision: int = 0) -> void:
	Net.send(Proto.encode_trade_accept({"accept": accept, "trade_id": trade_id, "revision": revision}))


func trade_cancel() -> void:
	Net.send(Proto.encode_trade_cancel())


func auction_search(npc_id: int, text: String, kind: String, min_level: int, max_level: int, page: int, mine: bool) -> void:
	Net.send(Proto.encode_auction_search({"npc_id": npc_id, "text": text, "kind": kind, "min_level": min_level, "max_level": max_level,
		"page": page, "mine": mine}))


func auction_post(npc_id: int, code: int, item_id: String, price: int) -> void:
	Net.send(Proto.encode_auction_post({"npc_id": npc_id, "slot": code, "item_id": item_id, "price": price}))


func auction_buy(npc_id: int, listing_id: int, price: int) -> void:
	Net.send(Proto.encode_auction_buy({"npc_id": npc_id, "listing_id": listing_id, "price": price}))


func auction_cancel(npc_id: int, listing_id: int) -> void:
	Net.send(Proto.encode_auction_cancel({"npc_id": npc_id, "listing_id": listing_id}))


func roll_choice(roll_id: int, choice: int) -> void:
	Net.send(Proto.encode_loot_roll_choice({"roll_id": roll_id, "choice": choice}))


## Players and NPCs show what they hold: the server sends the worn primary/secondary/ranged items.
func _on_equipment(msg: Dictionary) -> void:
	var eid: int = int(msg["entity_id"])
	var held: Dictionary = {"main_hand": str(msg["main_hand"]), "off_hand": str(msg["off_hand"]), "ranged": str(msg["ranged"])}
	if eid == my_id:
		local_player.set_equipment(held)
	elif remotes.has(eid):
		(remotes[eid] as RemoteEntity).set_equipment(held)


func trainer_buy(npc_id: int, item_id: String) -> void:
	Net.send(Proto.encode_trainer_buy({"npc_id": npc_id, "item_id": item_id}))


## Answers the offer the prompt showed (the server ignores replies to an offer it replaced).
func rez_answer(offer_id: int, accept: bool) -> void:
	Net.send(Proto.encode_rez_response({"offer_id": offer_id, "accept": accept}))


func loot_target() -> void:
	if target_id != 0 and remotes.has(target_id) and (remotes[target_id] as RemoteEntity).kind == RemoteEntity.KIND_CORPSE:
		Net.send(Proto.encode_loot_open({"corpse_id": target_id}))
	else:
		chat.add_system("Target a corpse to loot it.")


func loot_take(slot: int) -> void:
	if hud.loot_corpse != 0:
		Net.send(Proto.encode_loot_take({"corpse_id": hud.loot_corpse, "slot": slot}))


func loot_close() -> void:
	if hud.loot_corpse != 0:
		Net.send(Proto.encode_loot_close({"corpse_id": hud.loot_corpse}))
	hud.hide_loot()


func respawn() -> void:
	Net.send(Proto.encode_respawn_request())


func set_sitting(sitting: bool) -> void:
	Net.send(Proto.encode_set_sitting({"sitting": sitting}))


func consider() -> void:
	if target_id == 0 or not remotes.has(target_id):
		chat.add_system("You must have a target to consider.")
		return
	var r: RemoteEntity = remotes[target_id]
	var verdicts: Array[String] = [
		"looks like it would not be worth your time.", "looks like an easy fight.", "looks like a fair fight, in your favor.",
		"looks like an even fight.", "looks like quite a gamble.", "looks like it would wipe the floor with you!",
	]
	chat.add_system("%s (level %d) %s" % [_cap(r.display_name), r.level, verdicts[clampi(target_con, 0, 5)]])


func _on_command(cmd: String, args: String) -> void:
	match cmd:
		"sit":
			set_sitting(true)
		"stand":
			set_sitting(false)
		"loc":
			chat.add_system("Your location is %.1f, %.1f." % [local_player.position.x, local_player.position.z])
		"con", "consider":
			consider()
		"attack":
			toggle_auto_attack()
		"target":
			_target_by_name(args)
		"cast":
			var n: int = args.strip_edges().to_int()
			if n >= 1 and n <= 8:
				cast_gem(n - 1)
			else:
				chat.add_system("Usage: /cast 1-8 (the spell gem to cast).")
		"stopcast":
			stop_cast()
		"book", "spellbook":
			hud.spells.call("toggle_spellbook")
		"group":
			hud.spells.call("toggle_group")
		"pet":
			var word: String = args.strip_edges().to_lower().replace(" ", "")
			if PET_ORDERS.has(word):
				send_pet_command(int(PET_ORDERS[word]))
			else:
				chat.add_system("Usage: /pet attack | follow | guard | back | dismiss")
		"stopsong":
			stop_cast()
		"skills":
			hud.class_ui.call("toggle_skills")
		"trade":
			# A name that is not here must not fall back to whoever is targeted already.
			if args.strip_edges() == "" or _target_by_name(args):
				trade_request()
		"lootrule":
			match args.strip_edges().to_lower():
				"rr", "roundrobin", "round robin":
					Net.send(Proto.encode_set_loot_rule({"rule": 1}))
				"ng", "needgreed", "need", "greed":
					Net.send(Proto.encode_set_loot_rule({"rule": 0}))
				_:
					chat.add_system("Usage: /lootrule ng (need before greed) | rr (round robin)")
		"inventory", "inv":
			hud.toggle_inventory()
		_:
			chat.add_system("Unknown command /%s. Try /sit, /stand, /loc, /con, /attack, /target NAME, /cast N, /stopcast, /stopsong, /pet ORDER, /book, /skills, /trade [NAME], /lootrule ng|rr, /inv." % cmd)


## Targets the nearest entity whose name contains `text`; false (with a message) when there is none.
func _target_by_name(text: String) -> bool:
	var needle: String = text.strip_edges().to_lower()
	if needle == "":
		return false
	var best: int = 0
	var best_d: float = INF
	for r: RemoteEntity in remotes.values():
		if r.display_name.to_lower().contains(needle):
			var d: float = r.position.distance_to(local_player.position)
			if d < best_d:
				best_d = d
				best = r.entity_id
	if best != 0:
		set_target(best)
		return true
	chat.add_system("No '%s' nearby." % text)
	return false


# ------------------------------------------------------------------------------ server messages

func _on_world_message(msg: Dictionary) -> void:
	var id: int = msg["_id"]
	match id:
		Proto.ENTITY_SPAWN:
			var eid: int = int(msg["entity_id"])
			if eid == my_id:  # our own look changed (an illusion)
				local_player.apply_spawn_look(msg)
				return
			if remotes.has(eid):
				(remotes[eid] as RemoteEntity).refresh(msg)
				return
			var r: RemoteEntity = RemoteEntity.new()
			r.setup(msg, _server_time)
			if r.kind == RemoteEntity.KIND_CORPSE:
				r.fresh_death = _consume_recent_death(r.position)
			r.name = "Entity_%d" % eid
			add_child(r)
			remotes[eid] = r
			if r.kind == RemoteEntity.KIND_PLAYER:
				seen_names[str(msg["name"])] = true
			if eid == target_id:
				r.set_selected(true)
		Proto.ENTITY_DESPAWN:
			var eid: int = int(msg["entity_id"])
			if remotes.has(eid):
				var r: RemoteEntity = remotes[eid]
				r.queue_free()
				remotes.erase(eid)
			if eid == target_id:
				target_id = 0
				hud.clear_target()
				hud.spells.call("clear_target_buffs")
		Proto.SNAPSHOT:
			snapshots_received += 1
			var t: float = float(msg["server_tick"]) / float(_tick_rate)
			if not _has_time or absf(_server_time - t) > 0.25:
				_server_time = t
				_has_time = true
			else:
				_server_time = lerpf(_server_time, t, 0.05)
			local_player.reconcile(int(msg["ack_seq"]), Vector3(float(msg["self_x"]), float(msg["self_y"]), float(msg["self_z"])), false)
			for e: Dictionary in msg["entities"]:
				var eid: int = int(e["entity_id"])
				if remotes.has(eid):
					var r: RemoteEntity = remotes[eid]
					r.push_state(t, Vector3(float(e["x"]), float(e["y"]), float(e["z"])), float(e["yaw"]), int(e["hp_pct"]), int(e["flags"]),
						int(e.get("flags2", 0)))
		Proto.MOVE_CORRECTION:
			corrections_received += 1
			local_player.reconcile(int(msg["ack_seq"]), Vector3(float(msg["x"]), float(msg["y"]), float(msg["z"])), true)
		Proto.CHAT_MESSAGE:
			chat.add_message(int(msg["channel"]), str(msg["from_name"]), str(msg["text"]))
		Proto.PLAYER_STATS:
			var old_level: int = my_level
			stats = msg
			my_level = int(msg["level"])
			if float(msg["run_speed"]) > 0.0:
				local_player.run_speed = float(msg["run_speed"])
			if my_level > old_level:
				level_ups += 1
			hud.set_stats(msg)
		Proto.TARGET_INFO:
			_on_target_info(msg)
		Proto.COMBAT_EVENT:
			_on_combat_event(msg)
		Proto.ABILITY_LIST:
			abilities = msg["abilities"]
			hud.set_abilities(abilities, local_player.display_name)
		Proto.ABILITY_RESULT:
			hud.note_ability_result(msg)
			hud.spells.call("note_ability_result", msg)
			if bool(msg["ok"]):
				abilities_used += 1
			elif str(msg["message"]) != "":
				chat.add_combat(str(msg["message"]), Color(1.0, 0.55, 0.45))
		Proto.PLAYER_DIED:
			deaths += 1
			last_death = msg
			death_position = local_player.position
			hud.show_death(str(msg["killer"]), int(msg["xp_lost"]))
			set_target(0)
		Proto.LOOT_WINDOW:
			hud.show_loot(msg)
		Proto.LOOT_CLOSED:
			hud.hide_loot()
			last_loot_closed = str(msg["message"])
			if last_loot_closed != "":
				chat.add_system(last_loot_closed)
			else:
				corpses_looted += 1  # closed normally: everything we wanted is in the pack
		Proto.INVENTORY:
			hud.set_inventory(msg)
			var held: Dictionary = {"main_hand": "", "off_hand": "", "ranged": ""}
			for it: Dictionary in msg["items"]:
				match int(it["slot"]):
					ItemDb.PRIMARY: held["main_hand"] = str(it["item_id"])
					ItemDb.SECONDARY: held["off_hand"] = str(it["item_id"])
					ItemDb.RANGED: held["ranged"] = str(it["item_id"])
			local_player.set_equipment(held)
		Proto.SKILL_VALUES:
			hud.class_ui.call("set_skills", msg)
			if bool(msg["announce"]):
				for s: Dictionary in msg["skills"]:
					chat.add_system("You have become better at %s! (%d)" % [str(s["name"]), int(s["value"])])
		Proto.SPELL_BOOK:
			hud.spells.call("set_spellbook", msg)
			hud.refresh_hotbar()
		Proto.GEM_BAR:
			hud.spells.call("set_gems", msg)
			hud.refresh_hotbar()
		Proto.CAST_START:
			var caster: int = int(msg["caster_id"])
			if caster == my_id:
				hud.spells.call("cast_started", msg)
			else:
				chat.add_combat("%s begins to cast %s." % [_cap(_name_of(caster, true)), str(msg["spell_name"])], Color(0.7, 0.65, 0.9))
			_cast_glow(caster, int(msg["school"]), true)
			var who: Node3D = _anchor_of(caster)
			if who != null:
				who.call("on_cast_start", _anchor_of(int(msg["target_id"])))
		Proto.CAST_END:
			var caster: int = int(msg["caster_id"])
			_cast_glow(caster, 0, false)
			_note_cast_end(caster)
			var who: Node3D = _anchor_of(caster)
			if who != null:
				who.call("on_cast_end", int(msg["result"]) == 0, true)
			if caster == my_id:
				hud.spells.call("cast_ended", msg)
				var why: String = str(msg["message"])
				if int(msg["result"]) != 0 and why != "":
					chat.add_combat(why, Color(1.0, 0.55, 0.45))
		Proto.BUFF_LIST:
			var who: int = int(msg["entity_id"])
			if who == my_id:
				hud.spells.call("set_buffs", who, msg["buffs"], true)
			if who == target_id:
				hud.spells.call("set_buffs", who, msg["buffs"], false)
		Proto.TRAINER_WINDOW:
			last_trainer = msg
			hud.spells.call("show_trainer", msg)
		Proto.REZ_OFFER:
			hud.spells.call("show_rez", msg)
			chat.add_system("%s is trying to resurrect you." % str(msg["caster"]))
		Proto.PET_STATUS:
			pet_status = msg
			hud.class_ui.call("set_pet", msg)
		Proto.SONG_UPDATE:
			song = msg
			if str(msg["song_id"]) != "":
				songs_started += 1
			hud.class_ui.call("set_song", msg)
		Proto.CHARACTER_SHEET:
			sheet = msg
			hud.items.call("set_sheet", msg)
		Proto.ENTITY_EQUIPMENT:
			_on_equipment(msg)
		Proto.VENDOR_WINDOW:
			last_vendor = msg
			hud.items.call("show_vendor", msg)
		Proto.BANK_WINDOW:
			bank_open = int(msg["npc_id"]) != 0
			hud.items.call("show_bank", msg)
		Proto.TRADE_INVITE:
			hud.items.call("show_invite", msg)
			chat.add_system("%s would like to trade with you." % str(msg["from_name"]))
		Proto.TRADE_STATE:
			hud.items.call("show_trade", msg)
		Proto.TRADE_CLOSED:
			last_trade_closed = msg
			hud.items.call("close_trade", msg)
		Proto.AUCTION_WINDOW:
			auction_npc = int(msg["npc_id"])
			hud.items.call("show_auction", msg)
		Proto.AUCTION_RESULTS:
			last_auction_results = msg
			hud.items.call("show_auction_results", msg)
		Proto.LOOT_ROLL:
			hud.items.call("show_roll", msg)
			chat.add_system("Roll for %s: Need, Greed or Pass." % ItemDb.name_of(str(msg["item_id"])))


func _on_target_info(msg: Dictionary) -> void:
	var eid: int = int(msg["entity_id"])
	if eid == 0:
		if target_id != 0 and remotes.has(target_id):
			(remotes[target_id] as RemoteEntity).set_selected(false)
		target_id = 0
		hud.clear_target()
		hud.spells.call("clear_target_buffs")
		return
	if eid != target_id:
		hud.spells.call("clear_target_buffs")
	target_id = eid
	target_con = int(msg["con"])
	var kind: int = 0
	if remotes.has(eid):
		kind = (remotes[eid] as RemoteEntity).kind
	hud.set_target(_cap(str(msg["name"])), int(msg["level"]), target_con, int(msg["hp_pct"]), kind)


func _name_of(eid: int, subject: bool) -> String:
	if eid == my_id:
		return "You" if subject else "YOU"
	if remotes.has(eid):
		return (remotes[eid] as RemoteEntity).display_name
	return "something"


static func _cap(s: String) -> String:
	return s if s.is_empty() else s.substr(0, 1).to_upper() + s.substr(1)


func _on_combat_event(e: Dictionary) -> void:
	combat_events += 1
	var src: int = int(e["source_id"])
	var tgt: int = int(e["target_id"])
	var kind: int = int(e["kind"])
	var amount: int = int(e["amount"])
	_animate_event(kind, src, tgt, amount)
	var ability: String = str(e["ability"])
	var mine: bool = src == my_id
	var at_me: bool = tgt == my_id
	var s: String = _name_of(src, true)
	var t: String = _name_of(tgt, false)
	var line: String = ""
	var color: Color = Color(0.8, 0.8, 0.8)
	match kind:
		Ev.HIT, Ev.CRIT:
			var verb: String = "hit" if mine else "hits"
			line = "%s %s %s for %d points of damage." % [s, verb, t, amount]
			if kind == Ev.CRIT:
				line = ("You score a critical hit! " if mine else "%s scores a critical hit! " % _cap(s)) + line
			color = Color(1.0, 1.0, 1.0) if mine else (Color(1.0, 0.45, 0.4) if at_me else Color(0.75, 0.75, 0.75))
			_float(tgt, str(amount) + ("!" if kind == Ev.CRIT else ""), Color(1.0, 0.95, 0.5) if kind == Ev.CRIT else (Color(1, 0.4, 0.35) if at_me else Color.WHITE), mine or at_me)
		Ev.MISS:
			line = ("You try to hit %s, but miss!" % t) if mine else ("%s tries to hit %s, but misses!" % [s, t])
			_float(tgt, "miss", Color(0.7, 0.7, 0.7), mine or at_me)
		Ev.DODGE, Ev.PARRY, Ev.RIPOSTE:
			var what: String = ["dodges", "parries", "ripostes"][kind - Ev.DODGE]
			var me_what: String = ["dodge", "parry", "riposte"][kind - Ev.DODGE]
			line = ("You %s %s's attack!" % [me_what, s]) if at_me else ("%s %s %s attack!" % [_cap(t), what, "your" if mine else s + "'s"])
			_float(tgt, me_what, Color(0.6, 0.85, 1.0), mine or at_me)
		Ev.ABILITY_HIT:
			line = "%s %s hits %s for %d points of damage." % ["Your" if mine else s + "'s", ability, t, amount]
			color = Color(1.0, 0.85, 0.4) if mine else Color(1.0, 0.45, 0.4)
			_float(tgt, str(amount), Color(1.0, 0.75, 0.3), mine or at_me)
		Ev.ABILITY_MISS:
			line = "%s %s misses %s." % ["Your" if mine else s + "'s", ability, t]
		Ev.TAUNT:
			line = ("You goad %s into attacking you with %s." % [t, ability]) if mine else ("%s taunts %s." % [s, t])
			color = Color(1.0, 0.7, 0.3)
		Ev.TAUNT_FAIL:
			line = "%s ignores %s %s." % [_cap(t), "your" if mine else s + "'s", ability]
		Ev.STUN:
			line = "%s is stunned!" % _cap(t)
			color = Color(1.0, 0.9, 0.5)
		Ev.STUN_FAIL:
			line = "%s shrugs off %s %s." % [_cap(t), "your" if mine else s + "'s", ability]
		Ev.HEAL:
			line = ("Your %s heals you for %d." % [ability, amount]) if mine else ("%s is healed for %d." % [s, amount])
			color = Color(0.5, 1.0, 0.5)
			_float(tgt, "+%d" % amount, Color(0.45, 1.0, 0.45), mine or at_me)
		Ev.BUFF:
			line = ("You feel the power of %s." % ability) if mine else ("%s uses %s." % [s, ability])
			color = Color(0.6, 0.8, 1.0)
		Ev.BUFF_FADE:
			line = "Your %s fades." % ability
			color = Color(0.6, 0.8, 1.0)
		Ev.DEATH:
			if at_me:
				line = "You have been slain by %s!" % s.to_lower() if not mine else "You died."
				color = Color(1.0, 0.3, 0.3)
			elif mine:
				line = "You have slain %s!" % t
				color = Color(1.0, 0.9, 0.4)
				kills += 1
			else:
				line = "%s has been slain by %s!" % [_cap(t), s]
		Ev.EVADE:
			line = "%s evades %s attack." % [_cap(t), "your" if mine else s + "'s"]
		Ev.SPELL_HIT:
			var school: String = RESIST_NAMES[clampi(int(e["school"]), 0, 5)]
			line = "%s %s hits %s for %d points of %sdamage." % ["Your" if mine else s + "'s", ability, t, amount, (school + " ") if school != "" else ""]
			color = Color(0.85, 0.75, 1.0) if mine else (Color(1.0, 0.45, 0.4) if at_me else Color(0.75, 0.75, 0.8))
			if mine:
				spells_landed += 1
			_float(tgt, str(amount), _school_color(int(e["school"])), mine or at_me)
			_burst(tgt, _school_color(int(e["school"])), false)
		Ev.RESIST:
			line = ("%s resisted your %s!" % [_cap(t), ability]) if mine else ("You resist %s's %s!" % [s, ability] if at_me else "%s resisted %s." % [_cap(t), ability])
			color = Color(0.8, 0.8, 0.6)
			if mine:
				resists += 1
			_float(tgt, "resist", Color(0.8, 0.8, 0.6), mine or at_me)
		Ev.DOT_TICK:
			line = "%s %s from %s %s." % [_cap(t), "take" if at_me else "takes", "%d damage" % amount, ("your " + ability) if mine else ability]
			color = Color(0.9, 0.7, 0.4) if mine else Color(0.75, 0.6, 0.5)
			_float(tgt, str(amount), _school_color(int(e["school"])).darkened(0.1), mine or at_me)
		Ev.HOT_TICK:
			line = ("%s %s healed for %d by %s." % [_cap(t), "are" if at_me else "is", amount, ability])
			color = Color(0.5, 1.0, 0.6)
			if at_me:
				heals_received += 1
			_float(tgt, "+%d" % amount, Color(0.45, 1.0, 0.55), mine or at_me)
		Ev.MEZ:
			line = "%s is mesmerized by %s." % [_cap(t), ability]
			color = Color(0.9, 0.7, 1.0)
			_burst(tgt, Color(0.9, 0.6, 1.0), true)
		Ev.MEZ_BREAK:
			line = "%s is no longer mesmerized." % _cap(t)
		Ev.ROOT:
			line = "%s is rooted to the ground by %s." % [_cap(t), ability]
			color = Color(0.6, 0.9, 1.0)
			_burst(tgt, _school_color(int(e["school"])), false)
		Ev.ROOT_BREAK:
			line = "%s breaks free." % _cap(t)
		Ev.SNARE:
			line = "%s slows to a crawl." % _cap(t)
			color = Color(0.6, 0.85, 1.0)
		Ev.SLOW:
			line = "%s %s attacks slow down." % [_cap(t), "Your" if at_me else "'s"]
		Ev.FEAR:
			line = "%s flees in terror!" % _cap(t)
			color = Color(1.0, 0.85, 0.5)
		Ev.DEBUFF:
			line = ("Your %s takes hold on %s." % [ability, t]) if mine else ("%s is afflicted by %s." % [_cap(t), ability])
			color = Color(0.9, 0.7, 0.5)
			if mine:
				spells_landed += 1
			_burst(tgt, _school_color(int(e["school"])), false)
		Ev.DISPEL:
			line = "%s %s %d effect%s from %s." % ["Your" if mine else s + "'s", ability, amount, "" if amount == 1 else "s", t if not at_me else "you"]
			color = Color(0.85, 0.95, 1.0)
		Ev.RESURRECT:
			line = ("%s calls on the spirit within %s." % [_cap(s), t]) if ability != "" else ("%s has been resurrected!" % _cap(t))
			color = Color(1.0, 0.95, 0.6)
			_burst(tgt, Color(1.0, 0.95, 0.6), true)
		Ev.TELEPORT:
			line = "%s vanish%s in a swirl of light." % [_cap(t), "" if at_me else "es"]
			color = Color(0.7, 0.8, 1.0)
			_burst(tgt, Color(0.7, 0.8, 1.0), true)
		Ev.MANA_RESTORE:
			line = "%s gather%s %d mana." % [_cap(t), "" if at_me else "s", amount]
			color = Color(0.5, 0.65, 1.0)
			_burst(tgt, Color(0.4, 0.55, 1.0), true)
		Ev.IMMUNE:
			line = "%s is unaffected by %s %s." % [_cap(t), "your" if mine else s + "'s", ability]
		Ev.DAMAGE_SHIELD:
			line = "%s %s burned for %d points of damage." % [_cap(t), "are" if at_me else "is", amount]
			color = Color(1.0, 0.7, 0.4) if mine else Color(0.8, 0.6, 0.5)
			_float(tgt, str(amount), Color(1.0, 0.6, 0.3), mine or at_me)
		Ev.LIFETAP:
			line = "%s %s drains %s for %d points of life." % ["Your" if mine else s + "'s", ability, t, amount]
			color = Color(0.8, 0.5, 0.9) if mine else Color(0.7, 0.55, 0.75)
			if mine:
				spells_landed += 1
			_float(tgt, str(amount), Color(0.8, 0.45, 0.9), mine or at_me)
		Ev.DRAIN_TICK:
			line = "%s %s %d life to %s %s." % [_cap(t), "lose" if at_me else "loses", amount, "your" if mine else s + "'s", ability]
			color = Color(0.75, 0.5, 0.85)
			_float(tgt, str(amount), Color(0.75, 0.45, 0.85), mine or at_me)
		Ev.CHARM:
			line = ("%s falls under your control." % _cap(t)) if mine else ("%s falls under %s's control." % [_cap(t), s])
			color = Color(0.95, 0.7, 1.0)
			_burst(tgt, Color(0.95, 0.6, 1.0), true)
		Ev.CHARM_BREAK:
			line = "%s breaks free of %s control!" % [_cap(t), "your" if mine else s + "'s"]
			color = Color(1.0, 0.5, 0.5)
		Ev.FEIGN:
			line = "%s %s to the ground, still as death." % [_cap(s), "fall" if mine else "falls"]
			color = Color(0.7, 0.7, 0.75)
		Ev.FEIGN_FAIL:
			line = "%s %s to the ground, but fool%s nobody." % [_cap(s), "fall" if mine else "falls", "" if mine else "s"]
		Ev.HIDE:
			if mine:
				line = "You melt into the shadows."
		Ev.HIDE_FAIL:
			if mine:
				line = "You fail to hide."
		Ev.SUMMON:
			line = ("You call forth %s." % _name_of(tgt, false)) if mine else ("%s summons a companion." % _cap(s))
			color = Color(0.7, 0.9, 1.0)
			_burst(tgt, Color(0.7, 0.9, 1.0), true)
		Ev.THREAT_DROP:
			if mine:
				line = "%s loses interest in you." % _cap(t)
				color = Color(0.7, 0.8, 0.7)
		Ev.ILLUSION:
			line = ("You take on a new shape." if at_me else "%s takes on a new shape." % _cap(t))
			color = Color(0.9, 0.8, 1.0)
			_burst(tgt, Color(0.9, 0.75, 1.0), true)
		Ev.SONG_PULSE:
			_burst(src, Color(0.6, 0.75, 1.0), true)
		Ev.PET_DEATH:
			line = "%s is destroyed." % _cap(t)
	if kind == Ev.HEAL:
		if at_me:
			heals_received += 1
		_burst(tgt, Color(0.45, 1.0, 0.55), true)
	elif kind == Ev.BUFF:
		_burst(tgt, Color(0.6, 0.8, 1.0), true)
	if line != "":
		chat.add_combat(_cap(line), color)


## Combat events drive animation: swings for the attacker, flinches for whoever takes damage,
## a release gesture for instant spells (cast spells already animated through CastStart/CastEnd).
func _animate_event(kind: int, src: int, tgt: int, amount: int) -> void:
	var s: Node3D = _anchor_of(src) if src != 0 else null
	var t: Node3D = _anchor_of(tgt) if tgt != 0 else null
	match kind:
		Ev.HIT, Ev.CRIT, Ev.MISS, Ev.DODGE, Ev.PARRY, Ev.RIPOSTE:
			if s != null:
				s.call("on_attack", t, false)
		Ev.ABILITY_HIT, Ev.ABILITY_MISS, Ev.TAUNT, Ev.TAUNT_FAIL:
			if s != null:
				s.call("on_attack", t, true)
		Ev.SPELL_HIT, Ev.RESIST, Ev.HEAL, Ev.MEZ, Ev.ROOT, Ev.SNARE, Ev.SLOW, Ev.FEAR, Ev.DEBUFF, Ev.DISPEL, Ev.IMMUNE:
			# Spells with a cast time ended a moment ago (CastEnd); anything else from this source is instant.
			if s != null and Time.get_ticks_msec() - int(_last_cast_end.get(src, -100000)) > 500:
				s.call("on_cast_start", t)
				s.call("on_cast_end", true, true)
				_note_cast_end(src)
		Ev.DEATH:
			if t != null:
				_recent_deaths.append({"pos": t.global_position, "ms": Time.get_ticks_msec()})
	if t != null and amount > 0 and kind in [Ev.HIT, Ev.CRIT, Ev.ABILITY_HIT, Ev.SPELL_HIT]:
		t.call("on_hit")


## Remember when `caster` last finished a cast, forgetting entries older than the 500 ms window so
## the table never grows with casters that have since left.
func _note_cast_end(caster: int) -> void:
	var now: int = Time.get_ticks_msec()
	for k: Variant in _last_cast_end.keys():
		if now - int(_last_cast_end[k]) > 500:
			_last_cast_end.erase(k)
	_last_cast_end[caster] = now


## True (and forgotten) if something died near `pos` in the last two seconds: its corpse should
## play the death animation instead of appearing already dead.
func _consume_recent_death(pos: Vector3) -> bool:
	var now: int = Time.get_ticks_msec()
	var found: bool = false
	var keep: Array[Dictionary] = []
	for d: Dictionary in _recent_deaths:
		if now - int(d["ms"]) > 2000:
			continue
		if not found and (d["pos"] as Vector3).distance_to(pos) < 2.0:
			found = true
			continue
		keep.append(d)
	_recent_deaths = keep
	return found


func _school_color(school: int) -> Color:
	return GemSlot.SCHOOL_COLORS[clampi(school, 0, GemSlot.SCHOOL_COLORS.size() - 1)]


func _anchor_of(eid: int) -> Node3D:
	return local_player if eid == my_id else (remotes.get(eid) as Node3D)


## A one-shot burst of particles at an entity (spell landing, heal, buff): placeholder effects.
func _burst(eid: int, color: Color, rising: bool) -> void:
	var anchor: Node3D = _anchor_of(eid)
	if anchor == null:
		return
	var p: CPUParticles3D = CPUParticles3D.new()
	p.one_shot = true
	p.amount = 24
	p.lifetime = 0.9
	p.explosiveness = 0.85
	p.direction = Vector3.UP
	p.spread = 35.0 if rising else 180.0
	p.initial_velocity_min = 1.2 if rising else 2.0
	p.initial_velocity_max = 2.5 if rising else 4.0
	p.gravity = Vector3(0.0, 1.0 if rising else -3.0, 0.0)
	p.mesh = _spark_mesh(color)
	p.position = anchor.global_position + Vector3(0.0, 0.3 if rising else 1.1, 0.0)
	add_child(p)
	p.emitting = true
	get_tree().create_timer(1.6).timeout.connect(p.queue_free)


func _spark_mesh(color: Color) -> Mesh:
	var m: SphereMesh = SphereMesh.new()
	m.radius = 0.06
	m.height = 0.12
	m.radial_segments = 6
	m.rings = 3
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = color
	m.material = mat
	return m


## A swirl of motes around a caster while a spell is being cast.
func _cast_glow(eid: int, school: int, on: bool) -> void:
	var anchor: Node3D = _anchor_of(eid)
	if anchor == null:
		return
	var old: Node = anchor.get_node_or_null("CastGlow")
	if old != null:
		old.queue_free()
	if not on:
		return
	var p: CPUParticles3D = CPUParticles3D.new()
	p.name = "CastGlow"
	p.amount = 20
	p.lifetime = 0.8
	p.emission_shape = CPUParticles3D.EMISSION_SHAPE_RING
	p.emission_ring_axis = Vector3.UP
	p.emission_ring_radius = 0.7
	p.emission_ring_inner_radius = 0.5
	p.emission_ring_height = 0.1
	p.direction = Vector3.UP
	p.spread = 10.0
	p.initial_velocity_min = 0.8
	p.initial_velocity_max = 1.4
	p.gravity = Vector3.ZERO
	p.mesh = _spark_mesh(_school_color(school).lightened(0.2))
	p.position = Vector3(0.0, 0.2, 0.0)
	anchor.add_child(p)
	p.emitting = true


## Floating combat text above an entity, only for hits that involve us.
func _float(eid: int, text: String, color: Color, involves_me: bool) -> void:
	if not involves_me:
		return
	var anchor: Node3D = local_player if eid == my_id else (remotes.get(eid) as Node3D)
	if anchor == null:
		return
	var label: Label3D = Label3D.new()
	label.text = text
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.font_size = 56
	label.outline_size = 12
	label.modulate = color
	label.position = anchor.global_position + Vector3(randf_range(-0.4, 0.4), 2.6, 0.0)
	add_child(label)
	var tw: Tween = create_tween()
	tw.set_parallel(true)
	tw.tween_property(label, "position:y", label.position.y + 1.6, 1.1)
	tw.tween_property(label, "modulate:a", 0.0, 1.1).set_delay(0.3)
	tw.chain().tween_callback(label.queue_free)


# ------------------------------------------------------------------------------ queries for the autotest

## Nearest entity of `kind` that passes `accept` (a Callable taking the RemoteEntity), or 0.
func nearest(kind: int, accept: Callable, max_distance: float = 80.0) -> int:
	var best: int = 0
	var best_d: float = max_distance
	for r: RemoteEntity in remotes.values():
		if r.kind != kind or not accept.call(r):
			continue
		if kind == RemoteEntity.KIND_MOB and r.is_pet():  # pets are never prey
			continue
		var d: float = r.position.distance_to(local_player.position)
		if d < best_d:
			best_d = d
			best = r.entity_id
	return best


# ------------------------------------------------------------------------------ construction

func _build_environment() -> void:
	var sky_mat: ProceduralSkyMaterial = ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.29, 0.39, 0.6)
	sky_mat.sky_horizon_color = Color(0.78, 0.63, 0.52)
	sky_mat.ground_horizon_color = Color(0.45, 0.38, 0.32)
	sky_mat.ground_bottom_color = Color(0.2, 0.17, 0.15)
	var sky: Sky = Sky.new()
	sky.sky_material = sky_mat
	var env: Environment = Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_energy = 0.7
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.fog_enabled = true
	env.fog_light_color = Color(0.7, 0.6, 0.55)
	env.fog_density = 0.0035
	var we: WorldEnvironment = WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun: DirectionalLight3D = DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-52.0, -35.0, 0.0)
	sun.light_energy = 1.1
	sun.light_color = Color(1.0, 0.93, 0.82)
	sun.shadow_enabled = true
	add_child(sun)


func _grid_texture() -> ImageTexture:
	var size: int = 64
	var img: Image = Image.create_empty(size, size, false, Image.FORMAT_RGB8)
	img.fill(Color(0.33, 0.36, 0.27))
	for i: int in range(size):
		for w: int in range(2):
			img.set_pixel(i, w, Color(0.25, 0.27, 0.2))
			img.set_pixel(w, i, Color(0.25, 0.27, 0.2))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _build_ground() -> void:
	var min_x: float = float(info["min_x"])
	var min_z: float = float(info["min_z"])
	var max_x: float = float(info["max_x"])
	var max_z: float = float(info["max_z"])
	var ground_y: float = float(info["y"])
	var size: Vector2 = Vector2(max_x - min_x, max_z - min_z)

	var plane: PlaneMesh = PlaneMesh.new()
	plane.size = size
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.albedo_texture = _grid_texture()
	mat.uv1_scale = Vector3(size.x / 4.0, size.y / 4.0, 1.0)
	mat.roughness = 1.0
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	var ground: MeshInstance3D = MeshInstance3D.new()
	ground.mesh = plane
	ground.material_override = mat
	ground.position = Vector3((min_x + max_x) * 0.5, ground_y, (min_z + max_z) * 0.5)
	add_child(ground)

	# Boundary posts every 40 m so the zone edge is visible, and ring markers every 100 m from
	# the centre (danger rises outward).
	var post_mesh: BoxMesh = BoxMesh.new()
	post_mesh.size = Vector3(0.8, 3.0, 0.8)
	var post_mat: StandardMaterial3D = StandardMaterial3D.new()
	post_mat.albedo_color = Color(0.35, 0.3, 0.28)
	var x: float = min_x
	while x <= max_x:
		_post(post_mesh, post_mat, Vector3(x, ground_y + 1.5, min_z))
		_post(post_mesh, post_mat, Vector3(x, ground_y + 1.5, max_z))
		x += 40.0
	var z: float = min_z + 40.0
	while z < max_z:
		_post(post_mesh, post_mat, Vector3(min_x, ground_y + 1.5, z))
		_post(post_mesh, post_mat, Vector3(max_x, ground_y + 1.5, z))
		z += 40.0
	var ring_mat: StandardMaterial3D = StandardMaterial3D.new()
	ring_mat.albedo_color = Color(0.55, 0.32, 0.2)
	var ring_mesh: BoxMesh = BoxMesh.new()
	ring_mesh.size = Vector3(0.4, 1.2, 0.4)
	for ring: int in range(1, 10):
		var r: float = float(ring) * 100.0
		var n: int = 12 + ring * 6
		for i: int in range(n):
			var a: float = TAU * float(i) / float(n)
			_post(ring_mesh, ring_mat, Vector3(cos(a) * r, ground_y + 0.6, sin(a) * r))

	# A central ember brazier as a landmark.
	var base: MeshInstance3D = MeshInstance3D.new()
	var cyl: CylinderMesh = CylinderMesh.new()
	cyl.top_radius = 1.2
	cyl.bottom_radius = 1.5
	cyl.height = 1.0
	base.mesh = cyl
	base.position = Vector3(0.0, ground_y + 0.5, -8.0)
	base.material_override = post_mat
	add_child(base)
	var ember: MeshInstance3D = MeshInstance3D.new()
	var sphere: SphereMesh = SphereMesh.new()
	sphere.radius = 0.8
	sphere.height = 1.2
	ember.mesh = sphere
	var ember_mat: StandardMaterial3D = StandardMaterial3D.new()
	ember_mat.albedo_color = Color(1.0, 0.45, 0.15)
	ember_mat.emission_enabled = true
	ember_mat.emission = Color(1.0, 0.4, 0.1)
	ember_mat.emission_energy_multiplier = 2.0
	ember.material_override = ember_mat
	ember.position = Vector3(0.0, ground_y + 1.3, -8.0)
	add_child(ember)
	var light: OmniLight3D = OmniLight3D.new()
	light.light_color = Color(1.0, 0.55, 0.25)
	light.light_energy = 2.0
	light.omni_range = 12.0
	light.position = Vector3(0.0, ground_y + 2.5, -8.0)
	add_child(light)


func _post(mesh: Mesh, mat: Material, pos: Vector3) -> void:
	var m: MeshInstance3D = MeshInstance3D.new()
	m.mesh = mesh
	m.material_override = mat
	m.position = pos
	add_child(m)


func _build_hud() -> void:
	var layer: CanvasLayer = CanvasLayer.new()
	add_child(layer)
	var root: Control = Control.new()
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(root)

	_hud_label = Label.new()
	_hud_label.position = Vector2(12, 6)
	_hud_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_hud_label.add_theme_constant_override("outline_size", 6)
	_hud_label.add_theme_font_size_override("font_size", 13)
	root.add_child(_hud_label)

	var help: Label = Label.new()
	help.text = "W/S move  A/D turn  Q/E strafe  Right-drag look  Tab/click target  R attack  1-0 hotbar  K abilities  J skills  B spellbook  I inventory  L loot  T talk/trade  C consider  X sit  P group  Enter chat  Esc menu"
	help.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	help.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	help.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	help.grow_vertical = Control.GROW_DIRECTION_BEGIN
	help.offset_right = -12
	help.offset_bottom = -4
	help.add_theme_font_size_override("font_size", 12)
	help.add_theme_color_override("font_outline_color", Color.BLACK)
	help.add_theme_constant_override("outline_size", 6)
	root.add_child(help)

	hud = Hud.new()
	root.add_child(hud)
	hud.slot_used.connect(use_slot)
	hud.ability_used.connect(use_ability)
	hud.loot_requested.connect(loot_target)
	hud.loot_take.connect(loot_take)
	hud.loot_close.connect(loot_close)
	hud.respawn_requested.connect(respawn)
	hud.trainer_requested.connect(open_trainer)
	hud.scribe_requested.connect(scribe)
	hud.spells.connect("cast_gem", cast_gem)
	hud.spells.connect("memorize", memorize)
	hud.spells.connect("forget", forget)
	hud.spells.connect("trainer_buy", trainer_buy)
	hud.spells.connect("rez_answer", rez_answer)
	hud.class_ui.connect("pet_command", send_pet_command)
	hud.class_ui.connect("hotbar_command", run_hotbar_command)
	var it: Control = hud.items
	it.connect("move_item", move_item)
	it.connect("destroy_item", destroy_item)
	it.connect("use_item", use_item)
	it.connect("vendor_buy", vendor_buy)
	it.connect("vendor_sell", vendor_sell)
	it.connect("bank_coin", bank_coin)
	it.connect("trade_respond", trade_respond)
	it.connect("trade_set_item", trade_set_item)
	it.connect("trade_set_coin", trade_set_coin)
	it.connect("trade_accept", trade_accept)
	it.connect("trade_cancel", trade_cancel)
	it.connect("auction_search", auction_search)
	it.connect("auction_post", auction_post)
	it.connect("auction_buy", auction_buy)
	it.connect("auction_cancel", auction_cancel)
	it.connect("roll_choice", roll_choice)

	chat = ChatWindow.new()
	chat.anchor_left = 0.0
	chat.anchor_right = 0.0
	chat.anchor_top = 1.0
	chat.anchor_bottom = 1.0
	chat.offset_left = 12
	chat.offset_right = 392
	chat.offset_top = -262
	chat.offset_bottom = -22
	root.add_child(chat)
	chat.command.connect(_on_command)
	hud.notice.connect(chat.add_system)

	_menu = PanelContainer.new()
	_menu.visible = false
	_menu.set_anchors_preset(Control.PRESET_CENTER)
	_menu.grow_horizontal = Control.GROW_DIRECTION_BOTH
	_menu.grow_vertical = Control.GROW_DIRECTION_BOTH
	root.add_child(_menu)
	var box: VBoxContainer = VBoxContainer.new()
	box.custom_minimum_size = Vector2(280, 0)
	box.add_theme_constant_override("separation", 8)
	_menu.add_child(box)
	UI.title(box, "Menu", 22)
	UI.button(box, "Resume", func() -> void: _menu.visible = false)
	UI.button(box, "Camp (character select)", func() -> void: Session.logout())
	UI.button(box, "Log out", func() -> void: Session.quit_session("Logged out."))
	UI.button(box, "Exit game", _exit_game)


func _exit_game() -> void:
	Session.quit_session("")
	get_tree().quit()
