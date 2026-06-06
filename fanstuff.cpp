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
#include "TVicPort.h"
#include "fanlogic.h"   // pure decision logic (unit-tested in tests/fanlogic_tests.cpp)

#define TP_ECOFFSET_FAN         (char)0x2F    // 1 byte (binary xyzz zzz)
#define TP_ECOFFSET_FANSPEED    (char)0x84    // 16 bit word, lo/hi byte
#define TP_ECOFFSET_TEMP0       (char)0x78    // 8 temp sensor bytes from here
#define TP_ECOFFSET_TEMP1       (char)0xC0    // 4 temp sensor bytes from here
#define TP_ECOFFSET_FAN_SWITCH  (char)0x31
#define TP_ECVALUE_SELFAN1      (char)0x0000
#define TP_ECVALUE_SELFAN2      (char)0x0001

//-------------------------------------------------------------------------
//  log a "Change Mode from <prev>-><cur>" line; nothing if the mode is unchanged
//-------------------------------------------------------------------------
void
FANCONTROL::TraceModeChange() {
	if (this->PreviousMode == this->CurrentMode)
		return;

	const char* from =
		this->PreviousMode == 1 ? "BIOS->" :
		this->PreviousMode == 2 ? "Smart->" :
		this->PreviousMode == 3 ? "Manual->" : "";
	const char* to =
		this->CurrentMode == 1 ? "BIOS, setting fan speed" :
		this->CurrentMode == 2 ? "Smart, recalculate fan speed" :
		this->CurrentMode == 3 ? "Manual, setting fan speed" : "";

	char msg[128];
	sprintf_s(msg, sizeof(msg), "Change Mode from %s%s", from, to);
	this->Trace(msg);
}

//-------------------------------------------------------------------------
//  apply the per-sensor offset to a raw reading (hysteresis-aware).
//  Returns the raw value unchanged when ShowBiasedTemps is off or the
//  reading falls inside the sensor's hyst exclusion window.
//-------------------------------------------------------------------------
int
FANCONTROL::BiasedTemp(int rawTemp, int sensorIndex) const {
	// delegate to the pure, unit-tested implementation
	return fanlogic::biased_temp(rawTemp,
		this->SensorOffset[sensorIndex].offs,
		this->SensorOffset[sensorIndex].hystMin,
		this->SensorOffset[sensorIndex].hystMax,
		this->ShowBiasedTemps != 0);
}

