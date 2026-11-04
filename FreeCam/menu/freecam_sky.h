// freecam_sky.h - the skybox override: one switch, a variant, and a colour.
//
// The game owns the sky. It picks a skydome out of the SkyMarshall's own list, blends to
// it over thirty seconds, and changes it again whenever the weather feels like it - so a
// sky picked in a menu and then left alone is gone the moment it rains.
//
// This module holds it still:
//
//   * the variant is the game's own skydome list (the same list set_skydome picks from in
//     freecam_weather.cpp), re-applied whenever the game moves off it;
//   * the colour is written into the block the renderer copies the sky's colours out of
//     every frame, so it is the sky that changes and not the screen;
//   * switching it off puts every value that was borrowed back where it came from.
//
// The colour half is the one that has to be careful. The sky's colours are not a fixed
// address: the renderer reads them through SkyMarshall -> sky slot -> entry -> parameter
// block (see the table below), and every hop is checked before anything is written. A
// build whose sky does not look the way this one does finds nothing, says so in the menu,
// and leaves the memory alone - the variant half still works.
//
//   SkyMarshall            0x23a73e0   the object weather/skydome already use
//     +0x0d8  void*[4]                  the sky slots, stride 0x20
//     +0x9fec float                     the change-in duration, in seconds
//     +0x9ff0 float                     the change-out duration, in seconds
//   slot
//     +0x078  int64                     offset to the slot's entries (0 = none yet)
//   entry (0xc0 bytes)
//     +0x050  void*                     parameter block the renderer reads
//   parameters
//     +0x05c  float[3]                  colour
//     +0x068  float[3]                  colour
//     +0x074  float[3]                  colour
//
// Every one of these was read off TriadWars.exe (sha256 6bd58dbd...32cf7b) rather than
// guessed. The chain is the one the sky's own blend code walks at 0x14006d1e0, and the
// three colour triples are the ones it copies into the sky's constant buffer at
// 0x14006dbe0; the two durations are the divisors of its blend at 0x14006e28f. Being
// read off a binary is not the same as being tested against it, which is why every hop is
// checked before it is used and why a sky that is not shaped this way is left alone.
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

// The one settings object: the menu's rows write it, the K and Y hotkeys write it, and
// SkyTick reads it. There is no second copy to drift out of step.
tw::SkySettings& Sky();

// The sky this build can reach, in the menu's terms: "Automatic" first, then every
// skydome the installation has. Empty until the world has loaded, like SkydomeNames().
std::vector<std::string> SkyVariantNames();

// The menu's "Sky" row: what the override is doing, in one line.
std::string SkyStateLine();

// Called once a frame from the menu's present hook - the same place the menu's other game
// writes run. Applies the variant and the colour while the override is on, puts the
// borrowed values back when it is off, and does nothing at all before the world loads.
void SkyTick();

}  // namespace game
}  // namespace tw
