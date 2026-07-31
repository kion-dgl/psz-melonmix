/*
    Phantasy Star Zero plugin -- psz-melonmix. See PSZPlugin.h for the split.

    Addresses are psz-re's, each checked statically against every savestate in
    psz-melonmix sandbox directories before it was used:

      *(0x02108D04)           player object; NULL in every mode that is not the
                              main game -- shops, quest counter, title, file
                              select, character create
      player + 0x280          control mode: 1 normal, 5 full-screen menu open
      *(*(0x02108C64))        room table base; count at +0x410, records 0x34
                              bytes, cell at +0x2E/+0x2F, exits +0x18..+0x1B,
                              gate types +0x1C..+0x1F, keys +0x2C
      0x020346E0              3D projection aspect term; 0x1555 -> 0x1C71 is 16:9
*/

#include "PSZPlugin.h"
#include "NDS.h"
#include "GPU.h"
#include "PSZOverlayIDs.h"
#include "PSZCheats.h"

#include <cstdlib>
#include <cstdio>

namespace PSZMix
{

static constexpr u32 PlayerPtr = 0x02108D04;
static constexpr u32 RoomRoot  = 0x02108C64;
static constexpr u32 AspectVal = 0x020346E0;

// Source rects on the bottom screen, measured from a field capture and checked
// again in town. Overridable so they can be retuned without a rebuild.
struct RectDef { const char* env; int x, y, w, h; Corner corner; };
static const RectDef Rects[] = {
    { "PSZ_HUD_PANELRECT",   2,   4, 124, 50, Corner_TopLeft     },  // level / HP / PP
    { "PSZ_HUD_MAPRECT",   143, 109,  70, 80, Corner_TopRight    },  // room map + key row
    { "PSZ_HUD_TGTRECT",     2,  96, 124, 56, Corner_BottomLeft  },  // locked-on target
    { "PSZ_HUD_PALRECT",   128,   6, 124, 54, Corner_BottomRight },  // action palette
};
static constexpr int NumRects = sizeof(Rects) / sizeof(Rects[0]);

static bool EnvSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

static u32 Read(NDS* nds, u32 addr, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u32)nds->MainRAM[(addr + i) & nds->MainRAMMask] << (8 * i);
    return v;
}

static bool InMainRAM(NDS* nds, u32 addr)
{
    return addr >= 0x02000000 && addr <= 0x02000000 + nds->MainRAMMask;
}

// BAKED-IN CHEATS.
//
// This build is a wrapper around one game, so the codes that make that game
// behave the way we want ship with it rather than being something the player has
// to find, import and enable. They are applied per frame exactly as melonDS's
// own cheat engine would, so no cheat file, no UI and no config are involved.
//
// The interpreter covers the opcodes this game's codes actually use, which is a
// small set: 0 = write u32, 1 = write u16, 2 = write u8, 9 = run the following
// codes only while a masked u16 matches, D2 = end conditional. Anything else is
// ignored rather than guessed at -- a misinterpreted code writes to an arbitrary
// address, and silently corrupting emulated RAM is far worse than a cheat that
// does not fire.
//
// Toggle individually with PSZ_CHEAT_<NAME>=0/1.
struct Cheat
{
    const char* name;
    const char* env;
    bool on;                // default
    const u32* codes;
    int words;              // total u32s (pairs * 2)
};

// 16:9 widescreen, from DeadSkullzJr's NDS(i) Cheat Database. 0x1C71/0x1555 is
// 7281/5461 = 1.3333, exactly 16:9 over 4:3, so the value is the 3D projection's
// aspect term rather than anything to do with the display.
static const u32 kWidescreen[] = {
    0x920346E0, 0x00001555,
    0x120346E0, 0x00001C71,
    0xD2000000, 0x00000000,
};

// TWO CLASSES, and the split is deliberate.
//
// QUALITY OF LIFE is on by default: it is what makes this build pleasant rather
// than what makes the game easier. Widescreen belongs here.
//
// OPT-IN is off by default and lives in PSZCheats.h, generated from the cheat
// database. These skip grind and change the game, so nobody gets them without
// asking. A player who wants to pick PSZ up again and just have fun can turn
// them on; a player who wants PSZ gets PSZ.
static const Cheat kCheats[] = {
    { "16:9 widescreen", "PSZ_CHEAT_WIDESCREEN", true, kWidescreen, 6 },
};
static constexpr int NumCheats = sizeof(kCheats) / sizeof(kCheats[0]);

