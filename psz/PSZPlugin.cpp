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
#include "PSZArt.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace PSZMix
{

// PLAYER STATS, for the panels we draw ourselves rather than clip.
//
// From psz-re data/hud_memory_map.json, re-verified here against 13 savestates:
// levels 1..50 all plausible, EXP rises with level, and the one uncheated state
// reads HP 82/82 PP 67/67 at level 1 while every cheated save reads 9999/999.
//
// Level is in the SAVE-WORK object, not the player object -- reading +0x92 off
// the player pointer gives 538, which is what made an earlier pass here
// conclude, wrongly, that the offsets did not exist.
static constexpr u32 SaveWorkPtr = 0x0211A530;   // -> save-work; +0x92 u16 level
static constexpr u32 CurHpAddr   = 0x021A21FE;
static constexpr u32 CurPpAddr   = 0x021A2200;
static constexpr u32 MaxHpAddr   = 0x021A210C;
static constexpr u32 MaxPpAddr   = 0x021A210E;

static constexpr u32 PlayerPtr = 0x02108D04;
static constexpr u32 RoomRoot  = 0x02108C64;
static constexpr u32 AspectVal = 0x020346E0;

// Source rects on the bottom screen, measured from a field capture and checked
// again in town. Overridable so they can be retuned without a rebuild.
struct RectDef { const char* env; int x, y, w, h; Corner corner; };

// PSO-STYLE MENU.
//
// The default modal presentation blows the whole bottom screen up in the middle
// of the screen. It is readable, but it is still a screen-shaped thing sitting
// on top of the game. PSO instead puts the menu's parts at the edges and leaves
// the world visible between them.
//
// These are the main menu's own panels, measured at native resolution: the
// command list, the description box, the stats panel, the heading and the
// player strip. Spreading them is why Corner gained edge anchors.
//
// MAIN MENU ONLY. Nothing yet identifies WHICH menu is open, so the item menu
// and the Mag screen would be sliced by rects that do not describe them. Opt in
// with PSZ_MENU_PSO=1 until that is solved.
static const RectDef MenuRects[] = {
    { "PSZ_MENU_LIST",   4, 36,  80, 136, Corner_LeftCentre  },
    { "PSZ_MENU_DESC",  94, 30, 160,  36, Corner_RightTop    },
    { "PSZ_MENU_STATS", 92, 76, 164,  92, Corner_RightBottom },
    { "PSZ_MENU_TITLE",128,  2, 112,  18, Corner_TopCentre   },
    { "PSZ_MENU_WHO",    2,  2,  86,  26, Corner_TopLeft     },
};
static constexpr int NumMenuRects = sizeof(MenuRects) / sizeof(MenuRects[0]);
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

// WHICH SCREEN IS THE ONE TO SHOW.
//
// Most of PSZ uses one screen for content and the other for decoration, so
// simply presenting the right one is most of what this build has to do -- far
// more of the experience than any HUD drawing. The ids come from psz-re
// docs/game-state.md, which names eight of the nineteen slot overlays.
//
// ov16/ov17: psz-re names these "intro / boot logo" and "pre-title attract
// cutscene". This project's own captures instead put the opening cutscene in
// ov16 and the Kai conversation in ov17, both with a NULL player object. Those
// two accounts DISAGREE and neither has been retested since. It does not change
// the policy -- every reading of both wants the top screen -- so the conflict is
// recorded rather than resolved by picking a favourite.
static bool OverlayWantsBottomScreen(int ov)
{
    switch (ov)
    {
    case 16:   // cutscene / intro
    case 17:   // dialogue / attract
    case 6:    // ending / credits
        return false;
    case 11:   // character create -- the top screen is the character preview,
               // the bottom is the sliders. The preview is the thing worth
               // seeing; our own sliders replace the bottom screen later.
        return false;
    default:
        return true;   // title, file select, counter/shop, anything unmapped
    }
}

// Does this overlay keep a slice of the TOP screen over the presented bottom?
//
// The title is the one screen that genuinely needs both: the logo lives on the
// top screen and PRESS START on the bottom. Presenting only the bottom loses the
// logo, which is the whole identity of the screen.
//
// Clipped from the running game rather than shipped as an image: psz-godot has
// a clean logo.png with alpha, but loading it needs an image decoder and an
// asset path this build does not have yet. The clip works today and costs
// nothing. PSZ_TITLE_LOGORECT retunes it without a rebuild.
static bool OverlayKeepsTopSlice(int ov) { return ov == 0; }

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

        // Cutscene, dialogue, character create, ending: the top screen is the
        // content. Draw nothing and let it through untouched.
        if (lastOverlay >= 0 && !OverlayWantsBottomScreen(lastOverlay))
            return f;                                  // active stays false

        // The title needs both screens: PRESS START is on the bottom, the logo
        // on the top. Presenting the bottom alone throws the logo away.
        if (lastOverlay >= 0 && OverlayKeepsTopSlice(lastOverlay))
        {
            // Estimated from a device capture of the title, not measured
            // against the framebuffer -- retune with PSZ_TITLE_LOGORECT.
            int x = 16, y = 68, w = 224, h = 56;
            if (const char* o = std::getenv("PSZ_TITLE_LOGORECT"))
                std::sscanf(o, "%d,%d,%d,%d", &x, &y, &w, &h);
            if (w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= 256 && y + h <= 192)
            {
                f.keepTop = true;
                f.ktx = x; f.kty = y; f.ktw = w; f.kth = h;
            }
        }

        // PSO-style: spread the main menu's own panels to the edges rather than
        // presenting the whole bottom screen. Only inside the main game, where
        // control mode 5 means a full-screen menu is up -- the pre-game screens
        // are laid out differently and are left alone.
        if (inGame && EnvSet("PSZ_MENU_PSO"))
        {
            for (int i = 0; i < NumMenuRects; i++)
            {
                const RectDef& d = MenuRects[i];
                Element e { d.x, d.y, d.w, d.h, d.corner };
                if (const char* o = std::getenv(d.env))
                {
                    int x, y, w, h;
                    if (std::sscanf(o, "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
                    { e.sx = x; e.sy = y; e.sw = w; e.sh = h; }
                }
                f.elems[f.count++] = e;
            }
            f.active = true;
            return f;                                  // modal stays false
        }

        f.active = true;
        f.modal = true;
        return f;
    }

    // Player stats for the drawn panel. Addresses from psz-re
    // data/hud_memory_map.json, re-verified against 13 savestates -- see the
    // constants at the top of this file. Level is in the SAVE-WORK object, not
    // the player object.
    {
        const u32 sw = Read(nds, SaveWorkPtr, 4);
        f.hp    = (int)Read(nds, CurHpAddr, 2);
        f.maxHp = (int)Read(nds, MaxHpAddr, 2);
        f.pp    = (int)Read(nds, CurPpAddr, 2);
        f.maxPp = (int)Read(nds, MaxPpAddr, 2);
        f.level = InMainRAM(nds, sw) ? (int)Read(nds, sw + 0x92, 2) : 0;

        // Only claim the panel when the values are self-consistent. A zero max
        // or an out-of-range level means we are reading a stale or half-built
        // block, and drawing 0/0 over the game's own working panel is worse
        // than leaving the clip in place.
        f.panel = f.maxHp > 0 && f.maxPp > 0 && f.hp <= f.maxHp &&
                  f.pp <= f.maxPp && f.level > 0 && f.level <= 200 &&
                  !EnvSet("PSZ_HUD_NOART");
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

float HudScale()
{
    static const float s = [] {
        const char* v = getenv("PSZ_HUD_ELEMENT_SCALE");
        if (!v) return 1.0f;
        float f = (float)atof(v);
        return (f > 0.05f && f < 8.0f) ? f : 1.0f;
    }();
    return s;
}

Place PlaceElement(const Element& e, float hudScale)
{
    const float mx = 2.0f / 256.0f, my = 2.0f / 192.0f;
    const float w = (e.sw / 256.0f) * hudScale;
    const float h = (e.sh / 192.0f) * hudScale;
    float x = mx, y = my;
    switch (e.corner)
    {
    case Corner_TopLeft:     x = mx;              y = my;              break;
    case Corner_TopRight:    x = 1.0f - mx - w;   y = my;              break;
    case Corner_BottomLeft:  x = mx;              y = 1.0f - my - h;   break;
    case Corner_BottomRight: x = 1.0f - mx - w;   y = 1.0f - my - h;   break;
    case Corner_LeftCentre:  x = mx;              y = 0.5f - h * 0.5f; break;
    case Corner_RightTop:    x = 1.0f - mx - w;   y = my;              break;
    case Corner_RightBottom: x = 1.0f - mx - w;   y = 1.0f - my - h;   break;
    case Corner_TopCentre:   x = 0.5f - w * 0.5f; y = my;              break;
    }
    return { x, y, w, h };
}

// ART DRAWING.
//
// Decoded once and cached: the RLE in PSZArt.h exists to keep the header small,
// not to be walked every frame.
static const u32* DecodeArt(const PSZArtImage& img)
{
    static const PSZArtImage* cachedFor[4] = {};
    static u32* cached[4] = {};
    for (int i = 0; i < 4; i++)
        if (cachedFor[i] == &img) return cached[i];

    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; i++) if (!cachedFor[i]) slot = i;
    if (slot < 0) return nullptr;

    u32* buf = new u32[img.w * img.h];
    u32* w = buf;
    const u32* end = buf + img.w * img.h;
    for (unsigned int i = 0; i + 4 < img.rleLen; i += 5)
    {
        const unsigned char n = img.rle[i];
        // Framebuffer bytes are R,G,B,A -- the same order the PNG decodes to,
        // so this is a straight pack with no channel swap.
        const u32 px = (u32)img.rle[i + 1] | ((u32)img.rle[i + 2] << 8) |
                       ((u32)img.rle[i + 3] << 16) | ((u32)img.rle[i + 4] << 24);
        for (int k = 0; k < n && w < end; k++) *w++ = px;
    }
    cachedFor[slot] = &img;
    cached[slot] = buf;
    return buf;
}

// src-over, nearest-neighbour scaled. This is the whole point of the artwork:
// the panels are not rectangles, so anything that ignores alpha drags the
// background in around their edges.
static void BlitArt(u32* dst, const PSZArtImage& img,
                    int dx, int dy, int dw, int dh)
{
    const u32* src = DecodeArt(img);
    if (!src || dw <= 0 || dh <= 0) return;

    for (int y = 0; y < dh; y++)
    {
        const int ty = dy + y;
        if (ty < 0 || ty >= 192) continue;
        const int sy = y * img.h / dh;
        for (int x = 0; x < dw; x++)
        {
            const int tx = dx + x;
            if (tx < 0 || tx >= 256) continue;
            const u32 s = src[sy * img.w + (x * img.w / dw)];
            const u32 a = s >> 24;
            if (!a) continue;
            if (a == 255) { dst[ty * 256 + tx] = s; continue; }

            const u32 d = dst[ty * 256 + tx];
            u32 out = 0xFF000000u;
            for (int c = 0; c < 3; c++)
            {
                const u32 sc = (s >> (8 * c)) & 0xFF, dc = (d >> (8 * c)) & 0xFF;
                out |= (((sc * a + dc * (255 - a)) / 255) & 0xFF) << (8 * c);
            }
            dst[ty * 256 + tx] = out;
        }
    }
}

// Digits, right-aligned at (rx, y). Ours, not the game's -- see PSZArt.h.
static int DrawNumber(u32* dst, int value, int rx, int y, u32 rgb)
{
    char buf[12];
    int n = std::snprintf(buf, sizeof(buf), "%d", value);
    if (n <= 0) return 0;

    int x = rx - n * (kGlyphW + 1);
    for (int i = 0; i < n; i++)
    {
        const char* p = std::strchr(kGlyphChars, buf[i]);
        if (p)
        {
            const unsigned char* g = kGlyphs[p - kGlyphChars];
            for (int gy = 0; gy < kGlyphH; gy++)
            {
                const int ty = y + gy;
                if (ty < 0 || ty >= 192) continue;
                for (int gx = 0; gx < kGlyphW; gx++)
                {
                    const int tx = x + gx;
                    if (tx < 0 || tx >= 256 || !g[gy * kGlyphW + gx]) continue;
                    dst[ty * 256 + tx] = 0xFF000000u | rgb;
                }
            }
        }
        x += kGlyphW + 1;
    }
    return n * (kGlyphW + 1);
}

// The player panel, drawn from values instead of clipped from the bottom
// screen. Layout is against hp-pp.png's own 256x120 art, scaled to the
// destination box, so retuning the box does not need the numbers moved.
static void DrawPlayerPanel(u32* dst, const Frame& f)
{
    const float hs = HudScale();
    const int dw = (int)(124 * hs), dh = (int)(50 * hs);
    const int dx = 2, dy = 2;
    BlitArt(dst, kArt_panel, dx, dy, dw, dh);

    // MEASURED off hp-pp.png rather than guessed: the bar troughs are the long
    // dark runs at rows 61-68 and 97-104, both spanning x 75..237 of a 256x120
    // image. Interior is inset two rows from the trough border.
    const float kBarX0 = 77.0f / 256.0f, kBarX1 = 235.0f / 256.0f;
    const float kBarH  = 4.0f / 120.0f;
    struct Bar { float cur, max; float y; u32 rgb; };
    const Bar bars[2] = {
        { (float)f.hp, (float)(f.maxHp ? f.maxHp : 1), 63.0f / 120.0f, 0x40FF60 },
        { (float)f.pp, (float)(f.maxPp ? f.maxPp : 1), 99.0f / 120.0f, 0x40A0FF },
    };
    for (const Bar& b : bars)
    {
        float frac = b.cur / b.max;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;

        const int bx = dx + (int)(kBarX0 * dw);
        const int bw = (int)((kBarX1 - kBarX0) * dw * frac);
        const int by = dy + (int)(b.y * dh);
        const int bh = (int)(kBarH * dh) < 1 ? 1 : (int)(kBarH * dh);
        for (int y = by; y < by + bh; y++)
        {
            if (y < 0 || y >= 192) continue;
            for (int x = bx; x < bx + bw; x++)
            {
                if (x < 0 || x >= 256) continue;
                dst[y * 256 + x] = 0xFF000000u | b.rgb;
            }
        }

        // Numerals sit on the LABEL row, not inside the bar: the trough
        // interior is 4px and a glyph is 5px, so centring them in the bar puts
        // them half below it. Current left of the art's own slash, max right.
        const int ny = dy + (int)((b.y - 10.0f / 120.0f) * dh);
        DrawNumber(dst, (int)b.cur, dx + (int)(0.60f * dw), ny, 0xFFFFFF);
        DrawNumber(dst, (int)b.max, dx + (int)(0.93f * dw), ny, 0xFFFFFF);
    }

    // Level sits to the RIGHT of the art's own "Lv" label.
    DrawNumber(dst, f.level, dx + (int)(0.52f * dw), dy + (int)(0.16f * dh), 0xFFFFFF);
}

void Composite(u32* topFB, const u32* bottomFB, const Frame& f)
{
    if (!topFB || !bottomFB || !f.active) return;

    // Modal: the bottom screen IS the interaction, and at DS resolution the two
    // screens are the same size, so presenting it is a straight copy.
    if (f.modal)
    {
        // Save the slice first: the copy below is about to overwrite it, and it
        // is the top screen's own pixels that have to survive.
        static u32 slice[256 * 192];
        const bool keep = f.keepTop && f.ktw > 0 && f.kth > 0;
        if (keep)
            for (int y = 0; y < f.kth; y++)
                for (int x = 0; x < f.ktw; x++)
                {
                    const int sx = f.ktx + x, sy = f.kty + y;
                    if (sx >= 0 && sx < 256 && sy >= 0 && sy < 192)
                        slice[y * 256 + x] = topFB[sy * 256 + sx];
                }

        for (int i = 0; i < 256 * 192; i++) topFB[i] = bottomFB[i];

        if (keep)
            for (int y = 0; y < f.kth; y++)
                for (int x = 0; x < f.ktw; x++)
                {
                    const int dx = f.ktx + x, dy = f.kty + y;
                    if (dx >= 0 && dx < 256 && dy >= 0 && dy < 192)
                        topFB[dy * 256 + dx] = slice[y * 256 + x];
                }
        return;
    }

    // The player panel is DRAWN, not clipped -- our art, our numerals, from
    // values read out of RAM. The clip for it is skipped below.
    if (f.panel) DrawPlayerPanel(topFB, f);

    const float hs = HudScale();
    for (int i = 0; i < f.count; i++)
    {
        const Element& e = f.elems[i];
        if (f.panel && e.corner == Corner_TopLeft) continue;   // drawn instead
        const Place p = PlaceElement(e, hs);
        const int dx = (int)(p.x * 256.0f + 0.5f), dy = (int)(p.y * 192.0f + 0.5f);
        const int dw = (int)(p.w * 256.0f + 0.5f), dh = (int)(p.h * 192.0f + 0.5f);
        if (dw <= 0 || dh <= 0) continue;

        // Nearest-neighbour: a scaled element is a resample, and at DS
        // resolution anything smoother turns 6px-tall glyphs to mush.
        for (int y = 0; y < dh; y++)
        {
            const int ty = dy + y;
            if (ty < 0 || ty >= 192) continue;
            const int sy = e.sy + (int)((float)y * e.sh / (float)dh);
            for (int x = 0; x < dw; x++)
            {
                const int tx = dx + x;
                if (tx < 0 || tx >= 256) continue;
                const int sx = e.sx + (int)((float)x * e.sw / (float)dw);
                topFB[ty * 256 + tx] = bottomFB[sy * 256 + sx];
            }
        }
    }
}
}

namespace PSZMix
{

// From psz-re's ROM metadata for C24E. IDs and sizes, not content.
static constexpr u32 EternalSingleFileId = 3101;   // quest/single/free/quest_0021100.rel
static constexpr u32 WildValleyMultiId   = 3067;   // quest/multi/free/quest_0121100.rel
static constexpr u32 EternalSingleSize   = 137;
static constexpr u32 WildValleyMultiSize = 150;

static u32 Rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static void Wr32(u8* p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

bool PatchEternalMultiplayer(u8* rom, u32 romlen)
{
    static const bool on = EnvSet("PSZ_ETERNAL_MULTIPLAYER");
    if (!on || !rom || romlen < 0x200) return false;

    // Gamecode at 0x0C. Only ever touch the ROM this was measured against.
    if (memcmp(rom + 0x0C, "C24E", 4) != 0) return false;

    const u32 fatOff = Rd32(rom + 0x48), fatLen = Rd32(rom + 0x4C);
    const u32 need = (WildValleyMultiId > EternalSingleFileId
                      ? WildValleyMultiId : EternalSingleFileId) * 8 + 8;
    if (fatOff == 0 || fatLen < need || fatOff + fatLen > romlen) return false;

    u8* single = rom + fatOff + EternalSingleFileId * 8;
    u8* multi  = rom + fatOff + WildValleyMultiId  * 8;

    const u32 sStart = Rd32(single), sEnd = Rd32(single + 4);
    const u32 mStart = Rd32(multi),  mEnd = Rd32(multi + 4);
    if (sEnd < sStart || mEnd < mStart) return false;

    // Refuse unless both files are the size the metadata says. A FAT index that
    // has shifted would otherwise silently repoint some unrelated file.
    if (sEnd - sStart != EternalSingleSize) return false;
    if (mEnd - mStart != WildValleyMultiSize) return false;

    Wr32(multi, sStart);
    Wr32(multi + 4, sEnd);
    return true;
}

}
