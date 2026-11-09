TRIAD WARS PLAYER MODEL CHANGER — EXPERIMENTAL

This separate tool selects the outfits and clothing looks already in Triad Wars.
It does NOT load custom 3D meshes, GTA models, textures or Sleeping Dogs mods.
External model import needs a verified Triad Wars resource loader and compatible
assets; those are not implemented. Nothing here changes the debug-menu DLL.

INSTALL / USE
1. Extract PlayerModelChanger into the Triad Wars folder beside LocalServer.
   This needs the existing Triad Wars local server, its data and Python with
   Tkinter (from python.org, with Add to PATH). It is not a standalone game.
2. Start the game once to create LocalServer/state.json, then close the game
   AND the server window. Double-click Player-Model-Changer.bat.
3. Select a look or outfit and click Apply selected. Start the game again.
   The look is not applied live. Results have not been verified in game.
4. To undo, close game and server again and click Restore original appearance.
   The first pre-change CustomizationBag is kept in
   LocalServer/player-model-backup.json. Other progress is not rolled back;
   pieces added to your wardrobe inventory remain owned. Keep that backup.

PRESET FOLDERS
Mods/Example-Swat/model.json shows a folder preset. Copy the folder and change
name and look ID; use an ID in LocalServer/data/looks.json. Or replace "look"
with "outfit" and a valid outfit UID string from the server's outfit catalogue.
Click Rescan preset folders. Unknown IDs and extra manifest fields are rejected.
These presets refer to assets already in the game, not user-supplied models.

The manager refuses changes while port 443 is in use or TriadWars.exe is running,
and checks that the backup belongs to the same profile. You must still make sure
that any differently configured server has stopped before editing the profile.
