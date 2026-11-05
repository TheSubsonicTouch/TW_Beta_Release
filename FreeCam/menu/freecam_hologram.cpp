// freecam_hologram.cpp - the spawn hologram's geometry.
//
// Everything is in metres, in the frame of the NPC that would be spawned: x to its right,
// y up, z backwards. The spawn matrix's rows are exactly those three axes and its
// translation is the spot on the ground (see fc_mat_from_pose in freecam.c), so placing a
// local point is three multiply-adds and projecting it is one call to fc_project.
//
// A segment whose ends cannot both be projected is dropped rather than clipped: a point
// behind the camera has no screen position, and drawing it anyway would smear the figure
// across the view. Walking half a metre closer is enough to get it back.
#include "freecam_hologram.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "freecam_game.h"    // FreeCamCameraMatrices
#include "freecam_spawn.h"   // SpawnSpot, SpawnShortName, PropShortName

namespace tw {
namespace game {
namespace {

constexpr float kPi = 3.14159265f;

// The figure. A pair of shoulders and a head at 1.6 m read as a person at any distance,
// and the point count stays small: every segment costs two projections.
constexpr float kHeight     = 1.80f;
constexpr float kHeadCentre = 1.62f;
constexpr float kHeadRadius = 0.115f;
constexpr float kShoulderY  = 1.42f;
constexpr float kShoulderX  = 0.20f;
constexpr float kElbowY     = 1.15f;
constexpr float kElbowX     = 0.27f;
constexpr float kHandY      = 0.90f;
constexpr float kHandX      = 0.25f;
constexpr float kHipY       = 0.92f;
constexpr float kHipX       = 0.13f;
constexpr float kKneeY      = 0.50f;
constexpr float kKneeX      = 0.15f;
constexpr float kRingRadius = 0.45f;   // the ground ring, in metres
constexpr int   kRingSteps  = 16;
constexpr int   kHeadSteps  = 10;

bool  g_on = false;
bool  g_propMode = false;
float g_metres = 3.0f;
std::string g_note;

// Everything one frame needs, so the helpers do not have to pass six arguments around.
struct Build {
    const fc_mat* view;
    const fc_mat* proj;
    const float* mat;      // the spawn matrix
    float width, height;   // the game view, in pixels
    HoloFrame* out;
};

void ToWorld(const float m[16], float r, float u, float b, float out[3]) {
    out[0] = m[12] + m[0] * r + m[4] * u + m[8] * b;
    out[1] = m[13] + m[1] * r + m[5] * u + m[9] * b;
    out[2] = m[14] + m[2] * r + m[6] * u + m[10] * b;
}

bool ProjectLocal(const Build& b, float r, float u, float back, float out[2]) {
    float w[3];
    ToWorld(b.mat, r, u, back, w);
    return fc_project(b.view, b.proj, w, b.width, b.height, out, nullptr) != 0;
}

void AddSegment(const Build& b, float ax, float ay, float az,
                float bx, float by, float bz) {
    HoloFrame* f = b.out;
    if (f->segCount >= HoloFrame::kMaxSegments) return;
    float sa[2], sb[2];
    if (!ProjectLocal(b, ax, ay, az, sa)) return;
    if (!ProjectLocal(b, bx, by, bz, sb)) return;
    HoloSegment& s = f->segs[f->segCount++];
    s.ax = sa[0]; s.ay = sa[1];
    s.bx = sb[0]; s.by = sb[1];
}

// A closed loop of kRingSteps points lying flat on the ground: r = R cos, back = R sin.
void AddGroundRing(const Build& b) {
    float prev[2];
    bool have = false;
    for (int i = 0; i <= kRingSteps; ++i) {
        const float t = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kRingSteps);
        const float r = kRingRadius * std::cos(t);
        const float back = kRingRadius * std::sin(t);
        float p[2];
        if (!ProjectLocal(b, r, 0.02f, back, p)) { have = false; continue; }
        if (have) {
            HoloFrame* f = b.out;
            if (f->segCount < HoloFrame::kMaxSegments) {
                HoloSegment& s = f->segs[f->segCount++];
                s.ax = prev[0]; s.ay = prev[1];
                s.bx = p[0]; s.by = p[1];
            }
        }
        prev[0] = p[0]; prev[1] = p[1];
        have = true;
    }
}

// The head, as a polygon in the frontal plane (it faces the camera the NPC faces).
void AddHead(const Build& b) {
    float prev[2];
    bool have = false;
    for (int i = 0; i <= kHeadSteps; ++i) {
        const float t = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kHeadSteps);
        float p[2];
        const float r = kHeadRadius * std::cos(t);
        const float u = kHeadCentre + kHeadRadius * std::sin(t);
        if (!ProjectLocal(b, r, u, 0.0f, p)) { have = false; continue; }
        if (have) {
            HoloFrame* f = b.out;
            if (f->segCount < HoloFrame::kMaxSegments) {
                HoloSegment& s = f->segs[f->segCount++];
                s.ax = prev[0]; s.ay = prev[1];
                s.bx = p[0]; s.by = p[1];
            }
        }
        prev[0] = p[0]; prev[1] = p[1];
        have = true;
    }
}

// The prop figure: a crate on the same spot. A prop has no silhouette the game tells us
// about (its mesh lives inside a bundle), so the preview draws the thing every prop has in
// common - a footprint, a height and a lid - rather than pretending to be the object. The
// label says which object it is; the box says how big the spot is.
constexpr float kCrateHalf = 0.35f;   // half the footprint, in metres
constexpr float kCrateTall = 0.70f;   // its height

void AddCrate(const Build& b) {
    const float h = kCrateHalf;
    // Corners, counter-clockwise from the near-bottom-left of the footprint.
    const float c[8][3] = {
        { -h, 0.0f, h }, { h, 0.0f, h }, { h, 0.0f, -h }, { -h, 0.0f, -h },
        { -h, kCrateTall, h }, { h, kCrateTall, h }, { h, kCrateTall, -h }, { -h, kCrateTall, -h },
    };
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        AddSegment(b, c[i][0], c[i][1], c[i][2], c[j][0], c[j][1], c[j][2]);          // base
        AddSegment(b, c[i + 4][0], c[i + 4][1], c[i + 4][2],
                   c[j + 4][0], c[j + 4][1], c[j + 4][2]);                              // lid
        AddSegment(b, c[i][0], c[i][1], c[i][2], c[i + 4][0], c[i + 4][1], c[i + 4][2]); // upright
    }
    // An arrow up the middle: which way is up reads at any distance, and it is the one line
    // that makes a box look placed rather than drawn on the screen.
    AddSegment(b, 0.0f, kCrateTall, 0.0f, 0.0f, kCrateTall + 0.6f, 0.0f);
}

