#include "PSZOverlayGLES.h"

#include <android/log.h>
#include <vector>

namespace PSZGLES
{

static const char* kVert =
    "#version 300 es\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "out vec4 oCol;\n"
    "void main() { oCol = vec4(texture(uTex, vUV).rgb, 1.0); }\n";

static GLuint gProg = 0, gVAO = 0, gVBO = 0, gFBO = 0, gScratch = 0;
static int gScratchW = 0, gScratchH = 0;
static bool gFailed = false;

static GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, "psz-melonmix", "shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool Init()
{
    if (gProg) return true;
    if (gFailed) return false;

    GLuint vs = Compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) { gFailed = true; return false; }

    gProg = glCreateProgram();
    glAttachShader(gProg, vs);
    glAttachShader(gProg, fs);
    glLinkProgram(gProg);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(gProg, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(gProg, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, "psz-melonmix", "link: %s", log);
        glDeleteProgram(gProg);
        gProg = 0;
        gFailed = true;
        return false;
    }

    glGenVertexArrays(1, &gVAO);
    glGenBuffers(1, &gVBO);
    glGenFramebuffers(1, &gFBO);
    glGenTextures(1, &gScratch);
    glBindTexture(GL_TEXTURE_2D, gScratch);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// Push one quad. Positions are NDC over the top-screen viewport; UVs index the
// scratch copy of the bottom screen.
static void PushQuad(std::vector<float>& v, const PSZMix::Place& p,
                     float u0, float v0, float u1, float v1)
{
    // The top screen's row 0 sits at NDC y = -1 because the texture stores it
    // as framebuffer row 0. So a placement measured downward from the top of
    // the screen maps straight through without a flip.
    const float x0 = p.x * 2.0f - 1.0f;
    const float x1 = (p.x + p.w) * 2.0f - 1.0f;
    const float y0 = p.y * 2.0f - 1.0f;
    const float y1 = (p.y + p.h) * 2.0f - 1.0f;

    const float q[6][4] = {
        { x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x1, y1, u1, v1 },
        { x0, y0, u0, v0 }, { x1, y1, u1, v1 }, { x0, y1, u0, v1 },
    };
    for (auto& e : q) { v.push_back(e[0]); v.push_back(e[1]); v.push_back(e[2]); v.push_back(e[3]); }
}

void Draw(GLuint frameTexture, int texW, int texH, int screenH, int scale,
          const PSZMix::Frame& f)
{
    if (!f.active || !frameTexture || scale <= 0) return;
    if (!Init()) return;

    const int visibleH = 192 * scale;          // the top screen's own rows
    if (screenH + visibleH > texH) return;     // texture is not the layout we assume

    // Build the geometry before touching GL state, so a frame with nothing to
    // draw costs no binds at all.
    std::vector<float> verts;
    if (f.modal)
    {
        PSZMix::Place whole = { 0.0f, 0.0f, 1.0f, 1.0f };
        PushQuad(verts, whole, 0.0f, 0.0f, 1.0f, 1.0f);
    }
    else
    {
        const float hs = PSZMix::HudScale();
        for (int i = 0; i < f.count; i++)
        {
            const PSZMix::Element& e = f.elems[i];
            PushQuad(verts, PSZMix::PlaceElement(e, hs),
                     e.sx / 256.0f, e.sy / 192.0f,
                     (e.sx + e.sw) / 256.0f, (e.sy + e.sh) / 192.0f);
        }
    }
    if (verts.empty()) return;

    GLint prevFBO = 0, prevVP[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevVP);

    // Sampling a texture while rendering into it is undefined, so take a copy
    // of the bottom screen first. One glCopyTexSubImage2D per frame, only on
    // frames that draw something.
    if (gScratchW != texW || gScratchH != visibleH)
    {
        glBindTexture(GL_TEXTURE_2D, gScratch);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, visibleH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gScratchW = texW;
        gScratchH = visibleH;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, gFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frameTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
        return;
    }

    glBindTexture(GL_TEXTURE_2D, gScratch);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, screenH, texW, visibleH);

    // Draw into the top screen's rows only.
    glViewport(0, 0, texW, visibleH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(gProg);
    glUniform1i(glGetUniformLocation(gProg, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gScratch);

    glBindVertexArray(gVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 4));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
}

void Cleanup()
{
    if (gProg)    { glDeleteProgram(gProg); gProg = 0; }
    if (gVAO)     { glDeleteVertexArrays(1, &gVAO); gVAO = 0; }
    if (gVBO)     { glDeleteBuffers(1, &gVBO); gVBO = 0; }
    if (gFBO)     { glDeleteFramebuffers(1, &gFBO); gFBO = 0; }
    if (gScratch) { glDeleteTextures(1, &gScratch); gScratch = 0; }
    gScratchW = gScratchH = 0;
    gFailed = false;
}

}
