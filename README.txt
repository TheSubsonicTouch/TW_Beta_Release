TRIAD WARS - PLAY OFFLINE ON YOUR OWN PC
========================================

Triad Wars' online servers were shut down, so the game no longer starts.
This pack is a small local server that stands in for them, so you can play
single-player on your own PC. It does NOT include the game: you need your
own copy of Triad Wars.


WHAT YOU NEED
-------------
- Your Triad Wars game folder (the one with TriadWars.exe in it).
- Python 3.9 or newer: https://www.python.org/downloads/
  When installing, tick "Add python.exe to PATH".
- Port 443 free (it usually is; see below if not).


HOW TO INSTALL
--------------
1. Extract this zip straight into your game folder. Afterwards you should see
   these next to TriadWars.exe:

       LocalServer\            (folder)
       Play-TriadWars.bat
       README.txt

2. Double-click Play-TriadWars.bat.

That's it. The launcher points the game at your PC, starts the server in a
small minimised window, then starts the game. Leave the server window open
while you play. Next time, just double-click Play-TriadWars.bat again.

You start as a brand-new player: create your enforcer, pick a name, choose
your turf, then play through the tutorials.


YOUR SAVE
---------
Your progress is saved in LocalServer\state.json (created the first time you
play). Back that file up if you care about it. Delete it to start over from
scratch.


IF SOMETHING GOES WRONG
-----------------------
"Python was not found" (or the Microsoft Store opens)
    Install Python from python.org and tick "Add python.exe to PATH",
    then try again.

"The server did not start listening on port 443"
    Another program is using port 443 (often Skype, IIS, VMware or a VPN).
    Close it and try again.

Permission errors / nothing gets saved
    Your game folder may be write-protected (e.g. inside C:\Program Files).
    Either run Play-TriadWars.bat as administrator, or move the whole game
    folder somewhere like C:\Games\TriadWars.

The game crashes a few seconds after the world loads
    This is a known random bug in the game itself (about 1 launch in 7).
    Just start it again.

Some shops are empty
    The vehicle shop, the hub's bundle shop and the health/gold stores have
    nothing in them yet. Avoid the garage shop: opening an empty shop can
    crash the game.

Multiplayer, clans and friends do not work. Prices, rewards and timers are
best guesses, because the real values were on the original servers.


UNINSTALL
---------
In the game folder, run:   python LocalServer\useroptions.py restore
then delete the LocalServer folder, Play-TriadWars.bat and this README.
The game files themselves are never changed.


FOR TINKERERS / AI HELPERS
--------------------------
HANDOFF.txt has setup checks, troubleshooting and crash triage.
LocalServer\README.md is the full technical reference.


WHAT WORKS
----------
The local server stands in for every part of the single-player game: the intro
and tutorials, raids, turf and rackets, the wardrobe and the favour shops, XP
and ranks.

The hub's Jobs screen, the Challenges (community trial) screen and the export
side of the warehouse are emulated too.

* Jobs come from the game's own errand data: 116 of them, three on the board at
  a time, with the original prerequisites, level and safehouse gates and their
  own rewards (gold, dirty money, face or a favour card). They are the game's
  story ladder, not just the tutorials - the tutorial jobs are its first step.
* Challenges rotate through the client's trials with progress, opt-in and tier
  rewards.
* Exports put the warehouse to work: rackets produce commodities, the warehouse
  stores them (the game's own capacities), and export missions pay dirty money
  and face at the game's own rates.

To find out why the game closed on launch, run Play-TriadWars.bat again: it
keeps the window open and writes LocalServer\last-launch.txt (what is listening,
whether the game reached the server, its exit code, and the Windows error entry
with the faulting module). Play-TriadWars.bat safe launches with the new screens
switched off, and Play-TriadWars.bat diagnose only prints the setup report.

If a screen ever crashes the game, LocalServer\state.py has one switch per
feature (FEATURE_JOBS, FEATURE_CHALLENGES, FEATURE_EXPORTS, FEATURE_PRODUCTION).
Turn the one that matches the screen off and save - the server picks it up on the
next request, so the rest of the game keeps working while you report it.

LocalServer\README.md explains how each one works, what is exact and what is
this server's own design, and how to change it.
