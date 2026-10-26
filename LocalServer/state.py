"""Persistent player/world state for the local Triad Wars server.

Field names and encodings follow what the client deserialises (see NOTES.md):
  * 32-bit integers, floats and bools are plain JSON values;
  * every 64-bit value (ids, UIDs, object handles, money) is a decimal *string*,
    because the client's JSON library would otherwise round it through a double;
  * symbols (e.g. LocationName) are strings that the client hashes itself.
"""
import copy
import json
import os
import random
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STATE_PATH = os.path.join(HERE, "state.json")

# ---- feature switches --------------------------------------------------------
# Each emulated screen can be turned off on its own. Off means "answer exactly the
# way the original emulation did": an empty but valid reply, and nothing added to
# any data bag. If a screen ever crashes the game, switch that one off, restart,
# and the rest of the game still runs - that also tells you where the problem is.
# Play-TriadWars.bat safe sets TW_SAFE_MODE=1 in the server's environment: everything
# below starts switched off, which is the quickest way to tell a bad reply from a bad
# install. Nothing else reads this variable.
_SAFE_MODE = os.environ.get("TW_SAFE_MODE", "").strip().lower() in ("1", "true", "yes")

FEATURE_JOBS = not _SAFE_MODE          # game/errands, completederrands, errands/*
FEATURE_CHALLENGES = not _SAFE_MODE    # trialsscreenvisits, trial/reportprogress, trialinstance/*
FEATURE_EXPORTS = not _SAFE_MODE       # export/startmission, export/endmission, export board
FEATURE_PRODUCTION = not _SAFE_MODE    # rackets fill CurrentProduction / Production1..10
# The export board travels in its own data bag. The client asks for it with mask
# 0x800; this also adds it to every reply that carries the turf bag, which is a
# guess - turn it on only if the export screen is empty.
EXPORT_BAG_WITH_TURF = False

with open(os.path.join(HERE, "data", "gamedb_min.json"), encoding="utf-8") as f:
    DB = json.load(f)

# ---- game database lookups ---------------------------------------------------
RACKET_TYPES = {r["$uid"]: r for r in DB["UFG::RacketType"]}
_BY_CAPTION = {}
for _r in DB["UFG::RacketType"]:
    _BY_CAPTION.setdefault(_r["mCaption"], []).append(_r)

SAFEHOUSE_UID = _BY_CAPTION["RACKET_SAFEHOUSE"][0]["$uid"]
WAREHOUSE_UID = _BY_CAPTION["RACKET_WAREHOUSE"][0]["$uid"]
TEMPLE_UID = _BY_CAPTION["RACKET_TEMPLE"][0]["$uid"]
EMPTY_UID = {r["mSize"]: r["$uid"] for r in _BY_CAPTION["Empty"]}   # SMALL / MEDIUM / LARGE

TURF_TEMPLATES = DB["UFG::TurfTemplate"]
TRIADS = DB["UFG::SelectableTriad"]
COMMODITIES = DB["UFG::CommodityType"]
XP_LEVELS = DB["UFG::XPDefinitions"][0]["mAuthoredXPLevels"]    # XP needed to reach each level
METAGAME = DB["UFG::MetagameInfoWrapper"][0]                    # build/upgrade costs and times
INFORMANT_TYPES = {i["$uid"]: i for i in DB["UFG::InformantType"]}  # contacts and their favours

# Upgrade favours: favour definition UID -> {"upgrade": enforcer upgrade UID, "name", "ftpe"}.
with open(os.path.join(HERE, "data", "favour_upgrades.json"), encoding="utf-8") as f:
    FAVOUR_UPGRADES = json.load(f)
FTPE_RAID_FAVOURS = sorted(uid for uid, v in FAVOUR_UPGRADES.items() if v["ftpe"])

# Jobs ("errands") and Challenges ("community trials"), built by tools/errands.py and
# tools/challenges.py from the game's own object database. Both are optional: a server
# copied without them answers both screens with an empty but valid result, as before.
def _load_data(name, default):
    path = os.path.join(HERE, "data", name)
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


ERRAND_DB = _load_data("errands.json", {})
ERRANDS = ERRAND_DB.get("errands") or []
ERRANDS_BY_UID = {e["uid"]: e for e in ERRANDS}
ERRAND_REWARDS = ERRAND_DB.get("rewards") or {}

CHALLENGE_DB = _load_data("challenges.json", {})
CHALLENGES = CHALLENGE_DB.get("challenges") or []
CHALLENGE_TUNERS = CHALLENGE_DB.get("tuners") or {}
CHALLENGE_ROTATION_SECONDS = int(CHALLENGE_DB.get("rotationDays", 7)) * 86400

EXPORT_DB = _load_data("exports.json", {})
EXPORT_COMMODITIES = EXPORT_DB.get("commodities") or []
EXPORT_BY_UID = {c["uid"]: c for c in EXPORT_COMMODITIES}
EXPORT_FACE_PERCENT = EXPORT_DB.get("facePercentOfMoney") or []
EXPORT_MARKUP = EXPORT_DB.get("blackMarketMarkup") or []
EXPORT_MISSION_TYPES = EXPORT_DB.get("missionTypes") or []

# Store catalogues: store UID -> {"name", "type", "items": [store item UIDs]}. Built by
# tools/store_items.py; stores that are not listed have nothing for sale.
with open(os.path.join(HERE, "data", "store_items.json"), encoding="utf-8") as f:
    STORE_ITEMS = json.load(f)

# The open world has seven turf sites. The client maps each site's symbol to a
# slot 1..7 and looks the owner up as opponents[slot - 1].
TURF_LOCATIONS = ["Turf-Turf%05d" % n for n in range(1, 8)]

# mRequestedBags bit for each data bag in a serialized-player-data response.
BAG_PLAYER = 0x0001
BAG_CUSTOMIZATION = 0x0002
BAG_INFORMANTS = 0x0004
BAG_FAVOURS = 0x0008
BAG_ACTIVE_FAVOURS = 0x0010
BAG_CURRENCY = 0x0040
BAG_TURF = 0x0080
BAG_INVENTORY = 0x0100
BAG_SHIELD = 0x0200
BAG_TRIALS = 0x0400
BAG_EXPORT_REWARDS = 0x0800
BAG_ENFORCER_UPGRADES = 0x1000

AI_NAMES = ["Big Smile Lee", "Uncle Po", "Sonny Wo", "Broken Nose Jiang",
            "Two Chin Tsao", "Pockmark Cheuk", "Dogeyes Lin"]


