// freecam_wardrobe.h - put any of the game's own clothing on the player, mixed.
//
// The client draws the player from CustomizationBag.ClothingStoreItemUIDs: a list of
// UFG::StoreClothing pieces, one per slot. Buying an outfit fills that list with the
// outfit's pieces, but the wardrobe screen only offers pieces the game files give a shop
// category, so an outfit-only garment - the Deus Ex trousers, the Swat trousers, the
// Hitman set - cannot be worn with anything but its own outfit, and the two dozen
// pieces the developers never categorised cannot be worn at all.
//
// This writes the list itself: pick a top, pick a pair of pants, wear them. The hat, the
// shoes and the accessories already on the player are left alone. It edits the same
// profile the unlock does (state.json next to the server) and touches handlers.py, so the
// change is read the next time the client loads its bags - a world (re)load, not the
// frame after the key press.
#pragma once

#include <string>
#include <vector>

namespace tw {
namespace game {

// The pickers. Names are unique and human ("Deus Ex pants", "Swat top"); the menu shows
// them and hands the chosen one back.
std::vector<std::string> WardrobeTopNames();
std::vector<std::string> WardrobePantsNames();
std::vector<std::string> WardrobeLookNames();   // the named looks from data/looks.json

// Wearing. Each returns the status line for the menu.
std::string WardrobeWearTop(const std::string &name);
std::string WardrobeWearPants(const std::string &name);
std::string WardrobeWearCombo(const std::string &top, const std::string &pants);
std::string WardrobeWearLook(const std::string &name);

// Put the list back the way it was before the menu first changed it.
std::string WardrobeUndo();

// What the player is wearing now, for the section's read-only row.
std::string WardrobeStatus();

// True once a wear has been written in this session (so the undo has something to undo).
bool WardrobeCanUndo();

}  // namespace game
}  // namespace tw
