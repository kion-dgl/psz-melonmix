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

#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace PSZMix
{

// Diagnostics have to reach the platform's own log: on Android nothing captures
// stdout, so a printf here is invisible exactly where it is needed most.
static void PszLog(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(4 /*INFO*/, "psz-melonmix", "%s", buf);
#else
    std::fprintf(stderr, "[psz] %s\n", buf);
#endif
}

// SETTINGS: environment first, then a config file.
//
// Every knob in this file was reachable only through the environment, which on
// Android means not reachable at all -- no PSZ_MAP_OPACITY, no PSZ_CAM_SPEED,
// no PSZ_HUD_ELEMENT_SCALE, on the one platform this build actually targets.
// Tuning values were being baked in and rebuilt one at a time because of it.
//
// So an absent variable falls back to a "key=value" line in psz.conf, looked
// for beside the ROM. Read once, on first use.
static const char* SettingRaw(const char* name)
{
    if (const char* v = std::getenv(name)) return v;

    static char buf[4096];
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        buf[0] = 0;
        // The app's own internal files dir is first because it is the only
        // one reachable on this Retroid: its ROM denies adb every /sdcard
        // path, so a config pushed there cannot be written, and scoped storage
        // means the app may not be able to read it even if it were. run-as can
        // write the internal dir on a debug build, and the app can always read
        // it.
        static const char* paths[] = {
            "/data/data/com.dashgl.pszmelonmix.dev/files/psz.conf",
            "/data/data/com.dashgl.pszmelonmix/files/psz.conf",
            "/sdcard/Download/melonmix/psz.conf",
            "psz.conf",
        };
        for (const char* p : paths)
        {
            if (std::FILE* f = std::fopen(p, "rb"))
            {
                const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
                buf[n] = 0;
                std::fclose(f);
                PszLog("settings loaded from %s (%u bytes)", p, (unsigned)n);
                break;
            }
        }
    }
    if (!buf[0]) return nullptr;

    // Match "name" at the start of a line, then take the rest after '='.
    const size_t len = std::strlen(name);
    for (const char* p = buf; *p; )
    {
        const char* eol = std::strchr(p, '\n');
        if (!eol) eol = p + std::strlen(p);
        if ((size_t)(eol - p) > len && std::strncmp(p, name, len) == 0 && p[len] == '=')
        {
            static char val[128];
            size_t n = (size_t)(eol - (p + len + 1));
            if (n >= sizeof(val)) n = sizeof(val) - 1;
            std::memcpy(val, p + len + 1, n);
            while (n && (val[n - 1] == '\r' || val[n - 1] == ' ')) n--;
            val[n] = 0;
            return val;
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return nullptr;
}

static bool EnvSet(const char* name)
{
    const char* v = SettingRaw(name);
    return v && *v && *v != '0';
}

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

// The contextual info box's text, UTF-16LE and NUL-terminated. Confirmed
// against eight savestates: enemy names when targeting, item names when
// standing on one. See psz-re docs/melonmix-questions.md Q1.
//
// NOTE it is STALE-RETAINING -- it keeps the last string written, so it says
// what WOULD be shown, not whether anything is. The visibility signal is still
// unknown, which is why the panel currently always draws.
static constexpr u32 InfoTextAddr = 0x0211CCD0;

// THE CAMERA (psz-re melonmix-questions.md Q3, answered).
//
// One object, identified by its vtable, which occurs exactly once in the heap
// of all 45 field savestates:
//
//   +0x00  vtable = 0x020F99A8
//   +0x82  u16  CURRENT yaw
//   +0x86  u16  recentre source, used as +0x8000
//   +0x88  u32  mode
//   +0x90  u16  TARGET yaw
//   +0x94  u32  step cap
//
// WRITE THE TARGET, NOT THE CURRENT. The per-frame update lerps
// current += (target - current) >> 3, so a written current is dragged back an
// eighth of the way every frame -- it would read as the camera fighting the
// stick. The target is what the game's own turn-to call writes, and nothing
// recomputes it unconditionally, so it is not stomped.
static constexpr u32 CameraObj    = 0x022512C0;
static constexpr u32 CameraVTable = 0x020F99A8;
static constexpr u32 CamCurYaw    = CameraObj + 0x82;
static constexpr u32 CamTgtYaw    = CameraObj + 0x90;

// One plate colour for every drawn panel. The readout and the info box are the
// same UI and looked it only by coincidence before; this makes it structural.
static constexpr u32 kPlateRGBA = 0xD0101820u;

// ACTION PALETTE ICON CELLS.
//
// Measured, both sides. On the game's bottom screen the three action icons are
// 28x28 cells at these positions; in psz-godot's palette_bg.png (128x67) the
// slots are 28x28 at (13,14), (77,14) and (45,28). Same size, same relative
// layout -- green upper-left, red upper-right, purple lower-centre -- because
// both come from the same original panel.
//
// So the icons can be CUT from the running game and dropped into our own frame,
// which means the palette needs no knowledge of what is mapped to each slot.
struct IconCell { int sx, sy; float ax, ay; };   // game coords; art coords
static const IconCell kPaletteIcons[] = {
    { 141, 16, 13.0f, 14.0f },
    { 205, 16, 77.0f, 14.0f },
    { 173, 30, 45.0f, 28.0f },
};
static constexpr int kIconSize = 28;
static constexpr float kPalArtW = 128.0f, kPalArtH = 67.0f;

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
    // Cropped to the map itself: 4px off the top, 7 off the right, 1 off the
    // bottom of the plate bounds, measured by kion on the device. What came off
    // was the game's own frame and the key row.
    { "PSZ_HUD_MAPRECT",   145, 113,  61, 63, Corner_TopRight    },  // map only
    { "PSZ_HUD_TGTRECT",     2,  96, 124, 56, Corner_BottomLeft  },  // locked-on target
    { "PSZ_HUD_PALRECT",   128,   6, 124, 54, Corner_BottomRight },  // action palette
};
static constexpr int NumRects = sizeof(Rects) / sizeof(Rects[0]);

// Frames to keep presenting the outgoing mode after the game switches, so a
// cross-fade covers the change. Six improved the title-to-file-select fade on
// the Retroid but still cut in slightly early, so ten; PSZ_TRANSITION_HOLD
// retunes it without a rebuild.
// How solid the minimap is. Opaque it blocks about a seventh of the screen for
// something the player only glances at, so it defaults to 60%.
static float MapOpacity()
{
    static const float a = [] {
        const char* v = SettingRaw("PSZ_MAP_OPACITY");
        if (!v) return 0.6f;
        const float f = (float)std::atof(v);
        return (f > 0.05f && f <= 1.0f) ? f : 0.6f;
    }();
    return a;
}

// Frames the player pointer must stay NULL before an unrecognised mode counts
// as "not gameplay". A room change drops it briefly; a shop holds it forever.
static int PointerDebounce()
{
    static const int n = [] {
        const char* v = SettingRaw("PSZ_POINTER_DEBOUNCE");
        if (!v) return 24;
        const int k = std::atoi(v);
        return (k >= 0 && k <= 300) ? k : 24;
    }();
    return n;
}

static int TransitionHold()
{
    static const int n = [] {
        const char* v = SettingRaw("PSZ_TRANSITION_HOLD");
        if (!v) return 10;
        const int k = std::atoi(v);
        return (k >= 0 && k <= 60) ? k : 10;
    }();
    return n;
}



// The big artwork HUD, off by default -- see DrawPlayerReadout.
static bool UseArtHud()
{
    static const bool on = EnvSet("PSZ_HUD_ART");
    return on;
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
    if (const char* v = SettingRaw(c.env)) enabled = (*v && *v != '0');
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
    case 12:   // shops and counters. MEASURED, having had it backwards: with
               // the item shop open the TOP screen carries the Buy/Sell menu
               // and the bottom holds the shopkeeper and a "Select from the
               // Menu" prompt. The interactive half is on top. Applies to the
               // item, weapon, custom and personnel shops and both counters --
               // all of them are ov12.
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

// THE TWO FORKS EXPOSE DIFFERENT FRAMEBUFFER APIS.
//
// Upstream melonDS has GPU::GetFramebuffers(); melonDS-android-lib has the raw
// GPU::Framebuffer[front][screen] arrays and no GetFramebuffers at all. This
// file is shared by both, so it cannot name either directly -- doing so built
// on desktop and failed the Android core with "no member named
// GetFramebuffers".
//
// Overload resolution picks whichever exists: the int overload is preferred and
// only viable when GetFramebuffers() is declared, otherwise the long one wins.
//
// Both return null under an accelerated renderer, where the frame lives only as
// a GL texture and there is no CPU-side framebuffer to read.
template <typename G>
static auto BottomFramebuffer(G& gpu, int)
    -> decltype(gpu.GetFramebuffers(nullptr, nullptr), (const u32*)nullptr)
{
    void* top = nullptr;
    void* bot = nullptr;
    if (!gpu.GetFramebuffers(&top, &bot) || !bot) return nullptr;
    return (const u32*)bot;
}

template <typename G>
static auto BottomFramebuffer(G& gpu, long) -> const u32*
{
    const int front = gpu.FrontBuffer;
    if (!gpu.Framebuffer[front][1]) return nullptr;
    return (const u32*)gpu.Framebuffer[front][1].get();
}

// Is there text in this box? Counts dark pixels inside an inset margin, since
// the game draws contextual text dark on a pale panel. Inset so the panel's own
// border does not count as content.
static bool BoxHasText(const u32* bottomFB, const Element& e)
{
    const int x0 = e.sx + 6, y0 = e.sy + 6;
    const int x1 = e.sx + e.sw - 6, y1 = e.sy + e.sh - 6;
    if (x1 <= x0 || y1 <= y0) return false;

    int dark = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            const u32 c = bottomFB[y * 256 + x];
            const int sum = (int)((c >> 16) & 0xFF) + (int)((c >> 8) & 0xFF) +
                            (int)(c & 0xFF);
            if (sum < 260) dark++;
        }
    return dark > 24;
}

// Right-stick state, set by the frontend once per frame.
static float gCameraStick = 0.0f;
void SetCameraStick(float x)
{
    gCameraStick = (x < -1.0f) ? -1.0f : (x > 1.0f) ? 1.0f : x;
}

// Degrees-ish per frame at full deflection. 0x10000 is a full turn, so 400 is
// about 132 degrees a second at 60fps. PSZ_CAM_SPEED retunes it; negative
// inverts.
static int CameraSpeed()
{
    static const int n = [] {
        const char* v = SettingRaw("PSZ_CAM_SPEED");
        if (!v) return 400;
        const int k = std::atoi(v);
        return (k >= -4000 && k <= 4000 && k != 0) ? k : 400;
    }();
    return n;
}

// Steer the camera from the right stick. Only ever runs in the main game, and
// only when the object at the expected address really is the camera -- the
// vtable check is what keeps a stray write out of an arbitrary heap object if
// a future state puts something else there.
static void ApplyCameraStick(NDS* nds)
{
    // PSZ_CAM_DEBUG=1 reports what the stick and the guard are doing, once a
    // second, so a dead stick can be told apart from a failed vtable check
    // without another build.
    static const bool dbg = EnvSet("PSZ_CAM_DEBUG");
    static int tick = 0;
    if (dbg && ((tick++ % 60) == 0))
        PszLog("stick=%.3f vtable=%08X (want %08X) cur=%u tgt=%u",
               (double)gCameraStick, Read(nds, CameraObj, 4), CameraVTable,
               Read(nds, CamCurYaw, 2), Read(nds, CamTgtYaw, 2));

    if (gCameraStick == 0.0f) return;
    if (Read(nds, CameraObj, 4) != CameraVTable) return;

    const int delta = (int)(gCameraStick * (float)CameraSpeed());
    if (!delta) return;

    // Steer from the CURRENT yaw, not the stored target. Reading the target
    // back and adding to it accumulates the lerp's own lag into the input and
    // the camera runs away; anchoring to where the camera actually is keeps the
    // stick proportional.
    const u32 cur = Read(nds, CamCurYaw, 2);
    Write(nds, CamTgtYaw, (cur + delta) & 0xFFFF, 2);
}

// The GL frontend cannot read framebuffer pixels cheaply, so it measures the
// box itself on a throttled readback and reports the answer here. Defaults to
// true: an unnecessary panel is a smaller failure than a missing prompt.
static bool gBoxHasTextHint = true;
void SetBoxHasTextHint(bool v) { gBoxHasTextHint = v; }
static bool BoxHasTextHint() { return gBoxHasTextHint; }

Frame Update(NDS* nds)
{
    Frame f;
    if (!nds || !nds->MainRAM) return f;

    ApplyCheats(nds);
    ApplyCameraStick(nds);

    // CAMERA PROBE. PSZ_CAM_PROBE="0xADDR,delta" writes (player facing + 180
    // degrees + delta) to ADDR every frame. Differential analysis narrowed the
    // camera yaw to 63 addresses that all become facing+180 when L recentres;
    // most are an 0x130-stride array of per-object copies, so the question is
    // which one the renderer actually reads. Writing is the only way to tell,
    // and writing is what the feature needs regardless.
    if (const char* v = SettingRaw("PSZ_CAM_PROBE"))
    {
        unsigned int addr = 0; int delta = 0;
        if (std::sscanf(v, "%x,%d", &addr, &delta) == 2 && InMainRAM(nds, addr))
        {
            const u32 facing = Read(nds, 0x021A2170, 2);
            Write(nds, addr, (facing + 32768 + delta) & 0xFFFF, 2);
        }
    }
    f.areaMap = ServiceAreaMapToggle(nds);

    // The player object pointer gates everything: it is NULL in every mode that
    // is not the main game, so a null base already means "the bottom screen is
    // the interaction" without needing to identify which mode it is.
    u32 base = Read(nds, PlayerPtr, 4);
    bool inGame = InMainRAM(nds, base) && base != 0;

    // THE START MENU HAS ITS OWN TRANSITION. It opens on control mode 5 rather
    // than an overlay change, so the overlay-change hold below never covered
    // it. Same fade, same fix, separate latch -- and a longer one, because kion
    // measured this fade as slower than the title's.
    const bool menuNow = inGame && Read(nds, base + 0x280, 4) == 5;
    static bool menuShown = false;
    static int menuHold = 0;
    if (menuNow != menuShown)
    {
        if (menuHold <= 0) menuHold = TransitionHold() + 6;
        if (--menuHold <= 0) menuShown = menuNow;
    }

    // WHICH MODE ARE WE IN.
    //
    // First attempt asked the overlay alone: gameplay iff ov04. That BROKE THE
    // SHOPS. The item, weapon, personnel and custom shops do not put a
    // recognised overlay in the slot -- ResidentOverlay returns no match, the
    // held id stayed at 4, and every shop rendered as gameplay on the top
    // screen. It also did not fix the transition flash it was meant to fix.
    //
    // So: the overlay decides ONLY when it recognises the mode. Otherwise fall
    // back to the player pointer, DEBOUNCED -- which is what distinguishes the
    // two cases that look identical in a single frame. A shop holds the pointer
    // NULL indefinitely; a room change drops it for a moment. Waiting a few
    // frames tells them apart, where no instantaneous test can.
    //
    // The player object is torn down and rebuilt across a room change and while
    // a save loads, so the pointer goes NULL for a few frames while the game is
    // still very much the main game. Gating the modal on that pointer meant
    // every room transition flashed the whole bottom screen full-size before
    // dropping back to gameplay.
    //
    // ov04 IS the main game (psz-re docs/game-state.md), and it stays resident
    // across a room change. So the overlay decides, and the pointer is only
    // used for the values it actually holds -- which already refuse to draw
    // when they read back inconsistent.
    static int gameplayOv = -1;
    {
        const int ovNow = ResidentOverlay(nds);
        if (ovNow >= 0) gameplayOv = ovNow;
    }

    // -1 no opinion, 0 not gameplay, 1 gameplay.
    int overlaySays = -1;
    switch (gameplayOv)
    {
    case 4:                       overlaySays = 1; break;
    case 0: case 6: case 11:
    case 12: case 14: case 16:
    case 17:                      overlaySays = 0; break;
    default:                      overlaySays = -1; break;
    }

    // STICKY. Only a RECOGNISED overlay may change the decision; an
    // unrecognised one keeps whatever is already showing.
    //
    // This is what kills the transition flash for good. Loading a room or
    // returning from a cutscene leaves the slot unrecognisable for as long as
    // the load takes -- far longer than any debounce worth having, which is why
    // 24 frames did not help. Holding the previous mode costs nothing, because
    // every mode that matters IS recognised: ov04 gameplay, ov12 shops and
    // counters, ov00/11/14/16/17 the rest.
    //
    // The player pointer only seeds the very first decision, before any overlay
    // has been recognised.
    static int decided = -1;
    if (overlaySays >= 0) decided = overlaySays;
    else if (decided < 0) decided = inGame ? 1 : 0;
    const bool gameplayMode = (decided == 1);

    if (!gameplayMode || menuShown)
    {
        // Hold the last known overlay across the DMA transient, so a mode never
        // flickers while it is being swapped in.
        static int lastOverlay = -1;
        const int ov = ResidentOverlay(nds);
        if (ov >= 0) lastOverlay = ov;

        // BOOT vs CUTSCENE, without settling what ov16/ov17 are called.
        //
        // psz-re names ov16/ov17 "intro logo" and "attract cutscene"; this
        // project's captures call them the cutscene and dialogue players. That
        // conflict is still open, and flipping either one wholesale would fix
        // boot and break cutscenes, or the reverse.
        //
        // Sequence settles it without naming anything: everything before the
        // FIRST title is boot.
        //
        // ESRB is on the BOTTOM screen and the SEGA logo on the TOP -- measured
        // by consequence, not assumption. This first shipped the other way
        // round, presenting the bottom screen during boot, and kion reported
        // still seeing ESRB. So boot wants the TOP screen, the same as a
        // cutscene does; it is only the modal default it has to escape.
        static bool seenTitle = false;
        if (lastOverlay == 0) seenTitle = true;
        const bool booting = !seenTitle;

        // TRANSITION HOLD. The game cross-fades between modes over several
        // frames. Switching presentation the instant the overlay changes
        // uncovered the raw bottom screen for a few frames BEFORE the fade
        // reached white. Holding the outgoing mode briefly lets the fade cover
        // the switch.
        //
        // shown   what is being presented
        // pending what the game has actually moved to
        static int shown = -1, pending = -1, hold = 0;
        if (lastOverlay != shown)
        {
            if (lastOverlay != pending) { pending = lastOverlay; hold = TransitionHold(); }
            if (hold > 0) hold--;
            if (hold <= 0) shown = pending;
        }
        const int effectiveOverlay = shown >= 0 ? shown : lastOverlay;

        // Cutscene, dialogue, character create, ending: the top screen is the
        // content. Draw nothing and let it through untouched.
        if (booting ||
            (effectiveOverlay >= 0 && !OverlayWantsBottomScreen(effectiveOverlay)))
            return f;                                  // active stays false

        // The title needs both screens: PRESS START is on the bottom, the logo
        // on the top. Presenting the bottom alone throws the logo away.
        if (!booting && effectiveOverlay >= 0 && OverlayKeepsTopSlice(effectiveOverlay))
        {
            // Estimated from a device capture of the title, not measured
            // against the framebuffer -- retune with PSZ_TITLE_LOGORECT.
            int x = 16, y = 68, w = 224, h = 56;
            if (const char* o = SettingRaw("PSZ_TITLE_LOGORECT"))
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
                if (const char* o = SettingRaw(d.env))
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

    // THE INFO PANEL EARNS ITS SPACE.
    //
    // The bottom-left box is contextual: most of the time it is empty, and a
    // permanently visible empty panel is just clutter on a single screen. The
    // check counts dark pixels inside the box, because the game draws that text
    // dark on a pale panel.
    //
    // This used to live in the Qt overlay only, so the GL path -- the one that
    // matters now -- showed the box unconditionally. Moving it into the core
    // means every frontend gets it from one implementation.
    // GetFramebuffers() returns FALSE under an accelerated renderer -- there are
    // no RAM framebuffers to read, the frame only exists as a GL texture. So
    // this test works on the software path and cannot work here on the GL one.
    //
    // Rather than reading pixels back off the GPU every frame, which would cost
    // the sync stall the -O2 fix just bought back, the GL frontend supplies its
    // own answer through SetBoxHasTextHint(). Absent a hint, the box shows --
    // failing toward visible, since a missing prompt is worse than a spare one.
    const u32* bottomFB = BottomFramebuffer(nds->GPU, 0);

    // Fold the UTF-16 box text to ASCII. Anything outside our glyph set becomes
    // a space rather than a wrong character.
    for (int i = 0; i < (int)sizeof(f.info) - 1; i++)
    {
        const u32 c = Read(nds, InfoTextAddr + i * 2, 2);
        if (!c) { f.info[i] = 0; break; }
        f.info[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        f.info[i + 1] = 0;
    }

    // The action palette: our frame, the game's icons.
    if (f.panel)      // same gate as the rest of the field HUD
    {
        const float hs = HudScale();
        // Minimal: the icons alone, no frame, at two thirds the size. "Just the
        // squares" was kion's suggestion after seeing the framed version take
        // up a quarter of the screen.
        const float scale = UseArtHud() ? 1.0f : 0.62f;
        const float fw = (124.0f / 256.0f) * hs * scale;
        const float fh = fw * (kPalArtH / kPalArtW) * (256.0f / 192.0f);
        // Sits harder into the corner than the 2px margin the clips use --
        // kion asked for it a few pixels down and further right once the frame
        // came off. PSZ_PAL_MARGIN="x,y" in DS pixels retunes it.
        int mx = 0, my = 0;
        if (const char* o = SettingRaw("PSZ_PAL_MARGIN")) std::sscanf(o, "%d,%d", &mx, &my);
        const float fx = 1.0f - (mx / 256.0f) - fw;
        const float fy = 1.0f - (my / 192.0f) - fh;
        f.palette = true;
        f.px = fx; f.py = fy; f.pw = fw; f.ph = fh;

        for (const IconCell& c : kPaletteIcons)
        {
            if (f.cutCount >= 8) break;
            Frame::Cut& cut = f.cuts[f.cutCount++];
            cut.sx = c.sx; cut.sy = c.sy; cut.sw = kIconSize; cut.sh = kIconSize;
            cut.dx = fx + (c.ax / kPalArtW) * fw;
            cut.dy = fy + (c.ay / kPalArtH) * fh;
            cut.dw = (kIconSize / kPalArtW) * fw;
            cut.dh = (kIconSize / kPalArtH) * fh;
            cut.alpha = 1.0f;
        }
    }

    for (int i = 0; i < NumRects; i++)
    {
        const RectDef& d = Rects[i];
        Element e { d.x, d.y, d.w, d.h, d.corner };
        if (const char* o = SettingRaw(d.env))
        {
            int x, y, w, h;
            if (std::sscanf(o, "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
            { e.sx = x; e.sy = y; e.sw = w; e.sh = h; }
        }
        if (e.sw <= 0 || e.sh <= 0 || e.sx < 0 || e.sy < 0 ||
            e.sx + e.sw > 256 || e.sy + e.sh > 192)
            continue;
        if (e.corner == Corner_BottomLeft)
        {
            const bool has = bottomFB ? BoxHasText(bottomFB, e) : BoxHasTextHint();
            if (!has) continue;
        }

        // The minimap goes through the CUT path rather than the element path,
        // so it can be drawn at partial opacity. Opaque it blocked roughly a
        // seventh of the screen for something the player only glances at.
        if (e.corner == Corner_TopRight && f.cutCount < 8)
        {
            const Place pl = PlaceElement(e, HudScale());
            Frame::Cut& c = f.cuts[f.cutCount++];
            c.sx = e.sx; c.sy = e.sy; c.sw = e.sw; c.sh = e.sh;
            c.dx = pl.x; c.dy = pl.y; c.dw = pl.w; c.dh = pl.h;
            c.alpha = MapOpacity();
            continue;
        }

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
        const char* v = SettingRaw("PSZ_HUD_ELEMENT_SCALE");
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
    static const PSZArtImage* cachedFor[8] = {};
    static u32* cached[8] = {};
    for (int i = 0; i < 8; i++)
        if (cachedFor[i] == &img) return cached[i];

    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (!cachedFor[i]) slot = i;
    if (slot < 0) return nullptr;

    u32* buf = new u32[img.w * img.h];
    u32* w = buf;
    const u32* end = buf + img.w * img.h;
    for (unsigned int i = 0; i + 4 < img.rleLen; i += 5)
    {
        const unsigned char n = img.rle[i];
        // The framebuffer is 0xAARRGGBB, NOT the R,G,B,A byte order the PNG
        // decodes to. Packing it straight through swapped red and blue, and the
        // panel's blue came out orange on the Retroid. The solid fills below
        // were already written as 0xRRGGBB and were correct, which is why only
        // the artwork was wrong.
        const u32 px = ((u32)img.rle[i + 1] << 16) | ((u32)img.rle[i + 2] << 8) |
                       (u32)img.rle[i + 3] | ((u32)img.rle[i + 4] << 24);
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
            const u32 sa = s >> 24;
            if (!sa) continue;
            if (sa == 255) { dst[ty * 256 + tx] = s; continue; }

            // Full source-over, including destination alpha. The framebuffer is
            // opaque so this reduces to the usual blend there, but the art
            // layer starts TRANSPARENT -- blending against an implicit opaque
            // black would fringe every soft edge with dark pixels.
            const u32 d = dst[ty * 256 + tx];
            const u32 da = d >> 24;
            const u32 ia = da * (255 - sa) / 255;
            const u32 oa = sa + ia;
            if (!oa) continue;
            u32 out = (oa & 0xFF) << 24;
            for (int c = 0; c < 3; c++)
            {
                const u32 sc = (s >> (8 * c)) & 0xFF, dc = (d >> (8 * c)) & 0xFF;
                out |= (((sc * sa + dc * ia) / oa) & 0xFF) << (8 * c);
            }
            dst[ty * 256 + tx] = out;
        }
    }
}

// Text, left-aligned at (x, y). Ours, not the game's -- see PSZArt.h.
static int DrawText(u32* dst, const char* str, int x, int y, u32 rgb)
{
    const int x0 = x;
    for (const char* c = str; *c; c++)
    {
        const char* p = std::strchr(kGlyphChars, *c);
        if (p)
        {
            const unsigned char* g = kGlyphs[p - kGlyphChars];
            // Outline first: a one-pixel dark halo around every lit pixel. This
            // is what makes small text readable over a moving scene -- without
            // it a thin stroke disappears into whatever is behind it.
            for (int gy = -1; gy <= kGlyphH; gy++)
                for (int gx = -1; gx <= kGlyphW; gx++)
                {
                    const int ty = y + gy, tx = x + gx;
                    if (ty < 0 || ty >= 192 || tx < 0 || tx >= 256) continue;
                    if (gy >= 0 && gy < kGlyphH && gx >= 0 && gx < kGlyphW &&
                        g[gy * kGlyphW + gx]) continue;          // lit, not halo
                    bool near = false;
                    for (int oy = -1; oy <= 1 && !near; oy++)
                        for (int ox = -1; ox <= 1; ox++)
                        {
                            const int sy = gy + oy, sx = gx + ox;
                            if (sy < 0 || sy >= kGlyphH || sx < 0 || sx >= kGlyphW) continue;
                            if (g[sy * kGlyphW + sx]) { near = true; break; }
                        }
                    if (near) dst[ty * 256 + tx] = 0xFF000000u;
                }
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
        x += kGlyphW;
    }
    return x - x0;
}

// Digits, right-aligned at (rx, y).
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

// The title's logo is DRAWN, not clipped. Lifting a rectangle off the top
// screen carried its background with it and the seam was distracting on the
// device -- the logo is not a rectangle, so no rect was ever going to work.
// psz-godot's logo.png, with alpha.
static void DrawTitleLogo(u32* dst)
{
    const int lw = kArt_logo.w, lh = kArt_logo.h;
    BlitArt(dst, kArt_logo, (256 - lw) / 2, (192 - lh) / 2 - 8, lw, lh);
}

// A rounded translucent plate. Both drawn panels use it, so they match.
static void DrawPlate(u32* dst, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    for (int py = y; py < y + h; py++)
    {
        if (py < 0 || py >= 192) continue;
        for (int px = x; px < x + w; px++)
        {
            if (px < 0 || px >= 256) continue;
            const int ex = (px - x < 2) ? 2 - (px - x)
                         : (px - (x + w - 1) > -2 ? 2 + (px - (x + w - 1)) : 0);
            const int ey = (py - y < 2) ? 2 - (py - y)
                         : (py - (y + h - 1) > -2 ? 2 + (py - (y + h - 1)) : 0);
            if (ex && ey && ex + ey > 2) continue;      // corner notch
            dst[py * 256 + px] = kPlateRGBA;
        }
    }
}

// MINIMAL PLAYER READOUT -- the default.
//
// The artwork panel is faithful but it is 124x50, half the width of a 256px
// screen, and on a Retroid-sized display that dominates the game rather than
// informing it. kion's read after playing: the small subtle info text works
// precisely BECAUSE it does not obscure anything, and the HP/map/palette are
// all massive by comparison.
//
// So this draws the same five values as text and two thin bars, in about a
// quarter of the area. PSZ_HUD_ART=1 restores the artwork panel.
static void DrawPlayerReadout(u32* dst, const Frame& f)
{
    // Inset from the screen edge. At x=3 the plate started at 0 and sat flush
    // against the corner, which reads as clipped rather than placed.
    const float hs = HudScale();
    const int x = 7, y = 7;
    const int bw = (int)(56 * hs), bh = (int)(3 * hs);
    const int line = kGlyphH + 1;

    // Rounded, and the same plate as the info box -- they are the same UI.
    DrawPlate(dst, x - 3, y - 3, bw + (int)(8 * hs),
              (kGlyphH + 1) * 3 + bh * 2 + (int)(10 * hs));

    char buf[24];
    std::snprintf(buf, sizeof(buf), "Lv%d", f.level);
    DrawText(dst, buf, x, y, 0xFFFFFF);

    struct Row { int cur, max; u32 rgb; };
    const Row rows[2] = { { f.hp, f.maxHp, 0x40FF60 }, { f.pp, f.maxPp, 0x40A0FF } };
    int ry = y + line + 2;
    for (const Row& r : rows)
    {
        std::snprintf(buf, sizeof(buf), "%d/%d", r.cur, r.max);
        DrawText(dst, buf, x, ry, 0xFFFFFF);
        ry += line;

        // Trough, then fill. The trough keeps the bar readable over a bright
        // scene without needing a panel behind it.
        const float frac = r.max > 0 ? (float)r.cur / (float)r.max : 0.0f;
        const int fillw = (int)(bw * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
        for (int py = ry; py < ry + bh; py++)
        {
            if (py < 0 || py >= 192) continue;
            for (int px = x; px < x + bw; px++)
            {
                if (px < 0 || px >= 256) continue;
                dst[py * 256 + px] = (px - x) < fillw ? (0xFF000000u | r.rgb)
                                                      : 0xA0101820u;
            }
        }
        ry += bh + 3;
    }
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
        // Black. White read as broken against the panel's pale art.
        DrawNumber(dst, (int)b.cur, dx + (int)(0.60f * dw), ny, 0x000000);
        DrawNumber(dst, (int)b.max, dx + (int)(0.93f * dw), ny, 0x000000);
    }

    // Level sits to the RIGHT of the art's own "Lv" label.
    DrawNumber(dst, f.level, dx + (int)(0.52f * dw), dy + (int)(0.16f * dh), 0x000000);
}

// The contextual info box, DRAWN: our own panel and the game's own words set in
// our font, rather than a rectangle clipped out of the bottom screen.
//
// There is no artwork for this one -- psz-godot has nothing matching the target
// box -- so the panel is drawn in code: a soft dark plate that reads over any
// scene without competing with it.
static void DrawInfoPanel(u32* dst, const Frame& f)
{
    if (!f.info[0]) return;

    // Size the plate to the text. A fixed slab looks like a UI element that
    // failed to fill; a plate that hugs the words reads as a label.
    const float hs = HudScale();
    int chars = 0;
    for (const char* c = f.info; *c; c++) chars++;
    const int pad = (int)(4 * hs);
    const int w = chars * kGlyphW + pad * 2;
    const int h = kGlyphH + pad * 2;
    const int x = 2, y = 192 - 2 - h;
    if (w <= 0 || w > 256) return;

    DrawPlate(dst, x, y, w, h);

    DrawText(dst, f.info, x + pad, y + pad, 0xFFFFFF);
}

bool RenderArtLayer(u32* out, const Frame& f)
{
    if (!out || !f.active) return false;

    const bool wantLogo  = f.modal && f.keepTop;
    const bool wantPanel = !f.modal && f.panel;
    const bool wantInfo  = !f.modal && f.info[0];
    const bool wantPal   = !f.modal && f.palette;
    if (!wantLogo && !wantPanel && !wantInfo && !wantPal) return false;

    std::memset(out, 0, 256 * 192 * sizeof(u32));
    if (wantLogo)  DrawTitleLogo(out);
    if (wantPanel) { if (UseArtHud()) DrawPlayerPanel(out, f); else DrawPlayerReadout(out, f); }
    if (wantInfo)  DrawInfoPanel(out, f);
    // Frame only; the icons are cuts, drawn by the frontend from the game's own
    // bottom screen -- see Frame::cuts.
    if (wantPal && UseArtHud())
        BlitArt(out, kArt_palette, (int)(f.px * 256), (int)(f.py * 192),
                (int)(f.pw * 256), (int)(f.ph * 192));
    return true;
}

void Composite(u32* topFB, const u32* bottomFB, const Frame& f)
{
    if (!topFB || !bottomFB || !f.active) return;

    // Modal: the bottom screen IS the interaction, and at DS resolution the two
    // screens are the same size, so presenting it is a straight copy.
    if (f.modal)
    {
        for (int i = 0; i < 256 * 192; i++) topFB[i] = bottomFB[i];

        if (f.keepTop) DrawTitleLogo(topFB);
        return;
    }

    // The player panel is DRAWN, not clipped -- our art, our numerals, from
    // values read out of RAM. The clip for it is skipped below.
    if (f.panel) DrawPlayerPanel(topFB, f);

    const float hs = HudScale();
    for (int i = 0; i < f.count; i++)
    {
        const Element& e = f.elems[i];
        if (f.panel && e.corner == Corner_TopLeft) continue;      // drawn instead
        if (f.info[0] && e.corner == Corner_BottomLeft) continue; // drawn instead
        if (f.palette && e.corner == Corner_BottomRight) continue; // frame+cuts

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
