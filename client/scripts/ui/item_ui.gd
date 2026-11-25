## Item windows: inventory (paper doll, stats, pack, bags), bank, vendor, trade, auction board, loot
## and need/greed rolls. Drag items between squares to move them; right-click to use, equip, sell
## (vendor open) or bank (bank open). Pure presentation: every action is a signal the world turns into
## a server intent, and every window redraws from what the server sends back.
extends Control

const UI := preload("res://scripts/ui/ui.gd")
const ItemDb := preload("res://scripts/ui/item_db.gd")
const ItemSlot := preload("res://scripts/ui/item_slot.gd")

signal move_item(from_code: int, to_code: int, count: int)
signal destroy_item(code: int, item_id: String)
signal use_item(code: int)
signal vendor_buy(npc_id: int, item_id: String, count: int, buyback: bool)
signal vendor_sell(npc_id: int, code: int, count: int)
signal bank_coin(deposit: bool, amount: int)
signal trade_respond(from_id: int, accept: bool)
signal trade_set_item(index: int, code: int)
signal trade_set_coin(coin: int)
signal trade_accept(accept: bool, trade_id: int, revision: int)
signal trade_cancel
signal auction_search(npc_id: int, text: String, kind: String, min_level: int, max_level: int, page: int, mine: bool)
signal auction_post(npc_id: int, code: int, item_id: String, price: int)
signal auction_buy(npc_id: int, listing_id: int, price: int)
signal auction_cancel(npc_id: int, listing_id: int)
signal loot_take(index: int)
signal loot_close
signal roll_choice(roll_id: int, choice: int)
signal notice(text: String)

const LOOT_ALL: int = 255
const KINDS: Array[String] = ["", "weapon", "armor", "shield", "jewelry", "ammo", "consumable", "bag", "scroll", "loot", "tradeskill"]

var items: Dictionary = {}  # slot code -> {item_id, count, expires_ms}
var coin: int = 0
var bank_coin_amount: int = 0
var bank_slots: int = 16
var sheet: Dictionary = {}
var vendor: Dictionary = {}    # the open VendorWindow ({} = closed)
var bank_npc: int = 0
var trade: Dictionary = {}     # the open TradeState
var invite: Dictionary = {}    # a pending TradeInvite
var auction: Dictionary = {}   # the open AuctionWindow
var auction_results: Dictionary = {}
var loot: Dictionary = {}      # the open LootWindow
var rolls: Dictionary = {}     # roll_id -> LootRoll
var _auction_mine: bool = false
var _auction_page: int = 0
var _post_code: int = ItemDb.NO_SLOT
var _post_item: String = ""

var _inv_win: PanelContainer
var _doll: GridContainer
var _stats: Label
var _pack: HBoxContainer
var _bags: VBoxContainer
var _coin_label: Label
var _bank_win: PanelContainer
var _bank_grid: GridContainer
var _bank_bags: VBoxContainer
var _bank_coin_label: Label
var _bank_amount: LineEdit
var _vendor_win: PanelContainer
var _vendor_title: Label
var _vendor_list: VBoxContainer
var _vendor_note: Label
var _trade_win: PanelContainer
var _trade_title: Label
var _trade_mine: GridContainer
var _trade_theirs: GridContainer
var _trade_my_coin: LineEdit
var _trade_status: Label
var _invite_win: PanelContainer
var _invite_text: Label
var _auction_win: PanelContainer
var _auction_title: Label
var _auction_text: LineEdit
var _auction_kind: OptionButton
var _auction_min: SpinBox
var _auction_max: SpinBox
var _auction_list: VBoxContainer
var _auction_page_label: Label
var _auction_post_slot: Control
var _auction_price: LineEdit
var _loot_win: PanelContainer
var _loot_title: Label
var _loot_list: VBoxContainer
var _rolls_box: VBoxContainer
var _confirm: ConfirmationDialog
var _pending_destroy: Dictionary = {}


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_inventory()
	_build_bank()
	_build_vendor()
	_build_trade()
	_build_auction()
	_build_loot()
	_rolls_box = VBoxContainer.new()
	_rolls_box.position = Vector2(470, 120)
	_rolls_box.add_theme_constant_override("separation", 6)
	add_child(_rolls_box)
	_confirm = ConfirmationDialog.new()
	_confirm.dialog_text = ""
	_confirm.confirmed.connect(_on_destroy_confirmed)
	add_child(_confirm)


# ------------------------------------------------------------------------------ construction

func _panel(alpha: float = 0.88) -> PanelContainer:
	var p: PanelContainer = PanelContainer.new()
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = Color(0.05, 0.04, 0.04, alpha)
	s.border_color = Color(0.4, 0.3, 0.22)
	s.set_border_width_all(1)
	s.set_corner_radius_all(4)
	s.set_content_margin_all(8)
	p.add_theme_stylebox_override("panel", s)
	add_child(p)
	return p


