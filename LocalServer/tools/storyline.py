"""Print the job ("errand") ladder the local server hands out.

    python storyline.py            the ladder, safehouse level by safehouse level
    python storyline.py 3          only what is unlocked at safehouse 3 (and level 1)
    python storyline.py --check    what is unreachable, and why

data/errands.json holds every job in build 1.6.0.0: 116 of them, 87 enabled, each with
the jobs that must be completed before it (mErrandLinks), the level and safehouse it
needs, and what it pays. The four tutorial jobs are only the first rung; the rest of the
ladder is the game's own story progression, gated by the safehouse (racket) levels.

    python errands.py      rebuilds data/errands.json from devdata/gamedb.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DATA = os.path.join(ROOT, "data", "errands.json")

TUTORIALS = {"Raid A Rival", "Raid 2 Rivals", "Level Up", "Upgrade Safehouse II"}


def load():
    with open(DATA, encoding="utf-8") as f:
        data = json.load(f)
    errands = data["errands"]
    return errands, {e["uid"]: e for e in errands}


def describe(errand, by):
    links = [by[l]["name"] if l in by else l for l in errand.get("links") or []]
    reward = errand.get("reward", "").replace("REWARD_", "").lower()
    text = "%-30s %-22s" % (errand["name"], reward)
    if links:
        text += " after " + ", ".join(links)
    return text


def ladder(max_safehouse=None):
    errands, by = load()
    enabled = [e for e in errands if e["enabled"]]
    levels = sorted({e["safehouseLevel"] for e in enabled})
    print("%d jobs (%d enabled). Tutorials: %s\n"
          % (len(errands), len(enabled), ", ".join(sorted(TUTORIALS))))
    for level in levels:
        if max_safehouse is not None and level > max_safehouse:
            continue
        jobs = [e for e in enabled if e["safehouseLevel"] == level]
        jobs.sort(key=lambda e: (-e["priority"], e["name"]))
        print("safehouse %-2d  %d job(s)" % (level, len(jobs)))
        for errand in jobs:
            mark = " (tutorial)" if errand["name"] in TUTORIALS else ""
            print("   " + describe(errand, by) + mark)
        print()
    print("The server puts up to three of these on the Jobs screen at a time, best "
          "priority first,\nand refills a slot as soon as one is collected.")


def check():
    errands, by = load()
    enabled = [e for e in errands if e["enabled"]]
    print("jobs in data:            %d" % len(errands))
    print("disabled by the game:    %d" % (len(errands) - len(enabled)))
    # reachability: a job is reachable if every prerequisite is reachable (or disabled)
    reachable = set()
    changed = True
    while changed:
        changed = False
        for errand in enabled:
            if errand["uid"] in reachable:
                continue
            if all(l in reachable or not by.get(l, {}).get("enabled", True)
                   for l in errand.get("links") or []):
                reachable.add(errand["uid"])
                changed = True
    unreachable = [e for e in enabled if e["uid"] not in reachable]
    print("reachable:               %d" % len(reachable))
    print("unreachable:             %d" % len(unreachable))
    for errand in unreachable:
        print("   " + describe(errand, by))
    blocked = [(e, [by[l]["name"] for l in e["links"] if l in by and not by[l]["enabled"]])
               for e in enabled
               if any(l in by and not by[l]["enabled"] for l in e["links"] or [])]
    if blocked:
        print("\nenabled jobs that list a disabled job as a prerequisite (the server "
              "ignores those links):")
        for errand, names in blocked:
            print("   %-30s <- %s" % (errand["name"], ", ".join(names)))
    return 0 if not unreachable else 1


if __name__ == "__main__":
    if "--check" in sys.argv:
        sys.exit(check())
    ladder(int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else None)
