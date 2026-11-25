## Chat window with tabs: All (everything but the combat log, filtered per channel), Chat (say, shout,
## OOC, emotes), Group, Raid, Guild, Tells and Combat. Enter focuses the input; Esc cancels. Typing goes to
## the tab's channel (say on All and Chat, the group on Group, the raid on Raid, the guild on Guild, a reply
## on Tells), or to the one a prefix names: /say /s, /shout /sh, /ooc, /tell NAME /t, /reply /r, /gsay /g,
## /rsay /rs, /guildsay /gu, /emote /em /me. Any other line starting with "/" is a command, handed to the world via `command`.
extends PanelContainer

signal command(name: String, args: String)

const Proto := preload("res://scripts/net/protocol.gd")
## ChatMessage.channel values (server game/social_types.hpp ChatChannel).
const CHANNEL_SAY: int = 0
const CHANNEL_SYSTEM: int = 1
const CHANNEL_COMBAT: int = 2
const CHANNEL_SHOUT: int = 3
const CHANNEL_OOC: int = 4
const CHANNEL_TELL: int = 5
const CHANNEL_TELL_ECHO: int = 6
const CHANNEL_GROUP: int = 7
const CHANNEL_GUILD: int = 8
const CHANNEL_EMOTE: int = 9
const CHANNEL_GM: int = 10
const CHANNEL_RAID: int = 11
const COLORS: Dictionary = {
	CHANNEL_SAY: Color(0.95, 0.95, 0.95), CHANNEL_SYSTEM: Color(1.0, 0.82, 0.4), CHANNEL_SHOUT: Color(1.0, 0.45, 0.4),
	CHANNEL_OOC: Color(0.55, 0.85, 0.55), CHANNEL_TELL: Color(0.85, 0.55, 1.0), CHANNEL_TELL_ECHO: Color(0.85, 0.55, 1.0),
	CHANNEL_GROUP: Color(0.45, 0.75, 1.0), CHANNEL_GUILD: Color(0.45, 1.0, 0.6), CHANNEL_EMOTE: Color(0.85, 0.85, 0.65),
	CHANNEL_GM: Color(1.0, 0.6, 0.15), CHANNEL_RAID: Color(1.0, 0.55, 0.25),
}
## Tab name -> the channels it shows ([] = all but combat). The All tab also honours `filters`.
const TABS: Array = [
	["All", []], ["Chat", [0, 3, 4, 9]], ["Group", [7]], ["Raid", [11]], ["Guild", [8]], ["Tells", [5, 6]], ["Combat", [2]],
]
## Channels the All tab can hide (name shown in the filter menu).
const FILTERABLE: Array = [[0, "Say"], [3, "Shout"], [4, "OOC"], [9, "Emotes"], [7, "Group"], [11, "Raid"], [8, "Guild"], [5, "Tells"], [1, "System"]]
## Prefixes typed before a line: prefix -> channel. Tells take a name first.
const PREFIXES: Dictionary = {
	"say": CHANNEL_SAY, "s": CHANNEL_SAY, "shout": CHANNEL_SHOUT, "sh": CHANNEL_SHOUT, "ooc": CHANNEL_OOC,
	"tell": CHANNEL_TELL, "t": CHANNEL_TELL, "msg": CHANNEL_TELL, "gsay": CHANNEL_GROUP, "g": CHANNEL_GROUP,
	"rsay": CHANNEL_RAID, "rs": CHANNEL_RAID, "guildsay": CHANNEL_GUILD, "gu": CHANNEL_GUILD, "emote": CHANNEL_EMOTE, "em": CHANNEL_EMOTE, "me": CHANNEL_EMOTE,
}
const LIMIT: int = 400
const SETTINGS_PATH: String = "user://chat.cfg"

var my_name: String = ""
var lines: Array[String] = []         # every non-combat line, for tests and the autotest report
var combat_lines: Array[String] = []  # combat tab
var last_tell_from: String = ""
## Who a reply goes to: fixed when typing starts, so a tell arriving mid-sentence cannot redirect it
## (not even when nobody had sent one yet).
var _reply_to: String = ""
var _reply_fixed: bool = false
var filters: Dictionary = {}          # channel -> false when the All tab hides it
var sent: Array[Dictionary] = []      # ChatSend messages built (tests read these instead of the network)
var offline: bool = false             # tests: build messages without sending them
var _tabs: TabContainer
var _logs: Array[RichTextLabel] = []
var _input: LineEdit
var _filter_menu: PopupMenu


