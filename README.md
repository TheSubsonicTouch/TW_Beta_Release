
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

WHAT WORKS
----------
The local server stands in for every part of the single-player game: the intro
and tutorials, raids, turf and rackets, the wardrobe and the favour shops, XP
and ranks.

The hub's Jobs screen and the Challenges (community trial) screen are emulated
too. Jobs come from the game's own errand data - three daily slots, each with
the original prerequisites, level and safehouse gates and its own reward
(gold, dirty money, face or a favour card). The challenge rotates through the
client's trials with progress, opt-in and tier rewards. LocalServer\README.md
explains how both work, what is exact and what is this server's own design,
and how to change it.

* The shops sell everything the game has: the whole wardrobe (792 pieces), all 28
  outfits - including the twelve the NPCs wear (Swat Uniform, Zombie Enforcer,
  Agent 47, Deus Ex, Sun Wukong, ...) - 247 vehicles and the health vendor, and
  everything bought is kept in the inventory. Buying an outfit puts it on, so the
  non-player models are playable; LocalServer\tools\playermodel.py can also set
  one from outside the game.