void AddBody(const Build& b) {
    // shoulders and spine
    AddSegment(b, -kShoulderX, kShoulderY, 0.0f, kShoulderX, kShoulderY, 0.0f);
    AddSegment(b, 0.0f, kShoulderY, 0.0f, 0.0f, kHipY, 0.0f);
    // arms, both sides
    for (int side = -1; side <= 1; side += 2) {
        const float s = static_cast<float>(side);
        AddSegment(b, s * kShoulderX, kShoulderY, 0.0f, s * kElbowX, kElbowY, 0.0f);
        AddSegment(b, s * kElbowX, kElbowY, 0.0f, s * kHandX, kHandY, 0.0f);
    }
    // hips and legs
    AddSegment(b, -kHipX, kHipY, 0.0f, kHipX, kHipY, 0.0f);
    for (int side = -1; side <= 1; side += 2) {
        const float s = static_cast<float>(side);
        AddSegment(b, s * kHipX, kHipY, 0.0f, s * kKneeX, kKneeY, 0.0f);
        AddSegment(b, s * kKneeX, kKneeY, 0.0f, s * kKneeX, 0.0f, 0.0f);
    }
    // the neck, and a tick above the head so the height is legible at a distance
    AddSegment(b, 0.0f, kShoulderY, 0.0f, 0.0f, kHeadCentre - kHeadRadius, 0.0f);
    AddSegment(b, 0.0f, kHeadCentre + kHeadRadius, 0.0f, 0.0f, kHeight, 0.0f);
}

}  // namespace

bool HologramPreviewActive() { return g_on; }

bool HologramPropMode() { return g_propMode; }
void HologramSetPropMode(bool on) { g_propMode = on; }