func _ready() -> void:
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.0, 0.0, 0.0, 0.45)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(6)
	add_theme_stylebox_override("panel", style)
	_load_filters()
	var box: VBoxContainer = VBoxContainer.new()
	add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	_tabs = TabContainer.new()
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for t: Array in TABS:
		_logs.append(_make_log(str(t[0])))
	box.add_child(_tabs)
	_tabs.tab_changed.connect(func(_i: int) -> void: _input.placeholder_text = _placeholder())
	var filter_button: MenuButton = MenuButton.new()
	filter_button.text = "Filter"
	filter_button.tooltip_text = "Channels the All tab shows"
	filter_button.flat = false
	head.add_child(filter_button)
	var spacer: Control = Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(spacer)
	head.move_child(filter_button, 1)
	_filter_menu = filter_button.get_popup()
	for i: int in FILTERABLE.size():
		var f: Array = FILTERABLE[i]
		_filter_menu.add_check_item(str(f[1]), int(f[0]))
		_filter_menu.set_item_checked(i, shows(int(f[0])))
	_filter_menu.id_pressed.connect(_on_filter)
	_input = LineEdit.new()
	_input.placeholder_text = _placeholder()
	_input.max_length = 240
	_input.text_submitted.connect(_on_submit)
	_input.gui_input.connect(_on_input_gui)
	_input.focus_entered.connect(func() -> void:
		_reply_to = last_tell_from
		_reply_fixed = true
		_input.placeholder_text = _placeholder())
	_input.focus_exited.connect(func() -> void:
		_reply_to = ""
		_reply_fixed = false)
	box.add_child(_input)


func _make_log(title: String) -> RichTextLabel:
	var l: RichTextLabel = RichTextLabel.new()
	l.name = title
	l.scroll_following = true
	l.size_flags_vertical = Control.SIZE_EXPAND_FILL
	l.mouse_filter = Control.MOUSE_FILTER_PASS
	_tabs.add_child(l)
	return l


func _placeholder() -> String:
	match current_tab_name():
		"Group":
			return "Enter: speak to your group (/g)"
		"Raid":
			return "Enter: speak to your raid (/rs)"
		"Guild":
			return "Enter: speak to your guild (/gu)"
		"Tells":
			return "Enter: reply to %s" % reply_target() if reply_target() != "" else "/tell NAME message"
		"Combat":
			return "Enter: say, or /command"
	return "Enter: say. /shout /ooc /tell NAME /g /gu /me, or /help"


## Whom /reply (and plain text on the Tells tab) answers right now.
func reply_target() -> String:
	return _reply_to if _reply_fixed else last_tell_from


func current_tab_name() -> String:
	return str(TABS[_tabs.current_tab][0]) if _tabs != null else "All"


## True when the All tab shows `channel`.
func shows(channel: int) -> bool:
	return bool(filters.get(channel, true))


func set_filter(channel: int, on: bool) -> void:
	filters[channel] = on
	for i: int in FILTERABLE.size():
		if int(FILTERABLE[i][0]) == channel and _filter_menu != null:
			_filter_menu.set_item_checked(i, on)
	_save_filters()


func _on_filter(id: int) -> void:
	set_filter(id, not shows(id))


func _load_filters() -> void:
	var cfg: ConfigFile = ConfigFile.new()
	if cfg.load(SETTINGS_PATH) != OK:
		return
	for f: Array in FILTERABLE:
		filters[int(f[0])] = bool(cfg.get_value("all_tab", str(f[1]).to_lower(), true))


func _save_filters() -> void:
	if offline:
		return
	var cfg: ConfigFile = ConfigFile.new()
	for f: Array in FILTERABLE:
		cfg.set_value("all_tab", str(f[1]).to_lower(), shows(int(f[0])))
	cfg.save(SETTINGS_PATH)


func is_typing() -> bool:
	return _input.has_focus()


func focus_input(prefill: String = "") -> void:
	_input.grab_focus()
	if prefill != "":
		_input.text = prefill
		_input.caret_column = prefill.length()


## A typed line: a channel prefix, a command, or plain text for the current tab's channel.
func say(text: String) -> void:
	var t: String = text.strip_edges()
	if t == "":
		return
	if t.begins_with("/"):
		var parts: PackedStringArray = t.substr(1).split(" ", false, 1)
		if parts.size() == 0:
			return
		var word: String = parts[0].to_lower()
		var rest: String = parts[1] if parts.size() > 1 else ""
		if PREFIXES.has(word):
			var ch: int = int(PREFIXES[word])
			if ch == CHANNEL_TELL:
				var who: PackedStringArray = rest.split(" ", false, 1)
				if who.size() < 2:
					add_system("Usage: /tell NAME message")
					return
				_send(ch, who[0], who[1])
			else:
				_send(ch, "", rest)
			return
		if word == "r" or word == "reply":
			if reply_target() == "":
				add_system("Nobody has sent you a tell to reply to.")
				return
			_send(CHANNEL_TELL, reply_target(), rest)
			return
		command.emit(word, rest)
		return
	match current_tab_name():
		"Group":
			_send(CHANNEL_GROUP, "", t)
		"Raid":
			_send(CHANNEL_RAID, "", t)
		"Guild":
			_send(CHANNEL_GUILD, "", t)
		"Tells":
			if reply_target() == "":
				add_system("Nobody to reply to: /tell NAME message")
			else:
				_send(CHANNEL_TELL, reply_target(), t)
		_:
			_send(CHANNEL_SAY, "", t)


