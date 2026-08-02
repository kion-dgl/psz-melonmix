/*
    GL overlay path for the Android frontend.

    The software path composites on the CPU and uploads the result, so it only
    works when melonDS is rendering into system memory. Under the OpenGL
    renderer the 3D core draws straight into the frame texture and the CPU
    framebuffers are never populated -- which is exactly why no HUD element
    appeared on the Retroid under OpenGL while the software renderer was fine.

    So this draws the same Frame with GL, into that same texture.
*/

#ifndef PSZ_OVERLAY_GLES_H
#define PSZ_OVERLAY_GLES_H

#include <GLES3/gl3.h>
#include "PSZPlugin.h"

namespace PSZGLES
{

// frameTexture holds BOTH screens stacked: the top screen occupies rows
// [0, 192*scale) and the bottom screen rows [screenH, screenH + 192*scale),
// where screenH is (192+1)*scale -- the extra row is upstream's gap.
//
// Safe to call every frame; does nothing when the frame has nothing to draw.
void Draw(GLuint frameTexture, int texW, int texH, int screenH, int scale,
          const PSZMix::Frame& f);

// Drop the GL objects. Must be called on a thread with the context current.
void Cleanup();

}

#endif
