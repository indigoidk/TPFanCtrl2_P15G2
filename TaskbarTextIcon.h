#pragma once

#include "systemtraysdk.h"
#include "DynamicIcon.h"
#include <crtdbg.h>


class CTaskbarTextIcon
{
	private:

		CDynamicIcon *m_pDicon;
		int iFarbeIconC;
		int iFontIconC;
		LPCTSTR aTooltipC;

		void _createdicon(const int iFarbeIconC, LPCTSTR aTooltipC, const int iFontIconC)
			{
			if(m_pSystray) m_pSystray->SetTooltipText(aTooltipC);

			// The tooltip update above is cheap; rebuilding the icon bitmap is not
			// (it allocates DCs, bitmaps, a font and an HICON every call). This runs
			// up to twice a second from the title timer, so skip the GDI rebuild when
			// nothing that affects the rendered icon has actually changed.
			if(m_pDicon
				&& iFarbeIconC == m_lastFarbe
				&& iFontIconC == m_lastFont
				&& strcmp(m_line1, m_lastLine1) == 0
				&& strcmp(m_line2, m_lastLine2) == 0)
				return;

			CDynamicIcon* pDiconTemp = new CDynamicIcon(m_line1, m_line2, iFarbeIconC, iFontIconC, m_iconSize);
			if(m_pSystray) m_pSystray->SetIcon(pDiconTemp->GetHIcon());
			if(m_pDicon)delete m_pDicon;
			m_pDicon = pDiconTemp;

			m_lastFarbe = iFarbeIconC;
			m_lastFont  = iFontIconC;
			strcpy_s(m_lastLine1, sizeof(m_lastLine1), m_line1);
			strcpy_s(m_lastLine2, sizeof(m_lastLine2), m_line2);
			};

		void _ballondicon(LPCTSTR szText,LPCTSTR szTitle,DWORD dwIcon,UINT uTimeout) 
			{
			if(m_pDicon) m_pSystray->ShowBalloon(szText, szTitle, dwIcon, uTimeout);
			};

		void _createsystray
		(
			HINSTANCE hInst,
			HWND hWnd,
			UINT uCallbackMessage,
			UINT uID,
			LPCTSTR aTooltipC
		 ) 
			{
			if(m_pSystray) delete m_pSystray;
			m_pSystray = new CSystemTray();
			m_pSystray->Create
						(
						hInst,
						hWnd, 
						uCallbackMessage,
						aTooltipC,   // tooltip
						m_pDicon->GetHIcon(),
						0
						);  
			};


	protected:

		static const int LINESLEN = 4;
		CSystemTray *m_pSystray;
		char m_line1[LINESLEN],m_line2[LINESLEN];
		//TODO: implement set/get/use
		COLORREF m_TextColor, m_BgColor;
		bool m_BgTransparent;
		int m_iconSize;   // DPI-scaled tray icon size (SM_CXSMICON)

		// last-rendered icon inputs, so _createdicon can skip an unchanged rebuild
		char m_lastLine1[LINESLEN], m_lastLine2[LINESLEN];
		int  m_lastFarbe, m_lastFont;

		void _setlines(const char* line1,const char* line2) 
		{
        strncpy_s(m_line1,sizeof(m_line1),line1,3);
        strncpy_s(m_line2,sizeof(m_line2),line2,3);
        m_line1[LINESLEN-1] = 0;
        m_line2[LINESLEN-1] = 0;
		};

	public:

    CTaskbarTextIcon
		(
		HINSTANCE hInst,
		HWND hWnd,
		UINT uCallbackMessage,
		UINT uID,
		const char * line1 = 0, //can be 3 chars
		const char * line2 = 0, //can be 3 chars
		int iFarbeIconC = 0,
		int iFontIconC = 11,
		LPCTSTR aTooltipC = _T("")
        ) 
		: 
        m_pSystray(NULL),
        m_pDicon(NULL),
        m_TextColor(RGB(0,0,0)),
        m_BgColor(RGB(255,0,0)),
        m_BgTransparent(false),
        m_iconSize(::GetSystemMetrics(SM_CXSMICON)),
        m_lastFarbe(-1),
        m_lastFont(-1)
			{
			m_lastLine1[0] = '\0';
			m_lastLine2[0] = '\0';
			_setlines(line1,line2);
			_createdicon(iFarbeIconC, aTooltipC, iFontIconC);
			_createsystray(hInst, hWnd, uCallbackMessage, uID, aTooltipC);
			};

    ~CTaskbarTextIcon(void)
		{
        if(m_pSystray)delete m_pSystray;
        if(m_pDicon)delete m_pDicon;
		};

    void ChangeText
		(
		const char * line1, 
		const char * line2 = 0, 
		const int iFarbeIconC = 0,
		const int iFontIconC = 9,
		LPCTSTR aTooltipC =_T("/0")
		) 
			{
			_setlines(line1,line2);
			_createdicon(iFarbeIconC,aTooltipC,iFontIconC);        
			}

    void DiShowballon
		(
		LPCTSTR szText    ,
		LPCTSTR szTitle   ,
		DWORD dwIcon      ,
		UINT uTimeout
		) 
			{
		_ballondicon( szText, szTitle, dwIcon, uTimeout); 
			}



};