func _window(title: String, pos: Vector2, on_close: Callable) -> Array:
	var w: PanelContainer = _panel()
	w.position = pos
	w.visible = false
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	w.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	var t: Label = Label.new()
	t.text = title
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	t.add_theme_color_override("font_color", UI.ACCENT)
	head.add_child(t)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(on_close)
	head.add_child(close)
	return [w, box, t]


func _slot(role: String, slot_code: int, label: String = "") -> Control:
	var s: Panel = ItemSlot.new()
	s.set("role", role)
	s.set("code", slot_code)
	s.set("label_text", label)
	s.connect("dropped", _on_dropped)
	s.connect("used", _on_used)
	return s


func _build_inventory() -> void:
	var parts: Array = _window("Inventory (I) - drag to move, right-click to use or equip", Vector2(700, 40), func() -> void: _inv_win.visible = false)
	_inv_win = parts[0]
	var box: VBoxContainer = parts[1]
	var top: HBoxContainer = HBoxContainer.new()
	top.add_theme_constant_override("separation", 12)
	box.add_child(top)
	_doll = GridContainer.new()
	_doll.columns = 6
	top.add_child(_doll)
	_stats = Label.new()
	_stats.custom_minimum_size = Vector2(190, 0)
	_stats.add_theme_font_size_override("font_size", 12)
	top.add_child(_stats)
	UI.label(box, "Pack")
	_pack = HBoxContainer.new()
	box.add_child(_pack)
	_bags = VBoxContainer.new()
	box.add_child(_bags)
	var bottom: HBoxContainer = HBoxContainer.new()
	bottom.add_theme_constant_override("separation", 10)
	box.add_child(bottom)
	_coin_label = Label.new()
	_coin_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bottom.add_child(_coin_label)
	var trash: Control = _slot("destroy", ItemDb.NO_SLOT, "Destroy")
	trash.set("draggable", false)
	bottom.add_child(trash)
	_rebuild_inventory()


func _build_bank() -> void:
	var parts: Array = _window("Bank", Vector2(40, 40), func() -> void: _bank_win.visible = false)
	_bank_win = parts[0]
	var box: VBoxContainer = parts[1]
	UI.label(box, "Drag items between your pack and the bank; right-click moves them across.")
	_bank_grid = GridContainer.new()
	_bank_grid.columns = 8
	box.add_child(_bank_grid)
	_bank_bags = VBoxContainer.new()
	box.add_child(_bank_bags)
	_bank_coin_label = Label.new()
	box.add_child(_bank_coin_label)
	var row: HBoxContainer = HBoxContainer.new()
	box.add_child(row)
	_bank_amount = LineEdit.new()
	_bank_amount.placeholder_text = "amount (e.g. 2p 5g or 250)"
	_bank_amount.custom_minimum_size = Vector2(200, 0)
	row.add_child(_bank_amount)
	UI.button(row, "Deposit", func() -> void: _bank_move(true))
	UI.button(row, "Withdraw", func() -> void: _bank_move(false))


func _build_vendor() -> void:
	var parts: Array = _window("Merchant", Vector2(40, 40), func() -> void: _close_vendor())
	_vendor_win = parts[0]
	_vendor_title = parts[2]
	var box: VBoxContainer = parts[1]
	_vendor_note = UI.label(box, "")
	_vendor_note.custom_minimum_size = Vector2(440, 0)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(450, 380)
	box.add_child(scroll)
	_vendor_list = VBoxContainer.new()
	_vendor_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_vendor_list)
	var row: HBoxContainer = HBoxContainer.new()
	box.add_child(row)
	var sell: Control = _slot("sell", ItemDb.NO_SLOT, "Sell")
	sell.set("draggable", false)
	row.add_child(sell)
	UI.label(row, "Drop an item here, or right-click it in your pack, to sell it.")


func _build_trade() -> void:
	var parts: Array = _window("Trade", Vector2(40, 420), func() -> void: trade_cancel.emit())
	_trade_win = parts[0]
	_trade_title = parts[2]
	var box: VBoxContainer = parts[1]
	UI.label(box, "Your offer (drag items here):")
	_trade_mine = GridContainer.new()
	_trade_mine.columns = 8
	box.add_child(_trade_mine)
	for i: int in range(8):
		var s: Control = _slot("trade", ItemDb.NO_SLOT, "")
		s.set_meta("index", i)
		s.set("draggable", false)
		_trade_mine.add_child(s)
	var crow: HBoxContainer = HBoxContainer.new()
	box.add_child(crow)
	_trade_my_coin = LineEdit.new()
	_trade_my_coin.placeholder_text = "coin to give"
	_trade_my_coin.custom_minimum_size = Vector2(160, 0)
	crow.add_child(_trade_my_coin)
	UI.button(crow, "Set coin", _set_trade_coin)
	UI.label(box, "Their offer:")
	_trade_theirs = GridContainer.new()
	_trade_theirs.columns = 8
	box.add_child(_trade_theirs)
	for i: int in range(8):
		var s: Control = _slot("theirs", ItemDb.NO_SLOT, "")
		s.set("accept_drops", false)
		s.set("draggable", false)
		_trade_theirs.add_child(s)
	_trade_status = UI.label(box, "")
	var row: HBoxContainer = HBoxContainer.new()
	box.add_child(row)
	UI.button(row, "Accept", func() -> void: trade_accept.emit(true, int(trade.get("trade_id", 0)), int(trade.get("revision", 0))))
	UI.button(row, "Cancel", func() -> void: trade_cancel.emit())
	_invite_win = _panel(0.92)
	_invite_win.position = Vector2(470, 200)
	_invite_win.visible = false
	var ib: VBoxContainer = VBoxContainer.new()
	_invite_win.add_child(ib)
	_invite_text = UI.label(ib, "")
	var irow: HBoxContainer = HBoxContainer.new()
	ib.add_child(irow)
	UI.button(irow, "Trade", func() -> void: _answer_invite(true))
	UI.button(irow, "Decline", func() -> void: _answer_invite(false))


