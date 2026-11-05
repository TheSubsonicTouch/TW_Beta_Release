"""Put an outfit - or one of the curated looks - on the player.

    python playermodel.py looks                 the lookbook: 32 named looks, by theme
    python playermodel.py set "swat pointman"   wear a look
    python playermodel.py set gang              wear one; an ambiguous word lists the matches
    python playermodel.py menu                  pick one from a numbered list
    python playermodel.py list                  the game's own outfits, with their UIDs
    python playermodel.py list --npc            only the non-player models
    python playermodel.py set swat              wear an outfit (unique name fragment or UID)
    python playermodel.py show                  what the player is wearing now
    python playermodel.py clear                 back to the default model

`set` takes both kinds: a look from `data/looks.json` (a combination of clothing pieces,
built by `tools/looks.py`) or one of the game's 28 outfits. Say `set look swat` or
`set outfit swat` to remove any doubt.

Close the game and stop the server first: the server keeps `state.json` in memory and
writes it back, so an edit made underneath it is lost on the next request.

What this writes is the player's CustomizationBag - the same three fields the wardrobe
screen sends to `enforcer/customization`:

  ClothingOutfitCortexUID   the UFG::StoreOutfit being worn, or "0" for a look
  ClothingStoreItemUIDs     the UFG::StoreClothing pieces; this is what the client
                            draws, so these are the model, not the outfit name
  ScalingPresetCortexUID    the body preset, left alone ("0" = as created)

The outfit, or every piece of the look, is also added to the inventory, so the wardrobe
shows them as owned instead of offering them for sale again - which also means a look can
be adjusted by hand in the wardrobe afterwards.
"""
import argparse
import json
import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))
sys.path.insert(0, HERE)

import state  # noqa: E402
import looks as lookbook  # noqa: E402

OUTFIT_STORE = "13160186718612611233"
# The outfits whose head or hands are marked "NOT READY FOR STORE YET" in the data:
# the wardrobe never sold them, but the models are in the game and NPCs wear them.
NPC_OUTFITS = {
    "1830394833577068751": "Agent 47",
    "4514032563323111547": "Shaolin Warrior",
    "7564596263067345457": "Ultimate Beatdown",
    "7574621266568974758": "Spry and Viscious",
    "10209607326606741076": "Sun Wukong",
    "10951711769280898226": "Zombie Enforcer",
    "11089315892382218235": "NVidia Zombie Enforcer",
    "15325908797379668623": "Super Swat Uniform",
    "16601978319230876509": "Masked Justice",
    "16972082814130311141": "Deus Ex",
    "17252126931384523275": "Bronze Master",
    "17542338473309454167": "Swat Uniform",
}


def catalogue():
    return state.STORE_ITEMS[OUTFIT_STORE]


def looks():
    return lookbook.load()


_NAMES = None


def names():
    """{outfit uid: display name}, from the game data behind the catalogue."""
    global _NAMES
    if _NAMES is None:
        db_path = os.path.normpath(os.path.join(HERE, "..", "devdata", "gamedb.json"))
        with open(db_path, encoding="utf-8") as f:
            db = json.load(f)
        _NAMES = {o["$uid"]: o.get("mDisplayName") or "(unnamed)" for o in db["UFG::StoreOutfit"]}
    return _NAMES


def server_running(port=443):
    s = socket.socket()
    s.settimeout(0.5)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def load(path):
    if not os.path.isfile(path):
        sys.exit("no profile at %s - start the game once first" % path)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, path)


def stock(player, uids):
    """Add store items to the inventory if they are not there yet; return how many."""
    bag = player.setdefault("InventoryBag", [])
    account = player["PlayerDataBag"]["AccountID"]
    added = 0
    for uid in uids:
        if any(i.get("StoreItemID") == uid for i in bag):
            continue
        next_id = max([7000] + [int(i.get("InventoryItemID") or 0) for i in bag]) + 1
        bag.append(state.inventory_item(next_id, uid, account))
        added += 1
    return added


def wear(player, outfit_uid):
    """Set the outfit and its pieces on the player; return the pieces and what was added."""
    pieces = (catalogue().get("pieces") or {}).get(outfit_uid) or []
    custom = player.setdefault("CustomizationBag", state.new_customization_bag())
    custom["ClothingOutfitCortexUID"] = outfit_uid
    custom["ClothingStoreItemUIDs"] = list(pieces)
    return pieces, stock(player, [outfit_uid] + list(pieces))


def wear_look(player, look):
    """Wear a look: its pieces, and no outfit behind them."""
    pieces = [piece["uid"] for piece in look["pieces"]]
    custom = player.setdefault("CustomizationBag", state.new_customization_bag())
    custom["ClothingOutfitCortexUID"] = "0"
    custom["ClothingStoreItemUIDs"] = list(pieces)
    return pieces, stock(player, pieces)


def identify(pieces):
    """The look whose pieces these are, if any."""
    wanted = sorted(pieces)
    for look in looks():
        if sorted(piece["uid"] for piece in look["pieces"]) == wanted:
            return look
    return None


def _rank(name, wanted):
    name = (name or "").lower()
    if name == wanted:
        return 0
    if name.startswith(wanted):
        return 1
    if wanted in name:
        return 2
    return None


def candidates(argument, kind=None):
    """[(rank, kind, item)] for everything that could be meant by `argument`."""
    wanted = argument.strip().lower()
    found = []
    if kind in (None, "look"):
        for look in looks():
            ranks = [r for r in (_rank(look["id"].replace("-", " "), wanted),
                                 _rank(look["name"], wanted)) if r is not None]
            if ranks:
                found.append((min(ranks), "look", look))
    if kind in (None, "outfit"):
        for uid in catalogue()["items"]:
            rank = _rank(names().get(uid, ""), wanted)
            if rank is not None:
                found.append((rank, "outfit", uid))
    return sorted(found, key=lambda f: (f[0], f[2] if f[1] == "outfit" else f[2]["name"]))


