"""Exercise export missions (and the job chain they unlock) without starting the server.

    python test_exports.py

Builds a throw-away profile, puts a racket on its turf, and walks the whole loop:
production -> warehouse -> export board -> start -> end, plus the errands that only
appear once an export has been completed.
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
    workdir = tempfile.mkdtemp(prefix="tw-exports-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    player = handlers._player()
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-32s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-32s %s" % (name, e))

    def board():
        replies = state.State.bags(player, state.BAG_EXPORT_REWARDS)["ExportRewardsBag"]
        assert isinstance(replies, list) and replies, replies
        assert len(replies) == state.export_slots(player)
        mission = replies[0]
        for key in ("ExportMissionUID", "mCommodity1UID", "mCommodity1Amount",
                    "mMoneyReward", "mFaceReward"):
            assert key in mission, (key, mission)
        assert int(mission["mMoneyReward"]) > 0 and int(mission["mFaceReward"]) > 0
        return "%s x%d -> %d money, %d face" % (mission["mCommodityTitle"],
                                                mission["mCommodity1Amount"],
                                                mission["mMoneyReward"], mission["mFaceReward"])

    def slots_follow_warehouse():
        assert state.export_slots(player) == 1
        player["TurfBag"]["Turf"]["Warehouse"]["RacketLevel"] = 4
        board_now = state.State.bags(player, state.BAG_EXPORT_REWARDS)["ExportRewardsBag"]
        assert len(board_now) == 4, len(board_now)
        player["TurfBag"]["Turf"]["Warehouse"]["RacketLevel"] = 1
        state.export_missions(player)                     # trim back down to one slot
        return "warehouse 4 -> 4 slots"

    def stock_is_staged():
        mission = state.State.bags(player, state.BAG_EXPORT_REWARDS)["ExportRewardsBag"][0]
        held = state.warehouse_amount(player, mission["mCommodity1UID"])
        assert held >= int(mission["mCommodity1Amount"]), (held, mission)
        return "%d units in the warehouse" % held

    def production():
        shop = state._BY_CAPTION["RACKET_AUTO_SHOP"][0]["$uid"]
        racket = state.racket_instance(shop, 1, block=0, slot=0)
        player["TurfBag"]["Rackets"].append(racket)
        assert racket["Production1"] == state.racket_outputs(shop)[0] > 0, racket["Production1"]
        racket["_prod_at"] = time.time() - 3600
        state.settle_production(player)
        assert racket["CurrentProduction"] > 0, racket["CurrentProduction"]
        return "auto shop: %d units after an hour" % racket["CurrentProduction"]

    def move_to_warehouse():
        shop = state._BY_CAPTION["RACKET_AUTO_SHOP"][0]["$uid"]
        player["TurfBag"]["Turf"]["Warehouse"]["RacketLevel"] = 3   # room for more than a load
        moved, sold, earnings = state.move_to_warehouse(player, shop)
        assert moved + sold > 0, (moved, sold)
        assert earnings == int(round(sold * 1800)), (sold, earnings)   # rebuilt cars' price
        reply = handlers.turf_move_to_warehouse(None, "POST",
                                                ["turf", "movetowarehouse", shop, "0"], b"", {})
        assert reply["mReturnCode"] == 0
        assert "mCommodityMoved" in reply
        player["TurfBag"]["Turf"]["Warehouse"]["RacketLevel"] = 1
        return "%d moved, %d sold for %d" % (moved, sold, earnings)

    def start_and_finish():
        mission = state.State.bags(player, state.BAG_EXPORT_REWARDS)["ExportRewardsBag"][0]
        money = int(player["CurrencyBag"]["DirtyMoney"])
        face = int(player["PlayerDataBag"]["Face"])
        start = handlers.export_start(None, "POST",
                                      ["export", "startmission", mission["ExportMissionUID"]],
                                      b"", {})
        assert start["mReturnCode"] == 0, start
        end = handlers.export_end(None, "POST",
                                  ["export", "endmission", mission["ExportMissionUID"], "1"],
                                  b"", {})
        assert end["mReturnCode"] == 0, end
        assert end["mMoneyReward"] == mission["mMoneyReward"], end
        assert end["mFaceReward"] == mission["mFaceReward"], end
        assert int(player["CurrencyBag"]["DirtyMoney"]) - money == mission["mMoneyReward"]
        assert int(player["PlayerDataBag"]["Face"]) - face == mission["mFaceReward"]
        return "+%d dirty money, +%d face" % (end["mMoneyReward"], end["mFaceReward"])

    def cannot_pay_twice():
        paid = state.exports(player)["Paid"][0]
        money = int(player["CurrencyBag"]["DirtyMoney"])
        again = handlers.export_end(None, "POST", ["export", "endmission", paid, "1"], b"", {})
        assert again["mMoneyReward"] == 0, again
        assert int(player["CurrencyBag"]["DirtyMoney"]) == money
        return "replay refused"

    def failure_pays_nothing():
        mission = state.State.bags(player, state.BAG_EXPORT_REWARDS)["ExportRewardsBag"][0]
        money = int(player["CurrencyBag"]["DirtyMoney"])
        end = handlers.export_end(None, "POST",
                                  ["export", "endmission", mission["ExportMissionUID"], "0"],
                                  b"", {})
        assert end["mMoneyReward"] == 0 and end["mFaceReward"] == 0, end
        assert int(player["CurrencyBag"]["DirtyMoney"]) == money
        # the load is still in the warehouse, so the mission can be retried
        assert state.warehouse_amount(player, mission["mCommodity1UID"]) >= int(mission["mCommodity1Amount"])
        return "0 paid, mission still open"

    def errand_chain():
        # "Complete an Export" hangs off "Upgrade Safehouse III"; completing it opens the
        # 73 jobs that gate on "Complete 3 Exports". Check the chain is intact rather than
        # playing all of it.
        export_job = [e for e in state.ERRANDS if e["name"] == "Complete an Export"][0]
        assert export_job["links"], export_job
        three = [e for e in state.ERRANDS if e["name"] == "Complete 3 Exports"][0]
        assert export_job["uid"] in three["links"]
        gated = sum(1 for e in state.ERRANDS if three["uid"] in e["links"])
        return "%d jobs open behind it" % gated

    def board_rolls_at_ten():
        assert state.ERRAND_REFRESH_HOUR_UTC == 10, state.ERRAND_REFRESH_HOUR_UTC
        daytime = 1793050000                       # some mid-day timestamp
        assert state.seconds_to_refresh(daytime) <= 86400
        assert state.utc_day(daytime).count("-") == 2
        return "refresh 10:00 UTC"

    def storyline_playthrough():
        """Walk the whole job ladder: every safehouse level, every job the board offers."""
        hero = state.new_player(2001, "Walkthrough", 1, state.TURF_LOCATIONS[1], 0, account_id=2)
        seen, claims, stuck = set(), 0, 0
        for level in range(1, 11):
            hero["TurfBag"]["Turf"]["Safehouse"]["RacketLevel"] = level
            for _ in range(30):
                slots = [s for s in state.refresh_errands(hero)["Slots"] if s]
                if not slots:
                    break
                before = len(seen)
                for slot in slots:
                    seen.add(slot["ErrandUID"])
                    state.end_errand(hero, slot["SlotIndex"])
                    state.claim_errand(hero, slot["SlotIndex"])
                    claims += 1
                stuck = stuck + 1 if len(seen) == before else 0
                if stuck > 25:
                    break
        enabled = [e for e in state.ERRANDS if e["enabled"]]
        assert len(seen) >= 80, "%d of %d jobs" % (len(seen), len(enabled))
        return "%d of %d jobs offered over %d claims" % (len(seen), len(enabled), claims)

    check("export board", board)
    check("slots follow warehouse", slots_follow_warehouse)
    check("load staged in warehouse", stock_is_staged)
    check("racket production", production)
    check("move to warehouse", move_to_warehouse)
    check("start + finish", start_and_finish)
    check("no double payment", cannot_pay_twice)
    check("failed run pays nothing", failure_pays_nothing)
    check("export jobs behind it", errand_chain)
    check("errand refresh hour", board_rolls_at_ten)
    check("storyline playthrough", storyline_playthrough)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
