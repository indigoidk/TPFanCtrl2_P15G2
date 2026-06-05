// --------------------------------------------------------------
//
//  Thinkpad Fan Control
//
// --------------------------------------------------------------
//
//	This program and source code is in the public domain.
//
//	The author claims no copyright, copyleft, license or
//	whatsoever for the program itself (with exception of
//	WinIO driver).  You may use, reuse or distribute it's 
//	binaries or source code in any desired way or form,  
//	Useage of binaries or source shall be entirely and 
//	without exception at your own risk. 
// 
// --------------------------------------------------------------

#ifndef FANCONTROL_H
#define FANCONTROL_H

#include "_prec.h"

#pragma once


#include "winstuff.h"
#include "TaskbarTextIcon.h"

#define FANCONTROLVERSION "2.33 P15G2 Dual"

#define WM__DISMISSDLG WM_USER+5
#define WM__GETDATA WM_USER+6
#define WM__NEWDATA WM_USER+7
#define WM__TASKBAR WM_USER+8

#define setzero(adr, size) memset((void*)(adr), (char)0x00, (size))
#define ARRAYMAX(tab) (sizeof(tab)/sizeof((tab)[0]))
#define NULLSTRUCT    { 0, }

class FANCONTROL {
protected:
	HINSTANCE hinstapp;
	HINSTANCE m_hinstapp;
	HWND hwndDialog;
	HWND m_hwndTip = NULL;

	UINT_PTR m_fanTimer;
	UINT_PTR m_titleTimer;
	UINT_PTR m_iconTimer;
	UINT_PTR m_renewTimer;

	struct FCSTATE {

		unsigned char FanCtrl,
			Fan1SpeedLo,
			Fan1SpeedHi,
			Fan2SpeedLo,
			Fan2SpeedHi;

		unsigned char Sensors[12];
		int SensorAddr[12];
		const char* SensorName[12];

	} State;

	struct SMARTENTRY {
		int temp, fan, hystUp, hystDown;
	} SmartLevels[32];

	struct SMARTENTRY1 {
		int temp1, fan1, hystUp1, hystDown1;
	} SmartLevels1[32];

	struct SMARTENTRY2 {
		int temp2, fan2, hystUp2, hystDown2;
	} SmartLevels2[32];

	struct FSMARTENTRY {        //fahrenheit values
		int ftemp, ffan;
	} FSmartLevels[32];

	struct SENSOROFFSET {
		int offs, hystMin, hystMax; // min and max temp values that offs takes effect. -1 to disable
	} SensorOffset[16];
	int LastSmartLevel = -1;
	int IconLevels[3];    // temp levels for coloring the icon
	int FIconLevels[3];    // fahrenheit temp levels for coloring the icon
	int CurrentIcon;
	// tray text-icon state (was file-scope globals)
	int m_textIcon = 0;            // current text-icon color id
	int m_textIconPrev = 0;        // previous id, for IconColorFan "keep last"
	bool m_showSymbolBalloon = true;   // show the symbol-icon intro balloon once
	bool m_showTextBalloon = true;     // show the text-icon intro balloon once
	int IndSmartLevel;
	int FSensorOffset[16];
	int iFarbeIconB;
	int iFontIconB;
	int icontemp;
	int Cycle;
	int IconCycle;
	int ReIcCycle;
	int NoExtSensor;
	int FanSpeedLowByte;
	int ActiveMode,
		UseTWR,
		ManFanSpeed,
		FinalSeen;
	int CurrentMode, fanctrl2,
		PreviousMode;
	int TaskbarNew;
	int MaxTemp;
	int iMaxTemp;
	int fan1speed, lastfan1speed, fan2speed, lastfan2speed;
	int FanBeepFreq, FanBeepDura;
	int MinimizeToSysTray,
		Lev64Norm,
		IconColorFan,
		Fahrenheit,
		MinimizeOnClose,
		StartMinimized,
		NoWaitMessage,
		Runs_as_service;
	int ReadErrorCount;
	int m_ecErrorsTotal = 0;   // cumulative EC read errors (shown in tray tooltip)
	int MaxReadErrors;
	int SecWinUptime;
	int SlimDialog;
	int NoBallons,
		HK_BIOS_Method,
		HK_Manual_Method,
		HK_Smart_Method,
		HK_SM1_Method,
		HK_SM2_Method,
		HK_TG_BS_Method,
		HK_TG_BM_Method,
		HK_TG_MS_Method,
		HK_TG_12_Method,
		HK_BIOS,
		HK_Manual,
		HK_Smart,
		HK_SM1,
		HK_SM2,
		HK_TG_BS,
		HK_TG_BM,
		HK_TG_MS,
		HK_TG_12;
	int EC_CTRL, EC_DATA;
	int ManModeExit;
	int ManModeExitInternal;
	int FailsafeTemp = 0;          // thermal fail-safe threshold in Celsius (0 = off)
	bool m_failsafeTripped = false;   // true while the fail-safe is holding the fan at max
	bool m_maxWarned = false;         // slider already prompted for max this visit (avoids re-prompt on repeat WM_HSCROLL)
	int ShowBiasedTemps;
	int SecStartDelay;
	char gSensorNames[17][4];
	int Log2File;
	int Log2csv;
	int StayOnTop;
	int ShowAll;
	int ShowTempIcon;
	int ShowTempHex;   // show (0x..) EC address column in temp list
	int ShowLog;       // show the Log box
	int DarkMode;      // dark theme
	int ShowGraph;     // show the temperature history sparkline (control 8120)
	// last main-window position/size (restored rect), persisted across runs via the
	// WindowPos= ini line. WinW <= 0 means "not saved yet" -> let the OS place it.
	int WinX = 0, WinY = 0, WinW = 0, WinH = 0;
	char IgnoreSensors[256];
	char MenuLabelSM1[32];
	char MenuLabelSM2[32];
	HANDLE hThread;
	HANDLE hLock;
	HANDLE hLockS;
	BOOL Closing;
	MUTEXSEM EcAccess;
	bool m_needClose;

