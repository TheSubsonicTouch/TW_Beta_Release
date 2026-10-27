
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
       Change-Look.bat
       README.txt
       HANDOFF.txt

2. Double-click Play-TriadWars.bat.

To pick an outfit for your enforcer first, double-click Change-Look.bat instead - it
lists the 40 looks, you type a number, and then you start the game as usual.

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

LocalServer\README.md explains how each one works, what is exact and what is
this server's own design, and how to change it.

* The shops sell everything the game has: the whole wardrobe (792 pieces), all 28
  outfits - including the twelve the NPCs wear (Swat Uniform, Zombie Enforcer,
  Agent 47, Deus Ex, Sun Wukong, ...) - 247 vehicles and the health vendor, and
  everything bought is kept in the inventory. Buying an outfit puts it on, so the
  non-player models are playable; LocalServer\tools\playermodel.py can also set
  one from outside the game.
* There is an outfit lookbook as well: 40 ready-made looks built from the game's own
  clothing - gang, zombie, swat, tactical, suit, triad and no-clothes sets. Every piece of
  them is already owned, so the wardrobe lists them and they can be put together in the
  game; Change-Look.bat (game closed) puts a whole look on in one step. They are ordinary
  combinations of wardrobe pieces, so the wardrobe can change them afterwards.
  LocalServer\README.md lists them and explains how to add your own.
* The warehouse trades: rackets fill it with goods, the buy/sell tab deals in all eight
  commodities at the game's own prices, and export missions ship a load out of town for
  dirty money and face.
