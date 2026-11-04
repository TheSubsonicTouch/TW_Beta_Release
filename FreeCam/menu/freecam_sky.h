// freecam_sky.h - the skybox override: hold one of the game's skydomes, and paint it.
//
// Two halves, and they are different things on purpose.
//
//   * THE VARIANT is the game's own skydome list. The SkyMarshall keeps a qArray of them
//     (count at +0x9fd8, data at +0x9fe0, stride 0x60, id at +0x00), and set_skydome at
//     +0x6d150 matches an id against that array and sets the requested index at +0x9f94.
//     While the override is on, this module asks for the chosen entry again whenever the
//     game moves off it - so the sky the menu picked is the sky that stays. This changes
//     the actual skydome: the clouds, the sun, the horizon, all of it.
//
//   * THE COLOUR is painted on the screen, over the top of the frame, in a gradient that
//     fades out before the street. It is not written into the game's sky.
//
// The colour is painted rather than written because the sky's own colours are not reachable
// from outside the game. They live in the .perm.bin resource a skydome entry points at
// (+0x50), which is loaded by the game's resource system and handed to its renderer; there
// is no field in the marshall that holds them, and every write this build made into the
// places they were thought to be produced no change on screen - which is exactly what the
// log said: no colour block was ever found. Painting the top of the frame is honest about
// what it is doing, is visible in every build, and cannot corrupt anything.
//
// What it is not: a post-process shader. It is drawn with the menu's own draw list, so it
// costs one gradient a frame and needs nothing installed alongside the game.
#pragma once

#include <string>
#include <vector>

namespace tw {
// The menu model's settings object. Named rather than included: this header is reached
// from files that already have the model, and menu_core.h is present twice in the build
// tree (once for the checkout, once patched), so pulling it in from here would define it
// twice in the same translation unit.
class SkySettings;

namespace game {

// The one settings object: the menu's rows write it and SkyTick reads it. There is no
// second copy to drift out of step.
tw::SkySettings& Sky();

// The sky this build can reach, in the menu's terms: "Automatic" first, then every
// skydome the installation has. Just "Automatic" until the world has loaded.
std::vector<std::string> SkyVariantNames();

// The menu's "Sky" row: what the override is doing, in one line.
std::string SkyStateLine();

// The colour to paint the top of the frame with, and how strong it is there (0..1). False
// when there is nothing to paint: the override is off, or the colour is "unchanged", or
// the strength is at zero. The renderer supplies the gradient; this supplies the colour.
bool SkyPaint(float* r, float* g, float* b, float* alpha);
bool SkyPaintActive();

// Called once a frame from the menu's present hook - the same place the menu's other game
// writes run. Holds the chosen skydome while the override is on, and does nothing at all
// before the world loads.
void SkyTick();

}  // namespace game
}  // namespace tw
