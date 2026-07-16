
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
#include "tools.h"
#include "fancontrol.h"


//-------------------------------------------------------------------------
//  single home for temperature formatting, Win11 style "47\xb0C" (no spaces).
//  Values must already be in the display unit - several callers (e.g. the
//  Smart-curve dump) hold Fahrenheit numbers before the global conversion.
//-------------------------------------------------------------------------
void FmtTemp(char* out, size_t n, int dispTemp, int fahrenheitUnit) {
	sprintf_s(out, n, "%d\xb0%s", dispTemp, fahrenheitUnit ? "F" : "C");
}

const char* TempUnit(int fahrenheitUnit) {
	return fahrenheitUnit ? "\xb0" "F" : "\xb0" "C";
}

//-------------------------------------------------------------------------
//  write selected options back to the ini, preserving comments and order
//-------------------------------------------------------------------------
void
FANCONTROL::SaveConfig(const char* configfile)
{
	// FailsafeTemp/CriticalTemp are stored Celsius internally; write them in the user's
	// display unit so a Fahrenheit ini round-trips (mirrors IconLevels below). Guard >0
	// so 0 = off stays 0.
	int fsT = this->FailsafeTemp, crT = this->CriticalTemp;
	if (this->Fahrenheit) {
		if (fsT > 0) fsT = fsT * 9 / 5 + 32;
		if (crT > 0) crT = crT * 9 / 5 + 32;
	}

	struct KV { const char* key; int val; };
	KV items[] = {
		{ "StartMinimized", this->StartMinimized },
		{ "StayOnTop",      this->StayOnTop },
		{ "ShowInTaskbar",  this->ShowInTaskbar },
		{ "SuspendMode",    this->SuspendMode },   // 0=ignore, 1=BIOS+restore, 2=keep
		{ "ShowTempIcon",   this->ShowTempIcon },
		{ "ShowTempHex",    this->ShowTempHex },
		{ "ShowLog",        this->ShowLog },
		{ "DarkMode",       this->DarkModeSetting },   // 0/1/2 (2 = follow system)
		{ "NoBallons",      this->NoBallons },
		{ "Log2File",       this->Log2File },
		{ "Log2csv",        this->Log2csv },
		{ "Cycle",          this->Cycle },
		{ "ShowGraph",      this->ShowGraph },
		{ "IconColorFan",   this->IconColorFan },
		{ "ShowBiasedTemps",this->ShowBiasedTemps },
		{ "Lev64Norm",      this->Lev64Norm },
		{ "NoExtSensor",    this->NoExtSensor },
		{ "FailsafeTemp",   fsT },   // display unit on disk, Celsius internally
		{ "CriticalTemp",   crT },   // display unit on disk, Celsius internally
	};
	const int N = (int)(sizeof(items) / sizeof(items[0]));
	bool done[32] = { false };
	bool doneIcon = false;   // IconLevels is a 3-int line, handled separately

	// IconLevels are stored internally in Celsius; write them back in the user's
	// display unit so a Fahrenheit ini round-trips correctly
	int icA = this->IconLevels[0], icB = this->IconLevels[1], icC = this->IconLevels[2];
	if (this->Fahrenheit) {
		icA = icA * 9 / 5 + 32; icB = icB * 9 / 5 + 32; icC = icC * 9 / 5 + 32;
	}

	FILE* fin = NULL;
	if (fopen_s(&fin, configfile, "r") != 0 || !fin) {
		this->Trace("SaveConfig: cannot open ini for reading");
		return;
	}

	char tmpname[MAX_PATH];
	sprintf_s(tmpname, sizeof(tmpname), "%s.tmp", configfile);
	FILE* fout = NULL;
	if (fopen_s(&fout, tmpname, "w") != 0 || !fout) {
		fclose(fin);
		this->Trace("SaveConfig: cannot open temp file for writing");
		return;
	}

	char buf[1024];
	while (fgets(buf, sizeof(buf), fin)) {
		// match the key token at the start of the line (after any leading blanks)
		char* s = buf;
		while (*s == ' ' || *s == '\t') s++;

		// IconLevels: multi-value line not covered by the scalar table above
		if (!doneIcon && _strnicmp(s, "IconLevels=", 11) == 0) {
			char* cmt = strstr(buf, "//");
			if (cmt) {
				char c[1024];
				strcpy_s(c, sizeof(c), cmt);
				char* nl = strpbrk(c, "\r\n");
				if (nl) *nl = 0;
				fprintf(fout, "IconLevels=%d %d %d %s\r\n", icA, icB, icC, c);
			}
			else {
				fprintf(fout, "IconLevels=%d %d %d\r\n", icA, icB, icC);
			}
			doneIcon = true;
			continue;
		}

		int matched = -1;
		for (int i = 0; i < N; i++) {
			size_t klen = strlen(items[i].key);
			if (_strnicmp(s, items[i].key, klen) == 0 && s[klen] == '=') {
				matched = i;
				break;
			}
		}

		if (matched >= 0 && !done[matched]) {
			// preserve any inline "// ..." comment that followed the old value
			char* cmt = strstr(buf, "//");
			if (cmt) {
				char c[1024];
				strcpy_s(c, sizeof(c), cmt);
				char* nl = strpbrk(c, "\r\n");
				if (nl) *nl = 0;
				fprintf(fout, "%s=%d %s\r\n", items[matched].key, items[matched].val, c);
			}
			else {
				fprintf(fout, "%s=%d\r\n", items[matched].key, items[matched].val);
			}
			done[matched] = true;
		}
		else {
			fputs(buf, fout);
		}
	}

	// append any keys that were not present in the original file
	for (int i = 0; i < N; i++) {
		if (!done[i])
			fprintf(fout, "%s=%d\r\n", items[i].key, items[i].val);
	}
	if (!doneIcon)
		fprintf(fout, "IconLevels=%d %d %d\r\n", icA, icB, icC);

	fclose(fin);
	fclose(fout);

	// atomically replace the original with the rewritten copy. Unlike
	// remove()+rename(), this never leaves a window where the ini is missing,
	// and on failure the user's original config is left fully intact.
	if (::MoveFileExA(tmpname, configfile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		this->Trace("Settings saved to TPFanControl.ini");
	else {
		this->Trace("SaveConfig: could not replace ini (original kept)");
		remove(tmpname);
	}
}

//-------------------------------------------------------------------------
//  persist only the WindowPos= line (called on exit). Captures the current
//  restored rect first, then atomically rewrites that single key, leaving every
//  other setting and all comments untouched.
//-------------------------------------------------------------------------
void
FANCONTROL::SaveWindowPos(const char* configfile) {
	if (!this->hwndDialog)
		return;

	WINDOWPLACEMENT wp = NULLSTRUCT;
	wp.length = sizeof(wp);
	if (!::GetWindowPlacement(this->hwndDialog, &wp))
		return;
	const RECT& r = wp.rcNormalPosition;   // restored rect, even if currently minimized
	this->WinX = r.left;
	this->WinY = r.top;
	this->WinW = r.right - r.left;
	this->WinH = r.bottom - r.top;
	if (this->WinW <= 0 || this->WinH <= 0)
		return;

	FILE* fin = NULL;
	if (fopen_s(&fin, configfile, "r") != 0 || !fin)
		return;
	char tmpname[MAX_PATH];
	sprintf_s(tmpname, sizeof(tmpname), "%s.tmp", configfile);
	FILE* fout = NULL;
	if (fopen_s(&fout, tmpname, "w") != 0 || !fout) {
		fclose(fin);
		return;
	}

	char buf[1024];
	bool done = false;
	while (fgets(buf, sizeof(buf), fin)) {
		char* s = buf;
		while (*s == ' ' || *s == '\t') s++;
		if (!done && _strnicmp(s, "WindowPos=", 10) == 0) {
			fprintf(fout, "WindowPos=%d %d %d %d\r\n", this->WinX, this->WinY, this->WinW, this->WinH);
			done = true;
		}
		else {
			fputs(buf, fout);
		}
	}
	if (!done)
		fprintf(fout, "WindowPos=%d %d %d %d\r\n", this->WinX, this->WinY, this->WinW, this->WinH);

	fclose(fin);
	fclose(fout);
	::MoveFileExA(tmpname, configfile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

//-------------------------------------------------------------------------
//  rewrite the Level= / Level2= curve lines from SmartLevels1 / SmartLevels2.
//  The in-memory tables are Celsius; the ini stores the user's display unit, so
//  values are converted back to Fahrenheit when in F mode (matching ReadConfig,
//  and keeping the "first level >= 80 -> Fahrenheit" auto-detect consistent).
//  The new lines replace the first existing run of each key, preserving position
//  and every other line/comment; emptied profile 2 simply drops its Level2 lines.
//-------------------------------------------------------------------------
void
FANCONTROL::SaveCurves(const char* configfile) {
	bool f = this->Fahrenheit != 0;
	auto dispT = [f](int c) { return f ? c * 9 / 5 + 32 : c; };   // temp C->display
	auto dispD = [f](int c) { return f ? c * 9 / 5 : c; };        // delta C->display

	FILE* fin = NULL;
	if (fopen_s(&fin, configfile, "r") != 0 || !fin) {
		this->Trace("SaveCurves: cannot open ini for reading");
		return;
	}
	char tmpname[MAX_PATH];
	sprintf_s(tmpname, sizeof(tmpname), "%s.tmp", configfile);
	FILE* fout = NULL;
	if (fopen_s(&fout, tmpname, "w") != 0 || !fout) {
		fclose(fin);
		this->Trace("SaveCurves: cannot open temp file for writing");
		return;
	}

	const bool haveP2 = (this->SmartLevels2[0].temp2 != 0);

	// emit one profile's "<key>=temp fan [hystUp hystDown]" lines (hyst omitted
	// when both are zero, to keep the file tidy)
	auto writeProfile = [&](const char* key, bool profile2) {
		for (int i = 0; ; i++) {
			int t   = profile2 ? this->SmartLevels2[i].temp2     : this->SmartLevels1[i].temp1;
			if (t == -1) break;
			int fan = profile2 ? this->SmartLevels2[i].fan2      : this->SmartLevels1[i].fan1;
			int hu  = profile2 ? this->SmartLevels2[i].hystUp2   : this->SmartLevels1[i].hystUp1;
			int hd  = profile2 ? this->SmartLevels2[i].hystDown2 : this->SmartLevels1[i].hystDown1;
			if (hu != 0 || hd != 0)
				fprintf(fout, "%s=%d %d %d %d\r\n", key, dispT(t), fan, dispD(hu), dispD(hd));
			else
				fprintf(fout, "%s=%d %d\r\n", key, dispT(t), fan);
		}
	};

	char buf[1024];
	bool wroteL = false, wroteL2 = false;
	while (fgets(buf, sizeof(buf), fin)) {
		char* s = buf;
		while (*s == ' ' || *s == '\t') s++;

		if (_strnicmp(s, "level2=", 7) == 0) {
			if (!wroteL2) { if (haveP2) writeProfile("Level2", true); wroteL2 = true; }
			continue;   // drop the original line
		}
		if (_strnicmp(s, "level=", 6) == 0) {
			if (!wroteL) { writeProfile("Level", false); wroteL = true; }
			continue;
		}
		fputs(buf, fout);
	}
	if (!wroteL)               writeProfile("Level", false);
	if (!wroteL2 && haveP2)    writeProfile("Level2", true);

	fclose(fin);
	fclose(fout);

	if (::MoveFileExA(tmpname, configfile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		this->Trace("Fan curve saved to TPFanControl.ini");
	else {
		this->Trace("SaveCurves: could not replace ini (original kept)");
		remove(tmpname);
	}
}

//-------------------------------------------------------------------------
//  apply a saved window rect, but only if it is still visible on some monitor
//  (guards against a position left off-screen by an unplugged display).
//-------------------------------------------------------------------------
void
FANCONTROL::RestoreWindowPos() {
	if (!this->hwndDialog || this->WinW <= 0 || this->WinH <= 0)
		return;

	RECT r = { this->WinX, this->WinY, this->WinX + this->WinW, this->WinY + this->WinH };
	HMONITOR hMon = ::MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
	if (!hMon)
		return;   // saved rect lies entirely off all current monitors -> ignore it

	// Restore size only for the resizable full dialog; the slim dialogs have a
	// fixed frame, so a size saved in the other mode must not be forced on them.
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
	if (this->SlimDialog == 1)
		flags |= SWP_NOSIZE;
	::SetWindowPos(this->hwndDialog, NULL, r.left, r.top, this->WinW, this->WinH, flags);
}

//-------------------------------------------------------------------------
//  read config file
//-------------------------------------------------------------------------
int
FANCONTROL::ReadConfig(const char* configfile)
{
	char buf[1024];

	int i, ok = false, lcnt1 = 0, lcnt2 = 0;

	int ProcessPriority = 2;

	strncpy_s(this->MenuLabelSM1, sizeof(this->MenuLabelSM1), "Smart Level 1", 14);
	strncpy_s(this->MenuLabelSM2, sizeof(this->MenuLabelSM2), "Smart Level 2", 14);

	setzero(SensorOffset, sizeof(SensorOffset));
	setzero(FSensorOffset, sizeof(FSensorOffset));

	this->State.Fan1SpeedHi = 0x00;
	this->State.Fan1SpeedLo = 0x00;
	this->State.Fan2SpeedHi = 0x00;
	this->State.Fan2SpeedLo = 0x00;

	this->fan1speed = 0;
	this->fan2speed = 0;

	this->IndSmartLevel = 0;

	//
	// read from file
	//
	FILE* f;
	errno_t errf = fopen_s(&f, configfile, "r");
	if (!errf) {
		while (fgets(buf, sizeof(buf), f)) {

			if (buf[0] == '/' || buf[0] == '#' || buf[0] == ';')
				continue;

			if (_strnicmp(buf, "UseTWR=", 7) == 0) {
				this->UseTWR = atoi(buf + 7);
				continue;
			}

			if (_strnicmp(buf, "Active=", 7) == 0) {
				this->ActiveMode = atoi(buf + 7);
				continue;
			}

			if (_strnicmp(buf, "ManFanSpeed=", 12) == 0) {
				this->ManFanSpeed = atoi(buf + 12);
				continue;
			}

			if (_strnicmp(buf, "ProcessPriority=", 16) == 0) {
				ProcessPriority = atoi(buf + 16);
				continue;
			}

			if (_strnicmp(buf, "cycle=", 6) == 0) {
				this->Cycle = atoi(buf + 6);
				continue;
			}
			
			if (_strnicmp(buf, "IconCycle=", 10) == 0) {
				this->IconCycle = atoi(buf + 10);
				continue;
			}

			if (_strnicmp(buf, "ReIcCycle=", 10) == 0) {
				this->ReIcCycle = atoi(buf + 10);
				continue;
			}

			if (_strnicmp(buf, "IconFontSize=", 13) == 0) {
				this->iFontIconB = atoi(buf + 13);
				continue;
			}

			if (_strnicmp(buf, "MenuLabelSM1=", 13) == 0) {
				char* p = buf + 13, * p2 = this->MenuLabelSM1;
				char* p2end = this->MenuLabelSM1 + sizeof(this->MenuLabelSM1) - 1;
				while (*p && *p != '/' && p2 < p2end) {
					if (*p != '\t' && *p != '\r' && *p != '\n')
						*p2++ = *p;
					p++;
				}
				*p2 = '\0';
				continue;
			}

			if (_strnicmp(buf, "MenuLabelSM2=", 13) == 0) {
				char* p = buf + 13, * p2 = this->MenuLabelSM2;
				char* p2end = this->MenuLabelSM2 + sizeof(this->MenuLabelSM2) - 1;
				while (*p && *p != '/' && p2 < p2end) {
					if (*p != '\t' && *p != '\r' && *p != '\n')
						*p2++ = *p;
					p++;
				}
				*p2 = '\0';
				continue;
			}

			if (_strnicmp(buf, "FanSpeedLowByte=", 16) == 0) {
				this->FanSpeedLowByte = atoi(buf + 16);
				continue;
			}

			if (_strnicmp(buf, "NoExtSensor=", 12) == 0) {
				this->NoExtSensor = atoi(buf + 12);
				continue;
			}

			if (_strnicmp(buf, "SlimDialog=", 11) == 0) {
				this->SlimDialog = atoi(buf + 11);
				if (this->SlimDialog != 0) 
					this->SlimDialog = 1;
				continue;
			}

			if (_strnicmp(buf, "level=", 6) == 0) {
				// cap at array size - 1: the loop writes a -1 terminator at
				// SmartLevels[lcnt1] afterwards, so that slot must stay free.
				if (lcnt1 < (int)ARRAYMAX(this->SmartLevels) - 1) {
					sscanf_s(buf + 6, "%d %d %d %d", &this->SmartLevels[lcnt1].temp, &this->SmartLevels[lcnt1].fan, &this->SmartLevels[lcnt1].hystUp, &this->SmartLevels[lcnt1].hystDown);
					sscanf_s(buf + 6, "%d %d %d %d", &this->SmartLevels1[lcnt1].temp1, &this->SmartLevels1[lcnt1].fan1, &this->SmartLevels1[lcnt1].hystUp1, &this->SmartLevels1[lcnt1].hystDown1);
					lcnt1++;
				}
				continue;
			}

			if (_strnicmp(buf, "level2=", 7) == 0) {
				if (lcnt2 < (int)ARRAYMAX(this->SmartLevels2) - 1) {
					sscanf_s(buf + 7, "%d %d %d %d", &this->SmartLevels2[lcnt2].temp2, &this->SmartLevels2[lcnt2].fan2, &this->SmartLevels2[lcnt2].hystUp2, &this->SmartLevels2[lcnt2].hystDown2);
					lcnt2++;
				}
				continue;
			}

			if (_strnicmp(buf, "fanbeep=", 8) == 0) {
				sscanf_s(buf + 8, "%d %d", &this->FanBeepFreq, &this->FanBeepDura);
				continue;
			}

			if (_strnicmp(buf, "iconlevels=", 11) == 0) {
				sscanf_s(buf + 11, "%d %d %d", &this->IconLevels[0], &this->IconLevels[1], &this->IconLevels[2]);
				continue;
			}

			if (_strnicmp(buf, "NoWaitMessage=", 14) == 0) {
				this->NoWaitMessage = atoi(buf + 14);
				continue;
			}

			if (_strnicmp(buf, "StartMinimized=", 15) == 0) {
				this->StartMinimized = atoi(buf + 15);
				continue;
			}

			if (_strnicmp(buf, "NoBallons=", 10) == 0) {
				this->NoBallons = atoi(buf + 10);
				continue;
			}

			if (_strnicmp(buf, "HK_BIOS=", 8) == 0) {
				this->HK_BIOS_Method = buf[8] - 0x30;
				this->HK_BIOS = buf[10];
				if ((this->HK_BIOS == 0x46) && (buf[11] > 0x30) && (buf[11] < 0x40)) 
					this->HK_BIOS = 0x70 + atoi(buf + 11) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_Manual=", 10) == 0) {
				this->HK_Manual_Method = buf[10] - 0x30;
				this->HK_Manual = buf[12];
				if ((this->HK_Manual == 0x46) && (buf[13] > 0x30) && (buf[13] < 0x40)) 
					this->HK_Manual = 0x70 + atoi(buf + 13) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_Smart=", 9) == 0) {
				this->HK_Smart_Method = buf[9] - 0x30;
				this->HK_Smart = buf[11];
				if ((this->HK_Smart == 0x46) && (buf[12] > 0x30) && (buf[12] < 0x40))
					this->HK_Smart = 0x70 + atoi(buf + 12) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_SM1=", 7) == 0) {
				this->HK_SM1_Method = buf[7] - 0x30;
				this->HK_SM1 = buf[9];
				if ((this->HK_SM1 == 0x46) && (buf[10] > 0x30) && (buf[10] < 0x40))
					this->HK_SM1 = 0x70 + atoi(buf + 10) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_SM2=", 7) == 0) {
				this->HK_SM2_Method = buf[7] - 0x30;
				this->HK_SM2 = buf[9];
				if ((this->HK_SM2 == 0x46) && (buf[10] > 0x30) && (buf[10] < 0x40))
					this->HK_SM2 = 0x70 + atoi(buf + 10) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_TG_BS=", 9) == 0) {
				this->HK_TG_BS_Method = buf[9] - 0x30;
				this->HK_TG_BS = buf[11];
				if ((this->HK_TG_BS == 0x46) && (buf[12] > 0x30) && (buf[12] < 0x40))
					this->HK_TG_BS = 0x70 + atoi(buf + 12) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_TG_BM=", 9) == 0) {
				this->HK_TG_BM_Method = buf[9] - 0x30;
				this->HK_TG_BM = buf[11];
				if ((this->HK_TG_BM == 0x46) && (buf[12] > 0x30) && (buf[12] < 0x40))
					this->HK_TG_BM = 0x70 + atoi(buf + 12) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_TG_MS=", 9) == 0) {
				this->HK_TG_MS_Method = buf[9] - 0x30;
				this->HK_TG_MS = buf[11];
				if ((this->HK_TG_MS == 0x46) && (buf[12] > 0x30) && (buf[12] < 0x40))
					this->HK_TG_MS = 0x70 + atoi(buf + 12) - 1;
				continue;
			}

			if (_strnicmp(buf, "HK_TG_12=", 9) == 0) {
				this->HK_TG_12_Method = buf[9] - 0x30;
				this->HK_TG_12 = buf[11];
				if ((this->HK_TG_12 == 0x46) && (buf[12] > 0x30) && (buf[12] < 0x40))
					this->HK_TG_12 = 0x70 + atoi(buf + 12) - 1;
				continue;
			}

			if (_strnicmp(buf, "IconColorFan=", 13) == 0) {
				this->IconColorFan = atoi(buf + 13);
				continue;
			}

			if (_strnicmp(buf, "Lev64Norm=", 10) == 0) {
				this->Lev64Norm = atoi(buf + 10);
				continue;
			}

			if (_strnicmp(buf, "ManModeExit=", 12) == 0) {
				this->ManModeExit = atoi(buf + 12);
				continue;
			}

			if (_strnicmp(buf, "FailsafeTemp=", 13) == 0) {
				this->FailsafeTemp = atoi(buf + 13);   // deg C, 0 = disabled
				continue;
			}

			if (_strnicmp(buf, "ShowBiasedTemps=", 16) == 0) {
				this->ShowBiasedTemps = atoi(buf + 16);
				continue;
			}

			if (_strnicmp(buf, "MaxReadErrors=", 14) == 0) {
				this->MaxReadErrors = atoi(buf + 14);
				continue;
			}

			if (_strnicmp(buf, "SecWinUptime=", 13) == 0) {
				this->SecWinUptime = atoi(buf + 13);
				continue;
			}

			if (_strnicmp(buf, "SecStartDelay=", 14) == 0) {
				this->SecStartDelay = atoi(buf + 14);
				continue;
			}

			if (_strnicmp(buf, "Log2File=", 9) == 0) {
				this->Log2File = atoi(buf + 9);
				continue;
			}

			if (_strnicmp(buf, "StayOnTop=", 10) == 0) {
				this->StayOnTop = atoi(buf + 10);
				continue;
			}

			// opt-in taskbar button (Alt-Tab / Snap Layouts / overlay+progress)
			if (_strnicmp(buf, "ShowInTaskbar=", 14) == 0) {
				this->ShowInTaskbar = atoi(buf + 14);
				continue;
			}

			// sleep handling: 0=ignore, 1=BIOS during sleep + restore, 2=keep mode
			if (_strnicmp(buf, "SuspendMode=", 12) == 0) {
				int v = atoi(buf + 12);
				this->SuspendMode = (v >= 0 && v <= 2) ? v : 1;
				continue;
			}

			// critical-temp threshold (display unit; F->C convert + range-check
			// happen in the post-parse block once Fahrenheit mode is known)
			if (_strnicmp(buf, "CriticalTemp=", 13) == 0) {
				this->CriticalTemp = atoi(buf + 13);
				continue;
			}
			
			if (_strnicmp(buf, "Log2csv=", 8) == 0) {
				this->Log2csv = atoi(buf + 8);
				continue;
			}

			if (_strnicmp(buf, "ShowAll=", 8) == 0) {
				this->ShowAll = atoi(buf + 8);
				continue;
			}
			
			if (_strnicmp(buf, "ShowTempIcon=", 13) == 0) {
				this->ShowTempIcon = atoi(buf + 13);
				continue;
			}

			if (_strnicmp(buf, "ShowTempHex=", 12) == 0) {
				this->ShowTempHex = atoi(buf + 12);
				continue;
			}

			if (_strnicmp(buf, "ShowLog=", 8) == 0) {
				this->ShowLog = atoi(buf + 8);
				continue;
			}

			if (_strnicmp(buf, "DarkMode=", 9) == 0) {
				// 0 = light, 2 = follow the system theme, any other nonzero =
				// dark (preserves the old 0/1 meaning). The effective DarkMode
				// is resolved in the constructor once parsing is done.
				int v = atoi(buf + 9);
				this->DarkModeSetting = (v == 2) ? 2 : (v ? 1 : 0);
				continue;
			}

			if (_strnicmp(buf, "ShowGraph=", 10) == 0) {
				this->ShowGraph = atoi(buf + 10);
				continue;
			}

			// last main-window placement: "WindowPos=x y w h" (restored rect).
			// Applied after the dialog is created (see RestoreWindowPos).
			if (_strnicmp(buf, "WindowPos=", 10) == 0) {
				sscanf_s(buf + 10, "%d %d %d %d",
					&this->WinX, &this->WinY, &this->WinW, &this->WinH);
				continue;
			}

			// SensorNameN= (N=1..16) -> gSensorNames[N-1], up to 3 chars
			{
				bool handled = false;
				for (int n = 1; n <= 16; n++) {
					char key[20];
					int klen = sprintf_s(key, sizeof(key), "SensorName%d=", n);
					if (_strnicmp(buf, key, klen) == 0) {
						strncpy_s(this->gSensorNames[n - 1], sizeof(this->gSensorNames[n - 1]), buf + klen, 3);
						handled = true;
						break;
					}
				}
				if (handled) continue;
			}

			// SensorOffsetN= (N=1..16) -> SensorOffset[N-1]: offs, hystMin, hystMax
			{
				bool handled = false;
				for (int n = 1; n <= 16; n++) {
					char key[20];
					int klen = sprintf_s(key, sizeof(key), "SensorOffset%d=", n);
					if (_strnicmp(buf, key, klen) == 0) {
						sscanf_s(buf + klen, "%d %d %d", &this->SensorOffset[n - 1].offs, &this->SensorOffset[n - 1].hystMin, &this->SensorOffset[n - 1].hystMax);
						handled = true;
						break;
					}
				}
				if (handled) continue;
			}

			if (_strnicmp(buf, "IgnoreSensors=", 14) == 0) {
				char* p = buf + 14, * p2 = this->IgnoreSensors;
				char* p2end = this->IgnoreSensors + sizeof(this->IgnoreSensors) - 1;
				while (*p && p2 < p2end) {
					if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
						*p2++ = *p;
					p++;
				}
				*p2 = '\0';
				continue;
			}
		}

		fclose(f);

		if (this->UseTWR != 0) {
			this->Trace("UseTWR is unsupported on the PawnIO backend; using standard per-register temp reads");
			this->UseTWR = 0;
		}

		// end marker for smart levels array
		if (lcnt1) {
			this->SmartLevels[lcnt1].temp = -1;
			this->SmartLevels1[lcnt1].temp1 = -1;
			this->SmartLevels[lcnt1].fan = 0x80;
			this->SmartLevels1[lcnt1].fan1 = 0x80;
		}

		if (lcnt2) {
			this->SmartLevels2[lcnt2].temp2 = -1;
			this->SmartLevels2[lcnt2].fan2 = 0x80;
		}

		ok = true;

		this->Trace("Current Config:");
		this->Trace(FANCONTROLVERSION);
	}
	else {
		// no ini present -> write a sensible default so the user has one to edit
		FILE* fnew;
		if (fopen_s(&fnew, configfile, "w") == 0 && fnew) {
			fputs(
				"# TPFanControl.ini - auto-generated defaults (" FANCONTROLVERSION ")\r\n"
				"# Edit values and restart TPFanControl to apply.\r\n"
				"\r\n"
				"# 0=read only, 1=allow fan control, 2=start Smart, 3=start Manual\r\n"
				"Active=1\r\n"
				"\r\n"
				"# Manual fan speed: 0-7, or 64 = full/max\r\n"
				"ManFanSpeed=7\r\n"
				"\r\n"
				"# Temperature poll interval (seconds)\r\n"
				"Cycle=5\r\n"
				"\r\n"
				"# Start minimized to the tray (0/1)\r\n"
				"StartMinimized=0\r\n"
				"\r\n"
				"# Show a taskbar button too (Alt-Tab, Snap Layouts, severity badge\r\n"
				"# + fan-level progress on the button). 0 = classic tray-only.\r\n"
				"ShowInTaskbar=0\r\n"
				"\r\n"
				"# New-style temperature tray icon (0/1)\r\n"
				"ShowTempIcon=1\r\n"
				"\r\n"
				"# GUI options (also toggleable via the in-window checkboxes)\r\n"
				"# DarkMode: 0 = light, 1 = dark, 2 = follow the Windows app theme\r\n"
				"ShowTempHex=0\r\n"
				"ShowLog=1\r\n"
				"DarkMode=2\r\n"
				"ShowGraph=1\r\n"
				"\r\n"
				"# Switch back to BIOS after this many consecutive read errors\r\n"
				"MaxReadErrors=10\r\n"
				"\r\n"
				"# Logging (0/1)\r\n"
				"Log2File=0\r\n"
				"Log2csv=0\r\n"
				"\r\n"
				"# Sensor names (shown capitalized in the list)\r\n"
				"SensorName1=cpu\r\n"
				"SensorName2=gpu\r\n"
				"\r\n"
				"# Smart fan curve: Level=<temp C> <fan 0-7 | 64=full | 128=BIOS>\r\n"
				"# First level fan should be 0 (fan off).\r\n"
				"Level=50 0\r\n"
				"Level=60 2\r\n"
				"Level=65 4\r\n"
				"Level=70 7\r\n"
				"Level=80 64\r\n"
				"Level=90 128\r\n"
				"\r\n"
				"# Tray icon color thresholds: yellow orange red (deg C)\r\n"
				"IconLevels=50 65 78\r\n"
				"\r\n"
				"# Leave Manual mode if temperature reaches this (deg C)\r\n"
				"ManModeExit=80\r\n"
				"\r\n"
				"# Thermal fail-safe: force full fan speed (max) when the max temperature\r\n"
				"# reaches this (deg C), in Smart/Manual mode, until ~3 deg below.\r\n"
				"# 0 = disabled.\r\n"
				"FailsafeTemp=0\r\n"
				"\r\n"
				"# Sleep handling (suspend / Modern Standby): 0 = ignore sleep,\r\n"
				"# 1 = hand fan to BIOS during sleep and restore the mode after resume,\r\n"
				"# 2 = keep the current mode (still re-asserts the level after resume)\r\n"
				"SuspendMode=1\r\n"
				"\r\n"
				"# Critical temp: if the max temperature holds at/above this (deg C,\r\n"
				"# 70-110) for 3 polls even at full fan speed, pin the fan to maximum and\r\n"
				"# warn. 0 = disabled (opt-in). Set a few deg below your CPU thermal trip.\r\n"
				"CriticalTemp=0\r\n",
				fnew);
			fclose(fnew);
			this->Trace("TPFanControl.ini not found - created a default one");
		}
		else {
			this->Trace("TPFanControl.ini missing, default values:");
		}
	}

	// Create the main dialog regardless of ini presence: a first run with no
	// ini used to come up windowless AND tray-less (the tray binds to
	// hwndDialog) until a restart. StayOnTop is either the parsed value or
	// the default. (Fix observed in the BeteixZ fork, commit 10e3ab4.)
	this->hwndDialog = ::CreateDialogParam(hinstapp,
		MAKEINTRESOURCE(this->StayOnTop ? 9000 : 9002),
		HWND_DESKTOP,
		(DLGPROC)BaseDlgProc,
		(LPARAM)this);
	if (!this->hwndDialog) {
		// A service runs on the invisible session-0 desktop, where a modal box would
		// block forever with nobody to dismiss it - and SCM auto-restart would then
		// wedge on every restart. Only pop it in interactive mode. (Runs_as_service
		// is not assigned until below, so test g_SvcHandle, which it derives from.)
		extern SERVICE_STATUS_HANDLE g_SvcHandle;
		if (g_SvcHandle == NULL) {
			char emsg[128];
			sprintf_s(emsg, sizeof(emsg),
				"Could not create the main window (error %lu).", ::GetLastError());
			::MessageBoxA(NULL, emsg, "TPFanControl", MB_OK | MB_ICONERROR);
		}
	}

	// Running as a service exactly when the SCM control handler has been
	// registered (ServiceMain sets g_SvcHandle before the worker -> ReadConfig
	// runs; it stays NULL in desktop mode). Replaces a brittle reentrant-mutex /
	// thread-identity side-effect that also leaked its handle.
	extern SERVICE_STATUS_HANDLE g_SvcHandle;
	Runs_as_service = (g_SvcHandle != NULL);

	//Offset Fahrenheit to Celsius
	if (this->SmartLevels[0].temp >= 80) Fahrenheit = true;

	// Set ProcessPriority
	bool _SPC;
	switch (ProcessPriority) {
	case 5: _SPC = SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS); break;
	case 4: _SPC = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS); break;
	case 3: _SPC = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS); break;
//  case 2: _SPC = SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS); break;
	case 1: _SPC = SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); break;
	case 0: _SPC = SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS); break;
	default: break; // _SPC = SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS); 
	}

	//
	// display config
	//
	if (this->IconCycle <= 0 || this->IconCycle >= 60) this->IconCycle = 1;

	// Clamp values that come straight from atoi() and feed timers / waits / safety.
	// A hand-edited ini can otherwise set Cycle=0 (SetTimer(0) hammers the EC), a
	// negative delay (cast to a huge ULONGLONG wait later), or MaxReadErrors=0
	// (ReadErrorCount>=0 is always true -> instant BIOS fallback on the first error).
	// Matches the ranges the Settings dialog already enforces (H-10).
	if (this->Cycle < 1)          this->Cycle = 5;
	if (this->Cycle > 600)        this->Cycle = 600;
	if (this->SecStartDelay < 0)  this->SecStartDelay = 0;
	if (this->SecWinUptime < 0)   this->SecWinUptime = 0;
	if (this->MaxReadErrors < 1)  this->MaxReadErrors = 10;
	if (this->MaxReadErrors > 1000) this->MaxReadErrors = 1000;
	sprintf_s(buf, sizeof(buf), "  Active= %d, Cycle= %d, FanBeep= %d %d, MaxReadErrors= %d",
		this->ActiveMode, this->Cycle,
		this->FanBeepFreq, this->FanBeepDura, this->MaxReadErrors);
	this->Trace(buf);

	sprintf_s(buf, sizeof(buf), "  IconLevels= %d %d %d, NoExtSensor= %d, Lev64Norm= %d",
		this->IconLevels[0], this->IconLevels[1], this->IconLevels[2],
		this->NoExtSensor, this->Lev64Norm);
	this->Trace(buf);

	sprintf_s(buf, sizeof(buf), "  Log2File= %d, Log2csv= %d, ShowAll= %d, IconColorFan= %d",
		this->Log2File, this->Log2csv, this->ShowAll, this->IconColorFan);
	this->Trace(buf);

	this->ShowAllToDialog(ShowAll);

	setzero(buf, sizeof(buf));

	if (Fahrenheit) {
		strcpy_s(buf, sizeof(buf), "  ");
		for (i = 0; this->SmartLevels[i].temp != -1; i++) {
			sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d\xb0" "F -> ", i > 0 ? ", " : "", this->SmartLevels[i].temp);
			if (this->SmartLevels[i].fan != 0x80)
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels[i].fan);
			else
				strcat_s(buf, sizeof(buf), "0x80");
		}
	}
	else {
		strcpy_s(buf, sizeof(buf), "  Levels= ");
		for (i = 0; this->SmartLevels[i].temp != -1; i++) {
			sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d\xb0" "C -> ", i > 0 ? ",  " : "", this->SmartLevels[i].temp);
			if (this->SmartLevels[i].fan != 0x80)
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels[i].fan);
			else
				strcat_s(buf, sizeof(buf), "0x80");
		}
	}
	this->Trace(buf);

	//Levels2	

	if (this->SmartLevels2[0].temp2 != 0)
	{
		if (Fahrenheit) {
			strcpy_s(buf, sizeof(buf), "  ");
			for (i = 0; this->SmartLevels2[i].temp2 != -1; i++) {
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d\xb0" "F -> ", i > 0 ? ", " : "", this->SmartLevels2[i].temp2);
				if (this->SmartLevels2[i].fan2 != 0x80)
					sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels2[i].fan2);
				else
					strcat_s(buf, sizeof(buf), "0x80");
			}
		}
		else {
			strcpy_s(buf, sizeof(buf), "  Levels2= ");
			for (i = 0; this->SmartLevels2[i].temp2 != -1; i++) {
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d\xb0" "C -> ", i > 0 ? ",  " : "", this->SmartLevels2[i].temp2);
				if (this->SmartLevels2[i].fan2 != 0x80)
					sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels2[i].fan2);
				else
					strcat_s(buf, sizeof(buf), "0x80");
			}
		}
		this->Trace(buf);
	}

	if (Fahrenheit) {
		sprintf_s(buf, sizeof(buf), "  SensorOffset1-12= %d %d %d %d %d %d %d %d %d %d %d %d \xb0" "F",
			this->SensorOffset[0].offs, this->SensorOffset[1].offs, this->SensorOffset[2].offs,
			this->SensorOffset[3].offs, this->SensorOffset[4].offs, this->SensorOffset[5].offs,
			this->SensorOffset[6].offs, this->SensorOffset[7].offs, this->SensorOffset[8].offs,
			this->SensorOffset[9].offs, this->SensorOffset[10].offs, this->SensorOffset[11].offs);

		// Convert all entries, not just 15. offs is a temperature *delta* (scale
		// only), while hystMin/hystMax are absolute temps compared against the
		// Celsius sensor reading in HandleData, so convert them like a temperature.
		for (i = 0; i < (int)ARRAYMAX(SensorOffset); i++) {
			SensorOffset[i].offs    =  SensorOffset[i].offs * 5 / 9;
			SensorOffset[i].hystMin = (SensorOffset[i].hystMin - 32) * 5 / 9;
			SensorOffset[i].hystMax = (SensorOffset[i].hystMax - 32) * 5 / 9;
		}
	}
	else {
		sprintf_s(buf, sizeof(buf), "  SensorOffset1-12= %d %d %d %d %d %d %d %d %d %d %d %d \xb0" "C",
			this->SensorOffset[0].offs, this->SensorOffset[1].offs, this->SensorOffset[2].offs,
			this->SensorOffset[3].offs, this->SensorOffset[4].offs, this->SensorOffset[5].offs,
			this->SensorOffset[6].offs, this->SensorOffset[7].offs, this->SensorOffset[8].offs,
			this->SensorOffset[9].offs, this->SensorOffset[10].offs, this->SensorOffset[11].offs);
	}
	this->Trace(buf);

	sprintf_s(buf, sizeof(buf), "  IgnoreSensors= %s, ProcessPriority= %d, IconCycle= %d", IgnoreSensors, ProcessPriority, IconCycle);
	this->Trace(buf);

	sprintf_s(buf, sizeof(buf), "  NoWaitMessage= %d, ShowBiasedTemps= %d", NoWaitMessage, ShowBiasedTemps);
	this->Trace(buf);

	// ManModeExit is given in the display unit; derive the Celsius value the Manual-
	// mode auto-exit compares against MaxTemp (title timer). In Fahrenheit mode a
	// value too low to be a sane F threshold (e.g. the Celsius default 78/80 left
	// unconverted) is almost certainly a forgotten unit conversion, so treat it as
	// Celsius rather than letting Manual self-exit at ~25 C. (Replaces a stale
	// "== 80" check that matched the old constructor default but not the shipped 78.)
	if (Fahrenheit) {
		if (this->ManModeExit < 100)   // implausible as Fahrenheit -> interpret as Celsius
			this->ManModeExit = (this->ManModeExit * 9 / 5) + 32;
		this->ManModeExitInternal = (this->ManModeExit - 32) * 5 / 9;
	}
	else {
		this->ManModeExitInternal = this->ManModeExit;
	}

	sprintf_s(buf, sizeof(buf), "  ManModeExit= %d, SecWinUptime= %d, SecStartDelay= %d", this->ManModeExit, this->SecWinUptime, this->SecStartDelay);
	this->Trace(buf);

	//Offset& Smartlevels Fahrenheit to Celsius
	// Thresholds are absolute temps (F->C with the -32 offset); hystUp/hystDown are
	// *deltas*, so scale them by 5/9 only (a 10 F band must not act like 10 C).
	if (Fahrenheit) {
		for (i = 0; this->SmartLevels[i].temp != -1; i++) {
			this->SmartLevels[i].temp     = (this->SmartLevels[i].temp - 32) * 5 / 9;
			this->SmartLevels[i].hystUp   =  this->SmartLevels[i].hystUp   * 5 / 9;
			this->SmartLevels[i].hystDown =  this->SmartLevels[i].hystDown * 5 / 9;
		}
		for (i = 0; this->SmartLevels1[i].temp1 != -1; i++) {
			this->SmartLevels1[i].temp1     = (this->SmartLevels1[i].temp1 - 32) * 5 / 9;
			this->SmartLevels1[i].hystUp1   =  this->SmartLevels1[i].hystUp1   * 5 / 9;
			this->SmartLevels1[i].hystDown1 =  this->SmartLevels1[i].hystDown1 * 5 / 9;
		}
		// Only when profile 2 is present (temp2[0] != 0). When it is absent the array
		// is all-zero with NO -1 terminator (the terminator is written only when
		// lcnt2 > 0), so an unguarded scan for -1 walked off the end of
		// SmartLevels2[32] - an out-of-bounds write. Convert [0] and the rest together
		// inside the guard (matching the SmartLevels/SmartLevels1 loops above), with an
		// array-size cap as a belt-and-suspenders against a missing terminator.
		if (this->SmartLevels2[0].temp2 != 0) {   // 0 = no 2nd profile present
			for (i = 0; i < (int)ARRAYMAX(this->SmartLevels2) && this->SmartLevels2[i].temp2 != -1; i++) {
				this->SmartLevels2[i].temp2     = (this->SmartLevels2[i].temp2 - 32) * 5 / 9;
				this->SmartLevels2[i].hystUp2   =  this->SmartLevels2[i].hystUp2   * 5 / 9;
				this->SmartLevels2[i].hystDown2 =  this->SmartLevels2[i].hystDown2 * 5 / 9;
			}
		}
		//		for (i= 0; i<15; i++) {SensorOffset[i]= SensorOffset[i] * 5/9;}
		this->IconLevels[0] = (this->IconLevels[0] - 32) * 5 / 9;
		this->IconLevels[1] = (this->IconLevels[1] - 32) * 5 / 9;
		this->IconLevels[2] = (this->IconLevels[2] - 32) * 5 / 9;
	}

	// Safety thresholds are entered in the display unit like the curves/IconLevels.
	// Convert F->C when the config is Fahrenheit (0 = off must stay 0 -> guard >0),
	// then sanity-check in Celsius regardless of the input unit. Done here, after the
	// Fahrenheit block, because Fahrenheit is only known post-parse (see :839).
	if (Fahrenheit) {
		if (this->FailsafeTemp > 0) this->FailsafeTemp = (this->FailsafeTemp - 32) * 5 / 9;
		if (this->CriticalTemp > 0) this->CriticalTemp = (this->CriticalTemp - 32) * 5 / 9;
	}
	// FailsafeTemp: 0 = off, else clamp to a sane ceiling (mirrors the Settings dialog,
	// fancontrol.cpp:4237-4238). Catches an insane Celsius value too, not just F.
	if (this->FailsafeTemp < 0)   this->FailsafeTemp = 0;
	if (this->FailsafeTemp > 120) this->FailsafeTemp = 120;
	// CriticalTemp: only a sane Celsius window arms it; anything else = off (preserves
	// the original parse-time semantics, now applied after conversion).
	this->CriticalTemp = (this->CriticalTemp >= 70 && this->CriticalTemp <= 110) ? this->CriticalTemp : 0;

	// Safety coupling: sticky Manual (ManModeExit=0) removes the only temperature-driven
	// escape from a too-low manual fan level. That is safe ONLY while a fail-safe is armed
	// to force full fan on overheat. With both off, Manual has no software thermal
	// protection below any CriticalTemp threshold (the critical guard, if set, still pins
	// max fan at its threshold); the firmware throttle is the backstop. Warn on it.
	if (this->ManModeExitInternal <= 0 && this->FailsafeTemp <= 0) {
		this->Trace("WARNING: Manual auto-exit disabled (ManModeExit=0) AND fail-safe off "
			"(FailsafeTemp=0) - Manual has no software thermal protection below CriticalTemp; "
			"the firmware throttle is the backstop. Set FailsafeTemp to re-arm the fail-safe.");
	}

	this->Trace("");

	if (this->Log2csv == 1) {
		// rotate the previous CSV without spawning cmd.exe
		::DeleteFileA("TPFanControl_last_csv.txt");
		::MoveFileExA("TPFanControl_csv.txt", "TPFanControl_last_csv.txt", MOVEFILE_REPLACE_EXISTING);

		sprintf_s(buf, sizeof(buf), "time;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;rpm;fan;switch;", 
			this->gSensorNames[0], this->gSensorNames[1], this->gSensorNames[2], 
			this->gSensorNames[3], this->gSensorNames[4], this->gSensorNames[5],
			this->gSensorNames[6], this->gSensorNames[7], this->gSensorNames[8],
			this->gSensorNames[9], this->gSensorNames[10], this->gSensorNames[11]);
		this->Tracecsvod(buf);
	}

	// initial values
	this->TaskbarNew = 0;
	this->MaxTemp = 0;
	this->iMaxTemp = 0;
	this->iFarbeIconB = 10;
	this->iFontIconB = 9;
	this->lastfan1speed = 0;
	this->lastfan2speed = 0;
	this->fan1speed = 0;

	// show sensor names in capitals (CPU, GPU, BAT, ...) everywhere
	for (i = 0; i < 17; i++)
		_strupr_s(this->gSensorNames[i], sizeof(this->gSensorNames[i]));

	// precompute the ignore-list match data now that names are uppercased; both
	// IgnoreSensors and the names are constant for the process lifetime, so
	// HandleData no longer rebuilds this every poll.
	this->BuildIgnoreCache();

	return ok;
}