//-------------------------------------------------------------------------
//  switch fan according to settings
//-------------------------------------------------------------------------
int
FANCONTROL::HandleData(void) {
	char obuf2[128] = "",
		templist[256] = "", manlevel[16] = "", title2[128] = "";
	int i, maxtemp, imaxtemp, ok = 0;

	//
	// determine highest temp.
	//

	// build a list of sensors to ignore, separated by "|", e.g. "|XC1|BAT|CPU|"
	char what[16], list[128];
	sprintf_s(list, sizeof(list), "|%s|", this->IgnoreSensors);
	for (i = 0; list[i] != '\0'; i++) {
		if (list[i] == ',')
			list[i] = '|';
	}
	// sensor names are stored uppercase (CPU, GPU, ...), but the ini tells users
	// to enter IgnoreSensors in lower case. Uppercase the list so the match is
	// effectively case-insensitive and "pci,aps" actually ignores PCI/APS.
	_strupr_s(list, sizeof(list));

	maxtemp = 0;
	imaxtemp = 0;
	int senstemp;
	for (i = 0; i < 12; i++) {
		sprintf_s(what, sizeof(what), "|%s|", this->State.SensorName[i]); // name (e.g. "|CPU|") to match against list above

		if (this->State.Sensors[i] != 0x80 && this->State.Sensors[i] != 0x00 && strstr(list, what) == 0) {
			senstemp = this->BiasedTemp(this->State.Sensors[i], i);

			if (senstemp < 128) {
				maxtemp = __max(senstemp, maxtemp);
				if (maxtemp <= senstemp)
					imaxtemp = i;
			}
		}
	}

	this->MaxTemp = maxtemp;
	this->iMaxTemp = imaxtemp;

	// record this reading and refresh the history sparkline (owner-draw static 8120)
	this->PushTempSample(this->MaxTemp);
	{
		HWND hSpark = ::GetDlgItem(this->hwndDialog, 8120);
		if (hSpark) ::InvalidateRect(hSpark, NULL, FALSE);
	}

	//
	// update dialog elements
	//

	// title string (for minimized window)
	if (Fahrenheit)
		sprintf_s(title2, sizeof(title2), "%d° F", this->MaxTemp * 9 / 5 + 32);
	else
		sprintf_s(title2, sizeof(title2), "%d° C", this->MaxTemp);

	// display fan state
	int fanctrl = this->State.FanCtrl;
	fanctrl2 = fanctrl;

	if (this->SlimDialog == 1) {
		sprintf_s(obuf2, sizeof(obuf2), "Fan %d ", fanctrl);
		if (fanctrl & 0x80) {
			if (!(SlimDialog && StayOnTop))
				strcat_s(obuf2, sizeof(obuf2), "(= BIOS)");
			strcat_s(title2, sizeof(title2), " Default Fan");
		}
		else {
			if (!(SlimDialog && StayOnTop))
				sprintf_s(obuf2 + strlen(obuf2), sizeof(obuf2) - strlen(obuf2), " Non Bios");
			sprintf_s(title2 + strlen(title2), sizeof(title2) - strlen(title2), " Fan %d (%s)",	fanctrl & 0x3F,	this->CurrentModeFromDialog() == 2 ? "Smart" : "Fixed");
		}
	}
	else {
		sprintf_s(obuf2, sizeof(obuf2), "0x%02x (", fanctrl);
		if (fanctrl & 0x80) {
			strcat_s(obuf2, sizeof(obuf2), "BIOS Controlled)");
			strcat_s(title2, sizeof(title2), " Default Fan");
		}
		else {
			sprintf_s(obuf2 + strlen(obuf2), sizeof(obuf2) - strlen(obuf2), "Fan Level %d, Non Bios)", fanctrl & 0x3F);
			sprintf_s(title2 + strlen(title2), sizeof(title2) - strlen(title2), " Fan %d (%s)",	fanctrl & 0x3F,	this->CurrentModeFromDialog() == 2 ? "Smart" : "Fixed");
		}
	}

	::SetDlgItemText(this->hwndDialog, 8100, obuf2);

	strcpy_s(this->Title2, sizeof(this->Title2), title2);

	// display fan speeds
	this->lastfan1speed = this->fan1speed;
	this->fan1speed = ((unsigned char)this->State.Fan1SpeedHi << 8) | (unsigned char)this->State.Fan1SpeedLo;
	if (this->fan1speed > 0x1fff)
		fan1speed = lastfan1speed;

	this->lastfan2speed = this->fan2speed;
	this->fan2speed = ((unsigned char)this->State.Fan2SpeedHi << 8) | (unsigned char)this->State.Fan2SpeedLo;
	if (this->fan2speed > 0x1fff)
		fan2speed = lastfan2speed;

	// also surface RPM in the tray tooltip / minimized window title (Title2)
	sprintf_s(this->Title2 + strlen(this->Title2), sizeof(this->Title2) - strlen(this->Title2),
		" %d/%d rpm", this->fan1speed, this->fan2speed);

	// compose the richer multi-line tray tooltip (mode / max temp / fan / active
	// profile, plus game + EC-error flags). Title2 stays the single-line title.
	{
		int dT = Fahrenheit ? (this->MaxTemp * 9 / 5 + 32) : this->MaxTemp;
		const char* unit = Fahrenheit ? "F" : "C";

		char modeStr[40];
		switch (this->CurrentModeFromDialog()) {
		case 1:  strcpy_s(modeStr, sizeof(modeStr), "BIOS mode"); break;
		case 3:  strcpy_s(modeStr, sizeof(modeStr), "Manual mode"); break;
		case 2:
			if (this->SmartLevels2[0].temp2 != 0)
				sprintf_s(modeStr, sizeof(modeStr), "Smart mode (Profile %d)", this->IndSmartLevel + 1);
			else
				strcpy_s(modeStr, sizeof(modeStr), "Smart mode");
			break;
		default: strcpy_s(modeStr, sizeof(modeStr), "Read-only (no EC control)"); break;
		}

		char fanStr[16];
		if (fanctrl & 0x80)              strcpy_s(fanStr, sizeof(fanStr), "BIOS");
		else if ((fanctrl & 0x7f) == 64) strcpy_s(fanStr, sizeof(fanStr), "max");
		else                             sprintf_s(fanStr, sizeof(fanStr), "%d", fanctrl & 0x7f);

		sprintf_s(this->TrayTip, sizeof(this->TrayTip),
			"%s\r\nMax %d\xb0%s   Fan %s   %d/%d rpm",
			modeStr, dT, unit, fanStr, this->fan1speed, this->fan2speed);

		char extra[64] = "";
		if (this->m_failsafeTripped)
			strcpy_s(extra, sizeof(extra), "FAIL-SAFE: fan forced max");
		if (this->m_driversHidden) {
			if (extra[0]) strcat_s(extra, sizeof(extra), "   ");
			strcat_s(extra, sizeof(extra), "Game mode");
		}
		if (this->m_ecErrorsTotal > 0) {
			if (extra[0]) strcat_s(extra, sizeof(extra), "   ");
			sprintf_s(extra + strlen(extra), sizeof(extra) - strlen(extra),
				"EC errors: %d", this->m_ecErrorsTotal);
		}
		if (extra[0]) {
			strcat_s(this->TrayTip, sizeof(this->TrayTip), "\r\n");
			strcat_s(this->TrayTip, sizeof(this->TrayTip), extra);
		}
	}

	sprintf_s(obuf2, sizeof(obuf2), "%d RPM", this->fan1speed);
	::SetDlgItemText(this->hwndDialog, 8102, obuf2);

	sprintf_s(obuf2, sizeof(obuf2), "%d RPM", this->fan2speed);
	::SetDlgItemText(this->hwndDialog, 8104, obuf2);

	// display temperature list
	if (Fahrenheit)
		sprintf_s(obuf2, sizeof(obuf2), "%d° F", this->MaxTemp * 9 / 5 + 32);
	else
		sprintf_s(obuf2, sizeof(obuf2), "%d° C", this->MaxTemp);

	::SetDlgItemText(this->hwndDialog, 8103, obuf2);

	this->UpdateTempList();

	// keep the manual box/slider enabled state in sync with the current mode
	this->UpdateManualControlsEnabled();

	this->icontemp = this->BiasedTemp(this->State.Sensors[iMaxTemp], iMaxTemp);

	// compact single line status (combined). Use the same <128 validity test
	// and BiasedTemp() in both unit branches so the line matches MaxTemp/the list.
	strcpy_s(templist, sizeof(templist), "");

	for (i = 0; i < 12; i++) {
		int raw = this->State.Sensors[i];
		const char* sep = Fahrenheit ? "%d;" : "%d; ";
		if (raw >= 128 || raw == 0) {
			sprintf_s(templist + strlen(templist), sizeof(templist) - strlen(templist), sep, 0);
			continue;
		}
		int t = this->BiasedTemp(raw, i);
		if (Fahrenheit)
			t = t * 9 / 5 + 32;
		sprintf_s(templist + strlen(templist), sizeof(templist) - strlen(templist), sep, t);
	}

	if (templist[0])
		templist[strlen(templist) - 1] = '\0';   // drop trailing separator (guard empty)

	if (Fahrenheit)
		sprintf_s(CurrentStatus, sizeof(CurrentStatus), "Fan: 0x%02x / Switch: %d° F (%s)", State.FanCtrl, MaxTemp * 9 / 5 + 32, templist);
	else
		sprintf_s(CurrentStatus, sizeof(CurrentStatus), "Fan: 0x%02x / Switch: %d° C (%s)", State.FanCtrl, MaxTemp,	templist);

	// display fan speed

	if (fan1speed > 0x1fff)
		fan1speed = lastfan1speed;
	if (fan2speed > 0x1fff)
		fan2speed = lastfan2speed;
	sprintf_s(obuf2, sizeof(obuf2), "%d/%d", this->fan1speed, this->fan2speed);

	sprintf_s(CurrentStatuscsv, sizeof(CurrentStatuscsv), "%s %s; %d; %d; ", templist, obuf2, State.FanCtrl, MaxTemp);

	::SetDlgItemText(this->hwndDialog, 8112, this->CurrentStatus);

	//
	// handle fan control according to mode
	//

	this->CurrentModeFromDialog();
	this->ShowAllFromDialog();

	::SetDlgItemText(this->hwndDialog, 8115,
		(this->CurrentMode == 2 || this->CurrentMode == 3) ? "TPControlFAN = On" : "TPControlFAN = OFF");

	// thermal fail-safe (Smart/Manual only): if the max temperature reaches
	// FailsafeTemp, force full fan speed (0x40 = max airflow, not merely level 7)
	// regardless of the curve/manual level, and hold it until ~3 C below
	// (hysteresis). Guards against a curve or a stuck manual level that would
	// otherwise let the machine overheat. Evaluate the trip BEFORE the per-mode
	// control below so that mode can defer to it this cycle - otherwise Smart/
	// Manual writes a lower level and the fail-safe immediately overrides it back
	// to max every poll, doubling EC writes and log spam during an overheat.
	if (this->FailsafeTemp > 0 && this->ActiveMode &&
		(this->CurrentMode == 2 || this->CurrentMode == 3)) {
		if (!this->m_failsafeTripped && this->MaxTemp >= this->FailsafeTemp)
			this->m_failsafeTripped = true;
		else if (this->m_failsafeTripped && this->MaxTemp <= this->FailsafeTemp - 3)
			this->m_failsafeTripped = false;
	}
	else {
		this->m_failsafeTripped = false;   // disabled, or mode left Smart/Manual
	}

	switch (this->CurrentMode) {

	case 1: // BIOS
		this->TraceModeChange();

		if (this->State.FanCtrl != 0x080)
			ok = this->SetFan("BIOS", 0x80);
		break;

	case 2: // Smart
		if (!this->m_failsafeTripped)   // fail-safe holds the fan; skip curve control
			this->SmartControl();       // (logs its own mode-change transition)
		else
			this->TraceModeChange();    // still log a transition while the fail-safe holds
		break;

	case 3: // Manual
		this->TraceModeChange();

		::GetDlgItemText(this->hwndDialog, 8310, manlevel, sizeof(manlevel));

		if (!this->m_failsafeTripped &&
			isdigit(manlevel[0]) && atoi(manlevel) >= 0 && atoi(manlevel) <= 255) {
			if (this->State.FanCtrl != atoi(manlevel))
				ok = this->SetFan("Manual", atoi(manlevel));
			else
				ok = true;
		}

		break;
	}

	this->PreviousMode = this->CurrentMode;

	// apply the fail-safe override (full speed) once per cycle while tripped
	if (this->m_failsafeTripped && this->State.FanCtrl != 0x40)
		ok = this->SetFan("Fail-safe: max temp reached, forcing full fan speed", 0x40);

	if (this->CurrentMode == 3 && this->MaxTemp > this->ManModeExitInternal)
		this->CurrentMode = 2;

	return ok;
}

