"""Exercise rival availability and the raid cooldown without starting the server.

    python test_raids.py

The world map asks `playerdata/opponents` for mRemainingSeconds per rival and greys a site
out while it counts down; with every rival reported as "on cooldown for 24 hours" there was
nothing to raid after the tutorial. A fresh profile is open, a successful raid puts that one
site on state.RIVAL_RAID_COOLDOWN, and older profiles are migrated on load.
"""
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import handlers  # noqa: E402
import state  # noqa: E402


def remaining():
    return [o["mRemainingSeconds"] for o in handlers.STATE.opponents()[0]]


def raid(index, success=1):
    return handlers.raid_end(None, "POST", ["raid", "endraid", str(index), str(success), "0"],
                             b"", {})


def main():
    workdir = tempfile.mkdtemp(prefix="tw-raids-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    handlers._player()                       # make sure the profile exists
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-30s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-30s %s" % (name, e))

    def open_at_start():
        assert remaining() == [0] * 7, remaining()
        rivals = handlers.STATE.opponents()[0]
        assert len(rivals) == 7
        assert all(r["mPlayerDataID"].isdigit() for r in rivals)
        return "7 rivals, all available"

    def raid_puts_one_on_cooldown():
        reply = raid(2)
        assert reply["mReturnCode"] == 0, reply
        left = remaining()
        assert left[2] > 0, left
        assert left[:2] + left[3:] == [0] * 6, left
        assert abs(left[2] - state.RIVAL_RAID_COOLDOWN) <= 2, left[2]
        return "site 3 waits %ds, the rest stay open" % left[2]

    def lost_raid_stays_open():
        reply = raid(5, success=0)
        assert reply["mReturnCode"] == 0, reply
        assert remaining()[5] == 0, remaining()
        return "no cooldown after a failed raid"

    def cooldown_runs_out():
        opponent = handlers.STATE.data["opponents"][2]
        opponent["cooldown_until"] = time.time() - 1
        assert remaining()[2] == 0, remaining()
        return "back after the timer"

    def cooldown_survives_restart():
        state.start_rival_cooldown(handlers.STATE.data["opponents"][4])
        handlers.STATE.save()
        reloaded = state.State(os.path.join(workdir, "state.json"))
        handlers.STATE = reloaded
        left = [o["mRemainingSeconds"] for o in reloaded.opponents()[0]]
        assert left[4] > 0, left
        assert left[0] == 0, left
        return "site 5 still counting down"

    def old_profile_migrates():
        # A profile saved before the fix stored "every rival, 24 hours".
        data = state.default_state()
        data["version"] = 7
        for opponent in data["opponents"]:
            opponent.pop("cooldown_until", None)
            opponent["remaining_seconds"] = 86400
        path = os.path.join(workdir, "old.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
        migrated = state.State(path)
        assert migrated.data["version"] == 8, migrated.data["version"]
        assert "remaining_seconds" not in migrated.data["opponents"][0]
        left = [o["mRemainingSeconds"] for o in migrated.opponents()[0]]
        assert left == [0] * 7, left
        migrated.save()
        with open(path, encoding="utf-8") as f:
            assert json.load(f)["version"] == 8
        return "v7 -> v8, roster open"

    def opponent_bag_still_there():
        # The two parallel arrays the client zips together must stay the same length.
        rivals, bags = handlers.STATE.opponents()
        assert len(rivals) == len(bags) == 7, (len(rivals), len(bags))
        assert all("PlayerDataBag" in b for b in bags)
        return "7 rivals, 7 bags"

    check("rivals available", open_at_start)
    check("raid cooldown", raid_puts_one_on_cooldown)
    check("failed raid", lost_raid_stays_open)
    check("cooldown expires", cooldown_runs_out)
    check("restart", cooldown_survives_restart)
    check("old profile", old_profile_migrates)
    check("rival bags", opponent_bag_still_there)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
