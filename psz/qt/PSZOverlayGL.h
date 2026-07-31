/*
    OpenGL drawing for the PSZ single-screen overlay -- psz-melonmix.

    The Qt frontend has two display paths and picks between them in Window.cpp:

        hasOGL = Screen.UseGL || (3D.Renderer != renderer3D_Software)

    so asking for hi-res 3D (3D.GL.ScaleFactor) switches the whole display to
    ScreenPanelGL. The QPainter overlay lives in ScreenPanelNative and simply
    does not run there -- which meant hi-res 3D did not degrade the HUD, it
    removed it. This is the same overlay for that path.

    It draws quads sampling the screen texture the frontend has already uploaded:
    a GL_TEXTURE_2D_ARRAY with layer 0 = top screen and layer 1 = bottom. Every
    ported element is a sub-rect of layer 1, so no extra upload is needed and the
    elements stay crisp at whatever internal resolution the 3D renderer is using.
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
              float layer, float alpha, float r, float g, float b);

    unsigned int prog = 0, vao = 0;
    int uScreenSize = -1, uDstRect = -1, uSrcRect = -1, uLayer = -1, uTint = -1;
};

}

#endif // PSZOVERLAYGL_H