def racket_instance(type_uid, level=1, block=0, slot=0, state=0, staff=0):
    r = {
        "RacketTypeUID": str(type_uid),
        "RacketLevel": level,
        "Staff": staff,
        "BlockIndex": block,
        "RacketState": state,
        "SlotIndex": slot,
        "CurrentProduction": 0,
        "Stats": {
            "MaxStaff": 0,
            "OutputMultiplier": 1.0,
            "ProductionTimeDivision": 1.0,
            "UpgradeDiscountMultiplier": 1.0,
            "ProductionInputDiscountDivision": 1.0,
            "NoStaffSpawn": 0,
        },
    }
    # Production1..10 is the racket type's per-level output table; CurrentProduction is
    # what has piled up since the last collection (see settle_production). With
    # FEATURE_PRODUCTION off both stay at 0, which is what the earlier builds sent.
    outputs = racket_outputs(type_uid) if FEATURE_PRODUCTION else []
    for n in range(1, 11):
        r["Production%d" % n] = outputs[n - 1] if n - 1 < len(outputs) else 0
    return r


SAFEHOUSE_BLOCK = -1      # BlockIndex of the safehouse / warehouse / temple (off the grid)


def racket_outputs(type_uid):
    """mOutput1Amount per racket level (level 1 first): units made per production cycle."""
    return [int(level.get("mOutput1Amount") or 0)
            for level in (RACKET_TYPES.get(type_uid, {}).get("mLevels") or [])]


def racket_cycle_seconds(type_uid, level):
    """How long one production cycle takes at this level."""
    levels = RACKET_TYPES.get(type_uid, {}).get("mLevels") or []
    if not levels:
        return 0
    return int(levels[min(max(level - 1, 0), len(levels) - 1)].get("mProdTimeSeconds") or 0)


def new_turf(location, template_index=0, level=1):
    tpl = TURF_TEMPLATES[template_index % len(TURF_TEMPLATES)]
    return {
        "Turf": {
            "LocationName": location,
            # The safehouse, warehouse and temple are not on the block grid: the client
            # positions (and mouse-picks) them from the turf origin only when BlockIndex is
            # -1. Any grid index puts their pick boxes over that grid lot instead, so the
            # safehouse cannot be selected in the turf manager.
            "Safehouse": racket_instance(SAFEHOUSE_UID, level, SAFEHOUSE_BLOCK),
            "Warehouse": racket_instance(WAREHOUSE_UID, level, SAFEHOUSE_BLOCK),
            "Temple": racket_instance(TEMPLE_UID, level, SAFEHOUSE_BLOCK),
            "TurfTemplateUID": tpl["$uid"],
            "RacketsBuilt": "0",
        },
        # Only built rackets are listed. The client draws empty lots itself from the
        # template, and counts every entry here against the turf's build points
        # (safehouse level minus len(Rackets)), so placeholders leave none to spend.
        "Rackets": [],
        "Commodities": [],
    }


def new_player(player_id, name, triad_index, location, template_index=0, account_id=None):
    return {
        "PlayerDataBag": {
            "PlayerDataID": str(player_id),
            "Name": name,
            "Face": "0",
            "LaunderedMoney": 0,
            "LaundrySecondsRemaining": 0,
            "EnforcerXP": 0,
            "Triad": TRIADS[triad_index % len(TRIADS)]["$uid"],
            "AccountID": str(account_id if account_id is not None else player_id),
            "LastUsedVehicle": "0",
            "GameplayState": "",
            "AvatarUrl": "",
            "PreferMatchmakingWithFriends": False,
            "IsFavourListInitialized": True,
        },
        "CurrencyBag": {"CleanMoney": "25000", "DirtyMoney": "5000", "Gold": "0"},
        "TurfBag": new_turf(location, template_index),
        "FavoursBag": [],
        "EnforcerUpgradesBag": [],
        "InventoryBag": [],
        # Job board and challenge progress; not client data bags, so they are not
        # serialised into mDataBags (see State.bags).
        "ErrandsBag": new_errand_bag(),
        "TrialsBag": new_trial_bag(),
        # Export board. The client's ExportRewardsBag is built from this on the fly.
        "ExportBag": new_export_bag(),
    }


def favour_instance(favour_id, definition_uid, is_new=1):
    """A favour the player owns (UFG::FavourInstanceData)."""
    return {
        "FavourID": str(favour_id),
        "SecondsElapsed": 0,
        "TotalSeconds": 0,
        "FavourDefinitionUID": str(definition_uid),
        "IsNew": is_new,
    }


def next_favour_id(player):
    """A favour instance id that does not collide with the ones already owned."""
    used = [int(f.get("FavourID", 0)) for f in player.get("FavoursBag", [])]
    return max([5000 + len(player.get("EnforcerUpgradesBag", []))] + used) + 1


def grant_starting_favours(player):
    """Give the two upgrade favours the first-raid results screen awards.

    The client shows these as raid rewards without asking the server for them, then
    its upgrade tutorial expects them to be owned.
    """
    owned = {f["FavourDefinitionUID"] for f in player["FavoursBag"]}
    equipped = {u["Item1"] for u in player["EnforcerUpgradesBag"]}
    next_id = 5001 + len(player["FavoursBag"]) + len(player["EnforcerUpgradesBag"])
    for uid in FTPE_RAID_FAVOURS:
        if uid in owned or FAVOUR_UPGRADES[uid]["upgrade"] in equipped:
            continue
        player["FavoursBag"].append(favour_instance(next_id, uid))
        next_id += 1


# Items the first-time-experience contacts hand over. The client never asks the server
# for them: after speaking to a contact it just re-reads the InventoryBag and expects the
# item to be there (FTPE_01 "Ridge Sport" car, FTPE_02 "Cleaver").
FTPE_STORE_ITEMS = ["5666866140888112945", "4038481858749259483"]


def inventory_item(item_id, store_item_uid, account_id):
    """An owned store item (UFG::InventoryItemInstanceData)."""
    return {
        "SecondsElapsed": 0,
        "TotalSeconds": 0,
        "StoreItemID": str(store_item_uid),
        "InventoryItemID": str(item_id),
        "AccountID": str(account_id),
    }


def grant_starting_inventory(player):
    bag = player.setdefault("InventoryBag", [])
    owned = {i.get("StoreItemID") for i in bag}
    next_id = max([7000] + [int(i.get("InventoryItemID", 0)) for i in bag]) + 1
    account = player["PlayerDataBag"].get("AccountID", "1")
    for uid in FTPE_STORE_ITEMS:
        if uid not in owned:
            bag.append(inventory_item(next_id, uid, account))
            next_id += 1


# ---- "Jobs" (errands) ----------------------------------------------------------
# The client's Jobs screen is driven by game/errands: three daily slots, each holding
# one UFG::ErrandTypeMetadata (data/errands.json, built by tools/errands.py). The
# client checks the objective itself - errand completion conditions are its own stat
# counters - and tells the server with errands/end/<slot>; the reward is paid on
# errands/claimreward/<slot>. The server owns only the board: which errands are on
# offer, their state, and the payout.
ERRAND_SLOT_COUNT = 3
ERRAND_ACTIVE = 0       # on the board, objective not met yet
ERRAND_COMPLETED = 1    # objective met, reward waiting to be collected
ERRAND_COLLECTED = 2    # reward paid
# True: a collected slot refills with the next available job the next time the board is
# read (the hub's "Next Job"), so the Jobs screen keeps working through the chain.
# False: the three slots are fixed for the day and only the UTC rollover replaces them.
ERRAND_REFILL_COLLECTED = True

