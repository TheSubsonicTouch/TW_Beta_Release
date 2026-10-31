"""Build data/store_items.json: the catalogue each store hands out.

    python store_items.py [gamedb.json]        (the file written by `gamedb.py export`;
                                                default: ../devdata/gamedb.json)

The original server decided which items a store sold; that list is not in the client.
What the client does have is every store item object, and each `UFG::Store` names the
one item type it accepts (`mAllowedStoreItemType`), so the catalogue of a store is every
usable item of that type.

What "usable" means, type by type:

  * clothing - a finished wardrobe piece: it has a category (`mSubType`, "Tops|Casual")
    and the developers did not label it "NOT READY FOR STORE YET" or "TO BE DELETED".
    Pieces with no category exist only as parts of an outfit and are not sold alone.
  * outfit - every piece it is made of exists. The label on a piece does not matter:
    that is how the twelve non-player looks (Swat Uniform, Zombie Enforcer, Agent 47,
    Deus Ex, Sun Wukong, ...) are shipped - their head or hands are marked "NOT READY
    FOR STORE YET" because the wardrobe never sold them loose, but the model is in the
    game and NPCs wear it. Selling the outfit is what unlocks those models for the
    player. `pieces` keeps the clothing UIDs so the server can put them on.
  * vehicle - every `UFG::StoreVehicle` with a property set (the model to spawn).
  * vendor item - the health vendor's one consumable (paid for, nothing stored).

Favour packs take two stores each, both picked by hard-coded UID in the client:

  * a pack store (UFG::StoreContactBundle items), which the favour shop lists as tiles
    (TriadWars.exe func 140654510). The tutorial pack stores hold the one pack their
    tutorial expects; the regular store gets every pack that can be bought.
  * a bundle store (UFG::StoreBundle items), which the pack details screen searches for
    bundles made of the selected pack: 1x, 3x and 5x (func 1407100c0, func 1406a2dc0).
    The bundle is what is actually bought, and the 1x bundle supplies the price shown, so
    every listed pack needs one - a pack without a 1x bundle is left out, because
    selecting it crashes the details screen (TriadWars.exe!1406a34ec).

The Gold Store is left out on purpose: its items are `BeginExternalPurchase`, i.e. a
real-money purchase through Steam that cannot happen offline.
"""
import json
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "store_items.json")
# mSubType doubles as the wardrobe category path ("Tops|Casual"). Items the developers
# had not finished carry one of these labels instead.
UNFINISHED = ("NOT READY FOR STORE YET", "TO BE DELETED BY LGP")

RAID_TUTORIAL_PACK = "16287717993347821376"
LEVEL_UP_TUTORIAL_PACK = "12197698851607134720"
PRODUCTION_TUTORIAL_PACK = "11938320311128384764"

# pack store UID -> the packs it lists. None means "every pack that can be bought".
PACK_STORES = {
    "17679317271361684130": None,                           # Favor pack - Pack Store
    "1333401478411759499": [RAID_TUTORIAL_PACK],            # Raid Pack Tutorial - Pack Store
    "6121745478303023351": [LEVEL_UP_TUTORIAL_PACK],        # Level Up Pack Tutorial - Pack Store
    "6802491388125254368": [PRODUCTION_TUTORIAL_PACK],      # Production Pack Tutorial - Pack Store
}
# bundle store UID -> the packs whose bundles it sells. None means "every pack".
BUNDLE_STORES = {
    "2541658861465094719": None,                            # Favour Pack Bundle Store
    "14170981830216607711": None,                           # Bundle Store
    "14396531704373406508": [RAID_TUTORIAL_PACK],           # Raid Pack Tutorial - Bundle Store
    "15645208897816992095": [LEVEL_UP_TUTORIAL_PACK],       # Level Up Pack Tutorial Store
    "2975199966773664597": [PRODUCTION_TUTORIAL_PACK],      # Production Pack Tutorial Bundle Store
    # The client really does use this one as the *bundle* store for the racket-boss
    # (production pack) tutorial, whatever its own mAllowedStoreItemType says.
    "330971609030659674": [PRODUCTION_TUTORIAL_PACK],       # Counterfeiting Favor Pack Tutorial
}


