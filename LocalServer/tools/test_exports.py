"""Exercise the export business without starting the server.

    python test_exports.py

The client runs export missions itself and tells the server twice: when one begins
(export/startmission/<uid>) and when one ends (export/endmission/<uid>/<result>). This
checks what the server does with both - the money and the face an export pays, the
attempt/completed/face totals the profile screen reads, the failed run that pays nothing,
the hourly cap and the unlocked profile that is not capped. Run it after changing
handlers.py or state.py.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import handlers  # noqa: E402
import state  # noqa: E402


def main():
    workdir = tempfile.mkdtemp(prefix="tw-exports-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    player = handlers._player()
    failures = []
    now = [1_700_000_000]

    state.time.time = lambda: now[0]

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-30s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-30s %s" % (name, e))

    def start(mission="1001"):
        return handlers.export_start(None, "POST", ["export", "startmission", mission], b"", {})

    def end(mission="1001", result=1):
        return handlers.export_end(None, "POST",
                                   ["export", "endmission", mission, str(result)], b"", {})

    def money(kind):
        return int(player["CurrencyBag"].get(kind, "0"))

    def face():
        return int(player["PlayerDataBag"].get("Face", "0"))

    def paid_out():
        before_money, before_face = money("DirtyMoney"), face()
        reply = end("1002")
        assert reply["mReturnCode"] == 0, reply
        return (money("DirtyMoney") - before_money, face() - before_face,
                reply["mMoneyReward"], reply["mFaceReward"])

    def export_pays():
        start("1002")
        money_gain, face_gain, money_reply, face_reply = paid_out()
        assert money_gain == state.EXPORT_MONEY, money_gain
        assert face_gain == int(round(state.EXPORT_MONEY * state.EXPORT_FACE_PERCENT / 100.0))
        assert money_reply == money_gain and face_reply == face_gain
        return "+%d money, +%d face" % (money_gain, face_gain)

    def export_failed():
        start("1003")
        before = (money("DirtyMoney"), face())
        reply = end("1003", result=0)
        assert (money("DirtyMoney"), face()) == before, "a failed export paid something"
        assert reply["mMoneyReward"] == 0 and reply["mFaceReward"] == 0, reply
        return "nothing paid"

    def totals():
        bag = state.export_bag(player)
        assert bag["Attempted"] >= 2, bag
        assert bag["Completed"] >= 1, bag
        assert bag["Face"] >= 1, bag
        return "%d attempted, %d completed, %d face" % (bag["Attempted"], bag["Completed"],
                                                        bag["Face"])

    def profile_reads_face():
        reply = handlers.profile(None, "GET", ["profile", "playerdata", "1"], b"", {})
        bag = state.export_bag(player)
        assert reply["mExportFaceEarned"] == str(bag["Face"]), reply["mExportFaceEarned"]
        return "mExportFaceEarned = %s" % reply["mExportFaceEarned"]

    def hourly_cap():
        # A fresh profile, so the earlier exports in this test are not the ones counted.
        handlers.STATE = state.State(os.path.join(workdir, "capped.json"))
        capped = handlers._player()
        paid = []
        for i in range(state.EXPORT_MAX_PER_HOUR + 2):
            state.start_export_mission(capped, "9%02d" % i, now[0])
        for i in range(state.EXPORT_MAX_PER_HOUR + 2):
            paid.append(state.pay_export_mission(capped, "9%02d" % i, True, now[0]))
        # The cap is a per-hour total, so the last two are free and the rest are paid.
        assert paid[-1]["money"] == 0 and paid[-1]["capped"], paid[-1]
        assert paid[0]["money"] == state.EXPORT_MONEY, paid[0]
        return "capped after %d" % state.EXPORT_MAX_PER_HOUR

    def cap_lifted_by_unlock():
        handlers.STATE = state.State(os.path.join(workdir, "unlocked.json"))
        unlocked = handlers._player()
        unlocked["UnlockBag"] = {"All": True}
        for i in range(state.EXPORT_MAX_PER_HOUR + 2):
            state.start_export_mission(unlocked, "8%02d" % i, now[0])
        paid = state.pay_export_mission(unlocked, "last", True, now[0])
        assert paid["money"] == state.EXPORT_MONEY, paid
        assert not paid["capped"], paid
        return "no cap when unlocked"

    def restart_keeps_totals():
        handlers.STATE.save()
        again = state.State(handlers.STATE.path)
        handlers.STATE = again
        bag = state.export_bag(handlers._player())
        assert bag["Completed"] >= 1, bag
        return "%d completed after a restart" % bag["Completed"]

    def unknown_mission():
        reply = handlers.export_end(None, "POST", ["export", "endmission"], b"", {})
        assert reply["mReturnCode"] == 0, reply
        return "a mission with no uid still answers"

    check("export pays", export_pays)
    check("failed export", export_failed)
    check("export totals", totals)
    check("profile reads the face", profile_reads_face)
    check("totals survive a restart", restart_keeps_totals)
    check("hourly cap", hourly_cap)
    check("cap lifted by unlock", cap_lifted_by_unlock)
    check("unknown mission", unknown_mission)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
