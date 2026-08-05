/*
    OpenGL drawing for the PSZ single-screen overlay -- psz-melonmix.
    See PSZOverlayGL.h for why this is the only desktop drawing path.
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
uniform float uProbe;    // >0.5: counting mode, keep only dark fragments

smooth in vec2 fUV;
out vec4 oColor;

void main()
{
    // COUNTING MODE. Nothing is drawn: the caller masks colour writes off and
    // runs an occlusion query, so the only output is HOW MANY fragments survive.
    // Discarding everything but the dark ones counts the glyph pixels in the
    // box, which is the CPU path's test expressed as the one question a GPU can
    // answer without being read back.
    if (uProbe > 0.5)
    {
        vec4 p = texture(uTex, vec3(fUV, uLayer));
        // Same threshold as BoxHasText: channel sum below 260 of 765.
        if ((p.r + p.g + p.b) >= (260.0 / 255.0)) discard;
        oColor = vec4(1.0);
        return;
    }

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
    uProbe      = glGetUniformLocation(prog, "uProbe");

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

    glGenQueries(2, probeQuery);
    return true;
}

void OverlayGL::deinit()
{
    if (probeQuery[0]) { glDeleteQueries(2, probeQuery); probeQuery[0] = probeQuery[1] = 0; }
    probeIssued[0] = probeIssued[1] = false;
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

void OverlayGL::fill(float x, float y, float w, float h,
                     float r, float g, float b, float a)
{
    // Negative alpha is the shader's "solid fill" signal -- see kFS.
    quad(x, y, w, h, 0, 0, 1, 1, 0.f, -a, r, g, b);
}

// Carried over from the QPainter path, which is where SELECT used to be handled
// and nowhere else -- so the map did nothing on the renderer most people run.
// Deliberately the same arithmetic: cell size from 55% of screen height,
// centred, with a 10px surround.
void OverlayGL::drawAreaMap(const Frame& f, float ax, float ay, float aw, float ah)
{
    if (f.roomCount <= 0) return;

    int minX = 255, maxX = 0, minY = 255, maxY = 0;
    for (int i = 0; i < f.roomCount; i++)
    {
        const Room& r = f.rooms[i];
        if (r.cx < minX) minX = r.cx;
        if (r.cx > maxX) maxX = r.cx;
        if (r.cy < minY) minY = r.cy;
        if (r.cy > maxY) maxY = r.cy;
    }
    const int cols = maxX - minX + 1, rows = maxY - minY + 1;
    if (cols <= 0 || rows <= 0 || cols > 32 || rows > 32) return;

    float cell = (ah * 0.55f) / (float)(cols > rows ? cols : rows);
    if (cell < 8.f) cell = 8.f;
    const float gw = cols * cell, gh = rows * cell;
    const float x0 = ax + aw * 0.5f - gw * 0.5f;
    const float y0 = ay + ah * 0.5f - gh * 0.5f;

    // One opacity over the whole grid, so the field stays readable underneath.
    // QPainter had setOpacity for this; here it multiplies into each quad's
    // alpha, which is why every constant below is scaled by it.
    const float o = envFloat("PSZ_MAP_OPACITY", 0.5f, 0.05f, 1.f);

    // Surround: the border colour as a filled rect, with the backdrop inset by
    // one pixel over it. Cheaper than four edge quads and it reads the same.
    fill(x0 - 10.f, y0 - 10.f, gw + 20.f, gh + 20.f, 210/255.f, 230/255.f, 255/255.f, 0.86f * o);
    fill(x0 - 9.f,  y0 - 9.f,  gw + 18.f, gh + 18.f, 8/255.f,   12/255.f,  22/255.f,  0.92f * o);

    for (int i = 0; i < f.roomCount; i++)
    {
        const Room& r = f.rooms[i];
        const float rx = x0 + (r.cx - minX) * cell, ry = y0 + (r.cy - minY) * cell;
        const float pad = cell / 10.f + 1.f;
        const bool here = (i == f.curRoom);

        const float cx = rx + pad, cy = ry + pad, cw = cell - pad * 2, ch = cell - pad * 2;
        if (cw <= 2.f || ch <= 2.f) continue;

        fill(cx, cy, cw, ch, 1.f, 1.f, 1.f, 0.94f * o);      // outline
        if (here) fill(cx + 1, cy + 1, cw - 2, ch - 2, 1.f, 210/255.f, 70/255.f, 1.f * o);
        else      fill(cx + 1, cy + 1, cw - 2, ch - 2, 150/255.f, 200/255.f, 1.f, 0.96f * o);

        for (int k = 0; k < 4; k++)
        {
            if (r.exits[k] == 0xFF) continue;
            const bool open = (r.gates[k] == 0);
            const float cr = open ? 235/255.f : 1.f;
            const float cg = open ? 235/255.f : 120/255.f;
            const float cb = open ? 235/255.f : 120/255.f;
            const float ca = (open ? 0.86f : 0.92f) * o;

            float t = cell / 7.f; if (t < 2.f) t = 2.f;
            const float mx = rx + cell * 0.5f, my = ry + cell * 0.5f;
            switch (k)   // N E S W
            {
            case 0: fill(mx - t * 0.5f, ry,                    t,       pad + t, cr, cg, cb, ca); break;
            case 1: fill(rx + cell - pad - t, my - t * 0.5f,   pad + t, t,       cr, cg, cb, ca); break;
            case 2: fill(mx - t * 0.5f, ry + cell - pad - t,   t,       pad + t, cr, cg, cb, ca); break;
            case 3: fill(rx, my - t * 0.5f,                    pad + t, t,       cr, cg, cb, ca); break;
            }
        }
    }
}

void OverlayGL::probeBox(const Frame& f, float screenW)
{
    if (!probeQuery[0] || f.probe[2] <= 0 || f.probe[3] <= 0) return;

    // COLLECT whatever has finished. Both slots are checked every frame, not
    // just the one whose turn it is: a query that was not ready when its turn
    // came would otherwise stay outstanding, its slot never free again, and the
    // probe would issue nothing ever after -- a deadlock that only shows up when
    // the GPU is a frame or more behind, which is exactly when it matters.
    for (int i = 0; i < 2; i++)
    {
        if (!probeIssued[i]) continue;
        GLuint ready = 0;
        glGetQueryObjectuiv(probeQuery[i], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;   // asking anyway is the stall this avoids

        GLuint dark = 0;
        glGetQueryObjectuiv(probeQuery[i], GL_QUERY_RESULT, &dark);
        probeIssued[i] = false;
        // Same bar as the CPU path: more than 24 dark pixels is text rather
        // than the panel's own edging.
        SetBoxHasTextHint(dark > 24);

        // What the count actually is, which is the thing to know before
        // adjusting anything about it. Reports the rect it measured too, since a
        // count that never moves and a rect that is wrong look identical from
        // the outside.
        if (std::getenv("PSZ_BOX_DEBUG"))
        {
            static GLuint last = 0xFFFFFFFF;
            if (dark != last)
            {
                last = dark;
                // MSAA would inflate every count by the sample rate and make the
                // threshold meaningless -- GL_SAMPLES_PASSED counts SAMPLES.
                // Reported once so it is ruled in or out rather than assumed.
                static bool saidSamples = false;
                if (!saidSamples)
                {
                    saidSamples = true;
                    GLint bufs = 0, samples = 0;
                    glGetIntegerv(GL_SAMPLE_BUFFERS, &bufs);
                    glGetIntegerv(GL_SAMPLES, &samples);
                    fprintf(stderr, "[psz] box probe: sample buffers=%d samples=%d\n",
                            bufs, samples);
                }
                fprintf(stderr, "[psz] box probe: dark=%u -> %s   rect=%d,%d %dx%d\n",
                        dark, dark > 24 ? "SHOWING" : "hidden",
                        f.probe[0], f.probe[1], f.probe[2], f.probe[3]);
            }
        }
    }

    int slot = -1;
    for (int i = 0; i < 2 && slot < 0; i++) if (!probeIssued[i]) slot = i;
    if (slot < 0) return;   // both in flight; next frame

    // ISSUE. Drawn at exactly the source rect's size in pixels, so one fragment
    // stands for one DS pixel and the CPU path's threshold carries over
    // unchanged whatever internal resolution the 3D renderer is using.
    {
        // Sized in DEVICE pixels, not logical ones. The viewport is in device
        // pixels while uScreenSize is logical, so on a 2x display a quad of the
        // rect's nominal size covers FOUR fragments per DS pixel and the count
        // comes back 4x high against a threshold that has not moved -- the
        // panel's own edging then reads as text on a Retina screen and not on
        // any other. Dividing by the ratio makes one fragment one DS pixel
        // everywhere, which is what the threshold was measured against.
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        float ratio = (screenW > 1.f && vp[2] > 0) ? ((float)vp[2] / screenW) : 1.f;
        if (ratio < 0.1f || ratio > 8.f) ratio = 1.f;

        const float pw = (float)f.probe[2] / ratio, ph = (float)f.probe[3] / ratio;

        GLboolean mask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_BLEND);

        glUniform1f(uProbe, 1.f);
        glBeginQuery(GL_SAMPLES_PASSED, probeQuery[slot]);
        quad(0.f, 0.f, pw, ph,
             f.probe[0] / 256.f, f.probe[1] / 192.f,
             (f.probe[0] + f.probe[2]) / 256.f, (f.probe[1] + f.probe[3]) / 192.f,
             1.f, 1.f, 1.f, 1.f, 1.f);   // layer 1: the bottom screen
        glEndQuery(GL_SAMPLES_PASSED);
        glUniform1f(uProbe, 0.f);

        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glEnable(GL_BLEND);
        probeIssued[slot] = true;
    }
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

    // Before anything is drawn, while the screen texture is still what is bound
    // and the state is known. Costs one masked-off quad of the box's size.
    probeBox(f, screenW);

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

    // Over everything, including a modal: SELECT is meant to answer "where am
    // I" without leaving whatever is on screen.
    if (f.areaMap) drawAreaMap(f, ax, ay, aw, ah);

    glBindVertexArray(0);
}

}