def main(path):
    with open(path, encoding="utf-8") as f:
        db = json.load(f)
    clothing = {o["$uid"]: o for o in db["UFG::StoreClothing"]}
    outfits = {o["$uid"]: o for o in db["UFG::StoreOutfit"]}
    packs = {o["$uid"]: o for o in db["UFG::StoreContactBundle"]}
    bundles = {o["$uid"]: [h["$handle"] for h in o.get("mContents") or []]
               for o in db["UFG::Bundle"]}

    def wardrobe_ready(item):
        return item["mSubType"] not in UNFINISHED + ("",)

    def outfit_pieces(item):
        return [h["$handle"] for h in item.get("mClothingItems") or [] if h.get("$handle") in clothing]

    def outfit_ready(item):
        pieces = [h["$handle"] for h in item.get("mClothingItems") or []]
        return bool(pieces) and all(p in clothing for p in pieces)

    # pack uid -> {bundle size: store bundle uid}
    bundles_by_pack = {}
    for item in db["UFG::StoreBundle"]:
        contents = bundles.get((item.get("mBundle") or {}).get("$handle"), [])
        if contents and len(set(contents)) == 1 and contents[0] in packs:
            bundles_by_pack.setdefault(contents[0], {})[len(contents)] = item["$uid"]

    buyable_packs = sorted(p for p in packs if 1 in bundles_by_pack.get(p, {}))
    for required in (RAID_TUTORIAL_PACK, LEVEL_UP_TUTORIAL_PACK, PRODUCTION_TUTORIAL_PACK):
        assert required in buyable_packs, "tutorial pack %s has no 1x bundle" % required

    def pack_bundles(wanted):
        """{store bundle uid: [pack uid, ...]} for bundles made only of `wanted` packs."""
        found = {}
        for pack in wanted:
            for size, uid in bundles_by_pack.get(pack, {}).items():
                found[uid] = [pack] * size
        for pack in wanted:         # the details screen crashes without the 1x bundle
            assert [pack] in found.values(), "no 1x bundle for pack %s" % pack
        return found

    def order_bundles(found):
        return sorted(found, key=lambda b: (found[b][0], len(found[b])))

    out = {}
    for store in db["UFG::Store"]:
        uid, kind = store["$uid"], store["mAllowedStoreItemType"]
        entry = {"name": store["mDisplayName"]}
        if uid in PACK_STORES:
            wanted = PACK_STORES[uid] or buyable_packs
            assert set(wanted) <= set(packs), sorted(set(wanted) - set(packs))
            entry.update(type="UFG::StoreContactBundle", items=list(wanted))
        elif uid in BUNDLE_STORES:
            found = pack_bundles(BUNDLE_STORES[uid] or buyable_packs)
            entry.update(type="UFG::StoreBundle", items=order_bundles(found), packs=found)
        elif kind == "UFG::StoreClothing":
            entry.update(type=kind, items=sorted((o["$uid"] for o in db[kind]
                                                  if wardrobe_ready(o)), key=int))
        elif kind == "UFG::StoreOutfit":
            ready = [o for o in db[kind] if outfit_ready(o)]
            entry.update(type=kind,
                         items=sorted((o["$uid"] for o in ready), key=int),
                         pieces={o["$uid"]: outfit_pieces(o) for o in ready})
        elif kind == "UFG::StoreVehicle":
            entry.update(type=kind, items=sorted((o["$uid"] for o in db[kind]
                                                  if o.get("mPropertySet")), key=int))
        elif kind == "UFG::VendorItem":
            entry.update(type=kind, items=sorted((o["$uid"] for o in db[kind]), key=int))
        else:
            continue            # the gold store: real money, nothing this server can sell
        out[uid] = entry
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    for uid, s in out.items():
        print("%-20s %-38s %-24s %4d items" % (uid, s["name"], s["type"], len(s["items"])))
    print("%d stores, %d buyable packs, %d outfits (of %d), %d clothing pieces (of %d)"
          % (len(out), len(buyable_packs),
             len(out["13160186718612611233"]["items"]), len(outfits),
             len(out["3503175499248592696"]["items"]), len(clothing)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(
        os.path.dirname(OUT), "..", "devdata", "gamedb.json")))
