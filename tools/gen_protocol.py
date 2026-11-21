#!/usr/bin/env python3
"""Generate C++ and GDScript protocol code from protocol/messages.json.

Usage:
    python tools/gen_protocol.py          # write generated files
    python tools/gen_protocol.py --check  # exit 1 if generated files are stale (CI)
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "protocol" / "messages.json"
CPP_OUT = ROOT / "server" / "src" / "net" / "protocol.gen.hpp"
GD_OUT = ROOT / "client" / "scripts" / "net" / "protocol.gd"

PRIMS = {"u8", "u16", "u32", "u64", "i32", "f32", "bool", "string"}
CPP_TYPES = {
    "u8": "std::uint8_t", "u16": "std::uint16_t", "u32": "std::uint32_t",
    "u64": "std::uint64_t", "i32": "std::int32_t", "f32": "float",
    "bool": "bool", "string": "std::string",
}
GD_DEFAULTS = {
    "u8": "0", "u16": "0", "u32": "0", "u64": "0", "i32": "0",
    "f32": "0.0", "bool": "false", "string": '""',
}


class SpecError(Exception):
    pass


def snake(name: str) -> str:
    out = []
    for i, ch in enumerate(name):
        if ch.isupper() and i > 0:
            out.append("_")
        out.append(ch.lower())
    return "".join(out)


def load_spec() -> dict:
    spec = json.loads(SPEC.read_text(encoding="utf-8"))
    structs = spec.get("structs", {})
    seen_ids: set[int] = set()
    seen_names: set[str] = set()
    if not isinstance(spec.get("version"), int) or not 0 < spec["version"] < 65536:
        raise SpecError("version must be an integer 1..65535")

    def check_field(owner: str, f: dict) -> None:
        t = f.get("type")
        if t == "array":
            of = f.get("of")
            if of not in PRIMS and of not in structs:
                raise SpecError(f"{owner}.{f['name']}: unknown array element type {of!r}")
            if of == "array":
                raise SpecError(f"{owner}.{f['name']}: nested arrays are not supported")
            if not isinstance(f.get("max"), int) or not 0 < f["max"] <= 65535:
                raise SpecError(f"{owner}.{f['name']}: arrays need max 1..65535")
            if of == "string" and not isinstance(f.get("max_len"), int):
                raise SpecError(f"{owner}.{f['name']}: string arrays need max_len")
        elif t == "string":
            if not isinstance(f.get("max"), int) or not 0 < f["max"] <= 65535:
                raise SpecError(f"{owner}.{f['name']}: strings need max 1..65535")
        elif t not in PRIMS and t not in structs:
            raise SpecError(f"{owner}.{f['name']}: unknown type {t!r}")

    for sname, fields in structs.items():
        for f in fields:
            check_field(sname, f)
            if f["type"] in structs:
                raise SpecError(f"{sname}.{f['name']}: nested structs are not supported")
    for m in spec["messages"]:
        if m["id"] in seen_ids or not 0 < m["id"] < 65536:
            raise SpecError(f"duplicate or invalid message id {m['id']}")
        if m["name"] in seen_names:
            raise SpecError(f"duplicate message name {m['name']}")
        if m["dir"] not in ("c2s", "s2c"):
            raise SpecError(f"{m['name']}: dir must be c2s or s2c")
        if m["channel"] not in (0, 1, 2):
            raise SpecError(f"{m['name']}: channel must be 0, 1, or 2 (TLS login stream)")
        seen_ids.add(m["id"])
        seen_names.add(m["name"])
        for f in m["fields"]:
            check_field(m["name"], f)
    return spec


# --------------------------------------------------------------------------- C++


def cpp_field_type(f: dict) -> str:
    t = f["type"]
    if t == "array":
        of = f["of"]
        return f"std::vector<{CPP_TYPES.get(of, of)}>"
    return CPP_TYPES.get(t, t)


def cpp_init(f: dict) -> str:
    t = f["type"]
    if t in ("string", "array") or t not in PRIMS:
        return ""
    return "{}"


def cpp_encode_stmt(f: dict) -> list[str]:
    n, t = f["name"], f["type"]
    if t == "string":
        return [f"w.str({n}, {f['max']});"]
    if t == "array":
        of = f["of"]
        lines = [f"{{ const std::size_t count = std::min<std::size_t>({n}.size(), {f['max']});",
                 "  w.u16(static_cast<std::uint16_t>(count));",
                 "  for (std::size_t i = 0; i < count; ++i) {"]
        if of == "string":
            lines.append(f"    w.str({n}[i], {f['max_len']});")
        elif of in PRIMS:
            lines.append(f"    w.{'boolean' if of == 'bool' else of}({n}[i]);")
        else:
            lines.append(f"    {n}[i].encode(w);")
        lines += ["  }", "}"]
        return lines
    if t == "bool":
        return [f"w.boolean({n});"]
    if t in PRIMS:
        return [f"w.{t}({n});"]
    return [f"{n}.encode(w);"]


TRIVIAL_STRUCTS: set[str] = set()


def cpp_decode_stmt(f: dict) -> list[str]:
    n, t = f["name"], f["type"]
    if t == "string":
        return [f"if (!r.str({n}, {f['max']})) return false;"]
    if t == "array":
        of = f["of"]
        lines = [f"{{ std::uint16_t count = 0;",
                 f"  if (!r.u16(count) || count > {f['max']}) return r.fail();",
                 f"  {n}.clear();",
                 f"  {n}.reserve(count);",
                 "  for (std::uint16_t i = 0; i < count; ++i) {"]
        if of == "string":
            lines += [f"    std::string v;", f"    if (!r.str(v, {f['max_len']})) return false;",
                      f"    {n}.push_back(std::move(v));"]
        elif of in PRIMS:
            fn = "boolean" if of == "bool" else of
            lines += [f"    {CPP_TYPES[of]} v{{}};", f"    if (!r.{fn}(v)) return false;", f"    {n}.push_back(v);"]
        else:
            push = f"{n}.push_back(v);" if of in TRIVIAL_STRUCTS else f"{n}.push_back(std::move(v));"
            lines += [f"    {of} v;", "    if (!v.decode(r)) return false;", f"    {push}"]
        lines += ["  }", "}"]
        return lines
    if t == "bool":
        return [f"if (!r.boolean({n})) return false;"]
    if t in PRIMS:
        return [f"if (!r.{t}({n})) return false;"]
    return [f"if (!{n}.decode(r)) return false;"]


def indent(lines: list[str], n: int) -> list[str]:
    return [(" " * n + ln) if ln else ln for ln in lines]


def gen_struct_cpp(name: str, fields: list[dict], msg: dict | None) -> list[str]:
    out = [f"struct {name} {{"]
    if msg is not None:
        out += [f"  static constexpr MsgId kId = MsgId::{name};",
                f"  static constexpr std::uint8_t kChannel = {msg['channel']};",
                f"  static constexpr Direction kDirection = Direction::{'ClientToServer' if msg['dir'] == 'c2s' else 'ServerToClient'};",
                f"  static constexpr const char* kName = \"{name}\";"]
    for f in fields:
        out.append(f"  {cpp_field_type(f)} {f['name']}{cpp_init(f)};")
    if not fields:
        out.append("  void encode(Writer& /*w*/) const {}")
        out.append("  bool decode(Reader& /*r*/) { return true; }")
    else:
        out.append("  void encode(Writer& w) const {")
        for f in fields:
            out += indent(cpp_encode_stmt(f), 4)
        out.append("  }")
        out.append("  bool decode(Reader& r) {")
        for f in fields:
            out += indent(cpp_decode_stmt(f), 4)
        out.append("    return true;")
        out.append("  }")
    out.append("};")
    out.append("")
    return out


def gen_cpp(spec: dict) -> str:
    msgs = spec["messages"]
    TRIVIAL_STRUCTS.clear()
    for sname, fields in spec.get("structs", {}).items():
        if all(f["type"] in PRIMS and f["type"] != "string" for f in fields):
            TRIVIAL_STRUCTS.add(sname)
    out = [
        "// GENERATED by tools/gen_protocol.py from protocol/messages.json. DO NOT EDIT.",
        "#pragma once",
        "// Generated switches intentionally repeat identical case bodies.",
        "// NOLINTBEGIN(bugprone-branch-clone)",
        "",
        "#include <algorithm>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "#include <string>",
        "#include <utility>",
        "#include <vector>",
        "",
        '#include "net/byte_buffer.hpp"',
        "",
        "namespace ef::proto {",
        "",
        "using ef::net::Reader;",
        "using ef::net::Writer;",
        "",
        f"inline constexpr std::uint16_t kProtocolVersion = {spec['version']};",
        "inline constexpr std::uint8_t kChannelReliable = 0;",
        "inline constexpr std::uint8_t kChannelUnreliable = 1;",
        "inline constexpr std::uint8_t kChannelCount = 2;  // ENet channels",
        "inline constexpr std::uint8_t kChannelTls = 2;    // TLS login stream, never sent over ENet",
        "",
        "enum class Direction : std::uint8_t { ClientToServer, ServerToClient };",
        "",
        "enum class MsgId : std::uint16_t {",
    ]
    for m in msgs:
        out.append(f"  {m['name']} = {m['id']},")
    out += ["};", ""]

    for sname, fields in spec.get("structs", {}).items():
        out += gen_struct_cpp(sname, fields, None)
    for m in msgs:
        out += gen_struct_cpp(m["name"], m["fields"], m)

    out += ["inline const char* msg_name(std::uint16_t id) {", "  switch (id) {"]
    for m in msgs:
        out.append(f"    case {m['id']}: return \"{m['name']}\";")
    out += ["    default: return \"Unknown\";", "  }", "}", ""]

    out += ["// Returns true and sets dir/channel if the id is a known message.",
            "inline bool msg_info(std::uint16_t id, Direction& dir, std::uint8_t& channel) {",
            "  switch (id) {"]
    for m in msgs:
        d = "ClientToServer" if m["dir"] == "c2s" else "ServerToClient"
        out.append(f"    case {m['id']}: dir = Direction::{d}; channel = {m['channel']}; return true;")
    out += ["    default: return false;", "  }", "}", ""]

    out += [
        "// Reads the message id without consuming the payload.",
        "inline bool peek_id(std::span<const std::uint8_t> data, std::uint16_t& id) {",
        "  if (data.size() < 2) return false;",
        "  id = static_cast<std::uint16_t>(data[0] | (data[1] << 8));",
        "  return true;",
        "}",
        "",
        "template <class M>",
        "std::vector<std::uint8_t> encode_packet(const M& msg) {",
        "  std::vector<std::uint8_t> buf;",
        "  buf.reserve(64);",
        "  Writer w(buf);",
        "  w.u16(static_cast<std::uint16_t>(M::kId));",
        "  msg.encode(w);",
        "  return buf;",
        "}",
        "",
        "// Encodes into a caller-owned buffer (cleared first) so hot paths can reuse capacity.",
        "template <class M>",
        "void encode_packet_into(const M& msg, std::vector<std::uint8_t>& buf) {",
        "  buf.clear();",
        "  Writer w(buf);",
        "  w.u16(static_cast<std::uint16_t>(M::kId));",
        "  msg.encode(w);",
        "}",
        "",
        "// Strict decode: id must match, payload must be fully consumed, every bound checked.",
        "template <class M>",
        "bool decode_packet(std::span<const std::uint8_t> data, M& out) {",
        "  Reader r(data);",
        "  std::uint16_t id = 0;",
        "  if (!r.u16(id) || id != static_cast<std::uint16_t>(M::kId)) return false;",
        "  if (!out.decode(r)) return false;",
        "  return r.at_end();",
        "}",
        "",
        "// Calls fn(decoded_message) for any known message; returns false if decoding failed.",
        "template <class Fn>",
        "bool visit_packet(std::span<const std::uint8_t> data, Fn&& fn) {",
        "  std::uint16_t id = 0;",
        "  if (!peek_id(data, id)) return false;",
        "  switch (id) {",
    ]
    for m in msgs:
        out += [f"    case {m['id']}: {{ {m['name']} m; if (!decode_packet(data, m)) return false; fn(m); return true; }}"]
    out += ["    default: return false;", "  }", "}", "", "}  // namespace ef::proto", "", "// NOLINTEND(bugprone-branch-clone)", ""]
    return "\n".join(out)


# ---------------------------------------------------------------------- GDScript


def gd_read_expr(t: str, f: dict) -> str:
    if t == "string":
        return f"r.text({f['max']})"
    if t == "bool":
        return "r.boolean()"
    if t in PRIMS:
        return f"r.{t}()"
    return f"_dec_{t}(r)"


def gd_write_stmt(t: str, expr: str, f: dict) -> str:
    if t == "string":
        return f"w.text(str({expr}), {f['max']})"
    if t == "bool":
        return f"w.boolean(bool({expr}))"
    if t == "f32":
        return f"w.f32(float({expr}))"
    if t in PRIMS:
        return f"w.{t}(int({expr}))"
    return f"_enc_{t}(w, {expr})"


def gd_field_default(f: dict) -> str:
    t = f["type"]
    if t == "array":
        return "[]"
    if t in PRIMS:
        return GD_DEFAULTS[t]
    return "{}"


def gd_encode_body(fields: list[dict]) -> list[str]:
    out = []
    for f in fields:
        n, t = f["name"], f["type"]
        get = f'm.get("{n}", {gd_field_default(f)})'
        if t == "array":
            of = f["of"]
            elem = {"type": of, "max": f.get("max_len", 0)}
            out += [f"\tvar arr_{n}: Array = {get}",
                    f"\tvar cnt_{n}: int = mini(arr_{n}.size(), {f['max']})",
                    f"\tw.u16(cnt_{n})",
                    f"\tfor i in range(cnt_{n}):",
                    f"\t\t{gd_write_stmt(of, f'arr_{n}[i]', elem)}"]
        else:
            out.append(f"\t{gd_write_stmt(t, get, f)}")
    if not out:
        out.append("\tpass")
    return out


def gd_decode_body(fields: list[dict]) -> list[str]:
    out = ["\tvar m: Dictionary = {}"]
    for f in fields:
        n, t = f["name"], f["type"]
        if t == "array":
            of = f["of"]
            elem = {"max": f.get("max_len", 0)}
            out += [f"\tvar cnt_{n}: int = r.u16()",
                    f"\tif cnt_{n} > {f['max']}:",
                    "\t\tr.ok = false",
                    f"\tvar arr_{n}: Array = []",
                    f"\tfor _i in range(cnt_{n} if r.ok else 0):",
                    f"\t\tarr_{n}.append({gd_read_expr(of, elem)})",
                    "\t\tif not r.ok:",
                    "\t\t\tbreak",
                    f'\tm["{n}"] = arr_{n}']
        else:
            out.append(f'\tm["{n}"] = {gd_read_expr(t, f)}')
    out.append("\treturn m")
    return out


GD_HELPERS = '''
class Reader:
	extends RefCounted
	var buf: StreamPeerBuffer = StreamPeerBuffer.new()
	var ok: bool = true

	func _init(data: PackedByteArray) -> void:
		buf.big_endian = false
		buf.data_array = data

	func _need(n: int) -> bool:
		if not ok or buf.get_available_bytes() < n:
			ok = false
			return false
		return true

	func at_end() -> bool:
		return buf.get_available_bytes() == 0

	func u8() -> int:
		return buf.get_u8() if _need(1) else 0

	func u16() -> int:
		return buf.get_u16() if _need(2) else 0

	func u32() -> int:
		return buf.get_u32() if _need(4) else 0

	func u64() -> int:
		return buf.get_u64() if _need(8) else 0

	func i32() -> int:
		return buf.get_32() if _need(4) else 0

	func f32() -> float:
		if not _need(4):
			return 0.0
		var v: float = buf.get_float()
		if is_nan(v) or is_inf(v):
			ok = false
			return 0.0
		return v

	func boolean() -> bool:
		var v: int = u8()
		if v > 1:
			ok = false
		return v == 1

	func text(max_len: int) -> String:
		var n: int = u16()
		if n > max_len or not _need(n):
			ok = false
			return ""
		if n == 0:
			return ""
		var res: Array = buf.get_data(n)
		if int(res[0]) != OK:
			ok = false
			return ""
		var bytes: PackedByteArray = res[1]
		return bytes.get_string_from_utf8()


class Writer:
	extends RefCounted
	var buf: StreamPeerBuffer = StreamPeerBuffer.new()

	func _init() -> void:
		buf.big_endian = false

	func u8(v: int) -> void:
		buf.put_u8(v)

	func u16(v: int) -> void:
		buf.put_u16(v)

	func u32(v: int) -> void:
		buf.put_u32(v)

	func u64(v: int) -> void:
		buf.put_u64(v)

	func i32(v: int) -> void:
		buf.put_32(v)

	func f32(v: float) -> void:
		if is_nan(v) or is_inf(v):
			v = 0.0
		buf.put_float(v)

	func boolean(v: bool) -> void:
		buf.put_u8(1 if v else 0)

	func text(s: String, max_len: int) -> void:
		# Every character is at least one UTF-8 byte, so cap the character count first; the trim
		# loop below then runs at most a few iterations per multi-byte character.
		if s.length() > max_len:
			s = s.substr(0, max_len)
		var bytes: PackedByteArray = s.to_utf8_buffer()
		while bytes.size() > max_len and s.length() > 0:
			s = s.substr(0, s.length() - 1)
			bytes = s.to_utf8_buffer()
		buf.put_u16(bytes.size())
		if bytes.size() > 0:
			buf.put_data(bytes)

	func data() -> PackedByteArray:
		return buf.data_array
'''


def gen_gd(spec: dict) -> str:
    msgs = spec["messages"]
    out = [
        "# GENERATED by tools/gen_protocol.py from protocol/messages.json. DO NOT EDIT.",
        "# Little-endian, bounds-checked codec. Messages are Dictionaries keyed by field name.",
        "extends RefCounted",
        "",
        f"const PROTOCOL_VERSION: int = {spec['version']}",
        "const CHANNEL_RELIABLE: int = 0",
        "const CHANNEL_UNRELIABLE: int = 1",
        "const CHANNEL_COUNT: int = 2",
        "const CHANNEL_TLS: int = 2",
        "",
    ]
    for m in msgs:
        out.append(f"const {snake(m['name']).upper()}: int = {m['id']}")
    out += ["", "const NAMES: Dictionary = {"]
    for m in msgs:
        out.append(f"\t{m['id']}: \"{m['name']}\",")
    out += ["}", "", "const CHANNELS: Dictionary = {"]
    for m in msgs:
        out.append(f"\t{m['id']}: {m['channel']},")
    out += ["}", ""]
    out.append(GD_HELPERS)

    for sname, fields in spec.get("structs", {}).items():
        out += ["", f"static func _enc_{sname}(w: Writer, m: Dictionary) -> void:"]
        out += gd_encode_body(fields)
        out += ["", f"static func _dec_{sname}(r: Reader) -> Dictionary:"]
        out += gd_decode_body(fields)

    for m in msgs:
        fn = snake(m["name"])
        param = "m" if m["fields"] else "_m"
        out += ["", f"static func encode_{fn}({param}: Dictionary = {{}}) -> PackedByteArray:",
                "\tvar w: Writer = Writer.new()",
                f"\tw.u16({m['id']})"]
        body = gd_encode_body(m["fields"])
        if body != ["\tpass"]:
            out += body
        out.append("\treturn w.data()")
        out += ["", f"static func _dec_msg_{m['id']}(r: Reader) -> Dictionary:"]
        out += gd_decode_body(m["fields"])

    out += [
        "",
        "## Decodes a packet. Returns {} on any malformed input; otherwise the message",
        "## fields plus \"_id\" (int) and \"_name\" (String).",
        "static func decode(data: PackedByteArray) -> Dictionary:",
        "\tvar r: Reader = Reader.new(data)",
        "\tvar id: int = r.u16()",
        "\tif not r.ok:",
        "\t\treturn {}",
        "\tvar m: Dictionary = {}",
        "\tmatch id:",
    ]
    for m in msgs:
        out += [f"\t\t{m['id']}:", f"\t\t\tm = _dec_msg_{m['id']}(r)"]
    out += [
        "\t\t_:",
        "\t\t\treturn {}",
        "\tif not r.ok or not r.at_end():",
        "\t\treturn {}",
        "\tm[\"_id\"] = id",
        "\tm[\"_name\"] = NAMES[id]",
        "\treturn m",
        "",
    ]
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="fail if generated files are stale")
    args = ap.parse_args()
    try:
        spec = load_spec()
    except (SpecError, KeyError, json.JSONDecodeError) as e:
        print(f"protocol spec error: {e}", file=sys.stderr)
        return 2

    outputs = {CPP_OUT: gen_cpp(spec), GD_OUT: gen_gd(spec)}
    stale = []
    for path, text in outputs.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current != text:
            stale.append(path)
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8", newline="\n")
    if args.check:
        if stale:
            for p in stale:
                print(f"stale: {p.relative_to(ROOT)}", file=sys.stderr)
            print("Run `python tools/gen_protocol.py` and commit the result.", file=sys.stderr)
            return 1
        print("protocol: generated files are up to date")
        return 0
    for p in outputs:
        print(f"{'wrote' if p in stale else 'unchanged'}: {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
