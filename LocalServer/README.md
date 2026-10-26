# Triad Wars local server

A stand-in for the Triad Wars online services so the 1.6.0.0 client
(`TriadWars.exe`) can run on one machine with no network backend.

## Running

Double-click `Play-TriadWars.bat` in the game folder. It starts this server
if it is not already running, then starts the game.

To run the pieces by hand:

    cd LocalServer
    python server.py            # listens on https://127.0.0.1:443
    ..\TriadWars.exe

Requirements: Python 3.9 or newer on `PATH` (standard library only), and
port 443 free.

## What was changed in the game folder

| File | Change |
| --- | --- |
| `UserOptions.dat` | `Environment` switched from `prod.services.triadwars.com` to `localhost`. The original is kept as `UserOptions.dat.orig`. |
| `Play-TriadWars.bat` | New launcher. |
| `LocalServer\` | New folder (this server). |

`TriadWars.exe` itself is not modified, unless the optional crash patch below
has been applied. To undo everything:
`python LocalServer\useroptions.py restore`, then delete `LocalServer` and
`Play-TriadWars.bat`.

### Optional: shadow-crash patch for `TriadWars.exe`

About one launch in seven the client crashes a few seconds after the world
loads (access violation at `TriadWars.exe+0x548a`; Windows reports it as
offset `0x1b81b7`). It is a race inside the client, not something a server
reply causes: the spot-shadow pass draws a character whose model has not
finished loading. `tools\patch_exe.py` adds the missing null check:

    python LocalServer\tools\patch_exe.py status
    python LocalServer\tools\patch_exe.py apply      # keeps TriadWars.exe.orig
    python LocalServer\tools\patch_exe.py restore    # game must be closed

It changes 30 bytes, refuses to touch any executable other than the original
1.6.0.0 build, and can be applied while the game is running (it takes effect
on the next launch).

## Files

| File | Purpose |
| --- | --- |
| `server.py` | HTTPS + WebSocket plumbing, request log, routing. |
| `handlers.py` | One function per API endpoint. Edits are picked up without restarting. |
| `state.py` | Player/world model, the job board and challenge progress, and the JSON encodings the client expects. |
| `schema.py` | Reads the game's own `Reflection.rdb` (next to `TriadWars.exe`) and shapes replies to the type the client declares. Optional; without the file every helper is a no-op. |
| `state.json` | Your saved profile (created on first run). Delete it to start over. |
| `data/gamedb_min.json` | Object UIDs and tuning tables extracted from the game's own data. |
| `data/store_items.json` | What the clothing, outfit, favour-pack and pack-bundle stores sell (see `tools/store_items.py`). |
| `data/errands.json` | Every job (`UFG::ErrandTypeMetadata`) with its reward, requirements, prerequisites and completion conditions, plus the `UFG::ErrandRewards` tables (see `tools/errands.py`). |
| `data/challenges.json` | The rotating challenges (`JiangShiTrial`, `HappyCatTrial`, `EliteGhoulTrial`), their progress tiers and the `UFG::GameRewardTuner` each tier pays (see `tools/challenges.py`). |
| `data/exports.json` | The export catalogue: the eight physical commodities with their prices and per-level export amounts, the racket that produces each one, the face/markup tables and the three mission kinds (see `tools/exports.py`). |
| `data/favour_upgrades.json` | Every favour that grants an enforcer upgrade, with the rarity/tags/level job and challenge rewards filter on (see `tools/favour_rewards.py`). |
| `useroptions.py` | Reads/edits the obfuscated `UserOptions.dat`. |
| `cert.pem`, `key.pem` | Self-signed certificate for `localhost` (the client does not verify it). |
| `logs/` | One request log per server run. Lines marked `UNHANDLED` are endpoints with no handler yet. |
| `tools/` | The scripts used to unpack the game data and decode its schema, crash-analysis helpers, `smoke_test.py`, `test_jobs.py`, `test_exports.py`, `storyline.py`, `schema_dump.py`, `patch_exe.py` (see above), `make_bundle.py` and the data builders (`errands.py`, `challenges.py`, `exports.py`, `favour_rewards.py`). |
| `devdata/` | Reference data for extending the server: the full object-database export and the client's endpoint and key lists. Not needed to play. |

## How it works

The client builds every service URL as
`https://<Environment>/<service>/api/v1/<method>`, where `<Environment>` comes
from `UserOptions.dat` (XOR-obfuscated with a fixed 33-byte key). It logs in by
POSTing `{ "username", "password" }` to `login/api/v1/acceptcredentials` and
keeps the `connect.sid` cookie; with no credentials on the command line it uses
`guest` / `nopassword`. It disables TLS certificate checks, so a self-signed
certificate is accepted.

