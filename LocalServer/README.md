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
| `data/store_items.json` | What every store sells: 792 wardrobe pieces, all 28 outfits (12 of them non-player models), 247 vehicles, the health item, and the favour packs with their 1x/3x/5x bundles. Each outfit keeps its piece UIDs so the server can put it on (see `tools/store_items.py`). |
| `data/looks.json` | The outfit lookbook: 32 named looks, each a list of `UFG::StoreClothing` UIDs put together by hand - gang, zombie, swat, tactical, suit, triad and no-clothes sets (see `tools/looks.py`). |
| `data/errands.json` | Every job (`UFG::ErrandTypeMetadata`) with its reward, requirements, prerequisites and completion conditions, plus the `UFG::ErrandRewards` tables (see `tools/errands.py`). |
| `data/challenges.json` | The rotating challenges (`JiangShiTrial`, `HappyCatTrial`, `EliteGhoulTrial`), their progress tiers and the `UFG::GameRewardTuner` each tier pays (see `tools/challenges.py`). |
| `data/favour_upgrades.json` | Every favour that grants an enforcer upgrade, with the rarity/tags/level job and challenge rewards filter on (see `tools/favour_rewards.py`). |
| `useroptions.py` | Reads/edits the obfuscated `UserOptions.dat`. |
| `cert.pem`, `key.pem` | Self-signed certificate for `localhost` (the client does not verify it). |
| `logs/` | One request log per server run. Lines marked `UNHANDLED` are endpoints with no handler yet. |
| `tools/` | The scripts used to unpack the game data and decode its schema, crash-analysis helpers, `smoke_test.py`, `test_jobs.py`, `test_shops.py`, `test_looks.py`, `playermodel.py`, `looks.py` (the lookbook), `schema_dump.py`, `patch_exe.py` (see above), `make_bundle.py` and the data builders (`errands.py`, `challenges.py`, `favour_rewards.py`, `store_items.py`). |
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
   whose gates are met and whose prerequisite jobs are all completed, highest
   `mPriority` first. A brand-new profile has exactly one eligible job, "Raid A
   Rival" - the first step of the tutorial ladder the data encodes.
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
   The board rolls over at UTC midnight (unclaimed rewards survive it), and
   `mSecondsToRefresh` says how long is left.

#### The ladder, all of it

Until this round the board only ever offered the tutorial jobs and a handful of others
behind them: **17 of 87** enabled jobs could be reached. Three things were in the way, and
each is handled in `state.py` instead of editing the game's data:

* **49 jobs list "Complete 3 Exports" as a prerequisite**, and this build does not run the
  export chain, so that one job could never be finished and everything behind it stayed
  locked. Stats the build cannot produce are named in `ERRAND_UNSUPPORTED_STATS`; the jobs
  that ask for them never appear on the board and are skipped as prerequisites too, so the
  ladder opens up around them instead of waiting for them.
* **Four jobs ask for safehouse level 15**; the safehouse has ten levels
  (`mSafehouseUpgradeCosts`). `ERRAND_SAFEHOUSE_CAP` reads those as "the top level".
* **Repeatable jobs come back on the game's own cooldown** -
  `mRepeatableErrandCooldownHours`, 96 hours - not instantly.

That leaves **85 of the 87** enabled jobs playable, in the order the data intends; the two
left out are exactly the two export jobs. When the enabled ladder runs out, the board falls
back to the **29 jobs the game data switches off** (`mIsEnabled` false: the combat-tutorial
variants such as "Sweep Kicks", the commodity collectors, ...). They are ordinary jobs the
client still counts - they were simply never handed out in the beta. Set
`ERRAND_BONUS_JOBS = False` to keep the board to the game's own 87.

One more rule keeps the screen from going empty: when nothing the player's level and
safehouse open is left (everything done, or waiting out a repeatable cooldown), the board
falls back to the jobs whose prerequisites are met anyway, easiest requirement first. The
client evaluates the objective from its own stat counters, so a job whose level gate is out
of reach is still playable - and an empty Jobs screen is worse than an ambitious one. In a
simulation a level 1, safehouse 1 profile can play **114 jobs back to back** before every job
left is on cooldown.

    python tools/storyline.py          what can be reached, in the order it unlocks
    python tools/storyline.py --check  exit 1 if a job that should be playable is not

