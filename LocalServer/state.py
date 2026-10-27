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
    for n in range(1, 11):
        r["Production%d" % n] = 0
    return r


SAFEHOUSE_BLOCK = -1      # BlockIndex of the safehouse / warehouse / temple (off the grid)


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


def new_customization_bag():
    """What the player is wearing (UFG::EnforcerCustomization). Zero means "as created".

    ScalingPresetCortexUID picks a body preset, ClothingOutfitCortexUID the outfit, and
    ClothingStoreItemUIDs the individual UFG::StoreClothing pieces drawn on top. The
    wardrobe screen rewrites all three through enforcer/customization; the model the
    game draws is these pieces, so they are the player model as far as the client cares.
    """
    return {
        "ScalingPresetCortexUID": "0",
        "ClothingOutfitCortexUID": "0",
        "ClothingStoreItemUIDs": [],
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
        "CustomizationBag": new_customization_bag(),
        # Job board and challenge progress; not client data bags, so they are not
        # serialised into mDataBags (see State.bags).
        "ErrandsBag": new_errand_bag(),
        "TrialsBag": new_trial_bag(),
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

# The safehouse has ten levels (mSafehouseUpgradeCosts), but four errands ask for a
# "safehouse 15" that never existed in the shipped game - they are unreachable as written,
# so the requirement is read as "the top level". They are ordinary raid/combat jobs.
ERRAND_SAFEHOUSE_CAP = len(METAGAME.get("mSafehouseUpgradeCosts") or []) or 10
# A finished repeatable job comes back after the game's own cooldown
# (UFG::MetagameInfoWrapper.mRepeatableErrandCooldownHours, 96 hours).
ERRAND_REPEATABLE_COOLDOWN = int(METAGAME.get("mRepeatableErrandCooldownHours") or 0) * 3600

# Stats this build cannot produce. The server does not run the export chain, so the two
# errands that ask for completed exports can never be finished - and 49 other jobs list
# "Complete 3 Exports" as a prerequisite, so that one hole used to hide most of the ladder.
# Those jobs are skipped as board entries and skipped as prerequisites. Everything else the
# client counts itself (kills, raids, rackets, crimes scenes, operations, purchases).
ERRAND_UNSUPPORTED_STATS = {"ExportsCompleted"}
ERRAND_UNSUPPORTED = {e["uid"] for e in ERRANDS
                      if any((c or {}).get("stat") in ERRAND_UNSUPPORTED_STATS
                             for c in e.get("conditions") or [])}

# 29 errands are switched off in the game data (mEnabled = false): the combat-tutorial
# variants ("Sweep Kicks", "Roundhouse Kicks", ...), the commodity collectors and a few
# others. Nothing about them is broken - the client still counts their objectives - they
# were simply never handed out in the beta. Once every enabled job has been done, the
# board falls back to them instead of going empty, which is how the ladder keeps going
# after the story runs out. Set to False to keep the board to the game's own 87.
ERRAND_BONUS_JOBS = True

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


def utc_day(now=None):
    return time.strftime("%Y-%m-%d", time.gmtime(time.time() if now is None else now))


def seconds_to_refresh(now=None):
    """Seconds until the job board rolls over (next UTC midnight)."""
    t = time.gmtime(time.time() if now is None else now)
    return max(1, 86400 - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec))


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


def _errand_record(bag, uid):
    """(times done, when it was last paid) for one errand - both formats the bag used."""
    record = bag["Completed"].get(uid)
    if record is None:
        return 0, 0
    if isinstance(record, dict):
        return int(record.get("Times") or 0), int(record.get("Last") or 0)
    return int(record or 0), 0          # an early profile kept just a count


def errand_requirement(errand, field):
    """The level/safehouse an errand asks for, with the impossible 15 read as the top."""
    value = int(errand.get(field) or 0)
    if field == "safehouseLevel":
        return min(value, ERRAND_SAFEHOUSE_CAP)
    return value


def errand_available(bag, errand, level, safehouse, now=None, bonus=False):
    """Can this errand be put on the board?

    `bonus` allows the errands the game data switches off (see ERRAND_BONUS_JOBS); those
    also ignore their mErrandLinks, which point at other switched-off jobs.
    """
    enabled = bool(errand.get("enabled"))
    if errand["uid"] in ERRAND_UNSUPPORTED:
        return False
    if not enabled and not (bonus and ERRAND_BONUS_JOBS):
        return False
    if errand_requirement(errand, "xpLevel") > level:
        return False
    if errand_requirement(errand, "safehouseLevel") > safehouse:
        return False
    # mErrandLinks are the errands that must have been completed first ("Raid 2 Rivals"
    # links to "Raid A Rival"). With one link only the tutorial's first job is on offer.
    if enabled and any(link not in bag["Completed"] and link not in ERRAND_UNSUPPORTED
                       for link in errand.get("links") or []):
        return False
    times, last = _errand_record(bag, errand["uid"])
    if not times:
        return True
    if not errand.get("repeatable"):
        return False
    now = time.time() if now is None else now
    if ERRAND_REPEATABLE_COOLDOWN and now - last < ERRAND_REPEATABLE_COOLDOWN:
        return False
    return True


