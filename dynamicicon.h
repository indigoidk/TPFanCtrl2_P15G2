#pragma once

#include <windows.h>
#include <tchar.h>

class CDynamicIcon {
public:

	// iconSize: target square size in pixels; 0 = auto (SM_CXSMICON at current DPI)
	CDynamicIcon(const char line1[3], const char line2[3], const int iFarbeIconA, const int iFontIconA, int iconSize = 0);
	~CDynamicIcon();

	HICON GetHIcon();
private:

	HDC      memDC1_;
	HDC      memDC2_;
	HBITMAP  oldBmp_1;
	HBITMAP  oldBmp_2;
	HBITMAP  iconBmp_;
	HBITMAP  iconMaskBmp_;
	HBRUSH   hOldBrush;
	HRGN     rgn;
	HICON    icon_;
	int iconWidth_;    // DPI-scaled icon size (was fixed 16x16)
	int iconHeight_;

private:
	__inline static HFONT CreateFont(const HDC hDC, int size, bool big = false);
	// default und copy verbergen: deleted rather than private empty bodies so they
	// can never run with uninitialized GDI members (and silences static analysis)
	CDynamicIcon() = delete;
	CDynamicIcon(const CDynamicIcon&) = delete;
};