Player data travels as "data bags", gated by the `mRequestedBags` bitmask.
Where the bag object sits depends on the endpoint:

* `playerdata/serializedplayerdata/...` replies with the bag object as the
  **root** of the response (no wrapper).
* Action endpoints (name change, turf selection, raids, turf management, ...)
  nest it under `mDataBags`, next to `mReturnCode` / `mMessage`.
* `playerdata/opponents` carries one bag object per rival in the root-level
  array `mOpponentDataBags`.

Getting this wrong fails silently: the client builds a blank player record
(no name, no turf) and carries on until something needs the player's turf.
The first-raid results screen is one such place; it stays empty and its
Continue prompt does nothing.

The bags:

| Bit | Bag | Served |
| --- | --- | --- |
| 0x0001 | PlayerDataBag | yes |
| 0x0002 | CustomizationBag | yes, once the character has been customised |
| 0x0004 | InformantsBag | no |
| 0x0008 | FavoursBag | yes (array of favour instances) |
| 0x0010 | ActiveFavoursBag | no |
| 0x0040 | CurrencyBag | yes |
| 0x0080 | TurfBag | yes |
| 0x0100 | InventoryBag | yes (array of owned store items) |
| 0x0200 | ShieldBag | no |
| 0x0400 | TrialsBag | no |
| 0x0800 | ExportRewardsBag | no |
| 0x1000 | EnforcerUpgradesBag | yes (array of `{Item1: upgrade uid, Item2: slot}`) |

The reply only sets the bits for bags it actually contains, which the client
accepts.

Encoding rules that matter (getting these wrong reads as zero, or crashes):

* JSON keys are the reflected field names from `Reflection.rdb`; unknown keys are ignored.
* 32-bit integers, floats and booleans are plain JSON values.
* Every 64-bit value (ids, object UIDs, handles, face, money) is a decimal **string**.
* Symbols such as `LocationName` are strings; the client hashes them itself.
* A top-level reply must be an object unless the endpoint is known to take an array.
* `mReturnCode` 0 means success.

### Turf replies

