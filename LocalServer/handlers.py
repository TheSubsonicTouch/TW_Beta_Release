"""Endpoint handlers for the local Triad Wars server.

Each handler is registered with @route(service, "path/prefix") and receives
(req, method, parts, body, query); `parts` is the method path split on "/"
after "/<service>/api/v1/". Return a JSON-serialisable object, or
(status, object).
"""
import json
import random
import time

import schema
import state
from state import State

ROUTES = {}
STATE = State()


def route(service, prefix):
    def deco(fn):
        ROUTES[(service, prefix)] = fn
        return fn
    return deco


def dispatch(service, parts):
    for n in range(len(parts), -1, -1):
        fn = ROUTES.get((service, "/".join(parts[:n])))
        if fn:
            return fn
    return None


def ok(**extra):
    """Standard success wrapper: callbacks read mReturnCode (0 = success) and mMessage."""
    out = {"mReturnCode": 0, "mMessage": ""}
    out.update(extra)
    return out


def _int(s, default=0):
    try:
        return int(s)
    except (TypeError, ValueError):
        return default


def _json(body):
    try:
        return json.loads(body.decode("utf-8")) if body else {}
    except ValueError:
        return {}


# ---- logging service: telemetry is accepted and dropped ----------------------
@route("logging", "metrics/schema")
def metrics_schema(req, method, parts, body, query):
    return {"ClientSendInterval": 3600, "IncludeMetaData": False, "Schemas": []}


@route("logging", "")
def logging_sink(req, method, parts, body, query):
    return {}


# ---- game service: player data ----------------------------------------------
@route("game", "playerdata/serializedplayerdata")
def serialized_player_data(req, method, parts, body, query):
    # playerdata/serializedplayerdata/<bagmask>[/opponent/<index>]
    # Unlike the action endpoints, which nest the bags under "mDataBags", this reply IS the
    # bag object: the client passes the response root straight to its bag parser. Wrapping it
    # leaves the local player record blank (no name, no turf), which later strands the
    # first-raid results screen.
    mask = _int(parts[2] if len(parts) > 2 else None, 0x1FFF)
    if len(parts) >= 5 and parts[3] == "opponent":
        idx = _int(parts[4])
        opps = STATE.data["opponents"]
        if 0 <= idx < len(opps):
            return State.bags(opps[idx]["data"], mask)
        return {"mRequestedBags": 0}
    with STATE.lock:
        _refresh_laundry()
        _settle_turf()
    return STATE.player_bags(mask)


@route("game", "playerdata/opponents")
def opponents(req, method, parts, body, query):
    infos, bags = STATE.opponents()
    return {"mOpponents": infos, "mOpponentDataBags": bags}


@route("game", "playerdata/gameplaystate")
def gameplay_state(req, method, parts, body, query):
    if method in ("POST", "PUT"):
        data = _json(body)
        value = data.get("gameplaystate", data.get("mGameplayState", body.decode("utf-8", "replace")))
        with STATE.lock:
            STATE.data["gameplaystate"] = value
            STATE.data["player"]["PlayerDataBag"]["GameplayState"] = value
        STATE.save()
        return {}
    return {"mGameplayState": STATE.data.get("gameplaystate", "")}


@route("game", "playerdata/playersavegame")
def player_save_game(req, method, parts, body, query):
    if method in ("POST", "PUT"):
        data = _json(body)
        with STATE.lock:
            STATE.data["gamestate"] = data.get("gamestate", "")
        STATE.save()
        return {}
    return {"mGameState": STATE.data.get("gamestate", "")}


# Face needed to reach each rank. The client always walks exactly ten entries and then
# indexes the table by the Level it found, so this must be ten tiers, Level 0..9, with
# ascending thresholds; an empty or short table crashes the raid results screen. The
# original thresholds are not in the client, so these values are approximations.
FACE_TIER_THRESHOLDS = [0, 1000, 5000, 15000, 40000, 100000, 250000, 600000, 1500000, 4000000]


@route("game", "playerdata/facetiers")
def face_tiers(req, method, parts, body, query):
    return [{"Value": str(value), "Level": level, "LevelName": "$FACE_LEVEL_%d" % level}
            for level, value in enumerate(FACE_TIER_THRESHOLDS)]


@route("game", "storeaccess")
def store_access(req, method, parts, body, query):
    return {"mHasAccess": True}


# ---- list endpoints that are legitimately empty for a fresh solo profile ------
@route("game", "openworldcrimescene")
def open_world_crime_scenes(req, method, parts, body, query):
    return {"CrimeScenes": []}


@route("game", "raidcrimescene")
def raid_crime_scenes(req, method, parts, body, query):
    return {"CrimeScenes": []}


@route("game", "worldencounter/list")
def world_encounters(req, method, parts, body, query):
    return {"WorldEncounters": []}


@route("game", "triads/triadinvitation")
def triad_invitations(req, method, parts, body, query):
    return {"mTriadInvites": []}


@route("game", "triads/playertriad")
def player_triad(req, method, parts, body, query):
    return {}


@route("game", "triads/triadmembers")
def triad_members(req, method, parts, body, query):
    return {"mTriadMembers": []}


@route("game", "errands")
def errands(req, method, parts, body, query):
    return _errand_board()


@route("game", "completederrands")
def completed_errands(req, method, parts, body, query):
    return _errand_history()


@route("game", "turf/racketcountdowns")
def racket_countdowns(req, method, parts, body, query):
    # Timers for anything still being built or upgraded, so a restart mid-build recovers.
    with STATE.lock:
        _settle_turf()
        timers = [t for t in (_timer(r, with_uid=True) for r in _all_rackets(_player()["TurfBag"])) if t]
    return {"mReturnCode": 0, "mTimers": timers}


@route("game", "operation/list")
def operations(req, method, parts, body, query):
    return {"mSecondsElapsed": 0, "mOperationInstances": []}


# ---- commerce / accounts / social --------------------------------------------
# UFG::Currency: 0 = PriceInGame_Dirty (cash), 1 = PriceInGame_Clean (gold).
CURRENCY_DIRTY, CURRENCY_CLEAN = 0, 1
CURRENCY_BAG_KEY = {CURRENCY_DIRTY: "DirtyMoney", CURRENCY_CLEAN: "CleanMoney"}
# Item type -> (currency, price). Approximations: the original prices lived on the server.
STORE_PRICES = {
    "UFG::StoreClothing": (CURRENCY_DIRTY, 250),
    "UFG::StoreOutfit": (CURRENCY_DIRTY, 1500),
    "UFG::StoreVehicle": (CURRENCY_DIRTY, 4000),
    "UFG::VendorItem": (CURRENCY_DIRTY, 500),
    "UFG::StoreContactBundle": (CURRENCY_CLEAN, 100),
}


def _store_price(catalogue, item_uid):
    """(currency, price) of one catalogue item."""
    if catalogue["type"] == "UFG::StoreBundle":
        # A bundle of n packs: n times the pack price, 10% off for three, 20% off for five.
        currency, pack_price = STORE_PRICES["UFG::StoreContactBundle"]
        n = len(catalogue["packs"][item_uid])
        return currency, int(pack_price * n * (1 - 0.05 * (n - 1)))
    return STORE_PRICES[catalogue["type"]]


