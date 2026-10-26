"""Exercise every shop without starting the server.

    python test_shops.py

Checks the two things that matter about the catalogues - that every store the client
has is filled, and that every entry is a real object of the right type - and then buys
one item of each kind to make sure it is charged for and stored where the client looks
for it: clothing, an outfit (which also goes straight onto the model), a vehicle, a
pack and the health vendor.

Two rules come from crashes in the client and are asserted here:

  * a pack listed in a pack store must have a 1x bundle somewhere, or selecting it
    takes its price from a bundle that is not there (TriadWars.exe!1406a34ec);
  * every outfit piece must be a real UFG::StoreClothing, or the wardrobe reads a
    definition that does not exist.
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import handlers  # noqa: E402
import state  # noqa: E402

DB_PATH = os.path.normpath(os.path.join(HERE, "..", "devdata", "gamedb.json"))


def main():
    workdir = tempfile.mkdtemp(prefix="tw-shops-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    player = handlers._player()
    with open(DB_PATH, encoding="utf-8") as f:
        db = json.load(f)
    uids = {kind: {o["$uid"] for o in db[kind]} for kind in
            ("UFG::StoreClothing", "UFG::StoreOutfit", "UFG::StoreVehicle",
             "UFG::VendorItem", "UFG::StoreContactBundle", "UFG::StoreBundle")}
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-34s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-34s %s" % (name, e))

    def every_store_filled():
        assert len(state.STORE_ITEMS) == 14, len(state.STORE_ITEMS)
        for uid, catalogue in state.STORE_ITEMS.items():
            assert catalogue["items"], "store %s (%s) is empty" % (uid, catalogue["name"])
            assert catalogue["type"] in handlers.STORE_PRICES or \
                catalogue["type"] == "UFG::StoreBundle", catalogue["type"]
        return "%d stores, %d items" % (
            len(state.STORE_ITEMS), sum(len(c["items"]) for c in state.STORE_ITEMS.values()))

    def items_exist():
        for uid, catalogue in state.STORE_ITEMS.items():
            kind = catalogue["type"]
            if kind == "UFG::StoreBundle":
                missing = set(catalogue["items"]) - uids["UFG::StoreBundle"]
            else:
                missing = set(catalogue["items"]) - uids[kind]
            assert not missing, "%s (%s): %s" % (uid, catalogue["name"], sorted(missing)[:3])
        return "every item exists in the game data"

    def outfits_have_pieces():
        catalogue = next(c for c in state.STORE_ITEMS.values()
                         if c["type"] == "UFG::StoreOutfit")
        pieces = catalogue.get("pieces") or {}
        assert len(pieces) == len(catalogue["items"]), (len(pieces), len(catalogue["items"]))
        for uid, parts in pieces.items():
            assert parts, "outfit %s has no pieces" % uid
            assert set(parts) <= uids["UFG::StoreClothing"], (uid, parts)
        npc = [u for u in catalogue["items"] if u in
               ("17542338473309454167", "10951711769280898226", "1830394833577068751")]
        assert len(npc) == 3, npc
        return "%d outfits, %d pieces each on average" % (
            len(catalogue["items"]), sum(len(p) for p in pieces.values()) // len(pieces))

    def packs_have_a_1x_bundle():
        single = {}
        for catalogue in state.STORE_ITEMS.values():
            if catalogue["type"] == "UFG::StoreBundle":
                for bundle, packs in catalogue["packs"].items():
                    if len(packs) == 1:
                        single[packs[0]] = bundle
        for catalogue in state.STORE_ITEMS.values():
            if catalogue["type"] == "UFG::StoreContactBundle":
                for pack in catalogue["items"]:
                    assert pack in single, "pack %s has no 1x bundle" % pack
        return "%d packs, all with a 1x bundle" % len(single)

    def store_answers():
        for uid, catalogue in state.STORE_ITEMS.items():
            reply = handlers.store(None, "GET", ["store", uid], b"", {})
            assert len(reply["data"]) == len(catalogue["items"]), uid
            first = reply["data"][0]
            assert first["mCortexUID"] in catalogue["items"]
            assert isinstance(first["mPrice"], int) and first["mPrice"] > 0, first
            assert first["mStock"] == -1
        assert handlers.store(None, "GET", ["store", "123"], b"", {}) == {}
        return "%d catalogues served" % len(state.STORE_ITEMS)

    def buy(store_uid, item_uid):
        return handlers.purchase(None, "POST", ["purchase", store_uid, item_uid], b"", {})

    def refill(dirty="100000", clean="100000"):
        player["CurrencyBag"]["DirtyMoney"] = dirty
        player["CurrencyBag"]["CleanMoney"] = clean

    def money():
        return int(player["CurrencyBag"]["DirtyMoney"]), int(player["CurrencyBag"]["CleanMoney"])

    def buy_clothing():
        refill()
        uid, catalogue = next((u, c) for u, c in state.STORE_ITEMS.items()
                              if c["type"] == "UFG::StoreClothing")
        item = catalogue["items"][0]
        before = money()[0]
        reply = buy(uid, item)
        assert reply["mReturnCode"] == 0, reply
        assert money()[0] == before - 250, (before, money())
        owned = [i["StoreItemID"] for i in player["InventoryBag"]]
        assert item in owned, owned[-3:]
        assert reply["data"]["mDataBags"]["InventoryBag"]
        return "charged 250, stored in the inventory"

    def buy_outfit():
        refill()
        uid, catalogue = next((u, c) for u, c in state.STORE_ITEMS.items()
                              if c["type"] == "UFG::StoreOutfit")
        item = catalogue["items"][0]
        pieces = catalogue["pieces"][item]
        before = money()[0]
        reply = buy(uid, item)
        assert reply["mReturnCode"] == 0, reply
        assert money()[0] == before - 1500
        owned = [i["StoreItemID"] for i in player["InventoryBag"]]
        assert item in owned and all(p in owned for p in pieces), owned[-6:]
        custom = player["CustomizationBag"]
        assert custom["ClothingOutfitCortexUID"] == item, custom
        assert custom["ClothingStoreItemUIDs"] == pieces, custom
        assert reply["data"]["mDataBags"]["CustomizationBag"]["ClothingOutfitCortexUID"] == item
        return "worn straight away, %d pieces stored" % len(pieces)

    def buy_vehicle():
        refill()
        uid, catalogue = next((u, c) for u, c in state.STORE_ITEMS.items()
                              if c["type"] == "UFG::StoreVehicle")
        item = catalogue["items"][0]
        before = money()[0]
        buy(uid, item)
        assert money()[0] == before - 4000
        assert item in [i["StoreItemID"] for i in player["InventoryBag"]]
        return "charged 4000, stored in the inventory"

    def buy_vendor_item():
        refill()
        uid, catalogue = next((u, c) for u, c in state.STORE_ITEMS.items()
                              if c["type"] == "UFG::VendorItem")
        item = catalogue["items"][0]
        before, owned = money()[0], len(player["InventoryBag"])
        buy(uid, item)
        assert money()[0] == before - 500
        assert len(player["InventoryBag"]) == owned, "a consumable was stored"
        return "charged 500, nothing stored"

    def buy_pack():
        refill()
        uid, catalogue = next((u, c) for u, c in state.STORE_ITEMS.items()
                              if c["type"] == "UFG::StoreBundle")
        item = catalogue["items"][0]
        target = next(b for b, packs in catalogue["packs"].items() if [packs[0]] == packs)
        favours = len(player["FavoursBag"])
        before = money()[1]
        reply = buy(uid, target)
        assert reply["mReturnCode"] == 0, reply
        assert len(player["FavoursBag"]) > favours
        assert money()[1] == before - int(100 * len(catalogue["packs"][target]))
        assert len(reply["data"]["ItemContext"]) == len(catalogue["packs"][target])
        return "%d favour(s) from a %dx pack" % (
            len(player["FavoursBag"]) - favours, len(catalogue["packs"][target]))

    def cannot_afford():
        player["CurrencyBag"]["DirtyMoney"] = "0"
        uid, catalogue = next((u, c) for u, c in state.STORE_ITEMS.items()
                              if c["type"] == "UFG::StoreVehicle")
        reply = buy(uid, catalogue["items"][0])
        assert reply["mReturnCode"] == 1 and "afford" in reply["mMessage"], reply
        return "refused politely"

    def playermodel_tool():
        sys.path.insert(0, HERE)
        import playermodel
        uid = "10951711769280898226"                     # Zombie Enforcer
        pieces, added = playermodel.wear(player, uid)
        assert player["CustomizationBag"]["ClothingOutfitCortexUID"] == uid
        assert player["CustomizationBag"]["ClothingStoreItemUIDs"] == pieces
        assert pieces and added >= 1, (pieces, added)
        named = playermodel.names()[uid]
        assert named == "Zombie Enforcer", named
        return "%s -> %d pieces" % (named, len(pieces))

    check("every store is filled", every_store_filled)
    check("items exist in the game data", items_exist)
    check("outfits have real pieces", outfits_have_pieces)
    check("packs have a 1x bundle", packs_have_a_1x_bundle)
    check("store catalogues served", store_answers)
    check("buying clothing", buy_clothing)
    check("buying an outfit", buy_outfit)
    check("buying a vehicle", buy_vehicle)
    check("buying a health item", buy_vendor_item)
    check("buying a pack", buy_pack)
    check("too little money", cannot_afford)
    check("playermodel tool", playermodel_tool)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
