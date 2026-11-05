"""Build the menu's wardrobe table from the game's own clothing data.

    python wardrobe_table.py            rewrite FreeCam/menu/freecam_wardrobe_data.h
    python wardrobe_table.py --check    validate the table already on disk

The debug menu needs three things the client will not tell it:

  * every `UFG::StoreClothing` UID the player could be wearing, and which wardrobe slot
    it fills - so a mix can replace the top without throwing away the shoes;
  * a short, readable name per piece, because the data's own display names are
    "P_DeusEx - Tint [0]" and a menu row should not read like a filename;
  * the subset that is worth *listing* in the top and pants pickers, one entry per
    garment rather than one per tint.

The slot rule is not re-invented here: `LocalServer/tools/looks.py:slot_of` is the one
the lookbook already uses, and it is imported. Which pieces are in the table at all is
decided by two rules:

  * a piece the wardrobe store sells is in (it is already bought and worn in game), and
  * a piece that belongs to a complete outfit is in, even when it carries no category -
    those are the outfit-only pieces (`P_DeusEx`, `P_Swat`, ...), which are exactly the
    ones that were impossible to wear with anything else.

Anything else - the 42 records with no category that no outfit uses, the 13 marked "NOT
READY FOR STORE YET", the 2 marked "TO BE DELETED BY LGP" - stays out: the client has no
finished garment for them and a menu row that puts one on the player is a way to crash
the renderer, not a feature.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DB_PATH = os.path.join(ROOT, "LocalServer", "devdata", "gamedb.json")
OUT_PATH = os.path.normpath(os.path.join(HERE, "..", "menu", "freecam_wardrobe_data.h"))

sys.path.insert(0, os.path.join(ROOT, "LocalServer", "tools"))
import looks  # noqa: E402  (the lookbook's slot_of / _key)

UNFINISHED = ("NOT READY FOR STORE YET", "TO BE DELETED BY LGP")

# The slots the menu lists a picker for. Everything else (head, feet, accessories) is
# left alone by a mix, which is what "swap the pants, keep the hat" needs.
PICKER_SLOTS = ("torso", "legs")

SLOT_WORD = {"torso": "top", "legs": "pants", "feet": "shoes", "head": "head",
             "neck": "chain", "glasses": "glasses", "mask": "mask", "hair": "hair",
             "beard": "beard", "face": "face", "tattoo": "tattoo",
             "wrist_left": "watch", "wrist_right": "watch", "accessory": "piece"}

_PREFIX = re.compile(r"^([TPHSGWN])_(.+)$")
_TINT = re.compile(r"\s*-\s*(?:Set\s*\[[^\]]*\]\s*-\s*)?Tint\s*\[\s*\d+\s*\]\s*$")


def _camel(text):
    """SuperSwat -> Super Swat; DeusEx -> Deus Ex; KungFuPeasant -> Kung Fu Peasant."""
    out = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", text)
    out = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", " ", out)
    return out.replace("_", " ").strip()


def label(record, slot):
    """A menu name for one piece: "Deus Ex pants", "Baggy Jeans", "Hoody (top)"."""
    part = (record.get("mPartName") or "").strip()
    display = _TINT.sub("", (record.get("mDisplayName") or "").strip()).strip()
    word = SLOT_WORD.get(slot, "piece")
    match = _PREFIX.match(part)
    if match and display.lower().startswith(part.lower()):
        # The developer name for an outfit garment: T_DeusEx -> "Deus Ex top".
        name = _camel(match.group(2))
    elif display:
        # A real product name. Long ones are trimmed so a row stays on one line.
        name = display
        if len(name) > 28:
            name = name[:27].rstrip() + "~"
        return name
    else:
        name = _camel(part)
    name = name.strip() or word.title()
    # "Baggy Jeans" already says what it is; "Deus Ex" does not.
    if word in name.lower():
        return name
    return name + " " + word


def build(db_path):
    with open(db_path, encoding="utf-8") as f:
        db = json.load(f)
    clothing = {o["$uid"]: o for o in db["UFG::StoreClothing"]}

    # Every piece a complete outfit uses: those are wearable even with no category.
    used = set()
    for outfit in db["UFG::StoreOutfit"]:
        pieces = [h["$handle"] for h in outfit.get("mClothingItems") or []]
        if pieces and all(p in clothing for p in pieces):
            used.update(pieces)

    def wearable(record):
        if (record.get("mSubType") or "") in UNFINISHED:
            return False
        return (record.get("mSubType") or "") != "" or record["$uid"] in used

    pieces = []
    for uid, record in clothing.items():
        if not wearable(record):
            continue
        slot = looks.slot_of(record)
        pieces.append({"uid": uid, "slot": slot, "name": label(record, slot),
                       "key": looks._key(record)})
    pieces.sort(key=lambda p: (p["slot"], p["name"].lower(), int(p["uid"])))

    # One row per garment: tint variants share (part, display) and the lowest UID wins.
    pickers = {}
    for slot in PICKER_SLOTS:
        seen = {}
        for piece in pieces:
            if piece["slot"] != slot:
                continue
            if piece["key"] in seen:
                continue
            seen[piece["key"]] = piece
        pickers[slot] = sorted(seen.values(), key=lambda p: p["name"].lower())
    return pieces, pickers


def render(pieces, pickers):
    def cstr(text):
        return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

    lines = [
        "// freecam_wardrobe_data.h - the game's own clothing, as menu rows.",
        "//",
        "// Generated by FreeCam/tools/wardrobe_table.py from LocalServer/devdata/gamedb.json.",
        "// Do not edit: edit the tool and rebuild. Every entry is a `UFG::StoreClothing` UID",
        "// the client already resolves, so wearing one is a change of what the player is",
        "// wearing, never a new item.",
        "#ifndef TW_FREECAM_WARDROBE_DATA_H",
        "#define TW_FREECAM_WARDROBE_DATA_H",
        "",
        "namespace tw {",
        "namespace game {",
        "namespace wardrobe {",
        "",
        "struct Piece {",
        "    const char *uid;    // UFG::StoreClothing, decimal",
        "    const char *slot;   // torso, legs, feet, head, ...",
        "    const char *name;   // the menu row",
        "};",
        "",
        "static const Piece kPieces[] = {",
    ]
    for piece in pieces:
        lines.append("    { %s, %s, %s }," %
                     (cstr(piece["uid"]), cstr(piece["slot"]), cstr(piece["name"])))
    lines += [
        "};",
        "static const int kPieceCount = %d;" % len(pieces),
        "",
        "// The pickers: one row per garment, tint variants collapsed.",
    ]
    for slot in PICKER_SLOTS:
        lines.append("static const Piece k%s[] = {" %
                     ("Tops" if slot == "torso" else "Pants"))
        for piece in pickers[slot]:
            lines.append("    { %s, %s, %s }," %
                         (cstr(piece["uid"]), cstr(piece["slot"]), cstr(piece["name"])))
        lines.append("};")
        lines.append("static const int k%sCount = %d;" %
                     (("Tops" if slot == "torso" else "Pants"), len(pickers[slot])))
        lines.append("")
    lines += [
        "}  // namespace wardrobe",
        "}  // namespace game",
        "}  // namespace tw",
        "",
        "#endif  // TW_FREECAM_WARDROBE_DATA_H",
        "",
    ]
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="only verify the file on disk")
    ap.add_argument("--db", default=DB_PATH)
    args = ap.parse_args(argv)

    pieces, pickers = build(args.db)
    want = render(pieces, pickers)
    if args.check:
        with open(OUT_PATH, encoding="utf-8") as f:
            have = f.read()
        if have != want:
            sys.exit("%s is stale - run wardrobe_table.py" % OUT_PATH)
        print("wardrobe data is current: %d pieces, %d tops, %d pants"
              % (len(pieces), len(pickers["torso"]), len(pickers["legs"])))
        return
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(want)
    print("%s: %d pieces, %d tops, %d pants (%d bytes)"
          % (os.path.relpath(OUT_PATH, ROOT), len(pieces),
             len(pickers["torso"]), len(pickers["legs"]), len(want)))


if __name__ == "__main__":
    main(sys.argv[1:])