@route("commerce", "store")
def store(req, method, parts, body, query):
    # store/<store uid>: the catalogue is a bare array under "data", one record per item.
    # mCortexUID is the store item's UID as an UNSIGNED decimal string (a leading "-" reads
    # as 0). mStock -1 means unlimited; leaving the expiry fields out means "never".
    #
    # A store that has a screen must not be empty, or the client crashes:
    #   * clothing / outfit stores: the wardrobe screen shows its "empty" state without
    #     building its category tree, and the next button press reads a null pointer
    #     (TriadWars.exe!1406fcc48);
    #   * favour-pack stores: the favour shop builds its large tile from the first pack
    #     without checking that there is one (TriadWars.exe!14072240f);
    #   * pack bundle stores: the pack details screen takes its price from the 1x bundle
    #     of the selected pack without checking that it found one (TriadWars.exe!1406a34ec).
    catalogue = state.STORE_ITEMS.get(parts[1] if len(parts) > 1 else "")
    if catalogue is None:
        return {}
    items = []
    for uid in catalogue["items"]:
        currency, price = _store_price(catalogue, uid)
        items.append({"mCortexUID": uid, "mCurrency": currency, "mPrice": price,
                      "mFinalPrice": price, "mDiscountPercent": 0, "mStock": -1,
                      "mIsNew": False, "mFeatured": 0, "mFeatured2": 0})
    return ok(data=items)


@route("commerce", "coupons")
def coupons(req, method, parts, body, query):
    # The shared commerce wrapper reads mReturnCode/mMessage, then the list under "data".
    return {"mReturnCode": 0, "mMessage": "", "data": []}


@route("accounts", "friends")
def friends(req, method, parts, body, query):
    return {"mFriendInfos": []}


# ---- game service: profile changes made during the intro flow ----------------
@route("game", "playerdata/name")
def player_name(req, method, parts, body, query):
    name = str(_json(body).get("name", "")).strip()
    if name:
        with STATE.lock:
            STATE.data["player"]["PlayerDataBag"]["Name"] = name[:32]
        STATE.save()
    return ok(mDataBags=STATE.player_bags(state.BAG_PLAYER))


@route("game", "turf/location")
def turf_location(req, method, parts, body, query):
    # turf/location/<site id 1..7>: the player picks which world site their turf occupies.
    site = _int(parts[2] if len(parts) > 2 else None, 1)
    if 1 <= site <= len(state.TURF_LOCATIONS):
        with STATE.lock:
            STATE.data["player"]["TurfBag"]["Turf"]["LocationName"] = state.TURF_LOCATIONS[site - 1]
        STATE.save()
    return ok(mDataBags=STATE.player_bags(state.BAG_PLAYER | state.BAG_TURF))


@route("game", "enforcer/customization")
def enforcer_customization(req, method, parts, body, query):
    if method in ("POST", "PUT"):
        data = _json(body)
        bag = {
            "ScalingPresetCortexUID": str(data.get("mScalingPresetCortexUID", "0")),
            "ClothingOutfitCortexUID": str(data.get("mClothingOutfitCortexUID", "0")),
            "ClothingStoreItemUIDs": [str(u) for u in data.get("mClothingStoreItemUIDs", [])],
        }
        with STATE.lock:
            old = STATE.data["player"].get("CustomizationBag", {})
            for k in ("MeleeWeapon", "OneHandedWeapon", "TwoHandedWeapon"):
                if k in old:
                    bag[k] = old[k]
            STATE.data["player"]["CustomizationBag"] = bag
        STATE.save()
    return ok()


@route("game", "enforcer/weaponloadout")
def enforcer_weapon_loadout(req, method, parts, body, query):
    # enforcer/weaponloadout/<melee>/<one-handed>/<two-handed>
    ids = (parts[2:5] + ["0", "0", "0"])[:3]
    with STATE.lock:
        bag = STATE.data["player"].setdefault("CustomizationBag", {
            "ScalingPresetCortexUID": "0", "ClothingOutfitCortexUID": "0", "ClothingStoreItemUIDs": []})
        bag["MeleeWeapon"], bag["OneHandedWeapon"], bag["TwoHandedWeapon"] = ids
    STATE.save()
    return ok()


@route("game", "playerdata/defaultinventory")
def default_inventory(req, method, parts, body, query):
    with STATE.lock:
        STATE.data["default_inventory"] = parts[2] if len(parts) > 2 else ""
    STATE.save()
    return ok()


@route("game", "playerdata/eventlog")
def event_log(req, method, parts, body, query):
    return {"mServerUTC": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "mEventResults": []}


@route("game", "playerdata/lastusedvehicle")
def last_used_vehicle(req, method, parts, body, query):
    with STATE.lock:
        STATE.data["player"]["PlayerDataBag"]["LastUsedVehicle"] = str(_int(parts[2] if len(parts) > 2 else None))
    STATE.save()
    return ok()


@route("game", "playerdata/preferfriendmatchmaking")
def prefer_friend_matchmaking(req, method, parts, body, query):
    return ok()


# ---- helpers shared by the metagame handlers -----------------------------------
def _player():
    return STATE.data["player"]


def _money(kind, delta=0):
    """Read (and optionally adjust) CleanMoney / DirtyMoney, which are stored as strings."""
    bag = _player()["CurrencyBag"]
    value = max(0, _int(bag.get(kind), 0) + delta)
    bag[kind] = str(value)
    return value


def _refresh_laundry():
    """Laundering runs on wall-clock time; keep the remaining seconds current."""
    pdb = _player()["PlayerDataBag"]
    done_at = STATE.data.get("laundry_done_at", 0)
    pdb["LaundrySecondsRemaining"] = max(0, int(done_at - time.time())) if pdb.get("LaunderedMoney") else 0
    return pdb


def _bags(mask):
    with STATE.lock:
        _refresh_laundry()
        _settle_turf()
    return STATE.player_bags(mask)


ALL_OWN = state.BAG_PLAYER | state.BAG_CURRENCY | state.BAG_TURF


# ---- social service: leaderboards ------------------------------------------------
LEADERBOARD_NAMES = {1: "Face", 2: "Raids", 3: "Experience"}


def _leaderboard(board_id):
    pdb = _player()["PlayerDataBag"]
    value = pdb.get("Face", "0") if board_id == 1 else str(pdb.get("EnforcerXP", 0))
    row = {"mAccountID": pdb["AccountID"], "mUserName": pdb["Name"], "mAvatarURL": "",
           "mRank": 1, "mValue": value, "mFormattedValue": value}
    return {
        "mID": board_id,
        "mName": LEADERBOARD_NAMES.get(board_id, "Leaderboard %d" % board_id),
        "mDescription": "",
        "mType": "",
        "mLabel": "",
        "mDisplayOrder": board_id,
        "mResultSets": [{"mTimeFilter": t, "mPopulationFilter": p, "mResults": [row]}
                        for t in range(4) for p in range(3)],
    }


@route("social", "leaderboards")
def leaderboards(req, method, parts, body, query):
    # leaderboards | leaderboards/rankings/<id>/near[/<account>] | .../start/<n>/count/<n>
    # | leaderboards/playersrankings[/<account>]
    if len(parts) >= 3 and parts[1] == "rankings":
        boards = [_leaderboard(_int(parts[2], 1))]
    else:
        boards = [_leaderboard(i) for i in sorted(LEADERBOARD_NAMES)]
    return {"mReturnCode": 0, "mServerStatus": "Up", "mLeaderboards": boards}


@route("social", "achievements")
def achievements(req, method, parts, body, query):
    return {"mReturnCode": 0, "mAchievements": []}


