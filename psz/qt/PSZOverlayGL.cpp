/*
    OpenGL drawing for the PSZ single-screen overlay -- psz-melonmix.
    See PSZOverlayGL.h for why this path exists alongside the QPainter one.
*/

#include "PSZOverlayGL.h"

#include "OpenGLSupport.h"

#include <cstdlib>
#include <cstdio>

namespace PSZMix
{

namespace
{

// The quad is generated from gl_VertexID rather than a vertex buffer: there are
// only six vertices and the geometry is a rectangle, so an attribute buffer
// would be more moving parts than the thing it draws.
const char* kVS = R"(#version 140

uniform vec2 uScreenSize;
uniform vec4 uDstRect;   // x, y, w, h in the same logical pixels as uScreenSize
uniform vec4 uSrcRect;   // u0, v0, u1, v1 in 0..1

smooth out vec2 fUV;

void main()
{
    // two triangles: 0,1,2  2,1,3
    int id = gl_VertexID;
    float fx = (id == 0 || id == 2 || id == 4) ? 0.0 : 1.0;
    float fy = (id == 0 || id == 1 || id == 3) ? 0.0 : 1.0;
    if (id == 2) { fx = 0.0; fy = 1.0; }
    if (id == 3) { fx = 1.0; fy = 0.0; }
    if (id == 4) { fx = 0.0; fy = 1.0; }
    if (id == 5) { fx = 1.0; fy = 1.0; }

    vec2 pos = uDstRect.xy + vec2(fx, fy) * uDstRect.zw;
    vec2 ndc = ((pos * 2.0) / uScreenSize) - 1.0;
    ndc.y *= -1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    fUV = mix(uSrcRect.xy, uSrcRect.zw, vec2(fx, fy));
}
)";

const char* kFS = R"(#version 140

uniform sampler2DArray uTex;
uniform float uLayer;
uniform vec4 uTint;      // rgb multiplier, a = overall alpha; a<0 means solid fill
uniform float uTexAlpha; // 0 = ignore the texture's alpha, 1 = multiply by it

smooth in vec2 fUV;
out vec4 oColor;

void main()
{
    if (uTint.a < 0.0)
    {
        // Solid fill (the modal dim). No texture fetch at all.
        oColor = vec4(uTint.rgb, -uTint.a);
        return;
    }
    vec4 c = texture(uTex, vec3(fUV, uLayer));
    // The screen layers are opaque, so their alpha is ignored and uTint.a alone
    // decides. The art layer is mostly transparent and its alpha is the whole
    // point, hence the switch rather than always multiplying.
    oColor = vec4(c.rgb * uTint.rgb, uTint.a * mix(1.0, c.a, uTexAlpha));
}
)";

float envFloat(const char* name, float def, float lo, float hi)
{
    if (const char* v = std::getenv(name))
    {
        float f = (float)atof(v);
        if (f >= lo && f <= hi) return f;
    }
    return def;
}

}