	HBRUSH m_hbrDlg;     // modern flat dialog/static background
	HBRUSH m_hbrField;   // editable field background
	COLORREF m_clrText;  // current theme text color
	HFONT  m_hFontHdr;   // bold section-header font (replaces group-box frames)
	HFONT  m_hFontBig;   // semibold font for the State / Fan readouts
	HFONT  m_hFontTitle; // larger semibold font for the "TPFanControl = ..." line
	HFONT  m_hFontDlg;   // DPI-rescaled base font for the rest (PerMonitorV2)
	int m_fullW;         // full window width (captured once) for log auto-shrink
	// resize/reflow layout state
	BOOL m_layoutInit;   // base geometry captured yet?
	int  m_baseCW, m_baseCH;   // design-time client size
	int  m_minW, m_minH;       // minimum window size (= design size)
	RECT m_baseRC[17];   // design-time control rects (client coords)
	void ReflowLayout();       // re-anchor controls on WM_SIZE
	// PerMonitorV2 DPI state
	UINT m_curDpi;       // current window DPI (96 = 100%)
	BOOL m_inDpiChange;  // guard: suppress reflow while rescaling for a DPI change
	void RescaleForDpi(UINT newDpi, const RECT* suggested);  // WM_DPICHANGED handler

	// rolling history of MaxTemp for the in-dialog sparkline (owner-draw static 8120)
	static const int TEMPHIST_MAX = 120;   // samples kept (~10 min at Cycle=5s)
	unsigned char m_tempHist[TEMPHIST_MAX];
	int m_tempHistCount;       // valid samples (saturates at TEMPHIST_MAX)
	int m_tempHistHead;        // index of next write (ring buffer)
	void PushTempSample(int temp);          // record one MaxTemp reading
	void DrawSparkline(HDC hdc, const RECT& rc);   // paint the history graph
	void ApplyTheme();   // (re)build theme brushes + dark titlebar + repaint
	void InitThemeAndChrome();   // post-create: tab stops, menu checks, slider, theme
	void ApplyLogVisibility();   // show/hide Log + shrink/restore window width
	void UpdateTempList();   // repopulate RichEdit 8101 with per-sensor colors
	void ToggleGameMode(bool silent = false);   // hide/restore TVic driver files; silent on exit/shutdown
	void ShowSettingsDialog();   // modal in-app settings editor (writes TPFanControl.ini)
	static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	void ApplySettingsFromDialog(HWND hwnd);   // shared by OK and Apply in the Settings dialog

	// ---- Smart fan-curve editor (dialog 9400) ----------------------------------
	// In-app grid editor for the Level= / Level2= curves, so they no longer need
	// hand-editing in the ini. Both profiles are edited in working buffers; OK/Apply
	// writes them to SmartLevels1/2, re-activates the live profile, and rewrites the
	// ini curve lines (SaveCurves). Values are shown/edited in the display unit.
	static const int CURVE_ROWS = 12;   // visible grid rows (>= any realistic curve)
	struct CURVEROW { int temp, fan, hystUp, hystDown; };
	CURVEROW m_ceBuf[2][CURVE_ROWS];    // [0]=profile 1, [1]=profile 2 working copies
	int      m_ceProfile = 0;           // profile currently shown in the grid (0/1)
	void ShowCurveDialog(HWND owner = NULL);   // owner defaults to the main window
	static INT_PTR CALLBACK CurveDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	void CurveLoadProfileToBuf(int profile);   // SmartLevels{1,2} -> m_ceBuf (display unit)
	void CurveBufToGrid(HWND hwnd, int profile);   // m_ceBuf -> edit boxes
	void CurveGridToBuf(HWND hwnd, int profile);   // edit boxes -> m_ceBuf
	bool CurveApplyAndSave();                  // validate m_ceBuf -> SmartLevels{1,2}, re-activate, persist
	void SaveCurves(const char* filename);     // rewrite Level=/Level2= ini lines from SmartLevels{1,2}

