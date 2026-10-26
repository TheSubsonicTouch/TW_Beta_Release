"""Give a profile the money and levels it needs to reach the rest of the content.

    python unlock.py [state.json]        default: ../state.json (this folder's profile)

Run it with the server STOPPED, then start the game again. It only touches the
player's own profile:

  * dirty money, clean money and gold to a working amount, so nothing in the game
    is gated on grinding;
  * safehouse and warehouse at level 10 (the highest the shipped game allows), so
    every job gate and every export slot is open;
  * a fresh job board and export board, so the new content comes up at once.

It deliberately does not mark jobs complete: the job chain *is* the story, and
pre-completing it would empty the Jobs screen instead of filling it. It also does
not add rackets, buildings or favours, and never touches the data files - the turf
still gets built in the turf screen, where the game expects it.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.normpath(os.path.join(HERE, "..", "state.json"))

MONEY = {"DirtyMoney": "2500000", "CleanMoney": "500000", "Gold": "5000"}
MAX_LEVEL = 10


def main(path=DEFAULT):
    if not os.path.isfile(path):
        sys.exit("no profile at %s - start the game once first" % path)
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    player = data.get("player")
    if not player:
        sys.exit("%s has no player" % path)

    currency = player.setdefault("CurrencyBag", {})
    for key, value in MONEY.items():
        currency[key] = value

    turf = player["TurfBag"]["Turf"]
    changed = []
    for name in ("Safehouse", "Warehouse"):
        record = turf.setdefault(name, {})
        if int(record.get("RacketLevel", 1) or 1) < MAX_LEVEL:
            record["RacketLevel"] = MAX_LEVEL
            record["RacketState"] = 0
            record.pop("_ready_at", None)
            record.pop("_total", None)
            changed.append(name)

    # The job board and the export board roll over on the next read.
    player.setdefault("ErrandsBag", {})["Day"] = ""
    player.setdefault("ExportBag", {})["Missions"] = []
    player["ExportBag"]["Paid"] = []

    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    print("%s: %s money, safehouse/warehouse level %d (%s)"
          % (path, MONEY["DirtyMoney"], MAX_LEVEL, ", ".join(changed) or "already there"))


if __name__ == "__main__":
    main(*(sys.argv[1:2] or []))