func _send(channel: int, target: String, text: String) -> void:
	if text.strip_edges() == "":
		return
	var m: Dictionary = {"channel": channel, "target": target, "text": text}
	sent.append(m)
	if not offline:
		Net.send(Proto.encode_chat_send(m))


## The line a channel message reads as.
func format_line(channel: int, from_name: String, text: String) -> String:
	var mine: bool = from_name == my_name and my_name != ""
	match channel:
		CHANNEL_SAY:
			return "You say, '%s'" % text if mine else "%s says, '%s'" % [from_name, text]
		CHANNEL_SHOUT:
			return "You shout, '%s'" % text if mine else "%s shouts, '%s'" % [from_name, text]
		CHANNEL_OOC:
			return "You say out of character, '%s'" % text if mine else "%s says out of character, '%s'" % [from_name, text]
		CHANNEL_TELL:
			return "%s tells you, '%s'" % [from_name, text]
		CHANNEL_TELL_ECHO:
			return "You told %s, '%s'" % [from_name, text]
		CHANNEL_GROUP:
			if from_name == "":
				return text
			return "You tell your group, '%s'" % text if mine else "%s tells the group, '%s'" % [from_name, text]
		CHANNEL_GUILD:
			if from_name == "":
				return text
			return "You say to your guild, '%s'" % text if mine else "%s tells the guild, '%s'" % [from_name, text]
		CHANNEL_EMOTE:
			return "%s %s" % [from_name, text]
		CHANNEL_GM:
			return "[GM] %s: %s" % [from_name, text]
		CHANNEL_RAID:
			if from_name == "":
				return text
			return "You tell your raid, '%s'" % text if mine else "%s tells the raid, '%s'" % [from_name, text]
	return text


func add_message(channel: int, from_name: String, text: String) -> void:
	if channel == CHANNEL_COMBAT:
		add_combat(text, Color(0.85, 0.85, 0.7))
		return
	if channel == CHANNEL_TELL and from_name != "":
		last_tell_from = from_name
		if _input != null and not _input.has_focus():
			_input.placeholder_text = _placeholder()
	var line: String = format_line(channel, from_name, text)
	var color: Color = COLORS.get(channel, COLORS[CHANNEL_SYSTEM]) as Color
	for i: int in TABS.size():
		var chans: Array = TABS[i][1] as Array
		var here: bool = (chans.is_empty() and channel != CHANNEL_COMBAT and shows(channel)) or chans.has(channel)
		# Tells (the echo too) follow the Tells filter on the All tab.
		if chans.is_empty() and channel == CHANNEL_TELL_ECHO:
			here = shows(CHANNEL_TELL)
		if chans.is_empty() and channel == CHANNEL_GM:
			here = true  # game master notices always show
		if here:
			_append(_logs[i], line, color)
	lines.append(line)
	if lines.size() > LIMIT * 4:
		lines.pop_front()


## The text of a tab, for tests.
func tab_text(tab: String) -> String:
	for i: int in TABS.size():
		if str(TABS[i][0]) == tab:
			return _logs[i].get_parsed_text()
	return ""


func _append(into: RichTextLabel, line: String, color: Color) -> void:
	into.push_color(color)
	into.add_text(line)
	into.pop()
	into.newline()
	if into.get_paragraph_count() > LIMIT:
		into.remove_paragraph(0)


func add_system(text: String) -> void:
	add_message(CHANNEL_SYSTEM, "", text)


func add_combat(text: String, color: Color) -> void:
	_append(_logs[TABS.size() - 1], text, color)
	combat_lines.append(text)
	if combat_lines.size() > LIMIT:
		combat_lines.pop_front()


func show_tab(index: int) -> void:
	_tabs.current_tab = index


## Selects a tab by name ("All", "Chat", "Group", "Raid", "Guild", "Tells", "Combat").
func show_tab_named(tab: String) -> void:
	for i: int in TABS.size():
		if str(TABS[i][0]) == tab:
			_tabs.current_tab = i


func _on_submit(text: String) -> void:
	say(text)
	_input.clear()
	_input.release_focus()
	_reply_to = ""
	_reply_fixed = false


func _on_input_gui(event: InputEvent) -> void:
	var k: InputEventKey = event as InputEventKey
	if k != null and k.pressed and k.keycode == KEY_ESCAPE:
		_input.clear()
		_input.release_focus()
		_input.accept_event()