# ---- game service: progression -----------------------------------------------
@route("game", "playerdata/gainxp")
def gain_xp(req, method, parts, body, query):
    amount = max(0, _int(parts[2] if len(parts) > 2 else None))
    with STATE.lock:
        pdb = _player()["PlayerDataBag"]
        pdb["EnforcerXP"] = int(pdb.get("EnforcerXP", 0)) + amount
        xp = pdb["EnforcerXP"]
    STATE.save()
    # mCurrentXP must be a decimal STRING: the client only accepts a string here and reads
    # anything else as 0, which resets the enforcer to level 1 and then replays level-up
    # promotions endlessly once the data bag restores the real XP.
    return ok(mCurrentXP=str(xp), mDataBags=_bags(state.BAG_PLAYER))


@route("game", "playerdata/deathpenalty")
def death_penalty(req, method, parts, body, query):
    # Solo play: dying costs nothing.
    face = _player()["PlayerDataBag"].get("Face", "0")
    return ok(mCurrentFace=face, mFacePenalty="0", mDataBags=_bags(state.BAG_PLAYER))


@route("game", "playerdata/reset")
def reset_player(req, method, parts, body, query):
    return ok()


# ---- game service: money laundering -------------------------------------------
LAUNDRY_SECONDS = 60


@route("game", "turf/laundermoney")
def launder_money(req, method, parts, body, query):
    amount = max(0, _int(parts[2] if len(parts) > 2 else None))
    with STATE.lock:
        pdb = _player()["PlayerDataBag"]
        amount = min(amount, _money("DirtyMoney"))
        if amount:
            _money("DirtyMoney", -amount)
            pdb["LaunderedMoney"] = int(pdb.get("LaunderedMoney", 0)) + amount
            STATE.data["laundry_done_at"] = time.time() + LAUNDRY_SECONDS
        _refresh_laundry()
        laundered, remaining = pdb["LaunderedMoney"], pdb["LaundrySecondsRemaining"]
    STATE.save()
    return ok(mLaunderedMoney=laundered, mLaundrySecondsRemaining=remaining,
              mDataBags=_bags(state.BAG_PLAYER | state.BAG_CURRENCY))


@route("game", "turf/claimlaunderedmoney")
def claim_laundered_money(req, method, parts, body, query):
    with STATE.lock:
        pdb = _refresh_laundry()
        amount = 0
        if pdb.get("LaunderedMoney") and pdb["LaundrySecondsRemaining"] == 0:
            amount = int(pdb["LaunderedMoney"])
            pdb["LaunderedMoney"] = 0
            _money("CleanMoney", amount)
        clean = _money("CleanMoney")
    STATE.save()
    return ok(mAmount=amount, mNewCleanMoney=clean,
              mDataBags=_bags(state.BAG_PLAYER | state.BAG_CURRENCY))


# ---- game service: raids ------------------------------------------------------
def _raid_time():
    return ok(mCopResponseSeconds=300, mFavoursBonusSeconds=0, mFavoursPenaltySeconds=0)


@route("game", "raid/getraidtime")
def raid_time(req, method, parts, body, query):
    return _raid_time()


@route("game", "raid/startraid")
def raid_start(req, method, parts, body, query):
    with STATE.lock:
        STATE.data["raid"] = {"opponent": _int(parts[2] if len(parts) > 2 else None), "started": time.time()}
    STATE.save()
    return _raid_time()


@route("game", "raid/event")
def raid_event(req, method, parts, body, query):
    return ok()


@route("game", "raid/claimfromraidmutator")
def raid_claim_mutator(req, method, parts, body, query):
    return ok()


RAID_FAVOUR_COUNT = 2
PACK_FAVOUR_COUNT = 6        # the pack descriptions all add up to six items


def _award_raid_favours(player):
    """Pick the favours a successful raid pays out and add them to the player's bag.

    The results screen needs at least one: on the first open-world raid it runs in
    tutorial mode, where the Continue prompt only appears after the favour cards have been
    shown. With an empty list the screen never unlocks.
    """
    return _award_favours(player, RAID_FAVOUR_COUNT)


def _award_favours(player, count):
    """Add `count` upgrade favours the player does not have yet; returns their definition UIDs."""
    owned = {f["FavourDefinitionUID"] for f in player.setdefault("FavoursBag", [])}
    unlocked = {u["Item1"] for u in player.get("EnforcerUpgradesBag", [])}
    by_upgrade = {info["upgrade"]: info["name"] for info in state.FAVOUR_UPGRADES.values()}
    have = {state.FAVOUR_UPGRADES[uid]["name"] for uid in owned if uid in state.FAVOUR_UPGRADES}
    have |= {by_upgrade[u] for u in unlocked if u in by_upgrade}

    def fresh(uid, info):
        return not info["ftpe"] and info["name"] not in have

    def distinct(keep):
        # Several favour definitions grant the same upgrade; offer each upgrade once.
        by_name = {}
        for uid, info in state.FAVOUR_UPGRADES.items():
            if keep(uid, info):
                by_name.setdefault(info["name"], uid)
        return list(by_name.values())

    pool = distinct(lambda uid, info: fresh(uid, info) and info["name"].endswith(" I"))
    if len(pool) < count:
        pool = distinct(fresh)
    if len(pool) < count:
        # Nothing new left to hand out: repeats are better than an empty reward list.
        pool = distinct(lambda uid, info: not info["ftpe"])
    picks = random.sample(pool, min(count, len(pool)))
    for uid in picks:
        player["FavoursBag"].append(state.favour_instance(_next_favour_id(player), uid))
    return picks


@route("game", "raid/endraid")
def raid_end(req, method, parts, body, query):
    # raid/endraid/<opponent index>/<a>/<b>. The meaning of the two trailing values is
    # not confirmed yet; a non-zero first value is treated as a successful raid.
    idx = _int(parts[2] if len(parts) > 2 else None)
    args = [_int(p) for p in parts[3:5]]
    success = bool(args and args[0])
    face_gain = 250 if success else 0
    dirty_gain = 5000 if success else 0
    with STATE.lock:
        pdb = _player()["PlayerDataBag"]
        pdb["Face"] = str(_int(pdb.get("Face"), 0) + face_gain)
        _money("DirtyMoney", dirty_gain)
        opps = STATE.data["opponents"]
        opp = opps[idx]["data"]["PlayerDataBag"] if 0 <= idx < len(opps) else {}
        STATE.data.pop("raid", None)
        face = pdb["Face"]
        favours = _award_raid_favours(_player()) if success else []
    STATE.save()
    return ok(
        mOpponentFaceGained="0",
        mOpponentAccountID=opp.get("AccountID", "0"),
        mOpponentXPLevel=1,
        mOpponentFace=opp.get("Face", "0"),
        mPlayerFaceGained=str(face_gain),
        mFaceGainedPenalty="0",
        mPlayerBonusFaceGained="0",
        mPlayerFace=face,
        mOpponentIndex=idx,
        mDirtyMoneyGained=dirty_gain,
        mDirtyMoneyGainedFromSafes=0,
        mSlotCooldownMinutes=0,
        mCommodityRewards=[],
        mSafeCommodityRewards=[],
        mFavoursAwarded=favours,          # favour definition UIDs, as strings
        mDataBags=_bags(ALL_OWN | state.BAG_FAVOURS),
    )


