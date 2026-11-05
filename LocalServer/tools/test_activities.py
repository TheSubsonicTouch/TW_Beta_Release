"""Exercise the open-world activities without starting the server.

    python test_activities.py

Covers the three things the world does that the server has to answer: the client's own
activities (operation/endoperation), the crime scenes it leaves behind
(openworldcrimescene + collection) and the random street encounters
(worldencounter/endencounter). throw-away profile, real handlers, the replies the client
would see. Run it after changing handlers.py or state.py.
"""
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import handlers  # noqa: E402
import state  # noqa: E402


def main():
    workdir = tempfile.mkdtemp(prefix="tw-activities-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    player = handlers._player()
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-30s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-30s %s" % (name, e))

    def end_operation(name, successful=1):
        parts = ["operation", "endoperation", name, "1", str(successful)]
        return handlers.end_operation(None, "POST", parts, b"", {})

    def operations():
        return handlers.operations(None, "GET", ["operation", "list"], b"", {})

    def activity_kinds():
        # Every UFG::OperationType pays something, and face kinds stay face.
        seen = []
        for name, reward in sorted(state.OPERATION_REWARDS.items()):
            kind, table = state.ERRAND_REWARD_TABLES[reward]
            face_before = int(player["PlayerDataBag"]["Face"])
            money_before = int(player["CurrencyBag"].get("DirtyMoney", "0"))
            reply = end_operation(name)
            assert reply["mReturnCode"] == 0, reply
            assert reply["mOperation"] == name, reply
            assert reply["mSuccessful"] is True, reply
            assert int(reply["mReward"]) > 0, reply
            if kind == "Face":
                assert reply["mRewardType"] == state.JOB_REWARD_FACE, (name, reply)
                gained = int(player["PlayerDataBag"]["Face"]) - face_before
            else:
                assert reply["mRewardType"] == state.JOB_REWARD_CASH, (name, reply)
                gained = int(player["CurrencyBag"]["DirtyMoney"]) - money_before
            assert gained == int(reply["mReward"]), (name, gained, reply["mReward"])
            seen.append("%s+%d" % (name, gained))
        return " ".join(seen)

    def activity_by_uid():
        # The route is documented as taking the operation's own UID as %s.
        uid = "2751831883422413570"                       # UFG::OperationType "Defeat"
        money_before = int(player["CurrencyBag"].get("DirtyMoney", "0"))
        reply = end_operation(uid)
        assert reply["mOperation"] == "Defeat", reply
        assert int(player["CurrencyBag"]["DirtyMoney"]) > money_before
        return "uid -> Defeat +%s" % reply["mReward"]

    def failed_activity():
        face = int(player["PlayerDataBag"]["Face"])
        reply = end_operation("Catch", successful=0)
        assert reply["mSuccessful"] is False, reply
        assert reply["mReward"] == "0", reply
        assert int(player["PlayerDataBag"]["Face"]) == face
        return "no payout"

    def crime_scene_recorded():
        parts = ["openworldcrimescene", "120", "340", "12", "7"]
        reply = handlers.open_world_crime_scenes(None, "POST", parts, b"", {})
        assert reply["mCrimeSceneAdded"] is True, reply
        assert reply["mCrimeSceneID"].startswith("700"), reply
        listed = handlers.open_world_crime_scenes(None, "GET", ["openworldcrimescene"], b"", {})
        scenes = listed["CrimeScenes"]
        assert len(scenes) == 1, scenes
        scene = scenes[0]
        assert (scene["DeathX"], scene["DeathY"], scene["DeathZ"]) == (120, 340, 12), scene
        assert scene["VictimPlayerDataIDString"] == "7", scene
        assert scene["Collected"] is False, scene
        return scene["OpenWorldCrimeSceneID"]

    def crime_scene_deduped():
        parts = ["openworldcrimescene", "122", "342", "12", "7"]     # two steps away
        reply = handlers.open_world_crime_scenes(None, "POST", parts, b"", {})
        assert reply["mCrimeSceneAdded"] is False, reply
        listed = handlers.open_world_crime_scenes(None, "GET", ["openworldcrimescene"], b"", {})
        assert len(listed["CrimeScenes"]) == 1, listed
        return "one marker"

    def crime_scene_collected():
        scene = handlers.open_world_crime_scenes(None, "GET", ["openworldcrimescene"], b"", {})["CrimeScenes"][0]
        face = int(player["PlayerDataBag"]["Face"])
        parts = ["openworldcrimescenecollection", scene["OpenWorldCrimeSceneID"]]
        reply = handlers.open_world_crime_scene_collection(None, "POST", parts, b"", {})
        assert reply["mReturnCode"] == 0, reply
        assert reply["FaceCollected"] == state.CRIME_SCENE_FACE, reply
        assert int(player["PlayerDataBag"]["Face"]) - face == state.CRIME_SCENE_FACE
        left = handlers.open_world_crime_scenes(None, "GET", ["openworldcrimescene"], b"", {})
        assert left["CrimeScenes"] == [], left
        again = handlers.open_world_crime_scene_collection(None, "POST", parts, b"", {})
        assert again["mReturnCode"] == 1, again
        assert again["FaceCollected"] == 0, again
        return "+%d face, once" % state.CRIME_SCENE_FACE

    def crime_scene_expiry():
        now = time.time()
        state.add_crime_scene(player, 900, 900, 0, now=now)
        assert len(state.crime_scenes(player, now=now)) == 1
        assert state.crime_scenes(player, now=now + state.CRIME_SCENE_TTL + 1) == []
        return "gone after an hour"

    def crime_scene_limit():
        for i in range(state.CRIME_SCENE_LIMIT + 6):
            state.add_crime_scene(player, 100 + i * 10, 100, 0)
        assert len(state.crime_scenes(player)) == state.CRIME_SCENE_LIMIT
        state.crime_scene_bag(player)["Scenes"] = []
        return "%d markers at most" % state.CRIME_SCENE_LIMIT

    def street_encounter():
        face = int(player["PlayerDataBag"]["Face"])
        paid = 0
        for _ in range(state.WORLD_ENCOUNTER_LIMIT + 2):
            reply = handlers.end_world_encounter(
                None, "POST", ["worldencounter", "endencounter", "mugging", "1"], b"", {})
            assert reply["mRewardType"] == state.JOB_REWARD_FACE, reply
            paid += int(reply["mReward"])
        gained = int(player["PlayerDataBag"]["Face"]) - face
        assert gained == paid > 0, (gained, paid)
        assert paid == state._table_amount("mRewardFaceLow", 1) * state.WORLD_ENCOUNTER_LIMIT
        return "+%d face, then the hourly cap" % paid

    def street_encounter_failure():
        reply = handlers.end_world_encounter(
            None, "POST", ["worldencounter", "endencounter", "mugging", "0"], b"", {})
        assert reply["mReward"] == "0", reply
        return "no payout"

    def street_encounter_resets():
        bag = state.world_encounter_bag(player)
        bag["Paid"] = [int(time.time()) - state.WORLD_ENCOUNTER_WINDOW - 1] * state.WORLD_ENCOUNTER_LIMIT
        reply = state.pay_world_encounter_reward(player, 1)
        assert reply["amount"] > 0, reply
        return "yesterday's payouts do not count"

    def street_encounter_unlocked():
        # UnlockBag.All is the whole point: the street is worth stopping for every time.
        bag = state.world_encounter_bag(player)
        bag["Paid"] = [int(time.time())] * (state.WORLD_ENCOUNTER_LIMIT * 4)
        capped = state.pay_world_encounter_reward(player, 1)
        assert capped["amount"] == 0, capped                    # locked profile is capped
        player["UnlockBag"] = {"All": True}
        try:
            face = int(player["PlayerDataBag"]["Face"])
            paid = 0
            for _ in range(state.WORLD_ENCOUNTER_LIMIT * 3):
                reply = state.pay_world_encounter_reward(player, 1)
                assert reply["amount"] > 0, reply
                paid += reply["amount"]
            assert int(player["PlayerDataBag"]["Face"]) - face == paid > 0, paid
            bag["Paid"] = []
        finally:
            del player["UnlockBag"]
        return "+%d face, no cap" % paid

    def crime_scene_limit_unlocked():
        player["UnlockBag"] = {"All": True}
        try:
            assert state.crime_scene_limit(player) == state.CRIME_SCENE_LIMIT_UNLOCKED
            for i in range(state.CRIME_SCENE_LIMIT_UNLOCKED + 6):
                state.add_crime_scene(player, 5000 + i * 10, 5000, 0)
            kept = len(state.crime_scenes(player))
            assert kept == state.CRIME_SCENE_LIMIT_UNLOCKED, kept
            state.crime_scene_bag(player)["Scenes"] = []
        finally:
            del player["UnlockBag"]
        assert state.crime_scene_limit(player) == state.CRIME_SCENE_LIMIT
        return "%d markers with the unlock" % kept

    def no_hotspot_no_scatter():
        # A profile that has never fought has no ground to scatter around, so the map
        # stays empty even with the unlock on.
        state.crime_scene_bag(player)["Scenes"] = []
        state.crime_scene_bag(player)["Hotspots"] = []
        player["UnlockBag"] = {"All": True}
        try:
            assert state.scatter_crime_scenes(player) == 0
            assert state.crime_scenes(player) == []
        finally:
            del player["UnlockBag"]
        return "nothing invented out of nowhere"

    def scatter_only_on_known_ground():
        player["UnlockBag"] = {"All": True}
        try:
            state.add_crime_scene(player, 1000, 2000, 5, now=time.time())
            spots = [tuple(s) for s in state.crime_scene_bag(player)["Hotspots"]]
            assert spots, "the fight should have been remembered"
            added = state.scatter_crime_scenes(player)
            assert added == state.CRIME_SCENE_SCATTER - 1, added
            live = state.crime_scenes(player)
            assert len(live) == state.CRIME_SCENE_SCATTER, len(live)
            spot = spots[0]
            for scene in live:
                far = max(abs(scene["DeathX"] - spot[0]), abs(scene["DeathY"] - spot[1]))
                assert far <= state.CRIME_SCENE_SCATTER_SPREAD, (scene, spot)
            return "%d markers around one known spot" % len(live)
        finally:
            del player["UnlockBag"]

    def scatter_is_off_when_locked():
        before = len(state.crime_scenes(player))
        assert state.scatter_crime_scenes(player) == 0
        assert len(state.crime_scenes(player)) == before
        return "locked profile unchanged"

    def operation_list():
        reply = operations()
        assert reply["mOperationInstances"] == [], reply  # the client runs them itself
        assert reply["mSecondsElapsed"] == 0 and reply["mTotalSeconds"] == 0, reply
        return "empty, timer pair answered"

    def world_encounter_list():
        reply = handlers.world_encounters(None, "GET", ["worldencounter", "list"], b"", {})
        assert reply["WorldEncounters"] == [], reply      # the client spawns its own
        return "empty on purpose"

    def restart_keeps_scenes():
        handlers.STATE.save()
        saved = state.State(os.path.join(workdir, "state.json")).data["player"]
        assert "CrimeSceneBag" in saved and "WorldEncounterBag" in saved, sorted(saved)
        return "state.json round-trips"

    check("activity payouts", activity_kinds)
    check("activity by uid", activity_by_uid)
    check("failed activity", failed_activity)
    check("crime scene recorded", crime_scene_recorded)
    check("crime scene deduped", crime_scene_deduped)
    check("crime scene collected", crime_scene_collected)
    check("crime scene expiry", crime_scene_expiry)
    check("crime scene limit", crime_scene_limit)
    check("street encounter", street_encounter)
    check("street encounter failed", street_encounter_failure)
    check("street encounter window", street_encounter_resets)
    check("street encounter unlocked", street_encounter_unlocked)
    check("crime scene limit unlocked", crime_scene_limit_unlocked)
    check("scatter needs a hotspot", no_hotspot_no_scatter)
    check("scatter on known ground", scatter_only_on_known_ground)
    check("scatter off when locked", scatter_is_off_when_locked)
    check("activity list", operation_list)
    check("encounter list", world_encounter_list)
    check("restart", restart_keeps_scenes)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