static void Write(NDS* nds, u32 addr, u32 val, int bytes)
{
    for (int i = 0; i < bytes; i++)
        nds->MainRAM[(addr + i) & nds->MainRAMMask] = (u8)(val >> (8 * i));
}

static void ApplyCheat(NDS* nds, const Cheat& c)
{
    bool enabled = c.on;
    if (const char* v = std::getenv(c.env)) enabled = (*v && *v != '0');
    if (!enabled) return;

    bool active = true;      // inside a conditional that currently matches
    for (int i = 0; i + 1 < c.words; i += 2)
    {
        const u32 a = c.codes[i], v = c.codes[i + 1];
        switch (a >> 28)
        {
        case 0x0: if (active) Write(nds, a & 0x0FFFFFFF, v, 4); break;
        case 0x1: if (active) Write(nds, a & 0x0FFFFFFF, v, 2); break;
        case 0x2: if (active) Write(nds, a & 0x0FFFFFFF, v, 1); break;
        case 0x9:
        {
            // 9AAAAAAA XXXXYYYY -- run what follows only while
            // (u16 at A) & ~XXXX == YYYY.
            const u32 addr = a & 0x0FFFFFFF;
            const u16 cur = (u16)(nds->MainRAM[addr & nds->MainRAMMask] |
                                 (nds->MainRAM[(addr + 1) & nds->MainRAMMask] << 8));
            active = ((cur & ~(v >> 16)) == (v & 0xFFFF));
            break;
        }
        case 0xD: active = true; break;   // D2000000: end conditional
        default: break;                   // unrecognised: skip, never guess
        }
    }
}

// WHICH OVERLAY IS RESIDENT -- the game's own notion of its mode.
//
// ov00..ov18 all load at 0x0211E640, one mutually-exclusive slot, so exactly one
// is resident and which one it is IS the mode (psz-re docs/game-state.md).
// Recognised by CRC32 of its first 512 bytes against a generated table, so no
// overlay image and no game data ships.
//
// This replaces two failed pixel heuristics. Colour variety called a cutscene
// dense because its bottom screen is a large pastel panel; dark-text density
// then hid the "create this character?" confirmation because that prompt is
// small on a white panel. Both were guessing at the mode from what came out of
// the renderer. This reads the mode itself.
static u32 Crc32(const u8* p, int n)
{
    u32 c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (~(c & 1) + 1));
    }
    return ~c;
}

constexpr u32 OverlaySlot = 0x0211E640;

static int ResidentOverlay(NDS* nds)
{
    u8 head[512];
    for (int i = 0; i < 512; i++)
        head[i] = nds->MainRAM[(OverlaySlot + i) & nds->MainRAMMask];

    const u32 crc = Crc32(head, 512);
    for (int i = 0; i < kNumOverlayFingerprints; i++)
        if (kOverlayFingerprints[i].crc == crc)
            return kOverlayFingerprints[i].id;

    // No match is a TRANSIENT, not a mode: while an overlay is being DMA'd into
    // the slot those bytes are a mix of two images. Callers hold the last known
    // id rather than treating this as a state.
    return -1;
}

// Does this overlay want its bottom screen presented?
//
// Measured, not assumed. ov16 is the cutscene player and ov17 the dialogue
// player -- captured savestates put the opening cutscene in ov16 and the Kai
// conversation in ov17, both with a NULL player object. In those the top screen
// carries the content and the bottom holds only a skip prompt.
static bool OverlayWantsBottomScreen(int ov)
{
    switch (ov)
    {
    case 16:   // cutscene player
    case 17:   // dialogue player
    case 0:    // title -- the logo is the top screen
    case 6:    // ending / credits
        return false;
    default:
        return true;   // file select, create, counter/shop, and anything unmapped
    }
}

static void ApplyCheats(NDS* nds)
{
    for (int i = 0; i < NumCheats; i++)
        ApplyCheat(nds, kCheats[i]);

    // Opt-in, all default false.
    for (int i = 0; i < kNumGeneratedCheats; i++)
    {
        const GeneratedCheat& g = kGeneratedCheats[i];
        Cheat c { g.name, g.env, g.defaultOn, g.codes, g.words };
        ApplyCheat(nds, c);
    }
}

// SELECT toggles the area map. Chosen by measurement: idle, SELECT moved 78
// pixels of the bottom-screen HUD against an idle drift of 90 -- nothing above
// noise -- while X moved 227 and is therefore probably bound to something.
// KeyInput is active-low and packs KEYINPUT in its low half; bit 2 is SELECT.
// This only READS the key, so the game still receives it either way.
static bool ServiceAreaMapToggle(NDS* nds)
{
    static bool shown = false, prev = false;
    bool held = !(nds->KeyInput & (1 << 2));
    if (held && !prev) shown = !shown;
    prev = held;
    return shown || EnvSet("PSZ_HUD_AREAMAP");
}

