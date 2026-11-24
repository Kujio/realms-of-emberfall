## Social windows (Phase 7): the group window (P) with every member's health, mana and endurance (click a
## member to target them; F2-F6 do the same), the guild window (G: roster with online status, ranks,
## message of the day, and the officer tools), friends and ignore lists with chat channel switches (O),
## /who results, and the group and guild invitation popup. Pure presentation: every action goes out as a
## slash command through `command` (the server parses and checks it), targets through `target_requested`.
extends Control

signal command(text: String)
signal target_requested(entity_id: int)
signal tell_requested(name: String)

const LOOT_RULES: Array[String] = ["Need before greed", "Round robin"]
## GuildRankInfo.permissions bits (server game/social_types.hpp GuildPerm).
const PERM_INVITE: int = 1
const PERM_REMOVE: int = 2
const PERM_PROMOTE: int = 4
const PERM_DEMOTE: int = 8
const PERM_MOTD: int = 16
const PERM_DISBAND: int = 64
const MUTABLE_CHANNELS: Array = [["say", "Say"], ["shout", "Shout"], ["ooc", "OOC"], ["emote", "Emotes"], ["guild", "Guild"]]
## ChatChannel ids of MUTABLE_CHANNELS, for FriendsList.muted_channels bits.
const CHANNEL_IDS: Dictionary = {"say": 0, "shout": 3, "ooc": 4, "emote": 9, "guild": 8}

var my_name: String = ""
var group: Dictionary = {}    # last GroupState
var guild: Dictionary = {}    # last GuildInfo
var friends: Dictionary = {}  # last FriendsList
var who: Dictionary = {}      # last WhoResults
var invite: Dictionary = {}   # the SocialInvite on screen ({} = none)
var sent: Array[String] = []  # commands emitted (tests)
var selected_member: String = ""  # group window selection (kick / make leader)
var selected_guildie: String = ""

var _group_win: PanelContainer
var _group_title: Label
var _group_rows: VBoxContainer
var _loot_rule: OptionButton
var _group_buttons: Dictionary = {}
var _guild_win: PanelContainer
var _guild_title: Label
var _guild_motd: Label
var _guild_motd_edit: LineEdit
var _guild_motd_set: Button
var _guild_rows: VBoxContainer
var _guild_create: HBoxContainer
var _guild_create_name: LineEdit
var _guild_tools: HBoxContainer
var _guild_buttons: Dictionary = {}
var _friends_win: PanelContainer
var _friend_rows: VBoxContainer
var _ignore_rows: VBoxContainer
var _channel_checks: Dictionary = {}
var _friend_name: LineEdit
var _ignore_name: LineEdit
var _who_win: PanelContainer
var _who_title: Label
var _who_rows: VBoxContainer
var _invite_win: PanelContainer
var _invite_text: Label
var _invite_expires: float = 0.0


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_group()
	_build_guild()
	_build_friends()
	_build_who()
	_build_invite()
	set_group({"group_id": 0, "leader": "", "loot_rule": 0, "members": []})
	set_guild({"guild_id": 0, "name": "", "motd": "", "my_rank": 0, "ranks": [], "members": []})


static func place(c: Control, preset: Control.LayoutPreset, x: float, y: float) -> void:
	c.set_anchors_preset(preset)
	c.offset_left = x
	c.offset_top = y
	c.offset_right = x
	c.offset_bottom = y
	c.grow_horizontal = Control.GROW_DIRECTION_END
	c.grow_vertical = Control.GROW_DIRECTION_END


func _window(title_text: String, width: float, node_name: String) -> Array:
	var win: PanelContainer = PanelContainer.new()
	win.name = node_name
	win.visible = false
	win.mouse_filter = Control.MOUSE_FILTER_STOP
	win.custom_minimum_size = Vector2(width, 0.0)
	# Opaque enough to read when windows overlap (the /who window over the guild roster, say).
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.09, 0.08, 0.08, 0.94)
	style.border_color = Color(0.45, 0.32, 0.18, 0.9)
	style.set_border_width_all(1)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(8)
	win.add_theme_stylebox_override("panel", style)
	add_child(win)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	win.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	var title: Label = Label.new()
	title.text = title_text
	title.add_theme_font_size_override("font_size", 18)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(title)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(func() -> void: win.visible = false)
	head.add_child(close)
	return [win, box, title]


func _button(parent: Control, text: String, on_pressed: Callable) -> Button:
	var b: Button = Button.new()
	b.text = text
	b.pressed.connect(on_pressed)
	parent.add_child(b)
	return b


