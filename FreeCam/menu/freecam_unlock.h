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

namespace tw {
namespace game {

// The unlock. `content` also fills the bags and maxes the levels; without it only the
// opportunities are opened. Returns the status line for the menu.
std::string UnlockEventsAndActivities(bool content);

// The menu's smaller entry: the same unlock without the content grants.
std::string UnlockRaidsAndEvents();

}  // namespace game
}  // namespace tw