static void ReadRooms(NDS* nds, Frame& f)
{
    u32 root = Read(nds, RoomRoot, 4);
    if (!InMainRAM(nds, root)) return;
    u32 base = Read(nds, root, 4);
    if (!InMainRAM(nds, base)) return;

    // pszd.py's own bound. A stale base yields a wild count, and drawing a map
    // from one is worse than drawing none.
    u32 count = Read(nds, base + 0x410, 4);
    if (count == 0 || count > (u32)MaxRooms) return;

    f.roomCount = (int)count;
    for (u32 i = 0; i < count; i++)
    {
        u32 rec = base + i * 0x34;
        Room& r = f.rooms[i];
        r.cx = (u8)Read(nds, rec + 0x2E, 1);
        r.cy = (u8)Read(nds, rec + 0x2F, 1);
        r.keys = (u8)Read(nds, rec + 0x2C, 1);
        for (int k = 0; k < 4; k++)
        {
            r.exits[k] = (u8)Read(nds, rec + 0x18 + k, 1);
            r.gates[k] = (u8)Read(nds, rec + 0x1C + k, 1);
        }
    }
}

Frame Update(NDS* nds)
{
    Frame f;
    if (!nds || !nds->MainRAM) return f;

    ApplyCheats(nds);
    f.areaMap = ServiceAreaMapToggle(nds);

    // The player object pointer gates everything: it is NULL in every mode that
    // is not the main game, so a null base already means "the bottom screen is
    // the interaction" without needing to identify which mode it is.
    u32 base = Read(nds, PlayerPtr, 4);
    bool inGame = InMainRAM(nds, base) && base != 0;

    if (!inGame || Read(nds, base + 0x280, 4) == 5)
    {
        // Hold the last known overlay across the DMA transient, so a mode never
        // flickers while it is being swapped in.
        static int lastOverlay = -1;
        const int ov = ResidentOverlay(nds);
        if (ov >= 0) lastOverlay = ov;

        // Cutscene, dialogue, title, ending: the top screen is the content.
        // Draw nothing and let it through untouched.
        if (lastOverlay >= 0 && !OverlayWantsBottomScreen(lastOverlay))
            return f;                                  // active stays false

        f.active = true;
        f.modal = true;
        return f;
    }

    for (int i = 0; i < NumRects; i++)
    {
        const RectDef& d = Rects[i];
        Element e { d.x, d.y, d.w, d.h, d.corner };
        if (const char* o = std::getenv(d.env))
        {
            int x, y, w, h;
            if (std::sscanf(o, "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
            { e.sx = x; e.sy = y; e.sw = w; e.sh = h; }
        }
        if (e.sw <= 0 || e.sh <= 0 || e.sx < 0 || e.sy < 0 ||
            e.sx + e.sw > 256 || e.sy + e.sh > 192)
            continue;
        f.elems[f.count++] = e;
    }

    ReadRooms(nds, f);
    f.active = true;
    return f;
}

}

namespace PSZMix
{

static void Blit(u32* dst, const u32* src, int sx, int sy, int sw, int sh,
                 int dx, int dy)
{
    for (int y = 0; y < sh; y++)
    {
        int ty = dy + y;
        if (ty < 0 || ty >= 192) continue;
        for (int x = 0; x < sw; x++)
        {
            int tx = dx + x;
            if (tx < 0 || tx >= 256) continue;
            dst[ty * 256 + tx] = src[(sy + y) * 256 + (sx + x)];
        }
    }
}

void Composite(u32* topFB, const u32* bottomFB, const Frame& f)
{
    if (!topFB || !bottomFB || !f.active) return;

    // Modal: the bottom screen IS the interaction, and at DS resolution the two
    // screens are the same size, so presenting it is a straight copy.
    if (f.modal)
    {
        for (int i = 0; i < 256 * 192; i++) topFB[i] = bottomFB[i];
        return;
    }

    const int m = 2;
    for (int i = 0; i < f.count; i++)
    {
        const Element& e = f.elems[i];
        int dx = (e.corner == Corner_TopLeft || e.corner == Corner_BottomLeft)
                 ? m : 256 - m - e.sw;
        int dy = (e.corner == Corner_TopLeft || e.corner == Corner_TopRight)
                 ? m : 192 - m - e.sh;
        Blit(topFB, bottomFB, e.sx, e.sy, e.sw, e.sh, dx, dy);
    }
}

}
