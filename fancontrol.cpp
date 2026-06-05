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

// WM_DPICHANGED arrived in the Win8.1 SDK headers (_WIN32_WINNT >= 0x0603);
// this app targets Vista (0x0600), so define it locally. The message is simply
// ignored by pre-8.1 systems, which never send it.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif


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

	// theme defaults (overridable via TPFanControl.ini, toggled in-app)
	this->ShowTempHex = 0;
	this->ShowLog = 0;
	this->DarkMode = 1;
	this->ShowGraph = 1;
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
	this->m_hFontHdr = NULL;
	this->m_hFontBig = NULL;
	this->m_hFontTitle = NULL;
	this->m_hFontDlg = NULL;
	this->m_curDpi = 0;
	this->m_inDpiChange = FALSE;
	this->m_clrText = RGB(32, 32, 32);
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

	if (this->hwndDialog) {
		::GetWindowText(this->hwndDialog, this->Title, sizeof(this->Title));

		strcat_s(this->Title, sizeof(this->Title), this->Title3);

		::SetWindowText(this->hwndDialog, this->Title);

		::SetWindowLongPtr(this->hwndDialog, GWLP_USERDATA, (LONG_PTR)this);

		::SendDlgItemMessage(this->hwndDialog, 8112, EM_LIMITTEXT, 256, 0);

		::SendDlgItemMessage(this->hwndDialog, 9200, EM_LIMITTEXT, 4096, 0);

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

			::SendDlgItemMessage(this->hwndDialog, 9200, EM_LIMITTEXT, 4096, 0);

			_itoa_s(this->ManFanSpeed, buf, 10);

			::SetDlgItemText(this->hwndDialog, 8310, buf);

			this->ShowAllToDialog(ShowAll);

			this->hPowerNotify = RegisterPowerSettingNotification(this->hwndDialog, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE);
			this->InitThemeAndChrome();
		}
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
	                   "64 = maximum. Typing here switches to Manual mode.");
	this->AddTip(8311, "Drag to set the manual fan level: 0 = off, 1-7 = increasing "
	                   "speed, far right = MAX. Using the slider switches to Manual mode.");
	this->AddTip(8101, "Per-sensor temperatures. 'active' shows only sensors with a "
	                   "live reading; 'all' lists every EC sensor slot.");
	this->AddTip(8120, "Temperature history sparkline of the max sensor. Shows current, "
	                   "average and min-max for the window. Right-click to clear.");
	this->AddTip(7013, "Renames TVicHW64.sys and TVicPort64.sys to .sys.bak "
	                   "in System32\\drivers, hiding them from Valorant's Vanguard "
	                   "anti-cheat (ring 0 kernel access). Files are automatically "
	                   "restored when Game Mode is disabled or the app exits cleanly.");

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
				"TPFanControl v2.33 P15G2 Dual is started %d sec. after\nboot time (SecWinUptime=%d sec.)\n\nTo prevent missing systray icons\nand communication errors between\nTPFanControl v2.33 P15G2 Dual and embedded controller\nit will sleep for %d sec. (SecStartDelay)\n\nTo void this message box please set\nNoWaitMessage=1 in TPFanControl.ini",
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
				MessageBox(NULL, bufsec, "TPFanControl v2.33 P15G2 Dual is sleeping", MB_ICONEXCLAMATION);
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
			this->pTaskbarIcon = new TASKBARICON(this->hwndDialog, 10, "TPFanControl v2.33 P15G2 Dual");
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
	m_iconTimer = ::SetTimer(this->hwndDialog, 3, this->IconCycle * 1000, NULL); // Vista icon update
	if (this->ReIcCycle)
		m_renewTimer = ::SetTimer(this->hwndDialog, 4, this->ReIcCycle * 1000, NULL); // Vista icon update

	if (!this->StartMinimized)
		::ShowWindow(this->hwndDialog, TRUE);

	if (this->StartMinimized)
		::ShowWindow(this->hwndDialog, SW_MINIMIZE);
}

