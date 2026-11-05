"""Read the game's own reflection database and make replies match it.

The client deserialises a JSON reply by looking every key up in `Reflection.rdb`,
the type/field database that ships in the game folder (next to `TriadWars.exe`).
That file names the members of the response classes, their types and their order,
which is the only reliable way to learn what a reply for a screen that was never
emulated before (`GameService ErrandSlotData`, `UFG::TrialPlayerProgress`, ...)
has to look like.

This module reads that file when the game is installed, and uses it to:

  * conform a reply we assembled by hand to the real schema (drop keys the class
    does not have, turn 64-bit members into decimal strings, arrays and nested
    records recursively) - so a best-guess reply degrades to the members that
    really exist instead of being ignored wholesale;
  * answer questions about the schema, e.g. "which class holds an array of
    `GameService ErrandSlotData`, and what is that member called?" - the way the
    completed-errand list finds its root key;
  * dump a type for a human (`tools/schema_dump.py`).

If the file is missing (the server was copied without the game folder) or
anything about it fails to parse, `loaded` stays False and every helper is a
no-op: the server keeps answering with the reply it built. Nothing here is
required for the rest of the server to run.

The format (from the game's own binary, see `tools/rdb.py`): a 16-byte header
whose second dword is the string-table length, then the NUL-separated names,
then records of `{tag, size, size, 0}` + payload. Type records carry tag
0xe57f8e15; their payload is the hashed type name, then at +0x10 the size, then
0x28-byte field records (name hash, type hash, offset, count, flags).
"""
import os
import struct

NAME = "Reflection.rdb"
HERE = os.path.dirname(os.path.abspath(__file__))
CANDIDATES = [
    os.path.normpath(os.path.join(HERE, "..", NAME)),      # next to TriadWars.exe
    os.path.join(HERE, "data", NAME),
    os.path.join(HERE, NAME),
]
TYPE_TAG = 0xE57F8E15
MASK64 = 0xFFFFFFFFFFFFFFFF


def _crc_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x95AC9329AC4BC9B5 if c & 1 else c >> 1
        table.append(c & MASK64)
    return table


_CRC = _crc_table()


def _hash(name):
    if isinstance(name, str):
        name = name.encode("latin1")
    c = MASK64
    for b in name:
        c = _CRC[(c ^ b) & 0xFF] ^ (c >> 8)
    return c