def resolve(argument):
    """The one thing `argument` means, or a message listing the candidates."""
    kind = None
    words = argument.strip().split(None, 1)
    if len(words) == 2 and words[0].lower() in ("look", "looks"):
        kind, argument = "look", words[1]
    elif len(words) == 2 and words[0].lower() in ("outfit", "outfits"):
        kind, argument = "outfit", words[1]
    hits = candidates(argument, kind)
    if not hits:
        theme = [look for look in looks() if look["theme"].lower() == argument.strip().lower()]
        if theme:
            sys.exit("%r is a theme: %s" % (argument, ", ".join(look["name"] for look in theme)))
        sys.exit("nothing matches %r - try 'looks' or 'list'" % argument)
    best = [hit for hit in hits if hit[0] == hits[0][0]]
    if len(best) == 1:
        return best[0][1], best[0][2]
    lines = ["%r matches %d things:" % (argument, len(best))]
    for _, hit_kind, item in best:
        if hit_kind == "look":
            lines.append("  look    %s" % item["name"])
        else:
            lines.append("  outfit  %s" % names().get(item, item))
    lines.append("say more, or pick the kind: set look <name> / set outfit <name>")
    sys.exit("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description="Wear an outfit or one of the curated looks.")
    ap.add_argument("command", choices=("looks", "list", "set", "show", "clear", "menu"),
                    nargs="?", default="looks")
    ap.add_argument("what", nargs="*", help="look or outfit name fragment, or an outfit UID")
    ap.add_argument("--state", default=os.path.join(HERE, "..", "state.json"))
    ap.add_argument("--npc", action="store_true", help="list only the non-player outfits")
    ap.add_argument("--force", action="store_true", help="edit even if the server is running")
    args = ap.parse_args()
    path = os.path.abspath(args.state)
    outfits = catalogue()

    if args.command == "looks":
        lookbook.print_book()
        return 0

    if args.command == "list":
        for uid in outfits["items"]:
            if args.npc and uid not in NPC_OUTFITS:
                continue
            print("%-22s %-30s %s" % (uid, names().get(uid, "?"),
                                     "  <- non-player model" if uid in NPC_OUTFITS else ""))
        print("%d outfits, %d of them non-player models" % (
            len(outfits["items"]),
            len([u for u in outfits["items"] if u in NPC_OUTFITS])))
        return 0

    data = load(path)
    player = data["player"]

    if args.command == "show":
        custom = player.get("CustomizationBag") or {}
        uid = custom.get("ClothingOutfitCortexUID", "0")
        pieces = custom.get("ClothingStoreItemUIDs") or []
        look = identify(pieces) if pieces else None
        if look:
            print("look   : %s (%s)" % (look["name"], look["theme"]))
        if look:
            print("outfit : 0 (a look, not one of the game's outfits)")
        else:
            print("outfit : %s %s" % (uid, names().get(uid, "") if uid != "0" else "(default)"))
        if look:
            print("pieces : %s" % lookbook.describe(look))
        else:
            print("pieces : %s" % (", ".join(pieces) or "(none)"))
        print("preset : %s" % custom.get("ScalingPresetCortexUID", "0"))
        return 0

    if not args.force and server_running():
        sys.exit("the local server is running on port 443; close the game and the server "
                 "window first (or pass --force to edit anyway)")

    if args.command == "clear":
        player["CustomizationBag"] = state.new_customization_bag()
        save(path, data)
        print("back to the default model")
        return 0

    if args.command == "menu":
        chosen = None
        while chosen is None:
            chosen = _menu_choice()
            if chosen is False:
                return 0
        kind, item = "look", chosen
    else:
        if not args.what:
            sys.exit("say which look or outfit: python playermodel.py set \"swat pointman\"")
        kind, item = resolve(" ".join(args.what))

    if kind == "look":
        pieces, added = wear_look(player, item)
        save(path, data)
        print("wearing %s (%s)" % (item["name"], item["theme"]))
        print("pieces : %s" % lookbook.describe(item))
        print("%d item(s) added to the inventory" % added)
    else:
        pieces, added = wear(player, item)
        save(path, data)
        print("wearing %s (%s)" % (names().get(item, "?"), item))
        print("pieces : %s" % (", ".join(pieces) or "(none)"))
        print("%d item(s) added to the inventory" % added)
    print("start the game again to see it.")
    return 0


def _menu_choice():
    """Numbered lookbook on stdout, one answer from stdin: a look, or False to quit."""
    looks_ = looks()
    width = max(len(look["name"]) for look in looks_)
    numbered = []
    print("\n  OUTFITS TO WEAR")
    for theme in lookbook.THEMES:
        group = [look for look in looks_ if look["theme"] == theme]
        if not group:
            continue
        print("\n  %s" % theme.upper())
        for look in group:
            numbered.append(look)
            print("   %2d  %-*s  %s" % (len(numbered), width, look["name"],
                                        lookbook.describe(look)))
    print()
    while True:
        try:
            answer = input("  Pick a number (q to quit): ").strip()
        except EOFError:
            print()
            return False
        if not answer:
            continue
        if answer.lower() in ("q", "quit", "exit"):
            return False
        if answer.isdigit() and 1 <= int(answer) <= len(numbered):
            return numbered[int(answer) - 1]
        hits = [look for look in looks_ if answer.lower() in look["name"].lower()]
        if len(hits) == 1:
            return hits[0]
        print("  no single match for %r - try a number from the list" % answer)


if __name__ == "__main__":
    sys.exit(main())
