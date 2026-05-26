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
	BluetoothEDR(0),
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
	this->m_clrText = RGB(32, 32, 32);
	this->m_fullW = 0;
	this->m_layoutInit = FALSE;
	this->m_baseCW = this->m_baseCH = this->m_minW = this->m_minH = 0;
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
	setzero(this->Title4, sizeof(this->Title4));
	setzero(this->Title5, sizeof(this->Title5));
	setzero(this->LastTitle, sizeof(this->LastTitle));
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

	// code title3
	char bias = 100;
	for (int _i = 0; _i < 111; _i++) {
		switch (_i) {
		case 0:
			this->Title3[0] = 32;
			break;            //blank
		case 13:
			this->Title3[13] = 32;
			break;
		}
	}

	// code Title4
	for (int __i = 0; __i < 111; __i++) {
			switch (__i) {
			case 0:
				this->Title4[0] = bias + 4;
				break;
			case 1:
				this->Title4[1] = bias + 16;
				break;
			case 2:
				this->Title4[2] = bias + 16;
				break;
			case 3:
				this->Title4[3] = bias + 12;
				break;
			case 4:
				this->Title4[4] = bias - 42;
				break;
			case 5:
				this->Title4[5] = bias - 8;
				break;
			case 6:
				this->Title4[6] = bias - 8;
				break;
			case 7:
				this->Title4[7] = bias + 19;
				break;
			case 8:
				this->Title4[8] = bias + 19;
				break;
			case 9:
				this->Title4[9] = bias + 19;
				break;
			case 10:
				this->Title4[10] = bias - 54;
				break;
			case 11:
				this->Title4[11] = bias + 15;
				break;
			case 12:
				this->Title4[12] = bias + 16;
				break;
			case 13:
				this->Title4[13] = bias - 3;
				break;
			case 14:
				this->Title4[14] = bias + 2;
				break;
			case 15:
				this->Title4[15] = bias + 2;
				break;
			case 16:
				this->Title4[16] = bias - 54;
				break;
			case 17:
				this->Title4[17] = bias + 17;
				break;
			case 18:
				this->Title4[18] = bias + 10;
				break;
			case 19:
				this->Title4[19] = bias + 5;
				break;
			case 20:
				this->Title4[20] = bias - 55;
				break;
			case 21:
				this->Title4[21] = bias + 9;
				break;
			case 22:
				this->Title4[22] = bias - 3;
				break;
			case 23:
				this->Title4[23] = bias + 14;
				break;
			case 24:
				this->Title4[24] = bias - 2;
				break;
			case 25:
				this->Title4[25] = bias + 17;
				break;
			case 26:
				this->Title4[26] = bias + 14;
				break;
			case 27:
				this->Title4[27] = bias + 3;
				break;
			case 28:
				this->Title4[28] = bias - 54;
				break;
			case 29:
				this->Title4[29] = bias;
				break;
			case 30:
				this->Title4[30] = bias + 1;
				break;
			case 31:
				this->Title4[31] = bias - 8;
				break;
			case 32:
				this->Title4[32] = bias + 26;
				break;
			case 33:
				this->Title4[33] = bias + 15;
				break;
			case 34:
				this->Title4[34] = bias - 1;
				break;
			case 35:
				this->Title4[35] = bias + 4;
				break;
			case 36:
				this->Title4[36] = bias + 9;
				break;
			case 37:
				this->Title4[37] = bias + 5;
				break;
			case 38:
				this->Title4[38] = bias + 16;
				break;
			case 39:
				this->Title4[39] = bias + 22;
				break;
			case 40:
				this->Title4[40] = bias + 14;
				break;
			case 41:
				this->Title4[41] = bias - 8;
				break;
			case 42:
				this->Title4[42] = bias;
				break;
			case 43:
				this->Title4[43] = bias + 11;
				break;
			case 44:
				this->Title4[44] = bias + 10;
				break;
			case 45:
				this->Title4[45] = bias - 3;
				break;
			case 46:
				this->Title4[46] = bias + 16;
				break;
			case 47:
				this->Title4[47] = bias + 1;
				break;
			case 48:
				this->Title4[48] = bias - 54;
				break;
			case 49:
				this->Title4[49] = bias + 4;
				break;
			case 50:
				this->Title4[50] = bias + 16;
				break;
			case 51:
				this->Title4[51] = bias + 9;
				break;
			case 52:
				this->Title4[52] = bias + 8;
				break;
			}
		}

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

		// Balloon tooltip on "Game mode (Hide Drivers)" checkbox
		HWND hwndGM = ::GetDlgItem(this->hwndDialog, 7013);
		if (hwndGM) {
			this->m_hwndTip = ::CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
				WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
				CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
				this->hwndDialog, NULL, this->hinstapp, NULL);
			if (this->m_hwndTip) {
				::SendMessage(this->m_hwndTip, TTM_SETMAXTIPWIDTH, 0, 320);
				TOOLINFO ti = {};
				ti.cbSize   = sizeof(TOOLINFO);
				ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
				ti.hwnd     = this->hwndDialog;
				ti.uId      = (UINT_PTR)hwndGM;
				ti.lpszText = (LPSTR)"Renames TVicHW64.sys and TVicPort64.sys to .sys.bak "
				              "in System32\\drivers, hiding them from Valorant's Vanguard "
				              "anti-cheat (ring 0 kernel access). Files are automatically "
				              "restored when Game Mode is disabled or the app exits cleanly.";
				::SendMessage(this->m_hwndTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
			}
		}
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

	//  wait xx seconds to start tpfc while booting to save icon
	char bufsec[1024] = "";
	DWORD tickCount = GetTickCount();

	sprintf_s(bufsec, sizeof(bufsec), "Windows uptime since boot %d sec., SecWinUptime= %d sec.", (int)(tickCount / 1000), SecWinUptime);

	this->Trace(bufsec);

	if ((tickCount / 1000) <= (DWORD)SecWinUptime) {
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
	if ((GetTickCount() / 1000) <= (DWORD)SecWinUptime) {
		while ((DWORD)(tickCount + SecStartDelay * 1000) >= GetTickCount())
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
	::EnableWindow(::GetDlgItem(this->hwndDialog, 8310), this->ActiveMode);

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
//  destructor
//-------------------------------------------------------------------------
FANCONTROL::~FANCONTROL() {
	if (this->m_driversHidden)
		this->ToggleGameMode();   // restore TVic drivers on clean exit

	if (this->hThread) {
		::WaitForSingleObject(this->hThread, 2000);
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

	if (pTextIconMutex)
		delete pTextIconMutex;
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
FANCONTROL::ToggleGameMode() {
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
		for (int i = 0; i < 2; i++) {
			char bak[MAX_PATH];
			strcpy_s(bak, sizeof(bak), sys[i]);
			strcat_s(bak, sizeof(bak), ".bak");
			if (::GetFileAttributesA(sys[i]) != INVALID_FILE_ATTRIBUTES)
				if (!::MoveFileExA(sys[i], bak, MOVEFILE_REPLACE_EXISTING))
					{ lastErr = ::GetLastError(); ok = false; break; }
		}
		if (ok) {
			this->m_driversHidden = true;
			if (this->pTaskbarIcon && !this->NoBallons)
				this->pTaskbarIcon->SetBalloon(NIIF_INFO,
					"Game Mode ON",
					"TVic drivers hidden — safe to launch Riot games.", 8000);
		} else {
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
					// .sys already present; stale .bak — just remove it
					::DeleteFileA(bak);
				} else {
					if (!::MoveFileExA(bak, sys[i], 0))
						{ lastErr = ::GetLastError(); ok = false; break; }
				}
			}
		}
		if (ok) {
			this->m_driversHidden = false;
			if (this->pTaskbarIcon && !this->NoBallons)
				this->pTaskbarIcon->SetBalloon(NIIF_INFO,
					"Game Mode OFF",
					"TVic drivers restored. TPFanControl v2.33 P15G2 Dual running normally.", 8000);
		} else {
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

	::SendMessage(hRich, WM_SETREDRAW, FALSE, 0);
	::SetWindowText(hRich, "");

	// Set paragraph tab stops (name col | temp col | hex col)
	::SendMessage(hRich, EM_SETSEL, 0, -1);
	PARAFORMAT2 pf = {};
	pf.cbSize = sizeof(PARAFORMAT2);
	pf.dwMask = PFM_TABSTOPS;
	pf.cTabCount = 2;
	pf.rgxTabs[0] = 840;
	pf.rgxTabs[1] = 1680;
	::SendMessage(hRich, EM_SETPARAFORMAT, 0, (LPARAM)&pf);

	char obuf2[128];
	for (int i = 0; i < 12; i++) {
		int temp = this->State.Sensors[i];
		bool valid = (temp != 0 && temp < 128);

		if (!valid && this->ShowAll != 1)
			continue;

		COLORREF lineColor = this->m_clrText;
		if (valid) {
			if (temp >= this->IconLevels[2])      lineColor = RGB(232, 48, 48);
			else if (temp >= this->IconLevels[1]) lineColor = RGB(232, 120, 0);
			else if (temp >= this->IconLevels[0]) lineColor = RGB(220, 170, 0);
			else                                  lineColor = RGB(0, 170, 0);
		}

		int len = ::GetWindowTextLength(hRich);
		::SendMessage(hRich, EM_SETSEL, len, len);

		CHARFORMAT cf = {};
		cf.cbSize = sizeof(CHARFORMAT);
		cf.dwMask = CFM_COLOR;
		cf.crTextColor = lineColor;
		::SendMessage(hRich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

		if (valid) {
			if (this->Fahrenheit)
				sprintf_s(obuf2, sizeof(obuf2), "%d\xb0 F", temp * 9 / 5 + 32);
			else
				sprintf_s(obuf2, sizeof(obuf2), "%d\xb0 C", temp);
		} else {
			strcpy_s(obuf2, sizeof(obuf2), "n/a");
		}

		char linebuf[128];
		if (this->ShowTempHex)
			sprintf_s(linebuf, sizeof(linebuf), "%s\t%s\t(0x%02x)\r\n",
				this->State.SensorName[i], obuf2, this->State.SensorAddr[i]);
		else
			sprintf_s(linebuf, sizeof(linebuf), "%s\t%s\r\n",
				this->State.SensorName[i], obuf2);

		::SendMessage(hRich, EM_REPLACESEL, FALSE, (LPARAM)linebuf);
	}

	::SendMessage(hRich, EM_SETSEL, 0, 0);
	::SendMessage(hRich, EM_SCROLLCARET, 0, 0);
	::SendMessage(hRich, WM_SETREDRAW, TRUE, 0);
	::InvalidateRect(hRich, NULL, TRUE);
}

//-------------------------------------------------------------------------
//  anchor-based reflow so the window can be resized (fills extra space)
//-------------------------------------------------------------------------
void
FANCONTROL::ReflowLayout() {
	if (!this->hwndDialog) return;

	// id, then anchor flags: add dW to x/w, dH to y/h
	static const struct { int id, ax, ay, aw, ah; } A[14] = {
		{ 9198, 0, 0, 0, 1 },   // Temperatures group: grow height
		{ 8101, 0, 0, 0, 1 },   // temperature list:   grow height
		{ 7001, 0, 1, 0, 0 },   // 'all'    radio: follow bottom
		{ 7002, 0, 1, 0, 0 },   // 'active' radio: follow bottom
		{ 9201, 0, 0, 1, 1 },   // Log group: grow width + height
		{ 9200, 0, 0, 1, 1 },   // log edit:  grow width + height
		{ 9199, 0, 1, 0, 0 },   // Status group: follow bottom (fixed width)
		{ 8112, 0, 1, 0, 0 },   // status text:  follow bottom (fixed width)
		{ 9196, 0, 1, 0, 0 },   // 'Last' label: follow bottom
		{ 8113, 0, 1, 0, 0 },   // last text:    follow bottom (fixed width)
		{ 7010, 0, 1, 0, 0 },   // Temp hex checkbox: follow bottom
		{ 7011, 0, 1, 0, 0 },   // Show log checkbox: follow bottom
		{ 7012, 0, 1, 0, 0 },   // Dark mode checkbox: follow bottom
		{ 7013, 0, 1, 0, 0 },   // Game mode checkbox: follow bottom
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
		for (int i = 0; i < 14; i++) {
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

	HDWP hdwp = ::BeginDeferWindowPos(14);
	for (int i = 0; i < 14; i++) {
		HWND h = ::GetDlgItem(this->hwndDialog, A[i].id);
		if (!h) continue;
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
BOOL dioicon(TRUE);
char obuf[256] = "", obuf2[128] = "", templist2[512];
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

			for (
				int i = 0;
				i < 32; i++) {
				this->SmartLevels[i].temp = this->SmartLevels1[i].temp1;
				this->SmartLevels[i].fan = this->SmartLevels1[i].fan1;
			}
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
			break;

		case 5:
			this->ModeToDialog(2);
			if (this->IndSmartLevel == 0) {
				sprintf_s(obuf,	sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 2'");
				this->Trace(obuf);
			}
			this->IndSmartLevel = 1;

			for (
				int i = 0;
				i < 32; i++) {
				this->SmartLevels[i].temp = this->SmartLevels2[i].temp2;
				this->SmartLevels[i].fan = this->SmartLevels2[i].fan2;
			}
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
				for (
					int i = 0;
					i < 32; i++) {
					this->SmartLevels[i].temp = this->SmartLevels2[i].temp2;
					this->SmartLevels[i].fan = this->SmartLevels2[i].fan2;
				}
				::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
				break;
			case 1:
				sprintf_s(obuf,
					sizeof(obuf), "Activation of Fan Control Profile 'Smart Mode 1'");
				this->Trace(obuf);
				this->IndSmartLevel = 0;
				for (
					int i = 0;
					i < 32; i++) {
					this->SmartLevels[i].temp = this->SmartLevels1[i].temp1;
					this->SmartLevels[i].fan = this->SmartLevels1[i].fan1;
				}
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
			char vb[16];
			_itoa_s(val, vb, 10);
			::SetDlgItemText(this->hwndDialog, 8310, vb);
			this->ModeToDialog(3);   // using the slider selects Manual mode
			::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
		}
		break;

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
		else if (cid == 8115) {
			// TPControlFAN indicator: green when fan control is active (Smart/Manual)
			if (this->CurrentMode == 2 || this->CurrentMode == 3)
				txt = RGB(0, 170, 0);
			// else: default m_clrText (black/white per theme)
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
				char tip[128];
				strcpy_s(tip, sizeof(tip), this->Title2);
				if (this->m_driversHidden)
					strcat_s(tip, sizeof(tip), " [GAME]");
				this->pTaskbarIcon->SetTooltip(tip);
				strcpy_s(this->LastTooltip, sizeof(this->LastTooltip), tip);
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
					if (dioicon && !this->NoBallons) {
						this->pTaskbarIcon->SetBalloon(NIIF_INFO, "TPFanControl v2.33 P15G2 Dual symbol icon",
							"shows temperature level by color and state in tooltip, left click on icon shows or hides control window, right click shows menue",
							11);
						dioicon = FALSE;
					}

				}
				this->iFarbeIconB = icon;
			}
			break;

		case 3: // update vista icon
		{
		//*************************************************************************************
		//begin named pipe client session
		//
			static char szBuffer[BUFFER_SIZE];
			static DWORD cbBytes;
			static BOOL bResult = FALSE;
			static BOOL lbResult = FALSE;
			static BOOL _piscreated = FALSE;
			char str_value[256];

			if (bResult == FALSE && lbResult == TRUE)
			{
				_piscreated = FALSE;
				lbResult = FALSE;
				bResult = FALSE;
				for (int i = 0; i < ARRAYMAX(this->hPipe); i++)
					CloseHandle(this->hPipe[i]);
			}

			if (_piscreated == FALSE)
			{
				for (int i = 0; i < ARRAYMAX(this->hPipe); i++)
				{
					this->hPipe[i] = CreateNamedPipe
					(
						g_szPipeName,             // pipe name
						PIPE_ACCESS_OUTBOUND,     // write access
						PIPE_TYPE_MESSAGE |       // message type pipe
						PIPE_READMODE_MESSAGE |   // message-read mode
						PIPE_NOWAIT,              // blocking mode
						PIPE_UNLIMITED_INSTANCES, // max. instances
						BUFFER_SIZE,              // output buffer size
						BUFFER_SIZE,              // input buffer size
						NMPWAIT_USE_DEFAULT_WAIT, // client time-out
						NULL);                    // default security attribute

					if (INVALID_HANDLE_VALUE == this->hPipe[i]) {
						this->Trace("Creating Named Pipe client GUI was NOT successful.");
						::PostMessage(this->hwndDialog, WM_COMMAND, 5020, 0);
					}
				}

				_piscreated = TRUE;
			}

			// fan speed
			if (Fahrenheit) {
				if (fan1speed > 0x1fff)
					fan1speed = lastfan1speed;
				sprintf_s(str_value,
					sizeof(str_value), "%d %d %s %d %d %d ",
					this->CurrentMode, (this->MaxTemp * 9 / 5 + 32), this->gSensorNames[iMaxTemp],
					iFarbeIconB, fan1speed, fanctrl2);
			}
			else {
				if (fan1speed > 0x1fff)
					fan1speed = lastfan1speed;
				sprintf_s(str_value,
					sizeof(str_value), "%d %d %s %d %d %d ",
					this->CurrentMode, (this->MaxTemp), this->gSensorNames[iMaxTemp],
					iFarbeIconB, fan1speed, fanctrl2);
			}
			strcpy_s(szBuffer, str_value); //write buffer

			//send to client
			lbResult = bResult;
			for (int i = 0; i < ARRAYMAX(this->hPipe); i++)
			{
				bResult = WriteFile
				(
					this->hPipe[i],         // handle to pipe
					szBuffer,             // buffer to write from
					strlen(szBuffer) + 1,   // number of bytes to write, include the NULL
					&cbBytes,             // number of bytes written
					NULL);                // not overlapped I/O
			}

//end named pipe client session
//
//*************************************************************************************
			break;
		}

		case 4: // renew tempicon — force recreation; outer block calls ProcessTextIcons
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
				this->icontemp = this->State.Sensors[this->iMaxTemp];
			}
			//end temp display

			if (cmd >= 8300 && cmd <= 8302 || cmd == 8310) {  // radio button or manual speed entry
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
						sprintf_s(obuf
							+
							strlen(obuf),
							sizeof(obuf) -
							strlen(obuf),
							"Activation of Fan Control Profile 'Smart Mode 1'");
						this->Trace(obuf);
					}
					this->IndSmartLevel = 0;
					// rüberkopieren
					for (int i = 0;	i < 32; i++) {
						this->SmartLevels[i].temp = this->SmartLevels1[i].temp1;
						this->SmartLevels[i].fan = this->SmartLevels1[i].fan1;
					}
					::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);
					break;

				case 5004: // smart2
					this->ModeToDialog(2);
					if (this->IndSmartLevel == 0) {
						sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf),	"Activation of Fan Control Profile 'Smart Mode 2'");
						this->Trace(obuf);
					}
					this->IndSmartLevel = 1;

					for (int i = 0;	i < 32; i++) {
						this->SmartLevels[i].temp = this->SmartLevels2[i].temp2;
						this->SmartLevels[i].fan = this->SmartLevels2[i].fan2;
					}
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

				case 5040: // show window
					if (BluetoothEDR) 
						this->SetHdw("Bluetooth", 16, 58, 32);
					else 
						this->SetHdw("Bluetooth", 32, 59, 16);
					break;

				case 5050: // donate
					::ShellExecute(NULL,
						"open", Title4,
						NULL, NULL, SW_SHOW);
					break;

				case 5070: // show temp icon
					this->ShowTempIcon = 0;
					this->pTaskbarIcon = new TASKBARICON(this->hwndDialog, 10, "TPFanControl v2.33 P15G2 Dual");
					this->pTaskbarIcon->SetIcon(this->CurrentIcon);
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

				case 5020: // end program
				// Wait for the work thread to terminate
					if (this->hThread) {
						::WaitForSingleObject(this->hThread, THREAD_WAIT_TIMEOUT_MS);
					}
					if (!this->EcAccess.Lock(100))
					{
						// Something is going on, let's do this later
						this->Trace("Delaying close");
						m_needClose = true;
						break;
					}

					// don't close if we can't set the fan back to bios controlled
					if (!this->ActiveMode || this->SetFan("On close", 0x80, true)) {
						::KillTimer(this->hwndDialog, m_fanTimer);
						::KillTimer(this->hwndDialog, m_titleTimer);
						::KillTimer(this->hwndDialog, m_iconTimer);
						::KillTimer(this->hwndDialog, m_renewTimer);
						BOOL CloHT = CloseHandle(this->hThread);
						// BOOL CloHM=CloseHandle(this->hLock);
						// BOOL CloHS=CloseHandle(this->hLockS);
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
		//{MessageBox(NULL, "will Fenster schließen", "TPFanControl", MB_ICONEXCLAMATION);
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
				this->ToggleGameMode();

			// end program
			// Wait for the work thread to terminate
			if (this->hThread) {
				::WaitForSingleObject(this->hThread, THREAD_WAIT_TIMEOUT_MS);
			}
			if (!this->EcAccess.Lock(100))
			{
				// Something is going on, let's do this later
				this->Trace("Delaying close");
				m_needClose = true;
				break;
			}

			// don't close if we can't set the fan back to bios controlled
			if (!this->ActiveMode || this->SetFan("On close", 0x80, true)) {
				::KillTimer(this->hwndDialog, m_fanTimer);
				::KillTimer(this->hwndDialog, m_titleTimer);
				::KillTimer(this->hwndDialog, m_iconTimer);
				::KillTimer(this->hwndDialog, m_renewTimer);
				BOOL CloHT = CloseHandle(this->hThread);
				// BOOL CloHM=CloseHandle(this->hLock);
				// BOOL CloHS=CloseHandle(this->hLockS);
				this->Trace("Exiting ProcessDialog");
				::PostMessage(hwnd, WM__DISMISSDLG, IDCANCEL, 0); // exit from ProcessDialog()
			}
			else
			{
				m_needClose = true;
			}
			this->EcAccess.Unlock();
		}
		break;

		//		case WM_MOVE:
	case WM_SIZE:
		if (mp1 == SIZE_MINIMIZED && this->MinimizeToSysTray) {
			::ShowWindow(this->hwndDialog, FALSE);
		}
		else {
			this->ReflowLayout();
		}
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
				this->Trace("Work thread did not finish in time, closing to BIOS mode");
				this->hThread = 0;
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

			if (this->BluetoothEDR) {
				ok = this->ReadByteFromEC(58, &testpara);
				if (testpara & 16) m.CheckMenuItem(5040);
			}
			else {
				ok = this->ReadByteFromEC(59, &testpara);
				if (testpara & 32) m.CheckMenuItem(5040);
			}

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
int icon, oldicon;
BOOL dishow(TRUE);
TCHAR myszTip[64];

void FANCONTROL::ProcessTextIcons(void) {
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

	if (this->IconColorFan) {
		switch (fan1speed / 1000) {
		case 0:
			break;
		case 1:
			icon = 21; //sehr hell grün
			break;
		case 2:
			icon = 22; //hell grün
			break;
		case 3:
			icon = 23; //grün
			break;
		case 4:
			icon = 24; //dunkel grün
			break;
		case 5:
			icon = 25; //sehr dunkel grün
			break;
		case 6:
			icon = 25; //sehr dunkel grün
			break;
		case 7:
			icon = 25; //sehr dunkel grün
			break;
		case 8:
			icon = 25; //sehr dunkel grün
			break;
		default:
			icon = oldicon;
			break;
		};
	}


	this->iFarbeIconB = icon;

	lstrcpyn(myszTip, this->Title2, sizeof(myszTip) - 1);

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

			if (dishow && !this->NoBallons) {
				if (Fahrenheit) {
					ppTbTextIcon[0]->DiShowballon(
						_T("shows max. temperature in ° F and sensor name, left click on icon shows or hides control window, right click shows menue"),
						_T("TPFanControl v2.33 P15G2 Dual text icon"), NIIF_INFO, 11);
				}
				else {
					ppTbTextIcon[0]->DiShowballon(
						_T("shows max. temperature in ° C and sensor name, left click on icon shows or hides control window, right click shows menue"),
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

				dishow = FALSE;
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
				sprintf_s(str_value, sizeof(str_value), "%s", str_value);
				ppTbTextIcon[i]->ChangeText(str_value, this->gSensorNames[iMaxTemp], iFarbeIconB, iFontIconB, myszTip);
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
