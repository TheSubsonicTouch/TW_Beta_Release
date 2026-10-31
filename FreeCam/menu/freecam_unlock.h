// freecam_unlock.h - the part of "unlock all" that makes raids and events reachable.
//
// The menu's UnlockAllContent gives the content (vehicles, weapons, upgrades, levels). This
// gives the *opportunities*: it clears every rival's post-raid cooldown so the world map
// offers all of them, and marks the prerequisite jobs as completed so the job/event ladder
// is open rather than stuck on the tutorial's first job. Only prerequisites are marked, so
// every job is still there to play.
#pragma once

#include <string>

namespace tw {
namespace game {

// Edits state.json and nudges the running local server to re-read it. Returns the status
// line for the menu.
std::string UnlockRaidsAndEvents();

}  // namespace game
}  // namespace tw