	bool m_driversHidden = false;    // true when TVic .sys files are renamed to .bak
	char m_tempListSig[2048] = "";   // cache: skip RichEdit rebuild when nothing visible changed

	char Title[128];
	char Title2[128];
	char Title3[128];
	char Title5[128];
	char LastTitle[128];
	char LastTooltip[128];
	char TrayTip[128];   // multi-line tray tooltip: mode / max temp / fan / profile
	char CurrentStatus[256];
	char CurrentStatuscsv[256];

	// dialog.cpp
	int CurrentModeFromDialog();

	int ShowAllFromDialog();

	void ModeToDialog(int mode);

	void ShowAllToDialog(int mode);

	ULONG DlgProc(HWND hwnd, ULONG msg, WPARAM mp1, LPARAM mp2);

	static ULONG CALLBACK
		BaseDlgProc(HWND
			hwnd,
			ULONG msg, WPARAM
			mp1,
			LPARAM mp2
		);

	//The default app-icon with changing colors
	TASKBARICON* pTaskbarIcon;
	//
	CTaskbarTextIcon** ppTbTextIcon;
	MUTEXSEM* pTextIconMutex;

	static int _stdcall
		FANCONTROL_Thread(ULONG
			parm) \
	{ return ((FANCONTROL*)parm)->WorkThread(); }

	int WorkThread();

	// fancontrol.cpp
	bool LockECAccess();

	void FreeECAccess();

	bool SampleMatch(FCSTATE* smp1, FCSTATE* smp2);

	bool ReadEcStatus(FCSTATE* pfcstate);

	bool ReadEcRaw(FCSTATE* pfcstate);

	int HandleData();

	// apply SensorOffset to a raw sensor reading when ShowBiasedTemps is on,
	// honoring the per-sensor hysteresis window (offset disabled inside it).
	// State.Sensors always holds raw EC values; this is the single place bias
	// is computed, so display and fan-control decisions stay consistent.
	int BiasedTemp(int rawTemp, int sensorIndex) const;

	void SmartControl();

	// switch the active Smart table to profile 1 or 2: copies ALL fields
	// (temp, fan, hystUp, hystDown) and resets the hysteresis anchor.
	void ActivateSmartProfile(int profile);

	void TraceModeChange();   // log "Change Mode from <prev>-><cur>" (no-op if unchanged)

	int SetFan(const char* source, int level, bool final = false);

	int SetHdw(const char* source, int hdwctrl, int HdwOffset, int AnyWayBit);

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam);

	// register one balloon tooltip on a main-dialog control (no-op if the tip
	// window or control is missing). m_hwndTip is created once on first use.
	void AddTip(int ctrlId, const char* text);

	// gather the live State/Switch/Fan/Temperature fields into a text block and
	// put it on the clipboard (right-click "Copy readings" on the main dialog)
	void CopyReadingsToClipboard();

	// grey out the manual fan-level box + slider unless Manual mode is active, so
	// disabled controls reflect what actually applies to the current mode
	void UpdateManualControlsEnabled();

	// for detecting lid closing
	HPOWERNOTIFY hPowerNotify;
	bool isLidClosed = false;
	int previousModeBeforeLidClose = -1;
	// misc.cpp
	int ReadConfig(const char* filename);

	void SaveConfig(const char* filename);   // rewrite known keys in-place, preserving comments

	// window-position memory: capture the current restored rect and rewrite only the
	// WindowPos= ini line (isolated from SaveConfig so exit doesn't persist other
	// runtime toggles); RestoreWindowPos applies a saved rect if it lands on-screen.
	void SaveWindowPos(const char* filename);
	void RestoreWindowPos();

	void Trace(const char* text);

	void Tracecsv(const char* textcsv);

	void Tracecsvod(const char* textcsv);

	bool IsMinimized(void) const;

	void CurrentDateTimeLocalized(char* result, size_t sizeof_result);

	void CurrentTimeLocalized(char* result, size_t sizeof_result);

	HANDLE CreateThread(int(_stdcall
		* fnct)(ULONG),
		ULONG p
	);

	// portio.cpp
	bool ReadByteFromEC(int offset, unsigned char* pdata);

	bool WriteByteToEC(int offset, char data);

public:

	FANCONTROL(HINSTANCE hinstapp);

	~FANCONTROL();

	void Test(void);

	int ProcessDialog();

	HWND GetDialogWnd() { return hwndDialog; }

	HANDLE GetWorkThread() { return hThread; }

	// The texticons will be shown depending on variables
	void ProcessTextIcons(void);

	void RemoveTextIcons(void);
};

#endif // FANCONTROL_H
