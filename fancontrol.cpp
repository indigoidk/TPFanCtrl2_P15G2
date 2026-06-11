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
#include "_prec.h"
#include "fancontrol.h"
#include "tools.h"
#include "taskbartexticon.h"
#include "sysinfoapi.h"
#include <shobjidl.h>   // ITaskbarList3 (taskbar overlay/progress, inbox COM)
#include <winevt.h>     // EvtSubscribe (Modern Standby S0 watcher)
#pragma comment(lib, "wevtapi.lib")

// WM_DPICHANGED arrived in the Win8.1 SDK headers (_WIN32_WINNT >= 0x0603);
// this app targets Vista (0x0600), so define it locally. The message is simply
// ignored by pre-8.1 systems, which never send it.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// accent-color change broadcast (Vista+; absent from pre-7 SDK headers)
#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif

// theme/system helpers defined with the other theming statics further down,
// but used from the constructor and ReadConfig-adjacent code above them
static BOOL IsHighContrast();
static int QuerySystemDark();
static COLORREF GetAccentColor(COLORREF fallback, BOOL darkBg);
static void ApplyDwmChromeColors(HWND hwnd, BOOL dark, BOOL reset);
static void ShutdownGdiplus();   // sparkline GDI+ teardown (defined with DrawSparkline)
static BOOL CALLBACK SetFontChildProc(HWND h, LPARAM lp);   // defined near RescaleForDpi
static DWORD WINAPI ModernStandbyCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action,
	PVOID ctx, EVT_HANDLE hEvent);   // defined near OnSleepTransition


DEFINE_GUID(GUID_LIDSWITCH_STATE_CHANGE,
    0xba3e0f4d, 0xb817, 0x4094,
    0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3);

//-------------------------------------------------------------------------
//  constructor
//-------------------------------------------------------------------------
FANCONTROL::FANCONTROL(HINSTANCE hinstapp)
	: hinstapp(hinstapp),
	hwndDialog(NULL),
	CurrentMode(-1),
	PreviousMode(-1),
	Cycle(5),
	IconCycle(1),
	ReIcCycle(0),
	NoExtSensor(0),
	FanSpeedLowByte(0x84),
	CurrentIcon(-1),
	hThread(NULL),
	FanBeepFreq(440),
	FanBeepDura(50),
	ReadErrorCount(0),
	MaxReadErrors(10),
	NoBallons(0),
	HK_BIOS_Method(0),
	HK_BIOS(0),
	HK_Manual_Method(0),
	HK_Manual(0),
	HK_Smart_Method(0),
	HK_Smart(0),
	HK_SM1_Method(0),
	HK_SM1(0),
	HK_SM2_Method(0),
	HK_SM2(0),
	HK_TG_BS_Method(0),
	HK_TG_BS(0),
	HK_TG_BM_Method(0),
	HK_TG_BM(0),
	HK_TG_MS_Method(0),
	HK_TG_MS(0),
	HK_TG_12_Method(0),
	HK_TG_12(0),
	EC_DATA(0),
	EC_CTRL(0),
	ManModeExit(80),
	ManModeExitInternal(80),
	ShowBiasedTemps(0),
	SecWinUptime(0),
	SlimDialog(0),
	SecStartDelay(0),
	Log2File(0),
	StayOnTop(0),
	Log2csv(0),
	ShowAll(0),
	ShowTempIcon(1),
	pTaskbarIcon(NULL),
	Fahrenheit(FALSE),
	MinimizeToSysTray(TRUE),
	IconColorFan(FALSE),
	Lev64Norm(FALSE),
	StartMinimized(FALSE),
	NoWaitMessage(TRUE),
	MinimizeOnClose(TRUE),
	Runs_as_service(FALSE),
	ActiveMode(false),
	ManFanSpeed(7),
	UseTWR(0),
	FinalSeen(false),
	m_fanTimer(NULL),
	m_titleTimer(NULL),
	m_iconTimer(NULL),
	m_renewTimer(NULL),
	m_needClose(false),
	m_hinstapp(hinstapp),
	ppTbTextIcon(NULL),
	pTextIconMutex(new MUTEXSEM(0, "Global\\TPFanControl_ppTbTextIcon")) {
	int i = 0;
	char buf[256] = "";

	// log-tail lock first: Trace() can run (and record) before the window or
	// any other state exists
	::InitializeCriticalSection(&this->m_logLock);

	// theme defaults (overridable via TPFanControl.ini, toggled in-app)
	this->ShowTempHex = 0;
	this->ShowLog = 0;
	this->DarkMode = 1;
	this->DarkModeSetting = 1;   // 0=light, 1=dark, 2=follow the system theme
	this->ShowGraph = 1;
	this->ShowInTaskbar = 0;     // default: tray-only tool window, like always
	this->SuspendMode = 1;       // default: hand fan to BIOS across sleep, restore after
	// read by UpdateTaskbarIndicators before the first successful EC poll
	// assigns it in HandleData; BIOS bit = clean no-progress taskbar state
	this->fanctrl2 = FAN_CTRL_BIOS;
	// Detect if TVic drivers were left hidden (e.g., after a previous crash in game mode).
	// Require both files to be absent/backed-up; a partial state leaves m_driversHidden false.
	// Disable WOW64 FS redirection: this is a 32-bit process; without this, System32 maps to
	// SysWOW64 and the driver files are never found.
	{
		typedef BOOL (WINAPI *PFN_Disable)(PVOID*);
		typedef BOOL (WINAPI *PFN_Revert)(PVOID);
		HMODULE hK = ::GetModuleHandleA("kernel32.dll");
		PFN_Disable pfnOff = hK ? (PFN_Disable)::GetProcAddress(hK, "Wow64DisableWow64FsRedirection") : NULL;
		PFN_Revert  pfnOn  = hK ? (PFN_Revert) ::GetProcAddress(hK, "Wow64RevertWow64FsRedirection")  : NULL;
		PVOID fsOld = NULL;
		bool redir = pfnOff && pfnOff(&fsOld);
		this->m_driversHidden =
			(::GetFileAttributesA("C:\\Windows\\System32\\drivers\\TVicHW64.sys")       == INVALID_FILE_ATTRIBUTES) &&
			(::GetFileAttributesA("C:\\Windows\\System32\\drivers\\TVicHW64.sys.bak")   != INVALID_FILE_ATTRIBUTES) &&
			(::GetFileAttributesA("C:\\Windows\\System32\\drivers\\TVicPort64.sys")     == INVALID_FILE_ATTRIBUTES) &&
			(::GetFileAttributesA("C:\\Windows\\System32\\drivers\\TVicPort64.sys.bak") != INVALID_FILE_ATTRIBUTES);
		if (redir) pfnOn(fsOld);
	}
	this->m_hbrDlg = NULL;
	this->m_hbrField = NULL;
	this->m_hbrRule = NULL;
	this->m_hFontHdr = NULL;
	this->m_hFontBig = NULL;
	this->m_hFontTitle = NULL;
	this->m_hFontDlg = NULL;
	this->m_curDpi = 0;
	this->m_inDpiChange = FALSE;
	this->m_clrText = RGB(32, 32, 32);
	this->m_clrTextDim = RGB(96, 96, 96);
	this->m_clrAccent = RGB(0, 120, 212);
	this->m_highContrast = FALSE;
	this->m_fullW = 0;
	this->m_layoutInit = FALSE;
	this->m_baseCW = this->m_baseCH = this->m_minW = this->m_minH = 0;
	this->m_tempHistCount = 0;
	this->m_tempHistHead = 0;
	memset(this->m_tempHist, 0, sizeof(this->m_tempHist));
	this->ApplyTheme();   // create initial brushes (window themed after it exists)

	// SensorNames
		// 78-7F (state index 0-7)
	strcpy_s(this->gSensorNames[0], sizeof(this->gSensorNames[0]), "cpu"); // main processor
	strcpy_s(this->gSensorNames[1], sizeof(this->gSensorNames[1]), "aps"); // harddisk protection gyroscope
	strcpy_s(this->gSensorNames[2], sizeof(this->gSensorNames[2]), "crd"); // under PCMCIA slot (front left)
	strcpy_s(this->gSensorNames[3], sizeof(this->gSensorNames[3]), "gpu"); // graphical processor
	strcpy_s(this->gSensorNames[4], sizeof(this->gSensorNames[4]), "bat"); // inside T43 battery
	strcpy_s(this->gSensorNames[5], sizeof(this->gSensorNames[5]), "x7d"); // usually n/a
	strcpy_s(this->gSensorNames[6], sizeof(this->gSensorNames[6]), "bat"); // inside T43 battery
	strcpy_s(this->gSensorNames[7], sizeof(this->gSensorNames[7]), "x7f"); // usually n/a
//  	// C0-C4 (state index 8-11)
	strcpy_s(this->gSensorNames[8], sizeof(this->gSensorNames[8]), "bus"); // unknown
	strcpy_s(this->gSensorNames[9], sizeof(this->gSensorNames[9]), "pci"); // mini-pci, WLAN, southbridge area
	strcpy_s(this->gSensorNames[10], sizeof(this->gSensorNames[10]), "pwr"); // power supply (get's hot while charging battery)
	strcpy_s(this->gSensorNames[11], sizeof(this->gSensorNames[11]), "xc3"); // usually n/a
	// future
	strcpy_s(this->gSensorNames[12], sizeof(this->gSensorNames[12]), "");
	strcpy_s(this->gSensorNames[13], sizeof(this->gSensorNames[13]), "");
	strcpy_s(this->gSensorNames[14], sizeof(this->gSensorNames[14]), "");
	strcpy_s(this->gSensorNames[15], sizeof(this->gSensorNames[15]), "");
	strcpy_s(this->gSensorNames[16], sizeof(this->gSensorNames[16]), "");

	// clear title strings
	setzero(this->Title, sizeof(this->Title));
	setzero(this->Title2, sizeof(this->Title2));
	setzero(this->Title3, sizeof(this->Title3));
	setzero(this->Title5, sizeof(this->Title5));
	setzero(this->LastTitle, sizeof(this->LastTitle));
	setzero(this->TrayTip, sizeof(this->TrayTip));
	setzero(this->CurrentStatus, sizeof(this->CurrentStatus));
	setzero(this->CurrentStatuscsv, sizeof(this->CurrentStatuscsv));
	setzero(this->IgnoreSensors, sizeof(this->IgnoreSensors));

	this->IconLevels[0] = 50;    // yellow icon level
	this->IconLevels[1] = 55;    // orange icon level
	this->IconLevels[2] = 60;    // red icon level

	// initial default "smart" table
	setzero(this->SmartLevels, sizeof(this->SmartLevels));
	this->SmartLevels[i].temp = 50;
	this->SmartLevels[i].fan = 0;
	i++;
	this->SmartLevels[i].temp = 55;
	this->SmartLevels[i].fan = 3;
	i++;
	this->SmartLevels[i].temp = 60;
	this->SmartLevels[i].fan = 5;
	i++;
	this->SmartLevels[i].temp = 65;
	this->SmartLevels[i].fan = 7;
	i++;
	this->SmartLevels[i].temp = 70;
	this->SmartLevels[i].fan = 128;
	i++;
	this->SmartLevels[i].temp = -1;
	this->SmartLevels[i].fan = 0;
	i++;

	setzero(this->SmartLevels1, sizeof(this->SmartLevels1));
	i = 0;
	this->SmartLevels1[i].temp1 = 50;
	this->SmartLevels1[i].fan1 = 0;
	i++;
	this->SmartLevels1[i].temp1 = 55;
	this->SmartLevels1[i].fan1 = 3;
	i++;
	this->SmartLevels1[i].temp1 = 60;
	this->SmartLevels1[i].fan1 = 5;
	i++;
	this->SmartLevels1[i].temp1 = 65;
	this->SmartLevels1[i].fan1 = 7;
	i++;
	this->SmartLevels1[i].temp1 = 70;
	this->SmartLevels1[i].fan1 = 128;
	i++;
	this->SmartLevels1[i].temp1 = -1;
	this->SmartLevels1[i].fan1 = 0;
	i++;

	setzero(this->SmartLevels2, sizeof(this->SmartLevels2));
	i = 0;
	this->SmartLevels2[i].temp2 = 0;
	this->SmartLevels2[i].fan2 = 0;
	i++;
	this->SmartLevels2[i].temp2 = 55;
	this->SmartLevels2[i].fan2 = 3;
	i++;
	this->SmartLevels2[i].temp2 = 60;
	this->SmartLevels2[i].fan2 = 5;
	i++;
	this->SmartLevels2[i].temp2 = 65;
	this->SmartLevels2[i].fan2 = 7;
	i++;
	this->SmartLevels2[i].temp2 = 70;
	this->SmartLevels2[i].fan2 = 128;
	i++;
	this->SmartLevels2[i].temp2 = -1;
	this->SmartLevels2[i].fan2 = 0;
	i++;

	// Title3 is appended to the window title and just needs a leading blank.
	// (Replaces a pointless 111-iteration loop left over from the original code.)
	this->Title3[0] = ' ';

	// read config file
	this->ReadConfig("TPFanControl.ini");

	// resolve the effective theme now that the setting is known (2 = follow the
	// system); InitThemeAndChrome below re-applies it to the live window
	this->DarkMode = (this->DarkModeSetting == 2) ? QuerySystemDark()
	                                              : (this->DarkModeSetting != 0);

	if (this->hwndDialog) {
		::GetWindowText(this->hwndDialog, this->Title, sizeof(this->Title));

		strcat_s(this->Title, sizeof(this->Title), this->Title3);

		::SetWindowText(this->hwndDialog, this->Title);

		::SetWindowLongPtr(this->hwndDialog, GWLP_USERDATA, (LONG_PTR)this);

		::SendDlgItemMessage(this->hwndDialog, 8112, EM_LIMITTEXT, 256, 0);

		// 0 = no practical limit: EM_REPLACESEL (Trace's append path) respects
		// EM_LIMITTEXT, and the 100-line trim in Trace() bounds the real size
		::SendDlgItemMessage(this->hwndDialog, 9200, EM_LIMITTEXT, 0, 0);

		_itoa_s(this->ManFanSpeed, buf, 10);

		::SetDlgItemText(this->hwndDialog, 8310, buf);
		this->hPowerNotify = RegisterPowerSettingNotification(this->hwndDialog, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE);
		this->InitThemeAndChrome();
	}

	if (SlimDialog == 1) {
		// ReadConfig already created the non-slim dialog; destroy it before
		// creating the slim one instead of overwriting hwndDialog (window leak).
		// The power-setting notification is bound to that window, so drop it too.
		if (this->hwndDialog) {
			UnregisterPowerSettingNotification(this->hPowerNotify);
			this->hPowerNotify = NULL;
			::DestroyWindow(this->hwndDialog);
			this->hwndDialog = NULL;
		}
		if (this->StayOnTop)
			this->hwndDialog = ::CreateDialogParam(hinstapp,
				MAKEINTRESOURCE(9001),
				HWND_DESKTOP,
				(DLGPROC)BaseDlgProc,
				(LPARAM)
				this);
		else
			this->hwndDialog = ::CreateDialogParam(hinstapp,
				MAKEINTRESOURCE(9003),
				HWND_DESKTOP,
				(DLGPROC)BaseDlgProc,
				(LPARAM)
				this);

		if (this->hwndDialog) {
			::GetWindowText(this->hwndDialog, this->Title, sizeof(this->Title));

			strcat_s(this->Title, sizeof(this->Title), ".63 multiHotKey");

			if (SlimDialog == 0) strcat_s(this->Title, sizeof(this->Title), this->Title3);

			::SetWindowText(this->hwndDialog, this->Title);

			::SetWindowLongPtr(this->hwndDialog, GWLP_USERDATA, (LONG_PTR)this);

			::SendDlgItemMessage(this->hwndDialog, 8112, EM_LIMITTEXT, 256, 0);

			// 0 = no practical limit: EM_REPLACESEL (Trace's append path) respects
		// EM_LIMITTEXT, and the 100-line trim in Trace() bounds the real size
		::SendDlgItemMessage(this->hwndDialog, 9200, EM_LIMITTEXT, 0, 0);

			_itoa_s(this->ManFanSpeed, buf, 10);

			::SetDlgItemText(this->hwndDialog, 8310, buf);

			this->ShowAllToDialog(ShowAll);

			this->hPowerNotify = RegisterPowerSettingNotification(this->hwndDialog, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE);
			this->InitThemeAndChrome();
		}
	}

	// Opt-in taskbar presence (ShowInTaskbar=1): join the taskbar, Alt-Tab
	// and Win11 Snap Layouts, with severity overlay + fan-level progress.
	// Applied here, after the slim-dialog swap, so it targets the final
	// window; also re-applied live when toggled in Settings.
	if (this->ShowInTaskbar)
		this->ApplyTaskbarPresence();

	// Modern Standby (S0) watcher: classic PBT_APMSUSPEND/RESUME messages are
	// not delivered reliably on S0-idle machines (this one included), so also
	// watch Kernel-Power events 506 (S0 entry) / 507 (S0 exit). The callback
	// only posts to the window - see ModernStandbyCallback.
	if (this->SuspendMode != 0 && this->hwndDialog) {
		this->m_hEvtSub = ::EvtSubscribe(NULL, NULL, L"System",
			L"*[System[Provider[@Name='Microsoft-Windows-Kernel-Power']"
			L" and (EventID=506 or EventID=507)]]",
			NULL, (PVOID)this->hwndDialog, ModernStandbyCallback,
			EvtSubscribeToFutureEvents);
		if (!this->m_hEvtSub)
			this->Trace("Modern Standby watcher unavailable (EvtSubscribe failed)");
	}

	// Field tooltips: demystify the terse main-window labels. Registered here, on
	// the *final* window (after the optional slim-dialog swap above), so the tips
	// are not orphaned by that rebuild. AddTip creates the shared tip window on
	// first call and silently skips controls a given layout doesn't have, so the
	// slim dialogs (no Fan 1/2, Game Mode, etc.) just get the subset that applies.
	this->AddTip(8100, "Current fan-control state read back from the embedded "
	                   "controller (e.g. the active fan level or BIOS auto mode).");
	this->AddTip(8103, "Switch: the raw fan-control byte currently programmed into "
	                   "the EC. 0x00-0x07 are speed levels, 0x40 = full speed, "
	                   "0x80 = hand control back to the BIOS.");
	this->AddTip(8102, "Fan 1 tachometer reading (RPM) on dual-fan machines.");
	this->AddTip(8104, "Fan 2 tachometer reading (RPM) on dual-fan machines.");
	this->AddTip(8300, "BIOS mode: let the embedded controller run the fans on its "
	                   "own thermal table. Safest; TPFanControl only monitors.");
	this->AddTip(8301, "Smart mode: TPFanControl drives the fan from the curve in "
	                   "TPFanControl.ini (the Level= lines), with hysteresis.");
	this->AddTip(8302, "Manual mode: hold a fixed fan level you choose with the box "
	                   "and slider below. The fan will not auto-adjust.");
	this->AddTip(8310, "Manual fan level: 0 = off, 1-7 = increasing speed, "
	                   "64 = maximum. Select Manual mode first to enable this box.");
	this->AddTip(8311, "Drag to set the manual fan level: 0 = off, 1-7 = increasing "
	                   "speed, far right = MAX. Using the slider switches to Manual mode.");
	this->AddTip(8101, "Per-sensor temperatures. 'active' shows only sensors with a "
	                   "live reading; 'all' lists every EC sensor slot.");
	this->AddTip(7001, "Show every EC temperature slot, including idle/unused sensors.");
	this->AddTip(7002, "Show only sensors that currently report a live reading.");
	// (no static tip on the sparkline 8120: it has a live tracking tooltip that
	// shows the hovered sample's value and age - see SparkSubclassProc)
	this->AddTip(7013, "Renames TVicHW64.sys and TVicPort64.sys to .sys.bak "
	                   "in System32\\drivers, hiding them from Valorant's Vanguard "
	                   "anti-cheat (ring 0 kernel access). Files are automatically "
	                   "restored when Game Mode is disabled or the app exits cleanly.");
	this->AddTip(5100, "Open Settings: poll interval, icon color thresholds, thermal "
	                   "fail-safe and the fan curve.");
	this->AddTip(7011, "Show or hide the scrolling Log panel on the right side of the window.");
	this->AddTip(7012, "Switch between the light and dark color scheme.");
	this->AddTip(8112, "One-line status: current fan byte, switch temperature and all "
	                   "sensor readings.");
	this->AddTip(8113, "Result and timestamp of the most recent set-fan action.");
	this->AddTip(8115, "On = TPFanControl is driving the fan (Smart/Manual). "
	                   "OFF = the BIOS controls it.");

	// restore the main window to where the user last left it (on-screen only)
	this->RestoreWindowPos();

	// Log panel defaults to closed on every startup. RestoreWindowPos() may have
	// forced a saved width that disagrees with the checkbox, so collapse the panel
	// and re-sync the width here -- this also fixes the old bug where the box read
	// "checked" but the panel stayed clipped until you toggled it off and on.
	this->ShowLog = 0;
	::SendDlgItemMessage(this->hwndDialog, 7011, BM_SETCHECK, BST_UNCHECKED, 0);
	this->ApplyLogVisibility();

	//  wait xx seconds to start tpfc while booting to save icon
	char bufsec[1024] = "";
	ULONGLONG tickCount = GetTickCount64();   // 64-bit: no ~49-day wrap on long uptimes

	sprintf_s(bufsec, sizeof(bufsec), "Windows uptime since boot %d sec., SecWinUptime= %d sec.", (int)(tickCount / 1000), SecWinUptime);

	this->Trace(bufsec);

	if ((tickCount / 1000) <= (ULONGLONG)SecWinUptime) {
		sprintf_s(bufsec, sizeof(bufsec), "Save the icon by a start delay of %d seconds (SecStartDelay)", SecStartDelay);

		this->Trace(bufsec);

		if (!NoWaitMessage) {
			sprintf_s(bufsec, sizeof(bufsec),
				"TPFanControl v2.34 P15G2 Dual is started %d sec. after\nboot time (SecWinUptime=%d sec.)\n\nTo prevent missing systray icons\nand communication errors between\nTPFanControl v2.34 P15G2 Dual and embedded controller\nit will sleep for %d sec. (SecStartDelay)\n\nTo void this message box please set\nNoWaitMessage=1 in TPFanControl.ini",
				(int)(tickCount / 1000), SecWinUptime, SecStartDelay);

			// Don't show message box when running as service on Vista+
			OSVERSIONINFOEX osvi = {};
			osvi.dwOSVersionInfoSize = sizeof(osvi);
			osvi.dwMajorVersion = 6;
			DWORDLONG cond = 0;
			VER_SET_CONDITION(cond, VER_MAJORVERSION, VER_GREATER_EQUAL);
			bool isVistaOrLater = VerifyVersionInfoA(&osvi, VER_MAJORVERSION, cond) != FALSE;
			if (isVistaOrLater && Runs_as_service == TRUE)
				;
			else
				MessageBox(NULL, bufsec, "TPFanControl v2.34 P15G2 Dual is sleeping", MB_ICONEXCLAMATION);
		}
	}

	// sleep until start time + delay time
	if ((GetTickCount64() / 1000) <= (ULONGLONG)SecWinUptime) {
		while ((tickCount + (ULONGLONG)SecStartDelay * 1000) >= GetTickCount64())
			Sleep(200);
	}

	// taskbaricon (keep code after reading config)
	if (this->MinimizeToSysTray) {
		if (!this->ShowTempIcon) {
			this->pTaskbarIcon = new TASKBARICON(this->hwndDialog, 10, "TPFanControl v2.34 P15G2 Dual");
		}
		else {
			this->pTaskbarIcon = NULL;
		}
	}

	// read current fan control status and set mode buttons accordingly
	this->CurrentMode = this->ActiveMode;

	this->ModeToDialog(this->CurrentMode);

	this->PreviousMode = 1;

	if (HK_BIOS_Method) RegisterHotKey(this->hwndDialog, 1, HK_BIOS_Method, HK_BIOS);
	if (HK_Smart_Method) RegisterHotKey(this->hwndDialog, 2, HK_Smart_Method, HK_Smart);
	if (HK_Manual_Method) RegisterHotKey(this->hwndDialog, 3, HK_Manual_Method, HK_Manual);
	if (HK_SM1_Method) RegisterHotKey(this->hwndDialog, 4, HK_SM1_Method, HK_SM1);
	if (HK_SM2_Method) RegisterHotKey(this->hwndDialog, 5, HK_SM2_Method, HK_SM2);
	if (HK_TG_BS_Method) RegisterHotKey(this->hwndDialog, 6, HK_TG_BS_Method, HK_TG_BS);
	if (HK_TG_BM_Method) RegisterHotKey(this->hwndDialog, 7, HK_TG_BM_Method, HK_TG_BM);
	if (HK_TG_MS_Method) RegisterHotKey(this->hwndDialog, 8, HK_TG_MS_Method, HK_TG_MS);
	if (HK_TG_12_Method) RegisterHotKey(this->hwndDialog, 9, HK_TG_12_Method, HK_TG_12);

	// enable/disable mode radiobuttons
	::EnableWindow(::GetDlgItem(this->hwndDialog, 8300), this->ActiveMode);
	::EnableWindow(::GetDlgItem(this->hwndDialog, 8301), this->ActiveMode);
	::EnableWindow(::GetDlgItem(this->hwndDialog, 8302), this->ActiveMode);
	// manual level box + slider: only active in Manual mode
	this->UpdateManualControlsEnabled();

	// make it call HandleControl initially
	::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);

	m_fanTimer = ::SetTimer(this->hwndDialog, 1, this->Cycle * 1000, NULL);    // fan update
	m_titleTimer = ::SetTimer(this->hwndDialog, 2, 500, NULL);                // title update
	// No dedicated icon timer (id 3): the tray-icon refresh runs unconditionally
	// after every WM_TIMER tick, so the 500ms title timer already updates it more
	// often than IconCycle would. m_iconTimer stays NULL; its KillTimer calls no-op.
	if (this->ReIcCycle)
		m_renewTimer = ::SetTimer(this->hwndDialog, 4, this->ReIcCycle * 1000, NULL); // Vista icon update

	if (!this->StartMinimized)
		::ShowWindow(this->hwndDialog, TRUE);

	if (this->StartMinimized)
		::ShowWindow(this->hwndDialog, SW_MINIMIZE);
}

//-------------------------------------------------------------------------
//  theme one tooltip window to match dark/light mode (the proven dark-tip
//  recipe: AllowDarkModeForWindow + the DarkMode_Explorer theme class). Tips
//  are owned popups, so the EnumChildWindows theming pass never reaches them.
//-------------------------------------------------------------------------
static void ThemeTipWindow(HWND hTip, BOOL dark) {
	if (!hTip)
		return;
	HMODULE hUx = ::LoadLibraryA("uxtheme.dll");
	if (hUx) {
		typedef bool (WINAPI* fnADMW)(HWND, bool);   // AllowDarkModeForWindow (ord 133)
		typedef HRESULT(WINAPI* fnSWT)(HWND, LPCWSTR, LPCWSTR);
		fnADMW pADMW = (fnADMW)::GetProcAddress(hUx, MAKEINTRESOURCEA(133));
		fnSWT  pSet  = (fnSWT) ::GetProcAddress(hUx, "SetWindowTheme");
		if (pADMW) pADMW(hTip, dark ? true : false);
		if (pSet)  pSet(hTip, dark ? L"DarkMode_Explorer" : NULL, NULL);
		::FreeLibrary(hUx);
	}
}