func _small(text: String, color: Color = Color(0.8, 0.8, 0.8)) -> Label:
	var l: Label = Label.new()
	l.text = text
	l.add_theme_font_size_override("font_size", 12)
	l.add_theme_color_override("font_color", color)
	return l


func _bar(parent: Control, fill: Color, value: int) -> ProgressBar:
	var b: ProgressBar = ProgressBar.new()
	b.custom_minimum_size = Vector2(0, 7)
	b.show_percentage = false
	b.max_value = 100
	b.value = value
	var bg: StyleBoxFlat = StyleBoxFlat.new()
	bg.bg_color = Color(0.1, 0.1, 0.1, 0.8)
	var fg: StyleBoxFlat = StyleBoxFlat.new()
	fg.bg_color = fill
	b.add_theme_stylebox_override("background", bg)
	b.add_theme_stylebox_override("fill", fg)
	parent.add_child(b)
	return b


func send_command(text: String) -> void:
	sent.append(text)
	command.emit(text)


# ------------------------------------------------------------------------------ group

func _build_group() -> void:
	var w: Array = _window("Group", 270.0, "GroupWindow")
	_group_win = w[0]
	_group_title = w[2]
	place(_group_win, Control.PRESET_TOP_LEFT, 12.0, 150.0)
	var box: VBoxContainer = w[1]
	_group_rows = VBoxContainer.new()
	_group_rows.add_theme_constant_override("separation", 4)
	box.add_child(_group_rows)
	box.add_child(HSeparator.new())
	var row1: HBoxContainer = HBoxContainer.new()
	box.add_child(row1)
	_group_buttons["invite"] = _button(row1, "Invite", func() -> void: send_command("invite"))
	_group_buttons["leave"] = _button(row1, "Leave", func() -> void: send_command("disband"))
	_group_buttons["kick"] = _button(row1, "Kick", func() -> void:
		if selected_member != "":
			send_command("kick " + selected_member))
	_group_buttons["leader"] = _button(row1, "Lead", func() -> void:
		if selected_member != "":
			send_command("makeleader " + selected_member))
	_loot_rule = OptionButton.new()
	for r: String in LOOT_RULES:
		_loot_rule.add_item(r)
	_loot_rule.item_selected.connect(func(i: int) -> void: send_command("lootrule " + ("rr" if i == 1 else "ng")))
	box.add_child(_loot_rule)


## A GroupState message.
func set_group(msg: Dictionary) -> void:
	var was_grouped: bool = int(group.get("group_id", 0)) != 0
	group = msg
	var grouped: bool = int(msg["group_id"]) != 0
	var leader: String = str(msg["leader"])
	var i_lead: bool = grouped and leader == my_name
	_group_title.text = "Group (%d)" % (msg["members"] as Array).size() if grouped else "Group"
	for c: Node in _group_rows.get_children():
		_group_rows.remove_child(c)
		c.queue_free()
	if not grouped:
		_group_rows.add_child(_small("You are not in a group. Target a player and press Invite (or /invite NAME)."))
		selected_member = ""
	for m: Variant in msg["members"]:
		_group_rows.add_child(_member_row(m as Dictionary, leader))
	_group_buttons["leave"].disabled = not grouped
	_group_buttons["kick"].disabled = not i_lead
	_group_buttons["leader"].disabled = not i_lead
	_group_buttons["invite"].disabled = grouped and not i_lead
	_loot_rule.disabled = not i_lead
	_loot_rule.select(clampi(int(msg["loot_rule"]), 0, 1))
	if grouped and not was_grouped:
		_group_win.visible = true  # joining a group opens its window