//-------------------------------------------------------------------------
//  smart fan control depending on temperature
//-------------------------------------------------------------------------
void
FANCONTROL::SmartControl(void) {
	// CurrentMode is Smart (2) here; log a BIOS->Smart / Manual->Smart transition
	this->TraceModeChange();

	// Copy the Smart table into the hardware-independent view, then let the pure,
	// unit-tested decision logic choose the level. This mirrors the previous inline
	// ramp-up / ramp-down + hysteresis algorithm (see tests/fanlogic_tests.cpp).
	//   Smart-table rows: temp  fan  hystUp  hystDown   (a temp == -1 row terminates)
	const int n = (int)ARRAYMAX(this->SmartLevels);
	fanlogic::FanLevel levels[ARRAYMAX(this->SmartLevels)];
	for (int li = 0; li < n; li++) {
		levels[li].temp     = this->SmartLevels[li].temp;
		levels[li].fan      = this->SmartLevels[li].fan;
		levels[li].hystUp   = this->SmartLevels[li].hystUp;
		levels[li].hystDown = this->SmartLevels[li].hystDown;
	}

	int newfan = fanlogic::smart_decide(this->MaxTemp, this->State.FanCtrl,
		this->Lev64Norm != 0, this->PreviousMode, levels, n, this->LastSmartLevel);

	if (newfan != -1)
		this->SetFan("Smart", newfan);
}