Every `turf/*` endpoint shares one client callback and one reply shape:
`mReturnCode`, `mMessage`, `mActionType`, `mRacket` (the racket's fields),
`mRacketTypeUID` at the **root** (read only when `mRacket` is present),
timers, and `mDataBags`. Two details matter:

* If the root `mRacketTypeUID` is missing, the result is tagged as racket 0
  and the panel that asked stays on its loading spinner.
* `mActionType` decides what the panel does next and whether a tutorial step
  advances. Values confirmed from the client's result listener: `1` move to
  warehouse (shows the "units moved/sold" message), `4` remove racket (shows
  "racket destroyed"), `8` racket info, `9` safehouse info (the only value
  that advances the "select your safehouse" tutorial step). Inferred from how
  the listener groups the remaining values, not confirmed: `2` start
  construction, `3` upgrade racket, `5`/`6`/`7` upgrade safehouse / warehouse
  / temple, `10` warehouse info, `11` temple info.

The safehouse, warehouse and temple must carry `BlockIndex` -1. They are not
on the block grid, and any grid index places their mouse-pick boxes over that
grid lot, so they cannot be selected.

### Turf sites

The world has seven turf sites, `Turf-Turf00001` to `Turf-Turf00007`. The
client looks up the owner of site *n* as `mOpponents[n-1]`, so the server
always supplies seven rivals, with their bags in the parallel root-level array
`mOpponentDataBags`.

## Jobs and challenges

The two hub screens the original server never answered are driven by data that
ships with the game, so they can be emulated without guessing at content.

### Jobs (`game/errands`)

`UFG::ErrandTypeMetadata` (116 objects) is the job list, and `tools/errands.py`
exports it to `data/errands.json` together with the `UFG::ErrandRewards`
tables. Each job carries:

* `mPriority`, `mDisplayName`, `mDescription`, `mDisplayIcon`, `mRepeatable`;
* `mXPLevel` and `mSafeHouseLevel` (the level gates) and `mIsEnabled`;
* `mErrandLinks`, the jobs that must be completed **before** it (e.g. "Raid 2
  Rivals" links to "Raid A Rival"); 110 of the 116 have at least one;
* `mCompletionConditions`, the client's own stat counters (`RaidsCompleted`,
  `EnforcerKilled`, `NoAlarms`, ...). 83 distinct stats, resolved from their
  32-bit qSymbol hashes with `tools/symhash.py`;
* `mRewardType` and the favour rarity/tag rule for favour rewards.

That means **the client evaluates the objective**: it knows the player's stats
and only calls `game/errands/end/<slot>` when a job is done. The server owns the
board and the payout, not the progress:

1. `game/errands` returns up to three slots, one job each, picked from the jobs
   whose gates are met and whose prerequisite jobs are all completed. A job the
   player has never done comes before a repeat, then higher `mPriority` first.
   A brand-new profile has exactly one eligible job, "Raid A Rival" - the first
   step of the tutorial ladder the data encodes. Prerequisites the game itself
   disables are ignored (two enabled jobs hang off the disabled "Finish a raid"),
   and the four jobs that ask for safehouse 15 are treated as safehouse 10,
   because the turf cannot be upgraded past 10 - otherwise they could never appear.
2. `game/errands/end/<slot>` marks that job completed.
3. `game/errands/claimreward/<slot>` pays it and returns the bags:
   `UFG::ErrandRewards` is indexed by XP level (level 1 = index 0), so a level 1
   "Raid A Rival" pays the tutorial's 129 gold, a `REWARD_FACE_MID` job at level
   10 pays 850 face, and so on. The "clean money" reward types are the **gold**
   ones: that is the only currency ladder the errand tables contain. Gold is
   carried in `CurrencyBag.Gold`, a wallet the original server never sent.
4. `game/errands/change/<slot>` rerolls a slot ("Change Job"). Once a paid slot is
   read again it refills with the next available job - that is the hub's "Next Job"
   notification - so the chain keeps moving inside one session. `state.py`'s
   `ERRAND_REFILL_COLLECTED = False` makes the three slots fixed for the day instead.
   The board rolls over at the hour the game data names
   (`mErrandRefreshHourUTC`, **10:00 UTC** - not midnight), and `mSecondsToRefresh`
   says how long is left. A repeatable job stays off the board for
   `mRepeatableErrandCooldownHours` (96 h) after it paid out; with only a few jobs
   unlocked the cooldown is dropped rather than handing back an empty screen.

Favour rewards are picked from `data/favour_upgrades.json` (every favour that
grants an enforcer upgrade) by the rarity range and tag list on the errand, and
are granted to the FavoursBag, so a job reward is always a card the player can
equip.

A slot record is a `GameService ErrandSlotData`:

```json
{"mSlotIndex": 0,
 "mErrand": {"mState": 0, "mTimeBegun": "2026-10-26 21:25:40",
             "mErrandUID": "6002850353329125207",
             "mFavourCortexUIDs": [], "mFavourPackCortexUID": "0"},
 "mErrandUID": "6002850353329125207", "mState": 0,
 "mTimeBegun": "2026-10-26 21:25:40",
 "mFavourCortexUIDs": [], "mFavourPackCortexUID": "0"}
```

`mState` is 0 active / 1 completed / 2 collected. The client's own parsers do
not pin down whether it reads the state inside `mErrand` or at the root (and it
ignores members it does not declare), so both are filled in. `mRewardType` in
the claim reply is the job reward card - 0 gold, 1 cash, 2 face, 3 favour.

### Challenges (`game/trialsscreenvisits`, `game/trial/reportprogress`)

`UIScreenTrials` is a community trial: three containers (previous, current,
next), an opt-in, a progress counter and tier rewards. The trial names are
client-side (`JiangShiTrial`, `HappyCatTrial`, `EliteGhoulTrial`, ...) and the
client tracks its own progress - it reports it to
`game/trial/reportprogress/<value>`, and `trialsscreenvisits` asks for the same
report. `data/challenges.json` (built by `tools/challenges.py`) therefore holds
only what the server owns:

* which three challenges rotate and for how long (`rotationDays`), and
* what each progress tier pays, as a real `UFG::GameRewardTuner` object copied
  from the game data (dirty money, clean money, face or a favour).

The report carries `mPlayerProgress` (trial name, start/end seconds,
progress/target, the three containers), `mRewardList` (the tiers reached, each
with its tuner) and `mGameRewardTunerName`/`mReward` for the top tier. Tiers are
paid once, the first time the reported progress reaches them, into the bags. The
opt-in (`trialinstance/optin/<id>/<name>`) answers with `"<name>": true`, which
is how the client keys the result.

Unlike the errand data, the trial containers are not in the object database this
server can export: the challenge rotation, tier targets and reward choice above
are this server's own event design, built to fit the client's screen. They are
plain JSON - edit them freely.

### Is there more than the tutorials?

Yes - the four tutorial jobs are the first rung of a 116 job ladder (87 of them
enabled). `python tools/storyline.py` prints it safehouse level by safehouse
level; `python tools/storyline.py --check` reports anything unreachable. The
ladder is the game's own progression:

* the tutorial chain (`Raid A Rival` -> `Raid 2 Rivals` -> `Rank Up to "Blue
  Lantern"` -> `Upgrade Safehouse II` -> `Level Up`), then
* the upgrade chain (`Safehouse III..VIII`, `Warehouse II..V`, `Racket III..IX`),
  which is what raises the gates on everything else, then
* the export chain (`Complete an Export` -> `Complete 3 Exports`), which alone
  opens **73** of the jobs, and then the raid, combat, shop, favour and faction
  jobs that fill the rest of the list.

Simulated end to end (every safehouse level, collecting every job the board
offers) **83 of the 87** enabled jobs come up; the four that do not need an XP
level the simulation never reaches (3, 10, 12) and are gated on that alone. All
87 are reachable in play.

### Exports (`export/startmission`, `export/endmission`, `turf/movetowarehouse`)

The warehouse side of the turf is an export chain: a racket produces a
commodity, the warehouse stores it, and an export mission drives a load of it out
of town for dirty money and face. Every number is the game's own
(`data/exports.json`, built by `tools/exports.py`):

| Data | Where it comes from |
| --- | --- |
| which commodity a racket makes | `UFG::RacketType.mRacketOutput` |
| units in a load, per XP level | `UFG::CommodityType.mExportAmounts` |
| units the warehouse holds, per level | `MetagameInfoWrapper.mWarehouseStorageInfo` |
| dirty money paid | units x the commodity's price x `mBlackMarketSellMarkupForMoneyReward[level]` |
| face paid | money x `mFaceRewardPercentageOfMoneyReward[level]` / 100 |
| how many missions held at once | the warehouse's level (the client's "upgrading your warehouse will increase your export targets") |