The tool walks a whole playthrough - level 1 to 50, safehouse 1 to 10, an hour of game time
per step - and reports what is left over and why.

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

### Open-world activities (`game/operation/endoperation`)

The city's repeatable activities - Catch, Defeat, Photo, Deliver, Collect, Destroy, Hijack -
are `UFG::OperationType` objects in the game data. The client runs them; when one ends it
reports `game/operation/endoperation/<operation>/<completed>/<successful>`. The server used
to answer with a zero reward. It now pays from the same `UFG::ErrandRewards` ladders the
jobs use, indexed by XP level:

| Activity | Pays | At level 1 |
| --- | --- | --- |
| Catch | face, low | 20 face |
| Photo | face, medium | 40 face |
| Defeat, Destroy, Hijack | dirty money, medium | 65 |
| Deliver, Collect | dirty money, low | 35 |

A failed run (`<successful>` = 0) pays nothing. The reply is `mRewardType`, `mReward` (the
amount as a string), `mOperation`, `mSuccessful` and the bags. The operation can be named or
sent as its own UID - the route's format string is `%s`, and the handler resolves both.

### Crime scenes (`game/openworldcrimescene`)

After a fight the client leaves the bodies in the world and tells the server where:
`game/openworldcrimescene/<x>/<y>/<z>/<victim player data id>`. The server records the
scene and hands the same coordinates back from a bare `GET`, so the player's own fights
become collectable crime scene markers. Walking to one and taking it
(`openworldcrimescenecollection/<scene id>`) pays 10 face, once: the reply carries the
scene's id and `FaceCollected`, which is what the HUD credits.

* 24 markers are kept at a time, the oldest drops off;
* a scene expires an hour after it was reported;
* the same corpse reported again within 4 units is the same scene.

The server never places a scene itself - the client is the only thing that knows where
anything happened, so there is no invented position to get wrong.

### Street events (`game/worldencounter/...`)

The random events the world spawns - a mugging, a shakedown - are run by the client's own
`TWWorldEncounterManager` (Skookum). The server only hears about them when one ends:
`game/worldencounter/endencounter/<type>/<result>`. It now pays 20 face (the game's low
face ladder) for a successful one, at most six times an hour, so the street is worth
stopping for without out-earning the Jobs screen.

`worldencounter/list` stays empty on purpose: the encounters, their units and their
positions belong to the client, and handing it entries it did not spawn is how you get an
invisible or mismatched one. There is nothing for the server to fill in there.

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

### Trying it without the game

    python tools/test_jobs.py      # handlers directly: board, claim, history, change,
                                   # rollover, trial report/opt-in/tiers, restart
    python tools/test_shops.py     # every catalogue, every purchase path, the model
    python tools/test_looks.py     # the lookbook: real UIDs, slots, every look applies
    python tools/test_activities.py  # operations, crime scenes, street events, restart
    python tools/test_raids.py     # rival availability, raid cooldown, profile migration

The test scripts build a throw-away profile in a temp folder, so they never touch
`state.json`. `tools/smoke_test.py` runs the same screens over HTTPS against a running
server (start it with `--port 8443` and pass `--port 8443`).

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
| `ClothingStoreItemUIDs` holds the UIDs of the worn `UFG::StoreClothing` pieces | the field is a list of store item UIDs and the wardrobe sends it next to `ClothingOutfitCortexUID`; the outfit's own `mClothingItems` gives the pieces | the model does not change; `tools/playermodel.py show` prints what is set |
| `ClothingOutfitCortexUID` is the worn `UFG::StoreOutfit` UID | same reply; the field is named after the outfit | only the outfit name in the wardrobe is wrong, the pieces still apply |
| a crime scene pays 10 face, a street encounter 20 | neither is in the game data; both use the low face ladder as a yardstick | rewards feel low or high - `state.CRIME_SCENE_FACE`, `state.WORLD_ENCOUNTER_REWARD` and `WORLD_ENCOUNTER_LIMIT` are one line each |
| a beaten rival's turf is back after ten minutes | `mRemainingSeconds` is a countdown (the world map greys a site out while it runs, `FREE_REFRESH_RIVALS`), but the original number is unknown | raids come too often or too rarely - `state.RIVAL_RAID_COOLDOWN` |
| the `CrimeScenes` list carries exactly the six members the client's parser declares | `devdata/endpoints.txt`, "openworldcrimescene" | extra members would be dropped by `schema.py` anyway; `Created`/`CollectedAt` stay on our record and are never sent |
| buying an outfit also wears it | the shop has no "try on" step and the model has to come from somewhere | the player keeps the old look until they open the wardrobe; `tools/playermodel.py set <name>` does it directly |

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
* raids (start, timer, end and rewards), with a ten-minute turf cooldown on the site you
  just beat; using favours and equipping upgrades