//-------------------------------------------------------------------------
//  (re)build the cached ignore-list match data from IgnoreSensors + sensor
//  names. IgnoreSensors is entered lower-case in the ini while names are stored
//  upper-case, so both sides are uppercased to match case-insensitively. The
//  result feeds the per-poll max-temp loop in HandleData (m_sensorIgnored).
//-------------------------------------------------------------------------
void
FANCONTROL::BuildIgnoreCache() {
	sprintf_s(this->m_ignoreListNorm, sizeof(this->m_ignoreListNorm), "|%s|", this->IgnoreSensors);
	for (int j = 0; this->m_ignoreListNorm[j] != '\0'; j++)
		if (this->m_ignoreListNorm[j] == ',')
			this->m_ignoreListNorm[j] = '|';
	_strupr_s(this->m_ignoreListNorm, sizeof(this->m_ignoreListNorm));

	for (int j = 0; j < 12; j++) {
		char what[16];
		// match against gSensorNames (always uppercase, never the "n/a" placeholder)
		sprintf_s(what, sizeof(what), "|%s|", this->gSensorNames[j]);
		this->m_sensorIgnored[j] = (strstr(this->m_ignoreListNorm, what) != 0);
	}
}

//-------------------------------------------------------------------------
//  localized date&time
//-------------------------------------------------------------------------
void
FANCONTROL::CurrentDateTimeLocalized(char* result, size_t sizeof_result) {
	SYSTEMTIME s;
	::GetLocalTime(&s);

	// Locale format patterns are constant for the session; resolve them once
	// instead of on every trace/log call.
	static char otfmt[64] = "", odfmt[128] = "";
	if (otfmt[0] == '\0') {
		if (!::GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_STIMEFORMAT, otfmt, sizeof(otfmt)))
			strcpy_s(otfmt, sizeof(otfmt), "HH:mm:ss");
		::GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_SSHORTDATE, odfmt, sizeof(odfmt));
	}

	char otime[64], odate[64];

	::GetTimeFormat(LOCALE_USER_DEFAULT, 0, &s, otfmt, otime, sizeof(otime));

	::GetDateFormat(LOCALE_USER_DEFAULT, 0, &s, odfmt, odate, sizeof(odate));

	setzero(result, sizeof_result);
	strncpy_s(result, sizeof_result, odate, sizeof_result - 2);
	strcat_s(result, sizeof_result, " ");
	strncat_s(result, sizeof_result, otime, sizeof_result - strlen(result) - 1);
}