The eight physical commodities and the racket that fills them:

| Commodity | Racket | Price | Load at level 1 | Warehouse at level 1 |
| --- | --- | --- | --- | --- |
| Rebuilt Cars | Auto Shop | 1800 | 5 | 5 |
| Hot Cars | Repo | 1105 | 3 | 3 |
| Apothecary Goods | Exotic Animal Goods | 777.50 | 3 | 3 |
| Stolen Designs | Hackers | 250 | 15 | 15 |
| Knock-Off Electronics | Electronics | 150 | 25 | 20 |
| Seafood | Fish Market | 125 | 40 | 30 |
| Car Parts | Chop Shop | 31.50 | - (level 3) | 175 |
| Knock-Off Fashion | Sweat Shop | 25 | 200 | 180 |

Only loads that fit the warehouse are offered, which at level 1 means the first
four - the same rule the client's "You don't have room in your warehouse for this
commodity" message comes from.

How it runs:

* **Production.** An idle racket makes `mOutput1Amount` units every
  `mProdTimeSeconds` (both from its own type and level). `CurrentProduction` is
  what has piled up; two cycles' worth is the cap. `Production1..10` on the racket
  record is that per-level table, so the turf screen can show it.
* **Collecting.** `turf/movetowarehouse/<racket type uid>/<units>` moves units
  into the warehouse and answers with `mCommodityMoved`, `mCommoditySold` and
  `mCommoditySoldEarnings`; whatever does not fit the warehouse is sold on the
  black market at the commodity's price. `<units>` of 0 means "all of it".