# ---- game service: turf management ---------------------------------------------
# mActionType tells the client's turf-result listener what kind of reply this is. It
# drives what the panel does next and, during the tutorial, whether the step advances:
# the "select your safehouse" step only moves on when a reply arrives with type 9.
#   Confirmed from the listener: 1 (shows the "units moved/sold" message),
#   4 (shows "racket destroyed"), 8 (racket info) and 9 (safehouse info).
#   The rest are inferred from how the listener groups them; see README.md.
ACTION_MOVE_TO_WAREHOUSE = 1
ACTION_START_CONSTRUCTION = 2
ACTION_UPGRADE_RACKET = 3
ACTION_REMOVE_RACKET = 4
ACTION_UPGRADE_SAFEHOUSE = 5
ACTION_UPGRADE_WAREHOUSE = 6
ACTION_UPGRADE_TEMPLE = 7
ACTION_RACKET_INFO = 8
ACTION_SAFEHOUSE_INFO = 9
ACTION_WAREHOUSE_INFO = 10
ACTION_TEMPLE_INFO = 11

# RacketState values the client acts on: 1 = under construction and 2 = upgrading (both
# make the building "busy"); anything else is idle. A busy racket carries a private
# "_ready_at" timestamp here; unknown keys are ignored by the client.
STATE_IDLE = 0
STATE_CONSTRUCTING = 1
STATE_UPGRADING = 2
MAX_LEVEL = 10
BUILD_POINT_CAP = 9          # the client clamps the safehouse level to this


def _tuning(key, level, default=7):
    table = state.METAGAME.get(key, [])
    return table[level] if 0 <= level < len(table) else default


def _all_rackets(turf):
    return [turf["Turf"]["Safehouse"], turf["Turf"]["Warehouse"], turf["Turf"]["Temple"]] + turf["Rackets"]


def _settle_turf(turf=None):
    """Finish any construction or upgrade whose timer has run out."""
    turf = turf or _player()["TurfBag"]
    now = time.time()
    changed = False
    for r in _all_rackets(turf):
        ready = r.get("_ready_at")
        if ready is not None and now >= ready:
            if r.get("RacketState") == STATE_UPGRADING:
                r["RacketLevel"] = min(MAX_LEVEL, int(r.get("RacketLevel", 1)) + 1)
            r["RacketState"] = STATE_IDLE
            del r["_ready_at"]
            r.pop("_total", None)
            changed = True
    return changed


def _seconds_left(racket):
    ready = racket.get("_ready_at") if racket else None
    return max(0, int(round(ready - time.time()))) if ready is not None else 0


def _start_timer(racket, new_state, seconds):
    racket["RacketState"] = new_state
    racket["_ready_at"] = time.time() + seconds
    racket["_total"] = seconds


# Client-side timers are keyed by (racket, type). When one runs out the client acts by type;
# type 0 re-requests turf/racketinfo for that racket, which is how it learns that a build or
# upgrade has finished (and how the "wait for construction" tutorial step gets released).
TIMER_REFRESH_RACKET = 0


def _timer(racket, with_uid=False):
    left = _seconds_left(racket)
    if left <= 0:
        return None
    entry = {"mRacketTimerType": TIMER_REFRESH_RACKET,
             "mTotalSeconds": int(racket.get("_total", left)), "mSecondsToCompletion": left}
    if with_uid:
        entry["mRacketTypeUID"] = racket["RacketTypeUID"]
    return entry


def _build_points(turf):
    """Mirror of the client's formula: safehouse level (plus one while it is upgrading),
    capped, minus the number of rackets on the turf. Empty lots are not listed."""
    safehouse = turf["Turf"]["Safehouse"]
    level = int(safehouse.get("RacketLevel", 1))
    if safehouse.get("RacketState") == STATE_UPGRADING:
        level += 1
    return max(0, min(level, BUILD_POINT_CAP) - len(turf["Rackets"]))


def _turf_action(racket=None, action=0):
    """Common reply for every turf/* action: the affected racket plus fresh bags."""
    timer = _timer(racket) if racket else None
    out = ok(mActionType=action, mSecondsToCompletion=_seconds_left(racket),
             mSecondaryRacketTypeUID="0", mRefundedRacketBossFavourUID="0", mCommodityMoved=0,
             mCommoditySold=0, mCommoditySoldEarnings=0, mTimers=[timer] if timer else [],
             mDataBags=_bags(ALL_OWN))
    if racket is not None:
        # mRacket carries the racket's fields; its type UID must sit at the ROOT of the
        # reply (the client reads it there, and only when mRacket is present). Without it
        # the result is tagged as racket 0 and the panel that asked never leaves "loading".
        out["mRacket"] = dict(racket)
        out["mRacketTypeUID"] = racket.get("RacketTypeUID", "0")
    return out


def _turf_error(message, action, racket=None):
    out = _turf_action(racket, action)
    out["mReturnCode"] = 1
    out["mMessage"] = message
    return out


def _find_racket(type_uid):
    turf = _player()["TurfBag"]
    for r in _all_rackets(turf):
        if r["RacketTypeUID"] == str(type_uid):
            return r
    return None


@route("game", "turf/startconstruction")
def turf_start_construction(req, method, parts, body, query):
    # turf/startconstruction/<racket type uid>/<block>/<slot>   build a new racket
    # turf/startconstruction/<racket type uid>/<n>              upgrade an existing one
    type_uid = parts[2] if len(parts) > 2 else "0"
    with STATE.lock:
        turf = _player()["TurfBag"]
        _settle_turf(turf)
        if len(parts) <= 4:
            racket = _find_racket(type_uid)
            if racket is None:
                return _turf_error("Racket not found.", ACTION_UPGRADE_RACKET)
            level = int(racket.get("RacketLevel", 1))
            if level >= MAX_LEVEL:
                return _turf_error("The Racket is at the max level already.", ACTION_UPGRADE_RACKET, racket)
            if racket.get("RacketState") in (STATE_CONSTRUCTING, STATE_UPGRADING):
                return _turf_error("Racket is in the incorrect state for the operation.",
                                   ACTION_UPGRADE_RACKET, racket)
            _money("DirtyMoney", -_tuning("mRacketUpgradeCosts", level, 0))
            _start_timer(racket, STATE_UPGRADING, _tuning("mRacketUpgradeTimes", level))
            action = ACTION_UPGRADE_RACKET
        else:
            block, slot = _int(parts[3]), _int(parts[4])
            rtype = state.RACKET_TYPES.get(type_uid)
            if rtype is None or rtype.get("mIsEmptyType"):
                return _turf_error("Inputs to the Server are Invalid.", ACTION_START_CONSTRUCTION)
            if _build_points(turf) <= 0:
                return _turf_error("You have reached the max rackets per turf.", ACTION_START_CONSTRUCTION)
            racket = state.racket_instance(type_uid, 1, block, slot, STATE_CONSTRUCTING)
            _start_timer(racket, STATE_CONSTRUCTING, _tuning("mRacketBuildTimes", 1))
            turf["Rackets"] = [r for r in turf["Rackets"]
                               if (r["BlockIndex"], r["SlotIndex"]) != (block, slot)]
            turf["Rackets"].append(racket)
            turf["Turf"]["RacketsBuilt"] = str(_int(turf["Turf"].get("RacketsBuilt"), 0) + 1)
            action = ACTION_START_CONSTRUCTION
    STATE.save()
    return _turf_action(racket, action)


