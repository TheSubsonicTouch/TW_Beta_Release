// freecam_spawn.h - put an NPC on the ground a few metres in front of the player.
//
// The game has one spawn path: a property-set name is hashed, the hash is turned into an
// id, and the game's own SpawnAtXform builds the object at a matrix. That is exactly what
// its own encounters use to put an on-foot unit in the world, so this module drives the
// same three calls, with the matrix moved three metres along the player's facing.
//
// Two things make it less direct than it sounds:
//
//   * the spawn path allocates through the game's per-thread allocator, which lives in
//     TLS. A thread we made ourselves does not have it, and the first allocation inside
//     the spawn dies on [rax+0x58] with rax = NULL. So the request is carried out on the
//     camera thread after EnsureThreadAllocator() has published the block there, never
//     on the render thread where the menu lives.
//   * the model list cannot be read out of the game without the game running: the
//     character prefabs that exist are named in data bundles, not in the executable, and
//     the executable only names a handful. SpawnModels() therefore offers those, and
//     FreeCam.npc.txt next to the DLL replaces the list with anything else you want to
//     try. A name the game does not know fails with a message, it does not crash.
#pragma once

#include <string>
#include <vector>

namespace tw {
namespace game {

// Models the menu offers, most useful first. Built from the character property sets the
// executable itself names, or from FreeCam.npc.txt when that file is there (one name per
// line, # starts a comment).
const std::vector<std::string>& SpawnModels();

// Which model the hotkeys use, and whether it is spawned hostile. The menu sets these
// too, so both ways of choosing stay in step.
void SpawnSetModel(int index);
int SpawnModelIndex();
void SpawnSetHostile(bool hostile);
bool SpawnHostile();

// The part of a model name that tells the models apart - the property-set prefix is the
// same for all of them and the HUD line is 44 characters. Used by the on-screen line.
std::string SpawnShortName(const std::string& model);

// Make the hotkey path use the model with this name (the menu calls this when you spawn
// from the menu, so B and the menu agree afterwards). Ignored if the name is not listed.
void SpawnSelectByName(const std::string& model);

// Ask for a spawn. Safe to call from any thread: the request is queued and carried out by
// the camera thread (see SpawnPump), which is the only thread that has the game's
// per-thread allocator. `model` empty means "the selected model".
void SpawnRequest(const std::string& model, bool hostile);

// Carry out at most one queued request. Call every tick from the camera thread.
// Returns true when a request was carried out.
bool SpawnPump();

// The last result, e.g. "Spawned object-physical-character-police 3 m ahead (hostile)".
// Empty until something has been spawned. Also written to TW_MenuLog.txt, with the
// hashes and the faction ids, so a failure can be read back later.
const std::string& SpawnStatus();


}  // namespace game
}  // namespace tw
