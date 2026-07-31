/*
    Phantasy Star Zero plugin -- psz-melonmix.

    Everything the single-screen overlay knows about PSZ lives here, in the CORE,
    so that every frontend can share it. The frontend is left with drawing and
    nothing else.

    The split is deliberate and is NOT "composite in the core". Compositing into
    the 256x192 framebuffer would lock every HUD element to DS pixel scale, and a
    70px panel is 27% of a 256px screen at any output resolution -- which is
    exactly the crowding that anchoring the overlay in window pixels fixed. So
    the core decides WHAT to show and WHERE it comes from; the frontend draws it
    at its own output resolution and keeps that fix.

    Porting to another frontend (melonDS-android) therefore means implementing
    the drawing, not re-deriving any of the game knowledge below.
*/

#ifndef PSZPLUGIN_H
#define PSZPLUGIN_H

#include "types.h"

namespace melonDS { class NDS; }

namespace PSZ
{
using namespace melonDS;

// Where an element is anchored on the top screen.
enum Corner { Corner_TopLeft, Corner_TopRight, Corner_BottomLeft, Corner_BottomRight };

// A piece of the bottom screen to move to the top one. Source rect is in
// bottom-screen pixels (256x192).
struct Element
{
    int sx, sy, sw, sh;
    Corner corner;
};

struct Room
{
    u8 cx, cy;          // grid cell
    u8 exits[4];        // N E S W, 0xFF = none
    u8 gates[4];        // 0 = open, else keyed / enemy-gated
    u8 keys;
};

constexpr int MaxRooms = 20;
constexpr int MaxElements = 8;

struct Frame
{
    bool active = false;        // anything to draw at all
    bool modal  = false;        // present the WHOLE bottom screen instead of corners

    Element elems[MaxElements];
    int count = 0;

    bool areaMap = false;       // the centred translucent grid is showing
    int roomCount = 0;
    Room rooms[MaxRooms];

    // -1 = unknown. +0x414 was the obvious candidate and is REFUTED: in the
    // 8-room Gurhacia state it reads 6 (cell D2, a dead end) while the game's
    // own map puts the player in B4, room 0. Nothing else has been found, and
    // every savestate on disk has the player in room 0, so no state on hand can
    // distinguish "current room" from the constant zero.
    int curRoom = -1;
};

// Call once per rendered frame. Applies the widescreen poke and services the
// SELECT toggle, then returns what the frontend should draw.
Frame Update(NDS* nds);

// Reference compositor at DS resolution: writes the overlay straight into the
// TOP framebuffer, reading from the bottom one. Both are 256x192 BGRA.
//
// For frontends that only ever see the raw framebuffers -- melonDS-android
// uploads them to a GL texture as-is -- this is the whole integration. It is
// NOT what the Qt frontend uses, because at DS resolution a 70px element is 27%
// of the screen at any output size; a frontend that knows its window size gets a
// better result drawing the same Frame itself. Both are legitimate, and this one
// is what makes a handheld build possible today.
void Composite(u32* topFB, const u32* bottomFB, const Frame& f);

}

#endif // PSZPLUGIN_H