//-------------------------------------------------------------------------
//  localized time
//-------------------------------------------------------------------------
void
FANCONTROL::CurrentTimeLocalized(char* result, size_t sizeof_result) {
	SYSTEMTIME s;
	::GetLocalTime(&s);

	static char otfmt[64] = "";
	if (otfmt[0] == '\0') {
		if (!::GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_STIMEFORMAT, otfmt, sizeof(otfmt)))
			strcpy_s(otfmt, sizeof(otfmt), "HH:mm:ss");
	}

	char otime[64];
	// char odfmt[128], odate[64];

	::GetTimeFormat(LOCALE_USER_DEFAULT, 0, &s, otfmt, otime, sizeof(otime));

	// ::GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_SSHORTDATE, odfmt, sizeof(odfmt));

	// ::GetDateFormat(LOCALE_USER_DEFAULT, 0,	&s, odfmt, odate, sizeof(odate));

	setzero(result, sizeof_result);
	// strncpy_s(result,sizeof_result, odate, sizeof_result-2);
	// strcat_s(result,sizeof_result, " ");
	strncat_s(result, sizeof_result, otime, sizeof_result - 1);
	// strncat_s(result,sizeof_result, otime, sizeof_result-strlen(result)-1);
}

//-------------------------------------------------------------------------
//  
//-------------------------------------------------------------------------
bool
FANCONTROL::IsMinimized(void) const {
	WINDOWPLACEMENT wp = NULLSTRUCT;

	::GetWindowPlacement(this->hwndDialog, &wp);

	return wp.showCmd == SW_SHOWMINIMIZED;
}