func _member_row(m: Dictionary, leader: String) -> Control:
	var who_name: String = str(m["name"])
	var row: PanelContainer = PanelContainer.new()
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.25, 0.2, 0.1, 0.6) if who_name == selected_member else Color(0.0, 0.0, 0.0, 0.3)
	style.set_content_margin_all(3)
	row.add_theme_stylebox_override("panel", style)
	row.mouse_filter = Control.MOUSE_FILTER_STOP
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 1)
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(box)
	var head: Label = Label.new()
	var tag: String = " (leader)" if who_name == leader else ""
	var lvl: int = int(m["level"])
	var where: String = ""
	if int(m["entity_id"]) == 0 and str(m["zone"]) != "":
		where = "  - " + str(m["zone"])
	# Phase 12: a member who left the world keeps their place, offline, for a while (no bars to show).
	var online: bool = bool(m.get("online", true))
	head.text = "%s%s  %s%s%s" % [who_name, tag, ("%d %s" % [lvl, str(m["class_id"]).capitalize()]) if lvl > 0 else "", where,
		"  (dead)" if bool(m["dead"]) else ("  (offline)" if not online else "")]
	head.add_theme_font_size_override("font_size", 13)
	head.mouse_filter = Control.MOUSE_FILTER_IGNORE
	if not online:
		head.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	box.add_child(head)
	if not online:
		return _member_click(row, who_name, 0)
	_bar(box, Color(0.8, 0.15, 0.15), int(m["hp_pct"])).mouse_filter = Control.MOUSE_FILTER_IGNORE
	if int(m["mana_pct"]) != 255:
		_bar(box, Color(0.2, 0.4, 0.95), int(m["mana_pct"])).mouse_filter = Control.MOUSE_FILTER_IGNORE
	_bar(box, Color(0.85, 0.75, 0.2), int(m["endurance_pct"])).mouse_filter = Control.MOUSE_FILTER_IGNORE
	return _member_click(row, who_name, int(m["entity_id"]))


## Clicking a member's row selects them (and targets them when they are in your zone).
func _member_click(row: PanelContainer, who_name: String, eid: int) -> Control:
	row.gui_input.connect(func(e: InputEvent) -> void:
		var mb: InputEventMouseButton = e as InputEventMouseButton
		if mb != null and mb.pressed and mb.button_index == MOUSE_BUTTON_LEFT:
			select_member(who_name)
			if eid != 0:
				target_requested.emit(eid))
	return row


func select_member(who_name: String) -> void:
	selected_member = who_name if who_name != my_name else ""
	set_group(group)


## The entity of the n-th other member (F2 = 0, F3 = 1, ...), or 0.
func member_entity(n: int) -> int:
	var others: Array = []
	for m: Variant in group.get("members", []):
		if str((m as Dictionary)["name"]) != my_name:
			others.append(m)
	if n < 0 or n >= others.size():
		return 0
	return int((others[n] as Dictionary)["entity_id"])


func toggle_group() -> void:
	_group_win.visible = not _group_win.visible


func group_visible() -> bool:
	return _group_win.visible


# ------------------------------------------------------------------------------ guild

func _build_guild() -> void:
	var w: Array = _window("Guild", 430.0, "GuildWindow")
	_guild_win = w[0]
	_guild_title = w[2]
	place(_guild_win, Control.PRESET_CENTER, -215.0, -230.0)
	var box: VBoxContainer = w[1]
	_guild_motd = Label.new()
	_guild_motd.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_guild_motd.custom_minimum_size = Vector2(410, 0)
	box.add_child(_guild_motd)
	var motd_row: HBoxContainer = HBoxContainer.new()
	box.add_child(motd_row)
	_guild_motd_edit = LineEdit.new()
	_guild_motd_edit.placeholder_text = "New message of the day"
	_guild_motd_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_guild_motd_edit.max_length = 200
	motd_row.add_child(_guild_motd_edit)
	_guild_motd_set = _button(motd_row, "Set", func() -> void:
		if _guild_motd_edit.text.strip_edges() != "":
			send_command("guildmotd " + _guild_motd_edit.text.strip_edges())
			_guild_motd_edit.clear())
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(410, 220)
	box.add_child(scroll)
	_guild_rows = VBoxContainer.new()
	_guild_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_guild_rows)
	_guild_tools = HBoxContainer.new()
	box.add_child(_guild_tools)
	_guild_buttons["invite"] = _button(_guild_tools, "Invite", func() -> void: send_command("guildinvite"))
	_guild_buttons["promote"] = _button(_guild_tools, "Promote", func() -> void: _on_guildie("guildpromote"))
	_guild_buttons["demote"] = _button(_guild_tools, "Demote", func() -> void: _on_guildie("guilddemote"))
	_guild_buttons["remove"] = _button(_guild_tools, "Remove", func() -> void: _on_guildie("guildremove"))
	_guild_buttons["leader"] = _button(_guild_tools, "Make leader", func() -> void: _on_guildie("guildleader"))
	var tools2: HBoxContainer = HBoxContainer.new()
	box.add_child(tools2)
	_guild_buttons["leave"] = _button(tools2, "Leave guild", func() -> void: send_command("guildleave"))
	_guild_buttons["disband"] = _button(tools2, "Disband", func() -> void: send_command("guilddisband"))
	_guild_create = HBoxContainer.new()
	box.add_child(_guild_create)
	_guild_create_name = LineEdit.new()
	_guild_create_name.placeholder_text = "Guild name"
	_guild_create_name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_guild_create_name.max_length = 24
	_guild_create.add_child(_guild_create_name)
	_button(_guild_create, "Found guild", func() -> void:
		if _guild_create_name.text.strip_edges() != "":
			send_command("guildcreate " + _guild_create_name.text.strip_edges()))


