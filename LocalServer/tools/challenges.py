"""Build data/challenges.json - the rotating Community Trial ("Challenges") table.

    python challenges.py [gamedb.json] [out.json]

Triad Wars' Challenges are the client's UIScreenTrials: a timed, opt-in community
trial with a progress counter and a ladder of rewards. The trial names and the screen
come from the client (`JiangShiTrial`, `HappyCatTrial`, `EliteGhoulTrial`, and the
challenge_screen_*.png splash images); the server owns how long a challenge runs, how
much progress is needed, and what each tier pays.

The rewards are the game's own `UFG::GameRewardTuner` objects, copied verbatim so the
client can read them with the schema it already has (mGameRewardType, mRewardValues,
mFavourTypes, mStoreItem). Values are indexed by the player's XP level, the same way
errand rewards are.

Run from this folder. Needs devdata/gamedb.json.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_SRC = os.path.join(ROOT, "devdata", "gamedb.json")
DEFAULT_OUT = os.path.join(ROOT, "data", "challenges.json")

# Reward tuners taken from the game data (see tools/gamedb.py).
TUNERS = {
    # RewardType_CleanMoney   50 / 75 / 100 ...
    "clean": "15327428390534967028",
    # RewardType_DirtyMoney   500 / 2000 / 4000 ...
    "cash": "7298147056930626086",
    # RewardType_Face         100-125 / 150-200 / 175-225 ...
    "face": "8467621010084497927",
    # RewardType_Favours_All  one favour, by rarity
    "favour": "10422962267467488290",
}

# Three live challenges, like the three containers the trials screen pages through
# (previous / current / next). Names must match the client's trial names.
#
# No image URLs: the client also ships three hard-coded links to a CDN
# (http://tw.cdn.sqexeu.com/images/Trials/...) that shut down with the game, and asking
# for one is a request to a live server that no longer answers. With the field empty the
# card draws its own artwork, and the local server never hands out a URL it cannot serve.
CHALLENGES = [
    {
        "name": "JiangShiTrial",
        "title": "Jiang Shi",
        "description": "The hopping dead are loose in the streets. Put them back in the ground.",
        "image": "",
        "tiers": [{"target": 10, "tuner": "cash"},
                  {"target": 25, "tuner": "face"},
                  {"target": 50, "tuner": "favour"}],
    },
    {
        "name": "HappyCatTrial",
        "title": "Smiley Cat",
        "description": "A lucky cat has taken a liking to your turf. Keep it smiling.",
        "image": "",
        "tiers": [{"target": 5, "tuner": "clean"},
                  {"target": 15, "tuner": "cash"},
                  {"target": 30, "tuner": "favour"}],
    },
    {
        "name": "EliteGhoulTrial",
        "title": "Yao Guai",
        "description": "Something older than the triads has woken up. Hunt it down.",
        "image": "",
        "tiers": [{"target": 10, "tuner": "face"},
                  {"target": 25, "tuner": "clean"},
                  {"target": 50, "tuner": "favour"}],
    },
]


def main(src=DEFAULT_SRC, out=DEFAULT_OUT):
    with open(src, encoding="utf-8") as f:
        db = json.load(f)
    by_uid = {o["$uid"]: o for o in db["UFG::GameRewardTuner"]}

    tuners = {}
    for key, uid in TUNERS.items():
        obj = dict(by_uid[uid])
        obj.pop("$uid", None)
        obj.pop("$type", None)
        tuners[uid] = obj

    challenges = []
    for entry in CHALLENGES:
        tiers = [{"target": t["target"], "tuner": TUNERS[t["tuner"]]} for t in entry["tiers"]]
        challenges.append({k: entry[k] for k in ("name", "title", "description", "image")}
                          | {"tiers": tiers})

    data = {
        "version": 1,
        "rotationDays": 7,
        "tuners": tuners,
        "challenges": challenges,
    }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("%s: %d challenges, %d tuners" % (out, len(challenges), len(tuners)))


if __name__ == "__main__":
    main(*(sys.argv[1:3] or []))
