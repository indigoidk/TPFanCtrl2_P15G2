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
#include "portio_pawn.h"

// Registers of the embedded controller.
// PawnIO-only build: the stock LpcACPIEC module permits ONLY the ACPI-spec EC
// pair (TYPE2, 0x66/0x62), so that is the sole pair used. The legacy TYPE1
// pair (0x1604/0x1600) that the old TVicPort path preferred is not reachable
// through PawnIO and is kept here only for reference.
constexpr auto ACPI_EC_TYPE1_CTRLPORT = 0x1604;   // legacy, unused under PawnIO
constexpr auto ACPI_EC_TYPE1_DATAPORT = 0x1600;   // legacy, unused under PawnIO
constexpr auto ACPI_EC_TYPE2_CTRLPORT = 0x66;     // cmd/status (the only pair PawnIO allows)
constexpr auto ACPI_EC_TYPE2_DATAPORT = 0x62;     // data

// Embedded controller status register bits
constexpr auto ACPI_EC_FLAG_OBF = 0x01	/* Output buffer full */;
constexpr auto ACPI_EC_FLAG_IBF = 0x02	/* Input buffer full */;
constexpr auto ACPI_EC_FLAG_CMD = 0x08	/* Input buffer contains a command */;

// Embedded controller commands
constexpr auto ACPI_EC_COMMAND_READ = (char)0x80;
constexpr auto ACPI_EC_COMMAND_WRITE = (char)0x81;
constexpr auto ACPI_EC_BURST_ENABLE = (char)0x82;
constexpr auto ACPI_EC_BURST_DISABLE = (char)0x83;
constexpr auto ACPI_EC_COMMAND_QUERY = (char)0x84;

//--------------------------------------------------------------------------
// wait for the desired status from the embedded controller (EC) via the
// PawnIO port-I/O transport
//--------------------------------------------------------------------------
static bool
WaitForFlags(USHORT port, char flags, int onoff = false, int timeout = 1000) {
	unsigned char data;

	int time = 0, sleepTicks = 10;

	// wait for flags to clear and reach desired state
	for (time = 0; time < timeout; time += sleepTicks) {
		// A transport failure (dead handle / disallowed port) fails the wait
		// immediately instead of spinning the full timeout; the caller's
		// existing "timed out" retry / BIOS-fallback ladder then handles it.
		if (!g_PortIo->ReadPort8(port, &data)) return false;

		int flagstate = (data & flags) != 0;
		int	wantedstate = onoff != 0;

		if (flagstate == wantedstate) return TRUE;

		::Sleep(sleepTicks);
	}
	return false;
}

//-------------------------------------------------------------------------
// read a byte from the embedded controller (EC) via port io
//-------------------------------------------------------------------------
// A single EC byte read is retried up to this many times, with this gap between
// attempts, to ride out transient acpi.sys / ACPI contention on the shared 0x62/0x66
// ports (see ReadByteFromEC). kEcStageWaitMs caps the #1-#3 handshake stages (the
// WaitForFlags default is 1000ms): under the retry loop an un-capped stuck flag would
// otherwise burn 3x1000ms and, multiplied by ReadEcStatus's 10x sample retry, stall a
// wedged-EC poll for tens of seconds while holding EcAccess. The cap makes a wedged
// read fail fast into the retry / BIOS-fallback path (wedge worst-case is now shorter
// than the pre-retry single-shot design).
static const int   kEcReadAttempts = 3;
static const DWORD kEcRetryGapMs   = 15;
static const int   kEcStageWaitMs  = 100;