RARITY_ORDER = ["Common", "Rare", "VeryRare", "Epic"]

# Reward type -> (what it pays, which ErrandRewards table it reads).
# The tables are indexed by XP level (level 1 = index 0). UFG::ErrandRewards has dirty
# money, face and GOLD tables and no clean-money ladder at all - the "clean money"
# reward types are the gold ones, and the tutorial errands pay the fixed gold amounts
# (the Jobs tutorial celebrates "GoldEarned").
ERRAND_REWARD_TABLES = {
    "REWARD_MONEY_LOW": ("DirtyMoney", "mRewardDirtyMoneyLow"),
    "REWARD_MONEY_MID": ("DirtyMoney", "mRewardDirtyMoneyMedium"),
    "REWARD_MONEY_HIGH": ("DirtyMoney", "mRewardDirtyMoneyHigh"),
    "REWARD_CLEAN_MONEY_LOW": ("Gold", "mRewardGoldLow"),
    "REWARD_CLEAN_MONEY_MID": ("Gold", "mRewardGoldMedium"),
    "REWARD_CLEAN_MONEY_HIGH": ("Gold", "mRewardGoldHigh"),
    "REWARD_FACE_LOW": ("Face", "mRewardFaceLow"),
    "REWARD_FACE_MID": ("Face", "mRewardFaceMedium"),
    "REWARD_FACE_HIGH": ("Face", "mRewardFaceHigh"),
    "REWARD_FAVOUR": ("Favour", None),
    "REWARD_CLEAN_MONEY_TUTORIAL1": ("Gold", "mRewardGoldTutorial1"),
    "REWARD_CLEAN_MONEY_TUTORIAL2": ("Gold", "mRewardGoldTutorial2"),
    "REWARD_CLEAN_MONEY_TUTORIAL3": ("Gold", "mRewardGoldTutorial3"),
}

# mRewardType in the claim reply: the job reward category the collected card shows
# (JOB_REWARD_GOLD, _CASH, _FACE, _FAVOUR - the order the client's strings appear in).
# Only the card art depends on it, so a wrong guess is cosmetic.
JOB_REWARD_GOLD = 0
JOB_REWARD_CASH = 1
JOB_REWARD_FACE = 2
JOB_REWARD_FAVOUR = 3
JOB_REWARD_TYPES = {"Gold": JOB_REWARD_GOLD, "DirtyMoney": JOB_REWARD_CASH,
                    "Face": JOB_REWARD_FACE, "Favour": JOB_REWARD_FAVOUR}


# The job board does not turn over at midnight: UFG::MetagameInfoWrapper says which UTC
# hour it refreshes (10:00 in build 1.6.0.0), and how long a repeatable job stays off the
# board once it has been paid (96 hours).
ERRAND_REFRESH_HOUR_UTC = int(METAGAME.get("mErrandRefreshHourUTC", 0) or 0)
ERRAND_REPEATABLE_COOLDOWN_HOURS = int(METAGAME.get("mRepeatableErrandCooldownHours", 0) or 0)
# The turf stops at this safehouse level (one upgrade cost per level), but four enabled
# errands in build 1.6.0.0 ask for safehouse 15 - a level the shipped game cannot reach.
# Their requirement is clamped so they stay part of the ladder instead of being stranded.
MAX_SAFEHOUSE_LEVEL = len(METAGAME.get("mSafehouseUpgradeCosts") or []) or 10


def utc_day(now=None):
    """The job board's day: it changes at mErrandRefreshHourUTC, not at midnight."""
    shifted = (time.time() if now is None else now) - ERRAND_REFRESH_HOUR_UTC * 3600
    return time.strftime("%Y-%m-%d", time.gmtime(shifted))


def seconds_to_refresh(now=None):
    """Seconds until the job board rolls over (the next mErrandRefreshHourUTC)."""
    shifted = (time.time() if now is None else now) - ERRAND_REFRESH_HOUR_UTC * 3600
    return max(1, 86400 - int(shifted % 86400))


def xp_level(player):
    """The player's XP level, the same way profile/playerdata reports it."""
    xp = int(player["PlayerDataBag"].get("EnforcerXP", 0))
    return max(1, sum(1 for need in XP_LEVELS if xp >= need))


def safehouse_level(player):
    return int(player["TurfBag"]["Turf"]["Safehouse"].get("RacketLevel", 1))


def new_errand_bag():
    return {"Day": "", "Slots": [None] * ERRAND_SLOT_COUNT, "Completed": {}, "Changes": 0}


def errands(player):
    bag = player.setdefault("ErrandsBag", new_errand_bag())
    for key, value in new_errand_bag().items():
        bag.setdefault(key, value)
    return bag


def errand_slot(bag, index):
    """The slot with this mSlotIndex (slots are stored in index order)."""
    if index is None:
        return None
    for slot in bag["Slots"]:
        if slot and int(slot.get("SlotIndex", -1)) == int(index):
            return slot
    return None


def errand_uid(slot):
    return slot.get("ErrandUID") if slot else None


def errand_definition(slot):
    uid = errand_uid(slot)
    return ERRANDS_BY_UID.get(uid) if uid else None


def favour_pool(errand, level=1):
    """Upgrade favours that satisfy an errand's rarity/tag reward rule.

    Only the 137 upgrade favours are used: they are the ones the client can equip, so
    a job reward always turns into something the player can spend.
    """
    rule = errand.get("favour") or {}
    try:
        low = RARITY_ORDER.index(rule.get("min") or "Common")
        high = RARITY_ORDER.index(rule.get("max") or "Epic")
    except ValueError:
        low, high = 0, len(RARITY_ORDER) - 1
    tags = set(rule.get("tags") or [])
    pool = []
    for uid, info in FAVOUR_UPGRADES.items():
        if info.get("ftpe"):
            continue
        rarity = info.get("rarity")
        if rarity not in RARITY_ORDER or not low <= RARITY_ORDER.index(rarity) <= high:
            continue
        if int(info.get("xpLevel", 0)) > level:
            continue
        if tags and not tags.intersection(info.get("tags") or []):
            continue
        pool.append(uid)
    if not pool:
        pool = [uid for uid, info in FAVOUR_UPGRADES.items() if not info.get("ftpe")]
    return pool