* **The board.** The missions travel in the player's `ExportRewardsBag`
  (`playerdata/serializedplayerdata/2048`, and it rides along with the turf bags
  too, since which mask the warehouse screen asks for is not pinned down). Each
  new mission puts its load in the warehouse - that is the "N exports currently
  ready" the hub reports - and the list is topped back up to the warehouse's slot
  count as missions are paid.
* **Running one.** `export/startmission/<mission uid>` marks it started and
  `export/endmission/<mission uid>/<result>` pays it: 0 means the run failed and
  nothing is paid (the load stays in the warehouse, so it can be retried), any
  other value pays `mMoneyReward` / `mFaceReward` and empties the load.

A mission record looks like this (the `mCommodity1UID`/`mCommodity1Amount` names
are the client's own, from its `OperationManager::PopulateFromExportRewardsBag`
strings):

```json
{"ExportMissionUID": "900000000000000001",
 "mCommodity1UID": "12899405313262884080", "mCommodity1Amount": 5,
 "mCommodity2UID": "0", "mCommodity2Amount": 0,
 "mCommodityName": "COMMODITY_REBUILT_CARS", "mCommodityTitle": "Rebuilt Cars",
 "mCommodityIconName": "RebuiltCars",
 "mMoneyReward": 13500, "mFaceReward": 405, "mLevel": 1,
 "mOperationType": "ExportGunner", "mRacketTypeUID": "<auto shop>",
 "mDurationSeconds": 360, "mStartSeconds": 1793050000, "mStarted": 0}
```

A load may be paid only once: paid mission ids are remembered, so a repeated
`endmission` cannot pay whichever mission replaced it.

Two things about the export data worth knowing:

* `UFG::CommodityType.mDisplayName` is the client's localisation key
  (`COMMODITY_KNOCK_OFF_FASHION`); `mCommodityTitle` is a plain-English version
  for anything that wants it.
* The object database has no export mission list and no operation instances - the
  client builds missions from the bag and its own tuning - so the mission *record*
  is our shape, built from the game's numbers. Everything in the table above is
  exact.

### Trying it without the game

    python tools/test_jobs.py      # handlers directly: board, claim, history, change,
                                   # rollover, trial report/opt-in/tiers, restart
    python tools/test_exports.py   # production, warehouse, export missions, and a walk
                                   # through the whole 87 job ladder
    python tools/storyline.py      # print the job ladder; --check for unreachable jobs