//-------------------------------------------------------------------------
//  show trace output in lower window part
//-------------------------------------------------------------------------
void
FANCONTROL::Trace(const char* text) {
	char datebuf[128] = "", line[512] = "";

	this->CurrentDateTimeLocalized(datebuf, sizeof(datebuf));

	if (strlen(text))
		sprintf_s(line, sizeof(line), "[%s] %s\r\n", datebuf, text);	// probably acpi reading conflict
	else
		strcpy_s(line, sizeof(line), "\r\n");

	// write logfile
	if (this->Log2File == 1) {
		FILE* flog;
		errno_t errflog = fopen_s(&flog, "TPFanControl.log", "ab");
		if (!errflog) {
			//TODO: fwrite_s
			fwrite(line, strlen(line), 1, flog);
			fclose(flog);
		}
	}

	// record into the in-memory tail (any thread). The control itself is only
	// touched by FlushLogToControl on the UI thread: the panel starts collapsed
	// (lines must survive until it opens), and a cross-thread SendMessage from
	// the EC worker would deadlock against the UI thread's non-pumping
	// WaitForSingleObject(hThread) exit paths until their timeout expires.
	::EnterCriticalSection(&this->m_logLock);
	strcpy_s(this->m_logBuf[this->m_logHead], sizeof(this->m_logBuf[0]), line);
	this->m_logHead = (this->m_logHead + 1) % LOGBUF_LINES;
	if (this->m_logCount < LOGBUF_LINES)
		this->m_logCount++;
	this->m_logTotal++;
	::LeaveCriticalSection(&this->m_logLock);

	// UI thread: surface it immediately; worker-thread lines reach the panel
	// via the 500ms timer's FlushLogToControl call instead
	if (this->hwndDialog &&
			::GetCurrentThreadId() == ::GetWindowThreadProcessId(this->hwndDialog, NULL))
		this->FlushLogToControl();
}