//-------------------------------------------------------------------------
//  register a tooltip on one main-dialog control (flat themed tip; rounded
//  automatically on Win11)
//-------------------------------------------------------------------------
void
FANCONTROL::AddTip(int ctrlId, const char* text) {
	HWND hwndCtl = ::GetDlgItem(this->hwndDialog, ctrlId);
	if (!hwndCtl)
		return;

	// create the shared tip window on first use
	if (!this->m_hwndTip) {
		this->m_hwndTip = ::CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
			WS_POPUP | TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			this->hwndDialog, NULL, this->hinstapp, NULL);
		if (!this->m_hwndTip)
			return;
		::SendMessage(this->m_hwndTip, TTM_SETMAXTIPWIDTH, 0, 320);
		::SendMessage(this->m_hwndTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000); // keep up while reading
		// ApplyTheme runs before this window exists, so theme it at creation
		// (and again from ApplyTheme's uxtheme block on every theme toggle)
		ThemeTipWindow(this->m_hwndTip, this->DarkMode && !this->m_highContrast);
	}

	TOOLINFO ti = {};
	ti.cbSize   = sizeof(TOOLINFO);
	ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
	ti.hwnd     = this->hwndDialog;
	ti.uId      = (UINT_PTR)hwndCtl;
	ti.lpszText = (LPSTR)text;
	::SendMessage(this->m_hwndTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
}

//-------------------------------------------------------------------------
//  copy the current readings (as shown in the dialog) to the clipboard
//-------------------------------------------------------------------------
void
FANCONTROL::CopyReadingsToClipboard() {
	char st[256] = "", sw[64] = "", f1[64] = "", f2[64] = "", temps[1024] = "";
	::GetDlgItemTextA(this->hwndDialog, 8100, st,    sizeof(st));     // State
	::GetDlgItemTextA(this->hwndDialog, 8103, sw,    sizeof(sw));     // Switch
	::GetDlgItemTextA(this->hwndDialog, 8102, f1,    sizeof(f1));     // Fan 1
	::GetDlgItemTextA(this->hwndDialog, 8104, f2,    sizeof(f2));     // Fan 2
	::GetDlgItemTextA(this->hwndDialog, 8101, temps, sizeof(temps));  // per-sensor temps

	char buf[2048];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"TPFanControl\r\n"
		"State:\t%s\r\nSwitch:\t%s\r\nFan 1:\t%s\r\nFan 2:\t%s\r\n"
		"Temperatures:\r\n%s\r\n",
		st, sw, f1, f2, temps);

	if (!::OpenClipboard(this->hwndDialog))
		return;
	::EmptyClipboard();
	size_t len = strlen(buf) + 1;
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, len);
	if (hMem) {
		void* p = ::GlobalLock(hMem);
		if (p) {
			memcpy(p, buf, len);
			::GlobalUnlock(hMem);
			::SetClipboardData(CF_TEXT, hMem);   // clipboard owns hMem now
		}
		else
			::GlobalFree(hMem);
	}
	::CloseClipboard();
}

//-------------------------------------------------------------------------
//  fade-aware show/hide for the tray-toggle paths (the most frequent
//  interaction with a tray app). AnimateWindow returns FALSE when it cannot
//  animate or the window is already in the target state, so every branch
//  falls back to the previous instant ShowWindow behavior.
//-------------------------------------------------------------------------
void
FANCONTROL::ShowMainWindow(bool show) {
	if (!this->hwndDialog) return;
	if (show) {
		if (this->IsMinimized()) {
			::ShowWindow(this->hwndDialog, SW_RESTORE);
		}
		else if (!::AnimateWindow(this->hwndDialog, 150, AW_BLEND)) {
			::ShowWindow(this->hwndDialog, TRUE);
		}
		// AW_BLEND needs children to handle WM_PRINTCLIENT; the owner-draw
		// statics (8115 status line, 8120 sparkline) don't, so repaint them
		::RedrawWindow(this->hwndDialog, NULL, NULL,
			RDW_INVALIDATE | RDW_ALLCHILDREN);
		// rebuild the (visibility-gated) temp list from the last good State
		// right away - the posted poll below may not complete for a while (or
		// at all during an EC error streak). Safe: ReadEcStatus only updates
		// State on success, so it always holds the last good reading.
		this->m_tempListSig[0] = '\0';
		this->m_tempListRows = -1;
		this->UpdateTempList();
		this->UpdateManualControlsEnabled();
		::SetForegroundWindow(this->hwndDialog);
		::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);   // refresh the now-visible controls promptly
	}
	else if (this->MinimizeToSysTray) {
		// fade straight out instead of the SW_MINIMIZE round-trip, which plays
		// the genie animation toward a taskbar button this tool window lacks
		if (!::AnimateWindow(this->hwndDialog, 150, AW_HIDE | AW_BLEND))
			::ShowWindow(this->hwndDialog, FALSE);
	}
	else {
		::ShowWindow(this->hwndDialog, SW_MINIMIZE);
	}
}

