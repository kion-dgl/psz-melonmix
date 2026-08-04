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

// PSZMix, not PSZ. The Windows SDK's minwindef.h has `typedef char *PSZ;` --
// a legacy "pointer to zero-terminated string" alias -- so a namespace called
// PSZ redeclares it as a different kind of entity and every Windows build dies
// in the system headers. Found by CI on the first green Windows bootstrap.
namespace PSZMix
{
using namespace melonDS;

// Where an element is anchored on the top screen.
// Corner anchors for ported elements. The last four exist for the PSO-style
// menu, which spreads a menu's own panels to the screen edges instead of
// presenting the whole bottom screen -- so the field stays visible between them.
enum Corner
{
    Corner_TopLeft, Corner_TopRight, Corner_BottomLeft, Corner_BottomRight,
    Corner_LeftCentre, Corner_RightTop, Corner_RightBottom, Corner_TopCentre
};

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

    // The title keeps its logo: these pixels of the TOP screen are drawn back
    // over the presented bottom screen, at the same place they already occupy.
    bool keepTop = false;
    float logoAlpha = 1.0f;     // 0..1, ramped so the logo joins and leaves
                                // with the game's own fades
    int ktx = 0, kty = 0, ktw = 0, kth = 0;

    // Player stats, read from RAM so the panel can be DRAWN rather than clipped.
    // Valid only while inGame; panel stays false everywhere else.
    bool panel = false;
    int hp = 0, maxHp = 0, pp = 0, maxPp = 0, level = 0;

    // CUTS: a source rect on the bottom screen with an EXPLICIT destination,
    // as a fraction of the top screen. Elements anchor to a corner; these do
    // not, because they land inside artwork we draw -- the palette's three
    // action icons sit in the slots of our own frame.
    //
    // They stay cuts rather than becoming art because WHICH action is in a slot
    // is still unknown (psz-re melonmix-questions.md Q2). Cutting the icon
    // sidesteps that entirely: the game already drew the right one.
    struct Cut { int sx, sy, sw, sh; float dx, dy, dw, dh; float alpha; };
    Cut cuts[8];
    int cutCount = 0;

    // Where the palette frame art goes, as fractions of the top screen.
    bool palette = false;
    float px = 0, py = 0, pw = 0, ph = 0;

    // The contextual info box's own text, read from the game's UTF-16 buffer
    // and folded to ASCII so we can render it in our font. Empty when unknown.
    char info[48] = {0};

    // Character create (overlay 11). screen: 0 none, 1 race, 2 class, 3 appearance.
    int ccScreen = 0;
    int ccRace = -1;        // 0..2
    int ccClass = -1;       // global code 0..13, -1 when unchosen
    int ccOpt[6] = { -1, -1, -1, -1, -1, -1 };   // appearance option indices
    int ccCursor = -1;      // which row the player is on, 0..6

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

// Redirect the first multiplayer quest to "The Eternal".
//
// PSZ ships The Eternal as a SINGLE-player quest, so making it playable in
// multiplayer needs no new content -- only for the multiplayer slot to read the
// single-player file. The community patcher does this by rewriting the ROM on
// disk; this does the same thing to the in-memory image at load, so the file on
// disk is never touched and it can be a toggle.
//
// It rewrites one 8-byte FAT entry, not the data: the multiplayer quest's entry
// is pointed at the single-player quest's offset and length, so the game reads
// the whole of the right file rather than a truncated or padded copy.
//
// Returns true if it patched. Verifies the gamecode and both file sizes first
// and does nothing on a mismatch -- a wrong offset here corrupts a quest.
bool PatchEternalMultiplayer(u8* rom, u32 romlen);

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

// Render everything that comes from OUR OWN art -- the title logo, the drawn
// player panel -- into a transparent 256x192 RGBA layer. Straight alpha.
//
// This exists so the GL frontend does not reimplement the drawing in shaders.
// None of it needs game pixels, so a GL path can render this once on the CPU
// and upload it, and both renderers then produce the same image by
// construction rather than by two implementations agreeing.
//
// Returns false when there is nothing to draw, so the caller can skip upload.
bool RenderArtLayer(u32* out256x192, const Frame& f);

// Tell the core whether the contextual info box currently has text in it.
// Only needed by frontends whose frames live on the GPU, where the core cannot
// read the pixels itself.
void SetBoxHasTextHint(bool hasText);

// Right-stick camera control. x is -1..1; call once per frame from the
// frontend, 0 when the stick is centred or absent. The DS has no right stick,
// so this is input the emulated machine never had -- it steers the camera by
// writing the game's own target yaw.
void SetCameraStick(float x);

// Left-stick direction, -1..1 each. The DS has no analog stick, so the
// frontend maps this onto d-pad bits and the game only ever sees 4 or 8
// directions. Passing the true vector lets the overlay correct the FACING the
// game derived from those bits, which is where the resolution is lost.
void SetMoveStick(float x, float y);

// Where an element lands on the top screen, as a fraction of it. Normalised so
// that the DS-resolution compositor, the Qt window-pixel overlay and the GL
// texture path can all scale ONE layout definition into their own space --
// three hand-written copies of the same corner arithmetic is how they drift.
struct Place { float x, y, w, h; };
Place PlaceElement(const Element& e, float hudScale);

// Element size multiplier. 1.0 is DS-native; the elements are cut at DS
// resolution and looked oversized on a phone-sized top screen.
float HudScale();

}

#endif // PSZPLUGIN_H