def errand_available(bag, errand, level, safehouse, now=None, ignore_cooldown=False):
    if not errand.get("enabled"):
        return False
    required_safehouse = min(int(errand.get("safehouseLevel", 0) or 0), MAX_SAFEHOUSE_LEVEL)
    if errand.get("xpLevel", 0) > level or required_safehouse > safehouse:
        return False
    # mErrandLinks are the errands that must have been completed first ("Raid 2 Rivals"
    # links to "Raid A Rival"). With one link only the tutorial's first job is on offer.
    # A link to an errand the game itself disables is ignored: nothing can ever complete
    # it, so honouring it would strand every errand below it (two in build 1.6.0.0 hang
    # off the disabled "Finish a raid").
    for link in errand.get("links") or []:
        if link in bag["Completed"]:
            continue
        if ERRANDS_BY_UID.get(link, {}).get("enabled", True):
            return False
    done = bag["Completed"].get(errand["uid"])
    if not done:
        return True
    if not errand.get("repeatable"):
        return False
    # A repeatable job stays off the board for mRepeatableErrandCooldownHours after paying.
    if ignore_cooldown or not ERRAND_REPEATABLE_COOLDOWN_HOURS:
        return True
    now = time.time() if now is None else now
    return now - int(done.get("Last") or 0) >= ERRAND_REPEATABLE_COOLDOWN_HOURS * 3600


def errand_pool(player, bag, exclude=(), now=None):
    """Eligible errands, best first.

    The cooldown on repeatable jobs is dropped rather than handing the player an empty
    Jobs screen: with one safehouse level and a fresh profile only a handful of jobs are
    unlocked, and a 96 hour cooldown could otherwise empty all three slots.
    """
    level, safehouse = xp_level(player), safehouse_level(player)
    pool = [e for e in ERRANDS
            if e["uid"] not in exclude
            and errand_available(bag, e, level, safehouse, now)]
    if not pool:
        pool = [e for e in ERRANDS
                if e["uid"] not in exclude
                and errand_available(bag, e, level, safehouse, now, ignore_cooldown=True)]
    # Jobs the player has never done come before repeats: the one-time upgrade chain
    # ("Safehouse II", "Racket IV", "Warehouse III", ...) is the game's story progression,
    # and a repeatable job with a high priority would otherwise keep crowding it off the
    # board for ever.
    pool.sort(key=lambda e: (e["uid"] in bag["Completed"], -e["priority"], e["name"]))
    return pool


def new_errand_slot(player, errand, now=None, index=0):
    level = xp_level(player)
    favours = []
    if (errand.get("reward") or "").startswith("REWARD_FAVOUR"):
        pool = favour_pool(errand, level)
        if pool:
            favours = [random.choice(pool)]
    return {"SlotIndex": int(index), "ErrandUID": errand["uid"], "State": ERRAND_ACTIVE,
            "Begun": int(time.time() if now is None else now),
            "Favours": favours, "Pack": "0"}


def fill_errands(player, bag, now=None):
    """Put the highest-priority available errands into empty slots."""
    slotted = {errand_uid(s) for s in bag["Slots"] if s}
    for index, slot in enumerate(bag["Slots"]):
        if slot:
            continue
        pool = errand_pool(player, bag, exclude=slotted)
        if not pool:
            return
        bag["Slots"][index] = new_errand_slot(player, pool[0], now, index)
        slotted.add(pool[0]["uid"])


def refresh_errands(player, now=None):
    """Roll the board over and replace collected jobs.

    A new day (UTC) replaces every active and collected job; a completed but unclaimed
    one waits for its reward. Within a day, a collected slot gets the next available job
    the next time the board is read ("Next Job" on the hub), so finishing one job does
    not leave an empty card until midnight.
    """
    now = time.time() if now is None else now
    bag = errands(player)
    today = utc_day(now)
    if bag["Day"] != today:
        bag["Day"] = today
        bag["Changes"] = 0
        for index, slot in enumerate(bag["Slots"]):
            if slot and slot.get("State") in (ERRAND_ACTIVE, ERRAND_COLLECTED):
                bag["Slots"][index] = None
    elif ERRAND_REFILL_COLLECTED:
        for index, slot in enumerate(bag["Slots"]):
            if slot and slot.get("State") == ERRAND_COLLECTED:
                bag["Slots"][index] = None
    fill_errands(player, bag, now)
    return bag


def change_errand(player, index, now=None):
    """Replace the errand in a slot. Returns (slot, reason); slot is None on failure."""
    bag = errands(player)
    slot = errand_slot(bag, index)
    if slot is None:
        return None, "There is no job in that slot."
    slotted = {errand_uid(s) for s in bag["Slots"] if s}
    pool = errand_pool(player, bag, exclude=slotted)
    if not pool:
        return None, "No jobs available to change."
    fresh = new_errand_slot(player, pool[0], now, index)
    bag["Slots"][bag["Slots"].index(slot)] = fresh
    bag["Changes"] += 1
    return fresh, ""


def end_errand(player, index, now=None):
    """The client reports that the slot's objective is done."""
    slot = errand_slot(errands(player), index)
    if slot is None:
        return None
    if slot.get("State") == ERRAND_ACTIVE:
        slot["State"] = ERRAND_COMPLETED
        slot["Ended"] = int(time.time() if now is None else now)
    return slot


def _table_amount(name, level):
    table = ERRAND_REWARDS.get(name)
    if isinstance(table, list):
        if not table:
            return 0
        return int(table[min(max(level - 1, 0), len(table) - 1)])
    return int(table or 0)


def pay_errand_reward(player, errand, slot):
    """Credit the errand's reward and return what was paid (for the reply)."""
    kind, table = ERRAND_REWARD_TABLES.get(errand.get("reward"), ("None", None))
    level = xp_level(player)
    paid = {"kind": kind, "amount": 0, "favours": [], "pack": "0",
            "reward_type": JOB_REWARD_TYPES.get(kind, JOB_REWARD_GOLD)}
    if kind in ("DirtyMoney", "CleanMoney", "Gold"):
        amount = _table_amount(table, level) if table else 0
        bag = player["CurrencyBag"]
        bag[kind] = str(int(bag.get(kind, "0")) + amount)
        paid["amount"] = amount
    elif kind == "Face":
        amount = _table_amount(table, level) if table else 0
        pdb = player["PlayerDataBag"]
        pdb["Face"] = str(int(pdb.get("Face", "0")) + amount)
        paid["amount"] = amount
    elif kind == "Favour":
        pool = favour_pool(errand, level)
        picks = [uid for uid in slot.get("Favours") or [] if uid in FAVOUR_UPGRADES]
        if not picks and pool:
            picks = [random.choice(pool)]
        owned = player.setdefault("FavoursBag", [])
        for uid in picks:
            owned.append(favour_instance(next_favour_id(player), uid))
        paid["favours"] = picks
    return paid


def claim_errand(player, index, now=None):
    """Pay the slot's reward. Returns (slot, paid, reason)."""
    bag = errands(player)
    slot = errand_slot(bag, index)
    if slot is None:
        return None, None, "There is no job in that slot."
    errand = errand_definition(slot)
    if errand is None:
        return None, None, "That job no longer exists."
    if slot.get("State") == ERRAND_COLLECTED:
        return None, None, "That reward has already been collected."
    paid = pay_errand_reward(player, errand, slot)
    record = bag["Completed"].setdefault(errand["uid"], {"Times": 0, "Last": 0})
    record["Times"] += 1
    record["Last"] = int(time.time() if now is None else now)
    slot["State"] = ERRAND_COLLECTED
    slot["Claimed"] = record["Last"]
    return slot, paid, ""