void HologramSetPreview(bool on) {
    // Only the switch is written here: this can be called from the camera thread (F10) while
    // the render thread is building a frame, and a flag is not worth a lock. The note is the
    // builder's own, written and read on the render thread only.
    g_on = on;
}

float HologramDistance() { return g_metres; }

void HologramSetDistance(float metres) {
    if (!(metres > 0.0f)) metres = 0.0f;      // the player's own feet is a valid choice
    if (metres > 10.0f) metres = 10.0f;
    g_metres = metres;
}

const std::string& HologramNote() { return g_note; }

bool BuildHoloFrame(const std::string& model, float metres, bool crate, float screenW,
                    float screenH, HoloFrame* out) {
    if (!out) return false;
    auto fail = [out](const char* why) {
        out->visible = false;
        out->segCount = 0;
        std::snprintf(out->note, sizeof out->note, "%s", why);
        g_note = why;
        return false;
    };

    out->visible = false;
    out->segCount = 0;
    out->ppm = 0.0f;
    out->distance = 0.0f;

    fc_mat view, proj;
    int transposed = 0;
    if (!FreeCamCameraMatrices(&view, &proj, &transposed))
        return fail("spawn preview: waiting for the camera");
    if (transposed) {
        fc_mat t;
        fc_mat_transpose(&view, &t); view = t;
        fc_mat_transpose(&proj, &t); proj = t;
    }

    float mat[16];
    if (!SpawnSpot(metres, mat))
        return fail("spawn preview: waiting for you to be in the world");

    Build b{ &view, &proj, mat, screenW, screenH, out };

    // Where the feet land, and how many pixels a metre is worth there. The scale comes
    // from the projection rather than from a lens formula, so it is right whatever the
    // game's field of view happens to be.
    float feet[2], oneUp[2];
    if (!ProjectLocal(b, 0.0f, 0.0f, 0.0f, feet))
        return fail("spawn preview: the spot is behind the camera");
    if (ProjectLocal(b, 0.0f, 1.0f, 0.0f, oneUp)) {
        const float dx = oneUp[0] - feet[0], dy = oneUp[1] - feet[1];
        out->ppm = std::sqrt(dx * dx + dy * dy);
    }
    if (!(out->ppm > 0.01f))
        return fail("spawn preview: the projection does not scale");

    out->feetX = feet[0];
    out->feetY = feet[1];

    float head[2];
    if (ProjectLocal(b, 0.0f, kHeight, 0.0f, head)) {
        out->headX = head[0];
        out->headY = head[1];
    } else {
        out->headX = feet[0];
        out->headY = feet[1] - kHeight * out->ppm;
    }

    // Distance from the camera along the view axis, for the label. fc_project reports it;
    // asking for it once here keeps the figure's own projections cheap.
    {
        float w[3], depth = 0.0f;
        ToWorld(mat, 0.0f, 0.0f, 0.0f, w);
        if (fc_project(&view, &proj, w, screenW, screenH, feet, &depth)) out->distance = depth;
    }

    AddGroundRing(b);
    if (crate) {
        AddCrate(b);
    } else {
        AddBody(b);
        AddHead(b);
    }

    out->height = crate ? kCrateTall : kHeight;
    const std::string shown = model.empty() ? std::string("(no model named)") : model;
    std::snprintf(out->model, sizeof out->model, "%s",
                  (crate ? PropShortName(shown) : SpawnShortName(shown)).c_str());
    // No note on success: the overlay writes the figure's own label from `model` and the
    // distance it was built at, and a second line of text would only repeat it.
    out->note[0] = 0;
    g_note.clear();

    out->visible = out->segCount > 0;
    if (!out->visible) {
        // Every point was behind the camera even though the feet were not - the spot is
        // right at the edge of the view.
        std::snprintf(out->note, sizeof out->note, "spawn preview: out of view");
        g_note = out->note;
    }
    return out->visible;
}

bool HologramBuildFrame(const std::string& model, float screenW, float screenH,
                        HoloFrame* out) {
    return BuildHoloFrame(model, g_metres, false, screenW, screenH, out);
}

bool HologramBuildPropFrame(const std::string& model, float screenW, float screenH,
                            HoloFrame* out) {
    return BuildHoloFrame(model, g_metres, true, screenW, screenH, out);
}

}  // namespace game
}  // namespace tw
