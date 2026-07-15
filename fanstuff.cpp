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
#include <powrprof.h>   // SetSuspendState (emergency hibernate)
#pragma comment(lib, "powrprof.lib")

//-------------------------------------------------------------------------
//  enable SeShutdownPrivilege (held but disabled, even for an admin token)
//  and hibernate. SetSuspendState returns on resume; on failure the machine
//  simply keeps running with the fan already forced to max.
//-------------------------------------------------------------------------
static void EmergencyHibernate() {
	HANDLE hTok = NULL;
	if (::OpenProcessToken(::GetCurrentProcess(),
			TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
		TOKEN_PRIVILEGES tp = {};
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		if (::LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
			::AdjustTokenPrivileges(hTok, FALSE, &tp, 0, NULL, NULL);
		::CloseHandle(hTok);
	}
	::SetSuspendState(TRUE /*hibernate*/, TRUE, FALSE);
}
#include "fanlogic.h"   // pure decision logic (unit-tested in tests/fanlogic_tests.cpp)

#define TP_ECOFFSET_FAN         (char)0x2F    // 1 byte (binary xyzz zzz)
#define TP_ECOFFSET_FANSPEED    (char)0x84    // 16 bit word, lo/hi byte
#define TP_ECOFFSET_TEMP0       (char)0x78    // 8 temp sensor bytes from here
#define TP_ECOFFSET_TEMP1       (char)0xC0    // 4 temp sensor bytes from here
#define TP_ECOFFSET_FAN_SWITCH  (char)0x31
#define TP_ECVALUE_SELFAN1      (char)0x0000
#define TP_ECVALUE_SELFAN2      (char)0x0001

//-------------------------------------------------------------------------
//  Set a dialog control's text only when it differs from the last value we
//  wrote, so an unchanged per-poll readout doesn't trigger a redundant
//  WM_SETTEXT (which invalidates/repaints the control even for identical text).
//-------------------------------------------------------------------------
static void
SetDlgTextIfChanged(HWND dlg, int id, const char* s, char* cache, size_t n) {
	if (strcmp(cache, s) != 0) {
		strcpy_s(cache, n, s);
		::SetDlgItemText(dlg, id, s);
	}
}

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

	// The ignore list (IgnoreSensors) and the sensor names are constant after
	// config load, so the normalized pipe-delimited list and per-sensor "ignored"
	// membership are precomputed once in BuildIgnoreCache() (see m_sensorIgnored)
	// instead of rebuilding sprintf+_strupr_s+strstr on every poll.
	maxtemp = 0;
	imaxtemp = 0;
	int senstemp;
	bool anyValid = false;   // at least one NON-ignored sensor gave a usable reading this poll
	int  safetyMax = 0;      // max RAW temp over ALL trusted sensors (bias- and ignore-list-
	                         // independent); drives the thermal fail-safe / critical guards so a
	                         // bias or an ignored hottest sensor can't mask a real overheat (H-07)
	for (i = 0; i < 12; i++) {
		unsigned char raw = this->State.Sensors[i];
		if (raw == 0x00 || raw == 0x80)
			continue;                          // empty / invalid EC slot
		if (raw < 128 && (int)raw > safetyMax)
			safetyMax = raw;                   // safety backstop sees the raw hardware value
		if (this->m_sensorIgnored[i])
			continue;                          // ignore-list excludes only from the display/curve max
		if (raw < 128)
			anyValid = true;                   // a real reading is driving the curve max this poll
		senstemp = this->BiasedTemp(raw, i);

		// strict compare keeps the FIRST sensor that reached the max, so a later
		// sensor merely tying it does not steal iMaxTemp (and the bold temp-list
		// row) back and forth every poll cycle
		if (senstemp < 128 && senstemp > maxtemp) {
			maxtemp = senstemp;
			imaxtemp = i;
		}
	}
	bool safetyValid = (safetyMax > 0);   // at least one trusted sensor for the backstop

	this->MaxTemp = maxtemp;
	this->iMaxTemp = imaxtemp;

	// C-04: a poll with no usable sensor (all slots invalid or ignored) leaves
	// MaxTemp==0, which the Smart/Manual paths must NOT read as "cold" and use to
	// spin the fan down. Latch a one-shot notice here; the per-mode guards below
	// hold the current level, and the fail-safe override still forces max if tripped.
	if (!anyValid) {
		if (!this->m_noSensorWarned) {
			this->Trace("No usable temperature sensor this poll - holding fan level (not lowering)");
			this->m_noSensorWarned = true;
		}
	}
	else
		this->m_noSensorWarned = false;

	// record this reading and refresh the history sparkline (owner-draw static 8120)
	this->PushTempSample(this->MaxTemp);
	if (this->ShowGraph) {   // skip when the graph is hidden; history is still recorded above
		HWND hSpark = ::GetDlgItem(this->hwndDialog, 8120);
		if (hSpark) ::InvalidateRect(hSpark, NULL, FALSE);
	}

	// Read the mode once for this poll. The radio state can't change mid-function
	// (single-threaded UI, no message pump here), so this->CurrentMode is reused for
	// the title/tooltip text and the per-mode switch below instead of re-querying
	// the buttons ~5x via CurrentModeFromDialog().
	this->CurrentModeFromDialog();

	// The cheap readout fields (8100/8102/8103/8104/8112/8115) are maintained
	// even while hidden - SetDlgTextIfChanged's dedupe makes that a strcmp per
	// poll - so a restore from the tray always shows the last good readings
	// instantly, even mid EC-error-streak (HandleData doesn't run again until
	// a read succeeds). Only the expensive RichEdit rebuild (UpdateTempList)
	// is gated on visibility; its caches are cleared on the hidden->visible
	// transition so the first visible poll rebuilds the list.
	bool uiVisible = ::IsWindowVisible(this->hwndDialog) != FALSE;
	if (uiVisible && !this->m_uiVisible) {
		this->m_tempListSig[0] = '\0';
		this->m_tempListRows = -1;
	}
	this->m_uiVisible = uiVisible;

	//
	// update dialog elements
	//

	// title string (for minimized window)
	FmtTemp(title2, sizeof(title2),
		Fahrenheit ? this->MaxTemp * 9 / 5 + 32 : this->MaxTemp, Fahrenheit);

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
			sprintf_s(title2 + strlen(title2), sizeof(title2) - strlen(title2), " Fan %d (%s)",	fanctrl & 0x3F,	this->CurrentMode == 2 ? "Smart" : "Fixed");
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
			sprintf_s(title2 + strlen(title2), sizeof(title2) - strlen(title2), " Fan %d (%s)",	fanctrl & 0x3F,	this->CurrentMode == 2 ? "Smart" : "Fixed");
		}
	}

	SetDlgTextIfChanged(this->hwndDialog, 8100, obuf2, this->m_lastState, sizeof(this->m_lastState));

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

	// fan-stall watchdog (idea: Tinnci fork 67f1014): the EC register reports
	// a level that should spin the fans, but both tachs read ~0 - the EC has
	// silently dropped or ignored the command, a failure mode neither the
	// thermal fail-safe nor the read-error fallback can see. After 3 such
	// polls in a row, re-issue the level to kick the EC. Both tachs must be
	// dead before this fires, so a one-fan idle design or a single flaky tach
	// can't trigger it; re-issuing an already-programmed level is harmless.
	{
		int lvl = fanctrl & 0x7f;
		bool shouldSpin = !(fanctrl & FAN_CTRL_BIOS) && lvl >= 1;
		if (shouldSpin && this->fan1speed < 200 && this->fan2speed < 200) {
			if (++this->m_stallPolls >= 3) {
				char sbuf[128];
				sprintf_s(sbuf, sizeof(sbuf),
					"Fan stall: level 0x%02x commanded but %d/%d rpm - reissuing",
					fanctrl, this->fan1speed, this->fan2speed);
				this->Trace(sbuf);
				this->SetFan("StallRecover", lvl);
				this->m_stallPolls = 0;
			}
		}
		else
			this->m_stallPolls = 0;
	}

	// compose the richer multi-line tray tooltip (mode / max temp / fan / active
	// profile, plus fail-safe + EC-error flags). Title2 stays the single-line title.
	{
		char dT[16];
		FmtTemp(dT, sizeof(dT),
			Fahrenheit ? (this->MaxTemp * 9 / 5 + 32) : this->MaxTemp, Fahrenheit);

		char modeStr[40];
		switch (this->CurrentMode) {
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
		if (fanctrl & FAN_CTRL_BIOS)                   strcpy_s(fanStr, sizeof(fanStr), "BIOS");
		else if ((fanctrl & 0x7f) == FAN_CTRL_FULL)    strcpy_s(fanStr, sizeof(fanStr), "max");
		else                             sprintf_s(fanStr, sizeof(fanStr), "%d", fanctrl & 0x7f);

		sprintf_s(this->TrayTip, sizeof(this->TrayTip),
			"%s\r\nMax %s   Fan %s   %d/%d rpm",
			modeStr, dT, fanStr, this->fan1speed, this->fan2speed);

		char extra[64] = "";
		if (this->m_failsafeTripped)
			strcpy_s(extra, sizeof(extra), "FAIL-SAFE: fan forced max");
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
	SetDlgTextIfChanged(this->hwndDialog, 8102, obuf2, this->m_lastFan1, sizeof(this->m_lastFan1));

	sprintf_s(obuf2, sizeof(obuf2), "%d RPM", this->fan2speed);
	SetDlgTextIfChanged(this->hwndDialog, 8104, obuf2, this->m_lastFan2, sizeof(this->m_lastFan2));

	// display temperature list
	FmtTemp(obuf2, sizeof(obuf2),
		Fahrenheit ? this->MaxTemp * 9 / 5 + 32 : this->MaxTemp, Fahrenheit);
	SetDlgTextIfChanged(this->hwndDialog, 8103, obuf2, this->m_lastMaxT, sizeof(this->m_lastMaxT));

	if (uiVisible) {
		this->UpdateTempList();
		// keep the manual box/slider enabled state in sync with the current mode
		this->UpdateManualControlsEnabled(this->CurrentMode);
	}

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

	{
		char tb[16];
		FmtTemp(tb, sizeof(tb), Fahrenheit ? MaxTemp * 9 / 5 + 32 : MaxTemp, Fahrenheit);
		sprintf_s(CurrentStatus, sizeof(CurrentStatus), "Fan: 0x%02x / Switch: %s (%s)",
			State.FanCtrl, tb, templist);
	}

	// display fan speed

	// CurrentStatuscsv feeds only the CSV log (Tracecsv, gated on Log2csv == 1), so
	// build it only when CSV logging is on; obuf2 is its scratch buffer here.
	// (fan1speed/fan2speed were already clamped to <= 0x1fff when decoded above.)
	if (this->Log2csv == 1) {
		sprintf_s(obuf2, sizeof(obuf2), "%d/%d", this->fan1speed, this->fan2speed);
		sprintf_s(CurrentStatuscsv, sizeof(CurrentStatuscsv), "%s %s; %d; %d; ", templist, obuf2, State.FanCtrl, MaxTemp);
	}

	SetDlgTextIfChanged(this->hwndDialog, 8112, this->CurrentStatus, this->m_lastStatus, sizeof(this->m_lastStatus));

	//
	// handle fan control according to mode
	//

	this->ShowAllFromDialog();   // mode already read once at the top of this poll

	SetDlgTextIfChanged(this->hwndDialog, 8115,
		(this->CurrentMode == 2 || this->CurrentMode == 3) ? "TPControlFAN = On" : "TPControlFAN = OFF",
		this->m_lastTpf, sizeof(this->m_lastTpf));

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
		// (critical-temp hibernate guard sits after this block - it is the
		// escalation path for when full fan speed is not enough)
		// log both flag transitions explicitly: SetFan only logs when it has to
		// write, so a trip with the fan already at max would otherwise leave no
		// record in the trace - exactly the event worth a timestamp after a
		// thermal incident
		// use safetyMax (raw, all trusted sensors) not MaxTemp, so a positive bias or
		// an ignored hottest sensor can't hide an overheat (H-07). The safetyValid
		// guard means a poll with no trusted sensor neither trips nor RELEASES the
		// fail-safe - uncertain telemetry holds the current trip state.
		char fsbuf[128];
		if (!this->m_failsafeTripped && safetyValid && safetyMax >= this->FailsafeTemp) {
			this->m_failsafeTripped = true;
			sprintf_s(fsbuf, sizeof(fsbuf),
				"Fail-safe TRIPPED: max temp %d C reached threshold %d C, forcing full fan speed",
				safetyMax, this->FailsafeTemp);
			this->Trace(fsbuf);
		}
		else if (this->m_failsafeTripped && safetyValid && safetyMax <= this->FailsafeTemp - 3) {
			this->m_failsafeTripped = false;
			this->m_failsafeWriteWarned = false;
			sprintf_s(fsbuf, sizeof(fsbuf),
				"Fail-safe released: max temp %d C cooled below %d C",
				safetyMax, this->FailsafeTemp - 3);
			this->Trace(fsbuf);
		}
	}
	else {
		if (this->m_failsafeTripped)
			this->Trace("Fail-safe released: disabled or mode left Smart/Manual");
		this->m_failsafeTripped = false;   // disabled, or mode left Smart/Manual
		this->m_failsafeWriteWarned = false;
	}

	// Emergency hibernate (last line of defense; upstream ask #95): if max
	// temp holds at/above CriticalTemp for 3 consecutive polls - i.e. even
	// the fail-safe's full fan speed is losing - force max fan one final
	// time and hibernate before the firmware's hard thermal trip cuts power.
	// Mode-independent: critical heat is critical in BIOS mode too. Re-arms
	// only after cooling 5 C below the threshold so a resume into a
	// still-hot machine cannot loop straight back into hibernation.
	// safetyMax (raw) + safetyValid guards: a poll with no trusted sensor can neither
	// re-arm nor escalate, and any non-hot/invalid poll resets the streak - so garbage
	// telemetry can never accumulate three "critical" polls into a spurious hibernate.
	if (this->CriticalTemp > 0) {
		if (this->m_critFired) {
			if (safetyValid && safetyMax <= this->CriticalTemp - 5) {
				this->m_critFired = false;
				this->Trace("Critical-temp guard re-armed (cooled below threshold)");
			}
		}
		else if (safetyValid && safetyMax >= this->CriticalTemp) {
			if (++this->m_critPolls >= 3) {
				this->m_critFired = true;
				this->m_critPolls = 0;
				char cbuf[160];
				sprintf_s(cbuf, sizeof(cbuf),
					"CRITICAL: max temp %d C at/above %d C for 3 polls - forcing max fan and hibernating",
					safetyMax, this->CriticalTemp);
				this->Trace(cbuf);
				this->SetFan("Critical", FAN_CTRL_FULL);
				::MessageBeep(MB_ICONERROR);
				EmergencyHibernate();   // returns on resume (or on failure)
			}
		}
		else
			this->m_critPolls = 0;
	}

	switch (this->CurrentMode) {

	case 1: // BIOS
		this->TraceModeChange();

		if (this->State.FanCtrl != FAN_CTRL_BIOS)
			ok = this->SetFan("BIOS", FAN_CTRL_BIOS);
		break;

	case 2: // Smart
		// anyValid: never run the curve on a no-sensor poll (would drive to fan 0 - C-04)
		if (anyValid && !this->m_failsafeTripped)   // fail-safe holds the fan; skip curve control
			this->SmartControl();       // (logs its own mode-change transition)
		else
			this->TraceModeChange();    // logs only an actual mode change arriving while the fail-safe holds
		break;

	case 3: { // Manual
		this->TraceModeChange();

		::GetDlgItemText(this->hwndDialog, 8310, manlevel, sizeof(manlevel));

		// Only the documented set is a valid EC fan byte: levels 0-7, or 64 = full
		// speed. Values 8-63 / 65-127 are undefined, and >=128 sets the BIOS-control
		// bit (silently handing the fan back to the BIOS while the UI still says
		// "Manual"). Reject anything else rather than write a raw arbitrary byte to
		// the EC, and snap the box back to the last applied level so it stays honest.
		int lvl = atoi(manlevel);
		bool valid = isdigit((unsigned char)manlevel[0]) &&
		             ((lvl >= 0 && lvl <= 7) || lvl == 64);
		// anyValid: on a no-sensor poll hold the current level rather than re-issuing
		// (harmless for a fixed level, but keeps "no telemetry -> no fan change" - C-04)
		if (anyValid && !this->m_failsafeTripped && valid) {
			if (this->State.FanCtrl != lvl)
				ok = this->SetFan("Manual", lvl);
			else
				ok = true;
		}
		else if (!valid && manlevel[0] != '\0') {
			char snap[16];
			_itoa_s(this->State.FanCtrl & 0x7f, snap, 10);   // mask off the BIOS bit for display
			::SetDlgItemText(this->hwndDialog, 8310, snap);
		}

		break;
	}
	}

	this->PreviousMode = this->CurrentMode;

	// apply the fail-safe override (full speed) once per cycle while tripped
	if (this->m_failsafeTripped && this->State.FanCtrl != FAN_CTRL_FULL) {
		ok = this->SetFan("Fail-safe: max temp reached, forcing full fan speed", FAN_CTRL_FULL);
		// one-shot note (SetFan logs FAILED!! per attempt): if this EC does not
		// echo 0x40 back, the override re-fires every poll until the trip clears
		if (!ok && !this->m_failsafeWriteWarned) {
			this->Trace("Fail-safe: EC did not accept full speed (0x40); retrying every cycle");
			this->m_failsafeWriteWarned = true;
		}
	}

	// (Manual-mode auto-exit lives in the title timer, which calls ModeToDialog(2)
	// so the radio buttons actually change. Setting CurrentMode here did nothing:
	// CurrentModeFromDialog() overwrites it from the unchanged buttons next poll.)

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
		bool verified = false;   // set only when THIS attempt fully confirmed the level
		for (int i = 0; i < 3; i++) {
			// Set the new level on both fans. Sequence with && so a FAILED selector
			// write short-circuits its paired level write (H-03): a level byte must
			// never land on whatever fan happened to be selected before. Any failed
			// step drops the whole attempt into the retry/backoff below.
			bool write_ok =
				this->WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN1) &&
				this->WriteByteToEC(TP_ECOFFSET_FAN, fanctrl);

			::Sleep(100);

			write_ok = write_ok &&
				this->WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN2) &&
				this->WriteByteToEC(TP_ECOFFSET_FAN, fanctrl);

			::Sleep(100);

			// verify completion of fan2 into its OWN buffer (not the shared member),
			// so the fan1 read-back below can't overwrite or alias it (H-04)
			unsigned char fan2Read = 0xFF, fan1Read = 0xFF;
			fan2_ok = this->ReadByteFromEC(TP_ECOFFSET_FAN, &fan2Read);

			::Sleep(100);

			// re-select and verify completion of fan1
			write_ok = write_ok &&
				this->WriteByteToEC(TP_ECOFFSET_FAN_SWITCH, TP_ECVALUE_SELFAN1);

			::Sleep(100);

			fan1_ok = this->ReadByteFromEC(TP_ECOFFSET_FAN, &fan1Read);

			// Confirm only when every write AND both read-backs of THIS attempt
			// succeeded and both fans actually latched the level. ReadByteFromEC
			// leaves its out-param untouched on failure, so requiring fan1_ok/fan2_ok
			// (not merely a value compare against a possibly-stale buffer) stops a
			// failed read from being mistaken for success (H-04).
			if (write_ok && fan1_ok && fan2_ok &&
					fan1Read == (unsigned char)fanctrl && fan2Read == (unsigned char)fanctrl) {
				this->State.FanCtrl = fan1Read;   // publish the confirmed level
				verified = true;
				sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "[i=%d] ", i);
				break;
			}

			if (!write_ok)
				sprintf_s(obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), "[wr-fail i=%d] ", i);

			if (i < 2) ::Sleep(150);   // brief backoff before retry (skipped after last attempt)
		}

		this->FreeECAccess();

		if (verified) {
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

	return ok;
}