Both test scripts build a throw-away profile in a temp folder, so they never touch
`state.json`. `tools/smoke_test.py` runs the same screens over HTTPS against a running
server (start it with `--port 8443` and pass `--port 8443`); it now covers the jobs board,
the completed list, the challenge report and the export board.

### What is a guess, and how to check it

Everything content-side comes from the game's own data. The wire shapes come from the
client's own parser strings (`devdata/endpoints.txt`, `devdata/keys_all.txt`), but a few
values are inferred, and these are the ones to look at if a screen misbehaves:

| Guess | Where it comes from | If it is wrong |
| --- | --- | --- |
| `mState` 0/1/2 = active/completed/collected | the hub's `JOB_ACTIVE`, `JOB_COMPLETED`, `JOB_COLLECTED` strings, in that order | cards show the wrong badge; the number is in `state.ERRAND_ACTIVE` etc. |
| `mErrand` is an object of the errand's own fields, with the definition UID at the root | the turf replies' `mRacket` + `mRacketTypeUID` pair, which the client needs the same way | an empty Jobs list; try `mErrand` as the UID string instead |
| `mRewardType` 0/1/2/3 = gold/cash/face/favour | the `JOB_REWARD_GOLD`..`JOB_REWARD_FAVOUR` strings | only the reward card's art is wrong |
| `mServerUTC`, `mTimeBegun`, `mLastUpdated` are `"%Y-%m-%d %H:%M:%S"` UTC strings | that is the client's own date format string (`019cf4570`) and its `getDateTimeFromUTC` | timers show nonsense; `handlers._utc_string` is the one place to change |
| `mGameRewardTunerName` is the tuner's UID as a string | the field is a string and `UFG::GameRewardTuner` has no name field | the Challenges screen may show no reward name |
| `ExportRewardsBag` is a bare array of missions | the other list bags (`FavoursBag`, `InventoryBag`) are arrays, and the client's symbol table has `ExportMissionSlots` next to the mission members | the export screen stays empty: set `state.EXPORT_BAG_MEMBER = "ExportMissionSlots"` and the same records go under that member instead |
| `export/endmission/<uid>` sends the *mission* id (a commodity id is accepted too) | both are u64s in the request path and the reply has no id of its own | the payout falls to 0; `state._find_export` is where the lookup is |
| warehouse records use `CommodityUID` + `mAmount` | `mAmount` is the client's own key; the uid spelling is shared with the black market | the warehouse shows 0 of everything; `state.warehouse_set` is the one place |
| `turf/movetowarehouse/<uid>/<n>` counts units, 0 = everything | the request has no callback, so the client does not read the reply for meaning | collection moves the wrong amount; clamp logic is in `state.move_to_warehouse` |

The challenge rotation (which three trials, tier targets, which tuner each tier pays) is
this server's own design - the object database has no trial objects at all, and the
client's trial names are hard-coded strings. Edit `data/challenges.json` freely.

### Confirming a reply against the client's own schema

`Reflection.rdb` in the game folder is the type database the client deserialises
replies with. `schema.py` reads it when it is present and:

* drops members our hand-built reply has that the real class does not declare
  (and logs `schema: <class> has no member <name> (dropped)` - a line in that
  log is the definitive answer to "is this field right?"),
* converts 64-bit members to decimal strings and ints to numbers,
* answers "which class holds an array of these, and what is the member called?"

`python tools/schema_dump.py errands` prints the types and fields for the job
and challenge replies; `... show "UFG::TrialContainer"`, `... list`, and
`... find "GameService ErrandSlotData"` are the rest of the interface. Without
the file the server answers exactly as before and the tool says so.

## What is emulated

Working, and exercised in play (intro, tutorials, first open-world raids, turf
intro, shops). `..\HANDOFF.txt` has the setup steps and the current list of
what works and what does not.

