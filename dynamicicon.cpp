#include "_prec.h"
#include "DynamicIcon.h"
#include <string.h>


CDynamicIcon::CDynamicIcon(const char *line1, const char *line2, const int iFarbeIconA, const int iFontIconA, int iconSize, bool lightTaskbar) {
    //3 chars per line
    char _line1[4], _line2[4];
    strncpy_s(_line1, sizeof(_line1), line1, 3);
    strncpy_s(_line2, sizeof(_line2), line2, 3);
    _line1[3] = 0;
    _line2[3] = 0;

    // pick a DPI-appropriate size; fall back to the classic 16x16 if anything is odd
    if (iconSize <= 0)
        iconSize = ::GetSystemMetrics(SM_CXSMICON);
    if (iconSize < 16)
        iconSize = 16;
    iconWidth_ = iconSize;
    iconHeight_ = iconSize;

    //TODO: implement errorhandling

    HDC hDC = GetDC(0);

    memDC1_ = CreateCompatibleDC(hDC);

    // 32bpp top-down DIB so the icon carries per-pixel alpha (transparent
    // rounded corners) instead of the old hard-edged 1-bit mask square
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = iconWidth_;
    bi.bmiHeader.biHeight = -iconHeight_;   // negative = top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    iconBmp_ = CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bits)
        memset(bits, 0, (size_t)iconWidth_ * iconHeight_ * 4);

    oldBmp_1 = (HBITMAP) SelectObject(memDC1_, (HBITMAP) iconBmp_);

    // Win11-style rounded badge (radius ~ a third of the size)
    rgn = CreateRoundRectRgn(0, 0, iconWidth_ + 1, iconHeight_ + 1,
        iconWidth_ / 3, iconHeight_ / 3);

    // the neutral (BIOS-mode) badge follows the taskbar theme so it never
    // becomes a same-colored blob: near-white badge + black digits on the
    // default dark taskbar, dark badge + white digits on a light taskbar
    bool darkBadge = false;
    HBRUSH hBrush;
    switch (iFarbeIconA) {
        case 10:
            darkBadge = lightTaskbar;
            hBrush = CreateSolidBrush(darkBadge ? RGB(56, 56, 56) : RGB(245, 245, 245));
            break;
        case 11:
            hBrush = CreateSolidBrush(RGB(0, 170, 0)); //green  (in-app: below level0)
            break;
        case 12:
            hBrush = CreateSolidBrush(RGB(220, 170, 0)); //amber (in-app: >= level0)
            break;
        case 13:
            hBrush = CreateSolidBrush(RGB(232, 120, 0)); //orange (in-app: >= level1)
            break;
        case 14:
            hBrush = CreateSolidBrush(RGB(232, 48, 48)); //red   (in-app: >= level2)
            break;
        case 21:
            hBrush = CreateSolidBrush(RGB(175, 255, 175)); //sehr hell grün
            break;
        case 22:
            hBrush = CreateSolidBrush(RGB(123, 255, 123)); //hell grün
            break;
        case 23:
            hBrush = CreateSolidBrush(RGB(0, 255, 0)); //grün
            break;
        case 24:
            hBrush = CreateSolidBrush(RGB(0, 218, 0)); //dunkel grün
            break;
        case 25:
            hBrush = CreateSolidBrush(RGB(0, 164, 0)); //sehr dunkel grün
            break;
        default:
            // effectively unreachable (callers always set 10-14 / 21-25);
            // theme-aware belt and braces to match case 10
            darkBadge = lightTaskbar;
            hBrush = CreateSolidBrush(darkBadge ? RGB(56, 56, 56) : RGB(245, 245, 245));
    };


    FillRgn(memDC1_, rgn, hBrush);
    DeleteObject(hBrush);


    HFONT hfnt, hOldFont;

    // single value (no sensor-name line) -> center it bigger; 3 digits (e.g. degF)
    // stay at the normal size so they still fit the tiny icon.
    bool oneLine = (_line2[0] == 0);
    bool bigFont = oneLine && (strlen(_line1) <= 2);

    hfnt = this->CreateFont(memDC1_, iconWidth_, bigFont);

    if (hOldFont = (HFONT) SelectObject(memDC1_, hfnt)) {
        SetBkMode(memDC1_, TRANSPARENT);
        // white digits on the dark (light-taskbar) badge; the colored severity
        // badges keep black digits, which read on every hue in the palette
        SetTextColor(memDC1_, darkBadge ? RGB(255, 255, 255) : RGB(0, 0, 0));

        if (oneLine) {
            // center the temperature over the whole icon
            RECT r;
            r.left = 0;
            r.top = 0;
            r.right = iconWidth_;
            r.bottom = iconHeight_;
            DrawTextEx(memDC1_, (LPSTR) _line1, strlen(_line1), &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, NULL);
        }
        else {
            // rects were tuned for 16x16; scale proportionally for HiDPI sizes
            RECT r;
            r.top = MulDiv(-2, iconHeight_, 16); //org -1
            r.left = 0;
            r.bottom = MulDiv(9, iconHeight_, 16); //org 10
            r.right = iconWidth_;
            DrawTextEx(memDC1_, (LPSTR) _line1, strlen(_line1), &r, DT_CENTER, NULL);

            r.top = MulDiv(5, iconHeight_, 16);  //org 6 neu ?
            r.left = 0;
            r.bottom = MulDiv(16, iconHeight_, 16); // org 15
            r.right = iconWidth_;
            DrawTextEx(memDC1_, (LPSTR) _line2, strlen(_line2), &r, DT_CENTER, NULL);
        }

        SelectObject(memDC1_, hOldFont);
    }
    DeleteObject(hfnt);

    // alpha post-pass + AND mask in one walk: GDI drawing wrote 0 into the
    // DIB's alpha bytes (under the badge fill and the text alike), so set
    // every pixel inside the rounded region fully opaque and everything
    // outside fully transparent. The AND mask gets the same shape (1 =
    // transparent, MSB-first) so non-alpha renderers (low-color/RDP icon
    // paths) clip the corners exactly like the alpha path instead of
    // painting them black.
    int maskStride = ((iconWidth_ + 15) / 16) * 2;   // 1bpp, WORD-aligned rows
    unsigned char* maskBits = (unsigned char*)calloc((size_t)maskStride * iconHeight_, 1);
    if (bits) {
        GdiFlush();   // flush batched GDI before touching the DIB directly
        DWORD* px = (DWORD*)bits;
        for (int y = 0; y < iconHeight_; y++)
            for (int x = 0; x < iconWidth_; x++, px++) {
                if (PtInRegion(rgn, x, y))
                    *px |= 0xFF000000;
                else {
                    *px = 0;
                    if (maskBits)
                        maskBits[(size_t)y * maskStride + (x >> 3)] |=
                            (unsigned char)(0x80 >> (x & 7));
                }
            }
    }
    // CreateBitmap copies the bits, so the buffer is freed right after. (The
    // allocation is ~1.5 KB at most; if it ever failed, the mask would be
    // uninitialized but alpha-aware renderers - the normal path - ignore it.)
    iconMaskBmp_ = CreateBitmap(iconWidth_, iconHeight_, 1, 1, maskBits);
    free(maskBits);
    DeleteObject(rgn);

    SelectObject(memDC1_, (HBITMAP) oldBmp_1);
    DeleteDC(memDC1_);
    ReleaseDC(NULL, hDC);

    ICONINFO ii = {TRUE, 0, 0, iconMaskBmp_, iconBmp_};
    icon_ = CreateIconIndirect(&ii);

}

CDynamicIcon::~CDynamicIcon() {
    DestroyIcon(icon_);
    DeleteObject(iconBmp_);
    DeleteObject(iconMaskBmp_);
}

HICON CDynamicIcon::GetHIcon() {
    return icon_;
}

HFONT CDynamicIcon::CreateFont(const HDC hDC, int size, bool big) {
    LOGFONT lf;

    SecureZeroMemory(&lf, sizeof(LOGFONT));
    // base height -9 was tuned for 16px; scale it for the actual icon size
    lf.lfHeight = MulDiv(big ? -12 : -9, size, 16);
    lf.lfWeight = FW_SEMIBOLD;   // digits read heavier at tray sizes
    lf.lfOutPrecision = 1;
    lf.lfClipPrecision = 2;
    // grayscale antialiasing, NOT ClearType: subpixel rendering would write
    // colored fringes into the 32bpp DIB that survive the alpha post-pass
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = 0;
    strcpy_s(lf.lfFaceName, sizeof(lf.lfFaceName), "Segoe UI");

    return CreateFontIndirect(&lf);
};