func _build_auction() -> void:
	var parts: Array = _window("Auction board", Vector2(40, 40), func() -> void: _auction_win.visible = false)
	_auction_win = parts[0]
	_auction_title = parts[2]
	var box: VBoxContainer = parts[1]
	var row: HBoxContainer = HBoxContainer.new()
	box.add_child(row)
	_auction_text = LineEdit.new()
	_auction_text.placeholder_text = "name contains..."
	_auction_text.custom_minimum_size = Vector2(130, 0)
	row.add_child(_auction_text)
	_auction_kind = OptionButton.new()
	for k: String in KINDS:
		_auction_kind.add_item("any kind" if k == "" else k)
	row.add_child(_auction_kind)
	var lv: Label = Label.new()
	lv.text = "levels"
	row.add_child(lv)
	_auction_min = SpinBox.new()
	_auction_min.max_value = 60
	_auction_min.custom_minimum_size = Vector2(60, 0)
	row.add_child(_auction_min)
	_auction_max = SpinBox.new()
	_auction_max.max_value = 60
	_auction_max.value = 60
	_auction_max.custom_minimum_size = Vector2(60, 0)
	row.add_child(_auction_max)
	(UI.button(row, "Search", func() -> void: _search(false, 0)) as Button).custom_minimum_size = Vector2(80, 32)
	(UI.button(row, "My listings", func() -> void: _search(true, 0)) as Button).custom_minimum_size = Vector2(100, 32)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(580, 320)
	box.add_child(scroll)
	_auction_list = VBoxContainer.new()
	_auction_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_auction_list)
	var nav: HBoxContainer = HBoxContainer.new()
	box.add_child(nav)
	UI.button(nav, "< Prev", func() -> void: _search(_auction_mine, maxi(0, _auction_page - 1)))
	_auction_page_label = Label.new()
	nav.add_child(_auction_page_label)
	UI.button(nav, "Next >", func() -> void: _search(_auction_mine, _auction_page + 1))
	var post: HBoxContainer = HBoxContainer.new()
	post.add_theme_constant_override("separation", 8)
	box.add_child(post)
	_auction_post_slot = _slot("post", ItemDb.NO_SLOT, "Post")
	_auction_post_slot.set("draggable", false)
	post.add_child(_auction_post_slot)
	_auction_price = LineEdit.new()
	_auction_price.placeholder_text = "price (e.g. 1p 5g)"
	_auction_price.custom_minimum_size = Vector2(160, 0)
	post.add_child(_auction_price)
	UI.button(post, "List it", _post)


func _build_loot() -> void:
	var parts: Array = _window("Loot", Vector2(440, 300), func() -> void: loot_close.emit())
	_loot_win = parts[0]
	_loot_title = parts[2]
	_loot_list = VBoxContainer.new()
	(parts[1] as VBoxContainer).add_child(_loot_list)
	var row: HBoxContainer = HBoxContainer.new()
	(parts[1] as VBoxContainer).add_child(row)
	UI.button(row, "Take all", func() -> void: loot_take.emit(LOOT_ALL))
	UI.button(row, "Close", func() -> void: loot_close.emit())


# ------------------------------------------------------------------------------ helpers

## "2p 5g 3s 1c", "2.5p" or plain copper -> copper. Anything malformed ("5gg", "-3", "abc") is -1, so
## a typo never turns into a different amount; an empty field is 0.
static func parse_coin(text: String) -> int:
	var t: String = text.strip_edges().to_lower().replace(",", "")
	if t == "":
		return 0
	if t.is_valid_int():
		return t.to_int() if t.to_int() >= 0 else -1
	var total: float = 0.0
	var mult: Dictionary = {"p": 1000.0, "g": 100.0, "s": 10.0, "c": 1.0}
	for part: String in t.split(" ", false):
		var unit: String = part.right(1)
		var num: String = part.left(part.length() - 1)
		if not mult.has(unit) or not num.is_valid_float() or num.to_float() < 0.0:
			return -1
		total += num.to_float() * float(mult[unit])
	return int(round(total))


