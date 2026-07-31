/*
    Qt drawing for the PSZ single-screen overlay -- psz-melonmix.

    This is the frontend half of the split. PSZPlugin (core) decides WHAT to
    show; this decides how big and where, using the window's own geometry.

    Drawing is kept out of the core on purpose. Compositing into the 256x192
    framebuffer -- which is what PSZMix::Composite does for frontends that have no
    other option -- locks every element to DS pixel scale, where a 70px panel is
    27% of the screen at any output resolution. A frontend that knows its window
    size can do better, and this is that.
*/

#ifndef PSZOVERLAYQT_H
#define PSZOVERLAYQT_H

#include <QPainter>
#include <QImage>
#include <QRectF>

#include "PSZPlugin.h"

namespace PSZMix
{

// Draw the overlay for `f` onto the top screen, whose on-window rect is `top`.
// `bottom` is the bottom screen as an image, which is where every ported
// element is copied from.
void DrawOverlayQt(QPainter& p, const QImage& bottom, const QRectF& top, const Frame& f);

}

#endif // PSZOVERLAYQT_H