def errand_pool(player, bag, exclude=(), now=None, bonus=True):
    """The errands the board can offer right now.

    The game's own (enabled) jobs come first. If none of them is available - the story
    ladder is finished, or every open job is on cooldown - the switched-off bonus jobs
    fill in, so the Jobs screen never runs dry.
    """
    level, safehouse = xp_level(player), safehouse_level(player)
    pool = [e for e in ERRANDS
            if e["uid"] not in exclude and e["enabled"]
            and errand_available(bag, e, level, safehouse, now)]
    if not pool and bonus:
        pool = [e for e in ERRANDS
                if e["uid"] not in exclude and not e["enabled"]
                and errand_available(bag, e, level, safehouse, now, bonus=True)]
    pool.sort(key=lambda e: (-e["priority"], e["name"]))
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
        pool = errand_pool(player, bag, exclude=slotted, now=now)
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
    pool = errand_pool(player, bag, exclude=slotted, now=now)
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


# ---- World activities (operations) ----------------------------------------------
# The client runs the open-world activities itself and reports each one when it ends:
# operation/endoperation/<name>/<completed>/<successful>. The names are the game's own
# UFG::OperationType objects - Catch, Defeat, Photo, Deliver, Collect, Destroy, Hijack -
# so nothing here is invented; only the payout is the server's, and it uses the game's
# face/dirty-money ladders at the player's XP level (UFG::ErrandRewards).
OPERATION_TYPES = {o["$uid"]: o.get("mOperationTypeName", "")
                   for o in DB.get("UFG::OperationType") or []}
OPERATION_REWARDS = {
    "Catch": "REWARD_FACE_LOW",        # bring someone in alive
    "Defeat": "REWARD_MONEY_MID",      # clear a group of thugs
    "Photo": "REWARD_FACE_MID",        # photograph the scene
    "Deliver": "REWARD_MONEY_LOW",     # drive a package across town
    "Collect": "REWARD_MONEY_LOW",     # pick up a drop
    "Destroy": "REWARD_MONEY_MID",     # wreck the marked vehicles
    "Hijack": "REWARD_MONEY_MID",      # steal a specific vehicle
}


def operation_name(value):
    """'Catch' / 'catch' / the type's own UID -> the operation's name."""
    text = str(value or "").strip()
    if text in OPERATION_TYPES:
        return OPERATION_TYPES[text]
    for name in OPERATION_REWARDS:
        if text.lower() == name.lower():
            return name
    return text


def pay_operation_reward(player, name, successful=True):
    """Credit one finished activity and return what was paid (for the reply)."""
    name = operation_name(name)
    reward = OPERATION_REWARDS.get(name, "REWARD_MONEY_LOW")
    kind, table = ERRAND_REWARD_TABLES.get(reward, ("DirtyMoney", "mRewardDirtyMoneyLow"))
    amount = _table_amount(table, xp_level(player)) if successful else 0
    if amount:
        if kind == "Face":
            pdb = player["PlayerDataBag"]
            pdb["Face"] = str(int(pdb.get("Face", "0")) + amount)
        else:
            bag = player["CurrencyBag"]
            bag[kind] = str(int(bag.get(kind, "0")) + amount)
    return {"operation": name, "kind": kind, "amount": amount, "successful": bool(successful),
            "reward_type": JOB_REWARD_TYPES.get(kind, JOB_REWARD_CASH)}


# ---- Crime scenes -----------------------------------------------------------------
# Corpses and loot the client leaves in the world after a fight. The server never invents a
# position: it records the scene the client reports (openworldcrimescene/<x>/<y>/<z>/<id>)
# and hands the same coordinates back in the CrimeScenes list, so a player's own fights
# become collectible crime scenes. Collecting one pays face
# (openworldcrimescenecollection/<scene id> -> FaceCollected).
CRIME_SCENE_FACE = 10
CRIME_SCENE_LIMIT = 24          # markers kept at once; the oldest drops off
CRIME_SCENE_TTL = 60 * 60       # an hour, then the scene is gone
CRIME_SCENE_UID_BASE = 7000000000000000000


def crime_scene_bag(player):
    bag = player.setdefault("CrimeSceneBag", {})
    bag.setdefault("Scenes", [])
    bag.setdefault("NextUID", 0)
    return bag