//-------------------------------------------------------------------------
//  register a balloon tooltip on one main-dialog control
//-------------------------------------------------------------------------
void
FANCONTROL::AddTip(int ctrlId, const char* text) {
	HWND hwndCtl = ::GetDlgItem(this->hwndDialog, ctrlId);
	if (!hwndCtl)
		return;

	// create the shared tip window on first use
	if (!this->m_hwndTip) {
		this->m_hwndTip = ::CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
			WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			this->hwndDialog, NULL, this->hinstapp, NULL);
		if (!this->m_hwndTip)
			return;
		::SendMessage(this->m_hwndTip, TTM_SETMAXTIPWIDTH, 0, 320);
		::SendMessage(this->m_hwndTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000); // keep up while reading
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
//  destructor
//-------------------------------------------------------------------------
FANCONTROL::~FANCONTROL() {
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
	UnregisterPowerSettingNotification(this->hPowerNotify);
	if (this->hwndDialog)
		::DestroyWindow(this->hwndDialog);

	if (this->m_hbrDlg) ::DeleteObject(this->m_hbrDlg);
	if (this->m_hbrField) ::DeleteObject(this->m_hbrField);
	if (this->m_hFontHdr) ::DeleteObject(this->m_hFontHdr);
	if (this->m_hFontBig) ::DeleteObject(this->m_hFontBig);
	if (this->m_hFontTitle) ::DeleteObject(this->m_hFontTitle);
	if (this->m_hFontDlg) ::DeleteObject(this->m_hFontDlg);

	if (pTextIconMutex)
		delete pTextIconMutex;
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
//  attach a balloon tooltip to one control on a modal dialog. The tip window
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
			WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
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
//  apply dark/light chrome (titlebar + child controls) to an arbitrary dialog
//-------------------------------------------------------------------------
static void ApplyDarkToDialog(HWND hwnd, BOOL dark) {
	if (!hwnd) return;

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
//  (re)build theme brushes, apply dark title bar, repaint
//-------------------------------------------------------------------------
void
FANCONTROL::ApplyTheme() {
	COLORREF dlgbg, fieldbg;

	if (this->DarkMode) {
		dlgbg = RGB(32, 32, 32);
		fieldbg = RGB(45, 45, 48);
		this->m_clrText = RGB(235, 235, 235);
	}
	else {
		dlgbg = RGB(243, 243, 243);
		fieldbg = RGB(255, 255, 255);
		this->m_clrText = RGB(32, 32, 32);
	}

	if (this->m_hbrDlg) ::DeleteObject(this->m_hbrDlg);
	if (this->m_hbrField) ::DeleteObject(this->m_hbrField);
	this->m_hbrDlg = ::CreateSolidBrush(dlgbg);
	this->m_hbrField = ::CreateSolidBrush(fieldbg);

	if (this->hwndDialog) {
		// dark title bar (Win10 1809+/Win11); load dynamically so we add no link dependency
		HMODULE hDwm = ::LoadLibraryA("dwmapi.dll");
		if (hDwm) {
			typedef HRESULT(WINAPI* PFNDWMSWA)(HWND, DWORD, LPCVOID, DWORD);
			PFNDWMSWA pSet = (PFNDWMSWA)::GetProcAddress(hDwm, "DwmSetWindowAttribute");
			if (pSet) {
				BOOL dark = this->DarkMode ? TRUE : FALSE;
				// 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (19 on older Win10 builds)
				if (FAILED(pSet(this->hwndDialog, 20, &dark, sizeof(dark))))
					pSet(this->hwndDialog, 19, &dark, sizeof(dark));
			}
			::FreeLibrary(hDwm);
		}

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
			if (pSPAM) pSPAM(this->DarkMode ? 2 : 3);    // 2=ForceDark, 3=ForceLight
			if (pADMW) pADMW(this->hwndDialog, this->DarkMode ? true : false);
			if (pFMT)  pFMT();

			THEMECTX ctx;
			ctx.pSet = (PFNSWTHEME)::GetProcAddress(hUx, "SetWindowTheme");
			ctx.dark = this->DarkMode ? TRUE : FALSE;
			::EnumChildWindows(this->hwndDialog, ThemeChildProc, (LPARAM)&ctx);
			::FreeLibrary(hUx);
		}

		// dark menu background (popup + bar); text follows system in classic Win32
		HMENU hMenu = ::GetMenu(this->hwndDialog);
		if (hMenu) {
			MENUINFO mi;
			::ZeroMemory(&mi, sizeof(mi));
			mi.cbSize = sizeof(mi);
			mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
			mi.hbrBack = this->DarkMode ? this->m_hbrDlg : NULL;
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
//  post-create chrome: temp-list columns, checkbox states, log visibility
//-------------------------------------------------------------------------
void
FANCONTROL::InitThemeAndChrome() {
	if (!this->hwndDialog) return;

	// --- modern font hierarchy ------------------------------------------------
	// Bold section headers (they replace the old group-box frames) and a larger
	// semibold font on the primary readouts (State / Fan-speed). Sized in points
	// against the window DPI so they stay crisp; recreated on a DPI change.
	{
		HDC hdc = ::GetDC(this->hwndDialog);
		int dpiY = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSY) : 96;
		if (hdc) ::ReleaseDC(this->hwndDialog, hdc);
		this->m_curDpi = (UINT)dpiY;   // PerMonitorV2 baseline for later WM_DPICHANGED
		if (this->m_hFontHdr)   { ::DeleteObject(this->m_hFontHdr);   this->m_hFontHdr = NULL; }
		if (this->m_hFontBig)   { ::DeleteObject(this->m_hFontBig);   this->m_hFontBig = NULL; }
		if (this->m_hFontTitle) { ::DeleteObject(this->m_hFontTitle); this->m_hFontTitle = NULL; }
		this->m_hFontHdr = ::CreateFontA(-::MulDiv(9, dpiY, 72), 0, 0, 0, FW_BOLD,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
		this->m_hFontBig = ::CreateFontA(-::MulDiv(10, dpiY, 72), 0, 0, 0, FW_SEMIBOLD,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
		this->m_hFontTitle = ::CreateFontA(-::MulDiv(12, dpiY, 72), 0, 0, 0, FW_SEMIBOLD,
			0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
		static const int hdrIds[] = { 9210, 9198, 9199, 9201, 9202 };  // section headers
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

	this->ReflowLayout();      // capture design geometry before any resize
	this->ApplyLogVisibility();

	// initial visibility of the temperature history graph
	{
		int sw = this->ShowGraph ? SW_SHOW : SW_HIDE;
		::ShowWindow(::GetDlgItem(this->hwndDialog, 9202), sw);
		::ShowWindow(::GetDlgItem(this->hwndDialog, 8120), sw);
	}

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
}

//-------------------------------------------------------------------------
//  hide or restore TVic driver files to avoid Riot Vanguard detection
//-------------------------------------------------------------------------
void
FANCONTROL::ToggleGameMode(bool silent) {
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
				"Could not hide TVic drivers (error %lu).\n\nTPFanControl v2.33 P15G2 Dual must run with administrator privileges for Game Mode.",
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
					"TVic drivers restored. TPFanControl v2.33 P15G2 Dual running normally.", 8000);
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
	struct { COLORREF color; char line[128]; } rows[12];
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
		if (valid) {
			if (temp >= this->IconLevels[2])      lineColor = RGB(232, 48, 48);
			else if (temp >= this->IconLevels[1]) lineColor = RGB(232, 120, 0);
			else if (temp >= this->IconLevels[0]) lineColor = RGB(220, 170, 0);
			else                                  lineColor = RGB(0, 170, 0);
		}

		char obuf2[64];
		if (valid) {
			if (this->Fahrenheit)
				sprintf_s(obuf2, sizeof(obuf2), "%d\xb0 F", temp * 9 / 5 + 32);
			else
				sprintf_s(obuf2, sizeof(obuf2), "%d\xb0 C", temp);
		} else {
			strcpy_s(obuf2, sizeof(obuf2), "n/a");
		}

		rows[nrows].color = lineColor;
		if (this->ShowTempHex)
			sprintf_s(rows[nrows].line, sizeof(rows[nrows].line), "%s\t%s\t(0x%02x)\r\n",
				this->State.SensorName[i], obuf2, this->State.SensorAddr[i]);
		else
			sprintf_s(rows[nrows].line, sizeof(rows[nrows].line), "%s\t%s\r\n",
				this->State.SensorName[i], obuf2);

		if (siglen >= 0 && siglen < (int)sizeof(sig)) {
			int n = sprintf_s(sig + siglen, sizeof(sig) - siglen, "%lu:%s",
				(unsigned long)lineColor, rows[nrows].line);
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
		cf.dwMask = CFM_COLOR | CFM_BOLD;   // clear bold so rows aren't bold like the header
		cf.dwEffects = 0;
		cf.crTextColor = rows[r].color;
		::SendMessage(hRich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

		::SendMessage(hRich, EM_REPLACESEL, FALSE, (LPARAM)rows[r].line);
	}

	::SendMessage(hRich, EM_SETSEL, 0, 0);
	::SendMessage(hRich, EM_SCROLLCARET, 0, 0);
	::SendMessage(hRich, WM_SETREDRAW, TRUE, 0);
	::InvalidateRect(hRich, NULL, TRUE);

	// ---- auto-size the list to its content (header + visible rows) and tuck
	//      the all/active radios just beneath it. Runs only when the rendered
	//      set actually changed (we're past the signature early-out), so this
	//      grows on 'all' and shrinks on 'active'. ReflowLayout() leaves these
	//      three controls alone so the two don't fight.
	{
		HDC dc = ::GetDC(hRich);
		HFONT hf  = (HFONT)::SendMessage(hRich, WM_GETFONT, 0, 0);
		HFONT old = hf ? (HFONT)::SelectObject(dc, hf) : NULL;
		TEXTMETRIC tm;
		::GetTextMetrics(dc, &tm);
		if (old) ::SelectObject(dc, old);
		::ReleaseDC(hRich, dc);

		int lineH = tm.tmHeight + tm.tmExternalLeading;
		int wantH = (nrows + 1) * lineH + 8;   // +1 for the header row, + padding

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
	static const struct { int id, ax, ay, aw, ah; } A[17] = {
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
		for (int i = 0; i < 17; i++) {
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

	HDWP hdwp = ::BeginDeferWindowPos(17);
	for (int i = 0; i < 17; i++) {
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
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
	this->m_hFontHdr = ::CreateFontA(-::MulDiv(9, newDpi, 72), 0, 0, 0, FW_BOLD,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
	this->m_hFontBig = ::CreateFontA(-::MulDiv(10, newDpi, 72), 0, 0, 0, FW_SEMIBOLD,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
	this->m_hFontTitle = ::CreateFontA(-::MulDiv(12, newDpi, 72), 0, 0, 0, FW_SEMIBOLD,
		0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
	if (this->m_hFontDlg)
		::EnumChildWindows(this->hwndDialog, SetFontChildProc, (LPARAM)this->m_hFontDlg);
	static const int hdrIds[] = { 9210, 9198, 9199, 9201, 9202 };
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
		::DrawTextA(hdc, "collecting...", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
		return;
	}

	// double-buffer: build the frame in a memory DC and blit it once, so the
	// owner-draw static never shows a half-painted graph (no flicker on resize).
	// carry over the control's font so the labels keep the dialog typeface.
	HFONT  hFont = (HFONT)::GetCurrentObject(hdc, OBJ_FONT);
	HDC    mdc   = ::CreateCompatibleDC(hdc);
	HBITMAP mbm  = ::CreateCompatibleBitmap(hdc, w, h);
	HGDIOBJ obm  = ::SelectObject(mdc, mbm);
	HGDIOBJ ofn  = ::SelectObject(mdc, hFont);

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
	COLORREF lineClr;
	if      (latest >= this->IconLevels[2]) lineClr = RGB(232, 48, 48);
	else if (latest >= this->IconLevels[1]) lineClr = RGB(232, 120, 0);
	else if (latest >= this->IconLevels[0]) lineClr = RGB(220, 170, 0);
	else                                    lineClr = RGB(0, 170, 0);

	const int top = 1, bot = h - 2;   // 1px padding top/bottom (local coords)
	const int ploth = bot - top;

	// faint gridlines at each IconLevel threshold that falls inside the range
	COLORREF gridClr = this->DarkMode ? RGB(70, 70, 74) : RGB(210, 210, 210);
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

	// the temperature trace
	HPEN linePen = ::CreatePen(PS_SOLID, traceW, lineClr);
	oldPen = (HPEN)::SelectObject(mdc, linePen);
	for (int i = 0; i < n; i++) {
		int v = this->m_tempHist[(head - n + i + TEMPHIST_MAX) % TEMPHIST_MAX];
		int x = (n == 1 ? 0 : (w - 1) * i / (n - 1));
		int y = bot - ploth * (v - lo) / (hi - lo);
		if (i == 0) ::MoveToEx(mdc, x, y, NULL);
		else        ::LineTo(mdc, x, y);
	}
	::SelectObject(mdc, oldPen);
	::DeleteObject(linePen);

	// markers: filled dot at the current sample, hollow ring at the window peak
	{
		const int r = __max(2, ::MulDiv(2, dpi, 96));
		auto plotX  = [&](int i){ return (n == 1) ? 0 : (w - 1) * i / (n - 1); };
		auto plotY  = [&](int v){ return bot - ploth * (v - lo) / (hi - lo); };
		auto clampx = [&](int x){ return x < r ? r : (x > w - 1 - r ? w - 1 - r : x); };

		// current sample (right edge): filled dot in the trace color
		int cx = clampx(plotX(n - 1)), cy = plotY(latest);
		HBRUSH fb = ::CreateSolidBrush(lineClr);
		HBRUSH ob = (HBRUSH)::SelectObject(mdc, fb);
		HPEN   fp = ::CreatePen(PS_SOLID, 1, lineClr);
		HPEN   op = (HPEN)::SelectObject(mdc, fp);
		::Ellipse(mdc, cx - r, cy - r, cx + r, cy + r);

		// window peak: hollow ring (only when it isn't the current sample)
		if (maxIdx != n - 1) {
			::SelectObject(mdc, ::GetStockObject(NULL_BRUSH));
			HPEN mp = ::CreatePen(PS_SOLID, 1, this->m_clrText);
			HPEN op2 = (HPEN)::SelectObject(mdc, mp);
			int mx = clampx(plotX(maxIdx)), my = plotY(maxVal);
			::Ellipse(mdc, mx - r, my - r, mx + r, my + r);
			::SelectObject(mdc, op2);
			::DeleteObject(mp);
		}

		::SelectObject(mdc, op);
		::DeleteObject(fp);
		::SelectObject(mdc, ob);
		::DeleteObject(fb);
	}

	// labels: current value (left), range (right), window average (center). Drop
	// the less-essential ones rather than let them overlap on a narrow window.
	char lbl[48], rng[48], avgl[48];
	if (this->Fahrenheit) {
		sprintf_s(lbl,  sizeof(lbl),  "%d\xb0 F", latest * 9 / 5 + 32);
		sprintf_s(rng,  sizeof(rng),  "%d-%d\xb0 F", lo * 9 / 5 + 32, hi * 9 / 5 + 32);
		sprintf_s(avgl, sizeof(avgl), "avg %d\xb0", avg * 9 / 5 + 32);
	} else {
		sprintf_s(lbl,  sizeof(lbl),  "%d\xb0 C", latest);
		sprintf_s(rng,  sizeof(rng),  "%d-%d\xb0 C", lo, hi);
		sprintf_s(avgl, sizeof(avgl), "avg %d\xb0", avg);
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

	::SelectObject(mdc, ofn);
	::SelectObject(mdc, obm);
	::DeleteObject(mbm);
	::DeleteDC(mdc);
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
	this->UpdateManualControlsEnabled();
}

//-------------------------------------------------------------------------
//  the manual fan-level box (8310) and slider (8311) only do anything in
//  Manual mode; grey them out otherwise (and whenever EC control is off)
//-------------------------------------------------------------------------
void
FANCONTROL::UpdateManualControlsEnabled() {
	BOOL manual = this->ActiveMode && (this->CurrentModeFromDialog() == 3);

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

	if (msg == WM_INITDIALOG)
	{
		s_TaskbarCreated = RegisterWindowMessage("TaskbarCreated");
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
			this->DrawSparkline(dis->hDC, dis->rcItem);
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
			::SetTextColor(dis->hDC, on ? RGB(0, 170, 0) : this->m_clrText);
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
					int answer = ::MessageBoxA(this->hwndDialog,
						"Setting the fan to maximum (64) runs it at full, "
						"unregulated speed.\r\n\r\n"
						"This is loud and bypasses normal speed regulation; "
						"the firmware keeps the fan at full power until you "
						"change the level. Use it only briefly to cool down a "
						"hot machine.\r\n\r\nSet fan to maximum?",
						"Maximum fan speed",
						MB_OKCANCEL | MB_ICONWARNING);
					if (answer != IDOK) {
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

		if (cid == 8100 || cid == 8103) {
			// state and switch temp: severity from IconLevels
			int t = this->MaxTemp;
			if (t >= this->IconLevels[2])      txt = RGB(232, 48, 48);
			else if (t >= this->IconLevels[1]) txt = RGB(232, 120, 0);
			else if (t >= this->IconLevels[0]) txt = RGB(220, 170, 0);
			else                               txt = RGB(0, 170, 0);
		}

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
				this->pTaskbarIcon->SetTooltip(this->TrayTip);
				strcpy_s(this->LastTooltip, sizeof(this->LastTooltip), this->TrayTip);
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
						this->pTaskbarIcon->SetBalloon(NIIF_INFO, "TPFanControl v2.33 P15G2 Dual symbol icon",
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

				case 7012: // Dark mode checkbox
					this->DarkMode = (::SendDlgItemMessage(this->hwndDialog, 7012, BM_GETCHECK, 0, 0) == BST_CHECKED);
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
					::ShowWindow(this->hwndDialog, TRUE);
					::SetForegroundWindow(this->hwndDialog);
					break;

				case 5070: // switch to classic colored symbol icon
					this->ShowTempIcon = 0;
					if (!this->pTaskbarIcon) {   // guard: don't leak an existing icon
						this->pTaskbarIcon = new TASKBARICON(this->hwndDialog, 10, "TPFanControl v2.33 P15G2 Dual");
						this->pTaskbarIcon->SetIcon(this->CurrentIcon);
					}
					break;

				case 5080: // show temp icon
					delete this->pTaskbarIcon;
					this->pTaskbarIcon = NULL;
					this->ShowTempIcon = 1;
					break;

				case 5030: // hide window
					::ShowWindow(this->hwndDialog, SW_MINIMIZE);
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
					if (!this->ActiveMode || this->SetFan("On close", 0x80, true)) {
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
					ok = this->SetFan("Lid close, Switch to BIOS Mode", 0x80);
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
		break;


	case WM_CLOSE:
		//if (this->MinimizeOnClose && (this->MinimizeToSysTray || this->Runs_as_service))   // 0.24 new:  || this->Runs_as_service)
		//{MessageBox(NULL, "will Fenster schlie�en", "TPFanControl", MB_ICONEXCLAMATION);
		::ShowWindow(this->hwndDialog, SW_MINIMIZE);   //}
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
				this->SetFan("On shutdown", 0x80, true);

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

	case WM_DPICHANGED:
		// PerMonitorV2: lo-word of wParam = new DPI, lParam = suggested window rect
		this->RescaleForDpi(LOWORD(mp1), (const RECT*)mp2);
		rc = TRUE;
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

			// after so many consecutive read errors, try to switch back to bios mode
			if (this->ReadErrorCount > this->MaxReadErrors) {
				this->ModeToDialog(1);
				ok = this->SetFan("Max. Errors", 0x80);
				if (ok) {
					this->Trace("Set to BIOS Mode, to many consecutive read errors");
					::Sleep(2000);
					::SendMessage(this->hwndDialog, WM_ENDSESSION, 0, 0);
				}
			}
		}
		break;

	case WM__TASKBAR:

		switch (mp2) {

		case WM_LBUTTONDOWN:

			if (!IsWindowVisible(this->hwndDialog)) {
				::ShowWindow(this->hwndDialog, TRUE);
				::SetForegroundWindow(this->hwndDialog);
			}
			else    
				::ShowWindow(this->hwndDialog, SW_MINIMIZE);
			break;

		case WM_LBUTTONUP:
		{
			BOOL
				isshift = ::GetAsyncKeyState(VK_SHIFT) & 0x8000,
				isctrl = ::GetAsyncKeyState(VK_CONTROL) & 0x8000;

			int action = -1;

			// some fancy key dependent stuff could be done here.

		}
		break;

		case WM_LBUTTONDBLCLK:

			if (!IsWindowVisible(this->hwndDialog)) {
				::ShowWindow(this->hwndDialog, TRUE);
				::SetForegroundWindow(this->hwndDialog);
			}
			else    
				::ShowWindow(this->hwndDialog, SW_MINIMIZE);
			break;

		case WM_RBUTTONDOWN:
		{
			unsigned char testpara;
			MENU m(5000);

			if (!this->LockECAccess()) break;

			ok = this->ReadByteFromEC(59, &testpara);
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

			// bold the item a double-click would trigger (the surviving Show/Hide)
			m.SetDefaultItem(IsWindowVisible(this->hwndDialog) ? 5030 : 5010);

			this->FreeECAccess();

			m.Popup(this->hwndDialog);
		}
		break;
		}
		rc = TRUE;
		break;

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

		::CheckDlgButton(hwnd, 9301, self->StartMinimized ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9302, self->StayOnTop      ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9303, self->ShowTempIcon   ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9304, self->ShowTempHex    ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9305, self->ShowLog        ? BST_CHECKED : BST_UNCHECKED);
		::CheckDlgButton(hwnd, 9306, self->DarkMode       ? BST_CHECKED : BST_UNCHECKED);
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
			::SetDlgItemTextA(hwnd, 9319, self->Fahrenheit ? "(\xb0" "F)" : "(\xb0" "C)");
		}

		// thermal fail-safe threshold (stored Celsius; shown in the display unit)
		{
			int fs = self->FailsafeTemp;
			if (self->Fahrenheit && fs > 0) fs = fs * 9 / 5 + 32;
			::SetDlgItemInt(hwnd, 9325, fs, FALSE);
			::SetDlgItemTextA(hwnd, 9326, self->Fahrenheit ? "\xb0" "F" : "\xb0" "C");
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
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9314,
				"Show the temperature history sparkline along the bottom of the main "
				"window.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9301,
				"Start hidden in the tray instead of opening the window.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9302,
				"Keep the main window above other windows.");
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
				"Smart or Manual mode), the fan is forced to level 7 until it cools "
				"~3 degrees below. 0 disables it. Independent of the fan curve.");
			tip = AddDialogTip(hwnd, tip, self->hinstapp, 9330,
				"Open the Smart fan-curve editor (temperature -> fan level table).");
		}
		return TRUE;

	case WM_CTLCOLORDLG:
		if (self) return (INT_PTR)self->m_hbrDlg;
		break;

	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if (self) {
			// color the three threshold labels with their severity color so the
			// warm/hot/critical mapping reads at a glance (same hues as the icon)
			COLORREF tc = self->m_clrText;
			switch (::GetDlgCtrlID((HWND)lp)) {
			case 9315: tc = RGB(220, 170, 0); break;   // warm
			case 9316: tc = RGB(232, 120, 0); break;   // hot
			case 9317: tc = RGB(232, 48, 48); break;   // critical
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

	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			if (self) self->ApplySettingsFromDialog(hwnd);
			::EndDialog(hwnd, IDOK);
			return TRUE;

		case 9320: // Apply: persist + live-apply but keep the dialog open
			if (self) {
				self->ApplySettingsFromDialog(hwnd);
				// dark-mode may have just changed: re-theme this dialog too
				ApplyDarkToDialog(hwnd, self->DarkMode);
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
void
FANCONTROL::ApplySettingsFromDialog(HWND hwnd)
{
	int oldDark  = this->DarkMode;
	int oldLog   = this->ShowLog;
	int oldTop   = this->StayOnTop;
	int oldIcon  = this->ShowTempIcon;
	int oldCycle = this->Cycle;

	this->StartMinimized = (::IsDlgButtonChecked(hwnd, 9301) == BST_CHECKED);
	this->StayOnTop      = (::IsDlgButtonChecked(hwnd, 9302) == BST_CHECKED);
	this->ShowTempIcon   = (::IsDlgButtonChecked(hwnd, 9303) == BST_CHECKED);
	this->ShowTempHex    = (::IsDlgButtonChecked(hwnd, 9304) == BST_CHECKED);
	this->ShowLog        = (::IsDlgButtonChecked(hwnd, 9305) == BST_CHECKED);
	this->DarkMode       = (::IsDlgButtonChecked(hwnd, 9306) == BST_CHECKED);
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
		}
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

	// show/hide the temperature history graph
	{
		int sw = this->ShowGraph ? SW_SHOW : SW_HIDE;
		::ShowWindow(::GetDlgItem(main, 9202), sw);
		::ShowWindow(::GetDlgItem(main, 8120), sw);
	}

	// refresh temps/list so the hex column and icon repaint immediately
	::PostMessage(main, WM__GETDATA, 0, 0);
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

		// load both profiles into working buffers, show profile 1
		self->CurveLoadProfileToBuf(0);
		self->CurveLoadProfileToBuf(1);
		self->m_ceProfile = 0;
		::CheckRadioButton(hwnd, 9401, 9402, 9401);
		::SetDlgItemTextA(hwnd, 9461, self->Fahrenheit ? "(\xb0" "F)" : "(\xb0" "C)");
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
		}
		return TRUE;

	case WM_CTLCOLORDLG:
		if (self) return (INT_PTR)self->m_hbrDlg;
		break;
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if (self) {
			::SetTextColor((HDC)wp, self->m_clrText);
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
				if (Fahrenheit) {
					ppTbTextIcon[0]->DiShowballon(
						_T("shows max. temperature in \xb0 F and sensor name, left click on icon shows or hides control window, right click shows menue"),
						_T("TPFanControl v2.33 P15G2 Dual text icon"), NIIF_INFO, 11);
				}
				else {
					ppTbTextIcon[0]->DiShowballon(
						_T("shows max. temperature in \xb0 C and sensor name, left click on icon shows or hides control window, right click shows menue"),
						_T("TPFanControl v2.33 P15G2 Dual text icon"), NIIF_INFO, 11);
				}

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