bool OverlayGL::init()
{
    if (!melonDS::OpenGL::CompileVertexFragmentProgram(prog, kVS, kFS, "PSZOverlay",
                                                       {}, {{"oColor", 0}}))
    {
        fprintf(stderr, "[psz] overlay shader FAILED to compile\n");
        prog = 0;
        return false;
    }

    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    uScreenSize = glGetUniformLocation(prog, "uScreenSize");
    uDstRect    = glGetUniformLocation(prog, "uDstRect");
    uSrcRect    = glGetUniformLocation(prog, "uSrcRect");
    uLayer      = glGetUniformLocation(prog, "uLayer");
    uTint       = glGetUniformLocation(prog, "uTint");
    uTexAlpha   = glGetUniformLocation(prog, "uTexAlpha");

    // Core profile still requires *a* bound VAO even when the shader reads no
    // attributes, so this one is deliberately empty.
    glGenVertexArrays(1, &vao);

    // The art layer, as a one-layer 2D ARRAY rather than a plain 2D texture, so
    // the one shader can sample it and the screen texture without a second
    // sampler and a second program.
    //
    // GL_BGRA on upload, matching how the frontend uploads the framebuffers
    // themselves (Screen.cpp) -- RenderArtLayer packs 0xAARRGGBB, which is BGRA
    // in memory. Uploading it as GL_RGBA swaps red and blue.
    glGenTextures(1, &artTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, artTex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 256, 192, 1, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return true;
}

void OverlayGL::deinit()
{
    if (vao)    { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (artTex) { glDeleteTextures(1, &artTex); artTex = 0; }
    if (prog)   { glDeleteProgram(prog); prog = 0; }
}

void OverlayGL::quad(float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1,
                     float layer, float alpha, float r, float g, float b,
                     float texAlpha)
{
    glUniform4f(uDstRect, x, y, w, h);
    glUniform4f(uSrcRect, u0, v0, u1, v1);
    glUniform1f(uLayer, layer);
    glUniform4f(uTint, r, g, b, alpha);
    glUniform1f(uTexAlpha, texAlpha);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void OverlayGL::draw(const Frame& f, float screenW, float screenH, const float topRect[4])
{
    if (!prog || !f.active) return;

    const float ax = topRect[0], ay = topRect[1], aw = topRect[2], ah = topRect[3];

    // Rendered before any GL state is touched, so a frame with nothing to draw
    // costs no binds at all. RenderArtLayer clears the buffer itself.
    const bool haveArt = RenderArtLayer(artLayer, f);

    // The frontend has the screen texture bound on unit 0 and goes on using it
    // after this returns (the OSD, the next frame's screen quads), so whatever
    // was bound is put back before leaving.
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex);

    glUseProgram(prog);
    glBindVertexArray(vao);
    glUniform2f(uScreenSize, screenW, screenH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    if (f.modal)
    {
        // The bottom screen IS the interaction here (menus, shop, quest counter,
        // title, file select), and it takes the WHOLE top screen -- the same
        // presentation the Android build gives, where Composite() copies the
        // bottom framebuffer over the top one outright.
        //
        // It was a 66% centred inset with a dim, to keep the split scenes' top
        // screen readable beside the menu. That cost more than it bought: the
        // title logo and the character-create panels are drawn by the art layer
        // in the modal's own space, so the inset shrank the very thing it was
        // meant to leave room for. PSZ_MODAL_SCALE below 1 restores it.
        const float sc = envFloat("PSZ_MODAL_SCALE", 1.f, 0.2f, 1.f);
        if (sc >= 1.f)
        {
            quad(ax, ay, aw, ah, 0, 0, 1, 1, 1.f, 1.f, 1.f, 1.f, 1.f);
        }
        else
        {
            const float dim = envFloat("PSZ_MODAL_DIM", 90.f, 0.f, 255.f) / 255.f;
            quad(ax, ay, aw, ah, 0, 0, 1, 1, 1.f, -dim, 0.f, 0.f, 0.f);

            const float h = ah * sc, w = h * (256.f / 192.f);
            quad(ax + (aw - w) * 0.5f, ay + (ah - h) * 0.5f, w, h,
                 0, 0, 1, 1, 1.f, 1.f, 1.f, 1.f, 1.f);
        }
    }
    else
    {
        // Layout comes from PSZMix::PlaceElement, not from arithmetic of our
        // own. This path used to size elements as a fixed multiple of window
        // pixels, which put them somewhere no other frontend agreed with AND
        // somewhere the art layer -- which is positioned in DS space -- could
        // not line up with. One layout definition, three renderers.
        const float hs = HudScale();
        for (int i = 0; i < f.count; i++)
        {
            const Element& e = f.elems[i];

            // The art layer draws these outright, so their clips would show
            // through underneath. Composite() and the Android GL path skip the
            // same three.
            if (f.panel && e.corner == Corner_TopLeft) continue;
            if (f.info[0] && e.corner == Corner_BottomLeft) continue;
            if (f.palette && e.corner == Corner_BottomRight) continue;

            const Place p = PlaceElement(e, hs);
            quad(ax + p.x * aw, ay + p.y * ah, p.w * aw, p.h * ah,
                 e.sx / 256.f, e.sy / 192.f, (e.sx + e.sw) / 256.f, (e.sy + e.sh) / 192.f,
                 1.f, 1.f, 1.f, 1.f, 1.f);
        }
    }

    // Our own art over the top: the title logo, the drawn player panel, the
    // character-create panels. Layer 0 of a one-layer array, so uLayer is 0 here
    // rather than the 1 the screen quads use for the bottom screen.
    if (haveArt)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, artTex);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 256, 192, 1,
                        GL_BGRA, GL_UNSIGNED_BYTE, artLayer);
        quad(ax, ay, aw, ah, 0, 0, 1, 1, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (unsigned int)prevTex);
    }

    // Cuts LAST. They land inside artwork the layer draws -- the palette's
    // action icons sit in the slots of our own frame -- so drawing them with
    // the other clips puts them under it and they vanish.
    for (int i = 0; i < f.cutCount; i++)
    {
        const Frame::Cut& c = f.cuts[i];
        quad(ax + c.dx * aw, ay + c.dy * ah, c.dw * aw, c.dh * ah,
             c.sx / 256.f, c.sy / 192.f, (c.sx + c.sw) / 256.f, (c.sy + c.sh) / 192.f,
             1.f, c.alpha, 1.f, 1.f, 1.f);
    }

    glBindVertexArray(0);
}

}