func _bank_move(deposit: bool) -> void:
	var amount: int = parse_coin(_bank_amount.text)
	if amount <= 0:
		notice.emit("Enter an amount like 2p 5g 3s or 250 (copper).")
		return
	bank_coin.emit(deposit, amount)


func _set_trade_coin() -> void:
	var amount: int = parse_coin(_trade_my_coin.text)
	if amount < 0:
		notice.emit("Enter an amount like 2p 5g 3s or 250 (copper); 0 offers none.")
		return
	trade_set_coin.emit(amount)


func worn_by_index() -> Dictionary:
	var out: Dictionary = {}
	for i: int in range(ItemDb.EQUIP_NAMES.size()):
		var c: int = ItemDb.code(ItemDb.CONT_EQUIP, i)
		if items.has(c):
			out[i] = str((items[c] as Dictionary)["item_id"])
	return out


func _stack(c: int) -> Dictionary:
	return items.get(c, {}) as Dictionary


func _fill(s: Control, c: int) -> void:
	var st: Dictionary = _stack(c)
	if st.is_empty():
		s.call("clear")
		return
	var id: String = str(st["item_id"])
	var worn: Dictionary = {}
	if ItemDb.container_of(c) != ItemDb.CONT_EQUIP:
		worn = ItemDb.comparison_item(ItemDb.item(id), worn_by_index())
	s.call("set_item", id, int(st["count"]), worn)


## The first empty pack or pack-bag slot (bags excluded from bags), or NO_SLOT.
func free_pack_slot(for_item: String) -> int:
	for g: int in range(ItemDb.GENERAL_SLOTS):
		var c: int = ItemDb.code(ItemDb.CONT_GENERAL, g)
		if not items.has(c):
			return c
	if ItemDb.is_bag(for_item):
		return ItemDb.NO_SLOT
	for g: int in range(ItemDb.GENERAL_SLOTS):
		var parent: int = ItemDb.code(ItemDb.CONT_GENERAL, g)
		var cap: int = ItemDb.bag_slots(str(_stack(parent).get("item_id", "")))
		for i: int in range(cap):
			var c: int = ItemDb.code(ItemDb.bag_container(parent), i)
			if not items.has(c):
				return c
	return ItemDb.NO_SLOT


func free_bank_slot(for_item: String) -> int:
	for b: int in range(bank_slots):
		var c: int = ItemDb.code(ItemDb.CONT_BANK, b)
		if not items.has(c):
			return c
	if ItemDb.is_bag(for_item):
		return ItemDb.NO_SLOT
	for b: int in range(bank_slots):
		var parent: int = ItemDb.code(ItemDb.CONT_BANK, b)
		var cap: int = ItemDb.bag_slots(str(_stack(parent).get("item_id", "")))
		for i: int in range(cap):
			var c: int = ItemDb.code(ItemDb.bag_container(parent), i)
			if not items.has(c):
				return c
	return ItemDb.NO_SLOT


## The first slot holding an item (pack, bags, worn), or NO_SLOT.
func find_item(id: String, include_worn: bool = true) -> int:
	var keys: Array = items.keys()
	keys.sort()
	for c: Variant in keys:
		var code_i: int = int(c)
		if not include_worn and ItemDb.container_of(code_i) == ItemDb.CONT_EQUIP:
			continue
		if ItemDb.in_bank(code_i):
			continue
		if str((items[c] as Dictionary)["item_id"]) == id:
			return code_i
	return ItemDb.NO_SLOT


func count_of(id: String) -> int:
	var n: int = 0
	for st: Variant in items.values():
		if str((st as Dictionary)["item_id"]) == id:
			n += int((st as Dictionary)["count"])
	return n


# ------------------------------------------------------------------------------ server state

func set_inventory(inv: Dictionary) -> void:
	items.clear()
	for it: Dictionary in inv["items"]:
		items[int(it["slot"])] = {"item_id": str(it["item_id"]), "count": int(it["count"]), "expires_ms": int(it.get("expires_ms", 0))}
	coin = int(inv["coin"])
	bank_coin_amount = int(inv.get("bank_coin", 0))
	bank_slots = maxi(1, int(inv.get("bank_slots", 16)))
	_rebuild_inventory()
	# The square staged for an auction listing only means that item: forget it if its slot changed.
	if _post_code != ItemDb.NO_SLOT and str(_stack(_post_code).get("item_id", "")) != _post_item:
		_post_code = ItemDb.NO_SLOT
		_post_item = ""
		_auction_post_slot.call("clear")
	if _bank_win.visible:
		_rebuild_bank()
	if not trade.is_empty():
		_rebuild_trade()


func set_sheet(s: Dictionary) -> void:
	sheet = s
	_rebuild_stats()


