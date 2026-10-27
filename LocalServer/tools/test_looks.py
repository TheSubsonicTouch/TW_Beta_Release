"""Check the outfit lookbook without starting the server.

    python test_looks.py

A look is a named combination of `UFG::StoreClothing` pieces, worn by writing the same
`CustomizationBag` fields the wardrobe writes. The checks here are the ones that keep
that honest:

  * the lookbook on disk is up to date with the table in `tools/looks.py`, and every
    piece resolves to a real object in the game's own data - no invented UIDs, because
    the client looks every UID up in its own database and a miss is a null pointer;
  * no look asks for two pieces in one wardrobe slot, so it is a combination the
    wardrobe itself would allow;
  * every piece is one the player can also reach in the game: sold loose in the
    wardrobe, or part of an outfit the shops sell;
  * every look applies cleanly - the bag is written, the pieces land in the inventory,
    the server sends the bag at login, and `show` recognises what is worn.
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))
sys.path.insert(0, HERE)

import handlers  # noqa: E402
import looks as lookbook  # noqa: E402
import playermodel  # noqa: E402
import state  # noqa: E402

DB_PATH = lookbook.DB_PATH


def main():
    workdir = tempfile.mkdtemp(prefix="tw-looks-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    with open(DB_PATH, encoding="utf-8") as f:
        db = json.load(f)
    clothing = {record["$uid"]: record for record in db["UFG::StoreClothing"]}
    outfits = {outfit["$uid"]: outfit for outfit in db["UFG::StoreOutfit"]}
    sold = set()
    for catalogue in state.STORE_ITEMS.values():
        if catalogue["type"] == "UFG::StoreClothing":
            sold |= set(catalogue["items"])
    in_outfits = {piece["$handle"] for outfit in outfits.values()
                  for piece in outfit.get("mClothingItems") or []}
    book = lookbook.load()
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-34s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-34s %s" % (name, e))

    def in_sync():
        tmp = os.path.join(workdir, "looks.json")
        lookbook.build(DB_PATH, tmp, verbose=False)
        with open(tmp, encoding="utf-8") as f:
            rebuilt = f.read()
        with open(lookbook.OUT_PATH, encoding="utf-8") as f:
            on_disk = f.read()
        assert rebuilt == on_disk, "data/looks.json is stale - run: python tools/looks.py"
        return "%d looks" % len(book)

    def every_piece_is_real():
        problems = lookbook.check()
        assert not problems, "; ".join(problems)
        unknown = [(look["id"], piece["uid"]) for look in book for piece in look["pieces"]
                   if piece["uid"] not in clothing]
        assert not unknown, unknown
        return "%d pieces, all UFG::StoreClothing" % len({p["uid"] for l in book for p in l["pieces"]})

    def one_piece_per_slot():
        for look in book:
            slots = [piece["slot"] for piece in look["pieces"]]
            assert len(slots) == len(set(slots)), "%s: %s" % (look["id"], slots)
        return "%d looks, no slot used twice" % len(book)

    def every_theme_covered():
        themes = [t for t in lookbook.THEMES]
        counts = {t: len([l for l in book if l["theme"] == t]) for t in themes}
        assert all(counts.values()), counts
        assert all(count >= 3 for count in counts.values()), counts
        assert set(l["theme"] for l in book) <= set(themes), set(l["theme"] for l in book)
        return ", ".join("%s %d" % (t, n) for t, n in counts.items())

    def reachable_by_hand():
        """Every piece can also be had in the game, so no look shows a ghost garment."""
        stranded = sorted({(look["id"], piece["name"]) for look in book
                           for piece in look["pieces"]
                           if piece["uid"] not in sold and piece["uid"] not in in_outfits})
        assert not stranded, stranded
        return "all in the wardrobe catalogue or an outfit"

    def every_look_applies():
        player = handlers._player()
        for look in book:
            player["InventoryBag"] = []
            pieces, added = playermodel.wear_look(player, look)
            assert pieces == [piece["uid"] for piece in look["pieces"]], look["id"]
            custom = player["CustomizationBag"]
            assert custom["ClothingOutfitCortexUID"] == "0", look["id"]
            assert custom["ClothingStoreItemUIDs"] == pieces, look["id"]
            owned = {item["StoreItemID"] for item in player["InventoryBag"]}
            assert set(pieces) <= owned, "%s: %s missing" % (look["id"], set(pieces) - owned)
            assert added == len(pieces), (look["id"], added)
            assert (playermodel.identify(pieces) or {}).get("id") == look["id"], look["id"]
        return "all %d looks wear, own their pieces and read back" % len(book)

    def server_sends_the_look():
        player = handlers._player()
        look = book[0]
        playermodel.wear_look(player, look)
        handlers.STATE.save()
        bags = handlers.STATE.player_bags(state.BAG_CUSTOMIZATION | state.BAG_INVENTORY)
        assert bags["CustomizationBag"]["ClothingStoreItemUIDs"] == \
            [piece["uid"] for piece in look["pieces"]]
        assert bags["mRequestedBags"] & state.BAG_CUSTOMIZATION
        return "%s via mDataBags" % look["name"]

    def saved_and_reloaded():
        player = handlers._player()
        look = [l for l in book if l["theme"] == "Zombie"][0]
        playermodel.wear_look(player, look)
        handlers.STATE.save()
        again = state.State(handlers.STATE.path)
        custom = again.data["player"]["CustomizationBag"]
        assert custom["ClothingStoreItemUIDs"] == [piece["uid"] for piece in look["pieces"]]
        return "survives a restart"

    check("lookbook matches its table", in_sync)
    check("every piece is real", every_piece_is_real)
    check("one piece per slot", one_piece_per_slot)
    check("every theme covered", every_theme_covered)
    check("pieces obtainable in game", reachable_by_hand)
    check("every look applies", every_look_applies)
    check("server sends the look", server_sends_the_look)
    check("look survives a restart", saved_and_reloaded)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