//-------------------------------------------------------------------------
//  switch the active Smart table to profile 1 or 2.  Copies ALL fields
//  (temp, fan, hystUp, hystDown) - not just temp/fan - and resets the
//  hysteresis anchor so the new curve is not held by the old profile's state.
//-------------------------------------------------------------------------
void
FANCONTROL::ActivateSmartProfile(int profile) {
	const int n = (int)ARRAYMAX(this->SmartLevels);
	for (int i = 0; i < n; i++) {
		if (profile == 2) {
			this->SmartLevels[i].temp     = this->SmartLevels2[i].temp2;
			this->SmartLevels[i].fan      = this->SmartLevels2[i].fan2;
			this->SmartLevels[i].hystUp   = this->SmartLevels2[i].hystUp2;
			this->SmartLevels[i].hystDown = this->SmartLevels2[i].hystDown2;
		}
		else {
			this->SmartLevels[i].temp     = this->SmartLevels1[i].temp1;
			this->SmartLevels[i].fan      = this->SmartLevels1[i].fan1;
			this->SmartLevels[i].hystUp   = this->SmartLevels1[i].hystUp1;
			this->SmartLevels[i].hystDown = this->SmartLevels1[i].hystDown1;
		}
	}
	// the hysteresis anchor index refers to the previous curve; invalidate it
	this->LastSmartLevel = -1;
}

