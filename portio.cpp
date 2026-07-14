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
#include "TVicPort.h"

// Registers of the embedded controller
// V0.6.3+ V.2.2.0+
constexpr auto ACPI_EC_TYPE1_CTRLPORT = 0x1604;
constexpr auto ACPI_EC_TYPE1_DATAPORT = 0x1600  ;
// V0.6.2 final
constexpr auto ACPI_EC_TYPE2_CTRLPORT = 0x66  ;
constexpr auto ACPI_EC_TYPE2_DATAPORT = 0x62   ;

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
// wait for the desired status from the embedded controller (EC) via port io 
//--------------------------------------------------------------------------
static bool
WaitForFlags(USHORT port, char flags, int onoff = false, int timeout = 1000) {
	char data;

	int time = 0, sleepTicks = 10;

	// wait for flags to clear and reach desired state
	for (time = 0; time < timeout; time += sleepTicks) {
		data = ReadPort(port);

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
bool
FANCONTROL::ReadByteFromEC(int offset, unsigned char* pdata) {

	if (this->EC_CTRL == 0) {
		this->EC_CTRL = ACPI_EC_TYPE1_CTRLPORT;
		this->EC_DATA = ACPI_EC_TYPE1_DATAPORT;
		this->Trace("Using ACPI_EC_TYPE1");
	}

	// wait for IBF and OBF to clear
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF | ACPI_EC_FLAG_OBF)) {
		this->Trace("readec: timed out #1");
		// Only probe the alternate EC port pair until the transport is confirmed.
		// Once a read has succeeded the type is KNOWN, so a later transient busy /
		// timeout must NOT flip it - doing so turned an ordinary stall into a
		// permanent protocol switch that poisoned every following transaction (H-02).
		if (!this->m_ecTypeKnown) {
			if (this->EC_CTRL == ACPI_EC_TYPE1_CTRLPORT) {
				this->EC_CTRL = ACPI_EC_TYPE2_CTRLPORT;
				this->EC_DATA = ACPI_EC_TYPE2_DATAPORT;
				this->Trace("Now using ACPI_EC_TYPE2");
			}
			else {
				this->EC_CTRL = ACPI_EC_TYPE1_CTRLPORT;
				this->EC_DATA = ACPI_EC_TYPE1_DATAPORT;
				this->Trace("Now using ACPI_EC_TYPE1");
			}
		}
		return false;
	}

	// indicate read operation desired
	WritePort(this->EC_CTRL, ACPI_EC_COMMAND_READ);

	// wait for IBF to clear (command byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("readec: timed out #2");
		return false;
	}

	// indicate read operation desired location
	WritePort(this->EC_DATA, offset);

	// wait for IBF to clear (address byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("readec: timed out #3");
		return false;
	}

	// wait for OBF=TRUE: the result byte must actually be in the EC's output
	// buffer before we read it, otherwise we latch a stale/previous value and
	// can drive a wrong fan decision. Short timeout (100ms) so a misbehaving EC
	// fails fast into the caller's retry / BIOS-fallback path instead of hanging;
	// a healthy EC has OBF set already and returns immediately.
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_OBF, true, 100)) {
		this->Trace("readec: timed out #4 (OBF)");
		// Drain any late/stale output byte so it cannot be mis-consumed as the NEXT
		// transaction's result (H-01). Reading the data port clears OBF; the value
		// is discarded because this read has already failed.
		if (ReadPort(this->EC_CTRL) & ACPI_EC_FLAG_OBF)
			(void)ReadPort(this->EC_DATA);
		return false;
	}

	*pdata = ReadPort(this->EC_DATA);

	this->m_ecTypeKnown = true;   // transport confirmed; stop probing the alternate ports
	return TRUE;
}

//-------------------------------------------------------------------------
// write a byte to the embedded controller (EC) via port io
//-------------------------------------------------------------------------
bool
FANCONTROL::WriteByteToEC(int offset, char NewData) {

	// A SetFan() begins with EC writes, so a write can be the very first EC access
	// (e.g. a hotkey/menu action racing the first poll). EC_CTRL/EC_DATA start at 0,
	// and are otherwise only initialized inside ReadByteFromEC; initialize them here
	// too, so we never drive WaitForFlags/WritePort against port 0.
	if (this->EC_CTRL == 0) {
		this->EC_CTRL = ACPI_EC_TYPE1_CTRLPORT;
		this->EC_DATA = ACPI_EC_TYPE1_DATAPORT;
		this->Trace("Using ACPI_EC_TYPE1");
	}

	// wait for IBF and OBF to clear
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF | ACPI_EC_FLAG_OBF)) {
		this->Trace("writeec: timed out #1");
		return false;
	}

	// indicate write operation desired
	WritePort(this->EC_CTRL, ACPI_EC_COMMAND_WRITE);

	// wait for IBF to clear (command byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("writeec: timed out #2");
		return FALSE;
	}

	// indicate write operation desired location
	WritePort(this->EC_DATA, offset);

	// wait for IBF to clear (address byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("writeec: timed out #3");
		return false;
	}

	// perform the write operation
	WritePort(this->EC_DATA, NewData);

	// wait for IBF to clear (data byte removed from EC's input queue)
	if (!WaitForFlags(this->EC_CTRL, ACPI_EC_FLAG_IBF)) {
		this->Trace("writeec: timed out #4");
		return false;
	}

	return TRUE;
}
