// Child windows of the game - window-free parts (see childwin.h).
#include "childwin.h"
#include <string.h>

void picture_paint(HDC dc, const RECT &area, const RECT &vp, const PicSrc &s)
{
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    int vw = vp.right - vp.left, vh = vp.bottom - vp.top;
    // the part of the image inside the primary (the windowed desktop view may reach past its edges)
    int x0 = s.imgX < 0 ? 0 : s.imgX, y0 = s.imgY < 0 ? 0 : s.imgY;
    int x1 = s.imgX + s.imgW > s.primW ? s.primW : s.imgX + s.imgW;
    int y1 = s.imgY + s.imgH > s.primH ? s.primH : s.imgY + s.imgH;
    if (!s.bits || x1 <= x0 || y1 <= y0 || s.imgW <= 0 || s.imgH <= 0 || vw <= 0 || vh <= 0) {
        FillRect(dc, &area, black);
        return;
    }
    RECT bars[4] = {{area.left, area.top, area.right, vp.top},
                    {area.left, vp.bottom, area.right, area.bottom},
                    {area.left, vp.top, vp.left, vp.bottom},
                    {vp.right, vp.top, area.right, vp.bottom}};
    for (int i = 0; i < 4; i++)
        if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top)
            FillRect(dc, &bars[i], black);
    if (x0 != s.imgX || y0 != s.imgY || x1 != s.imgX + s.imgW || y1 != s.imgY + s.imgH)
        FillRect(dc, &vp, black);

    struct {
        BITMAPINFOHEADER h;
        DWORD extra[256];   // 16 bpp: the three masks, 8 bpp: the colour table
    } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = s.pitch / (s.bpp / 8);
    bi.h.biHeight = -(y1 - y0);   // top-down, starting at row y0
    bi.h.biPlanes = 1;
    bi.h.biBitCount = (WORD)s.bpp;
    bi.h.biCompression = s.bpp == 16 ? BI_BITFIELDS : BI_RGB;
    if (s.bpp == 16) {
        bi.extra[0] = 0xF800;
        bi.extra[1] = 0x07E0;
        bi.extra[2] = 0x001F;
    } else if (s.bpp == 8) {
        bi.h.biClrUsed = 256;
        for (int i = 0; i < 256; i++) {   // R | G << 8 | B << 16  ->  RGBQUAD (B, G, R)
            DWORD c = s.lut8 ? s.lut8[i] : (DWORD)i * 0x010101u;
            bi.extra[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
        }
    }
    int dx = vp.left + (int)((long long)(x0 - s.imgX) * vw / s.imgW);
    int dy = vp.top + (int)((long long)(y0 - s.imgY) * vh / s.imgH);
    int dw = (int)((long long)(x1 - x0) * vw / s.imgW), dh = (int)((long long)(y1 - y0) * vh / s.imgH);
    bool smooth = s.linear && (dw != x1 - x0 || dh != y1 - y0);
    SetStretchBltMode(dc, smooth ? HALFTONE : COLORONCOLOR);
    if (smooth)
        SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, dx, dy, dw, dh, x0, 0, x1 - x0, y1 - y0, s.bits + (size_t)y0 * s.pitch, (const BITMAPINFO *)&bi,
                  DIB_RGB_COLORS, SRCCOPY);
    GdiFlush();
}

bool child_paint_eligible(const char *cls, int w, int h, LONG exStyle)
{
    if (w <= 1 || h <= 1)
        return false;            // zero-size helper windows
    if (exStyle & WS_EX_TRANSPARENT)
        return false;            // declares that it does not cover what is under it
    if (cls && _stricmp(cls, "Edit") == 0)
        return false;            // a text field has to keep showing the caret and what is typed
    return true;
}

void child_coords(int cw, int ch, const RECT &vp, int cx, int cy, RECT *area, RECT *vpOut)
{
    SetRect(area, -cx, -cy, cw - cx, ch - cy);
    *vpOut = vp;
    OffsetRect(vpOut, -cx, -cy);
}

int child_sched_step(ChildSched *s, DWORD now, bool setChanged, int visible)
{
    int out = 0;
    if (setChanged) {
        s->changedAt = now;
        s->repaints = visible ? 3 : 0;
        out |= CS_PRESENT;   // a dialog came or went: present at once (paint-through / the game picture back)
    }
    static const DWORD repaintDue[] = {0, 1000, 250, 0};   // indexed by 'repaints'
    DWORD since = now - s->changedAt;
    if (s->repaints > 0 && since >= repaintDue[s->repaints]) {
        s->repaints--;
        out |= CS_REPAINT;
    }
    return out;
}

long fit_client_dx(long winLeft, long winRight, long clientLeft, long clientRight, long workLeft, long workRight)
{
    long workW = workRight - workLeft;
    if (winRight - winLeft <= workW || clientRight - clientLeft > workW)
        return 0;
    if (clientLeft < workLeft)
        return workLeft - clientLeft;
    if (clientRight > workRight)
        return workRight - clientRight;
    return 0;
}
