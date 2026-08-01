/*
    Qt drawing for the PSZ single-screen overlay -- psz-melonmix.
    See PSZOverlayQt.h for why this is not in the core.
*/

#include "PSZOverlayQt.h"

#include <cstdlib>

namespace PSZMix
{

namespace
{

float pszMapOpacity()
{
    static float o = -1.f;
    if (o < 0.f)
    {
        o = 0.5f;                       // kion: about half, so the field shows through
        if (const char* v = std::getenv("PSZ_MAP_OPACITY"))
        { float f = (float)atof(v); if (f > 0.05f && f <= 1.f) o = f; }
    }
    return o;
}

float pszHudScale()
{
    static float s = -1.f;
    if (s < 0.f)
    {
        s = 2.0f;
        if (const char* v = std::getenv("PSZ_HUD_SCALE"))
        { float f = (float)atof(v); if (f > 0.2f && f < 12.f) s = f; }
    }
    return s;
}

// Is there text in this source rect? The game draws the target box whether or
// not anything is locked on and there is no flag for it, but an empty box is a
// light panel with no dark glyphs and text is near-black, so counting dark
// pixels in the inset interior separates them with no RE.
bool pszBoxHasText(const QImage& bottom, const QRect& src)
{
    QRect r = src.adjusted(6, 6, -6, -6);
    if (r.width() <= 0 || r.height() <= 0) return false;
    int dark = 0;
    for (int y = r.top(); y <= r.bottom(); y++)
        for (int x = r.left(); x <= r.right(); x++)
        {
            QRgb c = bottom.pixel(x, y);
            if (qRed(c) + qGreen(c) + qBlue(c) < 260) dark++;
        }
    return dark > 24;
}

void pszDrawElement(QPainter& p, const QImage& bottom, const Element& e,
                    const QRectF& A, float S)
{
    QRect src(e.sx, e.sy, e.sw, e.sh);

    // The target box only earns its space when it has something to say.
    if (e.corner == Corner_BottomLeft && !pszBoxHasText(bottom, src)) return;

    // Layout comes from PSZMix::PlaceElement so this path, the DS-resolution
    // compositor and the GL path cannot drift apart. Only the space differs:
    // here it is window pixels inside the top screen's on-window rect A.
    // Size is a fraction of the top screen, not a fixed window-pixel multiple of
    // S: the elements are cut at DS resolution, and a fixed multiple made them
    // dominate a phone-sized screen. PSZ_HUD_ELEMENT_SCALE is the size control.
    (void)S;
    const PSZMix::Place pl = PSZMix::PlaceElement(e, PSZMix::HudScale());
    const int w = (int)(pl.w * A.width());
    const int h = (int)(pl.h * A.height());
    const int dx = (int)(A.left() + pl.x * A.width());
    const int dy = (int)(A.top()  + pl.y * A.height());

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.fillRect(QRect(dx - 2, dy - 2, w + 4, h + 4), QColor(0, 0, 0, 140));
    p.drawImage(QRect(dx, dy, w, h), bottom, src);
}

// Modal: the bottom screen IS the interaction (menus, shop, quest counter,
// title, file select), so present it whole rather than compositing corners.
// PSO-style: leave the top screen readable around the menu rather than burying
// it. The pre-game screens are SPLIT scenes -- character create puts its
// description on the top screen, the quest counter its "Select an area." prompt,
// the title its logo -- so a modal at 88% of height was hiding information the
// dual-screen original shows. At 66% with a light dim, both are legible at once.
//
// PSZ_MODAL_SCALE and PSZ_MODAL_DIM tune it; 1.0 and 170 restore the old
// full-cover behaviour.
void pszDrawModal(QPainter& p, const QImage& bottom, const QRectF& A)
{
    static float sc = -1.f; static int dim = -1;
    if (sc < 0.f)
    {
        sc = 0.66f;
        if (const char* v = std::getenv("PSZ_MODAL_SCALE"))
        { float f = (float)atof(v); if (f > 0.2f && f <= 1.f) sc = f; }
        dim = 90;
        if (const char* v = std::getenv("PSZ_MODAL_DIM"))
        { int d = atoi(v); if (d >= 0 && d <= 255) dim = d; }
    }

    const float fit = (float)(A.height() * sc) / 192.0f;
    const int w = (int)(256 * fit), h = (int)(192 * fit);
    const int dx = (int)A.center().x() - w / 2, dy = (int)A.center().y() - h / 2;

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.fillRect(A, QColor(0, 0, 0, dim));
    p.fillRect(QRect(dx - 3, dy - 3, w + 6, h + 6), QColor(230, 240, 255, 235));
    p.drawImage(QRect(dx, dy, w, h), bottom, QRect(0, 0, 256, 192));
}

// The area grid: every generated room as a cell, centred and translucent so the
// field stays readable underneath while navigating. SELECT toggles it.
void pszDrawAreaMap(QPainter& p, const Frame& f, const QRectF& A)
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
    int cols = maxX - minX + 1, rows = maxY - minY + 1;
    if (cols <= 0 || rows <= 0 || cols > 32 || rows > 32) return;