//-------------------------------------------------------------------------
//  apply the ShowInTaskbar setting to the live window: toggle the
//  WS_EX_TOOLWINDOW/WS_EX_APPWINDOW bits and bind ITaskbarList3 the first
//  time it is needed. Every COM step is failure-tolerant - on any failure
//  the window simply behaves like the classic tray-only tool window.
//-------------------------------------------------------------------------
void
FANCONTROL::ApplyTaskbarPresence() {
	if (!this->hwndDialog)
		return;

	LONG_PTR ex = ::GetWindowLongPtr(this->hwndDialog, GWL_EXSTYLE);
	LONG_PTR want = this->ShowInTaskbar
		? (ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW
		: (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW;
	if (want != ex) {
		::SetWindowLongPtr(this->hwndDialog, GWL_EXSTYLE, want);
		::SetWindowPos(this->hwndDialog, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	if (this->ShowInTaskbar) {
		if (!this->m_pTaskbar3) {
			if (!this->m_comInit && SUCCEEDED(::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
				this->m_comInit = true;
			if (this->m_comInit) {
				ITaskbarList3* p = NULL;
				if (SUCCEEDED(::CoCreateInstance(__uuidof(TaskbarList), NULL,
						CLSCTX_INPROC_SERVER, __uuidof(ITaskbarList3), (void**)&p)) && p) {
					if (SUCCEEDED(p->HrInit()))
						this->m_pTaskbar3 = p;
					else
						p->Release();
				}
			}
		}
		this->m_lastTbSig = -1;   // push overlay/progress on the next tick
	}
	else if (this->m_pTaskbar3) {
		// leaving the taskbar: clear our decorations from the departing button
		this->m_pTaskbar3->SetOverlayIcon(this->hwndDialog, NULL, NULL);
		this->m_pTaskbar3->SetProgressState(this->hwndDialog, TBPF_NOPROGRESS);
		this->m_lastTbSig = -1;
	}
}

//-------------------------------------------------------------------------
//  sleep/resume handling (idea: FanDjango fork 2.3.12/2.3.13; upstream
//  issues #94/#61). On sleep entry, SuspendMode picks the behavior:
//    0 = ignore sleep entirely
//    1 = hand the fan to the BIOS for the duration (default - a fixed
//        manual/smart level must not persist while the EC drifts through
//        standby power states), restore the saved mode after resume
//    2 = keep the current mode, but still re-assert it after resume
//        (firmware can reset the fan register during sleep)
//  On resume, EC access is deferred ~10s (the EC can return garbage or NAK
//  right after wake); the WM__GETDATA gate does the deferred restore.
//  UI thread only - Modern Standby events arrive via posted WM__SLEEPEVT.
//-------------------------------------------------------------------------
void
FANCONTROL::OnSleepTransition(bool entering) {
	if (this->SuspendMode == 0)
		return;

	if (entering) {
		if (this->m_savedSleepMode >= 0 || this->CurrentMode < 0)
			return;   // already inside a sleep window (APM + S0 can both fire)
		this->m_savedSleepMode = this->CurrentMode;
		if (this->SuspendMode == 1 && this->CurrentMode != 1) {
			this->Trace("Sleep transition: handing fan control to BIOS");
			this->ModeToDialog(1);
			this->SetFan("Sleep, switch to BIOS mode", FAN_CTRL_BIOS);
		}
		else
			this->Trace("Sleep transition detected (keeping mode)");
	}
	else {
		this->m_ecResumeDeferUntil = ::GetTickCount64() + 10000;
		this->Trace("Resume detected - deferring EC access (10s)");
	}
}

//-------------------------------------------------------------------------
//  Modern Standby (S0) watcher callback. Runs on an event-log worker thread,
//  so it must not touch the EC or any UI state: it renders the event XML,
//  picks out Kernel-Power EventID 506 (S0 entry) / 507 (S0 exit), and posts
//  WM__SLEEPEVT to the main window (ctx). Errors are silently ignored - the
//  APM path still covers classic sleep.
//-------------------------------------------------------------------------
static DWORD WINAPI ModernStandbyCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action,
	PVOID ctx, EVT_HANDLE hEvent) {
	if (action != EvtSubscribeActionDeliver || !ctx)
		return ERROR_SUCCESS;

	DWORD used = 0, props = 0;
	wchar_t xml[2048] = L"";
	if (::EvtRender(NULL, hEvent, EvtRenderEventXml,
			sizeof(xml) - sizeof(wchar_t), xml, &used, &props)) {
		// match ">506</EventID>" so an EventID element with a Qualifiers
		// attribute still hits
		bool entry = wcsstr(xml, L">506</EventID>") != NULL;
		bool exit2 = wcsstr(xml, L">507</EventID>") != NULL;
		if (entry || exit2)
			::PostMessage((HWND)ctx, WM__SLEEPEVT, entry ? 1 : 0, 0);
	}
	return ERROR_SUCCESS;
}

//-------------------------------------------------------------------------
//  taskbar button extras (opt-in via ShowInTaskbar=1): colored severity dot
//  as the overlay badge, fan level 0-8 as the progress fill (paused/error
//  states mirror the warm/critical thresholds). Deduped by signature so the
//  500ms timer doesn't spam cross-process COM calls.
//-------------------------------------------------------------------------
void
FANCONTROL::UpdateTaskbarIndicators() {
	if (!this->m_pTaskbar3 || !this->ShowInTaskbar || !this->hwndDialog)
		return;

	// overlay: the temperature-severity ids (11-14) map to icon resources;
	// neutral (10) and the fan-speed greens (21-25) clear the badge
	int id = this->iFarbeIconB;
	int overlayId = (id >= 11 && id <= 14) ? id : 0;

	// progress: current fan level out of 8 (0x40 "max" shows full)
	int  level = this->fanctrl2 & 0x7f;
	bool bios  = (this->fanctrl2 & FAN_CTRL_BIOS) != 0;
	int prog = 0, state = 0;   // 0 = TBPF_NOPROGRESS (BIOS mode: no bar)
	if (!bios) {
		prog = (level >= 8) ? 8 : level;   // FAN_CTRL_FULL (0x40) -> full bar
		state = (id == 14 || this->m_failsafeTripped) ? TBPF_ERROR
		      : (id == 12 || id == 13)                ? TBPF_PAUSED
		                                              : TBPF_NORMAL;
	}

	int sig = (overlayId << 16) | (state << 8) | prog;
	if (sig == this->m_lastTbSig)
		return;
	this->m_lastTbSig = sig;

	if (overlayId) {
		HICON h = NULL;
		if (SUCCEEDED(::LoadIconMetric(this->m_hinstapp,
				MAKEINTRESOURCEW(overlayId), LIM_SMALL, &h)) && h) {
			this->m_pTaskbar3->SetOverlayIcon(this->hwndDialog, h, L"temperature state");
			::DestroyIcon(h);   // the taskbar copies it
		}
	}
	else
		this->m_pTaskbar3->SetOverlayIcon(this->hwndDialog, NULL, NULL);

	if (state == 0) {
		this->m_pTaskbar3->SetProgressState(this->hwndDialog, TBPF_NOPROGRESS);
	}
	else {
		this->m_pTaskbar3->SetProgressState(this->hwndDialog, (TBPFLAG)state);
		this->m_pTaskbar3->SetProgressValue(this->hwndDialog, (ULONGLONG)prog, 8);
	}
}

//-------------------------------------------------------------------------
//  build and pop the tray context menu. Factored out of the WM__TASKBAR
//  handler so the v4 WM_CONTEXTMENU path can anchor it at the icon (anchor)
//  while the legacy path keeps popping at the cursor (NULL).
//-------------------------------------------------------------------------
void
FANCONTROL::ShowTrayMenu(const POINT* anchor) {
	unsigned char testpara = 0;   // ReadByteFromEC leaves *pdata untouched on failure
	MENU m(5000);

	if (!this->LockECAccess()) return;

	this->ReadByteFromEC(59, &testpara);
	if (testpara & 2)
		m.CheckMenuItem(5060);

	int mode = this->CurrentModeFromDialog();
	if (mode == 1) {
		m.CheckMenuItem(5001);

		if (this->ActiveMode == 0) {
			m.DisableMenuItem(5002);  // v0.25
			m.DisableMenuItem(5003);  // v0.25
			m.DisableMenuItem(5004);  // v0.25
			m.DisableMenuItem(5005);  // v0.25
		}
	}
	else
		if (mode == 2)
			m.CheckMenuItem(5002);

	if (mode == 3)
		m.CheckMenuItem(5005);

	m.InsertItem(this->MenuLabelSM1, 5003, 10);
	m.InsertItem(this->MenuLabelSM2, 5004, 11);

	if (this->SmartLevels2[0].temp2 == 0)
	{
		m.DeleteMenuItem(5003);
		m.DeleteMenuItem(5004);
	}

	if (this->SmartLevels2[0].temp2 != 0)
	{
		m.DeleteMenuItem(5002);

		if (mode == 2 && this->IndSmartLevel == 0)
			m.CheckMenuItem(5003);

		if (mode == 2 && this->IndSmartLevel != 0)
			m.CheckMenuItem(5004);
	}

	if (Runs_as_service)
		m.DeleteMenuItem(5020);

	if (!IsWindowVisible(this->hwndDialog))
		m.DeleteMenuItem(5030);

	if (IsWindowVisible(this->hwndDialog))
		m.DeleteMenuItem(5010);

	if (this->m_driversHidden)
		m.CheckMenuItem(5090);

	if (this->ShowTempIcon == 0)
		m.DeleteMenuItem(5070);

	if (this->ShowTempIcon == 1)
		m.DeleteMenuItem(5080);

	// grey "Open Log File" when no log file exists yet (logging off and none
	// left from a prior session), so the user doesn't click into an error box
	if (::GetFileAttributesA("TPFanControl.log") == INVALID_FILE_ATTRIBUTES)
		m.DisableMenuItem(5120);

	// bold the item a double-click would trigger (the surviving Show/Hide)
	m.SetDefaultItem(IsWindowVisible(this->hwndDialog) ? 5030 : 5010);

	this->FreeECAccess();

	POINT pt;
	if (anchor) pt = *anchor;
	m.Popup(this->hwndDialog, anchor ? &pt : NULL);
}

//-------------------------------------------------------------------------
//  modern confirmation via TaskDialogIndirect (comctl32 v6, Vista+): native
//  Win11 chrome for the prompts that were classic MessageBoxA boxes. Falls
//  back to MessageBoxA if the call fails. Returns the pressed button id.
//  verify/pVerified: optional "don't ask again"-style checkbox.
//-------------------------------------------------------------------------
static int ModernConfirm(HWND owner, PCWSTR title, PCWSTR instruction,
	PCWSTR content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon,
	const char* fbText, const char* fbCaption, UINT fbFlags,
	PCWSTR verify = NULL, BOOL* pVerified = NULL) {
	TASKDIALOGCONFIG tdc = { sizeof(tdc) };
	tdc.hwndParent = owner;
	tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
	tdc.dwCommonButtons = buttons;
	tdc.pszWindowTitle = title;
	tdc.pszMainIcon = icon;
	tdc.pszMainInstruction = instruction;
	tdc.pszContent = content;
	tdc.pszVerificationText = verify;
	int btn = 0;
	if (SUCCEEDED(::TaskDialogIndirect(&tdc, &btn, NULL, pVerified)) && btn)
		return btn;
	return ::MessageBoxA(owner, fbText, fbCaption, fbFlags);
}

//-------------------------------------------------------------------------
//  the "fan = maximum (64)" confirmation, shared by the slider and the typed
//  edit-box path so the warning can't be bypassed by typing the value.
//  Returns true to proceed to max, false if the user cancelled.
//-------------------------------------------------------------------------
bool
FANCONTROL::ConfirmMaxFan() {
	if (this->m_maxConfirmSuppressed)
		return true;

	BOOL dontAsk = FALSE;
	int rc = ModernConfirm(this->hwndDialog,
		L"Maximum fan speed",
		L"Set the fan to maximum?",
		L"Maximum (64) runs the fan at full, unregulated speed. This is loud "
		L"and bypasses normal speed regulation; the firmware keeps full power "
		L"until you change the level. Use it only briefly to cool down a hot "
		L"machine.",
		TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_WARNING_ICON,
		"Setting the fan to maximum (64) runs it at full, "
		"unregulated speed.\r\n\r\n"
		"This is loud and bypasses normal speed regulation; "
		"the firmware keeps the fan at full power until you "
		"change the level. Use it only briefly to cool down a "
		"hot machine.\r\n\r\nSet fan to maximum?",
		"Maximum fan speed",
		MB_OKCANCEL | MB_ICONWARNING,
		L"Don't ask again until the next start", &dontAsk);

	if (rc == IDOK && dontAsk)
		this->m_maxConfirmSuppressed = true;
	return rc == IDOK;
}

//-------------------------------------------------------------------------
//  destructor
//-------------------------------------------------------------------------
FANCONTROL::~FANCONTROL() {
	// stop the Modern Standby watcher first: its callback posts to the window
	// this destructor is about to destroy
	if (this->m_hEvtSub) {
		::EvtClose(this->m_hEvtSub);
		this->m_hEvtSub = NULL;
	}

	if (this->m_driversHidden)
		this->ToggleGameMode(true);   // restore TVic drivers on clean exit (no UI)

	if (this->m_hwndTip) {
		::DestroyWindow(this->m_hwndTip);
		this->m_hwndTip = NULL;
	}

	if (this->hThread) {
		// close the handle only after a confirmed clean exit; on timeout we leak
		// the handle rather than risk closing one still in use by a live thread
		if (::WaitForSingleObject(this->hThread, 2000) == WAIT_OBJECT_0)
			::CloseHandle(this->hThread);
		this->hThread = NULL;
	}

	if (this->pTaskbarIcon) {
		delete this->pTaskbarIcon;
		this->pTaskbarIcon = NULL;
	}

	if (this->ppTbTextIcon) {
		delete ppTbTextIcon[0];
		delete[] ppTbTextIcon;
		ppTbTextIcon = NULL;
	}
	if (this->m_pTaskbar3) {
		this->m_pTaskbar3->Release();
		this->m_pTaskbar3 = NULL;
	}
	if (this->m_comInit)
		::CoUninitialize();
	// guarded: registration only happens when a dialog was created, so an
	// early-exit path must not hand a never-assigned handle to the API
	if (this->hPowerNotify) {
		UnregisterPowerSettingNotification(this->hPowerNotify);
		this->hPowerNotify = NULL;
	}
	if (this->hwndDialog)
		::DestroyWindow(this->hwndDialog);

	if (this->m_hIconSm)  ::DestroyIcon(this->m_hIconSm);
	if (this->m_hIconBig) ::DestroyIcon(this->m_hIconBig);

	if (this->m_hbrDlg) ::DeleteObject(this->m_hbrDlg);
	if (this->m_hbrField) ::DeleteObject(this->m_hbrField);
	if (this->m_hbrRule) ::DeleteObject(this->m_hbrRule);
	if (this->m_hFontHdr) ::DeleteObject(this->m_hFontHdr);
	if (this->m_hFontBig) ::DeleteObject(this->m_hFontBig);
	if (this->m_hFontTitle) ::DeleteObject(this->m_hFontTitle);
	if (this->m_hFontDlg) ::DeleteObject(this->m_hFontDlg);

	// release the cached sparkline back-buffer (process-lifetime, recreated on resize)
	if (this->m_sparkBmp) ::DeleteObject(this->m_sparkBmp);
	if (this->m_sparkDC)  ::DeleteDC(this->m_sparkDC);
	ShutdownGdiplus();   // no-op unless the antialiased trace ever drew

	if (pTextIconMutex)
		delete pTextIconMutex;

	// last: ToggleGameMode/Trace above may still have recorded into the ring
	::DeleteCriticalSection(&this->m_logLock);
}

//-------------------------------------------------------------------------
//  open a companion file (log / ini) in the user's default editor; the paths
//  are relative, matching how the app reads/writes them in its working dir
//-------------------------------------------------------------------------
static void OpenCompanionFile(HWND owner, const char* file) {
	if (::GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) {
		::MessageBoxA(owner,
			"That file does not exist yet.\n\n(The log is only written when "
			"\"Write TPFanControl.log\" is enabled.)",
			file, MB_ICONINFORMATION);
		return;
	}
	HINSTANCE h = ::ShellExecuteA(owner, "open", file, NULL, NULL, SW_SHOWNORMAL);
	if ((INT_PTR)h <= 32)   // no association (e.g. .log): fall back to Notepad
		::ShellExecuteA(owner, "open", "notepad.exe", file, NULL, SW_SHOWNORMAL);
}

//-------------------------------------------------------------------------
//  attach a tooltip to one control on a modal dialog. The tip window
//  is created on first use (pass NULL) and returned for reuse on later calls;
//  it is owned by the dialog, so it is freed automatically when the dialog
//  closes. Mirrors FANCONTROL::AddTip, which targets the main window.
//-------------------------------------------------------------------------
static HWND AddDialogTip(HWND hwndDlg, HWND hTip, HINSTANCE hinst,
	int ctrlId, const char* text) {
	HWND hCtl = ::GetDlgItem(hwndDlg, ctrlId);
	if (!hCtl)
		return hTip;
	if (!hTip) {
		hTip = ::CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
			WS_POPUP | TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			hwndDlg, NULL, hinst, NULL);
		if (!hTip)
			return NULL;
		::SendMessage(hTip, TTM_SETMAXTIPWIDTH, 0, 320);
		::SendMessage(hTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
	}
	TOOLINFO ti = {};
	ti.cbSize   = sizeof(TOOLINFO);
	ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
	ti.hwnd     = hwndDlg;
	ti.uId      = (UINT_PTR)hCtl;
	ti.lpszText = (LPSTR)text;
	::SendMessage(hTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
	return hTip;
}

//-------------------------------------------------------------------------
//  per-child theming for dark mode: classic statics/buttons so WM_CTLCOLOR
//  text color applies (white), dark Explorer theme on edits for black scrollbars
//-------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFNSWTHEME)(HWND, LPCWSTR, LPCWSTR);
struct THEMECTX { PFNSWTHEME pSet; BOOL dark; };

static BOOL CALLBACK ThemeChildProc(HWND hChild, LPARAM lp) {
	THEMECTX* c = (THEMECTX*)lp;
	if (c->pSet) {
		char cls[32] = "";
		::GetClassNameA(hChild, cls, sizeof(cls));
		if (!c->dark)
			c->pSet(hChild, NULL, NULL);                  // restore default theme
		else if (_stricmp(cls, "Edit") == 0)
			c->pSet(hChild, L"DarkMode_Explorer", NULL);  // dark (black) scroll bars
		else
			c->pSet(hChild, L"", L"");                    // classic -> obeys WM_CTLCOLOR
	}
	::InvalidateRect(hChild, NULL, TRUE);
	return TRUE;
}

//-------------------------------------------------------------------------
//  like ThemeChildProc but also darkens push buttons (the main window has
//  none; the Settings dialog has OK/Apply/Cancel)
//-------------------------------------------------------------------------
static BOOL CALLBACK ThemeDlgChildProc(HWND hChild, LPARAM lp) {
	THEMECTX* c = (THEMECTX*)lp;
	if (c->pSet) {
		char cls[32] = "";
		::GetClassNameA(hChild, cls, sizeof(cls));
		if (!c->dark)
			c->pSet(hChild, NULL, NULL);                       // restore default theme
		else if (_stricmp(cls, "Edit") == 0)
			c->pSet(hChild, L"DarkMode_Explorer", NULL);       // dark edit + scrollbars
		else if (_stricmp(cls, "Button") == 0) {
			LONG bt = ::GetWindowLong(hChild, GWL_STYLE) & 0x0F;   // BS_TYPEMASK
			if (bt == BS_PUSHBUTTON || bt == BS_DEFPUSHBUTTON)
				c->pSet(hChild, L"DarkMode_Explorer", NULL);   // dark push buttons
			else
				c->pSet(hChild, L"", L"");                     // check/radio/group -> WM_CTLCOLOR
		}
		else
			c->pSet(hChild, L"", L"");                         // statics -> WM_CTLCOLOR
	}
	::InvalidateRect(hChild, NULL, TRUE);
	return TRUE;
}

//-------------------------------------------------------------------------
//  OS High Contrast theme active? (overrides our dark/light palettes so the
//  system colors stay legible)
//-------------------------------------------------------------------------
static BOOL IsHighContrast() {
	HIGHCONTRASTA hc = { sizeof(hc) };
	if (::SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
		return (hc.dwFlags & HCF_HIGHCONTRASTON) ? TRUE : FALSE;
	return FALSE;
}

//-------------------------------------------------------------------------
//  does the system theme want dark apps? (missing value = dark, matching
//  the constructor default)
//-------------------------------------------------------------------------
static int QuerySystemDark() {
	DWORD light = 0, cb = sizeof(light);
	if (::RegGetValueA(HKEY_CURRENT_USER,
			"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &cb) == ERROR_SUCCESS)
		return light ? 0 : 1;
	return 1;
}

//-------------------------------------------------------------------------
//  the user's Windows accent color (DWM colorization), blended toward white
//  when it would be unreadable on the dark dialog background; falls back to
//  the supplied color when the API is unavailable
//-------------------------------------------------------------------------
static COLORREF GetAccentColor(COLORREF fallback, BOOL darkBg) {
	COLORREF clr = fallback;
	HMODULE hDwm = ::LoadLibraryA("dwmapi.dll");
	if (hDwm) {
		typedef HRESULT(WINAPI* PFNDGCC)(DWORD*, BOOL*);
		PFNDGCC pGet = (PFNDGCC)::GetProcAddress(hDwm, "DwmGetColorizationColor");
		DWORD argb = 0;
		BOOL opaque = FALSE;
		if (pGet && SUCCEEDED(pGet(&argb, &opaque))) {
			// DWM returns 0xAARRGGBB; COLORREF wants 0x00BBGGRR
			int r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
			if (darkBg && (2 * r + 5 * g + b) / 8 < 80) {   // too dark for the dark bg
				r += (255 - r) * 2 / 5;
				g += (255 - g) * 2 / 5;
				b += (255 - b) * 2 / 5;
			}
			clr = RGB(r, g, b);
		}
		::FreeLibrary(hDwm);
	}
	return clr;
}

//-------------------------------------------------------------------------
//  Win11 22000+ chrome: paint caption/caption-text/border in the exact theme
//  palette so the title bar no longer has a seam against the client area.
//  reset=TRUE (High Contrast) hands the chrome back to the system - the
//  attributes persist per window, so merely skipping the call would leave a
//  previously applied palette in force. Pre-22000 builds fail harmlessly.
//-------------------------------------------------------------------------
static void ApplyDwmChromeColors(HWND hwnd, BOOL dark, BOOL reset) {
	HMODULE hDwm = ::LoadLibraryA("dwmapi.dll");
	if (hDwm) {
		typedef HRESULT(WINAPI* PFNDWMSWA)(HWND, DWORD, LPCVOID, DWORD);
		PFNDWMSWA pSet = (PFNDWMSWA)::GetProcAddress(hDwm, "DwmSetWindowAttribute");
		if (pSet) {
			const COLORREF DWMWA_COLOR_DEF = 0xFFFFFFFF;   // DWMWA_COLOR_DEFAULT
			COLORREF border  = reset ? DWMWA_COLOR_DEF
			                 : dark ? RGB(58, 58, 58)    : RGB(216, 216, 216);
			COLORREF caption = reset ? DWMWA_COLOR_DEF
			                 : dark ? RGB(32, 32, 32)    : RGB(243, 243, 243);
			COLORREF captext = reset ? DWMWA_COLOR_DEF
			                 : dark ? RGB(235, 235, 235) : RGB(32, 32, 32);
			pSet(hwnd, 34, &border,  sizeof(border));    // DWMWA_BORDER_COLOR
			pSet(hwnd, 35, &caption, sizeof(caption));   // DWMWA_CAPTION_COLOR
			pSet(hwnd, 36, &captext, sizeof(captext));   // DWMWA_TEXT_COLOR
		}
		::FreeLibrary(hDwm);
	}
}

//-------------------------------------------------------------------------
//  apply dark/light chrome (titlebar + child controls) to an arbitrary dialog
//-------------------------------------------------------------------------
static void ApplyDarkToDialog(HWND hwnd, BOOL dark) {
	if (!hwnd) return;

	// High Contrast: restore default theming and leave the system in charge
	BOOL hc = IsHighContrast();
	if (hc) dark = FALSE;

	HMODULE hDwm = ::LoadLibraryA("dwmapi.dll");
	if (hDwm) {
		typedef HRESULT(WINAPI* PFNDWMSWA)(HWND, DWORD, LPCVOID, DWORD);
		PFNDWMSWA pSet = (PFNDWMSWA)::GetProcAddress(hDwm, "DwmSetWindowAttribute");
		if (pSet) {
			BOOL d = dark ? TRUE : FALSE;
			if (FAILED(pSet(hwnd, 20, &d, sizeof(d))))   // DWMWA_USE_IMMERSIVE_DARK_MODE
				pSet(hwnd, 19, &d, sizeof(d));           // 19 on older Win10 builds
		}
		::FreeLibrary(hDwm);
	}
	ApplyDwmChromeColors(hwnd, dark, hc);

	HMODULE hUx = ::LoadLibraryA("uxtheme.dll");
	if (hUx) {
		typedef int  (WINAPI* fnSPAM)(int);          // SetPreferredAppMode (ord 135)
		typedef bool (WINAPI* fnADMW)(HWND, bool);   // AllowDarkModeForWindow (ord 133)
		typedef void (WINAPI* fnFMT)();              // FlushMenuThemes (ord 136)
		fnSPAM pSPAM = (fnSPAM)::GetProcAddress(hUx, MAKEINTRESOURCEA(135));
		fnADMW pADMW = (fnADMW)::GetProcAddress(hUx, MAKEINTRESOURCEA(133));
		fnFMT  pFMT  = (fnFMT) ::GetProcAddress(hUx, MAKEINTRESOURCEA(136));
		if (pSPAM) pSPAM(dark ? 2 : 3);
		if (pADMW) pADMW(hwnd, dark ? true : false);
		if (pFMT)  pFMT();

		THEMECTX ctx;
		ctx.pSet = (PFNSWTHEME)::GetProcAddress(hUx, "SetWindowTheme");
		ctx.dark = dark;
		::EnumChildWindows(hwnd, ThemeDlgChildProc, (LPARAM)&ctx);
		::FreeLibrary(hUx);
	}

	::InvalidateRect(hwnd, NULL, TRUE);
}

//-------------------------------------------------------------------------
//  severity color for a Celsius temperature (same ladder as the tray icon);
//  collapses to the plain text color under High Contrast
//-------------------------------------------------------------------------
COLORREF
FANCONTROL::SeverityColor(int tempC) const {
	if (this->m_highContrast)         return this->m_clrText;
	if (tempC >= this->IconLevels[2]) return RGB(232, 48, 48);
	if (tempC >= this->IconLevels[1]) return RGB(232, 120, 0);
	if (tempC >= this->IconLevels[0]) return RGB(220, 170, 0);
	return RGB(0, 170, 0);
}

//-------------------------------------------------------------------------
//  (re)build theme brushes, apply dark title bar, repaint
//-------------------------------------------------------------------------
void
FANCONTROL::ApplyTheme() {
	COLORREF dlgbg, fieldbg;

	// High Contrast overrides our palettes with the system colors; refreshed
	// here (not per paint) so the WM_SETTINGCHANGE re-theme picks up toggles
	this->m_highContrast = IsHighContrast();
	BOOL effDark = (this->DarkMode && !this->m_highContrast) ? TRUE : FALSE;

	COLORREF rulebg;
	if (this->m_highContrast) {
		dlgbg = ::GetSysColor(COLOR_BTNFACE);
		fieldbg = ::GetSysColor(COLOR_WINDOW);
		this->m_clrText = ::GetSysColor(COLOR_WINDOWTEXT);
		this->m_clrTextDim = this->m_clrText;   // no dimming under High Contrast
		rulebg = ::GetSysColor(COLOR_BTNSHADOW);
	}
	else if (effDark) {
		dlgbg = RGB(32, 32, 32);
		fieldbg = RGB(45, 45, 48);
		this->m_clrText = RGB(235, 235, 235);
		this->m_clrTextDim = RGB(150, 150, 150);
		rulebg = RGB(62, 62, 62);
	}
	else {
		dlgbg = RGB(243, 243, 243);
		fieldbg = RGB(255, 255, 255);
		this->m_clrText = RGB(32, 32, 32);
		this->m_clrTextDim = RGB(96, 96, 96);
		rulebg = RGB(223, 223, 223);
	}

	// emphasis color: the user's accent, readable on the active background
	// (plain text color under High Contrast so the system palette stays intact)
	this->m_clrAccent = this->m_highContrast ? this->m_clrText
		: GetAccentColor(RGB(0, 120, 212), effDark);

	if (this->m_hbrDlg) ::DeleteObject(this->m_hbrDlg);
	if (this->m_hbrField) ::DeleteObject(this->m_hbrField);
	if (this->m_hbrRule) ::DeleteObject(this->m_hbrRule);
	this->m_hbrDlg = ::CreateSolidBrush(dlgbg);
	this->m_hbrField = ::CreateSolidBrush(fieldbg);
	this->m_hbrRule = ::CreateSolidBrush(rulebg);

	if (this->hwndDialog) {
		// dark title bar (Win10 1809+/Win11); load dynamically so we add no link dependency
		HMODULE hDwm = ::LoadLibraryA("dwmapi.dll");
		if (hDwm) {
			typedef HRESULT(WINAPI* PFNDWMSWA)(HWND, DWORD, LPCVOID, DWORD);
			PFNDWMSWA pSet = (PFNDWMSWA)::GetProcAddress(hDwm, "DwmSetWindowAttribute");
			if (pSet) {
				BOOL dark = effDark;
				// 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (19 on older Win10 builds)
				if (FAILED(pSet(this->hwndDialog, 20, &dark, sizeof(dark))))
					pSet(this->hwndDialog, 19, &dark, sizeof(dark));
			}
			::FreeLibrary(hDwm);
		}
		// Win11 seamless chrome: caption/border in the exact theme colors;
		// under High Contrast this resets to the system defaults instead
		ApplyDwmChromeColors(this->hwndDialog, effDark, this->m_highContrast);

		// re-theme child controls: white text on statics/groupboxes (classic so
		// WM_CTLCOLOR applies) and black scroll bars on the edit/log boxes
		HMODULE hUx = ::LoadLibraryA("uxtheme.dll");
		if (hUx) {
			// undocumented (Win10 1809+) app dark mode -> dark scroll bars/menus
			typedef int  (WINAPI* fnSPAM)(int);          // SetPreferredAppMode (ord 135)
			typedef bool (WINAPI* fnADMW)(HWND, bool);   // AllowDarkModeForWindow (ord 133)
			typedef void (WINAPI* fnFMT)();              // FlushMenuThemes (ord 136)
			fnSPAM pSPAM = (fnSPAM)::GetProcAddress(hUx, MAKEINTRESOURCEA(135));
			fnADMW pADMW = (fnADMW)::GetProcAddress(hUx, MAKEINTRESOURCEA(133));
			fnFMT  pFMT  = (fnFMT) ::GetProcAddress(hUx, MAKEINTRESOURCEA(136));
			if (pSPAM) pSPAM(effDark ? 2 : 3);           // 2=ForceDark, 3=ForceLight
			if (pADMW) pADMW(this->hwndDialog, effDark ? true : false);
			if (pFMT)  pFMT();

			THEMECTX ctx;
			ctx.pSet = (PFNSWTHEME)::GetProcAddress(hUx, "SetWindowTheme");
			ctx.dark = effDark;
			::EnumChildWindows(this->hwndDialog, ThemeChildProc, (LPARAM)&ctx);

			// the shared tooltip window is an owned popup, not a child, so the
			// enumeration above misses it; restyle it here so the dark-mode
			// toggle flips live tips too
			if (this->m_hwndTip) {
				if (pADMW) pADMW(this->m_hwndTip, effDark ? true : false);
				if (ctx.pSet) ctx.pSet(this->m_hwndTip,
					effDark ? L"DarkMode_Explorer" : NULL, NULL);
			}
			::FreeLibrary(hUx);
		}

		// dark menu background (popup + bar); text follows system in classic Win32
		HMENU hMenu = ::GetMenu(this->hwndDialog);
		if (hMenu) {
			MENUINFO mi;
			::ZeroMemory(&mi, sizeof(mi));
			mi.cbSize = sizeof(mi);
			mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
			mi.hbrBack = effDark ? this->m_hbrDlg : NULL;
			::SetMenuInfo(hMenu, &mi);
			::DrawMenuBar(this->hwndDialog);
		}

		// RichEdit temp list: set its background directly (doesn't use WM_CTLCOLOREDIT)
		HWND hRich = ::GetDlgItem(this->hwndDialog, 8101);
		if (hRich)
			::SendMessage(hRich, EM_SETBKGNDCOLOR, 0, (LPARAM)fieldbg);

		this->UpdateTempList();
		::InvalidateRect(this->hwndDialog, NULL, TRUE);
	}
}

//-------------------------------------------------------------------------
//  Win11 type faces: prefer the Segoe UI Variable optical faces when they
//  are installed (build 22000+), falling back to plain Segoe UI. Probed once
//  via EnumFontFamiliesEx and cached - font installs are not live events.
//-------------------------------------------------------------------------
static int CALLBACK FaceExistsProc(const LOGFONTA*, const TEXTMETRICA*, DWORD, LPARAM lp) {
	*(BOOL*)lp = TRUE;
	return 0;   // stop after the first hit
}

static const char* PickUiFace(const char* want) {
	BOOL found = FALSE;
	HDC hdc = ::GetDC(NULL);
	if (hdc) {
		LOGFONTA lf = {};
		lf.lfCharSet = DEFAULT_CHARSET;
		strcpy_s(lf.lfFaceName, sizeof(lf.lfFaceName), want);
		::EnumFontFamiliesExA(hdc, &lf, (FONTENUMPROCA)FaceExistsProc, (LPARAM)&found, 0);
		::ReleaseDC(NULL, hdc);
	}
	return found ? want : "Segoe UI";
}

static const char* UiFaceText() {      // body text (Win11 type-ramp "Text" face)
	static const char* s = NULL;
	if (!s) s = PickUiFace("Segoe UI Variable Text");
	return s;
}

static const char* UiFaceDisplay() {   // large text ("Display" optical size)
	static const char* s = NULL;
	if (!s) s = PickUiFace("Segoe UI Variable Display");
	return s;
}

//-------------------------------------------------------------------------
//  re-create a modal dialog's template font with the Variable face (sized at
//  the dialog's own DPI under PMv2, so don't reuse the main window's font)
//  and push it to every child. Freed via a window prop on WM_DESTROY.
//-------------------------------------------------------------------------
static void ApplyDialogVariableFont(HWND hwnd) {
	HFONT cur = (HFONT)::SendMessage(hwnd, WM_GETFONT, 0, 0);
	LOGFONTA lf = {};
	if (!cur || !::GetObjectA(cur, sizeof(lf), &lf))
		return;
	if (_stricmp(lf.lfFaceName, UiFaceText()) == 0)
		return;   // Variable face not installed: template face already final
	strcpy_s(lf.lfFaceName, sizeof(lf.lfFaceName), UiFaceText());
	HFONT vf = ::CreateFontIndirectA(&lf);
	if (!vf)
		return;
	::SendMessage(hwnd, WM_SETFONT, (WPARAM)vf, FALSE);
	::EnumChildWindows(hwnd, SetFontChildProc, (LPARAM)vf);
	::SetPropA(hwnd, "TPFC_VARFONT", (HANDLE)vf);
}

static void FreeDialogVariableFont(HWND hwnd) {
	HFONT vf = (HFONT)::GetPropA(hwnd, "TPFC_VARFONT");
	if (vf) {
		::RemovePropA(hwnd, "TPFC_VARFONT");
		::DeleteObject(vf);
	}
}

//-------------------------------------------------------------------------
//  bold section-header font for a modal dialog, derived from the font the
//  dialog already owns (NOT the main window's m_hFontHdr: RescaleForDpi
//  deletes/rebuilds that on WM_DPICHANGED and only re-pushes it to the main
//  window's children, which would leave an open modal holding a freed HFONT).
//  Prop-owned, freed on WM_DESTROY - mirrors the TPFC_VARFONT pattern.
//-------------------------------------------------------------------------
static void ApplyDialogHeaderFont(HWND hwnd, const int* ids, int count) {
	HFONT base = (HFONT)::SendMessage(hwnd, WM_GETFONT, 0, 0);
	LOGFONTA lf = {};
	if (!base || !::GetObjectA(base, sizeof(lf), &lf))
		return;
	lf.lfWeight = FW_BOLD;
	HFONT hdr = ::CreateFontIndirectA(&lf);
	if (!hdr)
		return;
	for (int i = 0; i < count; i++) {
		HWND h = ::GetDlgItem(hwnd, ids[i]);
		if (h) ::SendMessage(h, WM_SETFONT, (WPARAM)hdr, TRUE);
	}
	::SetPropA(hwnd, "TPFC_HDRFONT", (HANDLE)hdr);
}

static void FreeDialogHeaderFont(HWND hwnd) {
	HFONT hf = (HFONT)::GetPropA(hwnd, "TPFC_HDRFONT");
	if (hf) {
		::RemovePropA(hwnd, "TPFC_HDRFONT");
		::DeleteObject(hf);
	}
}

//-------------------------------------------------------------------------
//  post-create chrome: temp-list columns, checkbox states, log visibility
//-------------------------------------------------------------------------
void
FANCONTROL::InitThemeAndChrome() {
	if (!this->hwndDialog) return;

	// title-bar (system-menu corner) + Alt-Tab/taskbar icon: dialog windows
	// show none until WM_SETICON. Loaded once - LoadIconMetric picks the
	// crispest frame for the current DPI - and re-sent when the slim-dialog
	// swap recreates the window. WM_SETICON does not copy the handle, so the
	// icons stay loaded until the destructor frees them.
	if (!this->m_hIconSm &&
			FAILED(::LoadIconMetric(this->hinstapp, MAKEINTRESOURCEW(1), LIM_SMALL, &this->m_hIconSm)))
		this->m_hIconSm = (HICON)::LoadImage(this->hinstapp, MAKEINTRESOURCE(1), IMAGE_ICON,
			::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	if (!this->m_hIconBig &&
			FAILED(::LoadIconMetric(this->hinstapp, MAKEINTRESOURCEW(1), LIM_LARGE, &this->m_hIconBig)))
		this->m_hIconBig = (HICON)::LoadImage(this->hinstapp, MAKEINTRESOURCE(1), IMAGE_ICON,
			::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	if (this->m_hIconSm)
		::SendMessage(this->hwndDialog, WM_SETICON, ICON_SMALL, (LPARAM)this->m_hIconSm);
	if (this->m_hIconBig)
		::SendMessage(this->hwndDialog, WM_SETICON, ICON_BIG, (LPARAM)this->m_hIconBig);

	// --- modern font hierarchy ------------------------------------------------
	// Bold section headers (they replace the old group-box frames) and a larger
	// semibold font on the primary readouts (State / Fan-speed). Sized in points
	// against the window DPI so they stay crisp; recreated on a DPI change.
	{
		HDC hdc = ::GetDC(this->hwndDialog);
		int dpiY = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSY) : 96;
		if (hdc) ::ReleaseDC(this->hwndDialog, hdc);
		this->m_curDpi = (UINT)dpiY;   // PerMonitorV2 baseline for later WM_DPICHANGED
		if (this->m_hFontDlg)   { ::DeleteObject(this->m_hFontDlg);   this->m_hFontDlg = NULL; }
		if (this->m_hFontHdr)   { ::DeleteObject(this->m_hFontHdr);   this->m_hFontHdr = NULL; }
		if (this->m_hFontBig)   { ::DeleteObject(this->m_hFontBig);   this->m_hFontBig = NULL; }
		if (this->m_hFontTitle) { ::DeleteObject(this->m_hFontTitle); this->m_hFontTitle = NULL; }
		this->m_hFontDlg = ::CreateFontA(-::MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceText());
		this->m_hFontHdr = ::CreateFontA(-::MulDiv(9, dpiY, 72), 0, 0, 0, FW_BOLD,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceText());
		this->m_hFontBig = ::CreateFontA(-::MulDiv(10, dpiY, 72), 0, 0, 0, FW_SEMIBOLD,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceText());
		this->m_hFontTitle = ::CreateFontA(-::MulDiv(12, dpiY, 72), 0, 0, 0, FW_SEMIBOLD,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceDisplay());
		// the .rc FONT statement can't express a fallback face, so the Variable
		// body font has to be pushed to every child; runs before ReflowLayout
		// below so design geometry is captured with the final font (the bold /
		// large overrides then re-claim their controls)
		if (this->m_hFontDlg)
			::EnumChildWindows(this->hwndDialog, SetFontChildProc, (LPARAM)this->m_hFontDlg);
		static const int hdrIds[] = { 9210, 9198, 9199, 9201, 9202, 9220, 9221 };  // section headers (9220/9221 = slim-dialog Temps / Fan Control)
		for (int i = 0; i < (int)(sizeof(hdrIds) / sizeof(hdrIds[0])); i++) {
			HWND h = ::GetDlgItem(this->hwndDialog, hdrIds[i]);
			if (h && this->m_hFontHdr) ::SendMessage(h, WM_SETFONT, (WPARAM)this->m_hFontHdr, TRUE);
		}
		static const int bigIds[] = { 8100, 8102, 8104 };  // State / Fan1 / Fan2 readouts
		for (int i = 0; i < (int)(sizeof(bigIds) / sizeof(bigIds[0])); i++) {
			HWND h = ::GetDlgItem(this->hwndDialog, bigIds[i]);
			if (h && this->m_hFontBig) ::SendMessage(h, WM_SETFONT, (WPARAM)this->m_hFontBig, TRUE);
		}
		HWND hTitle = ::GetDlgItem(this->hwndDialog, 8115);   // "TPFanControl = ..." line
		if (hTitle && this->m_hFontTitle) ::SendMessage(hTitle, WM_SETFONT, (WPARAM)this->m_hFontTitle, TRUE);
	}

	// two/three aligned columns in the temperature list (name | temp | hex)
	// RichEdit uses PARAFORMAT2 tab stops (in twips: ~40 and ~80 dialog units)
	PARAFORMAT2 pf = {};
	pf.cbSize = sizeof(PARAFORMAT2);
	pf.dwMask = PFM_TABSTOPS;
	pf.cTabCount = 2;
	pf.rgxTabs[0] = 840;
	pf.rgxTabs[1] = 1680;
	::SendDlgItemMessage(this->hwndDialog, 8101, EM_SETPARAFORMAT, 0, (LPARAM)&pf);

	// reflect flags as checkmarks in the View menu (no-op if dialog has no menu)
	// reflect flags in the in-window toggle checkboxes
	::SendDlgItemMessage(this->hwndDialog, 7010, BM_SETCHECK, this->ShowTempHex ? BST_CHECKED : BST_UNCHECKED, 0);
	::SendDlgItemMessage(this->hwndDialog, 7011, BM_SETCHECK, this->ShowLog ? BST_CHECKED : BST_UNCHECKED, 0);
	::SendDlgItemMessage(this->hwndDialog, 7012, BM_SETCHECK, this->DarkMode ? BST_CHECKED : BST_UNCHECKED, 0);
	::SendDlgItemMessage(this->hwndDialog, 7013, BM_SETCHECK, this->m_driversHidden ? BST_CHECKED : BST_UNCHECKED, 0);

	// manual-speed slider: positions 0..7 = fan 0..7, position 8 = 64 (max)
	HWND hSld = ::GetDlgItem(this->hwndDialog, 8311);
	if (hSld) {
		::SendMessage(hSld, TBM_SETRANGE, TRUE, MAKELONG(0, 8));
		::SendMessage(hSld, TBM_SETPOS, TRUE,
			this->ManFanSpeed >= 64 ? 8 : (this->ManFanSpeed > 7 ? 7 : this->ManFanSpeed));
	}

	// cap the manual fan-level box to 3 digits so 4+ digit junk can't be typed
	// (the value is also validated against the legal set when applied in HandleData)
	::SendDlgItemMessage(this->hwndDialog, 8310, EM_LIMITTEXT, 3, 0);

	this->ReflowLayout();      // capture design geometry before any resize
	this->ApplyLogVisibility();

	// initial visibility of the temperature history graph (incl. its divider)
	{
		int sw = this->ShowGraph ? SW_SHOW : SW_HIDE;
		::ShowWindow(::GetDlgItem(this->hwndDialog, 9202), sw);
		::ShowWindow(::GetDlgItem(this->hwndDialog, 8120), sw);
		::ShowWindow(::GetDlgItem(this->hwndDialog, 9242), sw);
	}

	// hover inspection on the sparkline (slim dialogs have no 8120 -> skipped;
	// the subclass removes itself on WM_NCDESTROY when a dialog is recreated)
	{
		HWND hSpark = ::GetDlgItem(this->hwndDialog, 8120);
		if (hSpark)
			::SetWindowSubclass(hSpark, FANCONTROL::SparkSubclassProc, 0,
				(DWORD_PTR)this);
	}

	// keyboard a11y: show focus rectangles and mnemonic underlines from launch
	// instead of only after the first Alt keypress
	::SendMessage(this->hwndDialog, WM_CHANGEUISTATE,
		MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);

	this->ApplyTheme();
}

//-------------------------------------------------------------------------
//  show/hide the Log box and auto-shrink/restore the window width
//-------------------------------------------------------------------------
void
FANCONTROL::ApplyLogVisibility() {
	if (!this->hwndDialog) return;

	int sw = this->ShowLog ? SW_SHOW : SW_HIDE;
	::ShowWindow(::GetDlgItem(this->hwndDialog, 9200), sw);
	::ShowWindow(::GetDlgItem(this->hwndDialog, 9201), sw);

	RECT rw;
	::GetWindowRect(this->hwndDialog, &rw);
	int hgt = rw.bottom - rw.top;
	if (this->m_fullW <= 0)
		this->m_fullW = rw.right - rw.left;   // capture full width once

	int w = this->m_fullW;
	if (!this->ShowLog) {
		HWND hLog = ::GetDlgItem(this->hwndDialog, 9201);
		if (hLog) {
			RECT rl;
			::GetWindowRect(hLog, &rl);
			int narrow = rl.left - rw.left;   // cut just left of the Log group
			if (narrow > 120) w = narrow;
		}
	}
	::SetWindowPos(this->hwndDialog, NULL, 0, 0, w, hgt,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	// the panel may have just opened: backfill it from the log tail so it
	// shows the recent history instead of starting empty
	this->FlushLogToControl();
}

//-------------------------------------------------------------------------
//  hide or restore TVic driver files to avoid Riot Vanguard detection
//-------------------------------------------------------------------------
void
FANCONTROL::ToggleGameMode(bool silent) {
	// Confirm before HIDING drivers in an interactive toggle: this renames kernel
	// drivers in System32, a privileged system-wide change. Restore, and any
	// silent (exit/shutdown) call, is never gated.
	if (!silent && !this->m_driversHidden) {
		int a = ModernConfirm(this->hwndDialog,
			L"Enable Game Mode (Hide Drivers)",
			L"Hide the TVic kernel drivers?",
			L"Game Mode renames the TVicHW64 / TVicPort64 kernel drivers in "
			L"C:\\Windows\\System32\\drivers so anti-cheat software (e.g. "
			L"Vanguard) cannot see them.\n\n"
			L"While hidden, fan control still works, but the drivers are only "
			L"restored on a clean exit (or recovered automatically on the next "
			L"launch).",
			TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_WARNING_ICON,
			"Game Mode renames the TVicHW64 / TVicPort64 kernel drivers in\r\n"
			"C:\\Windows\\System32\\drivers so anti-cheat software (e.g. Vanguard)\r\n"
			"cannot see them.\r\n\r\n"
			"While hidden, fan control still works, but the drivers are only\r\n"
			"restored on a clean exit (or recovered automatically on the next\r\n"
			"launch). Continue?",
			"Enable Game Mode (Hide Drivers)", MB_OKCANCEL | MB_ICONWARNING);
		if (a != IDOK) {
			// keep the in-window checkbox in sync with the unchanged (not-hidden) state
			::SendDlgItemMessage(this->hwndDialog, 7013, BM_SETCHECK, BST_UNCHECKED, 0);
			return;
		}
	}

	static const char* const sys[2] = {
		"C:\\Windows\\System32\\drivers\\TVicHW64.sys",
		"C:\\Windows\\System32\\drivers\\TVicPort64.sys"
	};

	// Disable WOW64 FS redirection: this 32-bit process normally sees SysWOW64 as System32;
	// driver files live in the real System32\drivers so we must bypass the redirector.
	typedef BOOL (WINAPI *PFN_Disable)(PVOID*);
	typedef BOOL (WINAPI *PFN_Revert)(PVOID);
	HMODULE hK = ::GetModuleHandleA("kernel32.dll");
	PFN_Disable pfnOff = hK ? (PFN_Disable)::GetProcAddress(hK, "Wow64DisableWow64FsRedirection") : NULL;
	PFN_Revert  pfnOn  = hK ? (PFN_Revert) ::GetProcAddress(hK, "Wow64RevertWow64FsRedirection")  : NULL;
	PVOID fsOld = NULL;
	bool redir = pfnOff && pfnOff(&fsOld);

	DWORD lastErr = 0;
	bool ok = true;
	if (!this->m_driversHidden) {
		// Hide: rename .sys -> .sys.bak, replacing any stale .bak from a prior manual rename
		bool moved[2] = { false, false };   // track successes for rollback
		for (int i = 0; i < 2; i++) {
			char bak[MAX_PATH];
			strcpy_s(bak, sizeof(bak), sys[i]);
			strcat_s(bak, sizeof(bak), ".bak");
			if (::GetFileAttributesA(sys[i]) != INVALID_FILE_ATTRIBUTES) {
				if (!::MoveFileExA(sys[i], bak, MOVEFILE_REPLACE_EXISTING))
					{ lastErr = ::GetLastError(); ok = false; break; }
				moved[i] = true;
			}
		}
		if (!ok) {
			// partial failure: undo the renames that did succeed so we never
			// leave one driver hidden with m_driversHidden=false (which would
			// skip restoration in the destructor / on the next toggle)
			for (int i = 0; i < 2; i++) {
				if (!moved[i]) continue;
				char bak[MAX_PATH];
				strcpy_s(bak, sizeof(bak), sys[i]);
				strcat_s(bak, sizeof(bak), ".bak");
				::MoveFileExA(bak, sys[i], MOVEFILE_REPLACE_EXISTING);
			}
		}
		if (ok) {
			this->m_driversHidden = true;
			if (!silent && this->pTaskbarIcon && !this->NoBallons)
				this->pTaskbarIcon->SetBalloon(NIIF_INFO,
					"Game Mode ON",
					"TVic drivers hidden - safe to launch Riot games.", 8000);
		} else if (!silent) {
			char msg[256];
			sprintf_s(msg, sizeof(msg),
				"Could not hide TVic drivers (error %lu).\n\nTPFanControl v2.34 P15G2 Dual must run with administrator privileges for Game Mode.",
				lastErr);
			::MessageBoxA(this->hwndDialog, msg, "Game Mode", MB_OK | MB_ICONWARNING);
		}
	} else {
		// Restore: rename .sys.bak -> .sys
		// If .sys already exists alongside .bak (mixed state), just delete the stale .bak.
		for (int i = 0; i < 2; i++) {
			char bak[MAX_PATH];
			strcpy_s(bak, sizeof(bak), sys[i]);
			strcat_s(bak, sizeof(bak), ".bak");
			if (::GetFileAttributesA(bak) != INVALID_FILE_ATTRIBUTES) {
				if (::GetFileAttributesA(sys[i]) != INVALID_FILE_ATTRIBUTES) {
					// .sys already present; stale .bak - just remove it
					::DeleteFileA(bak);
				} else {
					if (!::MoveFileExA(bak, sys[i], 0))
						{ lastErr = ::GetLastError(); ok = false; break; }
				}
			}
		}
		if (ok) {
			this->m_driversHidden = false;
			if (!silent && this->pTaskbarIcon && !this->NoBallons)
				this->pTaskbarIcon->SetBalloon(NIIF_INFO,
					"Game Mode OFF",
					"TVic drivers restored. TPFanControl v2.34 P15G2 Dual running normally.", 8000);
		} else if (!silent) {
			char msg[256];
			sprintf_s(msg, sizeof(msg),
				"Could not restore TVic drivers (error %lu).\n\nCheck C:\\Windows\\System32\\drivers for .sys.bak files.",
				lastErr);
			::MessageBoxA(this->hwndDialog, msg, "Game Mode", MB_OK | MB_ICONWARNING);
		}
	}

	if (redir) pfnOn(fsOld);

	// Sync the in-dialog checkbox to actual state (handles both click-from-checkbox and tray-menu)
	::SendDlgItemMessage(this->hwndDialog, 7013, BM_SETCHECK,
		this->m_driversHidden ? BST_CHECKED : BST_UNCHECKED, 0);
}

//-------------------------------------------------------------------------
//  populate the RichEdit temp list with per-sensor colors
//-------------------------------------------------------------------------
void
FANCONTROL::UpdateTempList() {
	HWND hRich = ::GetDlgItem(this->hwndDialog, 8101);
	if (!hRich) return;

	// Build every visible row plus a signature of everything that affects the
	// rendering. This runs each poll cycle, so bail out before touching the
	// RichEdit when nothing visible has changed since the last render.
	struct { COLORREF color; bool isMax; char line[128]; } rows[12];
	int nrows = 0;
	// sized for the worst case (12 rows * (color + ':' + 128-char line) + header)
	// and appended through a bounds guard so siglen can never pass the buffer end
	char sig[2048];
	int siglen = sprintf_s(sig, sizeof(sig), "%lu|%d|%d|%d|",
		(unsigned long)this->m_clrText, this->ShowTempHex ? 1 : 0,
		this->Fahrenheit ? 1 : 0, this->ShowAll);

	for (int i = 0; i < 12; i++) {
		int raw = this->State.Sensors[i];
		bool valid = (raw != 0 && raw < 128);
		int temp = this->BiasedTemp(raw, i);   // display/color use the biased value

		if (!valid && this->ShowAll != 1)
			continue;

		COLORREF lineColor = this->m_clrText;
		if (valid)
			lineColor = this->SeverityColor(temp);

		char obuf2[64];
		if (valid) {
			FmtTemp(obuf2, sizeof(obuf2),
				this->Fahrenheit ? temp * 9 / 5 + 32 : temp, this->Fahrenheit);
		} else {
			strcpy_s(obuf2, sizeof(obuf2), "n/a");
		}

		// the sensor currently driving MaxTemp gets a bold row so the user can see
		// at a glance which reading the fan logic is reacting to. Guard on MaxTemp > 0
		// so that "no sensor won" (iMaxTemp left at its default 0) doesn't bold row 0.
		bool isMax = valid && (i == this->iMaxTemp) && this->MaxTemp > 0;

		rows[nrows].color = lineColor;
		rows[nrows].isMax = isMax;
		if (this->ShowTempHex)
			sprintf_s(rows[nrows].line, sizeof(rows[nrows].line), "%s\t%s\t(0x%02x)\r\n",
				this->State.SensorName[i], obuf2, this->State.SensorAddr[i]);
		else
			sprintf_s(rows[nrows].line, sizeof(rows[nrows].line), "%s\t%s\r\n",
				this->State.SensorName[i], obuf2);

		if (siglen >= 0 && siglen < (int)sizeof(sig)) {
			int n = sprintf_s(sig + siglen, sizeof(sig) - siglen, "%lu%c:%s",
				(unsigned long)lineColor, isMax ? '*' : '-', rows[nrows].line);
			if (n > 0) siglen += n;
		}
		nrows++;
	}

	if (strcmp(sig, this->m_tempListSig) == 0)
		return;   // identical to last render - leave the control untouched
	strcpy_s(this->m_tempListSig, sizeof(this->m_tempListSig), sig);

	::SendMessage(hRich, WM_SETREDRAW, FALSE, 0);
	::SetWindowText(hRich, "");

	// Set paragraph tab stops: 'Temp' (and optional 'Hex') columns centered
	::SendMessage(hRich, EM_SETSEL, 0, -1);
	PARAFORMAT2 pf = {};
	pf.cbSize = sizeof(PARAFORMAT2);
	pf.dwMask = PFM_TABSTOPS;
	pf.cTabCount = 2;
	pf.rgxTabs[0] = 1150 | (1 << 24);   // 'Temp' column: centered tab
	pf.rgxTabs[1] = 1600 | (1 << 24);   // 'Hex'  column: centered tab
	::SendMessage(hRich, EM_SETPARAFORMAT, 0, (LPARAM)&pf);

	// bold column header over the value columns ("Location  Temp [ Hex ]")
	{
		const char* hdr = this->ShowTempHex
			? "Location\tTemp\tHex\r\n"
			: "Location\tTemp\r\n";
		int len = ::GetWindowTextLength(hRich);
		::SendMessage(hRich, EM_SETSEL, len, len);
		CHARFORMAT cf = {};
		cf.cbSize = sizeof(CHARFORMAT);
		cf.dwMask = CFM_COLOR | CFM_BOLD;
		cf.dwEffects = CFE_BOLD;
		cf.crTextColor = this->m_clrText;
		::SendMessage(hRich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
		::SendMessage(hRich, EM_REPLACESEL, FALSE, (LPARAM)hdr);
	}

	for (int r = 0; r < nrows; r++) {
		int len = ::GetWindowTextLength(hRich);
		::SendMessage(hRich, EM_SETSEL, len, len);

		CHARFORMAT cf = {};
		cf.cbSize = sizeof(CHARFORMAT);
		cf.dwMask = CFM_COLOR | CFM_BOLD;   // bold only the max-driving row
		cf.dwEffects = rows[r].isMax ? CFE_BOLD : 0;
		cf.crTextColor = rows[r].color;
		::SendMessage(hRich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

		::SendMessage(hRich, EM_REPLACESEL, FALSE, (LPARAM)rows[r].line);
	}

	if (nrows == 0) {
		// empty state: a dim, non-bold placeholder rather than a bare header, so
		// the user sees status (startup before the first read, or no live sensor)
		// instead of an unexplained empty column.
		int len = ::GetWindowTextLength(hRich);
		::SendMessage(hRich, EM_SETSEL, len, len);
		CHARFORMAT cf = {};
		cf.cbSize = sizeof(CHARFORMAT);
		cf.dwMask = CFM_COLOR | CFM_BOLD;
		cf.dwEffects = 0;
		cf.crTextColor = this->m_clrText;
		::SendMessage(hRich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
		::SendMessage(hRich, EM_REPLACESEL, FALSE, (LPARAM)"(no active sensors)\r\n");
	}

	::SendMessage(hRich, EM_SETSEL, 0, 0);
	::SendMessage(hRich, EM_SCROLLCARET, 0, 0);
	::SendMessage(hRich, WM_SETREDRAW, TRUE, 0);
	// bErase=FALSE: the RichEdit fully repaints its own background (EM_SETBKGNDCOLOR),
	// so an erase pass only adds a one-frame flash on each list update.
	::InvalidateRect(hRich, NULL, FALSE);

	// ---- auto-size the list to its content (header + visible rows) and tuck
	//      the all/active radios just beneath it. The computed height depends only
	//      on the row count and the font, so gate on the row count: temps jiggle
	//      almost every poll (changing the signature above) but the row count rarely
	//      does, and re-issuing the same SetWindowPos every poll just churns no-op
	//      window-pos traffic. ReflowLayout() leaves these three controls alone so
	//      the two don't fight. m_tempListRows is reset to -1 on a DPI/font change.
	int visRows = nrows ? nrows : 1;   // the empty state renders one placeholder row
	if (visRows != this->m_tempListRows) {
		this->m_tempListRows = visRows;

		HDC dc = ::GetDC(hRich);
		HFONT hf  = (HFONT)::SendMessage(hRich, WM_GETFONT, 0, 0);
		HFONT old = hf ? (HFONT)::SelectObject(dc, hf) : NULL;
		TEXTMETRIC tm;
		::GetTextMetrics(dc, &tm);
		if (old) ::SelectObject(dc, old);
		::ReleaseDC(hRich, dc);

		int lineH = tm.tmHeight + tm.tmExternalLeading;
		int wantH = (visRows + 1) * lineH + 8;   // +1 for the header row, + padding

		RECT rl;
		::GetWindowRect(hRich, &rl);
		::MapWindowPoints(NULL, this->hwndDialog, (LPPOINT)&rl, 2);

		::SetWindowPos(hRich, NULL, 0, 0, rl.right - rl.left, wantH,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

		int radioTop = rl.top + wantH + 4;
		for (int id = 7001; id <= 7002; id++) {
			HWND hr = ::GetDlgItem(this->hwndDialog, id);
			if (!hr) continue;
			RECT rr;
			::GetWindowRect(hr, &rr);
			::MapWindowPoints(NULL, this->hwndDialog, (LPPOINT)&rr, 2);
			::SetWindowPos(hr, NULL, rr.left, radioTop, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}
}

//-------------------------------------------------------------------------
//  anchor-based reflow so the window can be resized (fills extra space)
//-------------------------------------------------------------------------
void
FANCONTROL::ReflowLayout() {
	if (!this->hwndDialog) return;

	// id, then anchor flags: add dW to x/w, dH to y/h
	static const struct { int id, ax, ay, aw, ah; } A[20] = {
		{ 9198, 0, 0, 0, 0 },   // 'Temperatures' header: fixed top-left
		{ 8101, 0, 0, 0, 1 },   // temperature list:   grow height
		{ 7001, 0, 1, 0, 0 },   // 'all'    radio: follow bottom
		{ 7002, 0, 1, 0, 0 },   // 'active' radio: follow bottom
		{ 9201, 0, 0, 0, 0 },   // 'Log' header: fixed top-left
		{ 9200, 0, 0, 1, 1 },   // log edit:  grow width + height
		{ 9199, 0, 1, 0, 0 },   // 'Status' header: follow bottom
		{ 8112, 0, 1, 0, 0 },   // status text:  follow bottom (fixed width)
		{ 9196, 0, 1, 0, 0 },   // 'Last' label: follow bottom
		{ 8113, 0, 1, 0, 0 },   // last text:    follow bottom (fixed width)
		{ 7010, 0, 1, 0, 0 },   // Temp hex checkbox: follow bottom
		{ 7011, 0, 1, 0, 0 },   // Show log checkbox: follow bottom
		{ 7012, 0, 1, 0, 0 },   // Dark mode checkbox: follow bottom
		{ 7013, 0, 1, 0, 0 },   // Game mode checkbox: follow bottom
		{ 9202, 0, 1, 0, 0 },   // 'Temperature history' header: follow bottom
		{ 8120, 0, 1, 1, 0 },   // sparkline:          follow bottom, grow width
		{ 5100, 0, 1, 0, 0 },   // Settings button: follow bottom (fixed pos/size)
		{ 9240, 0, 0, 0, 1 },   // vertical column rule:   grow height
		{ 9241, 0, 1, 1, 0 },   // bottom-bar rule:    follow bottom, grow width
		{ 9242, 0, 1, 1, 0 },   // history rule:       follow bottom, grow width
	};

	RECT rc;
	::GetClientRect(this->hwndDialog, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

	if (!this->m_layoutInit) {
		// capture design-time geometry once, before any user resize
		this->m_baseCW = cw;
		this->m_baseCH = ch;
		RECT wr;
		::GetWindowRect(this->hwndDialog, &wr);
		this->m_minH = wr.bottom - wr.top;
		// Minimum width = the compact (no-log) width, NOT the full width:
		// otherwise WM_GETMINMAXINFO clamps and blocks the log auto-shrink.
		this->m_minW = wr.right - wr.left;            // fallback: full width
		{
			HWND hLog = ::GetDlgItem(this->hwndDialog, 9201);
			if (hLog) {
				RECT rl;
				::GetWindowRect(hLog, &rl);
				int narrow = rl.left - wr.left;       // width without the Log box
				if (narrow > 200) this->m_minW = narrow;
			}
		}
		for (int i = 0; i < 20; i++) {
			HWND h = ::GetDlgItem(this->hwndDialog, A[i].id);
			RECT r = { 0, 0, 0, 0 };
			if (h) {
				::GetWindowRect(h, &r);
				::MapWindowPoints(NULL, this->hwndDialog, (LPPOINT)&r, 2);
			}
			this->m_baseRC[i] = r;
		}
		this->m_layoutInit = TRUE;
		return;
	}

	int dW = cw - this->m_baseCW;
	int dH = ch - this->m_baseCH;

	HDWP hdwp = ::BeginDeferWindowPos(20);
	for (int i = 0; i < 20; i++) {
		HWND h = ::GetDlgItem(this->hwndDialog, A[i].id);
		if (!h) continue;
		// the temp list + all/active radios are sized to their content by
		// UpdateTempList(); don't let the resize reflow fight that.
		if (A[i].id == 8101 || A[i].id == 7001 || A[i].id == 7002)
			continue;
		const RECT& b = this->m_baseRC[i];
		int x = b.left + (A[i].ax ? dW : 0);
		int y = b.top + (A[i].ay ? dH : 0);
		int w = (b.right - b.left) + (A[i].aw ? dW : 0);
		int hh = (b.bottom - b.top) + (A[i].ah ? dH : 0);
		if (hdwp)
			hdwp = ::DeferWindowPos(hdwp, h, NULL, x, y, w, hh,
				SWP_NOZORDER | SWP_NOACTIVATE);
		else
			::SetWindowPos(h, NULL, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	if (hdwp) ::EndDeferWindowPos(hdwp);

	::InvalidateRect(this->hwndDialog, NULL, TRUE);
}

//-------------------------------------------------------------------------
//  PerMonitorV2 DPI change: scale every child's rect by the DPI ratio and
//  resize the window to the OS-suggested rectangle, then rebuild the fonts
//  at the new DPI. Only runs when the window actually moves to a monitor
//  with a different scale factor; on a single-DPI setup this never fires.
//-------------------------------------------------------------------------
struct DPISCALECTX { HWND parent; double s; };

static BOOL CALLBACK DpiScaleChildProc(HWND h, LPARAM lp) {
	DPISCALECTX* c = (DPISCALECTX*)lp;
	RECT r;
	::GetWindowRect(h, &r);
	::MapWindowPoints(NULL, c->parent, (LPPOINT)&r, 2);
	int x  = (int)(r.left * c->s + 0.5);
	int y  = (int)(r.top  * c->s + 0.5);
	int w  = (int)((r.right  - r.left) * c->s + 0.5);
	int hh = (int)((r.bottom - r.top ) * c->s + 0.5);
	::SetWindowPos(h, NULL, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
	return TRUE;
}

static BOOL CALLBACK SetFontChildProc(HWND h, LPARAM lp) {
	::SendMessage(h, WM_SETFONT, (WPARAM)lp, TRUE);
	return TRUE;
}

void
FANCONTROL::RescaleForDpi(UINT newDpi, const RECT* suggested) {
	if (!this->hwndDialog || newDpi == 0) return;
	UINT oldDpi = this->m_curDpi ? this->m_curDpi : 96;
	if (newDpi == oldDpi) return;

	this->m_inDpiChange = TRUE;   // suppress the reflow that WM_SIZE would trigger

	// 1. move/resize the window to the rectangle Windows recommends for the new DPI
	if (suggested)
		::SetWindowPos(this->hwndDialog, NULL,
			suggested->left, suggested->top,
			suggested->right - suggested->left, suggested->bottom - suggested->top,
			SWP_NOZORDER | SWP_NOACTIVATE);

	// 2. scale every child control's position + size by the DPI ratio
	double s = (double)newDpi / (double)oldDpi;
	DPISCALECTX ctx = { this->hwndDialog, s };
	::EnumChildWindows(this->hwndDialog, DpiScaleChildProc, (LPARAM)&ctx);

	// 3. rebuild fonts at the new DPI: a base font for the bulk of the controls
	//    plus the bold header / large readout fonts
	this->m_curDpi = newDpi;
	if (this->m_hFontDlg)   { ::DeleteObject(this->m_hFontDlg);   this->m_hFontDlg = NULL; }
	if (this->m_hFontHdr)   { ::DeleteObject(this->m_hFontHdr);   this->m_hFontHdr = NULL; }
	if (this->m_hFontBig)   { ::DeleteObject(this->m_hFontBig);   this->m_hFontBig = NULL; }
	if (this->m_hFontTitle) { ::DeleteObject(this->m_hFontTitle); this->m_hFontTitle = NULL; }
	this->m_hFontDlg = ::CreateFontA(-::MulDiv(9, newDpi, 72), 0, 0, 0, FW_NORMAL,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceText());
	this->m_hFontHdr = ::CreateFontA(-::MulDiv(9, newDpi, 72), 0, 0, 0, FW_BOLD,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceText());
	this->m_hFontBig = ::CreateFontA(-::MulDiv(10, newDpi, 72), 0, 0, 0, FW_SEMIBOLD,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceText());
	this->m_hFontTitle = ::CreateFontA(-::MulDiv(12, newDpi, 72), 0, 0, 0, FW_SEMIBOLD,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFaceDisplay());
	if (this->m_hFontDlg)
		::EnumChildWindows(this->hwndDialog, SetFontChildProc, (LPARAM)this->m_hFontDlg);
	static const int hdrIds[] = { 9210, 9198, 9199, 9201, 9202, 9220, 9221 };   // keep in sync with InitThemeAndChrome
	for (int i = 0; i < (int)(sizeof(hdrIds) / sizeof(hdrIds[0])); i++) {
		HWND h = ::GetDlgItem(this->hwndDialog, hdrIds[i]);
		if (h && this->m_hFontHdr) ::SendMessage(h, WM_SETFONT, (WPARAM)this->m_hFontHdr, TRUE);
	}
	static const int bigIds[] = { 8100, 8102, 8104 };
	for (int i = 0; i < (int)(sizeof(bigIds) / sizeof(bigIds[0])); i++) {
		HWND h = ::GetDlgItem(this->hwndDialog, bigIds[i]);
		if (h && this->m_hFontBig) ::SendMessage(h, WM_SETFONT, (WPARAM)this->m_hFontBig, TRUE);
	}
	HWND hTitle = ::GetDlgItem(this->hwndDialog, 8115);
	if (hTitle && this->m_hFontTitle) ::SendMessage(hTitle, WM_SETFONT, (WPARAM)this->m_hFontTitle, TRUE);

	// 4. re-baseline the reflow system at the new DPI/size, then repaint
	this->m_layoutInit = FALSE;
	this->m_fullW = 0;
	this->m_inDpiChange = FALSE;
	this->m_tempListSig[0] = '\0';   // force UpdateTempList to re-apply the content height at the new DPI
	this->m_tempListRows = -1;       // and re-run the auto-size reflow once at the new font metrics
	this->ReflowLayout();   // recapture design geometry at the new scale
	::InvalidateRect(this->hwndDialog, NULL, TRUE);
}

//-------------------------------------------------------------------------
//  push one MaxTemp reading into the rolling history ring buffer
//-------------------------------------------------------------------------
void
FANCONTROL::PushTempSample(int temp) {
	if (temp < 0)   temp = 0;
	if (temp > 255) temp = 255;
	this->m_tempHist[this->m_tempHistHead] = (unsigned char)temp;
	this->m_tempHistHead = (this->m_tempHistHead + 1) % TEMPHIST_MAX;
	if (this->m_tempHistCount < TEMPHIST_MAX)
		this->m_tempHistCount++;
}

//-------------------------------------------------------------------------
//  paint the temperature history as an autoscaled sparkline (owner-draw)
//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
//  GDI+ flat API for the antialiased sparkline trace, loaded at first use so
//  there is no link-time dependency. Tri-state load so a failure is not
//  retried on every WM_DRAWITEM; the GDI polyline below stays as fallback.
//-------------------------------------------------------------------------
struct GDIPSTARTUPINPUT {        // GdiplusStartupInput, v1 layout
	UINT32 GdiplusVersion;
	void*  DebugEventCallback;
	BOOL   SuppressBackgroundThread;
	BOOL   SuppressExternalCodecs;
};
struct GDIPPOINT { INT x, y; };  // GDI+ integer Point

typedef int  (WINAPI* PFNGpStartup)(ULONG_PTR*, const GDIPSTARTUPINPUT*, void*);
typedef void (WINAPI* PFNGpShutdown)(ULONG_PTR);
typedef int  (WINAPI* PFNGpFromHDC)(HDC, void**);
typedef int  (WINAPI* PFNGpDelGraphics)(void*);
typedef int  (WINAPI* PFNGpSmoothing)(void*, int);
typedef int  (WINAPI* PFNGpCreatePen1)(UINT32, float, int, void**);
typedef int  (WINAPI* PFNGpDelPen)(void*);
typedef int  (WINAPI* PFNGpPenInt)(void*, int);
typedef int  (WINAPI* PFNGpDrawLinesI)(void*, void*, const GDIPPOINT*, INT);
typedef int  (WINAPI* PFNGpSolidFill)(UINT32, void**);
typedef int  (WINAPI* PFNGpDelBrush)(void*);
typedef int  (WINAPI* PFNGpEllipseB)(void*, void*, INT, INT, INT, INT);

static struct {
	int       state = 0;          // 0 = untried, 1 = ready, -1 = unavailable
	HMODULE   dll = NULL;
	ULONG_PTR token = 0;
	PFNGpShutdown    pShutdown = NULL;
	PFNGpFromHDC     pFromHDC = NULL;
	PFNGpDelGraphics pDelGraphics = NULL;
	PFNGpSmoothing   pSmoothing = NULL;
	PFNGpCreatePen1  pCreatePen1 = NULL;
	PFNGpDelPen      pDelPen = NULL;
	PFNGpPenInt      pPenJoin = NULL;
	PFNGpPenInt      pPenStartCap = NULL;
	PFNGpPenInt      pPenEndCap = NULL;
	PFNGpDrawLinesI  pDrawLinesI = NULL;
	PFNGpSolidFill   pSolidFill = NULL;
	PFNGpDelBrush    pDelBrush = NULL;
	PFNGpEllipseB    pFillEllipseI = NULL;
	PFNGpEllipseB    pDrawEllipseI = NULL;
} g_gdip;

static bool EnsureGdiplus() {
	if (g_gdip.state)
		return g_gdip.state > 0;
	g_gdip.state = -1;
	g_gdip.dll = ::LoadLibraryA("gdiplus.dll");
	if (!g_gdip.dll)
		return false;
	PFNGpStartup pStartup = (PFNGpStartup)::GetProcAddress(g_gdip.dll, "GdiplusStartup");
	g_gdip.pShutdown     = (PFNGpShutdown)   ::GetProcAddress(g_gdip.dll, "GdiplusShutdown");
	g_gdip.pFromHDC      = (PFNGpFromHDC)    ::GetProcAddress(g_gdip.dll, "GdipCreateFromHDC");
	g_gdip.pDelGraphics  = (PFNGpDelGraphics)::GetProcAddress(g_gdip.dll, "GdipDeleteGraphics");
	g_gdip.pSmoothing    = (PFNGpSmoothing)  ::GetProcAddress(g_gdip.dll, "GdipSetSmoothingMode");
	g_gdip.pCreatePen1   = (PFNGpCreatePen1) ::GetProcAddress(g_gdip.dll, "GdipCreatePen1");
	g_gdip.pDelPen       = (PFNGpDelPen)     ::GetProcAddress(g_gdip.dll, "GdipDeletePen");
	g_gdip.pPenJoin      = (PFNGpPenInt)     ::GetProcAddress(g_gdip.dll, "GdipSetPenLineJoin");
	g_gdip.pPenStartCap  = (PFNGpPenInt)     ::GetProcAddress(g_gdip.dll, "GdipSetPenStartCap");
	g_gdip.pPenEndCap    = (PFNGpPenInt)     ::GetProcAddress(g_gdip.dll, "GdipSetPenEndCap");
	g_gdip.pDrawLinesI   = (PFNGpDrawLinesI) ::GetProcAddress(g_gdip.dll, "GdipDrawLinesI");
	g_gdip.pSolidFill    = (PFNGpSolidFill)  ::GetProcAddress(g_gdip.dll, "GdipCreateSolidFill");
	g_gdip.pDelBrush     = (PFNGpDelBrush)   ::GetProcAddress(g_gdip.dll, "GdipDeleteBrush");
	g_gdip.pFillEllipseI = (PFNGpEllipseB)   ::GetProcAddress(g_gdip.dll, "GdipFillEllipseI");
	g_gdip.pDrawEllipseI = (PFNGpEllipseB)   ::GetProcAddress(g_gdip.dll, "GdipDrawEllipseI");
	if (pStartup && g_gdip.pShutdown && g_gdip.pFromHDC && g_gdip.pDelGraphics &&
			g_gdip.pSmoothing && g_gdip.pCreatePen1 && g_gdip.pDelPen &&
			g_gdip.pPenJoin && g_gdip.pPenStartCap && g_gdip.pPenEndCap &&
			g_gdip.pDrawLinesI && g_gdip.pSolidFill && g_gdip.pDelBrush &&
			g_gdip.pFillEllipseI && g_gdip.pDrawEllipseI) {
		GDIPSTARTUPINPUT in = { 1, NULL, FALSE, FALSE };
		if (pStartup(&g_gdip.token, &in, NULL) == 0)
			g_gdip.state = 1;
	}
	if (g_gdip.state < 0) {
		::FreeLibrary(g_gdip.dll);
		g_gdip.dll = NULL;
	}
	return g_gdip.state > 0;
}

static void ShutdownGdiplus() {
	if (g_gdip.state > 0 && g_gdip.pShutdown)
		g_gdip.pShutdown(g_gdip.token);
	if (g_gdip.dll) {
		::FreeLibrary(g_gdip.dll);
		g_gdip.dll = NULL;
	}
	g_gdip.state = -1;   // never restarted after teardown
}

// COLORREF (0x00BBGGRR) -> opaque GDI+ ARGB (0xAARRGGBB)
static UINT32 GdipArgb(COLORREF c) {
	return 0xFF000000u | ((UINT32)GetRValue(c) << 16) |
	       ((UINT32)GetGValue(c) << 8) | (UINT32)GetBValue(c);
}

//-------------------------------------------------------------------------
//  sparkline hover: tracking tooltip with the hovered sample's value + age.
//  Registered lazily as a TTF_TRACK tool on the shared main-window tip.
//-------------------------------------------------------------------------
void
FANCONTROL::SparkHoverTip(HWND hSpark, int x, int y, bool show) {
	if (!this->m_hwndTip)
		return;

	TOOLINFO ti = {};
	ti.cbSize   = sizeof(TOOLINFO);
	ti.uFlags   = TTF_TRACK | TTF_ABSOLUTE;
	ti.hwnd     = this->hwndDialog;
	ti.uId      = 0x8120;   // private id; never collides with TTF_IDISHWND tools
	ti.lpszText = this->m_sparkTipText;

	if (!show) {
		if (this->m_sparkTipAdded)
			::SendMessage(this->m_hwndTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
		return;
	}

	int n = this->m_tempHistCount;
	RECT rcc;
	::GetClientRect(hSpark, &rcc);
	int w = rcc.right - rcc.left;
	if (n <= 0 || w <= 1)
		return;

	int cx = x < 0 ? 0 : (x > w - 1 ? w - 1 : x);
	int i = (n == 1) ? 0 : ::MulDiv(cx, n - 1, w - 1);
	int v = this->m_tempHist[(this->m_tempHistHead - n + i + TEMPHIST_MAX) % TEMPHIST_MAX];
	int disp = this->Fahrenheit ? v * 9 / 5 + 32 : v;
	const char* unit = this->Fahrenheit ? "F" : "C";
	int age = (n - 1 - i) * this->Cycle;
	if (age <= 0)
		sprintf_s(this->m_sparkTipText, sizeof(this->m_sparkTipText),
			"%d\xb0%s \xb7 now", disp, unit);
	else if (age < 600)
		sprintf_s(this->m_sparkTipText, sizeof(this->m_sparkTipText),
			"%d\xb0%s \xb7 %d s ago", disp, unit, age);
	else
		sprintf_s(this->m_sparkTipText, sizeof(this->m_sparkTipText),
			"%d\xb0%s \xb7 %d min ago", disp, unit, (age + 30) / 60);

	if (!this->m_sparkTipAdded) {
		::SendMessage(this->m_hwndTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
		this->m_sparkTipAdded = true;
	}
	else
		::SendMessage(this->m_hwndTip, TTM_SETTOOLINFO, 0, (LPARAM)&ti);

	// position near the cursor, DPI-scaled. TTF_ABSOLUTE means comctl32 does
	// NO screen-edge adjustment, so clamp to the work area ourselves (flip to
	// the left of the cursor at the right edge - where the newest samples are)
	UINT dpi = this->m_curDpi ? this->m_curDpi : 96;
	int dx = ::MulDiv(14, (int)dpi, 96);
	int dy = ::MulDiv(26, (int)dpi, 96);
	POINT pt = { x, y };
	::ClientToScreen(hSpark, &pt);
	int tx = pt.x + dx, ty = pt.y - dy;
	LRESULT sz = ::SendMessage(this->m_hwndTip, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
	if (sz) {
		int tw = LOWORD(sz), th = HIWORD(sz);
		HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { sizeof(mi) };
		if (::GetMonitorInfoA(mon, &mi)) {
			if (tx + tw > mi.rcWork.right)  tx = pt.x - dx - tw;
			if (tx < mi.rcWork.left)        tx = mi.rcWork.left;
			if (ty + th > mi.rcWork.bottom) ty = mi.rcWork.bottom - th;
			if (ty < mi.rcWork.top)         ty = mi.rcWork.top;
		}
	}
	::SendMessage(this->m_hwndTip, TTM_TRACKPOSITION, 0, MAKELPARAM(tx, ty));
	::SendMessage(this->m_hwndTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

//-------------------------------------------------------------------------
//  comctl32 subclass for the sparkline static: hover marker + tracking tip +
//  hand cursor. Right-click passes through DefSubclassProc so the existing
//  "Clear history" context menu (parent WM_CONTEXTMENU) keeps working.
//-------------------------------------------------------------------------
LRESULT CALLBACK
FANCONTROL::SparkSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR id, DWORD_PTR ref) {
	FANCONTROL* self = (FANCONTROL*)ref;
	switch (msg) {
	case WM_MOUSEMOVE:
		if (self) {
			if (!self->m_sparkTracking) {
				TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
				if (::TrackMouseEvent(&tme))
					self->m_sparkTracking = true;
			}
			int x = (short)LOWORD(lp);
			if (x != self->m_sparkHoverX) {
				self->m_sparkHoverX = x;
				self->SparkHoverTip(hwnd, x, (short)HIWORD(lp), true);
				::InvalidateRect(hwnd, NULL, FALSE);   // double-buffered: no erase
			}
		}
		break;

	case WM_MOUSELEAVE:
		if (self) {
			self->m_sparkTracking = false;
			if (self->m_sparkHoverX >= 0) {
				self->m_sparkHoverX = -1;
				self->SparkHoverTip(hwnd, 0, 0, false);
				::InvalidateRect(hwnd, NULL, FALSE);
			}
		}
		break;

	case WM_SETCURSOR:
		// hand cursor: signals the (right-click) interactivity of the graph
		::SetCursor(::LoadCursorA(NULL, (LPCSTR)IDC_HAND));
		return TRUE;

	case WM_NCDESTROY:
		::RemoveWindowSubclass(hwnd, FANCONTROL::SparkSubclassProc, 0);
		break;
	}
	return ::DefSubclassProc(hwnd, msg, wp, lp);
}

void
FANCONTROL::DrawSparkline(HDC hdc, const RECT& rc) {
	// background: match the editable-field color, fall back to the dialog color
	HBRUSH bg = this->m_hbrField ? this->m_hbrField : this->m_hbrDlg;

	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;
	const int n = this->m_tempHistCount;
	const int head = this->m_tempHistHead;

	if (n <= 0 || w < 4 || h < 4) {
		if (bg) ::FillRect(hdc, &rc, bg);
		::SetBkMode(hdc, TRANSPARENT);
		::SetTextColor(hdc, this->m_clrText);
		RECT tr = rc; tr.left += 4;
		// top-align to match the populated graph's top label band, so the text
		// doesn't jump from center to top when the first sample arrives
		::DrawTextA(hdc, "collecting...", -1, &tr, DT_SINGLELINE | DT_TOP | DT_LEFT);
		return;
	}

	// double-buffer: build the frame in a memory DC and blit it once, so the
	// owner-draw static never shows a half-painted graph (no flicker on resize).
	// carry over the control's font so the labels keep the dialog typeface.
	HFONT  hFont = (HFONT)::GetCurrentObject(hdc, OBJ_FONT);
	// Reuse a cached memory DC + bitmap, recreating the bitmap only when the
	// control size changes, so a click-drag resize no longer allocates a DC and a
	// screen-compatible bitmap on every WM_PAINT. (Freed in the destructor.)
	if (!this->m_sparkDC)
		this->m_sparkDC = ::CreateCompatibleDC(hdc);
	if (!this->m_sparkBmp || w != this->m_sparkW || h != this->m_sparkH) {
		if (this->m_sparkBmp) ::DeleteObject(this->m_sparkBmp);
		this->m_sparkBmp = ::CreateCompatibleBitmap(hdc, w, h);   // from hdc (color), not mdc (1bpp)
		this->m_sparkW = w;
		this->m_sparkH = h;
	}
	HDC     mdc = this->m_sparkDC;
	HGDIOBJ obm = ::SelectObject(mdc, this->m_sparkBmp);
	HGDIOBJ ofn = ::SelectObject(mdc, hFont);   // reselect each paint (font may change on theme/DPI)

	RECT lrc = { 0, 0, w, h };
	if (bg) ::FillRect(mdc, &lrc, bg);
	::SetBkMode(mdc, TRANSPARENT);

	// trace line scaled with the display DPI (crisp/proportional on high-DPI)
	const int dpi    = ::GetDeviceCaps(hdc, LOGPIXELSX);
	const int traceW = __max(1, ::MulDiv(2, dpi, 96));

	// data range over the window, with a sane minimum span for readability.
	// also remember the peak sample (value + position) for the max marker.
	int lo = 255, hi = 0, maxVal = 0, maxIdx = 0;
	long sum = 0;
	for (int i = 0; i < n; i++) {
		int v = this->m_tempHist[(head - n + i + TEMPHIST_MAX) % TEMPHIST_MAX];
		if (v < lo) lo = v;
		if (v > hi) hi = v;
		if (v >= maxVal) { maxVal = v; maxIdx = i; }   // last/right-most peak
		sum += v;
	}
	int avg = (int)(sum / n);
	if (hi - lo < 10) { int mid = (hi + lo) / 2; lo = mid - 5; hi = mid + 5; }
	if (lo < 0) { hi -= lo; lo = 0; }
	if (hi <= lo) hi = lo + 1;   // guard divide-by-zero

	// severity color from the latest sample (same thresholds as the temp list/icon)
	int latest = this->m_tempHist[(head - 1 + TEMPHIST_MAX) % TEMPHIST_MAX];
	COLORREF lineClr = this->SeverityColor(latest);

	// Reserve a top band for the value/range/avg labels so the trace and the peak
	// ring don't render underneath them; skip the inset on a very short control.
	int labelH = 0;
	{
		SIZE sz0 = { 0, 0 };
		::GetTextExtentPoint32A(mdc, "0", 1, &sz0);
		labelH = sz0.cy;
	}
	int top = 1;
	if (h - 2 - labelH >= 6) top = labelH + 1;   // only inset when enough plot height remains
	const int bot = h - 2;            // 1px padding bottom (local coords)
	const int ploth = bot - top;

	// faint gridlines at each IconLevel threshold that falls inside the range
	// (deliberately GDI/aliased: 1px horizontals look crisper without AA)
	COLORREF gridClr = (this->DarkMode && !this->m_highContrast)
		? RGB(70, 70, 74) : RGB(210, 210, 210);
	HPEN gridPen = ::CreatePen(PS_SOLID, 1, gridClr);
	HPEN oldPen = (HPEN)::SelectObject(mdc, gridPen);
	for (int g = 0; g < 3; g++) {
		int lvl = this->IconLevels[g];
		if (lvl <= lo || lvl >= hi) continue;
		int y = bot - ploth * (lvl - lo) / (hi - lo);
		::MoveToEx(mdc, 0, y, NULL);
		::LineTo(mdc, w, y);
	}
	::SelectObject(mdc, oldPen);
	::DeleteObject(gridPen);

	// shared geometry for the trace, the markers and the hover inspector
	const int mr = __max(2, ::MulDiv(2, dpi, 96));   // marker radius
	auto plotX  = [&](int i){ return (n == 1) ? 0 : (w - 1) * i / (n - 1); };
	auto plotY  = [&](int v){ return bot - ploth * (v - lo) / (hi - lo); };
	auto clampx = [&](int x){ return x < mr ? mr : (x > w - 1 - mr ? w - 1 - mr : x); };
	auto sample = [&](int i){ return (int)this->m_tempHist[(head - n + i + TEMPHIST_MAX) % TEMPHIST_MAX]; };
	int cx = clampx(plotX(n - 1)), cy = plotY(latest);    // current sample
	int mxp = clampx(plotX(maxIdx)), myp = plotY(maxVal); // window peak

	// the temperature trace + markers: antialiased via GDI+ when available
	// (in-box since XP, loaded dynamically), with the original aliased GDI
	// polyline kept as the fallback path - identical geometry either way
	bool gdipDrawn = false;
	if (n >= 2 && EnsureGdiplus()) {
		void* gfx = NULL;
		if (g_gdip.pFromHDC(mdc, &gfx) == 0 && gfx) {
			g_gdip.pSmoothing(gfx, 4);   // SmoothingModeAntiAlias
			void* pen = NULL;
			if (g_gdip.pCreatePen1(GdipArgb(lineClr), (float)traceW, 2 /*UnitPixel*/, &pen) == 0 && pen) {
				g_gdip.pPenJoin(pen, 2);       // LineJoinRound
				g_gdip.pPenStartCap(pen, 2);   // LineCapRound
				g_gdip.pPenEndCap(pen, 2);
				GDIPPOINT pts[TEMPHIST_MAX];
				for (int i = 0; i < n; i++) {
					pts[i].x = plotX(i);
					pts[i].y = plotY(sample(i));
				}
				if (g_gdip.pDrawLinesI(gfx, pen, pts, n) == 0)
					gdipDrawn = true;
				g_gdip.pDelPen(pen);
			}
			if (gdipDrawn) {
				// filled dot at the current sample, hollow ring at the peak
				void* br = NULL;
				if (g_gdip.pSolidFill(GdipArgb(lineClr), &br) == 0 && br) {
					g_gdip.pFillEllipseI(gfx, br, cx - mr, cy - mr, 2 * mr, 2 * mr);
					g_gdip.pDelBrush(br);
				}
				if (maxIdx != n - 1) {
					void* rp = NULL;
					if (g_gdip.pCreatePen1(GdipArgb(this->m_clrText), 1.0f, 2, &rp) == 0 && rp) {
						g_gdip.pDrawEllipseI(gfx, rp, mxp - mr, myp - mr, 2 * mr, 2 * mr);
						g_gdip.pDelPen(rp);
					}
				}
			}
			g_gdip.pDelGraphics(gfx);   // flushes GDI+ before the GDI labels/BitBlt
		}
	}

	if (!gdipDrawn) {
		// aliased GDI fallback (no GDI+, or a failed call mid-frame)
		HPEN linePen = ::CreatePen(PS_SOLID, traceW, lineClr);
		oldPen = (HPEN)::SelectObject(mdc, linePen);
		for (int i = 0; i < n; i++) {
			int x = plotX(i), y = plotY(sample(i));
			if (i == 0) ::MoveToEx(mdc, x, y, NULL);
			else        ::LineTo(mdc, x, y);
		}
		::SelectObject(mdc, oldPen);
		::DeleteObject(linePen);

		// markers: filled dot at the current sample, hollow ring at the peak
		HBRUSH fb = ::CreateSolidBrush(lineClr);
		HBRUSH ob = (HBRUSH)::SelectObject(mdc, fb);
		HPEN   fp = ::CreatePen(PS_SOLID, 1, lineClr);
		HPEN   op = (HPEN)::SelectObject(mdc, fp);
		::Ellipse(mdc, cx - mr, cy - mr, cx + mr, cy + mr);

		if (maxIdx != n - 1) {
			::SelectObject(mdc, ::GetStockObject(NULL_BRUSH));
			HPEN mp = ::CreatePen(PS_SOLID, 1, this->m_clrText);
			HPEN op2 = (HPEN)::SelectObject(mdc, mp);
			::Ellipse(mdc, mxp - mr, myp - mr, mxp + mr, myp + mr);
			::SelectObject(mdc, op2);
			::DeleteObject(mp);
		}

		::SelectObject(mdc, op);
		::DeleteObject(fp);
		::SelectObject(mdc, ob);
		::DeleteObject(fb);
	}

	// hover inspector: hairline + ring at the sample under the cursor (the
	// tracking tooltip with value/age is positioned by SparkSubclassProc)
	if (this->m_sparkHoverX >= 0 && n >= 1 && w > 1) {
		int hxr = this->m_sparkHoverX < 0 ? 0
			: (this->m_sparkHoverX > w - 1 ? w - 1 : this->m_sparkHoverX);
		int hidx = (n == 1) ? 0 : ::MulDiv(hxr, n - 1, w - 1);
		int px = clampx(plotX(hidx)), py = plotY(sample(hidx));
		HPEN linep = ::CreatePen(PS_SOLID, 1, gridClr);
		HPEN ringp = ::CreatePen(PS_SOLID, 1, this->m_clrText);
		HGDIOBJ oldp = ::SelectObject(mdc, linep);
		HGDIOBJ oldb = ::SelectObject(mdc, ::GetStockObject(NULL_BRUSH));
		::MoveToEx(mdc, px, top, NULL);
		::LineTo(mdc, px, bot);
		::SelectObject(mdc, ringp);
		::Ellipse(mdc, px - mr, py - mr, px + mr, py + mr);
		::SelectObject(mdc, oldp);
		::SelectObject(mdc, oldb);
		::DeleteObject(linep);
		::DeleteObject(ringp);
	}

	// labels: current value (left), range (right), window average (center). Drop
	// the less-essential ones rather than let them overlap on a narrow window.
	char lbl[48], rng[48], avgl[48];
	{
		int dl  = this->Fahrenheit ? latest * 9 / 5 + 32 : latest;
		int dlo = this->Fahrenheit ? lo * 9 / 5 + 32 : lo;
		int dhi = this->Fahrenheit ? hi * 9 / 5 + 32 : hi;
		int dav = this->Fahrenheit ? avg * 9 / 5 + 32 : avg;
		FmtTemp(lbl, sizeof(lbl), dl, this->Fahrenheit);
		// typographic en-dash range on CP1252 (this build's codepage); 0x96 is
		// a lead byte in some other ANSI codepages, so use a hyphen there
		const char* dash = (::GetACP() == 1252) ? "\x96" : "-";
		sprintf_s(rng, sizeof(rng), "%d%s%d%s", dlo, dash, dhi,
			TempUnit(this->Fahrenheit));
		sprintf_s(avgl, sizeof(avgl), "avg %d\xb0", dav);
	}
	SIZE szL = {}, szR = {}, szA = {};
	::GetTextExtentPoint32A(mdc, lbl,  (int)strlen(lbl),  &szL);
	::GetTextExtentPoint32A(mdc, rng,  (int)strlen(rng),  &szR);
	::GetTextExtentPoint32A(mdc, avgl, (int)strlen(avgl), &szA);

	::SetTextColor(mdc, lineClr);
	RECT lr = { 4, 0, w, h };
	::DrawTextA(mdc, lbl, -1, &lr, DT_SINGLELINE | DT_TOP | DT_LEFT);

	bool roomForRange = (szL.cx + szR.cx + 8 < w);
	if (roomForRange) {
		::SetTextColor(mdc, this->m_clrText);
		RECT rr = { 0, 0, w - 4, h };
		::DrawTextA(mdc, rng, -1, &rr, DT_SINGLELINE | DT_TOP | DT_RIGHT);
	}
	if (roomForRange && szL.cx + szA.cx + szR.cx + 16 < w) {
		::SetTextColor(mdc, this->m_clrText);
		RECT ar = { 0, 0, w, h };
		::DrawTextA(mdc, avgl, -1, &ar, DT_SINGLELINE | DT_TOP | DT_CENTER);
	}

	::BitBlt(hdc, rc.left, rc.top, w, h, mdc, 0, 0, SRCCOPY);

	// restore selections but keep the cached DC + bitmap for the next paint
	::SelectObject(mdc, ofn);
	::SelectObject(mdc, obm);
}

//-------------------------------------------------------------------------
//  mode integer from mode radio buttons
//-------------------------------------------------------------------------
int
FANCONTROL::CurrentModeFromDialog() {
	BOOL modetpauto = ::SendDlgItemMessage(this->hwndDialog, 8300, BM_GETCHECK, 0L, 0L),
		modefcauto = ::SendDlgItemMessage(this->hwndDialog, 8301, BM_GETCHECK, 0L, 0L),
		modemanual = ::SendDlgItemMessage(this->hwndDialog, 8302, BM_GETCHECK, 0L, 0L);

	if (modetpauto)
		this->CurrentMode = 1;
	else if (modefcauto)
		this->CurrentMode = 2;
	else if (modemanual)
		this->CurrentMode = 3;
	else
		this->CurrentMode = -1;

	return this->CurrentMode;
}

int
FANCONTROL::ShowAllFromDialog() {
	BOOL modefcauto = ::SendDlgItemMessage(this->hwndDialog, 7001, BM_GETCHECK, 0L, 0L),
		modemanual = ::SendDlgItemMessage(this->hwndDialog, 7002, BM_GETCHECK, 0L, 0L);

	if (modefcauto)
		this->ShowAll = 1;
	else if (modemanual)
		this->ShowAll = 0;
	else
		this->ShowAll = -1;

	return this->ShowAll;
}

void
FANCONTROL::ModeToDialog(int mode) {
	::SendDlgItemMessage(this->hwndDialog, 8300, BM_SETCHECK, mode == 1, 0L);
	::SendDlgItemMessage(this->hwndDialog, 8301, BM_SETCHECK, mode == 2, 0L);
	::SendDlgItemMessage(this->hwndDialog, 8302, BM_SETCHECK, mode == 3, 0L);
	this->UpdateManualControlsEnabled(mode);   // mode just set; no need to re-read the buttons
}

//-------------------------------------------------------------------------
//  the manual fan-level box (8310) and slider (8311) only do anything in
//  Manual mode; grey them out otherwise (and whenever EC control is off)
//-------------------------------------------------------------------------
void
FANCONTROL::UpdateManualControlsEnabled(int mode) {
	// mode < 0 means "read the live radio state"; callers that already know the
	// mode pass it to avoid 3 redundant BM_GETCHECK round-trips.
	if (mode < 0)
		mode = this->CurrentModeFromDialog();
	BOOL manual = this->ActiveMode && (mode == 3);

	// The enabled/visible state only changes on a real mode/ActiveMode change, so
	// skip the ~11 USER32 show/enable calls when nothing changed since last time.
	// (-1 sentinel forces the first apply.) Event-driven callers keep this correct.
	if ((int)manual == this->m_lastManualEnabled)
		return;
	this->m_lastManualEnabled = manual;

	// the level box greys visibly when disabled, so a plain disable reads fine
	::EnableWindow(::GetDlgItem(this->hwndDialog, 8310), manual);

	// a themed trackbar barely changes when merely disabled (the thumb greys
	// only faintly), so hide it outright in BIOS/Smart mode for a clear cue.
	// ShowWindow/EnableWindow with the unchanged state are cheap no-ops, so this
	// is safe to call every poll. (Slim dialogs have no slider -> GetDlgItem NULL.)
	HWND hSld = ::GetDlgItem(this->hwndDialog, 8311);
	if (hSld) {
		::EnableWindow(hSld, manual);
		::ShowWindow(hSld, manual ? SW_SHOW : SW_HIDE);
	}

	// the slider's tick labels (0..7, Max) must hide/show with the slider itself,
	// otherwise a row of orphaned numbers floats under empty space in BIOS/Smart.
	// ID range 8320-8328: keep in sync with BOTH dialog templates in res/FanControl.rc
	for (int id = 8320; id <= 8328; id++) {
		HWND h = ::GetDlgItem(this->hwndDialog, id);
		if (h) ::ShowWindow(h, manual ? SW_SHOW : SW_HIDE);
	}
}

void
FANCONTROL::ShowAllToDialog(int show) {
	::SendDlgItemMessage(this->hwndDialog, 7001, BM_SETCHECK, show == 1, 0L);
	::SendDlgItemMessage(this->hwndDialog, 7002, BM_SETCHECK, show == 0, 0L);
}

//-------------------------------------------------------------------------
//  process main dialog
//-------------------------------------------------------------------------
int FANCONTROL::ProcessDialog() {

	MSG qmsg, qmsg2;
	int dlgrc = -1;

	if (this->hwndDialog) {
		for (;;) {
			BOOL nodlgmsg = FALSE;

			::GetMessage(&qmsg, NULL, 0L, 0L);

			// control movements
			if (qmsg.message != WM__DISMISSDLG && IsDialogMessage(this->hwndDialog, &qmsg)) {
				continue;
			}

			qmsg2 = qmsg;
			TranslateMessage(&qmsg);
			DispatchMessage(&qmsg);

			if (qmsg2.message == WM__DISMISSDLG && qmsg2.hwnd == this->hwndDialog) {
				dlgrc = qmsg2.wParam;
				break;
			}
		}
	}

	return dlgrc;
}

//-------------------------------------------------------------------------
//  dialog window procedure (map to class method)
//-------------------------------------------------------------------------
ULONG CALLBACK
FANCONTROL::BaseDlgProc(HWND
	hwnd,
	ULONG msg, WPARAM
	mp1,
	LPARAM mp2
)
{
	ULONG rc = FALSE;

	static UINT s_TaskbarCreated;
	static UINT s_TaskbarButtonCreated;

	if (msg == WM_INITDIALOG)
	{
		s_TaskbarCreated = RegisterWindowMessage("TaskbarCreated");
		s_TaskbarButtonCreated = RegisterWindowMessage("TaskbarButtonCreated");

		// the app runs elevated (requireAdministrator), so the medium-integrity
		// Explorer's "TaskbarCreated"/"TaskbarButtonCreated" broadcasts are
		// silently dropped by UIPI and the re-add below never fires after an
		// Explorer crash/restart. Allow them process-wide (also covers
		// CSystemTray's hidden window). Re-running on a recreate is idempotent.
		typedef BOOL (WINAPI* PFNCWMF)(UINT, DWORD);
		PFNCWMF pFilter = (PFNCWMF)::GetProcAddress(
			::GetModuleHandleA("user32.dll"), "ChangeWindowMessageFilter");
		if (pFilter && s_TaskbarCreated)
			pFilter(s_TaskbarCreated, 1 /*MSGFLT_ADD*/);
		if (pFilter && s_TaskbarButtonCreated)
			pFilter(s_TaskbarButtonCreated, 1 /*MSGFLT_ADD*/);
	}

	FANCONTROL* This = (FANCONTROL*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

	if (This)
	{
		if (msg == s_TaskbarCreated)
		{
			This->TaskbarNew = 1;

			if (This->pTaskbarIcon)
			{
				This->pTaskbarIcon->RebuildIfNecessary(TRUE);
			}
			else {
				This->RemoveTextIcons();
				This->ProcessTextIcons();
			}
		}
		if (msg == s_TaskbarButtonCreated)
		{
			// Explorer rebuilt the taskbar button: re-apply overlay/progress
			This->m_lastTbSig = -1;
			This->UpdateTaskbarIndicators();
		}
		rc = This->DlgProc(hwnd, msg, mp1, mp2);
	}

	return rc;
}



//-------------------------------------------------------------------------
//  dialog window procedure as class method
//-------------------------------------------------------------------------
#define WANTED_MEM_SIZE 65536*12
// Bounded wait for the EC work thread; long enough to cover worst-case EC read
// retries, short enough to never hang the UI thread on shutdown/close paths.
#define THREAD_WAIT_TIMEOUT_MS 8000
ULONG
FANCONTROL::DlgProc(HWND
	hwnd,
	ULONG msg, WPARAM
	mp1,
	LPARAM mp2
)
{
	ULONG rc = 0, ok, res;
	char buf[1024];
	char obuf[256] = "";   // scratch for trace/log messages built in this call

	//	HANDLE hLockS = CreateMutex(NULL,FALSE,"TPFanControlMutex01");

	switch (msg) {
	case WM_HOTKEY:
		switch (mp1) {

		case 1:
			this->ModeToDialog(1);
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			break;

		case 2:
			this->ModeToDialog(2);
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			break;

		case 3:
			this->ModeToDialog(3);
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			break;

		case 4:
			this->ModeToDialog(2);
			if (this->IndSmartLevel == 1) {
				sprintf_s(obuf,	sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 1'");
				this->Trace(obuf);
			}
			this->IndSmartLevel = 0;

			this->ActivateSmartProfile(1);
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			break;

		case 5:
			this->ModeToDialog(2);
			if (this->IndSmartLevel == 0) {
				sprintf_s(obuf,	sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 2'");
				this->Trace(obuf);
			}
			this->IndSmartLevel = 1;

			this->ActivateSmartProfile(2);
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			break;

		case 6:
			if (this->CurrentMode > 1) {
				this->ModeToDialog(1);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}

			if (this->CurrentMode == 1) {
				this->ModeToDialog(2);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}
			break;

		case 7:
			if (this->CurrentMode > 1) {
				this->ModeToDialog(1);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}
			if (this->CurrentMode == 1) {
				this->ModeToDialog(3);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}
			break;

		case 8:
			if (this->CurrentMode < 3) {
				this->ModeToDialog(3);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}
			if (this->CurrentMode == 3) {
				this->ModeToDialog(2);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}
			break;

		case 9:
			this->ModeToDialog(2);
			switch (IndSmartLevel) {
			case 0:
				sprintf_s(obuf,	sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 2'");
				this->
					Trace(obuf);
				this->
					IndSmartLevel = 1;
				this->ActivateSmartProfile(2);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
				break;
			case 1:
				sprintf_s(obuf,
					sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 1'");
				this->Trace(obuf);
				this->IndSmartLevel = 0;
				this->ActivateSmartProfile(1);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
				break;
			}
			break;
		}
		break;

	case WM_MEASUREITEM:
	{
		MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)mp2;
		if (mis && mis->CtlType == ODT_MENU) {
			mis->itemWidth = 120;
			mis->itemHeight = 22;
			rc = TRUE;
		}
		break;
	}

	case WM_DRAWITEM:
	{
		DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)mp2;
		if (dis && dis->CtlType == ODT_STATIC && dis->CtlID == 8120) {
			if (this->ShowGraph)
				this->DrawSparkline(dis->hDC, dis->rcItem);
			else {
				// graph hidden: just clear, skip the whole sparkline build
				HBRUSH bg = this->m_hbrField ? this->m_hbrField : this->m_hbrDlg;
				if (bg) ::FillRect(dis->hDC, &dis->rcItem, bg);
			}
			rc = TRUE;
			break;
		}
		if (dis && dis->CtlType == ODT_STATIC && dis->CtlID == 8115) {
			// "TPControlFAN = On/OFF": draw the prefix in the theme text color
			// and color only the state word (green when fan control is active).
			bool on = (this->CurrentMode == 2 || this->CurrentMode == 3);
			const char* pre = "TPControlFAN = ";
			const char* st  = on ? "On" : "OFF";

			::FillRect(dis->hDC, &dis->rcItem, this->m_hbrDlg);
			::SetBkMode(dis->hDC, TRANSPARENT);

			HFONT hf  = (HFONT)::SendMessage(dis->hwndItem, WM_GETFONT, 0, 0);
			HFONT old = hf ? (HFONT)::SelectObject(dis->hDC, hf) : NULL;

			SIZE szPre = { 0, 0 }, szSt = { 0, 0 };
			::GetTextExtentPoint32A(dis->hDC, pre, (int)strlen(pre), &szPre);
			::GetTextExtentPoint32A(dis->hDC, st,  (int)strlen(st),  &szSt);

			int total = szPre.cx + szSt.cx;
			int x = dis->rcItem.left + ((dis->rcItem.right - dis->rcItem.left) - total) / 2;
			int y = dis->rcItem.top  + ((dis->rcItem.bottom - dis->rcItem.top) - szPre.cy) / 2;

			::SetTextColor(dis->hDC, this->m_clrText);
			::TextOutA(dis->hDC, x, y, pre, (int)strlen(pre));
			::SetTextColor(dis->hDC, on ? this->m_clrAccent : this->m_clrText);
			::TextOutA(dis->hDC, x + szPre.cx, y, st, (int)strlen(st));

			if (old) ::SelectObject(dis->hDC, old);
			rc = TRUE;
			break;
		}
		if (dis && dis->CtlType == ODT_MENU) {
			const char* txt =
				dis->itemID == 7010 ? "Temp hex" :
				dis->itemID == 7011 ? "Show log" :
				dis->itemID == 7012 ? "Dark mode" : "";
			bool chk =
				dis->itemID == 7010 ? this->ShowTempHex != 0 :
				dis->itemID == 7011 ? this->ShowLog != 0 :
				dis->itemID == 7012 ? this->DarkMode != 0 : false;
			BOOL sel = (dis->itemState & ODS_SELECTED) != 0;

			COLORREF bg = this->DarkMode
				? (sel ? RGB(62, 62, 64) : RGB(43, 43, 43))
				: (sel ? ::GetSysColor(COLOR_HIGHLIGHT) : ::GetSysColor(COLOR_MENU));
			COLORREF fg = this->DarkMode
				? RGB(235, 235, 235)
				: (sel ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : ::GetSysColor(COLOR_MENUTEXT));

			HBRUSH hb = ::CreateSolidBrush(bg);
			::FillRect(dis->hDC, &dis->rcItem, hb);
			::DeleteObject(hb);

			if (chk) {
				RECT cb;
				cb.left = dis->rcItem.left + 6;
				cb.right = cb.left + 6;
				cb.top = (dis->rcItem.top + dis->rcItem.bottom) / 2 - 3;
				cb.bottom = cb.top + 6;
				HBRUSH cbk = ::CreateSolidBrush(fg);
				::FillRect(dis->hDC, &cb, cbk);
				::DeleteObject(cbk);
			}

			::SetBkMode(dis->hDC, TRANSPARENT);
			::SetTextColor(dis->hDC, fg);
			RECT rt = dis->rcItem;
			rt.left += 20;
			::DrawTextA(dis->hDC, txt, -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
			rc = TRUE;
		}
		break;
	}

	case WM_NOTIFY:
	{
		// Win11-style rendering for the manual-fan trackbar via standard custom
		// draw: a slim rounded channel (accent fill left of the thumb) and a
		// round accent thumb. Pure paint code - WM_HSCROLL fan logic untouched.
		// BaseDlgProc returns rc directly, so stage results MUST go through
		// DWLP_MSGRESULT with rc = TRUE or the later stages never fire.
		NMHDR* nm = (NMHDR*)mp2;
		if (nm && nm->idFrom == 8311 && nm->code == NM_CUSTOMDRAW) {
			LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)mp2;
			LRESULT cdrf = CDRF_DODEFAULT;
			BOOL effDark = (this->DarkMode && !this->m_highContrast) ? TRUE : FALSE;

			switch (cd->dwDrawStage) {
			case CDDS_PREPAINT:
				// the control invalidates only the thumb area when the
				// position changes, but the channel's accent fill is split AT
				// the thumb - a partial clip leaves stale fill behind on
				// jump-clicks (e.g. 6 -> 7). Force one full repaint per
				// position change; the cached position keeps this from
				// looping (the forced paint sees an unchanged value).
				{
					int pos = (int)::SendMessage(cd->hdr.hwndFrom, TBM_GETPOS, 0, 0);
					if (pos != this->m_lastSliderPos) {
						this->m_lastSliderPos = pos;
						::InvalidateRect(cd->hdr.hwndFrom, NULL, TRUE);
					}
				}
				cdrf = CDRF_NOTIFYITEMDRAW;
				break;

			case CDDS_ITEMPREPAINT:
				if (cd->dwItemSpec == TBCD_TICS) {
					cdrf = CDRF_SKIPDEFAULT;   // the 8320-8328 labels mark the stops
				}
				else if (cd->dwItemSpec == TBCD_CHANNEL) {
					RECT ch = cd->rc;
					RECT tr = { 0, 0, 0, 0 };
					::SendMessage(cd->hdr.hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&tr);
					int split = (tr.left + tr.right) / 2;
					if (split < ch.left)  split = ch.left;
					if (split > ch.right) split = ch.right;

					// erase the stock sunken channel, then draw a slim flat bar
					::FillRect(cd->hdc, &ch, this->m_hbrDlg);
					int barH = __max(3, ::MulDiv(4, (int)(this->m_curDpi ? this->m_curDpi : 96), 96));
					int cy = (ch.top + ch.bottom) / 2;
					RECT bar = { ch.left, cy - barH / 2, ch.right, cy - barH / 2 + barH };

					HGDIOBJ oldPen = ::SelectObject(cd->hdc, ::GetStockObject(NULL_PEN));
					HBRUSH hbFill = ::CreateSolidBrush(this->m_clrAccent);
					HBRUSH hbRest = ::CreateSolidBrush(effDark ? RGB(96, 96, 100)
					                                           : RGB(198, 198, 198));
					// NULL_PEN shrinks the figure by one pixel; +1 compensates
					HGDIOBJ oldBr = ::SelectObject(cd->hdc, hbRest);
					::RoundRect(cd->hdc, bar.left, bar.top, bar.right + 1, bar.bottom + 1,
						barH, barH);
					if (split > bar.left) {
						::SelectObject(cd->hdc, hbFill);
						::RoundRect(cd->hdc, bar.left, bar.top, split + 1, bar.bottom + 1,
							barH, barH);
					}
					::SelectObject(cd->hdc, oldBr);
					::SelectObject(cd->hdc, oldPen);
					::DeleteObject(hbFill);
					::DeleteObject(hbRest);
					cdrf = CDRF_SKIPDEFAULT;
				}
				else if (cd->dwItemSpec == TBCD_THUMB) {
					// round accent thumb, centered in the stock thumb rect
					RECT th = cd->rc;
					::FillRect(cd->hdc, &th, this->m_hbrDlg);
					int d = __min(th.right - th.left, th.bottom - th.top);
					int cx = (th.left + th.right) / 2, cy = (th.top + th.bottom) / 2;
					int r2 = d / 2;
					HBRUSH hb = ::CreateSolidBrush(this->m_clrAccent);
					HPEN   hp = ::CreatePen(PS_SOLID, 1, this->m_clrAccent);
					HGDIOBJ ob = ::SelectObject(cd->hdc, hb);
					HGDIOBJ op = ::SelectObject(cd->hdc, hp);
					::Ellipse(cd->hdc, cx - r2, cy - r2, cx + r2, cy + r2);
					::SelectObject(cd->hdc, op);
					::SelectObject(cd->hdc, ob);
					::DeleteObject(hp);
					::DeleteObject(hb);
					cdrf = CDRF_SKIPDEFAULT;
				}
				break;
			}

			::SetWindowLongPtr(hwnd, DWLP_MSGRESULT, cdrf);
			rc = TRUE;
		}
		break;
	}

	case WM_HSCROLL:
		if ((HWND)mp2 == ::GetDlgItem(this->hwndDialog, 8311)) {
			int pos = (int)::SendMessage((HWND)mp2, TBM_GETPOS, 0, 0);
			int val = (pos <= 7) ? pos : 64;   // position 8 -> 64 (max)

			if (pos >= 8) {
				// Warn the first time the slider reaches max. WM_HSCROLL fires
				// repeatedly during a single drag (THUMBTRACK, THUMBPOSITION,
				// ENDTRACK), so gate on m_maxWarned to prompt only once per
				// visit to max -- otherwise Cancel has to be clicked for every
				// repeat notification.
				if (!this->m_maxWarned) {
					this->m_maxWarned = true;
					if (!this->ConfirmMaxFan()) {
						// Cancelled: revert the slider to level 7 and apply that.
						::SendMessage((HWND)mp2, TBM_SETPOS, TRUE, 7);
						pos = 7;
						val = 7;
					}
				}
			} else {
				// Off max again -> re-arm the warning for the next visit.
				this->m_maxWarned = false;
			}

			char vb[16];
			_itoa_s(val, vb, 10);
			::SetDlgItemText(this->hwndDialog, 8310, vb);
			this->ModeToDialog(3);   // using the slider selects Manual mode
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
		}
		break;

	case WM_CONTEXTMENU:
	{
		// right-click on the history sparkline -> offer to clear it; anywhere
		// else on the dialog -> offer to copy the current readings
		HWND hSpark = ::GetDlgItem(this->hwndDialog, 8120);
		POINT pt = { (short)LOWORD(mp2), (short)HIWORD(mp2) };
		RECT wr;
		bool onSpark = false;
		if (hSpark && ::GetWindowRect(hSpark, &wr)) {
			if (pt.x == -1 && pt.y == -1) {   // invoked via keyboard
				pt.x = (wr.left + wr.right) / 2;
				pt.y = (wr.top + wr.bottom) / 2;
			}
			if (::PtInRect(&wr, pt)) {
				onSpark = true;
				HMENU hm = ::CreatePopupMenu();
				::AppendMenuA(hm, MF_STRING, 1, "Clear history");
				int sel = ::TrackPopupMenu(hm, TPM_RETURNCMD | TPM_RIGHTBUTTON,
					pt.x, pt.y, 0, this->hwndDialog, NULL);
				::DestroyMenu(hm);
				if (sel == 1) {
					this->m_tempHistCount = 0;
					this->m_tempHistHead = 0;
					::InvalidateRect(hSpark, NULL, TRUE);
				}
				rc = TRUE;
			}
		}
		if (!onSpark) {
			if (pt.x == -1 && pt.y == -1)   // invoked via keyboard
				::GetCursorPos(&pt);
			HMENU hm = ::CreatePopupMenu();
			::AppendMenuA(hm, MF_STRING, 1, "Copy readings");
			int sel = ::TrackPopupMenu(hm, TPM_RETURNCMD | TPM_RIGHTBUTTON,
				pt.x, pt.y, 0, this->hwndDialog, NULL);
			::DestroyMenu(hm);
			if (sel == 1)
				this->CopyReadingsToClipboard();
			rc = TRUE;
		}
		break;
	}

	case WM_CTLCOLORDLG:
		rc = (ULONG)(LONG_PTR)this->m_hbrDlg;
		break;

	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORBTN:
	{
		HDC hdc = (HDC)mp1;
		int cid = ::GetDlgCtrlID((HWND)mp2);
		COLORREF txt = this->m_clrText;   // theme default (dark/light aware)

		// hairline divider statics: just the rule brush, no text to color
		if (msg == WM_CTLCOLORSTATIC && cid >= 9240 && cid <= 9242) {
			rc = (ULONG)(LONG_PTR)this->m_hbrRule;
			break;
		}

		if (cid == 8100 || cid == 8103) {
			// state and switch temp: severity from IconLevels
			txt = this->SeverityColor(this->MaxTemp);
		}
		else if (cid == 9210 || cid == 9198 || cid == 9199 || cid == 9201 ||
		         cid == 9202 || cid == 9220 || cid == 9221) {
			// bold section headers pick up the user's accent color
			txt = this->m_clrAccent;
		}
		else if (msg == WM_CTLCOLORSTATIC &&
				(cid == -1 || cid == 0xFFFF ||           // IDC_STATIC field labels
				 (cid >= 8320 && cid <= 8328) ||         // slider tick labels
				 cid == 9196)) {                         // 'Last change' caption
			// quiet tier: secondary labels recede behind their values
			txt = this->m_clrTextDim;
		}

		// manual fan-level box: red text while it holds a non-EC level (set in
		// the 8310 EN_CHANGE handler), so invalid input is flagged immediately
		// instead of looking accepted until the next poll snaps it back
		if (msg == WM_CTLCOLOREDIT && cid == 8310 && this->m_manualFieldInvalid &&
				!this->m_highContrast)
			txt = RGB(232, 48, 48);

		::SetTextColor(hdc, txt);
		::SetBkMode(hdc, TRANSPARENT);
		rc = (ULONG)(LONG_PTR)(msg == WM_CTLCOLOREDIT ? this->m_hbrField : this->m_hbrDlg);
		break;
	}

	case WM_INITDIALOG:
		// placing code here will NOT work!
		// (put it into BaseDlgProc instead)
		break;

	case WM_TIMER:
		switch (mp1)
		{

		case 1: // update fan state
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			if (this->Log2csv == 1)
			{
				this->Tracecsv(this->CurrentStatuscsv);
			}
			break;

		case 2: // update window title
			if (this->CurrentMode == 3 && this->MaxTemp > this->ManModeExitInternal) {
				this->ModeToDialog(2);
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			}

			res = this->IsMinimized();
			if (res && strcmp(this->LastTitle, this->Title2) != 0)
			{
				strcpy_s(this->LastTitle, sizeof(this->LastTitle), this->Title2);
			}
			else
				if (!res && strcmp(this->LastTitle, this->Title) != 0)
				{
					::SetWindowText(this->hwndDialog, this->Title);
					strcpy_s(this->LastTitle, sizeof(this->LastTitle), this->Title);
				}

			if (this->pTaskbarIcon)
			{
				// multi-line tooltip (mode / max temp / fan / profile / flags),
				// composed in HandleData; falls back to empty before the first read
				this->pTaskbarIcon->SetTooltip(this->TrayTip);   // dedups internally (TASKBARICON::SetTooltip)
				int icon = -1;

				if (this->CurrentModeFromDialog() == 1)
				{
					icon = 10;    // gray
				}
				else
				{
					icon = 11;    // blue
					for (
						int i = 0;
						i < ARRAYMAX(this->IconLevels); i++)
					{
						if (this->MaxTemp >= this->IconLevels[i])
						{
							icon = 12 + i;    // yellow, orange, red
						}
					}
				}

				if (icon != this->CurrentIcon && icon != -1)
				{
					this->pTaskbarIcon->SetIcon(icon);
					this->CurrentIcon = icon;
					if (this->m_showSymbolBalloon && !this->NoBallons) {
						this->pTaskbarIcon->SetBalloon(NIIF_INFO, "TPFanControl v2.34 P15G2 Dual symbol icon",
							"shows temperature level by color and state in tooltip, left click on icon shows or hides control window, right click shows menue",
							11);
						this->m_showSymbolBalloon = false;
					}

				}
				this->iFarbeIconB = icon;
			}
			break;

		case 3: // icon-refresh timer; the icon update itself runs below the switch
			break;

		case 4: // renew tempicon - force recreation; outer block calls ProcessTextIcons
			if (ShowTempIcon && ReIcCycle)
				this->RemoveTextIcons();
			break;

		default:
			break;
		} // End switch mp1

		if (this->ShowTempIcon == 1)
		{
			this->ProcessTextIcons();  //icon Einstieg
		}
		else {
			this->RemoveTextIcons();
		}

		// taskbar button extras (no-op unless ShowInTaskbar): both icon paths
		// above have refreshed iFarbeIconB by now
		this->UpdateTaskbarIndicators();

		// surface any log lines the EC worker thread recorded since the last
		// tick (it can't touch the control itself); cheap no-op when idle
		this->FlushLogToControl();

		//	tell windows not to hold much more memspace
		//	SetProcessWorkingSetSize(GetCurrentProcess(),65536,WANTED_MEM_SIZE);
		break;

	case WM_COMMAND:
		if (
			HIWORD(mp1)	== BN_CLICKED || HIWORD(mp1) == EN_CHANGE)
		{
			int cmd = LOWORD(mp1);

			//display temperature list

			if (cmd == 7001 || cmd == 7002)
			{
				this->ShowAllFromDialog();
				this->UpdateTempList();
				this->icontemp = this->BiasedTemp(this->State.Sensors[this->iMaxTemp], this->iMaxTemp);
			}
			//end temp display

			if (cmd >= 8300 && cmd <= 8302 || cmd == 8310) {  // radio button or manual speed entry
				if (cmd >= 8300 && cmd <= 8302)
					this->UpdateManualControlsEnabled();   // reflect the new mode at once

				if (cmd == 8310) {
					char b[16];
					::GetDlgItemText(hwnd, 8310, b, sizeof(b));
					int v = atoi(b);

					// Immediate feedback while the box holds a non-EC level (8-63,
					// 65+): tint the text red via WM_CTLCOLOREDIT until it's valid
					// again. Tint only - never rewrite the text here, so typing "64"
					// through the intermediate "6" is not disturbed; the poll-time
					// snap-back in HandleData remains the authoritative clamp (and
					// its SetDlgItemText re-enters here, clearing the tint).
					// An empty box (mid-edit) stays neutral.
					bool invalid = b[0] != '\0' &&
						!(isdigit((unsigned char)b[0]) && ((v >= 0 && v <= 7) || v == 64));
					if (invalid != this->m_manualFieldInvalid) {
						this->m_manualFieldInvalid = invalid;
						HWND hBox = ::GetDlgItem(hwnd, 8310);
						if (hBox) ::InvalidateRect(hBox, NULL, TRUE);
					}

					// Typing the max value (64) must raise the same warning as
					// dragging the slider to max, so the prompt can't be bypassed by
					// typing it. Gate on the window being visible so the programmatic
					// startup write (and a minimized start) never pops the dialog.
					// One-shot via m_maxWarned; re-armed when the value drops below max.
					if (::IsWindowVisible(this->hwndDialog)) {
						if (v == 64) {
							if (!this->m_maxWarned) {
								this->m_maxWarned = true;
								if (!this->ConfirmMaxFan())
									::SetDlgItemText(hwnd, 8310, "7");   // re-enters EN_CHANGE; "7" re-arms
							}
						}
						else if (v < 64)
							this->m_maxWarned = false;
					}
				}

				::PostMessage(hwnd, WM__GETDATA, 0, 0);
			}
			else
				switch (cmd) {
				case 7010: // Temp hex checkbox
					this->ShowTempHex = (::SendDlgItemMessage(this->hwndDialog, 7010, BM_GETCHECK, 0, 0) == BST_CHECKED);
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 7011: // Show log checkbox
					this->ShowLog = (::SendDlgItemMessage(this->hwndDialog, 7011, BM_GETCHECK, 0, 0) == BST_CHECKED);
					this->ApplyLogVisibility();
					break;

				case 7012: // Dark mode checkbox (explicit choice overrides auto-follow)
					this->DarkMode = (::SendDlgItemMessage(this->hwndDialog, 7012, BM_GETCHECK, 0, 0) == BST_CHECKED);
					this->DarkModeSetting = this->DarkMode ? 1 : 0;
					this->ApplyTheme();
					break;

				case 7013: // Game mode checkbox
					this->ToggleGameMode();
					break;

				case 5001: // bios
					this->ModeToDialog(1);
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 5002: // smart
					this->ModeToDialog(2);
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 5003: // smart1
					this->ModeToDialog(2);
					if (this->IndSmartLevel == 1) {
						sprintf_s(obuf, sizeof(obuf),
							"Activation of Fan Control Profile 'Smart Mode 1'");
						this->Trace(obuf);
					}
					this->IndSmartLevel = 0;
					// r�berkopieren
					this->ActivateSmartProfile(1);
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 5004: // smart2
					this->ModeToDialog(2);
					if (this->IndSmartLevel == 0) {
						sprintf_s(obuf, sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 2'");
						this->Trace(obuf);
					}
					this->IndSmartLevel = 1;

					this->ActivateSmartProfile(2);
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 5005: // manual
					this->ModeToDialog(3);
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 5010: // show window
					this->ShowMainWindow(true);
					break;

				case 5070: // switch to classic colored symbol icon
					this->ShowTempIcon = 0;
					if (!this->pTaskbarIcon) {   // guard: don't leak an existing icon
						this->pTaskbarIcon = new TASKBARICON(this->hwndDialog, 10, "TPFanControl v2.34 P15G2 Dual");
						this->pTaskbarIcon->SetIcon(this->CurrentIcon);
					}
					break;

				case 5080: // show temp icon
					delete this->pTaskbarIcon;
					this->pTaskbarIcon = NULL;
					this->ShowTempIcon = 1;
					break;

				case 5030: // hide window
					this->ShowMainWindow(false);
					break;

				case 5090: // game mode toggle
					this->ToggleGameMode();
					break;

				case 5100: // settings dialog
					this->ShowSettingsDialog();
					break;

				case 5110: // smart fan-curve editor
					this->ShowCurveDialog();
					break;

				case 5120: // open the log file in the default editor
					OpenCompanionFile(this->hwndDialog, "TPFanControl.log");
					break;

				case 5130: // open the ini config in the default editor
					OpenCompanionFile(this->hwndDialog, "TPFanControl.ini");
					break;

				case 5020: // end program
				// Wait for the work thread to terminate (capture the result: only
				// close the handle below on a confirmed clean exit)
					res = this->hThread ? ::WaitForSingleObject(this->hThread, THREAD_WAIT_TIMEOUT_MS) : WAIT_OBJECT_0;
					if (!this->EcAccess.Lock(100))
					{
						// Something is going on, let's do this later
						this->Trace("Delaying close");
						m_needClose = true;
						break;
					}

					// don't close if we can't set the fan back to bios controlled
					if (!this->ActiveMode || this->SetFan("On close", FAN_CTRL_BIOS, true)) {
						// remember where the window was for next launch
						this->SaveWindowPos("TPFanControl.ini");
						::KillTimer(this->hwndDialog, m_fanTimer);
						::KillTimer(this->hwndDialog, m_titleTimer);
						::KillTimer(this->hwndDialog, m_iconTimer);
						::KillTimer(this->hwndDialog, m_renewTimer);
						// close only after a confirmed clean exit; on timeout leave the
						// handle for the destructor rather than close a live thread
						if (this->hThread && res == WAIT_OBJECT_0) {
							CloseHandle(this->hThread); this->hThread = NULL;
						}
						this->Trace("Exiting ProcessDialog");
						::PostMessage(hwnd, WM__DISMISSDLG, IDCANCEL, 0); // exit from ProcessDialog()
					}
					else
					{
						m_needClose = true;
					}
					this->EcAccess.Unlock();

					break;
				}
		}
		break;

	case WM_POWERBROADCAST:
		if (mp1 == PBT_POWERSETTINGCHANGE) {
			POWERBROADCAST_SETTING* pbs = (POWERBROADCAST_SETTING*)mp2;
			if (pbs->PowerSetting == GUID_LIDSWITCH_STATE_CHANGE) {
				BYTE state = *(BYTE*)(&pbs->Data);
				if (state == 0) {  // Lid closed
					this->isLidClosed = true;
					this->previousModeBeforeLidClose = this->CurrentMode;
					this->Trace("Lid closed detected, will close to BIOS mode.");
					this->ModeToDialog(1);
					ok = this->SetFan("Lid close, Switch to BIOS Mode", FAN_CTRL_BIOS);
					if (ok) {
						this->Trace("Set to BIOS Mode");
						::Sleep(1000);
					}
				}
				else { // Lid opened
					if (this->isLidClosed) {
						// switch back to previous mode
						this->ModeToDialog(this->previousModeBeforeLidClose);
					}
					this->isLidClosed = false;
					this->Trace("Lid opened detected.");
				}
			}
		}
		else if (mp1 == PBT_APMSUSPEND) {
			this->OnSleepTransition(true);
		}
		else if (mp1 == PBT_APMRESUMEAUTOMATIC || mp1 == PBT_APMRESUMESUSPEND) {
			this->OnSleepTransition(false);
		}
		break;

	case WM__SLEEPEVT:
		// Modern Standby entry/exit, marshalled from the event-log thread
		this->OnSleepTransition(mp1 != 0);
		rc = TRUE;
		break;


	case WM_CLOSE:
		//if (this->MinimizeOnClose && (this->MinimizeToSysTray || this->Runs_as_service))   // 0.24 new:  || this->Runs_as_service)
		//{MessageBox(NULL, "will Fenster schlie�en", "TPFanControl", MB_ICONEXCLAMATION);
		this->ShowMainWindow(false);   //}
		rc = TRUE;
		break;

	case WM_ENDSESSION:  //WM_QUERYENDSESSION?
	//if running as service do not end
		if (!this->Runs_as_service) {
			// mp1 (wParam) == TRUE means Windows is actually shutting down.
			// After this handler returns the process can be killed, so restore
			// drivers here rather than relying on the destructor path.
			if (mp1 && this->m_driversHidden)
				this->ToggleGameMode(true);   // silent: no modal dialog can stall shutdown

			// end program
			// Wait for the work thread to terminate (capture the result: we must
			// not close a handle to a thread that is still alive)
			DWORD waitrc = WAIT_OBJECT_0;
			if (this->hThread)
				waitrc = ::WaitForSingleObject(this->hThread, THREAD_WAIT_TIMEOUT_MS);

			// Shutdown path: the process may be terminated as soon as we return,
			// so restore BIOS fan control *synchronously* here. Do NOT defer via
			// m_needClose (unlike the interactive close) — the deferred close
			// depends on future UI messages that may never be delivered during
			// shutdown. SetFan locks the EC itself (reentrant mutex) with its own
			// bounded retry and traces its own OK/FAILED result; best-effort, and
			// won't hang shutdown.
			if (this->ActiveMode)
				this->SetFan("On shutdown", FAN_CTRL_BIOS, true);

			::KillTimer(this->hwndDialog, m_fanTimer);
			::KillTimer(this->hwndDialog, m_titleTimer);
			::KillTimer(this->hwndDialog, m_iconTimer);
			::KillTimer(this->hwndDialog, m_renewTimer);

			// Close the handle only if the worker actually exited. On timeout the
			// worker is still alive (likely wedged in EC retries); leave the handle
			// (the process is terminating anyway) rather than close a live handle.
			// We still dismiss: a late PostMessage from the worker to the soon-to-be
			// destroyed window just fails harmlessly once the message loop stops.
			if (this->hThread && waitrc == WAIT_OBJECT_0) {
				CloseHandle(this->hThread);
				this->hThread = NULL;
			}
			else if (waitrc != WAIT_OBJECT_0) {
				this->Trace("On shutdown: work thread did not exit in time; closing anyway");
			}
			this->Trace("Exiting ProcessDialog");
			::PostMessage(hwnd, WM__DISMISSDLG, IDCANCEL, 0); // exit from ProcessDialog()
		}
		break;

		//		case WM_MOVE:
	case WM_SIZE:
		if (mp1 == SIZE_MINIMIZED && this->MinimizeToSysTray) {
			::ShowWindow(this->hwndDialog, FALSE);
		}
		else if (!this->m_inDpiChange) {   // RescaleForDpi handles its own layout
			this->ReflowLayout();
		}
		rc = TRUE;
		break;

	// double-buffer the whole child tree during interactive drags only (the
	// composited bit has a steady-state cost, so it comes off again on exit);
	// kills the erase/repaint flash of 17 controls per resize tick
	case WM_ENTERSIZEMOVE:
		::SetWindowLongPtr(hwnd, GWL_EXSTYLE,
			::GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_COMPOSITED);
		rc = TRUE;
		break;

	case WM_EXITSIZEMOVE:
		::SetWindowLongPtr(hwnd, GWL_EXSTYLE,
			::GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~WS_EX_COMPOSITED);
		::InvalidateRect(hwnd, NULL, TRUE);
		rc = TRUE;
		break;

	case WM_DPICHANGED:
		// PerMonitorV2: lo-word of wParam = new DPI, lParam = suggested window rect
		this->RescaleForDpi(LOWORD(mp1), (const RECT*)mp2);
		// drop the text tray icon so the next 500ms tick rebuilds it at the
		// (possibly changed) system icon size
		this->RemoveTextIcons();
		rc = TRUE;
		break;

	case WM_DWMCOLORIZATIONCOLORCHANGED:
		// the user changed their accent color: re-derive and repaint consumers
		this->m_clrAccent = this->m_highContrast ? this->m_clrText
			: GetAccentColor(RGB(0, 120, 212),
				(this->DarkMode && !this->m_highContrast) ? TRUE : FALSE);
		::InvalidateRect(hwnd, NULL, TRUE);
		break;

	case WM_THEMECHANGED:
		this->ApplyTheme();
		break;

	case WM_SETTINGCHANGE:
		// follow the OS live: High Contrast toggles re-theme always; dark/light
		// flips only in auto mode (DarkMode=2). Filtered, because unqualified
		// WM_SETTINGCHANGE also fires for wallpaper/locale/etc. changes.
		if (mp1 == SPI_SETHIGHCONTRAST) {
			this->ApplyTheme();
		}
		else if (mp2 && _stricmp((const char*)mp2, "ImmersiveColorSet") == 0) {
			if (this->DarkModeSetting == 2) {
				int sysDark = QuerySystemDark();
				if (sysDark != this->DarkMode) {
					this->DarkMode = sysDark;
					::SendDlgItemMessage(hwnd, 7012, BM_SETCHECK,
						this->DarkMode ? BST_CHECKED : BST_UNCHECKED, 0);
				}
			}
			this->ApplyTheme();   // also re-derives the accent/HC state
		}
		break;

	case WM_GETMINMAXINFO:
		if (this->m_minW > 0) {
			MINMAXINFO* mmi = (MINMAXINFO*)mp2;
			mmi->ptMinTrackSize.x = this->m_minW;
			mmi->ptMinTrackSize.y = this->m_minH;
		}
		break;

	case WM_DESTROY:
		break;

		//
		// USER messages
		//

	case WM__GETDATA:
		// resume settle gate: the EC can return garbage or NAK right after a
		// sleep exit, so skip polls until the window passes, then restore the
		// pre-sleep mode saved by OnSleepTransition and force the smart logic
		// to re-assert its level (the firmware may have reset the register)
		if (this->m_ecResumeDeferUntil) {
			if (::GetTickCount64() < this->m_ecResumeDeferUntil)
				break;
			this->m_ecResumeDeferUntil = 0;
			if (this->m_savedSleepMode >= 0) {
				if (this->SuspendMode == 1 &&
						this->m_savedSleepMode != this->CurrentMode) {
					this->Trace("Resume settle complete - restoring pre-sleep mode");
					this->ModeToDialog(this->m_savedSleepMode);
				}
				this->LastSmartLevel = -1;   // re-decide + rewrite on next poll
				this->m_savedSleepMode = -1;
			}
		}
		if (!this->hThread && !this->FinalSeen)
		{
			this->hThread = this->CreateThread(FANCONTROL_Thread, (ULONG)this);
		}
		break;

	case WM__NEWDATA:
		if (this->hThread) {
			if (::WaitForSingleObject(this->hThread, THREAD_WAIT_TIMEOUT_MS) == WAIT_OBJECT_0) {
				::CloseHandle(this->hThread);
				this->hThread = 0;
			}
			else {
				// worker still alive: do NOT clear/close the handle here, or the
				// shutdown handler below can't wait on it. Leave it intact and let
				// WM_ENDSESSION gate the close on its own WAIT_OBJECT_0 check.
				this->Trace("Work thread did not finish in time, closing to BIOS mode");
				::SendMessage(this->hwndDialog, WM_ENDSESSION, 0, 0);
			}
		}

		ok = mp1;  // equivalent of "ok = this->ReadEcStatus(&this->State);" via thread

		if (ok) {
			this->ReadErrorCount = 0;
			this->HandleData();

			if (m_needClose)
			{
				this->Trace("Program needs to be closed, changing to BIOS mode");
				::Sleep(1000);
				::PostMessage(this->hwndDialog, WM_COMMAND, 5020, 0);
				::SendMessage(this->hwndDialog, WM_ENDSESSION, 0, 0);
				m_needClose = false;
			}
		}
		else {
			sprintf_s(buf, sizeof(buf), "Warning: can't read Status, read error count = %d", this->ReadErrorCount);
			this->Trace(buf);
			sprintf_s(buf, sizeof(buf), "We will close to BIOS-Mode after %d consecutive read errors", this->MaxReadErrors);
			this->Trace(buf);
			this->ReadErrorCount++;
			this->m_ecErrorsTotal++;   // cumulative (ReadErrorCount resets on success)

			// Surface the degrading EC link in the always-visible Status field (8112);
			// the log panel defaults closed so errors are otherwise invisible until the
			// app force-switches to BIOS. Keep the m_lastStatus cache coherent so the
			// next successful read (HandleData's SetDlgTextIfChanged on the same cache)
			// overwrites this automatically.
			{
				char ecstat[128];
				sprintf_s(ecstat, sizeof(ecstat),
					"EC read error %d/%d - showing last good reading",
					this->ReadErrorCount, this->MaxReadErrors);
				if (strcmp(this->m_lastStatus, ecstat) != 0) {
					strcpy_s(this->m_lastStatus, sizeof(this->m_lastStatus), ecstat);
					::SetDlgItemText(this->hwndDialog, 8112, ecstat);
				}
			}

			// after so many consecutive read errors, try to switch back to bios mode
			if (this->ReadErrorCount > this->MaxReadErrors) {
				this->ModeToDialog(1);
				ok = this->SetFan("Max. Errors", FAN_CTRL_BIOS);
				if (ok) {
					this->Trace("Set to BIOS Mode, to many consecutive read errors");
					::Sleep(2000);
					::SendMessage(this->hwndDialog, WM_ENDSESSION, 0, 0);
				}
			}
		}
		break;

	case WM__TASKBAR:
	{
		// NOTIFYICON_VERSION_4 wraps the event in LOWORD(lParam); the legacy
		// protocol passes the raw message, which is < 0x10000, so LOWORD() is
		// correct for both. Under v4 the shell still delivers the raw mouse
		// messages IN ADDITION to NIN_SELECT/WM_CONTEXTMENU, so the legacy
		// cases are gated on !trayV4 or one click would act twice.
		bool trayV4 =
			(this->pTaskbarIcon && this->pTaskbarIcon->m_trayV4) ||
			(this->ppTbTextIcon && this->ppTbTextIcon[0] && this->ppTbTextIcon[0]->TrayV4());

		switch (LOWORD(mp2)) {

		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			// keyboard activation works under v4 (Enter/Space on the focused
			// icon); the shell sends NIN_KEYSELECT twice for Enter - debounce
			static DWORD s_lastSelect = 0;
			DWORD now = ::GetTickCount();
			if (now - s_lastSelect < 250)
				break;
			s_lastSelect = now;
			this->ShowMainWindow(!IsWindowVisible(this->hwndDialog));
			break;
		}

		case WM_CONTEXTMENU:
		{
			// v4 menu (mouse or Shift+F10): anchor coordinates arrive in wParam
			POINT pt = { (short)LOWORD(mp1), (short)HIWORD(mp1) };
			this->ShowTrayMenu(&pt);
			break;
		}

		case WM_LBUTTONDOWN:
			if (trayV4) break;   // NIN_SELECT covers this under v4
			this->ShowMainWindow(!IsWindowVisible(this->hwndDialog));
			break;

		case WM_LBUTTONDBLCLK:
			if (trayV4) break;   // NIN_SELECT covers this under v4
			this->ShowMainWindow(!IsWindowVisible(this->hwndDialog));
			break;

		case WM_RBUTTONDOWN:
			if (trayV4) break;   // WM_CONTEXTMENU covers this under v4
			this->ShowTrayMenu(NULL);
			break;
		}
		rc = TRUE;
		break;
	}

	default:
		break;

	}

	return
		rc;
}

//-------------------------------------------------------------------------
//  reading the EC status may take a while, hence do it in a thread
//-------------------------------------------------------------------------
int
FANCONTROL::WorkThread() {
	int ok = this->ReadEcStatus(&this->State);

	::PostMessage(this->hwndDialog, WM__NEWDATA, ok, 0);

	return 0;
}

// The texticons will be shown depending on variables
static const int MAX_TEXT_ICONS = 16;

//-------------------------------------------------------------------------
//  modal Settings dialog (resource 9300)
//-------------------------------------------------------------------------
INT_PTR CALLBACK
FANCONTROL::SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	FANCONTROL* self = (FANCONTROL*)::GetWindowLongPtr(hwnd, DWLP_USER);

	switch (msg) {
	case WM_INITDIALOG:
		self = (FANCONTROL*)lp;
		::SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)self);

		// Variable body face (no-op pre-Win11), always-visible focus cues, and
		// the bold section headers that anchor the two-tier text hierarchy
		// (header font runs after the body push so it derives from that face)
		ApplyDialogVariableFont(hwnd);
		::SendMessage(hwnd, WM_CHANGEUISTATE,
			MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
		{
			static const int hdrIds[] = { 9340, 9341, 9342, 9343, 9344, 9345 };
			ApplyDialogHeaderFont(hwnd, hdrIds, 6);
		}

		::CheckDlgButton(hwnd, 9301, self->StartMinimized ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9302, self->StayOnTop      ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9328, self->ShowInTaskbar  ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9303, self->ShowTempIcon   ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9304, self->ShowTempHex    ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9305, self->ShowLog        ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9306, self->DarkMode       ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9327, self->DarkModeSetting == 2 ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9307, self->NoBallons      ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9308, self->Log2File       ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9309, self->Log2csv        ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9314, self->ShowGraph      ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9321, self->IconColorFan    ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9322, self->ShowBiasedTemps ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9324, self->NoExtSensor      ? BST_CHECKED : BST_UNCHECKED);
		::SetDlgItemInt(hwnd, 9310, self->Cycle, FALSE);

		// icon color thresholds, shown in the user's current display unit
		{
			int t0 = self->IconLevels[0], t1 = self->IconLevels[1], t2 = self->IconLevels[2];
			if (self->Fahrenheit) { t0 = t0 * 9 / 5 + 32; t1 = t1 * 9 / 5 + 32; t2 = t2 * 9 / 5 + 32; }
			::SetDlgItemInt(hwnd, 9311, t0, FALSE);
			::SetDlgItemInt(hwnd, 9312, t1, FALSE);
			::SetDlgItemInt(hwnd, 9313, t2, FALSE);
			char ub[8];
			sprintf_s(ub, sizeof(ub), "(%s)", TempUnit(self->Fahrenheit));
			::SetDlgItemTextA(hwnd, 9319, ub);
		}

		// thermal fail-safe threshold (stored Celsius; shown in the display unit)
		{
			int fs = self->FailsafeTemp;
			if (self->Fahrenheit && fs > 0) fs = fs * 9 / 5 + 32;
			::SetDlgItemInt(hwnd, 9325, fs, FALSE);
			::SetDlgItemTextA(hwnd, 9326, TempUnit(self->Fahrenheit));
		}

		// match the main window's full dark theme (titlebar + child controls so
		// checkbox/label text is white in dark mode, not theme-drawn black)
		ApplyDarkToDialog(hwnd, self->DarkMode);

		// field tooltips (created lazily, owned by the dialog)
		{
			HWND tip = NULL;
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9303,
				"Show a small temperature number as the tray icon instead of the "
				"classic colored symbol.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9304,
				"Add a column to the temperature list showing each sensor's raw EC "
				"address (hex). For diagnostics.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9305,
				"Show the scrolling Log panel on the right side of the main window.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9306,
				"Dark color scheme for the main window and these dialogs.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9327,
				"Switch between dark and light automatically with the Windows "
				"app theme. Toggling Dark mode by hand turns this off.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9314,
				"Show the temperature history sparkline along the bottom of the main "
				"window.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9301,
				"Start hidden in the tray instead of opening the window.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9302,
				"Keep the main window above other windows.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9328,
				"Show a taskbar button (Alt-Tab, Snap Layouts) with a temperature "
				"badge and fan-level progress, in addition to the tray icon.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9307,
				"Suppress the tray balloon pop-ups (mode changes, warnings).");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9308,
				"Append readings to TPFanControl.log in the program folder.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9309,
				"Append readings to TPFanControl_csv.txt for spreadsheets.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9310,
				"How often (seconds) to re-read temperatures and re-decide the fan. "
				"1-600.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9311,
				"Temperature at which the tray icon turns warm (yellow).");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9312,
				"Temperature at which the tray icon turns hot (orange).");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9313,
				"Temperature at which the tray icon turns critical (red).");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9321,
				"Color the tray icon by current fan speed instead of by temperature.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9322,
				"Apply the per-sensor offsets (from the ini) to the temperatures shown "
				"here, not just to the control logic.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9324,
				"Ignore external / secondary temperature sensors when reading temps.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9325,
				"Thermal fail-safe: if the max temperature reaches this value (in "
				"Smart or Manual mode), the fan is forced to full speed (max) until "
				"it cools ~3 degrees below. 0 disables it. Independent of the fan curve.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9330,
				"Open the Smart fan-curve editor (temperature -> fan level table).");
			ThemeTipWindow(tip, self->DarkMode && !self->m_highContrast);
			// keep the tip reachable for the live re-theme paths (it is an
			// owned popup, so ApplyDarkToDialog's child pass never finds it)
			::SetPropA(hwnd, "TPFC_DLGTIP", (HANDLE)tip);
		}
		// land initial keyboard focus on the poll-interval field (the most-changed
		// setting) instead of the first checkbox; return FALSE so the dialog manager
		// does not override the focus we just set.
		::SetFocus(::GetDlgItem(hwnd, 9310));
		return FALSE;

	case WM_CTLCOLORDLG:
		if (self) return (INT_PTR)self->m_hbrDlg;
		break;

	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if (self) {
			// color the three threshold labels with their severity color so the
			// warm/hot/critical mapping reads at a glance (same hues as the icon)
			COLORREF tc = self->m_clrText;
			if (!self->m_highContrast) {
				switch (::GetDlgCtrlID((HWND)lp)) {
				case 9315: tc = RGB(220, 170, 0); break;   // warm
				case 9316: tc = RGB(232, 120, 0); break;   // hot
				case 9317: tc = RGB(232, 48, 48); break;   // critical
				case 9340: case 9341: case 9342:           // section headers:
				case 9343: case 9344: case 9345:           // accent, like the
					tc = self->m_clrAccent; break;         // main window's
				case 9319: case 9326:                      // unit hints recede
					tc = self->m_clrTextDim; break;
				}
			}
			::SetTextColor((HDC)wp, tc);
			::SetBkMode((HDC)wp, TRANSPARENT);
			return (INT_PTR)self->m_hbrDlg;
		}
		break;

	case WM_CTLCOLOREDIT:
		if (self) {
			::SetTextColor((HDC)wp, self->m_clrText);
			::SetBkMode((HDC)wp, TRANSPARENT);
			return (INT_PTR)self->m_hbrField;
		}
		break;

	case WM_SETTINGCHANGE:
		// auto theme: flip this modal live with the OS. Deferred via a posted
		// message because the broadcast order against the main window is
		// unspecified - the post is retrieved after the main window's
		// ApplyTheme has rebuilt the shared brushes/colors.
		if (self && self->DarkModeSetting == 2 && lp &&
				_stricmp((const char*)lp, "ImmersiveColorSet") == 0)
			::PostMessage(hwnd, WM__RETHEMEDLG, 0, 0);
		break;

	case WM__RETHEMEDLG:
		if (self) {
			int dark = QuerySystemDark();
			ApplyDarkToDialog(hwnd, dark);
			ThemeTipWindow((HWND)::GetPropA(hwnd, "TPFC_DLGTIP"),
				dark && !IsHighContrast());
			::CheckDlgButton(hwnd, 9306, dark ? BST_CHECKED : BST_UNCHECKED);
		}
		break;

	case WM_DWMCOLORIZATIONCOLORCHANGED:
		// the main window's handler already refreshed the shared m_clrAccent;
		// statics only repaint when invalidated, so do that here too
		::InvalidateRect(hwnd, NULL, TRUE);
		break;

	case WM_DESTROY:
		FreeDialogVariableFont(hwnd);
		FreeDialogHeaderFont(hwnd);
		::RemovePropA(hwnd, "TPFC_DLGTIP");   // tip itself dies with its owner
		break;

	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			// keep the dialog open if a value was out of range (feedback shown)
			if (self && !self->ApplySettingsFromDialog(hwnd))
				return TRUE;
			::EndDialog(hwnd, IDOK);
			return TRUE;

		case 9320: // Apply: persist + live-apply but keep the dialog open
			if (self) {
				self->ApplySettingsFromDialog(hwnd);
				// dark-mode may have just changed: re-theme this dialog (and
				// its owned tooltip popup, which the child pass can't reach),
				// re-seat the checkbox (follow-system can override its state)
				ApplyDarkToDialog(hwnd, self->DarkMode);
				ThemeTipWindow((HWND)::GetPropA(hwnd, "TPFC_DLGTIP"),
					self->DarkMode && !self->m_highContrast);
				::CheckDlgButton(hwnd, 9306, self->DarkMode ? BST_CHECKED : BST_UNCHECKED);
			}
			return TRUE;

		case 9330: // open the Smart fan-curve editor (modal, owned by Settings)
			if (self) self->ShowCurveDialog(hwnd);
			return TRUE;

		case IDCANCEL:
			::EndDialog(hwnd, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

//-------------------------------------------------------------------------
//  read the Settings controls, persist to ini, and live-apply (shared by
//  the OK and Apply buttons)
//-------------------------------------------------------------------------
bool
FANCONTROL::ApplySettingsFromDialog(HWND hwnd)
{
	bool badCycle = false, badThresholds = false;   // out-of-range fields that were rejected
	int oldDark  = this->DarkMode;
	int oldLog   = this->ShowLog;
	int oldTop   = this->StayOnTop;
	int oldIcon  = this->ShowTempIcon;
	int oldCycle = this->Cycle;
	int oldTb    = this->ShowInTaskbar;

	this->StartMinimized = (::IsDlgButtonChecked(hwnd, 9301) == BST_CHECKED);
	this->StayOnTop      = (::IsDlgButtonChecked(hwnd, 9302) == BST_CHECKED);
	this->ShowInTaskbar  = (::IsDlgButtonChecked(hwnd, 9328) == BST_CHECKED);
	this->ShowTempIcon   = (::IsDlgButtonChecked(hwnd, 9303) == BST_CHECKED);
	this->ShowTempHex    = (::IsDlgButtonChecked(hwnd, 9304) == BST_CHECKED);
	this->ShowLog        = (::IsDlgButtonChecked(hwnd, 9305) == BST_CHECKED);
	{
		// "Follow system theme" wins over the Dark mode checkbox; an unchecked
		// follow box makes the Dark mode state an explicit choice
		bool followSys = (::IsDlgButtonChecked(hwnd, 9327) == BST_CHECKED);
		bool wantDark  = (::IsDlgButtonChecked(hwnd, 9306) == BST_CHECKED);
		this->DarkModeSetting = followSys ? 2 : (wantDark ? 1 : 0);
		this->DarkMode = followSys ? QuerySystemDark() : (wantDark ? 1 : 0);
	}
	this->NoBallons      = (::IsDlgButtonChecked(hwnd, 9307) == BST_CHECKED);
	this->Log2File       = (::IsDlgButtonChecked(hwnd, 9308) == BST_CHECKED);
	this->Log2csv        = (::IsDlgButtonChecked(hwnd, 9309) == BST_CHECKED);
	this->ShowGraph      = (::IsDlgButtonChecked(hwnd, 9314) == BST_CHECKED);
	this->IconColorFan    = (::IsDlgButtonChecked(hwnd, 9321) == BST_CHECKED);
	this->ShowBiasedTemps = (::IsDlgButtonChecked(hwnd, 9322) == BST_CHECKED);
	this->NoExtSensor     = (::IsDlgButtonChecked(hwnd, 9324) == BST_CHECKED);
	{
		BOOL ok = FALSE;
		int c = (int)::GetDlgItemInt(hwnd, 9310, &ok, FALSE);
		if (ok && c >= 1 && c <= 600) this->Cycle = c;
		else                          badCycle = true;
	}

	// icon color thresholds (fields are in the display unit -> store as Celsius)
	{
		BOOL k0 = FALSE, k1 = FALSE, k2 = FALSE;
		int v0 = (int)::GetDlgItemInt(hwnd, 9311, &k0, FALSE);
		int v1 = (int)::GetDlgItemInt(hwnd, 9312, &k1, FALSE);
		int v2 = (int)::GetDlgItemInt(hwnd, 9313, &k2, FALSE);
		if (k0 && k1 && k2) {
			if (this->Fahrenheit) {
				v0 = (v0 - 32) * 5 / 9; v1 = (v1 - 32) * 5 / 9; v2 = (v2 - 32) * 5 / 9;
			}
			// require sane, strictly-ascending thresholds or leave unchanged
			if (v0 >= 1 && v2 <= 120 && v0 < v1 && v1 < v2) {
				this->IconLevels[0] = v0;
				this->IconLevels[1] = v1;
				this->IconLevels[2] = v2;
			}
			else badThresholds = true;
		}
		else badThresholds = true;   // a threshold field was empty/non-numeric
	}

	// thermal fail-safe threshold (field is in the display unit -> store Celsius;
	// 0 = off, otherwise clamp to a sane ceiling)
	{
		BOOL okfs = FALSE;
		int fs = (int)::GetDlgItemInt(hwnd, 9325, &okfs, FALSE);
		if (okfs) {
			if (this->Fahrenheit && fs > 0) fs = (fs - 32) * 5 / 9;
			if (fs < 0)   fs = 0;
			if (fs > 120) fs = 120;
			this->FailsafeTemp = fs;
			if (fs == 0) this->m_failsafeTripped = false;   // disabling clears any trip
		}
	}

	this->SaveConfig("TPFanControl.ini");

	HWND main = this->hwndDialog;

	// keep the in-window checkboxes in sync with the new state
	::SendDlgItemMessage(main, 7010, BM_SETCHECK, this->ShowTempHex ? BST_CHECKED : BST_UNCHECKED, 0);
	::SendDlgItemMessage(main, 7011, BM_SETCHECK, this->ShowLog     ? BST_CHECKED : BST_UNCHECKED, 0);
	::SendDlgItemMessage(main, 7012, BM_SETCHECK, this->DarkMode    ? BST_CHECKED : BST_UNCHECKED, 0);

	// live-apply the cheap changes
	if (this->DarkMode != oldDark) this->ApplyTheme();
	if (this->ShowLog != oldLog)   this->ApplyLogVisibility();
	if (this->StayOnTop != oldTop)
		::SetWindowPos(main, this->StayOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
			0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	if (this->ShowInTaskbar != oldTb)
		this->ApplyTaskbarPresence();
	if (this->Cycle != oldCycle) {
		::KillTimer(main, 1);
		this->m_fanTimer = ::SetTimer(main, 1, 1000 * this->Cycle, NULL);
	}
	if (this->ShowTempIcon != oldIcon) {
		if (this->ShowTempIcon == 0) {
			// classic colored symbol icon
			if (!this->pTaskbarIcon) {
				this->pTaskbarIcon = new TASKBARICON(main, 10, "TPFanControl");
				this->pTaskbarIcon->SetIcon(this->CurrentIcon);
			}
		}
		else {
			// text temperature icon (recreated by the icon timer)
			if (this->pTaskbarIcon) {
				delete this->pTaskbarIcon;
				this->pTaskbarIcon = NULL;
			}
		}
	}

	// show/hide the temperature history graph (incl. its divider)
	{
		int sw = this->ShowGraph ? SW_SHOW : SW_HIDE;
		::ShowWindow(::GetDlgItem(main, 9202), sw);
		::ShowWindow(::GetDlgItem(main, 8120), sw);
		::ShowWindow(::GetDlgItem(main, 9242), sw);
	}

	// refresh temps/list so the hex column and icon repaint immediately
	::PostMessage(main, WM__GETDATA, 0, 0);

	// Surface rejected out-of-range values instead of silently dropping them:
	// re-seat the offending field(s) to what is actually in effect and name the
	// valid range. (The fail-safe field is clamped, not dropped, so it is excluded
	// here - it always reflects the stored 0-120 value.)
	if (badCycle || badThresholds) {
		if (badCycle)
			::SetDlgItemInt(hwnd, 9310, this->Cycle, FALSE);
		if (badThresholds) {
			int t0 = this->IconLevels[0], t1 = this->IconLevels[1], t2 = this->IconLevels[2];
			if (this->Fahrenheit) { t0 = t0 * 9 / 5 + 32; t1 = t1 * 9 / 5 + 32; t2 = t2 * 9 / 5 + 32; }
			::SetDlgItemInt(hwnd, 9311, t0, FALSE);
			::SetDlgItemInt(hwnd, 9312, t1, FALSE);
			::SetDlgItemInt(hwnd, 9313, t2, FALSE);
		}
		char msg[256] = "Some values were out of range and left unchanged:\r\n";
		if (badCycle)      strcat_s(msg, sizeof(msg), "\r\n\xb7 Poll interval must be 1-600 seconds.");
		if (badThresholds) strcat_s(msg, sizeof(msg), "\r\n\xb7 Icon thresholds must ascend (warm < hot < critical), within 1-120.");
		wchar_t wmsg[256] = L"";
		if (badCycle)      wcscat_s(wmsg, _countof(wmsg), L"\x2022 Poll interval must be 1-600 seconds.\n");
		if (badThresholds) wcscat_s(wmsg, _countof(wmsg), L"\x2022 Icon thresholds must ascend (warm < hot < critical), within 1-120.\n");
		ModernConfirm(hwnd, L"Settings",
			L"Some values were out of range and left unchanged", wmsg,
			TDCBF_OK_BUTTON, TD_WARNING_ICON,
			msg, "Settings", MB_OK | MB_ICONWARNING);
		return false;
	}
	return true;
}

void FANCONTROL::ShowSettingsDialog()
{
	::DialogBoxParam(this->hinstapp, MAKEINTRESOURCE(9300),
		this->hwndDialog, (DLGPROC)FANCONTROL::SettingsDlgProc, (LPARAM)this);
}

//-------------------------------------------------------------------------
//  Smart fan-curve editor (dialog 9400)
//-------------------------------------------------------------------------

// row r, column c (0=temp,1=fan,2=hystUp,3=hystDown) -> dialog control id
static inline int CurveCellId(int r, int c) { return 9410 + r * 4 + c; }

// to/from the display unit: in-memory SmartLevels are Celsius; temps convert with
// the 32 offset, hysteresis values are deltas (scale only). Mirrors ReadConfig.
static inline int CurveToDisplayTemp(int c, bool f)  { return f ? c * 9 / 5 + 32 : c; }
static inline int CurveToDisplayDelta(int c, bool f) { return f ? c * 9 / 5      : c; }
static inline int CurveFromDisplayTemp(int d, bool f)  { return f ? (d - 32) * 5 / 9 : d; }
static inline int CurveFromDisplayDelta(int d, bool f) { return f ? d * 5 / 9         : d; }

// copy a profile's live table (SmartLevels1 / SmartLevels2, Celsius) into the
// working buffer, converted to the display unit. Empty rows get temp = INT_MIN.
void
FANCONTROL::CurveLoadProfileToBuf(int profile) {
	bool f = this->Fahrenheit != 0;
	for (int r = 0; r < CURVE_ROWS; r++) {
		int t, fan, hu, hd;
		if (profile == 0) {
			t = this->SmartLevels1[r].temp1; fan = this->SmartLevels1[r].fan1;
			hu = this->SmartLevels1[r].hystUp1; hd = this->SmartLevels1[r].hystDown1;
		}
		else {
			t = this->SmartLevels2[r].temp2; fan = this->SmartLevels2[r].fan2;
			hu = this->SmartLevels2[r].hystUp2; hd = this->SmartLevels2[r].hystDown2;
		}
		// terminator (-1) or "no profile 2" (temp 0 at row 0) ends the table
		if (t == -1 || (profile == 1 && r == 0 && t == 0)) {
			for (; r < CURVE_ROWS; r++) m_ceBuf[profile][r].temp = INT_MIN;
			return;
		}
		m_ceBuf[profile][r].temp     = CurveToDisplayTemp(t, f);
		m_ceBuf[profile][r].fan      = fan;
		m_ceBuf[profile][r].hystUp   = CurveToDisplayDelta(hu, f);
		m_ceBuf[profile][r].hystDown = CurveToDisplayDelta(hd, f);
	}
}

// working buffer -> the on-screen edit boxes (blank where temp is unset)
void
FANCONTROL::CurveBufToGrid(HWND hwnd, int profile) {
	for (int r = 0; r < CURVE_ROWS; r++) {
		const CURVEROW& row = m_ceBuf[profile][r];
		if (row.temp == INT_MIN) {
			for (int c = 0; c < 4; c++) ::SetDlgItemTextA(hwnd, CurveCellId(r, c), "");
			continue;
		}
		::SetDlgItemInt(hwnd, CurveCellId(r, 0), row.temp, TRUE);
		::SetDlgItemInt(hwnd, CurveCellId(r, 1), row.fan, FALSE);
		::SetDlgItemInt(hwnd, CurveCellId(r, 2), row.hystUp, FALSE);
		::SetDlgItemInt(hwnd, CurveCellId(r, 3), row.hystDown, FALSE);
	}
}

// the on-screen edit boxes -> working buffer. A row counts only if its Temp box
// is non-empty; blank Temp marks an unused row (temp = INT_MIN).
void
FANCONTROL::CurveGridToBuf(HWND hwnd, int profile) {
	for (int r = 0; r < CURVE_ROWS; r++) {
		char tb[16];
		::GetDlgItemTextA(hwnd, CurveCellId(r, 0), tb, sizeof(tb));
		char* s = tb; while (*s == ' ' || *s == '\t') s++;
		if (*s == 0) { m_ceBuf[profile][r].temp = INT_MIN; continue; }

		BOOL ok = FALSE;
		m_ceBuf[profile][r].temp     = (int)::GetDlgItemInt(hwnd, CurveCellId(r, 0), &ok, TRUE);
		m_ceBuf[profile][r].fan      = (int)::GetDlgItemInt(hwnd, CurveCellId(r, 1), NULL, FALSE);
		m_ceBuf[profile][r].hystUp   = (int)::GetDlgItemInt(hwnd, CurveCellId(r, 2), NULL, FALSE);
		m_ceBuf[profile][r].hystDown = (int)::GetDlgItemInt(hwnd, CurveCellId(r, 3), NULL, FALSE);
	}
}

INT_PTR CALLBACK
FANCONTROL::CurveDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	FANCONTROL* self = (FANCONTROL*)::GetWindowLongPtr(hwnd, DWLP_USER);

	switch (msg) {
	case WM_INITDIALOG:
		self = (FANCONTROL*)lp;
		::SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)self);

		// Variable body face, focus cues, bold column headers (as in Settings)
		ApplyDialogVariableFont(hwnd);
		::SendMessage(hwnd, WM_CHANGEUISTATE,
			MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
		{
			static const int hdrIds[] = { 9470, 9471, 9472, 9473 };
			ApplyDialogHeaderFont(hwnd, hdrIds, 4);
		}

		// load both profiles into working buffers, show profile 1
		self->CurveLoadProfileToBuf(0);
		self->CurveLoadProfileToBuf(1);
		self->m_ceProfile = 0;
		::CheckRadioButton(hwnd, 9401, 9402, 9401);
		{
			char ub[8];
			sprintf_s(ub, sizeof(ub), "(%s)", TempUnit(self->Fahrenheit));
			::SetDlgItemTextA(hwnd, 9461, ub);
		}
		self->CurveBufToGrid(hwnd, 0);

		ApplyDarkToDialog(hwnd, self->DarkMode);

		// field tooltips: profile radios + a per-column tip on every grid cell
		{
			HWND tip = NULL;
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9401,
				"Profile 1: the active Smart curve. At least one row is required.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9402,
				"Profile 2: an optional second curve you can switch to from the tray "
				"menu. Leave every row blank to disable it.");
			const char* colTip[4] = {
				"Temp: the temperature threshold for this step (in the displayed "
				"unit). Leave blank for an unused row.",
				"Fan: the level to hold at/above this temperature. 0 = off, 1-7 = "
				"increasing speed, 64 = max, 128 = hand back to BIOS.",
				"Hyst+: extra degrees above the threshold required before stepping "
				"UP to this row (reduces fan hunting).",
				"Hyst-: degrees below the threshold required before stepping DOWN "
				"from this row (reduces fan hunting)."
			};
			for (int r = 0; r < FANCONTROL::CURVE_ROWS; r++)
				for (int c = 0; c < 4; c++)
					tip = AddDialogTip(hwnd, tip, self->hinstapp,
						9410 + r * 4 + c, colTip[c]);
			ThemeTipWindow(tip, self->DarkMode && !self->m_highContrast);
			::SetPropA(hwnd, "TPFC_DLGTIP", (HANDLE)tip);
		}
		return TRUE;

	case WM_CTLCOLORDLG:
		if (self) return (INT_PTR)self->m_hbrDlg;
		break;
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if (self) {
			// two-tier hierarchy: accent column headers, dimmed hints
			COLORREF tc = self->m_clrText;
			if (!self->m_highContrast) {
				int ccid = ::GetDlgCtrlID((HWND)lp);
				if (ccid >= 9470 && ccid <= 9473)
					tc = self->m_clrAccent;
				else if (ccid == -1 || ccid == 0xFFFF || ccid == 9461)
					tc = self->m_clrTextDim;   // 'Profile:', footnote, unit hint
			}
			::SetTextColor((HDC)wp, tc);
			::SetBkMode((HDC)wp, TRANSPARENT);
			return (INT_PTR)self->m_hbrDlg;
		}
		break;
	case WM_CTLCOLOREDIT:
		if (self) {
			::SetTextColor((HDC)wp, self->m_clrText);
			::SetBkMode((HDC)wp, TRANSPARENT);
			return (INT_PTR)self->m_hbrField;
		}
		break;

	case WM_SETTINGCHANGE:
		// auto theme: flip this modal live with the OS (deferred - see the
		// matching SettingsDlgProc case for the broadcast-order rationale)
		if (self && self->DarkModeSetting == 2 && lp &&
				_stricmp((const char*)lp, "ImmersiveColorSet") == 0)
			::PostMessage(hwnd, WM__RETHEMEDLG, 0, 0);
		break;

	case WM__RETHEMEDLG:
		if (self) {
			int dark = QuerySystemDark();
			ApplyDarkToDialog(hwnd, dark);
			ThemeTipWindow((HWND)::GetPropA(hwnd, "TPFC_DLGTIP"),
				dark && !IsHighContrast());
		}
		break;

	case WM_DWMCOLORIZATIONCOLORCHANGED:
		::InvalidateRect(hwnd, NULL, TRUE);   // repaint accent column headers
		break;

	case WM_DESTROY:
		FreeDialogVariableFont(hwnd);
		FreeDialogHeaderFont(hwnd);
		::RemovePropA(hwnd, "TPFC_DLGTIP");
		break;

	case WM_COMMAND:
		if (!self) break;
		switch (LOWORD(wp)) {
		case 9401:   // profile 1 selected
		case 9402: { // profile 2 selected
			int want = (LOWORD(wp) == 9402) ? 1 : 0;
			if (want != self->m_ceProfile) {
				self->CurveGridToBuf(hwnd, self->m_ceProfile);   // keep current edits
				self->m_ceProfile = want;
				self->CurveBufToGrid(hwnd, want);
			}
			return TRUE;
		}

		case IDOK:
		case 9460:   // Apply
			self->CurveGridToBuf(hwnd, self->m_ceProfile);
			if (self->CurveApplyAndSave()) {
				if (LOWORD(wp) == IDOK) { ::EndDialog(hwnd, IDOK); return TRUE; }
				// Apply: reload the now-canonical tables so the grid reflects any
				// normalization (sorting, dropped blank rows) and re-theme.
				self->CurveLoadProfileToBuf(0);
				self->CurveLoadProfileToBuf(1);
				self->CurveBufToGrid(hwnd, self->m_ceProfile);
			}
			return TRUE;

		case IDCANCEL:
			::EndDialog(hwnd, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

// validate both working buffers, commit them to SmartLevels1/2 (Celsius, sorted,
// terminated), re-activate the live profile and persist to the ini. Returns false
// (leaving live state untouched) if validation fails, after telling the user why.
bool
FANCONTROL::CurveApplyAndSave() {
	bool f = this->Fahrenheit != 0;

	// a row's fan byte must be a real EC level
	auto fanOk = [](int v) {
		return (v >= 0 && v <= 7) || v == 64 || v == 128;
	};

	// gather, convert and sort one profile; returns count, or -1 on a bad value
	CURVEROW out[2][CURVE_ROWS];
	int count[2] = { 0, 0 };
	for (int p = 0; p < 2; p++) {
		int n = 0;
		for (int r = 0; r < CURVE_ROWS; r++) {
			const CURVEROW& row = m_ceBuf[p][r];
			if (row.temp == INT_MIN) continue;
			int tC  = CurveFromDisplayTemp(row.temp, f);
			int huC = CurveFromDisplayDelta(row.hystUp, f);
			int hdC = CurveFromDisplayDelta(row.hystDown, f);
			if (tC < 0 || tC > 150 || !fanOk(row.fan) ||
			    huC < 0 || huC > 50 || hdC < 0 || hdC > 50) {
				char m[160];
				sprintf_s(m, sizeof(m),
					"Profile %d has an invalid row.\n\n"
					"Temp must be 0-150, Fan must be 0-7, 64 or 128,\n"
					"and the hysteresis values 0-50.", p + 1);
				::MessageBoxA(this->hwndDialog, m, "Fan curve", MB_ICONWARNING);
				return false;
			}
			out[p][n].temp = tC; out[p][n].fan = row.fan;
			out[p][n].hystUp = huC; out[p][n].hystDown = hdC;
			n++;
		}
		// insertion-sort by temperature ascending (the Smart algorithm walks the
		// table low->high), keeping the small table cheap and stable
		for (int i = 1; i < n; i++) {
			CURVEROW key = out[p][i];
			int j = i - 1;
			while (j >= 0 && out[p][j].temp > key.temp) { out[p][j + 1] = out[p][j]; j--; }
			out[p][j + 1] = key;
		}
		count[p] = n;
	}

	if (count[0] < 1) {
		::MessageBoxA(this->hwndDialog,
			"Profile 1 needs at least one row (it is the active Smart curve).",
			"Fan curve", MB_ICONWARNING);
		return false;
	}

	// commit profile 1
	for (int i = 0; i < count[0]; i++) {
		this->SmartLevels1[i].temp1     = out[0][i].temp;
		this->SmartLevels1[i].fan1      = out[0][i].fan;
		this->SmartLevels1[i].hystUp1   = out[0][i].hystUp;
		this->SmartLevels1[i].hystDown1 = out[0][i].hystDown;
	}
	this->SmartLevels1[count[0]].temp1 = -1;
	this->SmartLevels1[count[0]].fan1  = 0x80;

	// commit profile 2 (count 0 => "no second profile": temp2[0] = 0)
	if (count[1] == 0) {
		this->SmartLevels2[0].temp2 = 0;
		this->SmartLevels2[0].fan2  = 0;
	}
	else {
		for (int i = 0; i < count[1]; i++) {
			this->SmartLevels2[i].temp2     = out[1][i].temp;
			this->SmartLevels2[i].fan2      = out[1][i].fan;
			this->SmartLevels2[i].hystUp2   = out[1][i].hystUp;
			this->SmartLevels2[i].hystDown2 = out[1][i].hystDown;
		}
		this->SmartLevels2[count[1]].temp2 = -1;
		this->SmartLevels2[count[1]].fan2  = 0x80;
	}

	// if profile 2 was the active one but is now empty, fall back to profile 1
	if (this->IndSmartLevel == 1 && count[1] == 0)
		this->IndSmartLevel = 0;

	// refresh the live table from the edited profile and reset hysteresis state
	this->ActivateSmartProfile(this->IndSmartLevel + 1);

	// persist the new curves to the ini and re-run a Smart decision immediately
	this->SaveCurves("TPFanControl.ini");
	if (this->CurrentModeFromDialog() == 2)
		::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
	return true;
}

void FANCONTROL::ShowCurveDialog(HWND owner)
{
	// owner = the Settings dialog when opened from there (correct nested-modal
	// enable/disable), else the main window for the tray-menu entry
	::DialogBoxParam(this->hinstapp, MAKEINTRESOURCE(9400),
		owner ? owner : this->hwndDialog, (DLGPROC)FANCONTROL::CurveDlgProc, (LPARAM)this);
}

void FANCONTROL::ProcessTextIcons(void) {
	TCHAR myszTip[128];
	int& icon    = this->m_textIcon;       // current text-icon color id (persists across calls)
	int& oldicon = this->m_textIconPrev;   // previous id, for IconColorFan "keep last"

	oldicon = icon;
	if (this->CurrentModeFromDialog() == 1) {
		icon = 10;    // gray
	}
	else {
		icon = 11;    // blue
		for (int i = 0; i < ARRAYMAX(this->IconLevels); i++) {
			if (this->MaxTemp >= this->IconLevels[i]) {
				icon = 12 + i;    // yellow, orange, red
			}
		}
	}

	// Fan-speed coloring (green shades 21-25) applies only while the temperature
	// is still in the safe/green band (icon == 11). Once a warm/hot/critical
	// threshold is crossed, the temperature color (amber/orange/red) wins, so
	// thermal state is always visible even with IconColorFan enabled.
	if (this->IconColorFan && icon == 11) {
		switch (fan1speed / 1000) {
		case 0:
			break;
		case 1:
			icon = 21; //sehr hell gr�n
			break;
		case 2:
			icon = 22; //hell gr�n
			break;
		case 3:
			icon = 23; //gr�n
			break;
		case 4:
			icon = 24; //dunkel gr�n
			break;
		case 5:
			icon = 25; //sehr dunkel gr�n
			break;
		case 6:
			icon = 25; //sehr dunkel gr�n
			break;
		case 7:
			icon = 25; //sehr dunkel gr�n
			break;
		case 8:
			icon = 25; //sehr dunkel gr�n
			break;
		default:
			icon = oldicon;
			break;
		};
	}


	this->iFarbeIconB = icon;

	lstrcpyn(myszTip, this->TrayTip, _countof(myszTip));   // count is in TCHARs, not bytes

	if (pTextIconMutex->Lock(100)) {
		//INIT ppTbTextIcon
		if (!ppTbTextIcon || this->TaskbarNew) {
			this->TaskbarNew = 0;
			ppTbTextIcon = new CTaskbarTextIcon * [MAX_TEXT_ICONS];
			for (int i = 0; i < MAX_TEXT_ICONS; ++i) {
				ppTbTextIcon[i] = NULL;
			}

			//erstmal nur eins

			ppTbTextIcon[0] = new CTaskbarTextIcon(this->m_hinstapp,
				this->hwndDialog, WM__TASKBAR, 0, "", "",  //WM_APP+5000 -> WM__TASKBAR
				this->iFarbeIconB, this->iFontIconB, myszTip);

			if (this->m_showTextBalloon && !this->NoBallons) {
				// brand the intro toast with the live severity icon (resources
				// 10-14 / 21-25 cover every iFarbeIconB value)
				HICON hToast = NULL;
				::LoadIconMetric(this->m_hinstapp,
					MAKEINTRESOURCEW(this->iFarbeIconB), LIM_LARGE, &hToast);
				if (Fahrenheit) {
					ppTbTextIcon[0]->DiShowballon(
						_T("shows max. temperature in \xb0 F and sensor name, left click on icon shows or hides control window, right click shows menue"),
						_T("TPFanControl v2.34 P15G2 Dual text icon"), NIIF_INFO, 11, hToast);
				}
				else {
					ppTbTextIcon[0]->DiShowballon(
						_T("shows max. temperature in \xb0 C and sensor name, left click on icon shows or hides control window, right click shows menue"),
						_T("TPFanControl v2.34 P15G2 Dual text icon"), NIIF_INFO, 11, hToast);
				}
				if (hToast)
					::DestroyIcon(hToast);   // the shell copied it

				// Input:
				//  szText: [in] Text for the balloon tooltip.
				//  szTitle: [in] Title for the balloon.  This text is shown in bold above
				//           the tooltip text (szText).  Pass "" if you don't want a title.
				//  dwIcon: [in] Specifies an icon to appear in the balloon.  Legal values are:
				//                 NIIF_NONE: No icon
				//                 NIIF_INFO: Information
				//                 NIIF_WARNING: Exclamation
				//                 NIIF_ERROR: Critical error (red circle with X)
				//  uTimeout: [in] Number of seconds for the balloon to remain visible.  Can
				//            be between 10 and 30 inclusive.
				//

				this->m_showTextBalloon = false;
			}
		}

		char str_value[256];
		//	char buf[256]= "";
		//  aktualisieren
		for (int i = 0; i < MAX_TEXT_ICONS; ++i) {
			if (ppTbTextIcon[i]) {
				if (Fahrenheit)
					_itoa_s((this->icontemp * 9 / 5) + 32, str_value, sizeof(str_value), 10);
				else
					_itoa_s(this->icontemp, str_value, sizeof(str_value), 10);
				// tiny icon: show just the temperature (no sensor-name line). The
				// sensor name is still in the tooltip (myszTip).
				ppTbTextIcon[i]->ChangeText(str_value, "", iFarbeIconB, iFontIconB, myszTip);
			}
		}
		pTextIconMutex->Unlock();
		//this->Trace(LastTooltip); 
	}
}

void FANCONTROL::RemoveTextIcons(void) {
	// Fast path: nothing allocated -> skip the named-kernel-mutex acquire/release.
	// In symbol-icon mode this is called every 500ms title-timer tick with nothing
	// to free; ppTbTextIcon is only (de)allocated on this (UI) thread, so a NULL
	// read here cannot miss a real cleanup.
	if (!ppTbTextIcon)
		return;

	if (pTextIconMutex->Lock(10000)) {
		if (ppTbTextIcon) {
			for (int i = 0; i < MAX_TEXT_ICONS; ++i) {
				if (ppTbTextIcon[i]) {
					delete ppTbTextIcon[i];
				}
			}
			delete[] ppTbTextIcon;
			ppTbTextIcon = NULL;
		}
		pTextIconMutex->Unlock();
	}
	else {
		_ASSERT(false);//Mutex not av within 10 sec
	}
}
