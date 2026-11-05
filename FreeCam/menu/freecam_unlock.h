// freecam_unlock.h - the part of "unlock all" that makes the game have things to do.
//
// The menu's UnlockAllContent (name, upgrades, levels) gives the player the *content*.
// This gives the *opportunities*, which are a different thing:
//
//   * every rival's turf off its post-raid cooldown, so the world map offers all seven;
//   * the whole job catalogue - all 116 errands, the 29 the game data switches off and the
//     ones behind the export chain included - reachable from the board;
//   * the challenge ("event") running and opted into, so the events screen is live;
//   * optionally the content as well: every favour, upgrade and shop item owned, top level.
//
// It edits the profile (state.json, LocalServer/state.json) and touches handlers.py, which
// is how the running local server is told to re-read both it and state.py: server.py
// reloads its two modules whenever one of their timestamps changes.
#pragma once

#include <string>
#include <vector>

namespace tw {
namespace game {

// The unlock. `content` also fills the bags and maxes the levels; without it only the
// opportunities are opened. Returns the status line for the menu.
std::string UnlockEventsAndActivities(bool content);

// The menu's smaller entry: the same unlock without the content grants.
std::string UnlockRaidsAndEvents();

// The game folder (with a trailing backslash), or "" when it cannot be found.
std::string GameFolder();

// UTF-8 text next to the game: the profile and the data files both live there.
bool ReadGameFile(const std::string &path, std::string &out);
bool WriteGameFile(const std::string &path, const std::string &text);
bool DeleteGameFile(const std::string &path);

// The state.json files this install keeps that exist right now - the game folder's and
// LocalServer's, in that order. Empty when the server has never run. Shared with the
// wardrobe, which edits the same profile.
std::vector<std::string> ProfileFiles();

// Tells the running local server to re-read state.json and its own modules (server.py
// stats handlers.py and state.py on every request).
void ReloadServer();

}  // namespace game
}  // namespace tw
