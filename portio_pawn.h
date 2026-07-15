// --------------------------------------------------------------
//
//  Thinkpad Fan Control - PawnIO port-I/O transport (PawnIO-only build)
//
// --------------------------------------------------------------
//
//  This program and source code is in the public domain.
//
//  NOTE: this header (the interface contract) is public-domain like the rest
//  of the app. The signed module it drives, LpcACPIEC.bin, is a separate
//  LGPL-2.1 artifact shipped alongside - see NOTICE / packaging.
// --------------------------------------------------------------
//
//  This replaces the former EnTech TVicPort dependency entirely. The app now
//  reaches the embedded controller ONLY through the Microsoft-signed PawnIO
//  kernel driver executing the signed LpcACPIEC module (raw CreateFileW +
//  DeviceIoControl; PawnIOLib.dll is x64-only and cannot be linked into this
//  Win32 build). If PawnIO is unavailable the app cannot drive the fans and
//  exits to firmware control - there is no software fallback by design.
//
//  Implementation: portio_pawn.cpp. Verified ABI + behaviour are specified in
//  PAWNIO_BACKEND_DESIGN.md (device path \\?\GLOBALROOT\Device\PawnIO; IOCTLs
//  LOAD 0xA1B22084 / EXEC 0xA1B22104 / VERSION 0xA1B22184; module functions
//  "ioctl_pio_read"/"ioctl_pio_write"; ports 0x62/0x66 only; mutex
//  Global\Access_EC). Runtime-verified on the target P15G2 (driver v2.1.0).

#ifndef PORTIO_PAWN_H
#define PORTIO_PAWN_H

#include <windows.h>

// --------------------------------------------------------------
//  IPortIo - the port-I/O seam under the EC ACPI handshake (portio.cpp).
//
//  One production implementation (PortIoPawn, portio_pawn.cpp) plus a mock in
//  tests/ so the handshake, backend-open, EC-type steering and transport-lost
//  handling are unit-testable off-hardware. Byte I/O returns bool so a dead
//  transport or a disallowed port is reported to the caller (the old TVic
//  ReadPort/WritePort were UCHAR(USHORT) / void and could not signal failure).
// --------------------------------------------------------------
class IPortIo {
public:
	virtual ~IPortIo() {}

	// Threading contract: DESTRUCTION (delete / ~IPortIo) must not race the EC
	// worker - the app destroys the transport only when no worker thread can
	// exist (approot.cpp deletes it solely on the startup-failure path; on a
	// normal exit it Close()s and lets the OS reclaim the small object at process
	// end). Close() ITSELF may run concurrently with a straggler ReadPort8/
	// WritePort8: the implementation serializes them internally, so a late port
	// op after Close() fails cleanly rather than racing.

	// Open the driver, (re)start its demand-start service if needed, load the
	// signed module, and become ready for port I/O. FALSE = PawnIO unavailable
	// (driver absent, module missing/rejected, not elevated). Idempotent.
	virtual bool	Open() = 0;
	virtual void	Close() = 0;
	virtual bool	IsOpen() const = 0;

	// Byte port I/O via the module's ioctl_pio_read / ioctl_pio_write.
	// FALSE = TRANSPORT failure (ioctl failed, disallowed port, handle dead) -
	// NOT an EC-protocol timeout, which stays in portio.cpp. On a fatal ioctl
	// failure the implementation latches TransportLost().
	virtual bool	ReadPort8(USHORT port, UCHAR* pdata) = 0;
	virtual bool	WritePort8(USHORT port, UCHAR data) = 0;

	// Capability query: can the loaded module drive this port at all? The stock
	// LpcACPIEC module permits ONLY the ACPI-spec EC pair (0x62 data / 0x66
	// cmd); TYPE1 0x1600/0x1604 and the TWR window are rejected here so the EC
	// layer's TYPE2 steering can never be silently bypassed.
	virtual bool	PortAllowed(USHORT port) const = 0;

	// Cross-process EC-transaction bracket (the PawnIO ecosystem Global\Access_EC
	// mutex). Held across a whole selector-dependent LOGICAL group - one
	// ReadEcRaw sample or one SetFan attempt - NOT per byte, so a concurrent EC
	// tool cannot flip the persistent fan selector (EC reg 0x31) mid-group.
	// Bounded wait; FALSE from Begin degrades the group to the existing EC-retry
	// ladder. Degrades to a logged no-op if the mutex could not be created (a
	// thermal app must never refuse to run over a courtesy mutex).
	virtual bool	BeginEcTransaction() = 0;
	virtual void	EndEcTransaction() = 0;

	// Sticky: set when a port op failed in a way that means the handle/driver is
	// gone (not a transient EC timeout). Callers use it to take the clean-exit
	// path (which must NOT require a further successful write through the dead
	// handle) after one bounded reopen attempt has failed. See design §12.
	virtual bool	TransportLost() const = 0;

	virtual const char*	Name() const = 0;	// e.g. "PawnIO (LpcACPIEC)" - for logs
};

// The single active transport, created at startup by CreatePawnIoTransport()
// and owned for the process lifetime. NULL until startup sets it.
extern IPortIo* g_PortIo;

// Factory: constructs the PawnIO transport (does NOT open it). Returns a new
// IPortIo the caller owns, or NULL on allocation failure. Callers must check
// the result before calling Open(). Implemented in portio_pawn.cpp so the
// driver-facing details stay in one translation unit.
IPortIo* CreatePawnIoTransport();

#endif // PORTIO_PAWN_H