//-------------------------------------------------------------------------
//  set fan state via EC
//-------------------------------------------------------------------------
int
FANCONTROL::SetFan(const char* source, int fanctrl, bool final) {
	int ok = 0;
	int fan1_ok = 0;
	int fan2_ok = 0;
	char obuf[256] = "", obuf2[256], datebuf[128];

	if (this->FanBeepFreq && this->FanBeepDura)
		::Beep(this->FanBeepFreq, this->FanBeepDura);

	this->CurrentDateTimeLocalized(datebuf, sizeof(datebuf));

	sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "%s: Set fan control to 0x%02x, ", source, fanctrl);
	if (this->IndSmartLevel == 1 && this->SmartLevels2[0].temp2 != 0 && strcmp(source, "Smart") == 0)
		sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "Mode 2, ");
	if (this->IndSmartLevel == 0 && this->SmartLevels2[0].temp2 != 0 && strcmp(source, "Smart") == 0)
		sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "Mode 1, ");
	sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "Result: ");

	if (this->ActiveMode && !this->FinalSeen) {
		if (!this->LockECAccess()) return false;

		// Verify-and-retry: keep the 100ms settle waits (the EC needs them for a
		// correct read-back) but cap retries/backoff so a failing EC can't freeze
		// the UI thread for ~3.5s. Worst case now ~1.5s.
		for (int i = 0; i < 3; i++) {
			// set new fan level; AND every write so a port failure is not masked
			bool write_ok = true;
			write_ok &= this->WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN1);
			write_ok &= this->WriteByteToEC(TP_ECOFFSET_FAN, fanctrl);

			::Sleep(100);

			write_ok &= this->WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN2);
			write_ok &= this->WriteByteToEC(TP_ECOFFSET_FAN, fanctrl);

			::Sleep(100);

			// verify completion of fan2
			fan2_ok = this->ReadByteFromEC(TP_ECOFFSET_FAN, &this->State.FanCtrl);

			::Sleep(100);

			// verify completion of fan1
			write_ok &= this->WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN1);

			::Sleep(100);

			fan1_ok = this->ReadByteFromEC(TP_ECOFFSET_FAN, &this->State.FanCtrl);

			if (write_ok && fan1_ok && fan2_ok) {
				sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "[i=%d] ", i);
				break;
			}

			if (!write_ok)
				sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "[wr-fail i=%d] ", i);

			if (i < 2) ::Sleep(150);   // brief backoff before retry (skipped after last attempt)
		}

		this->FreeECAccess();

		if (this->State.FanCtrl == fanctrl) {
			sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "OK");
			ok = true;
			if (final)
				this->FinalSeen = true;    // prevent further changes when setting final mode

		}
		else {
			sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "FAILED!!");

			/*			::Beep(880, 300);
						::Sleep(200);
						::Beep(880, 300);
						::Sleep(200);
						::Beep(880, 300);
			*/

			ok = false;
		}
	}
	else {
		sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "IGNORED!(passive mode");
	}

	// display result
	sprintf_s(obuf2, sizeof(obuf2), "%s   (%s)", obuf, datebuf);

	::SetDlgItemText(this->hwndDialog, 8113, obuf2);

	this->Trace(this->CurrentStatus);
	this->Trace(obuf);

	if (!final)
		::PostMessage(this->hwndDialog, WM__GETDATA, 0, 0);

	return ok;
}