    int cell = (int)(A.height() * 0.55) / (cols > rows ? cols : rows);
    if (cell < 8) cell = 8;
    const int gw = cols * cell, gh = rows * cell;
    const int x0 = (int)A.center().x() - gw / 2, y0 = (int)A.center().y() - gh / 2;

    p.setRenderHint(QPainter::Antialiasing, false);
    p.setOpacity(pszMapOpacity());
    p.fillRect(QRect(x0 - 10, y0 - 10, gw + 20, gh + 20), QColor(8, 12, 22, 235));
    p.setPen(QColor(210, 230, 255, 220));
    p.drawRect(QRect(x0 - 10, y0 - 10, gw + 19, gh + 19));

    for (int i = 0; i < f.roomCount; i++)
    {
        const Room& r = f.rooms[i];
        const int rx = x0 + (r.cx - minX) * cell, ry = y0 + (r.cy - minY) * cell;
        const int pad = cell / 10 + 1;
        const bool here = (i == f.curRoom);

        p.fillRect(QRect(rx + pad, ry + pad, cell - pad * 2, cell - pad * 2),
                   here ? QColor(255, 210, 70, 255) : QColor(150, 200, 255, 245));
        p.setPen(QColor(255, 255, 255, 240));
        p.drawRect(QRect(rx + pad, ry + pad, cell - pad * 2 - 1, cell - pad * 2 - 1));

        for (int k = 0; k < 4; k++)
        {
            if (r.exits[k] == 0xFF) continue;
            QColor c = (r.gates[k] == 0) ? QColor(235, 235, 235, 220)
                                         : QColor(255, 120, 120, 235);
            int t = cell / 7; if (t < 2) t = 2;
            const int mx = rx + cell / 2, my = ry + cell / 2;
            QRect stub;
            switch (k)
            {
            case 0: stub = QRect(mx - t / 2, ry,                  t, pad + t); break;
            case 1: stub = QRect(rx + cell - pad - t, my - t / 2, pad + t, t); break;
            case 2: stub = QRect(mx - t / 2, ry + cell - pad - t, t, pad + t); break;
            case 3: stub = QRect(rx, my - t / 2,                  pad + t, t); break;
            }
            p.fillRect(stub, c);
        }
    }
    p.setOpacity(1.0);
}


}

void DrawOverlayQt(QPainter& p, const QImage& bottom, const QRectF& top, const Frame& f)
{
    if (!f.active) return;

    if (f.modal)
    {
        pszDrawModal(p, bottom, top);
        return;
    }

    const float scale = pszHudScale();
    for (int i = 0; i < f.count; i++)
        pszDrawElement(p, bottom, f.elems[i], top, scale);

    if (f.areaMap)
        pszDrawAreaMap(p, f, top);
}

}