* turf: building info, racket construction, safehouse upgrade
* the wardrobe and favour shops and their purchases (below)

Implemented from the client's response parsers but not yet exercised in play
(values are simple approximations of the original server's rules):

* death penalty, money laundering, racket removal, warehouse/temple upgrades
* export missions

Added on top of the original server (see "Jobs and challenges" below):

* the Jobs screen: three daily jobs from the game's own errand list, with the
  prerequisites, level and safehouse gates the original data describes, and the
  29 jobs the game data switches off as a fallback once the ladder is finished
* the Challenges screen: a rotating community trial with progress, opt-in and
  tier rewards
* payouts for the city's own activities: the seven operation types, crime scenes
  and the random street encounters (below)

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
* `playerdata/opponents` carries `mRemainingSeconds` per rival, and it is a
  countdown, not decoration: the world map greys a site out until it reaches zero
  (`FREE_REFRESH_RIVALS`, and the client's own "Unable to provide new rivals when
  all turfs are on cooldown"). A fresh profile now has all seven rivals open; a
  successful raid puts that one site on `state.RIVAL_RAID_COOLDOWN` (ten minutes),
  and a lost raid leaves it open so the attempt can be retried. Profiles saved with
  the old "everyone, 24 hours" value are migrated to v8 on load.

* `commerce/store/<uid>` answers for every store the client has, with every item of
  the type that store accepts (`mAllowedStoreItemType`) that is usable, straight from
  the game data (`data/store_items.json`, built by `tools/store_items.py`):
  **792 wardrobe pieces**, **all 28 outfits**, **247 vehicles**, the health vendor's one
  item, and the favour packs with their 1x/3x/5x bundles. The reply is `mReturnCode`,
  `mMessage` and a bare array `data` of item records; `mCortexUID` must be an unsigned
  decimal string. The wardrobe and outfit stores must not be empty: the wardrobe screen
  then shows its "empty" state without building its category tree, and the next button
  press crashes the client (null read at `TriadWars.exe+0x6fcc48`).
* Prices are this server's own (`STORE_PRICES` in `handlers.py`): 250 dirty money for a
  clothing piece, 1500 for an outfit, 4000 for a vehicle, 500 for the health item, 100
  clean money per favour pack. The real prices lived on the original server.
* Clothing, outfits and vehicles are `AddToInventory` items and go into the
  `InventoryBag`, which is what the client reads to know the player owns them. An
  outfit also goes straight onto the player: `CustomizationBag.ClothingOutfitCortexUID`
  and `ClothingStoreItemUIDs` are set to it and its pieces, and the reply carries the
  customization bag, so buying "Swat Uniform" changes the model without a trip to the
  wardrobe. The health item is a pure currency transaction and stores nothing.
* The favour-pack stores (the regular "Favor pack - Pack Store" and the four tutorial
  pack stores) list favour packs at a flat approximate price in clean money. They must
  not be empty either: the favour shop builds its large tile from the first pack without
  checking that one exists (`TriadWars.exe+0x72240f`). Which pack belongs in which
  tutorial store is hard-coded in the client (func `140654510`). The regular store lists
  every pack that can actually be bought - one without a 1x bundle is left out, see
  below.
* Each pack store is paired with a bundle store (the regular "Favour Pack Bundle
  Store", the hub's "Bundle Store" and the tutorial ones; pairing hard-coded in func
  `1407100c0`). Selecting a pack opens a details screen that searches the bundle store
  for `UFG::StoreBundle` items made of that pack (1x, 3x, 5x). The bundle is what gets
  bought, and the 1x bundle supplies the price shown; if it is missing the client
  crashes (`TriadWars.exe+0x6a34ec`), so `tools/store_items.py` only lists packs that
  have one and asserts it.
* `game/purchase/<store uid>/<item uid>` handles all of the above. Packs are opened on
  the spot and pay out six upgrade favours each (what the real packs held was decided
  by the original server). The reply nests the bags under `data.mDataBags`, next to
  `data.CortexUID`; for a bundle, `data.ItemContext` holds one
  `{"StoreItemIDStr": pack uid, "FavourResults": [{"FavourDefinitionUIDString": uid},
  ...]}` record per pack.

### The player model, and the non-player ones

The client draws the player from `CustomizationBag`: `ClothingStoreItemUIDs` lists the
`UFG::StoreClothing` pieces that are worn, `ClothingOutfitCortexUID` names the outfit
they came from, and `ScalingPresetCortexUID` picks a body preset. There is no model UID
anywhere in the protocol - the model *is* the set of pieces - so changing it means
changing those UIDs, which is exactly what the wardrobe does.

The game ships 29 outfit definitions. Sixteen were sold in the wardrobe; the other
twelve are the looks the NPCs wear, and the wardrobe skipped them because their head or
hands are labelled "NOT READY FOR STORE YET" (the piece was never sold loose, but the
model is in the game):

    Swat Uniform            Super Swat Uniform      Agent 47
    Zombie Enforcer         NVidia Zombie Enforcer  Deus Ex
    Sun Wukong              Masked Justice          Bronze Master
    Shaolin Warrior         Ultimate Beatdown       Spry and Viscious

All 28 outfits that have every piece defined are now in the outfit store, so any of
those looks can be bought and worn. Two ways to put one on:

* in the game - buy the outfit; the server wears it as part of the purchase (see
  above), and the wardrobe lets you change back;
* without the game - close the game and the server, then

      python tools\playermodel.py list --npc       # the twelve models
      python tools\playermodel.py set swat         # or the full UID
      python tools\playermodel.py show             # what is worn now
      python tools\playermodel.py clear            # back to the default model

  It writes the same three fields into `state.json` and adds the outfit and its pieces
  to the inventory, so the wardrobe shows them as owned. Start the game afterwards.

The 42 clothing pieces with no category at all are outfit-only parts (a jacket's
sleeves, a mask's straps) and are not sold loose: the wardrobe builds its tree from the
category, and a piece without one has nowhere to go. They arrive with their outfit.

### The outfit lookbook

An outfit the wardrobe shows as a tile is an `UFG::StoreOutfit` in the game's own data,
and the client resolves every tile UID against that data before it draws anything. A UID
it has never seen is the same null-pointer family as the pack-tile crash above, so the
shop cannot be given brand-new outfit records from the server side. What *is* free is the
combination: the model is drawn from `ClothingStoreItemUIDs`, so any set of pieces the
game already ships is a wearable look. The wardrobe lets you assemble one by hand; the
lookbook gives the good ones names.

`data/looks.json` holds 32 of them, every piece a real `UFG::StoreClothing`:

    Gang        Corner Boy, Block Boss, Ride or Die, Street Soldier, Bam Bam, Sun On Yee
    Zombie      Fresh Zombie, Neon Zombie, Zombie Boss
    Swat        Swat Pointman, Swat Breacher, Swat Urban
    Tactical    Ghost, Desert Operator, Night Ops, Clown Unit
    Suit        Boardroom, Undertaker, Blood Money, Miami Vice, Suit & Tee, White Suit
    Triad       Red Pole, Temple Guardian, Jade Lotus, Tiger Style, Flying Dagger,
                Monkey Lord, Quiet Sage
    No clothes  Bare Essentials, Full Monty, Bare Knuckle

To wear one, with the game and the server closed:

    Change-Look.bat                              # double-click: numbered list, pick one
    python LocalServer\tools\playermodel.py looks          # the list, with its pieces
    python LocalServer\tools\playermodel.py set "swat pointman"
    python LocalServer\tools\playermodel.py set gang        # a theme lists its looks
    python LocalServer\tools\playermodel.py set outfit swat # the game's own outfits still work
    python LocalServer\tools\playermodel.py show           # names the look that is on

It writes the same three `CustomizationBag` fields the wardrobe writes, and adds every
piece to the inventory, so after one look the wardrobe shows those pieces as owned and
you can tweak the look by hand from there. The lookbook is a starting point, not a locked
costume: nothing stops the wardrobe changing it afterwards, and `clear` goes back to the
default model.

Each look uses at most one piece per wardrobe slot - the slots are the wardrobe's own,
read from the piece's `mSubType` (`Tops`, `Pants`, `Shoes`, `Hats`, `Accessories|Masks`,
`Body|Hair`, ...), so a look is a combination the wardrobe itself would allow. A
generated-file warning applies: edit the table in `tools/looks.py`, not the JSON, then

    python LocalServer\tools\looks.py            # rebuild data/looks.json
    python LocalServer\tools\test_looks.py       # real UIDs, slots, every look applies

Two details worth knowing. Where several UIDs share a display name they are tint variants
of one garment and the lowest UID is used, because the data does not say which colour is
which. And `Full Monty` uses `Bare Legs DEBUG ONLY`, the game's only bare-legs mesh - it
is marked debug in the data, and it is exactly what a no-clothes look needs.

Answered with an empty but valid result:

* the gold store's ten `BeginExternalPurchase` items and the two spare tutorial
  pack stores (Extortion and Gambling; the racket-boss tutorial uses the
  Counterfeiting one), coupons and their purchases; black market trades; favour
  sales and informants
* operations; notices and private messages
* player-run triads (clans) and friends, which need other players

Jobs, challenges and the world's activities are no longer in that list:
`game/errands`, `game/completederrands`, `game/errands/end|claimreward|change`,
`game/trialsscreenvisits`, `game/trial/reportprogress`, `game/trialinstance/optin`,
`game/operation/endoperation`, `game/openworldcrimescene` and
`game/openworldcrimescenecollection` all have real handlers now.

Not emulated: the separate "bombd" game-server layer used for multiplayer
sessions. Everything here is single-player.

## The free camera

This is the one part of the round that is **not** delivered, and it is not something the
local server can do.

The camera belongs entirely to the client. There is no camera endpoint in the protocol -
`devdata/endpoints.txt` has none - and every camera symbol in the executable is an engine
internal: `UFG::DebugCameraComponent` (with its RTTI name and `qNode`), the director's
`DebugCamera` and `DebugCamera1` objects, `FollowCameraDebugSlowMo`, and `InvertDebugCamera`,
which is a `HostInfo.xml` build flag, not a key binding. Nothing the server sends reaches
the view: the protocol carries the player's turf site and the camera is the client's own
follow rig.

A Caps Lock free camera has to live in the game process, the same way
`tools/patch_exe.py` fixes the shadow crash - either a byte patch that binds the engine's
existing debug camera, or a small external tool that holds the render camera's transform
while the player stands still. Both need `TriadWars.exe` in this folder: the code sites,
the camera component's layout and the input hook cannot be derived from the string dump
alone.

With the executable next to `Play-TriadWars.bat` (the same file `patch_exe.py` patches),
the work is:

1. `python tools/twre.py xref DebugCameraComponent` and `xref DebugCamera` - find what
   activates the director's debug camera, and what puts it back;
2. find where the game reads Caps Lock (`GetAsyncKeyState(VK_CAPITAL)`, `0x14`) or add a
   code cave that polls it and toggles that camera;
3. leave the player's own input alone while the camera is detached - the engine's debug
   camera already works that way.

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
| Nothing to raid after the tutorial | Every rival was sent as "on cooldown for 24 hours" (`mRemainingSeconds` 86400), which greys out the whole world map. The roster now starts open, and a win puts that one site on a ten-minute cooldown. |
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
   in `handlers.py`, and make `purchase` do something sensible for it. The
   generator now covers every store whose item type it knows - clothing,
   outfits, vehicles, vendor items and the two pack kinds - so a new store of
   one of those types only needs a line if the client pairs it with another.
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