@route("game", "turf/removeracket")
def turf_remove_racket(req, method, parts, body, query):
    target = parts[2] if len(parts) > 2 else "0"
    with STATE.lock:
        turf = _player()["TurfBag"]
        _settle_turf(turf)
        racket = next((r for r in turf["Rackets"] if r["RacketTypeUID"] == target), None)
        if racket is None:
            return _turf_error("Racket not found.", ACTION_REMOVE_RACKET)
        turf["Rackets"].remove(racket)
    STATE.save()
    return _turf_action(racket, ACTION_REMOVE_RACKET)


def _upgrade(key, action, cost_table, time_table):
    with STATE.lock:
        turf = _player()["TurfBag"]
        _settle_turf(turf)
        racket = turf["Turf"][key]
        level = int(racket.get("RacketLevel", 1))
        if level >= MAX_LEVEL:
            return _turf_error("The Racket is at the max level already.", action, racket)
        if racket.get("RacketState") in (STATE_CONSTRUCTING, STATE_UPGRADING):
            return _turf_error("Racket is in the incorrect state for the operation.", action, racket)
        cost = _tuning(cost_table, level, 0)
        if _money("DirtyMoney") < cost:
            return _turf_error("The user doesn't have enough money to upgrade the Racket.", action, racket)
        _money("DirtyMoney", -cost)
        _start_timer(racket, STATE_UPGRADING, _tuning(time_table, level))
    STATE.save()
    return _turf_action(racket, action)


@route("game", "turf/upgradesafehouse")
def turf_upgrade_safehouse(req, method, parts, body, query):
    return _upgrade("Safehouse", ACTION_UPGRADE_SAFEHOUSE, "mSafehouseUpgradeCosts", "mSafehouseUpgradeTimes")


@route("game", "turf/upgradewarehouse")
def turf_upgrade_warehouse(req, method, parts, body, query):
    return _upgrade("Warehouse", ACTION_UPGRADE_WAREHOUSE, "mWarehouseUpgradeCosts", "mWarehouseUpgradeTimes")


@route("game", "turf/upgradetriadtemple")
def turf_upgrade_temple(req, method, parts, body, query):
    return _upgrade("Temple", ACTION_UPGRADE_TEMPLE, "mTriadTempleUpgradeCosts", "mTriadTempleUpgradeTimes")


def _info(racket, action):
    with STATE.lock:
        changed = _settle_turf()
    if changed:
        STATE.save()
    return _turf_action(racket, action)


@route("game", "turf/racketinfo")
def turf_racket_info(req, method, parts, body, query):
    return _info(_find_racket(parts[2] if len(parts) > 2 else "0"), ACTION_RACKET_INFO)


@route("game", "turf/safehouseinfo")
def turf_safehouse_info(req, method, parts, body, query):
    return _info(_player()["TurfBag"]["Turf"]["Safehouse"], ACTION_SAFEHOUSE_INFO)


@route("game", "turf/warehouseinfo")
def turf_warehouse_info(req, method, parts, body, query):
    return _info(_player()["TurfBag"]["Turf"]["Warehouse"], ACTION_WAREHOUSE_INFO)


@route("game", "turf/triadtempleinfo")
def turf_temple_info(req, method, parts, body, query):
    return _info(_player()["TurfBag"]["Turf"]["Temple"], ACTION_TEMPLE_INFO)


@route("game", "turf/movetowarehouse")
def turf_move_to_warehouse(req, method, parts, body, query):
    return _turf_action(action=ACTION_MOVE_TO_WAREHOUSE)


# ---- game service: single opponent / profiles -----------------------------------
@route("game", "playerdata/opponent")
def opponent(req, method, parts, body, query):
    # playerdata/opponent/<player data id>: same reply shape as playerdata/opponents.
    infos, bags = STATE.opponents()
    wanted = parts[2] if len(parts) > 2 else ""
    keep = [i for i, info in enumerate(infos) if info["mPlayerDataID"] == wanted]
    if keep:
        infos, bags = [infos[i] for i in keep], [bags[i] for i in keep]
    return {"mOpponents": infos, "mOpponentDataBags": bags}


def _profile(player, is_ai):
    pdb = player["PlayerDataBag"]
    xp = int(pdb.get("EnforcerXP", 0))
    levels = state.XP_LEVELS
    level = max(1, sum(1 for need in levels if xp >= need))
    return ok(
        mAccountID=pdb["AccountID"], mPlayerName=pdb["Name"], mXP=xp, mXPLevel=level,
        mFace=pdb.get("Face", "0"), mRaidAttempts=0, mRaidSuccesses=0, mRacketsCompleted=0,
        mTotalTurfPointsPossible=0, mTurfPoints=0, mRaidFaceEarned="0", mExportFaceEarned="0",
        mAvatarURL=pdb.get("AvatarUrl", ""), mIsAI=is_ai, mUpgrades=[], mWeaponResults=[],
    )


def _find_player(player_id):
    if player_id in (_player()["PlayerDataBag"]["PlayerDataID"], _player()["PlayerDataBag"]["AccountID"]):
        return _player(), False
    for o in STATE.data["opponents"]:
        pdb = o["data"]["PlayerDataBag"]
        if player_id in (pdb["PlayerDataID"], pdb["AccountID"]):
            return o["data"], True
    return _player(), False


@route("game", "profile/playerdata")
def profile(req, method, parts, body, query):
    return _profile(*_find_player(parts[2] if len(parts) > 2 else ""))


@route("game", "profile/playerdatabyplayerdataid")
def profile_by_id(req, method, parts, body, query):
    return _profile(*_find_player(parts[2] if len(parts) > 2 else ""))


@route("game", "playerdata/addfriendbyplayername")
def add_friend_by_name(req, method, parts, body, query):
    return ok()


@route("game", "metagame/buyopponentintel")
def buy_opponent_intel(req, method, parts, body, query):
    return ok(mDataBags=_bags(ALL_OWN))


# ---- game service: favours, informants, upgrades, inventory ----------------------
def _ok_bags(**extra):
    return ok(mDataBags=_bags(ALL_OWN), **extra)


@route("game", "favours/usefavour")
def use_favour(req, method, parts, body, query):
    # favours/usefavour/<favour instance id>. The reply must name the favour's *definition*
    # UID inside mFavourUsageResult: the client stores it and the UI later resolves it to a
    # FavourType, crashing on a UID that does not resolve.
    ident = parts[2] if len(parts) > 2 else ""
    with STATE.lock:
        player = _player()
        favours = player.setdefault("FavoursBag", [])
        fav = next((f for f in favours if f["FavourID"] == ident), None)
        if fav is None:
            return ok(mReturnCode=1, mMessage="Favour not owned", mDataBags=_bags(UPGRADE_BAGS))
        favours.remove(fav)
        definition = fav["FavourDefinitionUID"]
        info = state.FAVOUR_UPGRADES.get(definition)
        if info:
            # An upgrade favour unlocks its enforcer upgrade; it is owned but not yet slotted.
            owned = player.setdefault("EnforcerUpgradesBag", [])
            if not any(u["Item1"] == info["upgrade"] for u in owned):
                owned.append({"Item1": info["upgrade"], "Item2": None})
    STATE.save()
    return ok(mDataBags=_bags(UPGRADE_BAGS), mFavourUsageResult={"mUsedFavourUid": definition})


@route("game", "favours/sellfavours")
def sell_favours(req, method, parts, body, query):
    return _ok_bags()


@route("game", "informants")
def informants(req, method, parts, body, query):
    return _ok_bags()


def _next_favour_id(player):
    return state.next_favour_id(player)