func _on_guildie(cmd: String) -> void:
	if selected_guildie != "":
		send_command("%s %s" % [cmd, selected_guildie])


func _perms() -> int:
	var ranks: Array = guild.get("ranks", []) as Array
	var r: int = int(guild.get("my_rank", 0))
	return int((ranks[r] as Dictionary)["permissions"]) if r >= 0 and r < ranks.size() else 0


func rank_name(rank: int) -> String:
	var ranks: Array = guild.get("ranks", []) as Array
	return str((ranks[rank] as Dictionary)["name"]) if rank >= 0 and rank < ranks.size() else "Member"


## A GuildInfo message.
func set_guild(msg: Dictionary) -> void:
	guild = msg
	var in_guild: bool = int(msg["guild_id"]) != 0
	var perms: int = _perms() if in_guild else 0
	var online: int = 0
	for m: Variant in msg["members"]:
		if bool((m as Dictionary)["online"]):
			online += 1
	_guild_title.text = "<%s>  %d members, %d online" % [str(msg["name"]), (msg["members"] as Array).size(), online] if in_guild else "Guild"
	_guild_motd.text = ("Message of the day: " + str(msg["motd"]) if str(msg["motd"]) != "" else "No message of the day.") if in_guild \
		else "You are not in a guild. Found one below, or accept an invitation."
	_guild_motd_edit.visible = in_guild and (perms & PERM_MOTD) != 0
	_guild_motd_set.visible = _guild_motd_edit.visible
	_guild_tools.visible = in_guild
	_guild_buttons["leave"].visible = in_guild
	_guild_buttons["disband"].visible = in_guild and (perms & PERM_DISBAND) != 0
	_guild_buttons["invite"].disabled = (perms & PERM_INVITE) == 0
	_guild_buttons["promote"].disabled = (perms & PERM_PROMOTE) == 0
	_guild_buttons["demote"].disabled = (perms & PERM_DEMOTE) == 0
	_guild_buttons["remove"].disabled = (perms & PERM_REMOVE) == 0
	_guild_buttons["leader"].disabled = int(msg["my_rank"]) != 0
	_guild_create.visible = not in_guild
	for c: Node in _guild_rows.get_children():
		_guild_rows.remove_child(c)
		c.queue_free()
	for m: Variant in msg["members"]:
		_guild_rows.add_child(_guildie_row(m as Dictionary))


func _guildie_row(m: Dictionary) -> Control:
	var who_name: String = str(m["name"])
	var b: Button = Button.new()
	b.toggle_mode = true
	b.button_pressed = who_name == selected_guildie
	b.alignment = HORIZONTAL_ALIGNMENT_LEFT
	var where: String = str(m["zone"]) if bool(m["online"]) else "offline"
	b.text = "%-15s  %-9s  %2d %-13s  %s" % [who_name, rank_name(int(m["rank"])), int(m["level"]), str(m["class_id"]).capitalize(), where]
	b.add_theme_font_size_override("font_size", 12)
	b.modulate = Color(1, 1, 1) if bool(m["online"]) else Color(0.6, 0.6, 0.6)
	b.pressed.connect(func() -> void:
		selected_guildie = who_name if selected_guildie != who_name else ""
		set_guild(guild))
	return b


func toggle_guild() -> void:
	_guild_win.visible = not _guild_win.visible


func guild_visible() -> bool:
	return _guild_win.visible


# ------------------------------------------------------------------------------ friends, ignore, channels

