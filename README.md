
WHAT YOU NEED
-------------
- Your Triad Wars game folder (the one with TriadWars.exe in it).
- Python 3.9 or newer: https://www.python.org/downloads/
  When installing, tick "Add python.exe to PATH".
- Port 443 free (it usually is; see below if not).


HOW TO INSTALL
--------------
1. Extract this zip straight into your game folder - the folder that already
   contains TriadWars.exe (about 1.5 GB of game; this zip is only 2 MB).
   Afterwards you should see these next to TriadWars.exe:

       LocalServer\            (folder)
       Play-TriadWars.bat
       README.txt

2. Double-click Play-TriadWars.bat.

If you extracted it somewhere else by mistake, run Play-TriadWars.bat anyway:
it looks for the game and says where it found it. To point it at a copy it did
not find, drag that folder onto Play-TriadWars.bat once - it remembers.

That's it. The launcher points the game at your PC, starts the server in a
small minimised window, then starts the game. Leave the server window open
while you play. Next time, just double-click Play-TriadWars.bat again.

You start as a brand-new player: create your enforcer, pick a name, choose
your turf, then play through the tutorials.

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