BOOL
FANCONTROL::SetHdw(const char* source, int hdwctrl, int HdwOffset, int AnyWayBit) {
	int ok = 0;
	char obuf[256] = "", obuf2[256], datebuf[128];
	unsigned char newhdwctrl = 0;   // guard: ReadByteFromEC leaves *pdata untouched on failure

	if (!this->LockECAccess()) return false;

	this->CurrentDateTimeLocalized(datebuf, sizeof(datebuf));

	for (int i = 0; i < 3; i++) {   // capped retries/backoff (see SetFan)
		ok = this->ReadByteFromEC(HdwOffset, &newhdwctrl);
		if (newhdwctrl & hdwctrl) {
			ok = this->WriteByteToEC(HdwOffset, (newhdwctrl - hdwctrl) | AnyWayBit);
			hdwctrl = newhdwctrl - hdwctrl;
		}
		else {
			ok = this->WriteByteToEC(HdwOffset, (newhdwctrl + hdwctrl) | AnyWayBit);
			hdwctrl = newhdwctrl + hdwctrl;
		}

		ok = this->ReadByteFromEC(HdwOffset, &newhdwctrl);

		if (hdwctrl == newhdwctrl)
			break;

		if (i < 2) ::Sleep(150);
	}

	sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "%s: Set EC register 0x%02x to %d, ", source, HdwOffset, hdwctrl);
	sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "Result: ");

	if (hdwctrl == newhdwctrl) {
		sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "OK");
		ok = true;
	}
	else {
		sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "COULD NOT SET HARDWARE STATE!!!!");
		ok = false;
	}


	// display result
	sprintf_s(obuf2, sizeof(obuf2), "%s   (%s)", obuf, datebuf);

	::SetDlgItemText(this->hwndDialog, 8113, obuf2);

	this->Trace(obuf);

	this->FreeECAccess();

	return ok;
}

//-------------------------------------------------------------------------
//  check two EC status samples for accpetable equivalence
//-------------------------------------------------------------------------
bool
FANCONTROL::SampleMatch(FCSTATE* smp1, FCSTATE* smp2) {
	
	// match for identical fanctrl settings
	if (smp1->FanCtrl != smp2->FanCtrl) return false;

	// insert any further match criteria here:
	// -----------------------
	//
	// if (......) ......
	//
	// -----------------------

	return TRUE;
}

//-------------------------------------------------------------------------
//  lock access to the EC controller
//-------------------------------------------------------------------------
bool
FANCONTROL::LockECAccess() {
	int numTries = 10, sleepTicks = 100;

	int ok_ecaccess = false;
	for (int i = 0; i < numTries; i++) {
		if (ok_ecaccess = this->EcAccess.Lock(100))	return TRUE;
		if (i < numTries - 1) ::Sleep(sleepTicks);
	}

	this->Trace("Could not acquire mutex to read EC status");
	return false;
}

//-------------------------------------------------------------------------
//  relinquisch any lock access to the EC controller
//-------------------------------------------------------------------------
void
FANCONTROL::FreeECAccess() {
	this->EcAccess.Unlock();
}