def completed_errands(player):
    """One record per errand ever completed, newest first."""
    bag = errands(player)
    out = []
    for uid, record in bag["Completed"].items():
        errand = ERRANDS_BY_UID.get(uid)
        if errand is None:
            continue
        out.append({"uid": uid, "name": errand["name"],
                    "times": int(record.get("Times", 0)),
                    "last": int(record.get("Last", 0))})
    out.sort(key=lambda r: -r["last"])
    return out


def _for_client(value):
    """Drop the server's private bookkeeping (keys starting with "_") from a bag.

    Rackets carry "_ready_at"/"_total"/"_prod_at" while a build, an upgrade or a
    production cycle is running; the client never reads them, and the fewer members a
    reply has that the client's own classes do not declare, the safer it is.
    """
    if isinstance(value, dict):
        return {k: _for_client(v) for k, v in value.items() if not str(k).startswith("_")}
    if isinstance(value, list):
        return [_for_client(v) for v in value]
    return value


# ---- "Exports" ------------------------------------------------------------------
# The warehouse screen (and the map's export markers) run on export missions. The client
# keeps them in a data bag called ExportRewardsBag - OperationManager::PopulateFromExport
# RewardsBag in the client builds one mission per record - and starts/finishes them with
# export/startmission/<uid> and export/endmission/<uid>/<result>.
#
# What a mission is, is in the game's own data (data/exports.json, built by
# tools/exports.py):
#   * a commodity (UFG::CommodityType), made by one racket (mRacketOutput),
#   * the units to move: mExportAmounts[XP level] of that commodity,
#   * the money: units x the commodity's floor/ceiling price x the black market markup
#     for the level (mBlackMarketSellMarkupForMoneyReward),
#   * the face: the money x mFaceRewardPercentageOfMoneyReward[XP level] / 100.
# How many missions a player holds follows the warehouse level, which is what the client's
# "Upgrading your warehouse will increase your export targets" message is about.
EXPORT_SLOT_CAP = 10
EXPORT_UID_BASE = 900000000000000000       # our mission ids; far from any real object uid
# How the mission list is shaped inside the ExportRewardsBag. Empty = a bare array (the
# shape FavoursBag and InventoryBag use). The client's own string table has a symbol called
# ExportMissionSlots right next to the mission's member names, so if the warehouse screen
# stays empty this is the one line to change: "ExportMissionSlots".
EXPORT_BAG_MEMBER = ""


def new_export_bag():
    return {"Day": "", "NextUID": 0, "Missions": [], "Paid": [], "Attempts": 0, "Successes": 0}


def exports(player):
    bag = player.setdefault("ExportBag", new_export_bag())
    for key, value in new_export_bag().items():
        bag.setdefault(key, value)
    return bag


def export_level(player):
    """XP level clamped to the reward tables (they are authored for levels 1..10)."""
    return max(1, min(xp_level(player), len(EXPORT_FACE_PERCENT) or 10))


def export_slots(player):
    """Missions the player can hold at once: the warehouse's racket level."""
    warehouse = player["TurfBag"]["Turf"].get("Warehouse") or {}
    return max(1, min(int(warehouse.get("RacketLevel", 1) or 1), EXPORT_SLOT_CAP))


def _table_at(table, level, default):
    if not table:
        return default
    return table[min(max(level - 1, 0), len(table) - 1)]


def export_reward(commodity, level):
    """(units, money, face) for exporting this commodity at this level."""
    amounts = commodity.get("amounts") or []
    units = int(_table_at(amounts, level, 0))
    markup = float(_table_at(EXPORT_MARKUP, level, 1.0) or 1.0)
    money = int(round(units * float(commodity.get("price") or 0) * markup))
    percent = float(_table_at(EXPORT_FACE_PERCENT, level, 0.0) or 0.0)
    face = int(round(money * percent / 100.0))
    return units, money, face


def export_commodity_pool(player):
    """Commodities an export can be built from.

    A load has to fit in the warehouse at its current level (the client will not start an
    export whose units are not there), and the units have to be non-zero at this XP level
    (car parts have no export amount until level 3). Racket-made commodities come first.
    """
    level = export_level(player)
    fits = [c for c in EXPORT_COMMODITIES
            if _table_at(c["amounts"], level, 0) > 0
            and warehouse_capacity(player, c["uid"]) >= _table_at(c["amounts"], level, 0)]
    built = {r.get("RacketTypeUID") for r in player["TurfBag"].get("Rackets") or []}
    own = [c for c in fits if (c.get("racket") or {}).get("uid") in built]
    return own or fits


def new_export_mission(player, commodity, now=None, index=0):
    level = export_level(player)
    units, money, face = export_reward(commodity, level)
    # Stage the load in the warehouse: this is the "N exports currently ready" the hub
    # reports, and what the client checks before letting the export start.
    warehouse_set(player, commodity, max(units, warehouse_amount(player, commodity["uid"])))
    bag = exports(player)
    bag["NextUID"] = int(bag.get("NextUID", 0)) + 1
    uid = str(EXPORT_UID_BASE + bag["NextUID"])
    seconds = 300 + 60 * level
    return {
        # The client reads mCommodity1UID/mCommodity1Amount (its own strings); the rest is
        # what the screen needs to draw a card and what we need to pay it out.
        "ExportMissionUID": uid,
        "mExportMissionUID": uid,
        "mCommodity1UID": commodity["uid"],
        "mCommodity1Amount": units,
        "mCommodity2UID": "0",
        "mCommodity2Amount": 0,
        "mCommodityName": commodity.get("name", ""),
        "mCommodityTitle": commodity.get("title", ""),
        "mCommodityIconName": commodity.get("icon", ""),
        "mMoneyReward": money,
        "mFaceReward": face,
        "mLevel": level,
        "mOperationType": _table_at(EXPORT_MISSION_TYPES, 1 + (index % max(1, len(EXPORT_MISSION_TYPES))), ""),
        "mRacketTypeUID": (commodity.get("racket") or {}).get("uid", "0"),
        "mDurationSeconds": seconds,
        "mStartSeconds": int(time.time() if now is None else now),
        "mStarted": 0,
    }


