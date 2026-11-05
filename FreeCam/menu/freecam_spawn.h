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
//   * the character prefabs that exist are named in the game's data bundles rather than
//     in the executable, and the executable only names a handful. SpawnModels() therefore
//     starts from those, adds anything in FreeCam.npc.txt, and - once, in the background -
//     every "object-physical-character-..." name it can read out of the game's own files.
//     A name the game does not know fails with a message, it does not crash.
#pragma once

#include <string>
#include <vector>

namespace tw {
namespace game {

// Models the menu offers, most useful first. Built from the character property sets the
// executable itself names, or from FreeCam.npc.txt when that file is there (one name per
// line, # starts a comment).
std::vector<std::string> SpawnModels();

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

// A scan of the game folder that collects every character property set it can see in the
// game's data files, merges them with the built-ins and FreeCam.npc.txt, writes the merged
// list back to FreeCam.npc.txt and makes it the list the menu offers. Read-only and
// bounded. Returns the status line for the menu.
std::string SpawnScanGameFiles();

// The same scan, on a thread of its own, so the menu does not stop while it reads a
// gigabyte of game data. Returns false when one is already running - starting it twice is
// harmless rather than useful, and the list is replaced once, at the end.
bool SpawnScanAsync();

// True while a background scan is reading the game folder.
bool SpawnScanRunning();

// How many models the menu can offer right now, and how many of those came from the game
// files rather than from the executable's own handful.
int SpawnModelCount();
int SpawnScannedModelCount();

// The rival crews: true for a name the built-in table marks as an enemy, and how many of
// those the list starts with. The menu uses the first to switch the side to Hostile when
// one is picked, and the second for the line that says they are there.
bool SpawnModelIsEnemy(const std::string& model);
int SpawnEnemyModelCount();

// Goes up every time the model list is replaced, so the menu can rebuild its selector.
int SpawnModelsRevision();

// After a spawn has faulted the spawner switches itself off rather than call the same
// function again; this turns it back on for one more try, and says when it is off.
void SpawnClearFault();
bool SpawnBlocked();

// Ask for a spawn. Safe to call from any thread: the request is queued and carried out by
// the camera thread (see SpawnPump), which is the only thread that has the game's
// per-thread allocator. `model` empty means "the selected model".
void SpawnRequest(const std::string& model, bool hostile);

// The same, with the distance chosen: 3 is the normal "in front of you", 0 puts the NPC
// where the player is standing (the fallback the spawner uses by itself when the spot in
// front is refused, and useful on its own where there is no room).
void SpawnRequestAt(const std::string& model, bool hostile, float metres);

// Where a spawn at `metres` would put the NPC, as a 4x4 world matrix (row-major, the
// layout fc_mat uses: rows are right/up/backwards, translation in 12..14). This is the
// same transform SpawnRequestAt would use, so what the preview draws is where the NPC
// will stand. Returns false when the player is not in the world yet.
bool SpawnSpot(float metres, float out[16]);

// Carry out at most one queued request. Call every tick from the camera thread.
// Returns true when a request was carried out.
bool SpawnPump();

// A copy of the last status line, taken under the lock the camera thread writes it with.
// The menu shows this every frame, so it must not read the live string while the pump is
// replacing it.
std::string SpawnStatusCopy();

// Whether a spawn could happen right now: the player is in the world, so there is a
// transform to stand in front of. False on a loading screen - the menu says so instead of
// queueing a spawn that can only time out.
bool SpawnPlayerReady();

// Whether a request is still queued (waiting for the world, or for the allocator).
bool SpawnPending();

// ---- props and objects -----------------------------------------------------------
//
// A prop is spawned through the same three calls as a character (hash, name lookup,
// SpawnAtXform) and differs only in the names it offers and in not being given a side.
// The list starts from the vehicles and weapons the executable names, adds
// FreeCam.props.txt, and grows with whatever the game-file scan finds; a name the game does
// not know fails with a message and a log line, never a crash.
std::vector<std::string> PropModels();
int PropModelCount();
int PropModelsRevision();          // goes up when the list is replaced
int PropScannedPropCount();        // how many of those came out of the game files
void PropSetModel(int index);
int PropModelIndex();
void PropSelectByName(const std::string& model);
std::string PropShortName(const std::string& model);

// Ask for a prop at a distance in front of the player (3 is the normal one, 0 is "on you").
// Queued like the NPC request and carried out by the camera thread.
void PropRequest(const std::string& model);
void PropRequestAt(const std::string& model, float metres);
bool PropPump();                   // one queued request, on the camera thread
std::string PropStatusCopy();
bool PropPending();

// The prop spawner switches itself off after a fault, independently of the NPC one.
void PropClearFault();
bool PropBlocked();

// The prop half of the game-file scan, and the background version of it.
std::string PropScanGameFiles();
bool PropScanAsync();
bool PropScanRunning();

// The last result, e.g. "Spawned object-physical-character-police 3 m ahead (hostile)".
// Empty until something has been spawned. Also written to TW_MenuLog.txt, with the
// hashes and the faction ids, so a failure can be read back later.
const std::string& SpawnStatus();


}  // namespace game
}  // namespace tw