func _build_friends() -> void:
	var w: Array = _window("Friends", 300.0, "FriendsWindow")
	_friends_win = w[0]
	place(_friends_win, Control.PRESET_TOP_RIGHT, -312.0, 160.0)
	var box: VBoxContainer = w[1]
	var tabs: TabContainer = TabContainer.new()
	tabs.custom_minimum_size = Vector2(290, 260)
	box.add_child(tabs)
	var fbox: VBoxContainer = VBoxContainer.new()
	fbox.name = "Friends"
	tabs.add_child(fbox)
	_friend_rows = _scrolling_list(fbox)
	var fadd: HBoxContainer = HBoxContainer.new()
	fbox.add_child(fadd)
	_friend_name = LineEdit.new()
	_friend_name.placeholder_text = "Name"
	_friend_name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	fadd.add_child(_friend_name)
	_button(fadd, "Add", func() -> void:
		if _friend_name.text.strip_edges() != "":
			send_command("friend " + _friend_name.text.strip_edges())
			_friend_name.clear())
	var ibox: VBoxContainer = VBoxContainer.new()
	ibox.name = "Ignore"
	tabs.add_child(ibox)
	_ignore_rows = _scrolling_list(ibox)
	var iadd: HBoxContainer = HBoxContainer.new()
	ibox.add_child(iadd)
	_ignore_name = LineEdit.new()
	_ignore_name.placeholder_text = "Name"
	_ignore_name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	iadd.add_child(_ignore_name)
	_button(iadd, "Ignore", func() -> void:
		if _ignore_name.text.strip_edges() != "":
			send_command("ignore " + _ignore_name.text.strip_edges())
			_ignore_name.clear())
	var cbox: VBoxContainer = VBoxContainer.new()
	cbox.name = "Channels"
	tabs.add_child(cbox)
	cbox.add_child(_small("Channels you hear (the server stops sending the ones you turn off):"))
	for ch: Array in MUTABLE_CHANNELS:
		var c: CheckBox = CheckBox.new()
		c.text = str(ch[1])
		c.button_pressed = true
		var id: String = str(ch[0])
		c.toggled.connect(func(on: bool) -> void: send_command("channel %s %s" % [id, "on" if on else "off"]))
		cbox.add_child(c)
		_channel_checks[id] = c


## A list of rows in a scroll area of fixed height (up to 64 names fit without growing the window).
func _scrolling_list(parent: Control) -> VBoxContainer:
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(280, 190)
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	parent.add_child(scroll)
	var rows: VBoxContainer = VBoxContainer.new()
	rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(rows)
	return rows


## A FriendsList message.
func set_friends(msg: Dictionary) -> void:
	friends = msg
	for c: Node in _friend_rows.get_children():
		_friend_rows.remove_child(c)
		c.queue_free()
	for c: Node in _ignore_rows.get_children():
		_ignore_rows.remove_child(c)
		c.queue_free()
	if (msg["friends"] as Array).is_empty():
		_friend_rows.add_child(_small("No friends yet. Add one below (or /friend NAME)."))
	for f: Variant in msg["friends"]:
		var fd: Dictionary = f as Dictionary
		var row: HBoxContainer = HBoxContainer.new()
		var who_name: String = str(fd["name"])
		var l: Label = Label.new()
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		l.text = "%s  %s" % [who_name, ("%d %s, %s" % [int(fd["level"]), str(fd["class_id"]).capitalize(), str(fd["zone"])]) if bool(fd["online"]) else "offline"]
		l.add_theme_font_size_override("font_size", 12)
		l.modulate = Color(0.6, 1.0, 0.6) if bool(fd["online"]) else Color(0.65, 0.65, 0.65)
		row.add_child(l)
		if bool(fd["online"]):
			_button(row, "Tell", func() -> void: tell_requested.emit(who_name))
			_button(row, "Invite", func() -> void: send_command("invite " + who_name))
		_button(row, "x", func() -> void: send_command("friend " + who_name))
		_friend_rows.add_child(row)
	for n: Variant in msg["ignored"]:
		var row: HBoxContainer = HBoxContainer.new()
		var l: Label = Label.new()
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		l.text = str(n)
		row.add_child(l)
		var who_name: String = str(n)
		_button(row, "Unignore", func() -> void: send_command("ignore " + who_name))
		_ignore_rows.add_child(row)
	var muted: int = int(msg["muted_channels"])
	for id: String in _channel_checks:
		(_channel_checks[id] as CheckBox).set_pressed_no_signal((muted & (1 << int(CHANNEL_IDS[id]))) == 0)


func toggle_friends() -> void:
	_friends_win.visible = not _friends_win.visible


func friends_visible() -> bool:
	return _friends_win.visible


# ------------------------------------------------------------------------------ /who