def export_missions(player, now=None):
    """The ExportRewardsBag contents: top the board up to the warehouse's slot count."""
    now = time.time() if now is None else now
    bag = exports(player)
    bag["Day"] = utc_day(now)
    pool = export_commodity_pool(player)
    if not pool:
        return []
    wanted = export_slots(player)
    # Keep missions in progress; drop paid ones so a fresh offer takes their place.
    bag["Missions"] = [m for m in bag["Missions"] if m and not m.get("Paid")]
    used = {m.get("mCommodity1UID") for m in bag["Missions"]}
    index = len(bag["Missions"])
    while len(bag["Missions"]) < wanted:
        choices = [c for c in pool if c["uid"] not in used] or pool
        commodity = choices[index % len(choices)]
        mission = new_export_mission(player, commodity, now, index)
        bag["Missions"].append(mission)
        used.add(commodity["uid"])
        index += 1
    return bag["Missions"]


def _find_export(bag, uid):
    """Locate a mission by mission id, then by commodity id (which one the client sends
    is not pinned down; both are u64s in the request path)."""
    uid = str(uid)
    for mission in bag["Missions"]:
        if str(mission.get("ExportMissionUID")) == uid:
            return mission
    for mission in bag["Missions"]:
        if str(mission.get("mCommodity1UID")) == uid:
            return mission
    return None


def start_export(player, uid, now=None):
    """The client has begun an export mission. Returns the mission (or None)."""
    missions = export_missions(player, now)
    mission = _find_export(exports(player), uid)
    if mission is None and missions:
        mission = missions[0]
    if mission is not None:
        mission["mStarted"] = 1
        mission["mStartSeconds"] = int(time.time() if now is None else now)
    return mission


def end_export(player, uid, success=True, now=None):
    """Pay an export mission. Returns (mission, money, face); all zero when it cannot pay.

    A mission is paid exactly once. Its record leaves the board once it is paid, so a
    repeat of the same request arrives with a mission id that no longer exists - the paid
    id list is what stops that from paying whatever mission replaced it.
    """
    bag = exports(player)
    export_missions(player, now)          # the board must exist before it can be paid
    if str(uid) in [str(u) for u in bag.get("Paid") or []]:
        return None, 0, 0
    mission = _find_export(bag, uid)
    if mission is None:
        return None, 0, 0
    bag["Attempts"] = int(bag.get("Attempts", 0)) + 1
    if not success:
        mission["mStarted"] = 0
        return mission, 0, 0
    money = int(mission.get("mMoneyReward") or 0)
    face = int(mission.get("mFaceReward") or 0)
    if money:
        _credit(player["CurrencyBag"], "DirtyMoney", money)
    if face:
        pdb = player["PlayerDataBag"]
        pdb["Face"] = str(int(pdb.get("Face", "0")) + face)
    commodity = EXPORT_BY_UID.get(str(mission.get("mCommodity1UID")))
    if commodity is not None:
        left = warehouse_amount(player, commodity["uid"]) - int(mission.get("mCommodity1Amount") or 0)
        warehouse_set(player, commodity, max(0, left))
    bag["Successes"] = int(bag.get("Successes", 0)) + 1
    bag["Paid"] = ([str(uid)] + [str(u) for u in bag.get("Paid") or []])[:50]
    mission["Paid"] = 1
    mission["mStarted"] = 0
    return mission, money, face


def _credit(bag, key, amount):
    bag[key] = str(int(bag.get(key, "0")) + int(amount))


# How much of each commodity the warehouse holds, per warehouse level. The client's own
# message is "You don't have room in your warehouse for this commodity."; the numbers are
# UFG::MetagameInfoWrapper.mWarehouseStorageInfo (index = warehouse level - 1).
WAREHOUSE_STORAGE = {}
for _entry in METAGAME.get("mWarehouseStorageInfo") or []:
    _handle = (_entry.get("mWarehouseCommodityType") or {}).get("$handle")
    if _handle:
        WAREHOUSE_STORAGE[_handle] = [int(n) for n in _entry.get("mMaxStorage") or []]


def warehouse_level(player):
    warehouse = player["TurfBag"]["Turf"].get("Warehouse") or {}
    return max(1, int(warehouse.get("RacketLevel", 1) or 1))


def warehouse_capacity(player, commodity_uid):
    """Units of this commodity the warehouse can hold at its current level."""
    return int(_table_at(WAREHOUSE_STORAGE.get(str(commodity_uid)) or [], warehouse_level(player), 0))


def warehouse(player):
    """The TurfBag's Commodities array: what the warehouse holds right now."""
    return player["TurfBag"].setdefault("Commodities", [])


def warehouse_amount(player, commodity_uid):
    for record in warehouse(player):
        if str(record.get("CommodityUID") or record.get("mCommodityUID") or "") == str(commodity_uid):
            return int(record.get("Amount") or record.get("mAmount") or 0)
    return 0


def warehouse_set(player, commodity, amount):
    """Set the stored units of one commodity (0 removes the record)."""
    uid = str(commodity["uid"])
    records = warehouse(player)
    amount = max(0, int(amount))
    for record in records:
        if str(record.get("CommodityUID") or record.get("mCommodityUID") or "") == uid:
            record["Amount"] = amount
            record["mAmount"] = amount
            if amount == 0:
                records.remove(record)
            return
    if amount:
        # The client's own key for the count is mAmount; the uid/name spellings below are
        # the ones its other screens use. It ignores members it does not declare.
        records.append({"CommodityUID": uid, "mCommodityUID": uid,
                        "CommodityName": commodity.get("name", ""),
                        "Amount": amount, "mAmount": amount})


def commodity_of_racket(player, type_uid):
    """The commodity a built racket type produces, if any."""
    for commodity in EXPORT_COMMODITIES:
        if (commodity.get("racket") or {}).get("uid") == str(type_uid):
            return commodity
    return None


