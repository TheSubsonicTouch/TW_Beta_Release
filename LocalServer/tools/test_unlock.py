"""Exercise the "unlock everything" flag without starting the server.

    python test_unlock.py

The menu's Unlock all content entry writes UnlockBag.All into state.json; the server
reads it through state.unlocked() and is supposed to open the whole game: every job,
every favour card, an event that stays live, and a rival roster that never greys out.
This checks both halves of that - what a locked profile still gets, and what the flag
adds - against a throw-away profile, the real handlers and the real data files.
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
    workdir = tempfile.mkdtemp(prefix="tw-unlock-")
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

    def unlock(on=True):
        """Flip the flag the way the menu's Unlock all content entry does."""
        if on:
            player["UnlockBag"] = {"All": True}
        else:
            player.pop("UnlockBag", None)
        # The menu also empties ErrandsBag.Day so the board refills on the next read.
        player["ErrandsBag"]["Day"] = ""

    def board():
        return handlers.errands(None, "GET", ["errands"], b"", {})

    def remaining():
        return [o["mRemainingSeconds"] for o in handlers.STATE.opponents()[0]]

    def raid(index, success=1):
        return handlers.raid_end(None, "POST",
                                 ["raid", "endraid", str(index), str(success), "0"], b"", {})

    # ---- what a profile without the flag gets (the old behaviour, unchanged) ----
    def locked_board_is_the_story_ladder():
        reply = board()
        uids = [slot["mErrandUID"] for slot in reply["mErrands"]]
        assert len(uids) == state.ERRAND_SLOT_COUNT, uids
        assert uids[0] == "6002850353329125207", uids        # "Raid A Rival" starts the chain
        for uid in uids:
            assert uid not in state.ERRAND_UNSUPPORTED, state.ERRANDS_BY_UID[uid]["name"]
        # The rest of the board is the game's switched-off filler, never a levelled job.
        level = state.xp_level(player)
        for uid in uids[1:]:
            errand = state.ERRANDS_BY_UID[uid]
            assert not errand["enabled"], errand["name"]
            assert state.errand_requirement(errand, "xpLevel") <= level, errand["name"]
        return "%s, then %d filler jobs" % (state.ERRANDS_BY_UID[uids[0]]["name"], len(uids) - 1)

    def locked_favours_are_level_gated():
        errand = {"favour": {"min": "Common", "max": "Epic", "tags": []}}
        pool = state.favour_pool(errand, 1)
        assert len(pool) < len(state.FAVOUR_UPGRADES), len(pool)
        assert all(state.FAVOUR_UPGRADES[uid]["xpLevel"] <= 1 for uid in pool), pool[:5]
        assert all(not state.FAVOUR_UPGRADES[uid]["ftpe"] for uid in pool), pool[:5]
        return "%d of %d cards at level 1" % (len(pool), len(state.FAVOUR_UPGRADES))

    def locked_event_opts_out_on_rotation():
        bag = state.trials(player)
        bag.update({"Start": int(time.time()) - 8 * 86400, "End": int(time.time()) - 1,
                    "OptedIn": state.TRIAL_OPT_IN, "Index": 0, "Progress": 5})
        running, _ = state.current_challenge(player)
        assert running["OptedIn"] == state.TRIAL_OPT_OUT, running["OptedIn"]
        assert running["End"] > time.time()
        return "a new rotation clears the opt-in"

    def locked_raid_starts_a_cooldown():
        raid(1)
        left = remaining()
        assert left[1] > 0, left
        assert left[:1] + left[2:] == [0] * 6, left
        return "site 2 waits %ds" % left[1]

    # ---- the same profile with the flag the menu writes ----
    def unlocked_board_is_the_whole_catalogue():
        unlock(True)
        reply = board()
        uids = {slot["mErrandUID"] for slot in reply["mErrands"]}
        bag = state.errands(player)
        pool = state.errand_pool(player, bag)
        everything = {e["uid"] for e in state.ERRANDS}
        assert {e["uid"] for e in pool} == everything, len(pool)
        assert uids <= everything, uids
        disabled = sum(1 for e in state.ERRANDS if not e["enabled"])
        export = len(state.ERRAND_UNSUPPORTED)
        assert disabled and export, (disabled, export)
        return "%d jobs on offer (%d switched off, %d export-gated)" % (
            len(pool), disabled, export)

    def unlocked_board_cards_differ():
        # A few jobs exist twice in the game's data (one record per racket), so the wide
        # catalogue can put the same card up twice. The board keeps one per name.
        unlock(True)
        player["ErrandsBag"] = state.new_errand_bag()
        state.refresh_errands(player)
        boards = 0
        for _ in range(30):
            bag = state.errands(player)
            names = [state.errand_definition(s)["name"] for s in bag["Slots"] if s]
            assert len(names) == len(set(names)), names
            for index in range(state.ERRAND_SLOT_COUNT):
                state.change_errand(player, index)
            boards += 1
        return "%d boards, no repeated card" % boards

    def unlocked_favours_are_the_whole_catalogue():
        errand = {"favour": {"min": "Common", "max": "Epic", "tags": []}}
        pool = state.favour_pool(errand, 1, state.unlocked(player))
        assert set(pool) == set(state.FAVOUR_UPGRADES), (len(pool), len(state.FAVOUR_UPGRADES))
        high = [uid for uid in pool if state.FAVOUR_UPGRADES[uid]["xpLevel"] > 1]
        assert high, "nothing above level 1"
        return "%d cards at level 1, %d of them above it" % (len(pool), len(high))

    def unlocked_job_reward_ignores_the_level_gate():
        errand = next(e for e in state.ERRANDS
                      if (e.get("reward") or "").startswith("REWARD_FAVOUR"))
        picks = set()
        for index in range(20):
            slot = state.new_errand_slot(player, errand, None, index)
            picks.update(slot["Favours"])
        levels = [state.FAVOUR_UPGRADES[uid]["xpLevel"] for uid in picks]
        assert any(level > 1 for level in levels), levels
        return "%d different cards paid, up to level %d" % (len(picks), max(levels))

    def unlocked_event_stays_live():
        bag = state.trials(player)
        bag.update({"Start": int(time.time()) - 8 * 86400, "End": int(time.time()) - 1,
                    "OptedIn": state.TRIAL_OPT_IN, "Index": 0, "Progress": 5})
        running, challenge = state.current_challenge(player)
        assert running["OptedIn"] == state.TRIAL_OPT_IN, running["OptedIn"]
        assert running["End"] > time.time()
        assert challenge and challenge.get("name"), challenge
        reply = handlers.trial_instance(None, "GET", ["trialinstance"], b"", {})
        assert reply["mPlayerProgress"]["mOptedIn"] is True, reply["mPlayerProgress"]
        assert reply["mPlayerProgress"]["mTrial"] == challenge["name"]
        assert reply["mPlayerProgress"]["mTrialEndSeconds"] > time.time()
        return "%s, opted in, ends in %dd" % (
            challenge["name"], (running["End"] - time.time()) // 86400)

    def unlocked_raid_leaves_every_turf_open():
        before = remaining()
        assert before == [0] * 7, before
        raid(3)
        after = remaining()
        assert after == [0] * 7, after
        assert handlers.STATE.data["opponents"][3]["cooldown_until"] == 0
        return "site 4 still open after a won raid"

    def unlocked_shop_reaches_the_last_two_cards():
        # Own every card but the two the game marks FTPE-only: the next pack has to be
        # one of those two, which only happens when ftpe stops filtering.
        player["FavoursBag"] = [
            state.favour_instance(10000 + i, uid)
            for i, (uid, info) in enumerate(sorted(state.FAVOUR_UPGRADES.items()))
            if not info["ftpe"]]
        player["EnforcerUpgradesBag"] = [
            {"Item1": info["upgrade"], "Item2": None}
            for _, info in sorted(state.FAVOUR_UPGRADES.items()) if not info["ftpe"]]
        picks = handlers._award_favours(player, 2)
        assert len(picks) == 2, picks
        assert all(state.FAVOUR_UPGRADES[uid]["ftpe"] for uid in picks), \
            [state.FAVOUR_UPGRADES[uid]["name"] for uid in picks]
        return "the two FTPE cards: %s" % ", ".join(
            state.FAVOUR_UPGRADES[uid]["name"] for uid in picks)

    def locked_shop_still_skips_them():
        unlock(False)
        picks = handlers._award_favours(player, 2)
        assert picks and not any(state.FAVOUR_UPGRADES[uid]["ftpe"] for uid in picks), picks
        return "repeats a normal card instead"

    def flag_survives_a_restart():
        unlock(True)
        path = handlers.STATE.path
        handlers.STATE.save()
        reloaded = state.State(path)
        assert state.unlocked(reloaded.data["player"]), reloaded.data["player"].get("UnlockBag")
        handlers.STATE = reloaded
        nonlocal player
        player = handlers._player()
        assert state.unlocked(player)
        return "UnlockBag.All round-trips through state.json"

    check("locked board is the ladder", locked_board_is_the_story_ladder)
    check("locked favours level-gated", locked_favours_are_level_gated)
    check("locked event opts out", locked_event_opts_out_on_rotation)
    check("locked raid cools down", locked_raid_starts_a_cooldown)
    check("unlocked board is everything", unlocked_board_is_the_whole_catalogue)
    check("unlocked cards all differ", unlocked_board_cards_differ)
    check("unlocked favours are all 137", unlocked_favours_are_the_whole_catalogue)
    check("unlocked job reward", unlocked_job_reward_ignores_the_level_gate)
    check("unlocked event stays live", unlocked_event_stays_live)
    check("unlocked turf stays open", unlocked_raid_leaves_every_turf_open)
    check("unlocked shop reaches them", unlocked_shop_reaches_the_last_two_cards)
    check("locked shop skips them", locked_shop_still_skips_them)
    check("flag survives a restart", flag_survives_a_restart)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