def _informant_favour(informant_uid):
    """Give the player the favour an informant (contact) hands out.

    The reply names the favour's definition UID at the root; the client resolves it to a
    FavourType for the "favour received" card. The turf intro's contact step polls for an
    owned favour whose definition is named FTPE_01, so the favour stays in the bag until
    the client uses it.
    """
    informant = state.INFORMANT_TYPES.get(str(informant_uid))
    if informant is None:
        return ok(mReturnCode=1, mMessage="Informant not found.", FavourDefinitionUID="0",
                  mDataBags=_bags(FAVOUR_BAGS))
    first = informant.get("mFirstFavourType") or {}
    definition = first.get("$handle") if isinstance(first, dict) else None
    if not definition:
        return ok(mReturnCode=1, mMessage="This contact has nothing for you.",
                  FavourDefinitionUID="0", mDataBags=_bags(FAVOUR_BAGS))
    price = informant.get("mPrice") or {}
    cost = int(price.get("mAmount") or 0)
    kind = "CleanMoney" if "Clean" in str(price.get("mCurrencyType")) else "DirtyMoney"
    with STATE.lock:
        player = _player()
        if cost and _money(kind) < cost:
            return ok(mReturnCode=1, mMessage="You can't afford this contact.",
                      FavourDefinitionUID="0", mDataBags=_bags(FAVOUR_BAGS))
        if cost:
            _money(kind, -cost)
        favours = player.setdefault("FavoursBag", [])
        favours.append(state.favour_instance(_next_favour_id(player), definition))
    STATE.save()
    return ok(mMessage="Favor collected successfully.", FavourDefinitionUID=str(definition),
              mDataBags=_bags(FAVOUR_BAGS))


@route("game", "informants/buyfrominformanttype")
def buy_from_informant_type(req, method, parts, body, query):
    # informants/buyfrominformanttype/<InformantType uid>
    return _informant_favour(parts[2] if len(parts) > 2 else "0")


@route("game", "informants/claimfrominformant")
def claim_from_informant(req, method, parts, body, query):
    # informants/claimfrominformant/<informant id>. No informant instances are tracked, so
    # the id is accepted when it is an InformantType uid.
    return _informant_favour(parts[2] if len(parts) > 2 else "0")


UPGRADE_BAGS = (state.BAG_PLAYER | state.BAG_CURRENCY | state.BAG_FAVOURS
                | state.BAG_ENFORCER_UPGRADES)
FAVOUR_BAGS = UPGRADE_BAGS | state.BAG_INVENTORY


def _find_upgrade_favour(ident):
    """Locate an owned upgrade favour by instance id, definition UID or upgrade UID."""
    for fav in _player().setdefault("FavoursBag", []):
        info = state.FAVOUR_UPGRADES.get(fav["FavourDefinitionUID"])
        if info and ident in (fav["FavourID"], fav["FavourDefinitionUID"], info["upgrade"]):
            return fav, info
    return None, None


@route("game", "playerdata/equipenforcerupgrade")
def equip_enforcer_upgrade(req, method, parts, body, query):
    # playerdata/equipenforcerupgrade/<slot>/<enforcer upgrade uid>. Owned upgrades live in
    # EnforcerUpgradesBag as {Item1: upgrade uid, Item2: slot}; Item2 is null while the
    # upgrade is unlocked but not slotted (the client reads null as slot -1).
    slot = _int(parts[2] if len(parts) > 2 else None)
    ident = parts[3] if len(parts) > 3 else ""
    with STATE.lock:
        player = _player()
        owned = player.setdefault("EnforcerUpgradesBag", [])
        entry = next((u for u in owned if u["Item1"] == ident), None)
        if entry is None:
            # Tolerate being handed a still-unused favour: unlock it on the spot.
            fav, info = _find_upgrade_favour(ident)
            if fav is None:
                return ok(mReturnCode=1, mMessage="Upgrade not owned", mDataBags=_bags(UPGRADE_BAGS))
            player["FavoursBag"].remove(fav)
            entry = {"Item1": info["upgrade"], "Item2": None}
            owned.append(entry)
        for other in owned:
            if other is not entry and other["Item2"] == slot:
                other["Item2"] = None
        entry["Item2"] = slot
    STATE.save()
    return ok(mDataBags=_bags(UPGRADE_BAGS))


@route("game", "playerdata/unequipenforcerupgrade")
def unequip_enforcer_upgrade(req, method, parts, body, query):
    # playerdata/unequipenforcerupgrade/<slot>: the upgrade stays owned but leaves its slot.
    slot = _int(parts[2] if len(parts) > 2 else None)
    with STATE.lock:
        for u in _player().setdefault("EnforcerUpgradesBag", []):
            if u["Item2"] == slot:
                u["Item2"] = None
    STATE.save()
    return ok(mDataBags=_bags(UPGRADE_BAGS))


@route("game", "playerdata/sellenforcerupgrade")
def sell_enforcer_upgrade(req, method, parts, body, query):
    # playerdata/sellenforcerupgrade/<enforcer upgrade uid>: drop an owned upgrade.
    ident = parts[2] if len(parts) > 2 else ""
    with STATE.lock:
        owned = _player().setdefault("EnforcerUpgradesBag", [])
        owned[:] = [u for u in owned if u["Item1"] != ident]
    STATE.save()
    return ok(mDataBags=_bags(UPGRADE_BAGS))


@route("game", "enforcer/sellinventoryitem")
def sell_inventory_item(req, method, parts, body, query):
    return _ok_bags()


@route("game", "itemkey/unlock")
def item_key_unlock(req, method, parts, body, query):
    return ok(mReturnCode=1, mMessage="Item keys are not available on the local server.")


@route("game", "purchase")
def purchase(req, method, parts, body, query):
    # purchase/<store uid>/<store item uid>. Unlike the other action endpoints, the reply
    # nests everything under "data": the bags as data.mDataBags and the bought item's UID as
    # data.CortexUID. data.ItemContext lists what a pack held:
    #   * a pack bundle (what the favour shop actually sells): one record per pack,
    #     {"StoreItemIDStr": pack uid, "FavourResults": [{"FavourDefinitionUIDString": uid}, ...]};
    #   * a bare pack: one {"FavourDefinitionUIDString": uid} per favour.
    # Packs are opened on the spot. What the real packs held was decided by the server; here
    # every pack holds upgrade favours, the kind this server can honour.
    #
    # Everything with mPurchaseOperation "AddToInventory" - clothing, outfits, vehicles -
    # goes into the InventoryBag, which is what the client reads to decide that the player
    # owns it. An outfit also goes straight onto the player: its pieces are what the client
    # draws, so buying "Swat Uniform" makes the player look like one without a trip to the
    # wardrobe. The vendor items are pure currency transactions and store nothing.
    store_uid, item_uid = (parts[1:3] + ["", ""])[:2]
    catalogue = state.STORE_ITEMS.get(store_uid)
    if catalogue is None or item_uid not in catalogue["items"]:
        return _ok_bags()
    currency, price = _store_price(catalogue, item_uid)
    data = {"CortexUID": item_uid}
    worn = False
    with STATE.lock:
        player = _player()
        if _money(CURRENCY_BAG_KEY[currency]) < price:
            return ok(mReturnCode=1, mMessage="You can't afford this item.")
        _money(CURRENCY_BAG_KEY[currency], -price)

        def open_pack():
            return [{"FavourDefinitionUIDString": uid}
                    for uid in _award_favours(player, PACK_FAVOUR_COUNT)]

        if catalogue["type"] == "UFG::StoreBundle":
            data["ItemContext"] = [{"StoreItemIDStr": pack, "FavourResults": open_pack()}
                                   for pack in catalogue["packs"][item_uid]]
        elif catalogue["type"] == "UFG::StoreContactBundle":
            data["ItemContext"] = open_pack()
        elif catalogue["type"] != "UFG::VendorItem":
            bag = player.setdefault("InventoryBag", [])
            pieces = (catalogue.get("pieces") or {}).get(item_uid) or []

            def store(uid):
                if not any(i.get("StoreItemID") == uid for i in bag):
                    next_id = max([7000] + [_int(i.get("InventoryItemID")) for i in bag]) + 1
                    bag.append(state.inventory_item(next_id, uid,
                                                    player["PlayerDataBag"]["AccountID"]))

            store(item_uid)
            if catalogue["type"] == "UFG::StoreOutfit":
                for piece in pieces:
                    store(piece)
                custom = player.setdefault("CustomizationBag", state.new_customization_bag())
                custom["ClothingOutfitCortexUID"] = item_uid
                if pieces:
                    custom["ClothingStoreItemUIDs"] = list(pieces)
                worn = True
    STATE.save()
    mask = ALL_OWN | state.BAG_INVENTORY | state.BAG_FAVOURS
    if worn:
        mask |= state.BAG_CUSTOMIZATION
    data["mDataBags"] = _bags(mask)
    return ok(data=data)