def settle_production(player, now=None):
    """Let idle rackets produce.

    A racket makes mOutput1Amount units every mProdTimeSeconds (both from its own type and
    level); CurrentProduction is what has piled up since the warehouse last collected it.
    Two cycles' worth is kept, so a player who leaves the game running does not come back
    to an unlimited pile.
    """
    now = time.time() if now is None else now
    if not FEATURE_PRODUCTION:
        return False
    changed = False
    for racket in player["TurfBag"].get("Rackets") or []:
        type_uid = racket.get("RacketTypeUID")
        level = int(racket.get("RacketLevel", 1) or 1)
        if racket.get("RacketState"):
            continue                          # busy building or upgrading
        seconds = racket_cycle_seconds(type_uid, level)
        per_cycle = int(_table_at(racket_outputs(type_uid), level, 0))
        if seconds <= 0 or per_cycle <= 0:
            continue
        if not racket.get("_prod_at"):
            racket["_prod_at"] = now          # start the clock the first time we look
            changed = True
            continue
        cycles = int((now - int(racket["_prod_at"])) // seconds)
        if cycles <= 0:
            continue
        current = int(racket.get("CurrentProduction") or 0)
        racket["CurrentProduction"] = min(per_cycle * 2, current + cycles * per_cycle)
        racket["_prod_at"] = int(racket["_prod_at"]) + cycles * seconds
        changed = True
    return changed


def move_to_warehouse(player, type_uid, amount=None, now=None):
    """Move a racket's production into the warehouse.

    Returns (moved, sold, earnings): whatever does not fit is sold on the black market at
    the commodity's price (its mFloorPrice/mCeilingPrice midpoint), which is what the
    client's mCommoditySold/mCommoditySoldEarnings report.
    """
    settle_production(player, now)
    racket = None
    for candidate in player["TurfBag"].get("Rackets") or []:
        if str(candidate.get("RacketTypeUID")) == str(type_uid):
            racket = candidate
            break
    if racket is None:
        return 0, 0, 0
    commodity = commodity_of_racket(player, type_uid)
    if commodity is None:
        return 0, 0, 0
    available = int(racket.get("CurrentProduction") or 0)
    wanted = available if not amount else min(int(amount), available)
    if wanted <= 0:
        return 0, 0, 0
    room = max(0, warehouse_capacity(player, commodity["uid"]) - warehouse_amount(player, commodity["uid"]))
    moved = min(wanted, room)
    sold = wanted - moved
    if moved:
        warehouse_set(player, commodity, warehouse_amount(player, commodity["uid"]) + moved)
        racket["CurrentProduction"] = available - moved
    earnings = 0
    if sold:
        earnings = int(round(sold * float(commodity.get("price") or 0)))
        _credit(player["CurrencyBag"], "DirtyMoney", earnings)
        racket["CurrentProduction"] = int(racket.get("CurrentProduction") or 0) - sold
    return moved, sold, earnings


def sync_export_slots(player):
    """The number of exports currently ready, for the export bag and for tooling.

    It is NOT written into the turf bag: earlier builds of this server added an
    ExportMissionSlots member to the turf/warehouse records, and a member the client's
    own class does not declare is exactly the kind of thing that can upset its parser.
    """
    return export_slots(player)


# ---- "Challenges" (community trials) -------------------------------------------
# UIScreenTrials pages through trial containers - the previous, current and next
# challenge. The client tracks challenge progress itself (the trial names are ordinary
# stat symbols) and reports it with trial/reportprogress/<value>; trialsscreenvisits
# asks for the same report. The server keeps the reported progress, pays the reward
# tier the first time it is reached, and records the opt-in.
TRIAL_OPT_IN, TRIAL_OPT_OUT = 1, 0


def new_trial_bag():
    return {"Index": 0, "Start": 0, "End": 0, "Progress": 0, "Rewarded": [], "OptedIn": 0,
            "Name": ""}


def trial_instance_id(bag, index=None):
    """A stable id for one rotation slot: unique per challenge and start time.

    trialinstance/optin/<this id>/<name> is how the client opts in, so the id in the
    container card has to be the same value.
    """
    index = bag["Index"] if index is None else index
    return str(int(bag["Start"]) * 1000 + int(index) % 1000 + 1)


def trials(player):
    bag = player.setdefault("TrialsBag", new_trial_bag())
    for key, value in new_trial_bag().items():
        bag.setdefault(key, value)
    return bag


def current_challenge(player, now=None):
    """The challenge running now; rotates through data/challenges.json."""
    now = time.time() if now is None else now
    bag = trials(player)
    if not bag["Start"] or now >= bag["End"]:
        index = 0 if not bag["Start"] else bag["Index"] + 1
        bag["Index"] = index % max(1, len(CHALLENGES))
        bag["Start"] = int(now)
        bag["End"] = int(now + CHALLENGE_ROTATION_SECONDS)
        bag["Progress"] = 0
        bag["Rewarded"] = []
        bag["OptedIn"] = TRIAL_OPT_OUT
    elif bag.get("Name"):
        # The client opted into a named container: follow it rather than the rotation.
        names = {c.get("name"): i for i, c in enumerate(CHALLENGES)}
        if bag["Name"] in names and names[bag["Name"]] != bag["Index"]:
            bag["Index"] = names[bag["Name"]]
            bag["Progress"] = 0          # progress and paid tiers belong to one container
            bag["Rewarded"] = []
    return bag, (CHALLENGES[bag["Index"]] if CHALLENGES else None)


def _rarity_range(rule):
    try:
        return RARITY_ORDER.index(rule.get("min") or "Common"), \
               RARITY_ORDER.index(rule.get("max") or "Epic")
    except (AttributeError, ValueError):
        return 0, len(RARITY_ORDER) - 1


def trial_tier_reward(player, tier):
    """Pay one challenge tier and describe it for the reply.

    The tier names a UFG::GameRewardTuner from data/challenges.json; its
    mGameRewardType decides what is handed over, and mRewardValues is indexed by the
    player's XP level.
    """
    tuner_uid = str(tier.get("tuner") or "")
    tuner = CHALLENGE_TUNERS.get(tuner_uid) or {}
    kind = tuner.get("mGameRewardType") or ""
    values = tuner.get("mRewardValues") or []
    level = xp_level(player)
    amount = 0
    favours = []
    paid = "None"
    if values:
        entry = values[min(max(level - 1, 0), len(values) - 1)] or {}
        amount = int(entry.get("mMinReward", 0))
    if kind == "RewardType_DirtyMoney":
        bag = player["CurrencyBag"]
        bag["DirtyMoney"] = str(int(bag.get("DirtyMoney", "0")) + amount)
        paid = "DirtyMoney"
    elif kind == "RewardType_CleanMoney":
        bag = player["CurrencyBag"]
        bag["CleanMoney"] = str(int(bag.get("CleanMoney", "0")) + amount)
        paid = "CleanMoney"
    elif kind == "RewardType_Face":
        pdb = player["PlayerDataBag"]
        pdb["Face"] = str(int(pdb.get("Face", "0")) + amount)
        paid = "Face"
    elif kind in ("RewardType_Favours_All", "RewardType_Favours_RaidBonus"):
        rules = tuner.get("mFavourTypes") or []
        rule = rules[min(max(level - 1, 0), len(rules) - 1)] if rules else {}
        low, high = _rarity_range(rule)
        tags = {t.get("$handle") for t in rule.get("favourTags") or [] if isinstance(t, dict)}
        pool = [uid for uid, info in FAVOUR_UPGRADES.items()
                if not info.get("ftpe") and info.get("rarity") in RARITY_ORDER
                and low <= RARITY_ORDER.index(info["rarity"]) <= high
                and (not tags or tags.intersection(info.get("tags") or []))]
        if not pool:
            pool = [uid for uid, info in FAVOUR_UPGRADES.items() if not info.get("ftpe")]
        if pool:
            favours = [random.choice(pool)]
            bag = player.setdefault("FavoursBag", [])
            for uid in favours:
                bag.append(favour_instance(next_favour_id(player), uid))
        paid = "Favour"
        amount = len(favours)          # mRewardValues is the tuner's favour budget, not a count
    return {"tuner": tuner_uid, "kind": kind or paid, "paid": paid,
            "amount": amount, "favours": favours, "target": int(tier.get("target", 0))}


def trial_snapshot(player, value=None, now=None):
    """Update (optionally) and describe the challenge report."""
    bag, challenge = current_challenge(player, now)
    if value is not None:
        bag["Progress"] = max(int(bag["Progress"]), max(0, int(value)))
    tiers = (challenge or {}).get("tiers") or []
    target = max([int(t.get("target", 0)) for t in tiers] or [0])
    progress = min(int(bag["Progress"]), target) if target else int(bag["Progress"])
    granted = []
    for index, tier in enumerate(tiers):
        if progress >= int(tier.get("target", 0)) and index not in bag["Rewarded"]:
            bag["Rewarded"].append(index)
            reward = trial_tier_reward(player, tier)
            reward["index"] = index
            granted.append(reward)
    reached = [i for i, t in enumerate(tiers) if progress >= int(t.get("target", 0))]
    return bag, challenge, {"progress": progress, "target": target, "reached": reached,
                            "opted_in": bool(bag["OptedIn"]), "granted": granted}


def trial_container(bag, challenge, start, end, index):
    """One UFG::TrialContainer-shaped record (the trials screen's card)."""
    return {"mTrial": challenge.get("name", ""),
            "mTrialInstance": trial_instance_id(bag, index),
            "mTrialStartSeconds": int(start), "mTrialEndSeconds": int(end),
            "mBonusFavour": "0",
            "mDisplayName": challenge.get("title", ""),
            "mDescription": challenge.get("description", ""),
            "mImage": challenge.get("image", "")}


def default_state():
    player = new_player(1001, "Player", 1, TURF_LOCATIONS[0], 0, account_id=1)
    grant_starting_favours(player)
    grant_starting_inventory(player)
    return {
        "version": 7,
        "created": int(time.time()),
        "player": player,
        "opponents": [
            {
                "type": 0,
                "remaining_seconds": 86400,
                "data": new_player(2001 + i, AI_NAMES[i], i, TURF_LOCATIONS[i], i),
            }
            for i in range(7)
        ],
        "gamestate": "",          # opaque save blob the client POSTs to playersavegame
        "gameplaystate": "",
    }


class State:
    def __init__(self, path=STATE_PATH):
        self.path = path
        self.lock = threading.RLock()
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                self.data = json.load(f)
            self._migrate()
        else:
            self.data = default_state()
            self.save()

    def _migrate(self):
        """Bring a profile saved by an older server version up to date."""
        version = self.data.get("version", 1)
        if version >= 6:
            return
        player = self.data["player"]
        if version < 2:
            for key in ("FavoursBag", "EnforcerUpgradesBag", "InventoryBag"):
                player.setdefault(key, [])
            grant_starting_favours(player)
        if version < 3:
            # Move the safehouse-family buildings off the block grid (see new_turf).
            for owner in [player] + [o["data"] for o in self.data.get("opponents", [])]:
                turf = owner.get("TurfBag", {}).get("Turf", {})
                for key in ("Safehouse", "Warehouse", "Temple"):
                    if key in turf:
                        turf[key]["BlockIndex"] = SAFEHOUSE_BLOCK
        if version < 4:
            # Empty-lot placeholders used to be stored as rackets; drop them (see new_turf).
            empties = set(EMPTY_UID.values())
            for owner in [player] + [o["data"] for o in self.data.get("opponents", [])]:
                bag = owner.get("TurfBag")
                if bag:
                    bag["Rackets"] = [r for r in bag.get("Rackets", [])
                                      if r.get("RacketTypeUID") not in empties]
        if version < 5:
            grant_starting_inventory(player)
        if version < 6:
            # Jobs and challenges keep their state on the player, not in a data bag.
            for owner in [player] + [o["data"] for o in self.data.get("opponents", [])]:
                owner.setdefault("ErrandsBag", new_errand_bag())
                owner.setdefault("TrialsBag", new_trial_bag())
                owner.setdefault("CurrencyBag", {}).setdefault("Gold", "0")
        if version < 7:
            # Export missions: same idea as the job board, kept off the data bags.
            for owner in [player] + [o["data"] for o in self.data.get("opponents", [])]:
                owner.setdefault("ExportBag", new_export_bag())
                sync_export_slots(owner)
        self.data["version"] = 7
        self.save()

    def save(self):
        with self.lock:
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=1)
            os.replace(tmp, self.path)

    # -- response builders ---------------------------------------------------
    @staticmethod
    def bags(player, mask):
        """Build an mDataBags object containing only the bags we actually hold."""
        out = {}
        have = 0
        # PlayerData/Customization/Currency/Turf are objects; Favours, Inventory and
        # EnforcerUpgrades are arrays of records (EnforcerUpgrades as {Item1: uid, Item2: slot}).
        for bit, key in ((BAG_PLAYER, "PlayerDataBag"), (BAG_CUSTOMIZATION, "CustomizationBag"),
                         (BAG_FAVOURS, "FavoursBag"), (BAG_CURRENCY, "CurrencyBag"),
                         (BAG_TURF, "TurfBag"), (BAG_INVENTORY, "InventoryBag"),
                         (BAG_ENFORCER_UPGRADES, "EnforcerUpgradesBag")):
            if mask & bit and key in player:
                out[key] = copy.deepcopy(player[key])
                have |= bit
        if "TurfBag" in out:
            out["TurfBag"] = _for_client(out["TurfBag"])
        # The export board is generated, not stored: the client's ExportRewardsBag is a
        # list of missions (see new_export_mission). It is only sent when the client asks
        # for that bag, because every reply that carries it is one the client parses
        # eagerly - a bag it does not expect is the one thing here that could upset it.
        # A bag the client asked for is always answered - an absent one is exactly the
        # shape that makes the parser read past the end. With FEATURE_EXPORTS off the
        # board is simply empty.
        if mask & BAG_EXPORT_REWARDS or (EXPORT_BAG_WITH_TURF and mask & BAG_TURF):
            missions = copy.deepcopy(export_missions(player)) if FEATURE_EXPORTS else []
            out["ExportRewardsBag"] = ({EXPORT_BAG_MEMBER: missions}
                                       if EXPORT_BAG_MEMBER else missions)
            have |= BAG_EXPORT_REWARDS
        out["mRequestedBags"] = have
        return out

    def player_bags(self, mask):
        with self.lock:
            return self.bags(self.data["player"], mask)

    def opponents(self, mask=0x1FFF):
        """Opponent summaries plus their data bags.

        The client reads these as two parallel root-level arrays: mOpponents[i]
        describes the rival and mOpponentDataBags[i] holds that rival's bags.
        """
        with self.lock:
            infos, bags = [], []
            for o in self.data["opponents"]:
                p = o["data"]
                infos.append({
                    "mPlayerDataID": p["PlayerDataBag"]["PlayerDataID"],
                    "mOpponentType": o.get("type", 0),
                    "mTriad": {},
                    "mRemainingSeconds": o.get("remaining_seconds", 86400),
                })
                bags.append(self.bags(p, mask))
            return infos, bags