//-------------------------------------------------------------------------
//  push pending log-tail lines into the (visible) log edit. Appends in place
//  via EM_REPLACESEL - repaints only the new line, no flicker, no O(n) text
//  shuffling - and rebuilds from the whole ring when the panel just opened
//  or more lines arrived than the ring holds. UI thread only.
//-------------------------------------------------------------------------
void
FANCONTROL::FlushLogToControl() {
	HWND hLog = ::GetDlgItem(this->hwndDialog, 9200);
	if (!hLog || !::IsWindowVisible(hLog))
		return;

	if (this->m_logTotal == this->m_logShown)   // fast path: nothing new
		return;

	long total, pending;
	int  count, start;
	::EnterCriticalSection(&this->m_logLock);
	total = this->m_logTotal;
	count = this->m_logCount;
	pending = total - this->m_logShown;
	bool rebuild = (pending >= (long)count);   // panel just opened / ring lapped
	if (rebuild)
		pending = count;
	start = (this->m_logHead - (int)pending + LOGBUF_LINES) % LOGBUF_LINES;
	::LeaveCriticalSection(&this->m_logLock);

	bool batch = rebuild || pending > 1;
	if (batch)
		::SendMessage(hLog, WM_SETREDRAW, FALSE, 0);
	if (rebuild)
		::SetWindowTextA(hLog, "");

	for (long i = 0; i < pending; i++) {
		char l[512];
		::EnterCriticalSection(&this->m_logLock);
		strcpy_s(l, sizeof(l), this->m_logBuf[(start + (int)i) % LOGBUF_LINES]);
		::LeaveCriticalSection(&this->m_logLock);
		int len = ::GetWindowTextLength(hLog);
		::SendMessage(hLog, EM_SETSEL, len, len);
		::SendMessage(hLog, EM_REPLACESEL, FALSE, (LPARAM)l);
	}

	// keep at most 100 lines, deleting from the top only once the cap is hit
	// (the trailing \r\n makes EM_GETLINECOUNT report one extra, empty line)
	int lines = (int)::SendMessage(hLog, EM_GETLINECOUNT, 0, 0);
	if (lines > LOGBUF_LINES + 1) {
		int cut = (int)::SendMessage(hLog, EM_LINEINDEX, lines - (LOGBUF_LINES + 1), 0);
		if (cut > 0) {
			if (!batch) {
				::SendMessage(hLog, WM_SETREDRAW, FALSE, 0);
				batch = true;
			}
			::SendMessage(hLog, EM_SETSEL, 0, cut);
			::SendMessage(hLog, EM_REPLACESEL, FALSE, (LPARAM)"");
			int len = ::GetWindowTextLength(hLog);
			::SendMessage(hLog, EM_SETSEL, len, len);
		}
	}

	if (batch) {
		::SendMessage(hLog, WM_SETREDRAW, TRUE, 0);
		::InvalidateRect(hLog, NULL, FALSE);
	}
	::SendMessage(hLog, EM_SCROLLCARET, 0, 0);   // ES_AUTOVSCROLL keeps the tail in view

	this->m_logShown = total;
}

