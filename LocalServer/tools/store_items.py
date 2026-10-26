"""Build data/store_items.json: the catalogue each store hands out.

    python store_items.py [gamedb.json]        (the file written by `gamedb.py export`;
                                                default: ../devdata/gamedb.json)

The original server decided which items a store sold; that list is not in the client.
What the client does have is every store item object, and each UFG::Store names the one
item type it accepts (mAllowedStoreItemType), so the catalogue of the two stores behind
the wardrobe screen is simply every finished item of that type.

Favour packs take two stores each, both picked by hard-coded UID in the client:

  * a pack store (UFG::StoreContactBundle items), which the favour shop lists as tiles
    (TriadWars.exe func 140654510). The three tutorial pack stores hold the one pack the
    tutorial expects; the regular store gets the four packs the game's bundles are built
    from.
  * a bundle store (UFG::StoreBundle items), which the pack details screen searches for
    bundles made of the selected pack: 1x, 3x and 5x (func 1407100c0, func 1406a2dc0).
    The bundle is what is actually bought, and the 1x bundle supplies the price shown, so
    every listed pack needs one.

Stores not covered here keep answering with an empty catalogue.
"""
import json
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "store_items.json")
STORE_TYPES = ("UFG::StoreClothing", "UFG::StoreOutfit")
# mSubType doubles as the wardrobe category path ("Tops|Casual"). Items the developers
# had not finished carry one of these labels instead. Pieces with no category at all are
# not sold on their own; they exist only as parts of an outfit.
UNFINISHED = ("NOT READY FOR STORE YET", "TO BE DELETED BY LGP")

REGULAR_PACKS = [
    "10136327405205463185",             # Raid Pack
    "12310364248914734042",             # Level Up Pack
    "1425082927751523674",              # Epic Level Up Pack
    "17409356890332470585",             # Production Pack
]
RAID_TUTORIAL_PACK = "16287717993347821376"
LEVEL_UP_TUTORIAL_PACK = "12197698851607134720"
PRODUCTION_TUTORIAL_PACK = "11938320311128384764"

# pack store UID -> the packs it lists
PACK_STORES = {
    "17679317271361684130": REGULAR_PACKS,                  # Favor pack - Pack Store
    "1333401478411759499": [RAID_TUTORIAL_PACK],            # Raid Pack Tutorial - Pack Store
    "6121745478303023351": [LEVEL_UP_TUTORIAL_PACK],        # Level Up Pack Tutorial - Pack Store
    "6802491388125254368": [PRODUCTION_TUTORIAL_PACK],      # Production Pack Tutorial - Pack Store
}
# bundle store UID -> the packs whose bundles it sells
BUNDLE_STORES = {
    "2541658861465094719": REGULAR_PACKS,                   # Favour Pack Bundle Store
    "14396531704373406508": [RAID_TUTORIAL_PACK],           # Raid Pack Tutorial - Bundle Store
    "15645208897816992095": [LEVEL_UP_TUTORIAL_PACK],       # Level Up Pack Tutorial Store
    # The client really does use this store for the racket-boss (production pack) tutorial.
    "330971609030659674": [PRODUCTION_TUTORIAL_PACK],       # Counterfeiting Favor Pack Tutorial
}


def main(path):
    with open(path, encoding="utf-8") as f:
        db = json.load(f)
    clothing = {o["$uid"]: o for o in db["UFG::StoreClothing"]}
    pack_uids = {o["$uid"] for o in db["UFG::StoreContactBundle"]}
    bundles = {o["$uid"]: [h["$handle"] for h in o["mContents"]] for o in db["UFG::Bundle"]}

    def sellable(item):
        if item["$type"] == "UFG::StoreClothing":
            return item["mSubType"] not in UNFINISHED + ("",)
        # an outfit is sellable when every piece it is made of exists and is finished
        pieces = [h["$handle"] for h in item["mClothingItems"] if h]
        return bool(pieces) and all(p in clothing and clothing[p]["mSubType"] not in UNFINISHED
                                    for p in pieces)

    def pack_bundles(packs):
        """{store bundle uid: [pack uid, ...]} for every bundle made only of one of `packs`."""
        found = {}
        for item in db["UFG::StoreBundle"]:
            contents = bundles[item["mBundle"]["$handle"]]
            if contents and contents[0] in packs and len(set(contents)) == 1:
                found[item["$uid"]] = contents
        for pack in packs:      # the details screen crashes without the single-pack bundle
            assert [pack] in found.values(), "no 1x bundle for pack %s" % pack
        return found

    out = {}
    for store in db["UFG::Store"]:
        uid, kind = store["$uid"], store["mAllowedStoreItemType"]
        entry = {"name": store["mDisplayName"]}
        if uid in PACK_STORES:
            assert pack_uids.issuperset(PACK_STORES[uid])
            entry.update(type="UFG::StoreContactBundle", items=PACK_STORES[uid])
        elif uid in BUNDLE_STORES:
            packs = pack_bundles(BUNDLE_STORES[uid])
            entry.update(type="UFG::StoreBundle", items=sorted(packs, key=lambda b: (packs[b][0], len(packs[b]))),
                         packs=packs)
        elif kind in STORE_TYPES:
            entry.update(type=kind, items=sorted((o["$uid"] for o in db[kind] if sellable(o)), key=int))
        else:
            continue
        out[uid] = entry
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    for uid, s in out.items():
        print("%-20s %-38s %-24s %d items" % (uid, s["name"], s["type"], len(s["items"])))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(OUT), "..", "devdata", "gamedb.json"))