func _rebuild_inventory() -> void:
	for c: Node in _doll.get_children():
		c.queue_free()
	# Paper doll in a body-like order: head to feet down the sides, weapons along the bottom.
	var order: Array[int] = [0, 1, 2, 19, 20, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18]
	for i: int in order:
		var c: int = ItemDb.code(ItemDb.CONT_EQUIP, i)
		var s: Control = _slot("equip", c, ItemDb.EQUIP_LABELS[i])
		_doll.add_child(s)
		_fill(s, c)
	for c: Node in _pack.get_children():
		c.queue_free()
	for g: int in range(ItemDb.GENERAL_SLOTS):
		var c: int = ItemDb.code(ItemDb.CONT_GENERAL, g)
		var s: Control = _slot("inv", c, "")
		_pack.add_child(s)
		_fill(s, c)
	_rebuild_bags(_bags, ItemDb.CONT_GENERAL, ItemDb.GENERAL_SLOTS)
	_coin_label.text = "Coin: %s" % ItemDb.format_coin(coin)
	_rebuild_stats()


func _rebuild_bags(holder: VBoxContainer, container: int, slots: int) -> void:
	for c: Node in holder.get_children():
		c.queue_free()
	for g: int in range(slots):
		var parent: int = ItemDb.code(container, g)
		var id: String = str(_stack(parent).get("item_id", ""))
		var cap: int = ItemDb.bag_slots(id)
		if cap <= 0:
			continue
		var row: HBoxContainer = HBoxContainer.new()
		holder.add_child(row)
		var l: Label = Label.new()
		l.text = ItemDb.name_of(id)
		l.custom_minimum_size = Vector2(110, 0)
		l.add_theme_font_size_override("font_size", 11)
		row.add_child(l)
		var grid: GridContainer = GridContainer.new()
		grid.columns = 8
		row.add_child(grid)
		for i: int in range(cap):
			var c: int = ItemDb.code(ItemDb.bag_container(parent), i)
			var s: Control = _slot("inv", c, "")
			s.custom_minimum_size = Vector2(38, 38)
			grid.add_child(s)
			_fill(s, c)


func _rebuild_stats() -> void:
	if _stats == null:
		return
	if sheet.is_empty():
		_stats.text = ""
		return
	var s: Dictionary = sheet
	var lines: Array[String] = [
		"AC %d   HP %d" % [int(s["ac"]), int(s["max_hp"])],
		"Mana %d   End %d" % [int(s["max_mana"]), int(s["max_endurance"])],
		"STR %d  STA %d  AGI %d" % [int(s["str"]), int(s["sta"]), int(s["agi"])],
		"DEX %d  WIS %d  INT %d" % [int(s["dex"]), int(s["wis"]), int(s["intel"])],
		"CHA %d   Haste %d%%" % [int(s["cha"]), int(s["haste"])],
		"Weapon %d / %.1fs" % [int(s["weapon_damage"]), float(s["weapon_delay_ms"]) / 1000.0],
	]
	if int(s["offhand_damage"]) > 0:
		lines.append("Off hand %d / %.1fs" % [int(s["offhand_damage"]), float(s["offhand_delay_ms"]) / 1000.0])
	lines.append("Crit %.1f%%" % float(s["crit_pct"]))
	lines.append("Resists: M %d  F %d  C %d" % [int(s["resist_magic"]), int(s["resist_fire"]), int(s["resist_cold"])])
	lines.append("         P %d  D %d" % [int(s["resist_poison"]), int(s["resist_disease"])])
	_stats.text = "\n".join(lines)


# ------------------------------------------------------------------------------ drag, drop, use

func _on_dropped(data: Dictionary, target: Control) -> void:
	var from: int = int(data.get("code", ItemDb.NO_SLOT))
	var n: int = int(data.get("count", 0))
	var role: String = str(target.get("role"))
	var source: String = str(data.get("source", ""))
	if source not in ["inv", "equip"]:
		return
	match role:
		"inv", "equip":
			var to: int = int(target.get("code"))
			if to != from:
				move_item.emit(from, to, n)
		"trade":
			trade_set_item.emit(int(target.get_meta("index")), from)
		"destroy":
			_pending_destroy = {"code": from, "item_id": str(data.get("item_id", ""))}
			_confirm.dialog_text = "Destroy %s? This cannot be undone." % ItemDb.name_of(str(data.get("item_id", "")))
			_confirm.popup_centered()
		"sell":
			if not vendor.is_empty():
				vendor_sell.emit(int(vendor["npc_id"]), from, n)
		"post":
			_post_code = from
			_post_item = str(data.get("item_id", ""))
			_auction_post_slot.call("set_item", _post_item, int(_stack(from).get("count", 1)))


func _on_destroy_confirmed() -> void:
	if not _pending_destroy.is_empty():
		destroy_item.emit(int(_pending_destroy["code"]), str(_pending_destroy["item_id"]))
	_pending_destroy = {}