func _build_who() -> void:
	var w: Array = _window("Who", 470.0, "WhoWindow")
	_who_win = w[0]
	_who_title = w[2]
	place(_who_win, Control.PRESET_CENTER_TOP, -235.0, 40.0)
	var box: VBoxContainer = w[1]
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(450, 260)
	box.add_child(scroll)
	_who_rows = VBoxContainer.new()
	_who_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_who_rows)
	box.add_child(_small("/who [all] [name] [class] [race] [10-20] [zone] [guild \"Name\"]"))


## A WhoResults message: opens the window with the matches.
func show_who(msg: Dictionary) -> void:
	who = msg
	var entries: Array = msg["entries"] as Array
	_who_title.text = "Who: %s  (%d%s)" % [str(msg["query"]), int(msg["total"]), " shown %d" % entries.size() if entries.size() < int(msg["total"]) else ""]
	for c: Node in _who_rows.get_children():
		_who_rows.remove_child(c)
		c.queue_free()
	if entries.is_empty():
		_who_rows.add_child(_small("Nobody matches."))
	for e: Variant in entries:
		var ed: Dictionary = e as Dictionary
		var row: HBoxContainer = HBoxContainer.new()
		var l: Label = Label.new()
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var g: String = " <%s>" % str(ed["guild"]) if str(ed["guild"]) != "" else ""
		l.text = "%s[%d %s] %s (%s)%s  %s%s" % ["*GM* " if bool(ed["gm"]) else "", int(ed["level"]), str(ed["class_id"]).capitalize(), str(ed["name"]),
			str(ed["race"]).capitalize(), g, str(ed["zone"]), "  LFG" if int(ed["group_size"]) == 0 else "  grouped"]
		l.add_theme_font_size_override("font_size", 12)
		row.add_child(l)
		var who_name: String = str(ed["name"])
		if who_name != my_name:
			_button(row, "Tell", func() -> void: tell_requested.emit(who_name))
			_button(row, "Invite", func() -> void: send_command("invite " + who_name))
		_who_rows.add_child(row)
	_who_win.visible = true


func who_visible() -> bool:
	return _who_win.visible


# ------------------------------------------------------------------------------ invitations

func _build_invite() -> void:
	var w: Array = _window("Invitation", 320.0, "InviteWindow")
	_invite_win = w[0]
	place(_invite_win, Control.PRESET_CENTER_TOP, -160.0, 90.0)
	var box: VBoxContainer = w[1]
	_invite_text = Label.new()
	_invite_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_invite_text.custom_minimum_size = Vector2(300, 0)
	box.add_child(_invite_text)
	var row: HBoxContainer = HBoxContainer.new()
	box.add_child(row)
	_button(row, "Accept", func() -> void: answer_invite(true))
	_button(row, "Decline", func() -> void: answer_invite(false))


## A SocialInvite message (kind 0 = group, 1 = guild).
func show_invite(msg: Dictionary) -> void:
	invite = msg
	_invite_expires = Time.get_ticks_msec() / 1000.0 + float(msg["expires_ms"]) / 1000.0
	if int(msg["kind"]) == 1:
		_invite_text.text = "%s invites you to join the guild <%s>." % [str(msg["from_name"]), str(msg["guild_name"])]
	else:
		_invite_text.text = "%s invites you to join a group." % str(msg["from_name"])
	_invite_win.visible = true


func answer_invite(accept: bool) -> void:
	if invite.is_empty():
		return
	var guild_invite: bool = int(invite["kind"]) == 1
	if guild_invite:
		send_command("guildjoin" if accept else "guilddecline")
	else:
		send_command("join" if accept else "decline")
	invite = {}
	_invite_win.visible = false


func invite_visible() -> bool:
	return _invite_win.visible


func _process(_delta: float) -> void:
	if _invite_win.visible and Time.get_ticks_msec() / 1000.0 > _invite_expires:
		invite = {}
		_invite_win.visible = false


# ------------------------------------------------------------------------------ HUD plumbing

## The invitation popup is not counted: Escape leaves it up (it has its own buttons and times out).
func any_window_open() -> bool:
	return _group_win.visible or _guild_win.visible or _friends_win.visible or _who_win.visible


func close_windows() -> void:
	for w: Control in [_group_win, _guild_win, _friends_win, _who_win]:
		w.visible = false


## True if every open window lies inside the HUD.
func windows_on_screen() -> bool:
	var view: Rect2 = get_global_rect().grow(1.0) if get_parent() is Control else get_viewport_rect().grow(1.0)
	for c: Control in [_group_win, _guild_win, _friends_win, _who_win, _invite_win]:
		if c.is_visible_in_tree() and not view.encloses(c.get_global_rect()):
			return false
	return true
