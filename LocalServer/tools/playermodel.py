"""Put one of the game's outfits - including the non-player models - on the player.

    python playermodel.py list                 the looks, with their UIDs
    python playermodel.py list --npc           only the non-player models
    python playermodel.py set swat             wear one (unique name fragment or UID)
    python playermodel.py set 17542338473309454167
    python playermodel.py show                 what the player is wearing now
    python playermodel.py clear                back to the default model

Close the game and stop the server first: the server keeps `state.json` in memory and
writes it back, so an edit made underneath it is lost on the next request.

What this writes is the player's CustomizationBag - the same three fields the wardrobe
screen sends to `enforcer/customization`:

  ClothingOutfitCortexUID   the UFG::StoreOutfit being worn
  ClothingStoreItemUIDs     the UFG::StoreClothing pieces it is made of; this is what
                            the client draws, so these are the model, not the outfit name
  ScalingPresetCortexUID    the body preset, left alone ("0" = as created)

The outfit and its pieces are also added to the inventory, so the wardrobe shows them as
owned instead of offering them for sale again.
"""
import argparse
import json
import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import state  # noqa: E402

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


def wear(player, outfit_uid):
    """Set the outfit and its pieces on the player; return what was added to the bag."""
    pieces = (catalogue().get("pieces") or {}).get(outfit_uid) or []
    custom = player.setdefault("CustomizationBag", state.new_customization_bag())
    custom["ClothingOutfitCortexUID"] = outfit_uid
    custom["ClothingStoreItemUIDs"] = list(pieces)
    bag = player.setdefault("InventoryBag", [])
    added = 0
    for uid in [outfit_uid] + list(pieces):
        if any(i.get("StoreItemID") == uid for i in bag):
            continue
        next_id = max([7000] + [int(i.get("InventoryItemID") or 0) for i in bag]) + 1
        bag.append(state.inventory_item(next_id, uid, player["PlayerDataBag"]["AccountID"]))
        added += 1
    return pieces, added


def match(argument, outfits):
    if argument in outfits["items"]:
        return argument
    wanted = argument.strip().lower()
    exact = [uid for uid in outfits["items"] if names().get(uid, "").lower() == wanted]
    if len(exact) == 1:
        return exact[0]
    starts = [uid for uid in outfits["items"]
              if names().get(uid, "").lower().startswith(wanted)]
    hits = starts or [uid for uid in outfits["items"] if wanted in names().get(uid, "").lower()]
    if len(hits) == 1:
        return hits[0]
    if not hits:
        sys.exit("no outfit matches %r - try 'list'" % argument)
    sys.exit("%r matches %d outfits: %s" % (
        argument, len(hits), ", ".join(names().get(u, u) for u in hits)))


def main():
    ap = argparse.ArgumentParser(description="Wear one of the game's outfits.")
    ap.add_argument("command", choices=("list", "set", "show", "clear"), nargs="?", default="list")
    ap.add_argument("what", nargs="?", help="outfit name fragment or UID")
    ap.add_argument("--state", default=os.path.join(HERE, "..", "state.json"))
    ap.add_argument("--npc", action="store_true", help="list only the non-player models")
    ap.add_argument("--force", action="store_true", help="edit even if the server is running")
    args = ap.parse_args()
    path = os.path.abspath(args.state)
    outfits = catalogue()

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
        print("outfit : %s %s" % (uid, names().get(uid, "") if uid != "0" else "(default)"))
        print("pieces : %s" % (", ".join(custom.get("ClothingStoreItemUIDs") or []) or "(none)"))
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

    if not args.what:
        sys.exit("say which outfit: python playermodel.py set swat")
    uid = match(args.what, outfits)
    pieces, added = wear(player, uid)
    save(path, data)
    print("wearing %s (%s)" % (names().get(uid, "?"), uid))
    print("pieces : %s" % ", ".join(pieces))
    print("%d item(s) added to the inventory" % added)
    print("start the game again to see it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
