// --------------------------------------------------------------
//  fanlogic.h - pure, hardware-independent fan-control decision logic
// --------------------------------------------------------------
//
//  These functions contain no Win32, no I/O and no global/member state, so the
//  exact math that drives fan decisions can be unit-tested off-hardware (see
//  tests/fanlogic_tests.cpp). The FANCONTROL methods BiasedTemp() and
//  SmartControl() delegate here, so those tests cover the real production paths.
//
//  (Inspired by the IIOProvider/Mock split in the Tinnci/TPFanCtrl2 fork, but
//  kept header-only and dependency-free so it needs no build-system changes.)
//
//  This program and source code is in the public domain.
// --------------------------------------------------------------

#ifndef FANLOGIC_H
#define FANLOGIC_H

namespace fanlogic {

// One Smart-table row. Mirrors the first four fields of FANCONTROL::SMARTENTRY.
struct FanLevel {
	int temp;       // threshold temperature; a row with temp == -1 terminates the table
	int fan;        // fan level to apply at/above this temperature
	int hystUp;     // extra degrees required before ramping UP into this level
	int hystDown;   // degrees of margin before ramping DOWN out of this level
};

// Apply a per-sensor offset to a raw reading, honoring the hysteresis window in
// which the offset is disabled. Returns the raw value unchanged when biasing is
// off or the reading is inside [hystMin, hystMax].
inline int biased_temp(int rawTemp, int offs, int hystMin, int hystMax, bool showBiased) {
	if (!showBiased)
		return rawTemp;
	if (rawTemp >= hystMin && rawTemp <= hystMax)
		offs = 0;
	return rawTemp - offs;
}

// Decide the Smart-mode fan level for the current temperature.
//   maxTemp   : current max sensor temperature (already biased)
//   curFan    : current EC fan-control byte
//   lev64Norm : treat fan level 64 as a normal level (not "max")
//   prevMode  : previous control mode (1=BIOS, 2=Smart, 3=Manual)
//   levels/n  : the Smart table (terminated by temp == -1, or by n entries)
//   lastLevel : in/out index of the last applied level (for hysteresis); <0 = none yet
// Returns the fan level to set, or -1 for "leave the fan unchanged".
inline int smart_decide(int maxTemp, int curFan, bool lev64Norm, int prevMode,
                        const FanLevel* levels, int n, int& lastLevel) {
	int fanctrl = curFan;
	int newfanctrl = -1, levelIndex = -1;

	// coming from a high fixed level, or from BIOS/Manual: recompute from zero
	if ((fanctrl > 7 && (fanctrl != 64 || !lev64Norm)) || prevMode == 3 || prevMode == 1) {
		newfanctrl = 0;
		levelIndex = 0;
		fanctrl = 0;
	}

	// ramp up: highest level whose threshold we have reached
	for (int i = 0; i < n && levels[i].temp != -1; i++) {
		if (maxTemp >= levels[i].temp && levels[i].fan >= fanctrl) {
			newfanctrl = levels[i].fan;
			levelIndex = i;
		}
	}

	// ramp down: first lower level we have cooled below
	if (newfanctrl == -1) {
		for (int i = 0; i < n && levels[i].temp != -1; i++) {
			if (maxTemp <= levels[i].temp && levels[i].fan < fanctrl) {
				newfanctrl = levels[i].fan;
				levelIndex = i;
				break;
			}
		}
	}

	if (newfanctrl == -1 || newfanctrl == curFan)
		return -1;

	if (lastLevel < 0) {            // first decision: no hysteresis
		lastLevel = levelIndex;
		return newfanctrl;
	}

	const FanLevel& nl = levels[levelIndex];
	if (maxTemp < levels[lastLevel].temp) {
		if (maxTemp > nl.temp - nl.hystDown)
			return -1;              // still within the cooling hysteresis band
	}
	else {
		if (maxTemp < nl.temp + nl.hystUp)
			return -1;              // still within the rising hysteresis band
	}

	lastLevel = levelIndex;
	return newfanctrl;
}

} // namespace fanlogic

#endif // FANLOGIC_H
