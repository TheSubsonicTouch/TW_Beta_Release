"""Build data/favour_upgrades.json - the favour definitions that grant enforcer upgrades.

    python favour_rewards.py [gamedb.json] [out.json]

Every `UFG::FavourType` whose `mEnforcerUpgrade` is set is one obtainable upgrade card.
The server needs them for three things: the raid results and the favour shop (which
have always used this file, by `upgrade`/`name`/`ftpe`), and now job and challenge
rewards, which pick a card by `rarity`, `tags` and `xpLevel` to satisfy the reward
rules in the errand/challenge data.

The upgrade's own display name is the card's name; the favour's mDisplayName is the
generic "UPGRADE" label.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_SRC = os.path.join(ROOT, "devdata", "gamedb.json")
DEFAULT_OUT = os.path.join(ROOT, "data", "favour_upgrades.json")


def main(src=DEFAULT_SRC, out=DEFAULT_OUT):
    with open(src, encoding="utf-8") as f:
        db = json.load(f)
    upgrades = {o["$uid"]: o for o in db["UFG::EnforcerUpgrade"]}
    favours = {}
    for favour in db["UFG::FavourType"]:
        handle = favour.get("mEnforcerUpgrade")
        if not isinstance(handle, dict):
            continue
        upgrade = upgrades.get(handle.get("$handle"), {})
        favours[favour["$uid"]] = {
            "upgrade": handle.get("$handle", ""),
            "name": upgrade.get("mDisplayName", ""),
            "ftpe": bool(favour.get("mIsFTPEOnly")),
            "rarity": favour.get("mRarity", ""),
            "tags": [t.get("$handle") for t in favour.get("mTypes") or [] if isinstance(t, dict)],
            "category": (favour.get("mCategory") or {}).get("$handle", ""),
            "xpLevel": int(favour.get("mXPLevelRequired", 0)),
        }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(favours.items(), key=lambda kv: kv[1]["name"])), f, indent=1)
        f.write("\n")
    print("%s: %d upgrade favours" % (out, len(favours)))


if __name__ == "__main__":
    main(*(sys.argv[1:3] or []))