## Right-click: sell at a vendor, move across at the bank, take off worn gear, otherwise use or equip.
func _on_used(target: Control) -> void:
	var c: int = int(target.get("code"))
	var role: String = str(target.get("role"))
	var id: String = str(target.get("item_id"))
	if role == "trade":
		trade_set_item.emit(int(target.get_meta("index")), ItemDb.NO_SLOT)
		return
	if role not in ["inv", "equip"]:
		return
	if not vendor.is_empty() and role == "inv" and not ItemDb.in_bank(c):
		vendor_sell.emit(int(vendor["npc_id"]), c, 0)
		return
	if bank_npc != 0 and _bank_win.visible and role == "inv":
		var to: int = free_pack_slot(id) if ItemDb.in_bank(c) else free_bank_slot(id)
		if to == ItemDb.NO_SLOT:
			notice.emit("There is no room for that.")
		else:
			move_item.emit(c, to, 0)
		return
	if role == "equip":
		var to2: int = free_pack_slot(id)
		if to2 == ItemDb.NO_SLOT:
			notice.emit("Your pack is full.")
		else:
			move_item.emit(c, to2, 0)
		return
	use_item.emit(c)


func toggle_inventory() -> void:
	_inv_win.visible = not _inv_win.visible


func is_inventory_open() -> bool:
	return _inv_win.visible


# ------------------------------------------------------------------------------ vendor

func show_vendor(w: Dictionary) -> void:
	if int(w["npc_id"]) == 0:
		_close_vendor()
		return
	_only_npc_window(_vendor_win)
	vendor = w
	_vendor_title.text = str(w["name"])
	_vendor_note.text = "%s\nThey pay %.0f%% of an item's value." % [str(w["greeting"]), float(w["sell_permille"]) / 10.0]
	for c: Node in _vendor_list.get_children():
		c.queue_free()
	var shown_buyback: bool = false
	for e: Dictionary in w["entries"]:
		var buyback: bool = bool(e["buyback"])
		if buyback and not shown_buyback:
			shown_buyback = true
			var h: Label = UI.label(_vendor_list, "Sold by other adventurers:")
			h.add_theme_color_override("font_color", UI.ACCENT)
		var id: String = str(e["item_id"])
		var row: HBoxContainer = HBoxContainer.new()
		_vendor_list.add_child(row)
		var name_l: Label = Label.new()
		name_l.text = ItemDb.name_of(id)
		name_l.add_theme_color_override("font_color", ItemDb.rarity_color(id))
		name_l.tooltip_text = ItemDb.tooltip(id, 1, ItemDb.comparison_item(ItemDb.item(id), worn_by_index()))
		name_l.mouse_filter = Control.MOUSE_FILTER_STOP
		name_l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(name_l)
		var stock: int = int(e["stock"])
		var price_l: Label = Label.new()
		price_l.text = ItemDb.format_coin(int(e["price"])) + (("  (%d left)" % stock) if stock >= 0 else "")
		row.add_child(price_l)
		var qty: SpinBox = SpinBox.new()
		var stackable: int = int(ItemDb.item(id).get("stack", 1))
		qty.min_value = 1
		qty.max_value = maxi(1, stackable if stock < 0 else mini(stock, stackable))
		qty.value = 1
		qty.visible = stackable > 1
		row.add_child(qty)
		var b: Button = Button.new()
		b.text = "Buy"
		b.disabled = stock == 0
		var npc: int = int(w["npc_id"])
		b.pressed.connect(func() -> void: vendor_buy.emit(npc, id, int(qty.value), buyback))
		row.add_child(b)
	_vendor_win.visible = true
	_inv_win.visible = true


func _close_vendor() -> void:
	vendor = {}
	_vendor_win.visible = false


func is_vendor_open() -> bool:
	return not vendor.is_empty()


# ------------------------------------------------------------------------------ bank

func show_bank(w: Dictionary) -> void:
	bank_npc = int(w["npc_id"])
	if bank_npc == 0:
		_bank_win.visible = false
		return
	bank_slots = maxi(1, int(w["slots"]))
	_only_npc_window(_bank_win)
	_rebuild_bank()
	_bank_win.visible = true
	_inv_win.visible = true


func _rebuild_bank() -> void:
	for c: Node in _bank_grid.get_children():
		c.queue_free()
	for b: int in range(bank_slots):
		var c: int = ItemDb.code(ItemDb.CONT_BANK, b)
		var s: Control = _slot("inv", c, "")
		_bank_grid.add_child(s)
		_fill(s, c)
	_rebuild_bags(_bank_bags, ItemDb.CONT_BANK, bank_slots)
	_bank_coin_label.text = "In the bank: %s   Carried: %s" % [ItemDb.format_coin(bank_coin_amount), ItemDb.format_coin(coin)]


func is_bank_open() -> bool:
	return bank_npc != 0 and _bank_win.visible


# ------------------------------------------------------------------------------ trade

func show_invite(msg: Dictionary) -> void:
	invite = msg
	_invite_text.text = "%s would like to trade with you." % str(msg["from_name"])
	_invite_win.visible = true


