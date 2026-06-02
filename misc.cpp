
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
//  write selected options back to the ini, preserving comments and order
//-------------------------------------------------------------------------
void
FANCONTROL::SaveConfig(const char* configfile)
{
	struct KV { const char* key; int val; };
	KV items[] = {
		{ "StartMinimized", this->StartMinimized },
		{ "StayOnTop",      this->StayOnTop },
		{ "ShowTempIcon",   this->ShowTempIcon },
		{ "ShowTempHex",    this->ShowTempHex },
		{ "ShowLog",        this->ShowLog },
		{ "DarkMode",       this->DarkMode },
		{ "NoBallons",      this->NoBallons },
		{ "Log2File",       this->Log2File },
		{ "Log2csv",        this->Log2csv },
		{ "Cycle",          this->Cycle },
		{ "ShowGraph",      this->ShowGraph },
		{ "IconColorFan",   this->IconColorFan },
		{ "ShowBiasedTemps",this->ShowBiasedTemps },
		{ "Lev64Norm",      this->Lev64Norm },
		{ "NoExtSensor",    this->NoExtSensor },
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

	// replace the original with the rewritten copy
	if (remove(configfile) == 0) {
		if (rename(tmpname, configfile) != 0)
			this->Trace("SaveConfig: rename of temp ini failed");
		else
			this->Trace("Settings saved to TPFanControl.ini");
	}
	else {
		this->Trace("SaveConfig: could not replace ini");
		remove(tmpname);
	}
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
				sscanf_s(buf + 6, "%d %d %d %d", &this->SmartLevels[lcnt1].temp, &this->SmartLevels[lcnt1].fan, &this->SmartLevels[lcnt1].hystUp, &this->SmartLevels[lcnt1].hystDown);
				sscanf_s(buf + 6, "%d %d %d %d", &this->SmartLevels1[lcnt1].temp1, &this->SmartLevels1[lcnt1].fan1, &this->SmartLevels1[lcnt1].hystUp1, &this->SmartLevels1[lcnt1].hystDown1);
				lcnt1++;
				continue;
			}

			if (_strnicmp(buf, "level2=", 7) == 0) {
				sscanf_s(buf + 7, "%d %d %d %d", &this->SmartLevels2[lcnt2].temp2, &this->SmartLevels2[lcnt2].fan2, &this->SmartLevels2[lcnt2].hystUp2, &this->SmartLevels2[lcnt2].hystDown2);
				lcnt2++;
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
				this->DarkMode = atoi(buf + 9);
				continue;
			}

			if (_strnicmp(buf, "ShowGraph=", 10) == 0) {
				this->ShowGraph = atoi(buf + 10);
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

		if (this->StayOnTop)
			this->hwndDialog = ::CreateDialogParam(hinstapp,
				MAKEINTRESOURCE(9000),
				HWND_DESKTOP,
				(DLGPROC)BaseDlgProc,
				(LPARAM)this);

		else
			this->hwndDialog = ::CreateDialogParam(hinstapp,
				MAKEINTRESOURCE(9002),
				HWND_DESKTOP,
				(DLGPROC)BaseDlgProc,
				(LPARAM)this);

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
				"# New-style temperature tray icon (0/1)\r\n"
				"ShowTempIcon=1\r\n"
				"\r\n"
				"# GUI options (also toggleable via the in-window checkboxes)\r\n"
				"ShowTempHex=0\r\n"
				"ShowLog=1\r\n"
				"DarkMode=0\r\n"
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
				"ManModeExit=80\r\n",
				fnew);
			fclose(fnew);
			this->Trace("TPFanControl.ini not found - created a default one");
		}
		else {
			this->Trace("TPFanControl.ini missing, default values:");
		}
	}

	HANDLE hLockS = CreateMutex(NULL, FALSE, "TPFanControlMutex01");

	if (hLockS == NULL) Runs_as_service = true;
	if (WAIT_OBJECT_0 != WaitForSingleObject(hLockS, 0))
		Runs_as_service = true;

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
			sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d° F->", i > 0 ? ", " : "", this->SmartLevels[i].temp);
			if (this->SmartLevels[i].fan != 0x80)
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels[i].fan);
			else
				strcat_s(buf, sizeof(buf), "0x80");
		}
	}
	else {
		strcpy_s(buf, sizeof(buf), "  Levels= ");
		for (i = 0; this->SmartLevels[i].temp != -1; i++) {
			sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d° C -> ", i > 0 ? ",  " : "", this->SmartLevels[i].temp);
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
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d° F->", i > 0 ? ", " : "", this->SmartLevels2[i].temp2);
				if (this->SmartLevels2[i].fan2 != 0x80)
					sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels2[i].fan2);
				else
					strcat_s(buf, sizeof(buf), "0x80");
			}
		}
		else {
			strcpy_s(buf, sizeof(buf), "  Levels2= ");
			for (i = 0; this->SmartLevels2[i].temp2 != -1; i++) {
				sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d° C -> ", i > 0 ? ",  " : "", this->SmartLevels2[i].temp2);
				if (this->SmartLevels2[i].fan2 != 0x80)
					sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", this->SmartLevels2[i].fan2);
				else
					strcat_s(buf, sizeof(buf), "0x80");
			}
		}
		this->Trace(buf);
	}

	if (Fahrenheit) {
		sprintf_s(buf, sizeof(buf), "  SensorOffset1-12= %d %d %d %d %d %d %d %d %d %d %d %d ° F",
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
		sprintf_s(buf, sizeof(buf), "  SensorOffset1-12= %d %d %d %d %d %d %d %d %d %d %d %d ° C",
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

	//ManModeExit Fahrenheit to Celsius and v.v.

	if (Fahrenheit && (this->ManModeExit == 80))
		this->ManModeExit = (this->ManModeExit * 9 / 5) + 32;

	if (Fahrenheit)
		this->ManModeExitInternal = (this->ManModeExit - 32) * 5 / 9;
	else
		this->ManModeExitInternal = this->ManModeExit;

	sprintf_s(buf, sizeof(buf), "  ManModeExit= %d, SecWinUptime= %d, SecStartDelay= %d", this->ManModeExit, this->SecWinUptime, this->SecStartDelay);
	this->Trace(buf);

	//Offset& Smartlevels Fahrenheit to Celsius
	if (Fahrenheit) {
		for (i = 0; this->SmartLevels[i].temp != -1; i++) { this->SmartLevels[i].temp = (this->SmartLevels[i].temp - 32) * 5 / 9; }
		for (i = 0; this->SmartLevels1[i].temp1 != -1; i++) { this->SmartLevels1[i].temp1 = (this->SmartLevels1[i].temp1 - 32) * 5 / 9; }
		if (this->SmartLevels2[0].temp2 == 0); // Indikator für 2.Profil
		else this->SmartLevels2[0].temp2 = (this->SmartLevels2[0].temp2 - 32) * 5 / 9;
		for (i = 1; this->SmartLevels2[i].temp2 != -1; i++) { this->SmartLevels2[i].temp2 = (this->SmartLevels2[i].temp2 - 32) * 5 / 9; }
		//		for (i= 0; i<15; i++) {SensorOffset[i]= SensorOffset[i] * 5/9;}
		this->IconLevels[0] = (this->IconLevels[0] - 32) * 5 / 9;
		this->IconLevels[1] = (this->IconLevels[1] - 32) * 5 / 9;
		this->IconLevels[2] = (this->IconLevels[2] - 32) * 5 / 9;
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

	return ok;
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
	char trace[16384] = "", datebuf[128] = "", line[512] = "", linecsv[512] = "";

	this->CurrentDateTimeLocalized(datebuf, sizeof(datebuf));

	if (strlen(text))
		sprintf_s(line, sizeof(line), "[%s] %s\r\n", datebuf, text);	// probably acpi reading conflict
	else
		strcpy_s(line, sizeof(line), "\r\n");

	::GetDlgItemText(this->hwndDialog, 9200, trace, sizeof(trace) - strlen(line) - 1);

	strcat_s(trace, sizeof(trace), line);

	// display 100 lines max
	char* p = trace + strlen(trace);
	int linecount = 0;

	while (p >= trace) {
		if (*p == '\n') {
			linecount++;
			if (linecount > 100)
				break;
		}

		p--;
	}

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

	// redisplay log and scroll to bottom
	::SetDlgItemText(this->hwndDialog, 9200, p + 1);
	::SendDlgItemMessage(this->hwndDialog, 9200, EM_SETSEL, strlen(trace) - 2, strlen(trace) - 2);
	::SendDlgItemMessage(this->hwndDialog, 9200, EM_LINESCROLL, 0, 9999);
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