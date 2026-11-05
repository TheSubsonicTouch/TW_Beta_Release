"""Build data/errands.json - the job (errand) catalogue the local server hands out.

    python errands.py [gamedb.json] [out.json]

The catalogue comes from the game's own reflected objects, so the client and the
server agree on every job without either side hard-coding names:

  UFG::ErrandTypeMetadata         one object per job (116 in build 1.6.0.0)
  UFG::ErrandCompletionCondition  the stat(s) that complete a job
  UFG::ErrandRewards              the reward tables the job's reward type indexes

The condition stat is a qSymbol: the client hashes the stat name (CRC-32, poly
0x04C11DB7, init ~0, no final xor) and compares. tools/symhash.py implements that
hash and the pass below resolves every hash back to a readable stat name by hashing
the identifiers the client's string table already contains.

Run from this folder. Needs devdata/gamedb.json and devdata/strings_ascii.txt.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from symhash import sym                      # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_SRC = os.path.join(ROOT, "devdata", "gamedb.json")
DEFAULT_OUT = os.path.join(ROOT, "data", "errands.json")

REWARD_KEYS = ("mRewardFaceHigh", "mRewardFaceMedium", "mRewardFaceLow",
               "mRewardDirtyMoneyHigh", "mRewardDirtyMoneyMedium", "mRewardDirtyMoneyLow",
               "mRewardGoldHigh", "mRewardGoldMedium", "mRewardGoldLow",
               "mRewardGoldTutorial1", "mRewardGoldTutorial2", "mRewardGoldTutorial3")


def stat_names(db, strings_path):
    """Every qSymbol hash used by an errand condition -> the name that hashes to it."""
    conditions = {c["$uid"]: c for c in db["UFG::ErrandCompletionCondition"]}
    wanted = set()
    for errand in db["UFG::ErrandTypeMetadata"]:
        for handle in errand["mCompletionConditions"]:
            cond = conditions.get(handle and handle.get("$handle"))
            if cond and cond["mStat"].startswith("sym:"):
                wanted.add(int(cond["mStat"][4:], 16))
    names = {}
    if not os.path.exists(strings_path):
        return names
    words = set()
    with open(strings_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            # the dump is "<address> <string>"; the address is 8 hex digits.
            text = line.rstrip("\n")[9:] if len(line) > 9 and line[8] == " " else line
            words.add(text)
            for word in re.findall(r"[A-Za-z_][A-Za-z_0-9]{2,40}", text):
                words.add(word)
    for word in words:
        for candidate in {word, word.strip()}:     # case variants are covered by upper flag
            for upper in (False, True):
                try:
                    value = sym(candidate, upper)
                except (UnicodeEncodeError, ValueError):
                    continue
                if value in wanted and value not in names:
                    names[value] = candidate
    return names


def condition_record(cond, stats):
    kind = "bool" if cond["mStatType"] == "STAT_BOOL" else "int"
    op = {"STAT_GREATER_THAN_OR_EQUAL": "gte", "STAT_EQUAL": "eq",
          "STAT_LESS_THAN_OR_EQUAL": "lte", "STAT_LESS_THAN": "lt"}.get(
              cond["mConditionType"], cond["mConditionType"].lower())
    out = {"stat": stats.get(int(cond["mStat"][4:], 16)) if cond["mStat"].startswith("sym:")
           else cond["mStat"], "kind": kind, "op": op}
    if out["stat"] is None:
        out["stat"] = cond["mStat"]
    if kind == "bool":
        out["value"] = bool(cond["mBoolValue"])
    else:
        out["value"] = int(cond["mIntValue"])
    reset = []
    for key, label in (("mResetOnErrandStart", "errand_start"), ("mResetOnErrandComplete", "errand_end"),
                       ("mResetOnRaidStart", "raid_start"), ("mResetOnRaidComplete", "raid_end")):
        if cond.get(key):
            reset.append(label)
    if reset:
        out["reset"] = reset
    return out


def main(src=DEFAULT_SRC, out=DEFAULT_OUT):
    with open(src, encoding="utf-8") as f:
        db = json.load(f)
    stats = stat_names(db, os.path.join(ROOT, "devdata", "strings_ascii.txt"))
    conditions = {c["$uid"]: c for c in db["UFG::ErrandCompletionCondition"]}
    rewards = db["UFG::ErrandRewards"][0]

    errands = []
    for errand in db["UFG::ErrandTypeMetadata"]:
        favour = errand.get("mFavourTypes") or {}
        entry = {
            "uid": errand["$uid"],
            "name": errand.get("mDisplayName", ""),
            "description": errand.get("mDescription", ""),
            "reward": errand.get("mRewardType", ""),
            "repeatable": bool(errand.get("mRepeatable")),
            "enabled": bool(errand.get("mIsEnabled")),
            "priority": int(errand.get("mPriority") or 0),
            "xpLevel": int(errand.get("mXPLevel") or 0),
            "safehouseLevel": int(errand.get("mSafeHouseLevel") or 0),
            "racketLevel": int(errand.get("mRacketLevel") or 0),
            "conditions": [condition_record(conditions[h["$handle"]], stats)
                           for h in errand["mCompletionConditions"] if h and h.get("$handle") in conditions],
            "links": [h["$handle"] for h in errand["mErrandLinks"] if h and h.get("$handle")],
        }
        if favour:
            entry["favour"] = {"min": favour.get("minRarity", "Common"),
                               "max": favour.get("maxRarity", "Epic"),
                               "tags": [t["$handle"] for t in favour.get("favourTags") or [] if t]}
        if errand.get("mFavourPack"):
            entry["favourPack"] = errand["mFavourPack"].get("$handle")
        if errand.get("mRacketType"):
            entry["racketType"] = errand["mRacketType"].get("$handle")
        if errand.get("mDisplayIcon"):
            entry["icon"] = errand["mDisplayIcon"]
        errands.append(entry)
    errands.sort(key=lambda e: (-e["priority"], e["name"], e["uid"]))

    data = {
        "version": 1,
        "source": os.path.basename(src),
        # Every reward table is indexed by the player's XP level (0..9). Gold is flat,
        # except for the three tutorial tiers, which are the first-time-flow payouts.
        "rewards": {key: rewards[key] for key in REWARD_KEYS if key in rewards},
        "errands": errands,
    }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    unresolved = sorted({c["stat"] for e in errands for c in e["conditions"] if c["stat"].startswith("sym:")})
    print("%s: %d errands, %d reward tables, %d unresolved stat(s)"
          % (out, len(errands), len(data["rewards"]), len(unresolved)))
    for stat in unresolved:
        print("  unresolved", stat)


if __name__ == "__main__":
    main(*(sys.argv[1:3] or []))