func _answer_invite(accept: bool) -> void:
	if not invite.is_empty():
		trade_respond.emit(int(invite["from_id"]), accept)
	invite = {}
	_invite_win.visible = false


func show_trade(msg: Dictionary) -> void:
	trade = msg
	invite = {}
	_invite_win.visible = false
	_rebuild_trade()
	_trade_win.visible = true
	_inv_win.visible = true


func _rebuild_trade() -> void:
	if trade.is_empty():
		return
	_trade_title.text = "Trading with %s" % str(trade["partner_name"])
	for i: int in range(8):
		(_trade_mine.get_child(i) as Control).call("clear")
		(_trade_theirs.get_child(i) as Control).call("clear")
	for e: Dictionary in trade["mine"]:
		(_trade_mine.get_child(int(e["index"])) as Control).call("set_item", str(e["item_id"]), int(e["count"]), {})
	for e: Dictionary in trade["theirs"]:
		(_trade_theirs.get_child(int(e["index"])) as Control).call("set_item", str(e["item_id"]), int(e["count"]), {})
	_trade_status.text = "You give %s%s.   They give %s%s." % [
		ItemDb.format_coin(int(trade["my_coin"])), " (accepted)" if bool(trade["my_accept"]) else "",
		ItemDb.format_coin(int(trade["their_coin"])), " (accepted)" if bool(trade["their_accept"]) else ""]


func close_trade(msg: Dictionary) -> void:
	trade = {}
	invite = {}
	_trade_win.visible = false
	_invite_win.visible = false
	var text: String = str(msg.get("message", ""))
	if text != "":
		notice.emit(text)


func is_trading() -> bool:
	return not trade.is_empty()


# ------------------------------------------------------------------------------ auctions

func show_auction(w: Dictionary) -> void:
	auction = w
	_only_npc_window(_auction_win)
	_auction_title.text = "%s - auction board (fee %.0f%%, min %s, listings last %.0f h)" % [
		str(w["name"]), float(w["fee_pct"]), ItemDb.format_coin(int(w["min_fee"])), float(w["hours"])]
	_auction_win.visible = true
	_inv_win.visible = true


func _search(mine: bool, page: int) -> void:
	if auction.is_empty():
		return
	_auction_mine = mine
	_auction_page = page
	auction_search.emit(int(auction["npc_id"]), _auction_text.text, KINDS[_auction_kind.selected], int(_auction_min.value),
		int(_auction_max.value), page, mine)


func _post() -> void:
	if auction.is_empty() or _post_code == ItemDb.NO_SLOT:
		notice.emit("Drag an item from your pack onto the Post square first.")
		return
	var price: int = parse_coin(_auction_price.text)
	if price <= 0:
		notice.emit("Set a price like 1p 5g or 1500 (copper).")
		return
	auction_post.emit(int(auction["npc_id"]), _post_code, _post_item, price)
	_post_code = ItemDb.NO_SLOT
	_post_item = ""
	_auction_post_slot.call("clear")


func show_auction_results(r: Dictionary) -> void:
	auction_results = r
	_auction_mine = bool(r["mine"])
	_auction_page = int(r["page"])
	for c: Node in _auction_list.get_children():
		c.queue_free()
	var entries: Array = r["entries"]
	if entries.is_empty():
		UI.label(_auction_list, "Nothing listed." if not _auction_mine else "You have nothing on the board.")
	for e: Dictionary in entries:
		var id: String = str(e["item_id"])
		var row: HBoxContainer = HBoxContainer.new()
		_auction_list.add_child(row)
		var n: Label = Label.new()
		n.text = ItemDb.name_of(id) + ((" x%d" % int(e["count"])) if int(e["count"]) > 1 else "")
		n.add_theme_color_override("font_color", ItemDb.rarity_color(id))
		n.tooltip_text = ItemDb.tooltip(id, int(e["count"]), ItemDb.comparison_item(ItemDb.item(id), worn_by_index()))
		n.mouse_filter = Control.MOUSE_FILTER_STOP
		n.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(n)
		var info: Label = Label.new()
		var state: int = int(e["state"])
		var left_h: float = float(e["expires_ms"]) / 3600000.0
		var when: String = ["%.1f h left" % left_h, "sold", "expired"][clampi(state, 0, 2)]
		info.text = "%s   %s   %s" % [ItemDb.format_coin(int(e["price"])), str(e["seller"]), when]
		row.add_child(info)
		var b: Button = Button.new()
		var npc: int = int(auction.get("npc_id", 0))
		var lid: int = int(e["id"])
		var price: int = int(e["price"])
		if _auction_mine:
			b.text = "Cancel"
			b.disabled = state != 0
			b.pressed.connect(func() -> void: auction_cancel.emit(npc, lid))
		else:
			b.text = "Buy"
			b.pressed.connect(func() -> void: auction_buy.emit(npc, lid, price))
		row.add_child(b)
	var pages: int = maxi(1, ceili(float(int(r["total"])) / 20.0))
	_auction_page_label.text = "  page %d of %d (%d listings)  " % [_auction_page + 1, pages, int(r["total"])]