def prune_crime_scenes(player, now=None):
    """Forget scenes older than CRIME_SCENE_TTL. Returns True if anything went."""
    now = time.time() if now is None else now
    bag = crime_scene_bag(player)
    keep = [s for s in bag["Scenes"] if int(s.get("Created") or 0) + CRIME_SCENE_TTL > now]
    if len(keep) == len(bag["Scenes"]):
        return False
    bag["Scenes"] = keep
    return True


def add_crime_scene(player, x, y, z, victim="", now=None):
    """Record a scene the client reported. Returns the record, or None if it already had it."""
    now = time.time() if now is None else now
    bag = crime_scene_bag(player)
    prune_crime_scenes(player, now)
    for scene in bag["Scenes"]:
        # The client reports the same corpse again as it stays in the world; one marker is
        # enough. 4 units is about a body's width on the map.
        if abs(int(scene["DeathX"]) - int(x)) <= 4 and abs(int(scene["DeathY"]) - int(y)) <= 4:
            return None
    bag["NextUID"] = int(bag.get("NextUID") or 0) + 1
    scene = {"OpenWorldCrimeSceneID": str(CRIME_SCENE_UID_BASE + bag["NextUID"]),
             "DeathX": int(x), "DeathY": int(y), "DeathZ": int(z),
             "VictimPlayerDataIDString": str(victim or "0"),
             "Collected": False, "Created": int(now)}
    bag["Scenes"].append(scene)
    del bag["Scenes"][:-CRIME_SCENE_LIMIT]
    return scene


def crime_scenes(player, now=None):
    """The scenes the client should draw: everything not collected yet."""
    prune_crime_scenes(player, now)
    return [dict(s) for s in crime_scene_bag(player)["Scenes"] if not s.get("Collected")]


def collect_crime_scene(player, scene_id, now=None):
    """Collect one scene and pay its face. Returns (scene, face); (None, 0) if not found."""
    prune_crime_scenes(player, now)
    for scene in crime_scene_bag(player)["Scenes"]:
        if str(scene.get("OpenWorldCrimeSceneID")) == str(scene_id) and not scene.get("Collected"):
            scene["Collected"] = True
            scene["CollectedAt"] = int(time.time() if now is None else now)
            pdb = player["PlayerDataBag"]
            pdb["Face"] = str(int(pdb.get("Face", "0")) + CRIME_SCENE_FACE)
            return scene, CRIME_SCENE_FACE
    return None, 0


# ---- World encounters --------------------------------------------------------------
# The street events the client's own manager runs (Skookum TWWorldEncounterManager spawns
# the units; the server only hears about them when one ends, at
# worldencounter/endencounter/<type>/<result>). The list endpoint stays empty on purpose:
# the encounters are the client's, and handing it entries it did not spawn itself is the
# one way to get an invisible or mismatched one. Paying for the ones it does run turns the
# free-roam events into something worth stopping for - the game's own face ladder, capped
# so the street cannot out-earn the Jobs screen.
WORLD_ENCOUNTER_REWARD = "REWARD_FACE_LOW"
WORLD_ENCOUNTER_LIMIT = 6                   # payouts ...
WORLD_ENCOUNTER_WINDOW = 60 * 60            # ... per hour of play


def world_encounter_bag(player):
    bag = player.setdefault("WorldEncounterBag", {})
    bag.setdefault("Paid", [])
    return bag


def pay_world_encounter_reward(player, result=1, now=None):
    """Credit one finished street encounter; returns what was paid (for the reply)."""
    now = time.time() if now is None else now
    kind, table = ERRAND_REWARD_TABLES.get(WORLD_ENCOUNTER_REWARD, ("Face", "mRewardFaceLow"))
    amount = _table_amount(table, xp_level(player)) if str(result) != "0" else 0
    bag = world_encounter_bag(player)
    recent = [t for t in bag["Paid"] if now - int(t) < WORLD_ENCOUNTER_WINDOW]
    bag["Paid"] = recent
    if amount and len(recent) >= WORLD_ENCOUNTER_LIMIT:
        amount = 0                  # enough for now; the street keeps its own pace
    if amount:
        bag["Paid"].append(int(now))
        pdb = player["PlayerDataBag"]
        pdb["Face"] = str(int(pdb.get("Face", "0")) + amount)
    return {"kind": kind, "amount": amount,
            "reward_type": JOB_REWARD_TYPES.get(kind, JOB_REWARD_FACE)}


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
        if version >= 7:
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
            # The customization bag is a data bag now, so it is always present and the
            # client never reads a missing one (it is sent only when asked for).
            for owner in [player] + [o["data"] for o in self.data.get("opponents", [])]:
                owner.setdefault("CustomizationBag", new_customization_bag())
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
