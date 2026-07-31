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

// 16:9, from the cheat DB's "16:9 Widescreen": if the u16 is 0x1555 write
// 0x1C71. 7281/5461 is 1.3333, exactly 16:9 over 4:3, so this is the projection
// term rather than anything about the display. Applied per frame as the AR code
// would be, so no cheat-engine wiring is needed.
static void ApplyWidescreen(NDS* nds)
{
    // On by default: this is a PSZ-specific build and the 16:9 projection is
    // the point of it. PSZ_WIDESCREEN=0 turns it off.
    static const bool on = (std::getenv("PSZ_WIDESCREEN") == nullptr) || EnvSet("PSZ_WIDESCREEN");
    if (!on) return;
    u32 o = AspectVal & nds->MainRAMMask;
    if ((u16)(nds->MainRAM[o] | (nds->MainRAM[o + 1] << 8)) != 0x1555) return;
    nds->MainRAM[o]     = 0x71;
    nds->MainRAM[o + 1] = 0x1C;
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

    ApplyWidescreen(nds);
    f.areaMap = ServiceAreaMapToggle(nds);

    // The player object pointer gates everything: it is NULL in every mode that
    // is not the main game, so a null base already means "the bottom screen is
    // the interaction" without needing to identify which mode it is.
    u32 base = Read(nds, PlayerPtr, 4);
    bool inGame = InMainRAM(nds, base) && base != 0;

    if (!inGame || Read(nds, base + 0x280, 4) == 5)
    {
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