func is_auction_open() -> bool:
	return _auction_win.visible


# ------------------------------------------------------------------------------ loot and rolls

func show_loot(w: Dictionary) -> void:
	loot = w
	_loot_title.text = str(w["name"])
	for c: Node in _loot_list.get_children():
		c.queue_free()
	var entries: Array = w["items"]
	if entries.is_empty():
		UI.label(_loot_list, "Nothing left on this corpse.")
	for it: Dictionary in entries:
		var id: String = str(it["item_id"])
		var row: HBoxContainer = HBoxContainer.new()
		_loot_list.add_child(row)
		var l: Label = Label.new()
		l.text = "%s%s" % [ItemDb.name_of(id), (" x%d" % int(it["count"])) if int(it["count"]) > 1 else ""]
		l.add_theme_color_override("font_color", ItemDb.rarity_color(id))
		l.tooltip_text = ItemDb.tooltip(id, int(it["count"]), ItemDb.comparison_item(ItemDb.item(id), worn_by_index()))
		l.mouse_filter = Control.MOUSE_FILTER_STOP
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(l)
		var owner: String = str(it.get("owner", ""))
		if owner != "":
			var o: Label = Label.new()
			o.text = owner
			o.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
			row.add_child(o)
		var index: int = int(it["index"])
		var b: Button = Button.new()
		b.text = "Take"
		b.disabled = not bool(it.get("mine", true))
		b.pressed.connect(func() -> void: loot_take.emit(index))
		row.add_child(b)
	_loot_win.visible = true


func hide_loot() -> void:
	loot = {}
	_loot_win.visible = false


func is_loot_open() -> bool:
	return _loot_win.visible


func loot_corpse() -> int:
	return int(loot.get("corpse_id", 0))


func show_roll(msg: Dictionary) -> void:
	var roll_id: int = int(msg["roll_id"])
	rolls[roll_id] = msg
	var p: PanelContainer = PanelContainer.new()
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = Color(0.06, 0.05, 0.05, 0.92)
	s.border_color = ItemDb.rarity_color(str(msg["item_id"]))
	s.set_border_width_all(2)
	s.set_content_margin_all(6)
	p.add_theme_stylebox_override("panel", s)
	p.name = "Roll_%d" % roll_id
	_rolls_box.add_child(p)
	var box: HBoxContainer = HBoxContainer.new()
	p.add_child(box)
	var l: Label = Label.new()
	var id: String = str(msg["item_id"])
	l.text = "Roll for %s%s" % [ItemDb.name_of(id), (" x%d" % int(msg["count"])) if int(msg["count"]) > 1 else ""]
	l.tooltip_text = ItemDb.tooltip(id, int(msg["count"]), ItemDb.comparison_item(ItemDb.item(id), worn_by_index()))
	l.mouse_filter = Control.MOUSE_FILTER_STOP
	box.add_child(l)
	var need: Button = UI.button(box, "Need", func() -> void: _answer_roll(roll_id, 2))
	need.disabled = not bool(msg["can_need"])
	need.custom_minimum_size = Vector2(70, 30)
	(UI.button(box, "Greed", func() -> void: _answer_roll(roll_id, 1)) as Button).custom_minimum_size = Vector2(70, 30)
	(UI.button(box, "Pass", func() -> void: _answer_roll(roll_id, 0)) as Button).custom_minimum_size = Vector2(70, 30)
	var timer: SceneTreeTimer = get_tree().create_timer(float(msg["expires_ms"]) / 1000.0)
	timer.timeout.connect(func() -> void: _drop_roll(roll_id))


func _answer_roll(roll_id: int, choice: int) -> void:
	roll_choice.emit(roll_id, choice)
	_drop_roll(roll_id)


func _drop_roll(roll_id: int) -> void:
	rolls.erase(roll_id)
	var n: Node = _rolls_box.get_node_or_null("Roll_%d" % roll_id)
	if n != null:
		n.queue_free()


# ------------------------------------------------------------------------------ windows

## Vendor, bank and auction board share one spot beside the inventory: opening one closes the others.
func _only_npc_window(keep: Control) -> void:
	if keep != _vendor_win and not vendor.is_empty():
		_close_vendor()
	if keep != _bank_win:
		_bank_win.visible = false
	if keep != _auction_win:
		_auction_win.visible = false


## Phase 13: a crafting station opened, so the vendor, bank and auction windows give way to it.
func close_npc_windows() -> void:
	_only_npc_window(null)


func any_window_open() -> bool:
	return _inv_win.visible or _bank_win.visible or _vendor_win.visible or _trade_win.visible or _auction_win.visible or _loot_win.visible


func close_windows() -> void:
	_inv_win.visible = false
	_bank_win.visible = false
	_auction_win.visible = false
	_close_vendor()
	if not trade.is_empty():
		trade_cancel.emit()
	if _loot_win.visible:
		loot_close.emit()
