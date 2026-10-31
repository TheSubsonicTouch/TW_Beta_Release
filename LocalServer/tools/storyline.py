"""Print the job ladder: which errands a playthrough can actually reach, and when.

    python storyline.py             the ladder, in the order the board hands it out
    python storyline.py --check     exit 1 if a job that should be playable is not

The board is simulated through a long playthrough - level 1 to 50, safehouse 1 to 10, an
hour of game time per step - with every completed job marked done. What is left unseen is
either gated behind something this build does not run, or genuinely unreachable, and the
report says which.

The two export jobs ("Complete an Export", "Complete 3 Exports") are listed as skipped:
the server does not run the export chain, so their condition can never be met, and 49
other jobs used to hang off them - state.ERRAND_UNSUPPORTED is what unblocks those.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import state  # noqa: E402


def simulate(player, bag, steps=8000, start=None):
    """Walk the board: (errands seen, order they were first offered)."""
    now = time.time() if start is None else start
    seen, order = set(), []
    for step in range(steps):
        level = min(1 + step // 60, len(state.XP_LEVELS))
        player["PlayerDataBag"]["EnforcerXP"] = state.XP_LEVELS[level - 1]
        player["TurfBag"]["Turf"]["Safehouse"]["RacketLevel"] = 1 + (step // 10) % 10
        pool = state.errand_pool(player, bag, now=now)
        if not pool:
            now += 3600          # nothing open: wait for the cooldowns
            continue
        errand = pool[0]
        if errand["uid"] not in seen:
            seen.add(errand["uid"])
            order.append(errand)
        record = bag["Completed"].setdefault(errand["uid"], {"Times": 0, "Last": 0})
        record["Times"] = int(record.get("Times") or 0) + 1
        record["Last"] = int(now)
        now += 600
    return seen, order


def main():
    ap = argparse.ArgumentParser(description="Show which jobs a playthrough can reach.")
    ap.add_argument("--check", action="store_true", help="exit 1 if a playable job is unreachable")
    ap.add_argument("--all", action="store_true", help="list every errand, reached or not")
    args = ap.parse_args()

    player = state.new_player(1, "Ladder", 1, state.TURF_LOCATIONS[0], 0, account_id=1)
    bag = state.errands(player)
    seen, order = simulate(player, bag)

    enabled = [e for e in state.ERRANDS if e["enabled"]]
    bonus = [e for e in state.ERRANDS if not e["enabled"]]
    unsupported = [e for e in enabled if e["uid"] in state.ERRAND_UNSUPPORTED]
    wanted = [e for e in enabled if e["uid"] not in state.ERRAND_UNSUPPORTED]
    missing = [e for e in wanted if e["uid"] not in seen]
    bonus_seen = [e for e in bonus if e["uid"] in seen]

    if args.all:
        for errand in order:
            print("  %-4s %-40s lvl %-3s sh %-2s %s" % (
                "job" if errand["enabled"] else "bonus", errand["name"][:40],
                errand.get("xpLevel"), errand.get("safehouseLevel"),
                errand.get("reward", "")))
        print()

    print("%d of the game's %d jobs can be played (%d skipped)"
          % (len(wanted) - len(missing), len(enabled), len(unsupported)))
    print("%d of the %d bonus jobs can be played (switched off in the game data)"
          % (len(bonus_seen), len(bonus)))
    print("skipped: %s" % ", ".join(e["name"] for e in unsupported))
    if missing:
        print("%d unreachable:" % len(missing))
        for errand in missing:
            print("  %-40s lvl %-3s sh %-2s links %s" % (
                errand["name"][:40], errand.get("xpLevel"), errand.get("safehouseLevel"),
                ", ".join(errand.get("links") or []) or "-"))
    if args.check and missing:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