bool
FANCONTROL::ReadByteFromEC(int offset, unsigned char* pdata) {

	if (this->EC_CTRL == 0) {
		// PawnIO allows only the ACPI-spec pair, so default straight to TYPE2.
		// The FANCONTROL ctor also pre-seeds these; this is a belt-and-braces
		// init in case a hotkey-driven access races the ctor.
		this->EC_CTRL = ACPI_EC_TYPE2_CTRLPORT;
		this->EC_DATA = ACPI_EC_TYPE2_DATAPORT;
		this->Trace("Using ACPI_EC_TYPE2");
	}

	// The 0x62/0x66 EC ports are shared with acpi.sys (and other ACPI consumers),
	// which frequently win the port mid-handshake so our byte's OBF never arrives in
	// the short #4 window. Re-issue the whole handshake a few times, with a brief gap
	// for the contending consumer to finish, before giving up - this stops one unlucky
	// byte among the many read per sample from failing the whole sample and, in bursts,
	// tripping the consecutive-read-error BIOS fallback.
	//
	// PROVENANCE: raw port I/O has no transaction ownership, so an individual byte
	// cannot be *proven* to belong to our request (true of the original single-shot
	// design too). The end-to-end guard against a wrong byte reaching a fan decision is
	// ReadEcStatus's double-sample match (SampleMatch, fanstuff.cpp): the FanCtrl field
	// AND all 12 temperature sensors (agree within 5C) must match between two back-to-back
	// samples, retried up to 10x, so a torn/foreign temperature breaks the match and is
	// re-read. The per-attempt drain also clears a stale byte before each fresh handshake
	// so it can't be re-consumed. RESIDUAL (accepted): a torn value within 5C of truth, or
	// the same wrong value torn into BOTH samples (correlated contention, ~p^2) - bounded
	// by max-over-12 aggregation (a wrong-low on a non-hottest sensor can't move MaxTemp) +
	// smart hysteresis + the FailsafeTemp fail-safe on raw safetyMax (when configured) +
	// one-poll blast radius + the firmware's ~99C throttle. Wrong-high fails safe (fan up).
	// A fatal transport failure aborts at once; genuine flag timeouts (incl. a not-yet-
	// latched TransportLost) retry.
	int lastStage = 0;
	for (int attempt = 0; attempt < kEcReadAttempts; ++attempt) {
		if (attempt > 0) {
			::Sleep(kEcRetryGapMs);
			// Drain any byte left in OBF by the previous attempt (our own late
			// response, or a foreign one) BEFORE re-issuing: otherwise #1's
			// wait-for-OBF-clear spins its whole timeout (nothing else here reads the
			// data port to clear it), and a stale byte could be re-consumed. The fresh
			// handshake below then produces this attempt's own response.
			unsigned char stale;
			if (g_PortIo->ReadPort8((USHORT)this->EC_CTRL, &stale) && (stale & ACPI_EC_FLAG_OBF))
				(void)g_PortIo->ReadPort8((USHORT)this->EC_DATA, &stale);
		}

		// wait for IBF and OBF to clear (capped: a stuck flag must fail fast into the
		// retry, not spin the full default timeout)
		if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF | ACPI_EC_FLAG_OBF, false, kEcStageWaitMs)) {
			if (g_PortIo->TransportLost()) return false;   // fatal transport loss: don't retry
			lastStage = 1;
			continue;
		}

		// indicate read operation desired
		if (!g_PortIo->WritePort8((USHORT)this->EC_CTRL, ACPI_EC_COMMAND_READ)) {
			this->Trace("readec: write READ command failed");
			return false;   // transport failure - retrying cannot help
		}

		// wait for IBF to clear (command byte removed from EC's input queue)
		if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF, false, kEcStageWaitMs)) {
			if (g_PortIo->TransportLost()) return false;
			lastStage = 2;
			continue;
		}

		// indicate read operation desired location
		if (!g_PortIo->WritePort8((USHORT)this->EC_DATA, (unsigned char)offset)) {
			this->Trace("readec: write address failed");
			return false;   // transport failure
		}

		// wait for IBF to clear (address byte removed from EC's input queue)
		if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF, false, kEcStageWaitMs)) {
			if (g_PortIo->TransportLost()) return false;
			lastStage = 3;
			continue;
		}

		// wait for OBF=TRUE: the result byte must actually be in the EC's output
		// buffer before we read it, otherwise we latch a stale/previous value and
		// can drive a wrong fan decision. Short per-attempt timeout so a contended
		// read fails fast into the retry above (or, after the last attempt, the
		// caller's BIOS-fallback path) instead of hanging; a healthy EC has OBF set
		// already and returns immediately.
		if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_OBF, true, kEcStageWaitMs)) {
			// Drain any byte that lands right at the timeout so it cannot be
			// mis-consumed as the next attempt's (or transaction's) result (H-01).
			unsigned char status;
			if (g_PortIo->ReadPort8((USHORT)this->EC_CTRL, &status) && (status & ACPI_EC_FLAG_OBF)) {
				unsigned char discard;
				(void)g_PortIo->ReadPort8((USHORT)this->EC_DATA, &discard);
			}
			if (g_PortIo->TransportLost()) return false;
			lastStage = 4;
			continue;   // OBF never arrived (contention) - re-issue the handshake
		}

		if (!g_PortIo->ReadPort8((USHORT)this->EC_DATA, pdata)) {
			this->Trace("readec: data read failed");
			return false;   // transport failure
		}

		this->m_ecTypeKnown = true;   // a read has completed on this pair
		return TRUE;
	}

	// Every attempt hit a flag timeout at the recorded stage: another ACPI consumer is
	// holding the EC ports. Same failure the single-shot path reported, so the caller's
	// read-error accounting / BIOS fallback is unchanged.
	char tb[80];
	sprintf_s(tb, sizeof(tb), "readec: EC busy - timed out at stage #%d after %d attempts",
		lastStage, kEcReadAttempts);
	this->Trace(tb);
	return false;
}

//-------------------------------------------------------------------------
// write a byte to the embedded controller (EC) via port io
//-------------------------------------------------------------------------
bool
FANCONTROL::WriteByteToEC(int offset, char NewData) {

	// A SetFan() begins with EC writes, so a write can be the very first EC access
	// (e.g. a hotkey/menu action racing the first poll). EC_CTRL/EC_DATA start at 0,
	// and are otherwise only initialized inside ReadByteFromEC; initialize them here
	// too, so we never drive WaitForFlags/WritePort8 against port 0.
	if (this->EC_CTRL == 0) {
		this->EC_CTRL = ACPI_EC_TYPE2_CTRLPORT;
		this->EC_DATA = ACPI_EC_TYPE2_DATAPORT;
		this->Trace("Using ACPI_EC_TYPE2");
	}

	// wait for IBF and OBF to clear
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF | ACPI_EC_FLAG_OBF)) {
		this->Trace("writeec: timed out #1");
		return false;
	}

	// indicate write operation desired
	if (!g_PortIo->WritePort8((USHORT)this->EC_CTRL, ACPI_EC_COMMAND_WRITE)) {
		this->Trace("writeec: write WRITE command failed");
		return false;
	}

	// wait for IBF to clear (command byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("writeec: timed out #2");
		return FALSE;
	}

	// indicate write operation desired location
	if (!g_PortIo->WritePort8((USHORT)this->EC_DATA, (unsigned char)offset)) {
		this->Trace("writeec: write address failed");
		return false;
	}

	// wait for IBF to clear (address byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("writeec: timed out #3");
		return false;
	}

	// perform the write operation
	if (!g_PortIo->WritePort8((USHORT)this->EC_DATA, (unsigned char)NewData)) {
		this->Trace("writeec: write data failed");
		return false;
	}

	// wait for IBF to clear (data byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("writeec: timed out #4");
		return false;
	}

	return TRUE;
}