# ---- game service: black market --------------------------------------------------
@route("game", "blackmarket/quotes")
def blackmarket_quotes(req, method, parts, body, query):
    # The reply is a bare ARRAY of quotes: the client indexes the root value directly, and
    # an object here (even {"mReturnCode": 0}) crashes it.
    quotes = []
    for c in state.COMMODITIES:
        if c.get("mVirtualCommodity"):
            continue
        floor, ceiling = float(c.get("mFloorPrice") or 0), float(c.get("mCeilingPrice") or 0)
        sell = (floor + ceiling) / 2.0
        buy = sell * (1.0 + float(c.get("mMarkUp") or 0))
        quotes.append({
            "CommodityName": c.get("mDisplayName", ""),
            "CommodityUID": c["$uid"],
            "BuyPrice": buy,
            "DiscountedBuyPrice": buy,
            "SellPrice": sell,
            "BonusSellPrice": sell,
            "AmountInMarket": int(c.get("mFloorUnits") or 0),
        })
    return quotes


@route("game", "blackmarket")
def blackmarket_trade(req, method, parts, body, query):
    return _ok_bags(AmountPaid=0, AmountBought=0, AmountEarned=0, AmountSold=0)


# ---- game service: operations, exports, encounters, crime scenes, errands, trials --
@route("game", "operation/endoperation")
def end_operation(req, method, parts, body, query):
    return _ok_bags(mRewardType=0, mReward="0")


@route("game", "export/startmission")
def export_start(req, method, parts, body, query):
    return _ok_bags()


@route("game", "export/endmission")
def export_end(req, method, parts, body, query):
    # export/endmission/<mission uid>/<result>
    success = _int(parts[3] if len(parts) > 3 else None) != 0
    money = 2500 if success else 0
    with STATE.lock:
        _money("DirtyMoney", money)
    STATE.save()
    return _ok_bags(mFaceReward=0, mMoneyReward=money)


@route("game", "worldencounter/endencounter")
def end_world_encounter(req, method, parts, body, query):
    return _ok_bags(mRewardType=0, mReward="0")


@route("game", "openworldcrimescenecollection")
def collect_open_world_crime_scene(req, method, parts, body, query):
    return _ok_bags(CrimeSceneID=parts[1] if len(parts) > 1 else "0", FaceCollected=0)


@route("game", "raidcrimescenecompletion")
def complete_raid_crime_scene(req, method, parts, body, query):
    return _ok_bags(CrimeSceneID=parts[1] if len(parts) > 1 else "0", FaceCollected=0)


# ---- game service: jobs ("errands") ------------------------------------------------
# game/errands hands out a board of up to three jobs a day. The client evaluates an
# errand's completion conditions itself - they are its own stat counters - reports the
# job with errands/end/<slot> and collects the reward with errands/claimreward/<slot>.
# The board is rebuilt once a day (UTC) and survives restarts in state.json.
def _utc_string(ts=None):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(time.time() if ts is None else ts))


def _job_slot(board, index):
    for slot in board["Slots"]:
        if slot and int(slot.get("SlotIndex", -1)) == index:
            return slot
    return None


def _job_payload(slot):
    """A GameService ErrandSlotData record.

    The state and start time are written both inside mErrand and at the root: which of
    the two the client reads is not confirmed (its errands/change parser reads mErrand
    and then mState through a helper), and members it does not declare are ignored.
    """
    inner = {
        "mState": int(slot.get("State", state.ERRAND_ACTIVE)),
        "mTimeBegun": _utc_string(slot.get("Begun") or time.time()),
        "mErrandUID": str(slot["ErrandUID"]),
        "mFavourCortexUIDs": [str(u) for u in slot.get("Favours") or []],
        "mFavourPackCortexUID": str(slot.get("Pack") or "0"),
    }
    payload = {
        "mSlotIndex": int(slot.get("SlotIndex", 0)),
        "mErrand": inner,
        "mErrandUID": inner["mErrandUID"],
        "mState": inner["mState"],
        "mTimeBegun": inner["mTimeBegun"],
        "mFavourCortexUIDs": inner["mFavourCortexUIDs"],
        "mFavourPackCortexUID": inner["mFavourPackCortexUID"],
    }
    # With the game installed, Reflection.rdb names the real members of this class:
    # anything above it does not declare is dropped and 64-bit values are stringified.
    return schema.conform(payload, "GameService ErrandSlotData")


def _errand_board():
    with STATE.lock:
        board = state.refresh_errands(_player())
        jobs = [_job_payload(s) for s in board["Slots"] if s]
        seconds = state.seconds_to_refresh()
    STATE.save()
    return ok(mErrands=jobs, mSecondsToRefresh=seconds, mServerUTC=_utc_string())


def _completed_payload(player, entry):
    payload = {
        "CompletedErrandID": str(entry["uid"]),
        "PlayerDataID": player["PlayerDataBag"]["PlayerDataID"],
        "mErrandUID": str(entry["uid"]),
        "mTimesCompleted": int(entry["times"]),
        "mLastUpdated": _utc_string(entry["last"]),
    }
    return schema.conform(payload, "GameService CompletedErrandSlotData")


def _errand_history():
    with STATE.lock:
        player = _player()
        records = [_completed_payload(player, e) for e in state.completed_errands(player)]
    # The response class's own member name is not in the client's string table (its
    # callbacks only name the record type), so the list is offered under both likely
    # names; the client ignores the one it does not declare.
    return ok(mErrands=records, mCompletedErrands=records, mServerUTC=_utc_string())


def _errand_end(index):
    with STATE.lock:
        player = _player()
        board = state.refresh_errands(player)
        if _job_slot(board, index) is None:
            return ok(mReturnCode=1, mMessage="There is no job in that slot.",
                      mSlotIndex=index, mServerUTC=_utc_string())
        slot = state.end_errand(player, index)
        payload = _job_payload(slot)
    STATE.save()
    return ok(mSlotIndex=index, mErrand=payload["mErrand"], mErrandUID=payload["mErrandUID"],
              mState=payload["mState"], mTimeBegun=payload["mTimeBegun"],
              mServerUTC=_utc_string())