//-------------------------------------------------------------------------
//  read fan and temperatures from embedded controller
//-------------------------------------------------------------------------
bool
FANCONTROL::ReadEcStatus(FCSTATE* pfcstate) {
	int numTries = 10, sleepTicks = 200;

	FCSTATE sample1, sample2;

	if (!this->LockECAccess()) return false;

	// reading from the EC seems to yield erratic results at times (probably
	// due to collision with other drivers reading from the port).  So try
	// up to ten times to read two samples which look ok and have matching
	// values, using the above match function

	for (int i = 0; i < numTries; i++) {
		if (this->ReadEcRaw(&sample1) && this->ReadEcRaw(&sample2) && this->SampleMatch(&sample1, &sample2)) {
			memcpy(pfcstate, &sample2, sizeof(*pfcstate));
			this->FreeECAccess();
			return TRUE;
		}
		if (i < numTries - 1) ::Sleep(sleepTicks);
	}

	this->FreeECAccess();

	this->Trace("failed to read reliable status values from EC");

	return false;
}

//-------------------------------------------------------------------------
//  read fan and temperatures from embedded controller
//-------------------------------------------------------------------------
bool
FANCONTROL::ReadEcRaw(FCSTATE* pfcstate) {

	// At any point in time, a failure in "ReadByteFromEC" or "WriteByteToEC"
	// is a reason to abort the entire process and return "false" to indicate failure.
	// This process will be retried by the caller of this routine.

	pfcstate->FanCtrl = 0xFF;

	// Status Register
	if (!ReadByteFromEC(TP_ECOFFSET_FAN, &pfcstate->FanCtrl)) {
		this->Trace("failed to read status register from EC");
		return false;
	}

	//
	// Fan 2 next
	//

	// Select 
	if (!WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN2)) {
		this->Trace("failed to select Fan 2 in EC");
		return false;
	}

	// Lo
	if (!ReadByteFromEC(TP_ECOFFSET_FANSPEED, &pfcstate->Fan2SpeedLo)) {
		this->Trace("failed to read FanSpeedLowByte 2 from EC");
		return false;
	}
	
	// Hi
	if (!ReadByteFromEC(TP_ECOFFSET_FANSPEED + 1, &pfcstate->Fan2SpeedHi)) {
		this->Trace("failed to read FanSpeedHighByte 2 from EC");
		return false;
	}

	//
	// Fan 1 last
	//
	if (!WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN1)) {
		this->Trace("failed to select Fan 1 in EC");
		return false;
	}

	// Lo
	if (!ReadByteFromEC(TP_ECOFFSET_FANSPEED, &pfcstate->Fan1SpeedLo)) {
		this->Trace("failed to read FanSpeedLowByte 1 from EC");
		return false;
	}
	
	// Hi
	if (!ReadByteFromEC(TP_ECOFFSET_FANSPEED + 1, &pfcstate->Fan1SpeedHi)) {
		this->Trace("failed to read FanSpeedHighByte 1 from EC");
		return false;
	}

	// Get Sensors finally

	int i, idxtemp, ok = TRUE;

	memset(pfcstate->Sensors, 0, sizeof(pfcstate->Sensors));

	if (!this->UseTWR) {
		
		idxtemp = 0;

		for (i = 0; i < 8; i++) {    // temp sensors 0x78 - 0x7f
			pfcstate->SensorAddr[idxtemp] = TP_ECOFFSET_TEMP0 + i;

			pfcstate->SensorName[idxtemp] = this->gSensorNames[idxtemp];

			// store the raw EC reading; bias (if any) is applied later via
			// BiasedTemp() so display and fan decisions stay consistent.
			if (!ReadByteFromEC(TP_ECOFFSET_TEMP0 + i, &pfcstate->Sensors[idxtemp])) {
				this->Trace("failed to read a TEMP0 byte from EC");
				return false;
			}

			idxtemp++;
		}

		for (i = 0; i < 4; i++) {    // temp sensors 0xC0 - 0xC4
			pfcstate->SensorAddr[idxtemp] = TP_ECOFFSET_TEMP1 + i;

			pfcstate->SensorName[idxtemp] = "n/a";

			if (!this->NoExtSensor) {
				pfcstate->SensorName[idxtemp] = this->gSensorNames[idxtemp];

				// store the raw reading; bias applied later via BiasedTemp()
				if (!ReadByteFromEC(TP_ECOFFSET_TEMP1 + i, &pfcstate->Sensors[idxtemp])) {
					this->Trace("failed to read a TEMP1 byte from EC");
					return false;
				}
			}

			idxtemp++;
		}
	}
	else {
		char data = -1;
		char dataOut[16];
		int iOK = false;
		int iTimeout = 100;
		int iTimeoutBuf = 1000;
		int iTime = 0;
		int iTick = 10;
		int iNumTry = 0;

	retry:
		iNumTry++;

		if (iNumTry >= 3) {
			this->Trace("failed to read temps , EC is not ready for TWR");
			return false;
		}

		for (iTime = 0; iTime < iTimeoutBuf; iTime += iTick) {    // wait for ec ready
			data = (char)ReadPort(0x1604) & 0xff;                // or timeout iTimeoutBuf = 1000
			if (!data)                                            // ec is ready: ctrlprt = 0
				break;
			if (data & 0x50)                                    // some unrequested outputis waiting
				ReadPort(0x161f);                                // clear data output
			::Sleep(iTick);
		}

		WritePort(0x1610, 0x20);                            // tell them we want to read
		data = (char)ReadPort(0x1604) & 0xff;
		if (!(data & 0x20))                                    // ec is not ready
			goto retry;

		for (int i = 1; i < 15; i++) {
			WritePort(0x1610 + i, 0x00);
		}

		WritePort(0x161f, 0x00);

		for (iTime = 0; iTime < iTimeoutBuf; iTime++) {            // wait for full buffers to clear
			data = (char)ReadPort(0x1604) & 0xff;                // or timeout iTimeoutBuf = 1000
			if (data == 0x50)
				break;
		}

		if (data != 0x50)
			goto retry;

		for (int i = 0; i < 16; i++) {
			dataOut[i] = (char)ReadPort(0x1610 + i) & 0xff;
		}

		pfcstate->SensorAddr[0] = 0x78;
		pfcstate->SensorName[0] = this->gSensorNames[0];
		pfcstate->Sensors[0] = dataOut[0];

		pfcstate->SensorAddr[1] = 0x79;
		pfcstate->SensorName[1] = this->gSensorNames[1];
		pfcstate->Sensors[1] = dataOut[1];

		pfcstate->SensorAddr[2] = 0x7a;
		pfcstate->SensorName[2] = this->gSensorNames[2];
		pfcstate->Sensors[2] = dataOut[2];

		pfcstate->SensorAddr[3] = 0x7b;
		pfcstate->SensorName[3] = this->gSensorNames[3];
		pfcstate->Sensors[3] = dataOut[3];

		pfcstate->SensorAddr[4] = 0x7c;
		pfcstate->SensorName[4] = this->gSensorNames[4];
		pfcstate->Sensors[4] = dataOut[4];

		pfcstate->SensorAddr[5] = 0x7d;
		pfcstate->SensorName[5] = this->gSensorNames[5];
		pfcstate->Sensors[5] = dataOut[6];

		pfcstate->SensorAddr[6] = 0x7e;
		pfcstate->SensorName[6] = this->gSensorNames[6];
		pfcstate->Sensors[6] = dataOut[8];

		pfcstate->SensorAddr[7] = 0x7f;
		pfcstate->SensorName[7] = this->gSensorNames[7];
		pfcstate->Sensors[7] = dataOut[9];

		pfcstate->SensorAddr[8] = 0xc0;
		pfcstate->SensorName[8] = this->gSensorNames[8];
		pfcstate->Sensors[8] = dataOut[10];

		pfcstate->SensorAddr[9] = 0xc1;
		pfcstate->SensorName[9] = this->gSensorNames[9];
		pfcstate->Sensors[9] = dataOut[11];

		pfcstate->SensorAddr[10] = 0xc2;
		pfcstate->SensorName[10] = this->gSensorNames[10];
		pfcstate->Sensors[10] = dataOut[12];

		pfcstate->SensorAddr[11] = 0xc3;
		pfcstate->SensorName[11] = this->gSensorNames[11];
		pfcstate->Sensors[11] = dataOut[13];
	}

	return ok;
}
