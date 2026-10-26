"""Guard the invariants that keep the client from crashing, without the game.

    python test_crash.py

The client parses every bag in a reply eagerly. So the rules are:

  * a bag is only sent when the client asked for that bit;
  * nothing the client does not know about (our `_`-prefixed bookkeeping) is sent;
  * a bag member that was added by guesswork is not in the record at all;
  * every route answers something JSON-shaped, even for a nonsense path, so a
    mis-sized id can never take the connection down mid-transition.

Each check states the rule it is protecting, so a regression says what it broke.
"""
import json
import os
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import handlers  # noqa: E402
import state  # noqa: E402

BAGS = {
    state.BAG_PLAYER: "PlayerDataBag",
    state.BAG_CUSTOMIZATION: "CustomizationBag",
    state.BAG_FAVOURS: "FavoursBag",
    state.BAG_CURRENCY: "CurrencyBag",
    state.BAG_TURF: "TurfBag",
    state.BAG_INVENTORY: "InventoryBag",
    state.BAG_ENFORCER_UPGRADES: "EnforcerUpgradesBag",
    state.BAG_EXPORT_REWARDS: "ExportRewardsBag",
}
# Members the client's TurfBag reader knows. Anything else is a guess that can move
# the parser; ExportMissionSlots used to be here and was removed for that reason.
TURF_MEMBERS = {"Turf", "Rackets", "Commodities", "mRequestedBags"}


def walk(value, path=""):
    """Yield (path, key_or_None) for every dict key and list index in a value."""
    if isinstance(value, dict):
        for key, item in value.items():
            yield path, key
            for found in walk(item, path + "/" + str(key)):
                yield found
    elif isinstance(value, list):
        for index, item in enumerate(value):
            for found in walk(item, "%s[%d]" % (path, index)):
                yield found


def main():
    workdir = tempfile.mkdtemp(prefix="tw-crash-")
    handlers.STATE = state.State(os.path.join(workdir, "state.json"))
    player = handlers._player()
    # A bit of everything on the turf, so the walk below sees a populated profile.
    player["TurfBag"]["Turf"]["Warehouse"]["RacketLevel"] = 3
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-32s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-32s %s" % (name, e))
        except Exception:
            failures.append(name)
            print("FAIL  %-32s %s" % (name, traceback.format_exc().strip().splitlines()[-1]))
            traceback.print_exc()

    def only_requested_bags():
        """A bag is sent only for a bit the client asked for."""
        for mask in range(0, 0x2000):
            reply = state.State.bags(player, mask)
            sent = reply["mRequestedBags"]
            assert sent & ~mask == 0, "mask %#x got bits %#x it did not ask for" % (mask, sent & ~mask)
            for bit, key in BAGS.items():
                if key in reply:
                    assert mask & bit, "mask %#x answered with %s" % (mask, key)
                elif mask & bit:
                    assert key != "ExportRewardsBag", "mask %#x got no export bag" % mask
                if sent & bit:
                    assert key in reply, "mask %#x claimed %s and did not send it" % (mask, key)
        return "8192 masks"

    def no_private_members():
        """Our bookkeeping (`_ready_at`, `_prod_at`, ...) never reaches the client."""
        for mask in (0x1, 0x40, 0x41, 0x80, 0x81, 0xC1, 0x1FFF, 0x2000 - 1):
            reply = state.State.bags(player, mask)
            json.dumps(reply)                     # also proves the reply is serialisable
            for path, key in walk(reply):
                if key is None:
                    continue
                assert not key.startswith("_"), "%s <- %s" % (path, key)
        return "8 masks, no _ keys"

    def turf_members_are_known():
        """TurfBag carries the members the client's reader expects, and nothing else."""
        turf = state.State.bags(player, state.BAG_TURF)["TurfBag"]
        unknown = set(turf) - TURF_MEMBERS
        assert not unknown, unknown
        for key in ("Turf", "Rackets", "Commodities"):
            assert key in turf, "TurfBag lost " + key
        record = turf["Turf"]["Safehouse"]
        assert "ExportMissionSlots" not in record, record
        return ", ".join(sorted(turf))

    def opponents_answer_every_bag():
        """Rivals are parsed with the same reader, so the same rule applies to them."""
        infos, bags = handlers.STATE.opponents(0x1FFF)
        assert len(infos) == len(bags) == 7
        known = set(BAGS.values()) | {"mRequestedBags"}
        for query in (0x1FFF, 0x1, 0x800, 0xC1):
            for bag in handlers.STATE.opponents(query)[1]:
                assert set(bag) <= known, sorted(set(bag) - known)
                assert bag["mRequestedBags"] & ~query == 0, hex(query)
                assert ("ExportRewardsBag" in bag) == bool(query & state.BAG_EXPORT_REWARDS)
        return "7 rivals, requested bags only"

    def export_switches_off_cleanly():
        """With FEATURE_EXPORTS off the bag is still answered, just empty."""
        saved = state.FEATURE_EXPORTS
        state.FEATURE_EXPORTS = False
        try:
            bag = state.State.bags(player, state.BAG_EXPORT_REWARDS)["ExportRewardsBag"]
            assert bag == [], bag
            reply = handlers.export_start(None, "POST", ["export", "startmission", "1"], b"", {})
            json.dumps(reply)
            assert reply["mReturnCode"] == 0
        finally:
            state.FEATURE_EXPORTS = saved
        return "empty board, neutral reply"

    def jobs_switch_off_cleanly():
        """With FEATURE_JOBS off the errand routes answer the old stub."""
        saved = state.FEATURE_JOBS
        state.FEATURE_JOBS = False
        try:
            reply = handlers.errands(None, "GET", ["errands"], b"", {})
            assert reply["mErrands"] == [] and reply["mSecondsToRefresh"] > 0
        finally:
            state.FEATURE_JOBS = saved
        return "empty board, neutral reply"

    def every_route_survives_a_bad_id():
        """A wrong-sized id gets an answer, not an exception."""
        bad_parts = [["0"], ["", ""], ["9" * 40, "7"], ["x", "-1"], ["0", "0", "0"]]
        for (service, prefix), fn in sorted(handlers.ROUTES.items()):
            for extra in bad_parts:
                parts = prefix.split("/") + extra
                try:
                    reply = fn(None, "GET", parts, b"", {})
                except Exception:
                    raise AssertionError("%s/%s raised on %r: %s"
                                         % (service, prefix, extra,
                                            traceback.format_exc().strip().splitlines()[-1]))
                json.dumps(reply)
        return "%d routes" % len(handlers.ROUTES)

    def unknown_route_is_neutral():
        """dispatch() returns None for an unknown path; the caller answers neutrally."""
        assert handlers.dispatch("game", ["no", "such", "thing"]) is None
        return "dispatch -> None"

    check("only requested bags", only_requested_bags)
    check("no private members", no_private_members)
    check("turf bag members", turf_members_are_known)
    check("rival bags", opponents_answer_every_bag)
    check("exports off", export_switches_off_cleanly)
    check("jobs off", jobs_switch_off_cleanly)
    check("bad ids on every route", every_route_survives_a_bad_id)
    check("unknown route", unknown_route_is_neutral)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
