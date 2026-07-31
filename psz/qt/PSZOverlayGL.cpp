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
    oColor = vec4(c.rgb * uTint.rgb, uTint.a);
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

    // Core profile still requires *a* bound VAO even when the shader reads no
    // attributes, so this one is deliberately empty.
    glGenVertexArrays(1, &vao);
    return true;
}

void OverlayGL::deinit()
{
    if (vao)  { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (prog) { glDeleteProgram(prog); prog = 0; }
}

void OverlayGL::quad(float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1,
                     float layer, float alpha, float r, float g, float b)
{
    glUniform4f(uDstRect, x, y, w, h);
    glUniform4f(uSrcRect, u0, v0, u1, v1);
    glUniform1f(uLayer, layer);
    glUniform4f(uTint, r, g, b, alpha);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void OverlayGL::draw(const Frame& f, float screenW, float screenH, const float topRect[4])
{
    if (!prog || !f.active) return;

    const float ax = topRect[0], ay = topRect[1], aw = topRect[2], ah = topRect[3];

    glUseProgram(prog);
    glBindVertexArray(vao);
    glUniform2f(uScreenSize, screenW, screenH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    if (f.modal)
    {
        // Dim the field, then the bottom screen whole and centred, matching the
        // QPainter path's 66% so the top screen stays readable around it.
        const float dim = envFloat("PSZ_MODAL_DIM", 90.f, 0.f, 255.f) / 255.f;
        quad(ax, ay, aw, ah, 0, 0, 1, 1, 1.f, -dim, 0.f, 0.f, 0.f);

        const float sc = envFloat("PSZ_MODAL_SCALE", 0.66f, 0.2f, 1.f);
        const float h = ah * sc, w = h * (256.f / 192.f);
        quad(ax + (aw - w) * 0.5f, ay + (ah - h) * 0.5f, w, h,
             0, 0, 1, 1, 1.f, 1.f, 1.f, 1.f, 1.f);
        glBindVertexArray(0);
        return;
    }

    // Elements are sized in real pixels, not DS ones, for the same reason the
    // QPainter path is: at DS scale a 70px panel is 27% of the screen at any
    // resolution, which is what made the overlay feel cramped in the first place.
    const float s = envFloat("PSZ_HUD_SCALE", 2.f, 0.2f, 12.f);
    const float m = 6.f * s;

    for (int i = 0; i < f.count; i++)
    {
        const Element& e = f.elems[i];
        const float w = e.sw * s, h = e.sh * s;
        const float x = (e.corner == Corner_TopLeft || e.corner == Corner_BottomLeft)
                        ? ax + m : ax + aw - m - w;
        const float y = (e.corner == Corner_TopLeft || e.corner == Corner_TopRight)
                        ? ay + m : ay + ah - m - h;

        quad(x, y, w, h,
             e.sx / 256.f, e.sy / 192.f, (e.sx + e.sw) / 256.f, (e.sy + e.sh) / 192.f,
             1.f, 1.f, 1.f, 1.f, 1.f);
    }

    glBindVertexArray(0);
}

}
