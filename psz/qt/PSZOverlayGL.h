/*
    OpenGL drawing for the PSZ single-screen overlay -- psz-melonmix.

    The ONLY desktop overlay. Upstream has two display paths and picks between
    them in Window.cpp, so this build used to carry an overlay for each: this
    one, and a QPainter one in ScreenPanelNative. Two implementations of the same
    drawing is how they drift, and they did -- the QPainter path went a release
    without our own art, so the title had no logo and character create had no UI
    at all, on whichever path the user's renderer setting happened to select.

    So createScreenPanel is patched to always build ScreenPanelGL and the
    QPainter overlay is gone. The software 3D RENDERER still works and is still
    selectable; it feeds this panel like the GL one does. What was removed is the
    software DISPLAY path, which is the thing that chose between overlays.

    It draws quads sampling the screen texture the frontend has already uploaded:
    a GL_TEXTURE_2D_ARRAY with layer 0 = top screen and layer 1 = bottom. Every
    ported element is a sub-rect of layer 1, so no extra upload is needed and the
    elements stay crisp at whatever internal resolution the 3D renderer is using.

    Our OWN art -- the title logo, the drawn player panel, the character-create
    panels -- has no source pixels on either screen, so it cannot be a clip. It
    comes from PSZMix::RenderArtLayer as a 256x192 straight-alpha layer that is
    uploaded and drawn over the top screen, exactly as the Android GL path does
    it. Rendering it on the CPU and uploading is deliberate: both renderers then
    produce the same image by construction rather than by two implementations of
    the same drawing agreeing with each other.
*/

#ifndef PSZOVERLAYGL_H
#define PSZOVERLAYGL_H

#include "PSZPlugin.h"

namespace PSZMix
{

class OverlayGL
{
public:
    // Compile the shader. Needs a current GL context.
    bool init();
    void deinit();

    // Draw `f` over the top screen. `screenW/H` are the logical screen size the
    // frontend passes to its own shader (window size / device pixel ratio), and
    // `topRect` is where the top screen lands in that space: x, y, w, h.
    void draw(const Frame& f, float screenW, float screenH, const float topRect[4]);

private:
    void quad(float x, float y, float w, float h,
              float u0, float v0, float u1, float v1,
              float layer, float alpha, float r, float g, float b,
              float texAlpha = 0.f);

    // Solid colour, no texture fetch. r/g/b/a are 0..1.
    void fill(float x, float y, float w, float h,
              float r, float g, float b, float a);

    // The SELECT area grid. Drawn from quads rather than clipped from anywhere:
    // it is built out of the game's own room table, and has no pixels on either
    // screen to copy.
    void drawAreaMap(const Frame& f, float ax, float ay, float aw, float ah);

    unsigned int prog = 0, vao = 0, artTex = 0;
    int uScreenSize = -1, uDstRect = -1, uSrcRect = -1, uLayer = -1, uTint = -1;
    int uTexAlpha = -1;

    // Per instance rather than a file-scope buffer: melonDS can open a second
    // window, and each one draws on its own GL thread.
    u32 artLayer[256 * 192] = {};
};

}

#endif // PSZOVERLAYGL_H