class Schema:
    def __init__(self, path):
        self.path = path
        self.types = {}
        self.names = {}
        self._warned = set()
        with open(path, "rb") as f:
            data = f.read()
        str_end = 0x10 + struct.unpack_from("<I", data, 4)[0]
        for raw in data[0x10:str_end].split(b"\0"):
            if raw:
                self.names[_hash(raw)] = raw.decode("latin1")
        off = str_end
        while off + 16 <= len(data):
            tag, size, _size2, _zero = struct.unpack_from("<IIII", data, off)
            payload = data[off + 16:off + 16 + size]
            off += 16 + size
            if tag != TYPE_TAG or len(payload) < 0x28:
                continue
            name = self.names.get(struct.unpack_from("<Q", payload, 8)[0])
            if not name:
                continue
            fields = []
            for i in range(0x28, len(payload) - 0x27, 0x28):
                fn, ft, fo, cnt, _flags = struct.unpack_from("<5Q", payload, i)
                fields.append((self.names.get(fn, ""), self.names.get(ft, ""),
                               fo, cnt))
            self.types[name] = {"size": struct.unpack_from("<Q", payload, 0x10)[0],
                                "fields": fields,
                                "base": self.names.get(struct.unpack_from("<Q", payload, 0)[0], "")}

    # -- lookups ---------------------------------------------------------------
    def fields(self, type_name):
        info = self.types.get(type_name)
        return list(info["fields"]) if info else []

    def member_type(self, type_name, field):
        for name, ftype, _off, _cnt in self.fields(type_name):
            if name.lower() == field.lower():
                return ftype
        return None

    def find_array_member(self, element_type):
        """Every (class, member) whose member is an array of `element_type`."""
        out = []
        for tname, info in self.types.items():
            for name, ftype, _off, _cnt in info["fields"]:
                elem = self.element_type(ftype)
                if elem and elem.lower() == element_type.lower():
                    out.append((tname, name))
        return out

    # -- type names ------------------------------------------------------------
    @staticmethod
    def element_type(type_name):
        if not type_name:
            return ""
        low = type_name.lower()
        if "array<" in low:
            return type_name[low.index("array<") + 6:].rstrip(">")
        return ""

    @staticmethod
    def inner_type(type_name):
        """For qReflectObjectType<UFG::X> / qReflectInventory<UFG::X>, return UFG::X."""
        if not type_name or "<" not in type_name:
            return ""
        return type_name[type_name.index("<") + 1:].rstrip(">")

    @staticmethod
    def kind(type_name):
        if not type_name:
            return "unknown"
        low = type_name.lower()
        if "array<" in low:
            return "array"
        if "string" in low:
            return "string"
        if "bool" in low:
            return "bool"
        if "float" in low or "double" in low:
            return "float"
        if "64" in low or "long long" in low or low.endswith("s64") or low.endswith("u64"):
            return "int64"
        if "int" in low or "symbol" in low or "enum" in low or "char" in low:
            return "int"
        return "struct"

    # -- reply shaping ---------------------------------------------------------
    def conform(self, value, type_name, depth=0):
        """Return `value` filtered and coerced to what `type_name` declares.

        Unknown types are returned untouched, so a partial schema still helps.
        """
        if depth > 6 or value is None:
            return value
        kind = self.kind(type_name)
        if kind == "array":
            elem = self.element_type(type_name)
            if isinstance(value, list):
                return [self.conform(v, elem, depth + 1) for v in value]
            return value
        if kind == "int64":
            # Every 64-bit value is a decimal string in a reply.
            if isinstance(value, bool):
                return str(int(value))
            if isinstance(value, str):
                return value
            return str(int(value))
        if kind == "int":
            if isinstance(value, bool):
                return int(value)
            if isinstance(value, str):
                text = value.strip().lstrip("-")
                return int(value) if text.isdigit() else 0
            return int(value)
        if kind == "bool":
            return bool(value)
        if kind == "float":
            try:
                return float(value)
            except (TypeError, ValueError):
                return 0.0
        if kind == "string":
            return value if isinstance(value, str) else str(value)
        if type_name not in self.types:
            inner = self.inner_type(type_name)
            if inner in self.types:
                return self.conform(value, inner, depth + 1)
            return value
        if not isinstance(value, dict):
            return value
        wanted = {f[0].lower(): f for f in self.fields(type_name)}
        out = {}
        for key, item in value.items():
            field = wanted.get(key.lower())
            if field is None:
                self.warn_once("%s has no member %r (dropped)" % (type_name, key))
                continue
            out[field[0]] = self.conform(item, field[1], depth + 1)
        return out

    def reply_key(self, element_type, *fallbacks):
        """Best name for a root member holding an array of `element_type`.

        Returns the schema's own name when exactly one class declares such a
        member, else the first fallback that appears in any class (the order the
        client would read it), else the first fallback.
        """
        found = self.find_array_member(element_type)
        names = []
        for _cls, member in found:
            if member.lower() not in [n.lower() for n in names]:
                names.append(member)
        for want in fallbacks:
            if want.lower() in [n.lower() for n in names]:
                return want
        if len(names) == 1:
            return names[0]
        return fallbacks[0] if fallbacks else (names[0] if names else "")

    def warn_once(self, message):
        if message not in self._warned:
            self._warned.add(message)
            print("schema: " + message, flush=True)


def load(path=None):
    paths = [path] if path else CANDIDATES
    for candidate in paths:
        if candidate and os.path.isfile(candidate):
            try:
                return Schema(candidate)
            except Exception as e:                       # never break the server
                print("schema: cannot read %s: %r" % (candidate, e), flush=True)
                return None
    return None


SCHEMA = load()
loaded = SCHEMA is not None


def conform(value, type_name):
    return SCHEMA.conform(value, type_name) if SCHEMA else value


def reply_key(element_type, *fallbacks):
    if SCHEMA:
        key = SCHEMA.reply_key(element_type, *fallbacks)
        if key:
            return key
    return fallbacks[0] if fallbacks else ""


def describe(type_name):
    """Human-readable dump of one type; used by tools/schema_dump.py."""
    if not SCHEMA:
        return "no Reflection.rdb found (looked in %s)" % ", ".join(CANDIDATES)
    info = SCHEMA.types.get(type_name)
    if not info:
        near = [t for t in SCHEMA.types if type_name.lower() in t.lower()]
        return "no type %r; %d types loaded; similar: %s" % (type_name, len(SCHEMA.types), near[:20])
    lines = ["%s  (size 0x%x, base %r)" % (type_name, info["size"], info["base"])]
    for name, ftype, off, cnt in info["fields"]:
        lines.append("  +%03x %-36s %-46s%s" % (off, name, ftype, "" if cnt == 1 else " x%d" % cnt))
    return "\n".join(lines)