void
FANCONTROL::Tracecsv(const char* text) {
	char datebuf[128] = "", line[512] = "";

	this->CurrentTimeLocalized(datebuf, sizeof(datebuf));

	if (strlen(text))
		sprintf_s(line, sizeof(line), "%s; %s\r\n", datebuf, text);	// probably acpi reading conflict
	else
		strcpy_s(line, sizeof(line), "\r\n");

	// write logfile
	if (this->Log2csv == 1) {
		FILE* flogcsv;
		errno_t errflogcsv = fopen_s(&flogcsv, "TPFanControl_csv.txt", "ab");
		if (!errflogcsv) { 
			fwrite(line, strlen_s(line, sizeof(line)), 1, flogcsv); 
			fclose(flogcsv); 
		}
	}
}

void
FANCONTROL::Tracecsvod(const char* text) {
	char line[512] = "";

	if (strlen(text))
		sprintf_s(line, sizeof(line), "%s\r\n", text);	// probably acpi reading conflict
	else
		strcpy_s(line, sizeof(line), "\r\n");

	// write logfile
	if (this->Log2csv == 1) {
		FILE* flogcsv;
		errno_t errflogcsv = fopen_s(&flogcsv, "TPFanControl_csv.txt", "ab");
		if (!errflogcsv) { 
			fwrite(line, strlen(line), 1, flogcsv); 
			fclose(flogcsv); 
		}
	}
}

//-------------------------------------------------------------------------
//  create a thread
//-------------------------------------------------------------------------
HANDLE
FANCONTROL::CreateThread(int(_stdcall* fnct)(ULONG), ULONG p) {
	LPTHREAD_START_ROUTINE thread = (LPTHREAD_START_ROUTINE)fnct;
	DWORD tid;
	HANDLE hThread;
	// 256 KB stack: the work thread calls Trace(), which uses a large local
	// buffer; the old 32 KB stack left little headroom.
	hThread = ::CreateThread(NULL, 256 * 1024, thread, (void*)p, 0, &tid);
	return hThread;
}
