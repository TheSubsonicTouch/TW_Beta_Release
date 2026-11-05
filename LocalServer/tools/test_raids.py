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

    def rivals_bare_when_locked():
        # Nothing about a locked profile changes: every rival turf is still empty.
        handlers.STATE.data["player"].pop("UnlockBag", None)
        handlers.STATE.opponents()
        bare = [len(o["data"]["TurfBag"]["Rackets"]) for o in handlers.STATE.data["opponents"]]
        assert bare == [0] * 7, bare
        return "all seven empty"

    def rivals_built_when_unlocked():
        handlers.STATE.data["player"]["UnlockBag"] = {"All": True}
        handlers.STATE.opponents()
        counts = [len(o["data"]["TurfBag"]["Rackets"]) for o in handlers.STATE.data["opponents"]]
        assert counts == [len(state.RIVAL_TURF_LOTS)] * 7, counts
        return "5 rackets on every site"

    def rival_rackets_match_their_lots():
        # A SMALL lot takes a SMALL racket, and no site is left carrying an empty-lot type.
        for index, opponent in enumerate(handlers.STATE.data["opponents"]):
            rackets = opponent["data"]["TurfBag"]["Rackets"]
            for block, racket in enumerate(rackets):
                rtype = state.RACKET_TYPES[racket["RacketTypeUID"]]
                assert rtype["mSize"] == state.RIVAL_TURF_LOTS[block], (index, block, rtype["mSize"])
                assert not rtype["mIsEmptyType"], (index, block)
                assert racket["BlockIndex"] == block, (index, block)
                assert racket["RacketTypeUID"] not in state.EMPTY_UID.values()
        return "sizes, blocks and types line up"

    def rival_turf_is_within_budget():
        # The client reads a turf's spare build points as safehouse level minus rackets.
        for opponent in handlers.STATE.data["opponents"]:
            turf = opponent["data"]["TurfBag"]
            level = int(turf["Turf"]["Safehouse"]["RacketLevel"])
            assert level >= len(turf["Rackets"]), (level, len(turf["Rackets"]))
            assert turf["Turf"]["RacketsBuilt"] == str(len(turf["Rackets"]))
        return "level 6..9, five rackets, RacketsBuilt set"

    def filling_is_idempotent():
        before = json.dumps(handlers.STATE.data["opponents"], sort_keys=True)
        handlers.STATE.opponents()
        handlers.STATE.opponents()
        after = json.dumps(handlers.STATE.data["opponents"], sort_keys=True)
        assert before == after, "a second read changed the rivals"
        return "same turf every read"

    def built_turf_pays_safes():
        # Every racket on the target is loot, so a built-out site pays more than the base.
        opponent = handlers.STATE.data["opponents"][3]
        expected = len(opponent["data"]["TurfBag"]["Rackets"]) * state.RAID_SAFE_DIRTY
        dirty = int(handlers._player()["CurrencyBag"]["DirtyMoney"])
        reply = raid(3)
        assert reply["mDirtyMoneyGainedFromSafes"] == expected, reply["mDirtyMoneyGainedFromSafes"]
        assert reply["mDirtyMoneyGained"] == 5000, reply["mDirtyMoneyGained"]
        assert int(handlers._player()["CurrencyBag"]["DirtyMoney"]) - dirty == 5000 + expected
        return "+%d from the safes" % expected

    def lost_raid_pays_no_safes():
        reply = raid(1, success=0)
        assert reply["mDirtyMoneyGainedFromSafes"] == 0, reply
        return "nothing from a failed raid"

    check("rivals available", open_at_start)
    check("raid cooldown", raid_puts_one_on_cooldown)
    check("failed raid", lost_raid_stays_open)
    check("cooldown expires", cooldown_runs_out)
    check("restart", cooldown_survives_restart)
    check("old profile", old_profile_migrates)
    check("rival bags", opponent_bag_still_there)
    check("locked rivals bare", rivals_bare_when_locked)
    check("unlocked rivals built", rivals_built_when_unlocked)
    check("rackets match lots", rival_rackets_match_their_lots)
    check("rival turf budget", rival_turf_is_within_budget)
    check("filling idempotent", filling_is_idempotent)
    check("safes on a built turf", built_turf_pays_safes)
    check("no safes on a loss", lost_raid_pays_no_safes)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
