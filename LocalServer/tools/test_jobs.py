"""Exercise the job board and the challenge report without starting the server.

    python test_jobs.py

Creates a throw-away profile, calls the handlers directly and checks the replies
the client would see. Run it after changing handlers.py or state.py; smoke_test.py
does the same against a live server but cannot touch the POST endpoints.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import handlers  # noqa: E402
import state  # noqa: E402


def main():
    workdir = tempfile.mkdtemp(prefix="tw-jobs-")
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

    def board():
        reply = handlers.errands(None, "GET", ["errands"], b"", {})
        assert reply["mReturnCode"] == 0
        assert 1 <= len(reply["mErrands"]) <= state.ERRAND_SLOT_COUNT, reply
        assert reply["mSecondsToRefresh"] > 0
        for slot in reply["mErrands"]:
            assert isinstance(slot["mSlotIndex"], int)
            assert slot["mErrandUID"].isdigit()
            assert slot["mState"] == state.ERRAND_ACTIVE
        first = reply["mErrands"][0]
        errand = state.ERRANDS_BY_UID[first["mErrandUID"]]
        assert errand["name"] == "Raid A Rival", errand["name"]     # the tutorial job
        return errand["name"]

    def end_and_claim():
        gold = int(player["CurrencyBag"].get("Gold", "0"))
        handlers.errand_end(None, "POST", ["errands", "end", "0"], b"", {})
        reply = handlers.errand_claim(None, "POST", ["errands", "claimreward", "0"], b"", {})
        assert reply["mReturnCode"] == 0, reply
        assert reply["mState"] == state.ERRAND_COLLECTED
        paid = int(player["CurrencyBag"].get("Gold", "0")) - gold
        assert paid == 129, paid                     # TUTORIAL1 pays mRewardGoldTutorial1
        assert reply["mRewardType"] == state.JOB_REWARD_GOLD
        assert reply["mDataBags"]["CurrencyBag"]["Gold"] == player["CurrencyBag"]["Gold"]
        return "129 gold"

    def history():
        reply = handlers.completed_errands(None, "GET", ["completederrands"], b"", {})
        assert len(reply["mErrands"]) == 1, reply
        record = reply["mErrands"][0]
        assert record["mTimesCompleted"] == 1
        assert record["mLastUpdated"][:2] == "20"
        return record["CompletedErrandID"]

    def next_job():
        # Raid A Rival is done: the board refills the collected slot with a job whose
        # prerequisites are now met.
        reply = handlers.errands(None, "GET", ["errands"], b"", {})
        slots = [s for s in reply["mErrands"] if s["mState"] == state.ERRAND_ACTIVE]
        assert slots, reply
        errand = state.ERRANDS_BY_UID[slots[0]["mErrandUID"]]
        completed = state.errands(player)["Completed"]
        assert all(link in completed for link in errand["links"]), errand["links"]
        assert errand["uid"] != "6002850353329125207"        # not the tutorial job again
        return errand["name"]

    def change_a_job():
        before = state.errands(player)["Slots"][0]["ErrandUID"]
        reply = handlers.errand_change(None, "POST", ["errands", "change", "0"], b"", {})
        if reply["mReturnCode"] == 1:                       # only one job is unlocked yet
            assert reply["mMessage"] == "No jobs available to change.", reply
            return "none available yet"
        assert reply["mReturnCode"] == 0, reply
        assert reply["mErrandUID"] != before
        return state.ERRANDS_BY_UID[reply["mErrandUID"]]["name"]

    def daily_roll():
        bag = state.errands(player)
        bag["Day"] = "2000-01-01"                 # pretend the saved board is old
        reply = handlers.errands(None, "GET", ["errands"], b"", {})
        assert state.errands(player)["Day"] == state.utc_day()
        assert reply["mErrands"], reply
        return "%d jobs" % len(reply["mErrands"])

    def challenge():
        reply = handlers.trials_screen_visits(None, "GET", ["trialsscreenvisits"], b"", {})
        progress = reply["mPlayerProgress"]
        assert progress["mTarget"] > 0, progress
        assert progress["mProgress"] == 0, progress
        assert progress["mCurrentContainer"]["mTrial"], progress
        return progress["mTrial"]

    def opt_in():
        reply = handlers.trial_optin(None, "POST",
                                     ["trialinstance", "optin", "1", "HappyCatTrial"], b"", {})
        assert reply["HappyCatTrial"] is True, reply
        assert state.trials(player)["OptedIn"] == state.TRIAL_OPT_IN
        return "HappyCatTrial"

    def tier_rewards():
        clean = int(player["CurrencyBag"].get("CleanMoney", "0"))
        reply = handlers.trial_progress(None, "GET", ["trial", "reportprogress", "30"], b"", {})
        assert len(reply["mRewardList"]) == 3, reply["mRewardList"]   # 5 / 15 / 30
        again = handlers.trial_progress(None, "GET", ["trial", "reportprogress", "30"], b"", {})
        assert len(again["mRewardList"]) == 3
        gained = int(player["CurrencyBag"].get("CleanMoney", "0")) - clean
        assert gained == 50, gained                # HappyCat tier 0 is RewardType_CleanMoney, level 1
        return "3 tiers, 50 clean money"

    def restart_keeps_progress():
        handlers.STATE.save()
        reloaded = state.State(os.path.join(workdir, "state.json"))
        saved = reloaded.data["player"]
        assert saved["ErrandsBag"]["Day"] == state.utc_day()
        assert saved["TrialsBag"]["Progress"] == 30
        return "state.json round-trips"

    check("job board", board)
    check("end + claim", end_and_claim)
    check("completed jobs", history)
    check("next job unlocked", next_job)
    check("change a job", change_a_job)
    check("daily roll", daily_roll)
    check("challenge report", challenge)
    check("challenge opt-in", opt_in)
    check("tier rewards", tier_rewards)
    check("restart", restart_keeps_progress)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