def _errand_claim(index):
    with STATE.lock:
        player = _player()
        state.refresh_errands(player)
        slot, paid, reason = state.claim_errand(player, index)
        if slot is None:
            return ok(mReturnCode=1, mMessage=reason, mFavourCortexUIDs=[],
                      mFavourPackCortexUID="0", mDataBags=_bags(ALL_OWN | state.BAG_FAVOURS))
        bags = _bags(ALL_OWN | state.BAG_FAVOURS)
        payload = _job_payload(slot)
    STATE.save()
    return ok(mSlotIndex=index, mErrand=payload["mErrand"], mErrandUID=payload["mErrandUID"],
              mState=payload["mState"], mRewardType=paid["reward_type"],
              mFavourCortexUIDs=[str(u) for u in paid["favours"]],
              mFavourPackCortexUID=str(paid["pack"]), mDataBags=bags)


def _errand_change(index):
    with STATE.lock:
        player = _player()
        state.refresh_errands(player)
        slot, reason = state.change_errand(player, index)
        if slot is None:
            return ok(mReturnCode=1, mMessage=reason, mServerUTC=_utc_string())
        payload = _job_payload(slot)
    STATE.save()
    return ok(mSlotIndex=index, mErrand=payload["mErrand"], mErrandUID=payload["mErrandUID"],
              mState=payload["mState"], mTimeBegun=payload["mTimeBegun"],
              mServerUTC=_utc_string())


# ---- game service: challenges ("community trials") ---------------------------------
def _trial_uid(bag):
    return state.trial_instance_id(bag)


def _trial_containers(bag):
    """Previous / current / next UFG::TrialContainer cards."""
    count = max(1, len(state.CHALLENGES))
    span = state.CHALLENGE_ROTATION_SECONDS
    index = bag["Index"] % count

    def card(offset, start, end):
        if not state.CHALLENGES:
            return {}
        slot = index + offset
        card_data = state.trial_container(bag, state.CHALLENGES[slot % count], start, end,
                                          slot % count)
        return schema.conform(card_data, "UFG::TrialContainer")

    return {
        "mPreviousContainer": card(-1, bag["Start"] - span, bag["Start"]),
        "mCurrentContainer": card(0, bag["Start"], bag["End"]),
        "mNextContainer": card(1, bag["End"], bag["End"] + span),
    }


def _trial_report(value=None):
    with STATE.lock:
        player = _player()
        bag, challenge, report = state.trial_snapshot(player, value)
        containers = _trial_containers(bag)
        tiers = (challenge or {}).get("tiers") or []
        rewards = []
        for tier_index in report["reached"]:
            tier = tiers[tier_index]
            tuner_uid = str(tier.get("tuner") or "")
            rewards.append(schema.conform({
                "mGameRewardTunerName": tuner_uid,
                "mReward": state.CHALLENGE_TUNERS.get(tuner_uid, {}),
                "mRewardIndex": tier_index,
                "mTarget": int(tier.get("target", 0)),
            }, "UFG::TrialReward"))
        top = str(tiers[report["reached"][-1]].get("tuner") or "") if report["reached"] else ""
        progress = {
            "mTrial": (challenge or {}).get("name", ""),
            "mTrialInstance": _trial_uid(bag),
            "mProgress": int(report["progress"]),
            "mTarget": int(report["target"]),
            "mTrialStartSeconds": int(bag["Start"]),
            "mTrialEndSeconds": int(bag["End"]),
            "mOptedIn": bool(report["opted_in"]),
            "mCompleted": bool(report["target"] and report["progress"] >= report["target"]),
            "mRewardList": rewards,
        }
        progress.update(containers)
        progress = schema.conform(progress, "UFG::TrialPlayerProgress")
        for reward in report["granted"]:
            print("challenge: tier %d paid %s (%s)"
                  % (reward["index"], reward["paid"], reward["amount"]), flush=True)
        bags = _bags(ALL_OWN | state.BAG_FAVOURS)
    STATE.save()
    return ok(mPlayerProgress=progress, mRewardList=rewards, mGameRewardTunerName=top,
              mReward=state.CHALLENGE_TUNERS.get(top, {}), mDataBags=bags)


@route("game", "errands/end")
def errand_end(req, method, parts, body, query):
    return _errand_end(_int(parts[2] if len(parts) > 2 else None, -1))


@route("game", "errands/claimreward")
def errand_claim(req, method, parts, body, query):
    return _errand_claim(_int(parts[2] if len(parts) > 2 else None, -1))


@route("game", "errands/change")
def errand_change(req, method, parts, body, query):
    return _errand_change(_int(parts[2] if len(parts) > 2 else None, -1))


@route("game", "trial")
def trial_progress(req, method, parts, body, query):
    # trial/reportprogress/<value>: the client reports its own challenge progress here
    # and expects the same report trialsscreenvisits gets back.
    return _trial_report(_int(parts[2] if len(parts) > 2 else None, None))


@route("game", "trialinstance")
def trial_instance(req, method, parts, body, query):
    return _trial_report()


@route("game", "trialinstance/optin")
def trial_optin(req, method, parts, body, query):
    # trialinstance/optin/<trial instance id>/<trial name>. The client keys the result
    # by the trial's own name, so the reply carries "<name>": true.
    ident = parts[2] if len(parts) > 2 else ""
    name = parts[3] if len(parts) > 3 else ""
    with STATE.lock:
        bag = state.trials(_player())
        bag["OptedIn"] = state.TRIAL_OPT_IN
        if name:
            bag["Name"] = name
    STATE.save()
    reply = ok(mTrial=name, mTrialInstance=ident, mOptedIn=True, removeslotcooldowns=False)
    if name:
        reply[name] = True
    return reply


@route("game", "trialsscreenvisits")
def trials_screen_visits(req, method, parts, body, query):
    return _trial_report()


@route("game", "calltoactionclicks")
def call_to_action_clicks(req, method, parts, body, query):
    # Telemetry: which call-to-action button the player pressed. Nothing to answer.
    return ok()


@route("game", "triads")
def triads_other(req, method, parts, body, query):
    # Player-run triads (clans) need other players; report none available.
    return ok(mTriads=[], mTriadMembers=[], mTriadInvites=[])


# ---- commerce / messaging / accounts ---------------------------------------------
@route("commerce", "storelink")
def store_link(req, method, parts, body, query):
    return ok(mStoreURL="")


@route("messaging", "channel/notices")
def notices(req, method, parts, body, query):
    return ok()


@route("social", "messaging")
def private_messages(req, method, parts, body, query):
    return ok()


def _account_info(req, method, parts, body, query):
    return ok(AvatarUrl="", SEAvatarUrl="", FacebookLinked=False, FacebookAvatarUrl="",
              FacebookLink="", SteamLinked=False, SteamAvatarUrl="", SteamLink="",
              GravatarUrl="", GravatarLink="", Email="")


def _twitter_status(req, method, parts, body, query):
    return ok(mText="", mStatusUrl="", mTwitterUrl="", mUserName="", mTwitterUserName="",
              mProfileImageUrl="")


def _friend_referral(req, method, parts, body, query):
    return ok(data={"URL": ""})


for _service in ("login", "accounts", "social"):
    route(_service, "accountinfo")(_account_info)
    route(_service, "updateavatar")(_account_info)
    route(_service, "twitterstatus")(_twitter_status)
    route(_service, "friendreferral")(_friend_referral)