* login, heartbeat, logout, the messaging WebSocket handshake
* player profile, currency, turf and customization bags; rivals
* save game and gameplay-state storage (persisted in `state.json`)
* name change, turf site selection, character customization
* XP gain, leaderboards, profiles, telemetry sinks
* raids (start, timer, end and rewards); using favours and equipping upgrades
* turf: building info, racket construction, safehouse upgrade
* the wardrobe and favour shops and their purchases (below)

Implemented from the client's response parsers but not yet exercised in play
(values are simple approximations of the original server's rules):

* death penalty, money laundering, racket removal, warehouse/temple upgrades
* world encounters, crime scenes

Added on top of the original server (see "Jobs and challenges" and "Exports" below):

* the Jobs screen: three daily jobs from the game's own errand list, with the
  prerequisites, level and safehouse gates the original data describes
* the Challenges screen: a rotating community trial with progress, opt-in and
  tier rewards
* the Export side of the warehouse: rackets produce commodities, the warehouse
  holds them (real capacities from the game data), and export missions pay dirty
  money and face

Details that matter (each one was a hang or a crash before it was right):

* contacts (informants): `informants/buyfrominformanttype/<uid>` grants the
  contact's first favour and names it in a root-level `FavourDefinitionUID`.
  The turf intro's "speak with your contact" step waits for the favour whose
  database object is named `FTPE_01` (given by contact `FTPE_Informant_01`).
  The two tutorial contacts make no server request at all: the client just
  re-reads the InventoryBag afterwards and expects the car (Ridge Sport) and
  the Cleaver to be in it, so new profiles start with both.
* `blackmarket/quotes` must reply with a bare array of quotes; an object
  crashes the client.
* `playerdata/gainxp` must return `mCurrentXP` as a string; a number is read
  as 0 and sends the level-up screen into a loop.
* `raid/endraid` must list at least one favour in `mFavoursAwarded` (favour
  definition UIDs as strings) for a successful raid. The first open-world raid
  shows its results in tutorial mode, which only offers Continue after the
  favour cards; with none the screen never unlocks.

* `commerce/store/<uid>` for the Wardrobe Store and the Outfit Store returns
  every finished clothing item / outfit in the game data (`data/store_items.json`,
  built by `tools/store_items.py`) at a flat approximate price in dirty money.
  The reply is `mReturnCode`, `mMessage` and a bare array `data` of item
  records; `mCortexUID` must be an unsigned decimal string. These two stores
  must not be empty: the wardrobe screen then shows its "empty" state without
  building its category tree and the next button press crashes the client
  (null read at `TriadWars.exe+0x6fcc48`).
* The favour-pack stores (the regular "Favor pack - Pack Store" and the three
  tutorial pack stores) list favour packs at a flat approximate price in clean
  money. They must not be empty either: the favour shop builds its large tile
  from the first pack without checking that one exists
  (`TriadWars.exe+0x72240f`). Which pack belongs in which tutorial store is
  hard-coded in the client (func `140654510`).
* Each pack store is paired with a bundle store (the regular "Favour Pack
  Bundle Store" and three tutorial ones; pairing hard-coded in func
  `1407100c0`). Selecting a pack opens a details screen that searches the
  bundle store for `UFG::StoreBundle` items made of that pack (1x, 3x, 5x).
  The bundle is what gets bought, and the 1x bundle supplies the price shown;
  if it is missing the client crashes (`TriadWars.exe+0x6a34ec`).
* `game/purchase/<store uid>/<item uid>` for the stores above takes the price.
  Clothing and outfits are added to the InventoryBag. Packs are opened on the
  spot and pay out six upgrade favours each (what the real packs held was
  decided by the original server). The reply nests the bags under
  `data.mDataBags`, next to `data.CortexUID`; for a bundle, `data.ItemContext`
  holds one `{"StoreItemIDStr": pack uid, "FavourResults":
  [{"FavourDefinitionUIDString": uid}, ...]}` record per pack.

Answered with an empty but valid result:

* the other stores (the hub's general "Bundle Store", vehicles, health, gold,
  two unused tutorial stores), coupons and their purchases; black market
  trades; favour sales and informants
* operations; notices and private messages
* player-run triads (clans) and friends, which need other players

Jobs and challenges are no longer in that list: `game/errands`,
`game/completederrands`, `game/errands/end|claimreward|change`,
`game/trialsscreenvisits`, `game/trial/reportprogress` and
`game/trialinstance/optin` all have real handlers now.

Not emulated: the separate "bombd" game-server layer used for multiplayer
sessions. Everything here is single-player.

## Problems already solved

All fixed in the current server. Listed so the family is recognisable.

| Symptom | Cause |
| --- | --- |
| Blank player; first-raid results screen empty and stuck | Data bags wrapped wrongly (root vs `mDataBags`, see above). |
| Values read as zero | 64-bit values must be unsigned decimal strings; a leading minus reads as 0. The client itself sends some UIDs as negative numbers; convert with `int(x) % 2**64`. |
| Wrong or no rival at a turf site | Site *n*'s owner is rival *n-1*; seven rivals must always be sent, in order. |
| Safehouse could not be selected | Safehouse, warehouse and temple need `BlockIndex` -1. |
| No build points | Empty lots were listed as rackets and counted against the limit. |
| Turf panel stuck loading; "select your safehouse" never advanced | Turf replies need `mRacketTypeUID` at the root and the right `mActionType` (9 for safehouse info). |
| "Speak with your contact" never completed; no car or cleaver | The contact's favour must be granted; the two tutorial items are expected to already be in the inventory, with no request made. |
| Raid results crashed | Face-tier table must have exactly ten tiers. |
| Level-up screen looped | `mCurrentXP` must be a string. |
| First open-world raid results never offered Continue | A successful raid must award at least one favour. |
| Using a favour crashed the UI | The reply must name the favour's definition UID. |
| Black market crashed | `blackmarket/quotes` must be a bare array. |
| Outfits crashed (`+0x6fcc48`) | Clothing/outfit stores were empty. |
| Favour shop crashed (`+0x72240f`) | Pack store was empty. |
| Selecting a pack crashed (`+0x6a34ec`) | The pack's 1x bundle was missing from the paired bundle store. |
| Game vanished mid-play with no crash dump | A debugging script force-closed it on a timeout. Not a game fault; never `taskkill` the game. |

## Filling another store

1. `devdata\gamedb.json` has every `UFG::Store` (17) and every store item
   type. A store accepts one item type, named in `mAllowedStoreItemType`.
2. `python tools\findimm.py <store uid>` shows where the client hard-codes
   that store, which tells you which screen uses it. Read that screen's code
   for what it assumes about the items before choosing them.
3. Add the store to `tools\store_items.py` and run it (it rewrites
   `data\store_items.json`), add a price for the item type to `STORE_PRICES`
   in `handlers.py`, and make `purchase` do something sensible for it.
4. Extend `tools\smoke_test.py` if the client pairs the store with another
   one, as it does for packs and bundles.

To regenerate `devdata\gamedb.json` from the game's own files (not re-run
since it was first produced):

    cd devdata
    python ..\tools\qcmp.py unpack ..\..\Global.big     # writes unpack\Global_<offset>.bin
    python ..\tools\gamedb.py export gamedb.json        # reads unpack\Global_0279af88.bin

## Extending

Play with the server window visible and watch for `UNHANDLED` lines; each one
names an endpoint that needs a handler in `handlers.py`. Request and response
bodies are in `logs/`. `tools/keys.py` lists the JSON keys (and their types) a
given endpoint's client-side parser reads.

The three catalogues are generated, not hand-written - re-run the builder after
changing anything under `devdata/`:

    python tools\errands.py          -> data\errands.json    (116 jobs, reward tables)
    python tools\exports.py          -> data\exports.json    (commodities, prices, racks)
    python tools\challenges.py       -> data\challenges.json (trials, tiers, tuners)

`data/exports.json` and `data/challenges.json` are also safe to hand-edit: the
server reads them as plain JSON at start-up.
