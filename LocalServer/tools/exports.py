"""Build data/exports.json - the export mission catalogue the local server hands out.

    python exports.py [gamedb.json] [out.json]

Export missions are the warehouse side of the game: a racket produces a commodity,
the warehouse stores it, and an export mission drives that commodity out of town for
dirty money and face. Everything the client needs to price one is in the game's own
reflected objects:

  UFG::CommodityType          unit price, and mExportAmounts (units per XP level)
  UFG::ExportMissionMetadata  face reward as a percentage of the money reward, and the
                              black market sell markup, both indexed by XP level
  UFG::RacketType             mRacketOutput links a racket to the commodity it makes

The client's own strings name the three mission kinds (ExportDefend, ExportEscort,
ExportGunner) and the bag the missions travel in (ExportRewardsBag); the field names
of a mission record are the client's (mCommodity1UID, mCommodity1Amount).

Run from this folder. Needs devdata/gamedb.json.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_SRC = os.path.join(ROOT, "devdata", "gamedb.json")
DEFAULT_OUT = os.path.join(ROOT, "data", "exports.json")

# The client's export mission kinds, from its string table (019579e0-01957a00).
MISSION_TYPES = ["ExportGunner", "ExportEscort", "ExportDefend"]


def pretty(name):
    """"COMMODITY_KNOCK_OFF_FASHION" -> "Knock Off Fashion" (the client localises it)."""
    text = re.sub(r"^COMMODITY_", "", name or "")
    words = re.split(r"[_\s]+", text)
    return " ".join(w.capitalize() for w in words if w)


def main(src=DEFAULT_SRC, out=DEFAULT_OUT):
    with open(src, encoding="utf-8") as f:
        db = json.load(f)

    meta = (db.get("UFG::ExportMissionMetadata") or [{}])[0]
    face = list(meta.get("mFaceRewardPercentageOfMoneyReward") or [])
    markup = list(meta.get("mBlackMarketSellMarkupForMoneyReward") or [])

    # Which racket makes which commodity (mRacketOutput is a handle to a CommodityType).
    producer = {}
    for racket in db.get("UFG::RacketType") or []:
        handle = (racket.get("mRacketOutput") or {}).get("$handle")
        if handle and handle not in producer:
            levels = racket.get("mLevels") or [{}]
            producer[handle] = {
                "uid": racket["$uid"],
                "name": racket.get("mCaption", ""),
                "caption": pretty(racket.get("mCaption", "")),
                # mOutput1Amount / mProdTimeSeconds of the first level: one production cycle
                "output": int(levels[0].get("mOutput1Amount") or 0),
                "seconds": int(levels[0].get("mProdTimeSeconds") or 0),
            }

    commodities = []
    for c in db.get("UFG::CommodityType") or []:
        if c.get("mVirtualCommodity"):
            continue                      # dirty/clean money are not physical goods
        floor = float(c.get("mFloorPrice") or 0)
        ceiling = float(c.get("mCeilingPrice") or 0)
        amounts = [int(a) for a in c.get("mExportAmounts") or []]
        if not amounts or max(amounts) <= 0:
            continue
        commodities.append({
            "uid": c["$uid"],
            "name": c.get("mDisplayName", ""),
            "title": pretty(c.get("mDisplayName", "")),
            "icon": c.get("mDisplayIconName", ""),
            "price": round((floor + ceiling) / 2.0, 2),
            # The price band the black market trades in: mFloorPrice holds while the
            # market has mFloorUnits or more, mCeilingPrice when it is down to
            # mCeilingUnits. mMarkUp is the dealer's cut on a purchase (the game has 0).
            "floor": floor,
            "ceiling": ceiling,
            "floorUnits": int(c.get("mFloorUnits") or 0),
            "ceilingUnits": int(c.get("mCeilingUnits") or 0),
            "markUp": float(c.get("mMarkUp") or 0.0),
            "amounts": amounts,
            "racket": producer.get(c["$uid"], {}),
        })
    commodities.sort(key=lambda c: (-c["price"], c["name"]))

    data = {
        "version": 1,
        "source": os.path.basename(src),
        # Both arrays are indexed by the player's XP level (level 1 = index 0).
        "facePercentOfMoney": face,
        "blackMarketMarkup": markup,
        "missionTypes": MISSION_TYPES,
        "commodities": commodities,
    }
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("%s: %d commodities, %d mission types, face%% %s"
          % (out, len(commodities), len(MISSION_TYPES), face))
    for c in commodities:
        print("  %-30s %-24s price %8.2f  units %s"
              % (c["name"], c["racket"].get("name", "-"), c["price"], c["amounts"][:3]))


if __name__ == "__main__":
    main(*(sys.argv[1:3] or []))
