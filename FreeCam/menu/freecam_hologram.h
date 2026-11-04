// freecam_hologram.h - the spawn hologram: a wireframe NPC standing where the spawn will
// put it, drawn over the game view by the debug menu.
//
// Two questions it answers at a glance, which is what it is for:
//
//   * where  - the figure stands on the exact transform SpawnRequestAt would use, because
//              it is built from the same SpawnSpot() call rather than from a second
//              estimate that could drift from it;
//   * what   - the label carries the model name, the distance and the side, so the figure
//              and the thing that will be spawned cannot disagree.
//
// Nothing is spawned to draw it. A preview that put a real NPC in the world would be a
// spawn - the same allocator, the same fault path, and something to clean up afterwards -
// and a ghost you can walk through is a worse answer than a shape on the screen. This is
// geometry only: the camera's matrices come from FreeCamCameraMatrices, the spot from
// SpawnSpot, and everything here is arithmetic on those.
//
// The figure is built in the character's own frame (right, up, back, in metres) and every
// point is projected through the game's own view and projection matrices. So it stands
// upright, turns with the NPC that will be spawned, sits correctly in the world when you
// move, and gets smaller with distance - it is not a sticker on the screen.
#pragma once

#include <string>

namespace tw {
namespace game {

// On/off, and how far in front of the player the NPC goes. Both are set from the menu and
// kept here so the render loop can read them without asking the menu.
bool HologramPreviewActive();
void HologramSetPreview(bool on);
float HologramDistance();
void HologramSetDistance(float metres);

// The last thing the builder could not do, for the menu's status line. Empty when the
// figure is being drawn. Written by HologramBuildFrame, so read it on the thread that
// draws - it is not the switch, and it is deliberately not shared across threads.
const std::string& HologramNote();

// One line of the figure, already in screen pixels.
struct HoloSegment {
    float ax, ay, bx, by;
};

// Everything the renderer needs for one frame. Built on demand: the module keeps no
// per-frame state, so a frame that is dropped is simply not drawn.
struct HoloFrame {
    static const int kMaxSegments = 64;

    bool visible = false;        // false: nothing to draw, `note` says why
    float feetX = 0.0f, feetY = 0.0f;
    float headX = 0.0f, headY = 0.0f;
    float ppm = 0.0f;            // screen pixels per world metre at the spawn point
    float distance = 0.0f;       // metres from the camera to the spawn point
    float height = 1.80f;        // metres, the figure's own height
    char model[56] = { 0 };      // what will be spawned
    char note[128] = { 0 };      // "3.0 m ahead - friendly", or the reason there is no figure
    int segCount = 0;
    HoloSegment segs[kMaxSegments] = {};
};

// Builds the figure for this frame. `model` is what the spawn would use (the menu's own
// text), `screenW`/`screenH` the size of the game view in pixels. Returns out->visible.
//
// Returns false, with a note, when the camera matrices are not there yet, when the player
// is not in the world, or when the spawn point ends up behind the camera.
bool HologramBuildFrame(const std::string& model, float screenW, float screenH,
                        HoloFrame* out);

}  // namespace game
}  // namespace tw
