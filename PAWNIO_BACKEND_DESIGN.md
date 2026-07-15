# PawnIO Port-I/O Backend — Design Document

**Status:** DRAFT for review — implementation-ready except items marked `TBD-impl`
**Scope:** `fancontrol.x/` only (the live tree). Version baseline: `2.34 P15G2 Dual` (`fancontrol.h:29`).
**Author's note:** every claim about existing behavior below is cited as `file:line` against the current tree. Line numbers drift; search by symbol if stale.

---

## 1. Goal and non-negotiable constraints

Port the low-level port-I/O layer off the EnTech **TVicPort** kernel driver (unsigned, LOLDrivers-flagged) onto **PawnIO** (Microsoft-signed driver executing signed, sandboxed Pawn modules), as a *second backend* — the security-improving direction that FanControl / LibreHardwareMonitor took in 2025.

Hard constraints:

1. **Win32/x86 build is preserved.** The project builds `Release|Win32` only (`fancontrol.vcxproj:3-11`; runbook §5: "Win32 only (no x64 config)"). `PawnIOLib.dll` is x64-only — a 32-bit process can neither link its import lib nor `LoadLibrary` it. The PawnIO backend therefore talks to the driver **directly** via `CreateFileW` + `DeviceIoControl`, reimplementing the only three calls we need: *open device*, *load module*, *execute function*. Device I/O is bitness-agnostic and the PawnIO execute ABI is expressed in fixed-size `ULONG64` cells, so x86→x64 marshaling is a non-issue (no pointers cross the boundary).
2. **Zero functional regression.** TVicPort remains a fully supported backend. BIOS/Smart/Manual modes, dual-fan select/tach (`fanstuff.cpp:45-51`, `843-897`), the `UseTWR` block-read temp path (`fanstuff.cpp:942-991`), service mode (`approot.cpp:196-311`), Game Mode (`fancontrol.cpp:1672-1804`), and sleep/resume (`fancontrol.h:433-441`) all keep working exactly as today.
3. **Auto-selection PawnIO→TVic** with a new ini override `Driver=auto|pawnio|tvicport` (default `auto`): try PawnIO first, fall back to TVicPort when the PawnIO driver or module is unavailable **or cannot drive this machine's EC** (see §9 — this is the design's highest risk).

---

## 2. Current state — grounded inventory

### 2.1 The TVicPort API surface actually used

`TVicPort.h` declares ~45 functions; grep of `fancontrol.x/*.cpp` shows the app calls exactly **six**:

| Function | Declared | Called from |
|---|---|---|
| `OpenTVicPort()` | `TVicPort.h:39` | `approot.cpp:389` (180×1s retry loop) |
| `CloseTVicPort()` | `TVicPort.h:37` | `approot.cpp:416` |
| `TestHardAccess()` | `TVicPort.h:43` | `approot.cpp:401,403` |
| `SetHardAccess(BOOL)` | `TVicPort.h:45` | `approot.cpp:402` |
| `UCHAR ReadPort(USHORT)` | `TVicPort.h:47` | `portio.cpp:53,127,128,132`; `fanstuff.cpp:961,965,970,981,990` (TWR) |
| `void WritePort(USHORT, UCHAR)` | `TVicPort.h:49` | `portio.cpp:100,109,161,170,179`; `fanstuff.cpp:969,975,978` (TWR) |

`IsDriverOpened` (`TVicPort.h:41`) and every W/L/FIFO/`MapPhysToLinear`/`GetMem`/`SetMem`/LPT variant (`TVicPort.h:51-129`) are **declared but never called** anywhere in the live tree (verified by grep; the only hits are the header itself). The port-I/O contract we must reproduce is therefore just: *open, close, read byte, write byte* — plus two TVic-specific knobs (`TestHardAccess`/`SetHardAccess`) that stay TVic-only.

### 2.2 The EC ACPI handshake (`portio.cpp`)

- Two candidate EC port pairs (`portio.cpp:22-28`):
  - **TYPE1** (preferred, "V0.6.3+ V.2.2.0+"): CTRL `0x1604`, DATA `0x1600`
  - **TYPE2** (legacy, "V0.6.2 final"): CTRL `0x66`, DATA `0x62` — the ACPI-spec standard EC pair
- `WaitForFlags()` (`portio.cpp:45-63`) polls the status port via `ReadPort` every 10 ms up to a timeout (default 1000 ms) for IBF/OBF states.
- `ReadByteFromEC` (`portio.cpp:68-136`): lazily initializes `EC_CTRL/EC_DATA` to **TYPE1** on first use (`:71-75`); on the *first* wait timeout it flips to the alternate pair **only while `m_ecTypeKnown` is false** (`:84-95`); the first successful read latches `m_ecTypeKnown = true` (`:134`) so a later transient stall can't flip the transport (the H-02 fix). It also drains a late OBF byte on the read-timeout path (`:122-130`, H-01 fix).
- `WriteByteToEC` (`portio.cpp:141-188`) mirrors the handshake and does the same lazy TYPE1 init (`:148-152`) because a `SetFan()` can be the very first EC access.
- `EC_CTRL/EC_DATA` are `FANCONTROL` members (`fancontrol.h:162`), zero-initialized in the ctor (`fancontrol.cpp:92-93`); `m_ecTypeKnown` at `fancontrol.h:163-165`.
- All `WritePort` calls in the handshake are **fire-and-forget `void`**; only flag waits can fail.

### 2.3 The `UseTWR` alternate temp path (`fanstuff.cpp:905, 942-1019`)

When `UseTWR=1` (`misc.cpp:364-367`, default 0 at `fancontrol.cpp:116`), `ReadEcRaw` skips per-register temp reads and instead runs a **completely different block protocol** with *direct* port I/O, bypassing `ReadByteFromEC` entirely:

- polls `ReadPort(0x1604)` for ready (`fanstuff.cpp:961`), drains via `ReadPort(0x161f)` (`:965`),
- commands a block read with `WritePort(0x1610, 0x20)` (`:969`), zero-fills `0x1611..0x161e` (`:974-976`), terminates with `WritePort(0x161f, 0x00)` (`:978`),
- then reads 16 bytes from `0x1610..0x161f` (`:989-991`).

This path needs ports **0x1604 and 0x1610–0x161F** — all outside the stock PawnIO module's allowlist. Consequence: **`UseTWR=1` forces the TVic backend** (§8).

### 2.4 Startup / open sequence and service interplay (`approot.cpp:313-440`)

`WorkerThread()`:
1. `SetCurrentDirectory(<exe dir>)` (`:365-378`) — everything after this can use exe-relative paths.
2. `RecoverHiddenDrivers()` (`:319-347`, called at `:386`) — Game-Mode crash recovery: restores `TVicHW64.sys` / `TVicPort64.sys` from `.sys.bak` (WOW64-redirection-bypassed) so `OpenTVicPort` can load them.
3. **180 × 1 s retry** of `OpenTVicPort()` (`:388-399`), aborting early if the service stop event `g_stopEvent` fires; the SCM stop handler sets that event precisely to abort this loop (`:283-286`).
4. On success: `TestHardAccess()` / `SetHardAccess(TRUE)` / `TestHardAccess()` (`:401-403` — results stored in locals never read again), create `FANCONTROL`, run dialog loop, then `CloseTVicPort()` (`:416`).
5. On failure: interactive error box (text hardcodes "tvicport.sys", `:421-426`), and if running as a service report `SERVICE_STOPPED` with a specific exit code (`:429-439`).

### 2.5 Concurrency (`winstuff.*`, `fanstuff.cpp`)

`MUTEXSEM` wraps a **named Win32 mutex** — default name `"Access_Thinkpad_EC"` (`winstuff.h:76`, impl `winstuff.cpp:39-68`). Win32 mutexes are recursive per-thread and cross-process; `Lock()` treats `WAIT_ABANDONED` as acquired (`winstuff.cpp:56-60`). The `FANCONTROL::EcAccess` member (`fancontrol.h:207`) serializes EC access:

- `LockECAccess()`/`FreeECAccess()` (`fanstuff.cpp:786-805`): up to 10×100 ms tries.
- Held across whole multi-register transactions: `ReadEcStatus` two-sample read (`fanstuff.cpp:816-832`), `SetFan` write+verify (`:612-669`), `SetHdw` (`:715-758`), and the clean-exit path which holds it and then calls `SetFan` — **relying on recursion** (`fancontrol.cpp:3557-3585`; runbook §8).

### 2.6 Game Mode (`fancontrol.cpp:1672-1804`)

Renames `C:\Windows\System32\drivers\TVicHW64.sys` / `TVicPort64.sys` to `.bak` (WOW64 bypass, rollback on partial failure, `MOVEFILE_DELAY_UNTIL_REBOOT` crash net) so anti-cheat doesn't see the unsigned driver; restores on toggle-off, clean exit (`fancontrol.cpp:989-990`), and shutdown (`:3646-3647`). Startup detects a leftover hidden state (`fancontrol.cpp:156-161`). **Entirely TVicPort-specific.**

### 2.7 Config (`misc.cpp`)

- `ReadConfig` (`misc.cpp:329+`): flat `key=value` parser, `_strnicmp` prefix matches, lines starting `/ # ;` skipped (`:361-362`). `UseTWR=` parsed at `:364-367`. **The main dialog is created inside `ReadConfig`** (`:828-832`), which itself runs inside the `FANCONTROL` ctor — i.e., *config is parsed after the port driver is already open* (§8.2 handles this ordering problem).
- `SaveConfig` (`misc.cpp:39-171`): rewrites only a fixed table of **integer** keys (`:51-72`), passes every unmatched line through untouched (`:146-148`), atomic replace (`:162-170`).
- **°F rule** (runbook §4.8, `memory/config-ini-constraints.md`): `Fahrenheit` is derived (`misc.cpp:848`) and temp-valued keys are stored Celsius-internally — naive write-back corrupts °F configs. `Driver=` is a **plain string key with no temperature semantics**, so: parse it in `ReadConfig`, do **not** add it to `SaveConfig`'s numeric KV table. The passthrough at `misc.cpp:146-148` preserves a hand-written `Driver=` line across saves with zero code. (Same pattern as `UseTWR`, which `SaveConfig` also never writes.)

---

## 3. Architecture overview

A minimal transport-interface seam is introduced **below** `portio.cpp` (the EC protocol layer) and **beside** the TWR fast path. Everything above the seam — handshake, retries, two-sample matching, mode logic, guards — is untouched.

```
                       FANCONTROL  (fanstuff.cpp)
     ReadEcStatus / ReadEcRaw / SetFan / SetHdw          TWR block path
        │  (EcAccess held across transactions)           (fanstuff.cpp:942-991)
        ▼                                                       │
   ReadByteFromEC / WriteByteToEC  (portio.cpp)                 │  direct ReadPort/
   ACPI handshake, TYPE1/TYPE2 detect, m_ecTypeKnown            │  WritePort calls
        │                                                       │  (TVic linkage kept;
        │  g_PortIo->ReadPort8/WritePort8/PortAllowed           │  UseTWR forces TVic,
        ▼                                                       ▼  see §8/§10.1)
  ┌───────────────────────────────────────────────────────────────────┐
  │            IPortIo            (new: portio_backend.h/.cpp)        │
  │  Open  Close  IsOpen  ReadPort8  WritePort8  PortAllowed          │
  │  BeginEcTransaction/EndEcTransaction (mutex hook)   Name          │
  └───────────────┬──────────────────────────────┬────────────────────┘
                  │                              │
     ┌────────────▼─────────────┐   ┌────────────▼──────────────────────┐
     │ PortIoTVic               │   │ PortIoPawn                        │
     │ 1:1 wrapper over the six │   │ CreateFileW(\\.\PawnIO [TBD])     │
     │ TVicPort.lib imports;    │   │ IOCTL_PIO_LOAD_BINARY(blob)       │
     │ Open() folds in the      │   │ IOCTL_PIO_EXECUTE_FN("ioctl_pio_  │
     │ Test/Set/TestHardAccess  │   │   read"/"...write", ULONG64[])    │
     │ dance; PortAllowed=true  │   │ PortAllowed = {0x62, 0x66} only   │
     └────────────┬─────────────┘   └────────────┬──────────────────────┘
                  │                              │
        TVicHW64.sys / TVicPort64.sys      PawnIO.sys  +  LpcACPIEC.bin
        (unsigned, LOLDrivers-flagged)     (MS-signed driver, signed module,
                                            EC ports 0x62/0x66 allowlisted)

  Backend selection (approot.cpp WorkerThread, per §8):
     Driver=tvicport ──────────────────────────────► PortIoTVic
     Driver=pawnio  ───────────────► PortIoPawn (strict; no silent fallback)
     Driver=auto (default):
        UseTWR=1 ──────────────────────────────────► PortIoTVic (forced)
        else: PortIoPawn.Open() + EC probe on 0x66/0x62
                  ok ──► PortIoPawn        fail ──► PortIoTVic
```

New files: `fancontrol.x/portio_backend.h` + `fancontrol.x/portio_backend.cpp` (interface, both backends, selection logic, the raw PawnIO ioctl plumbing, and the standalone EC probe). One global, mirroring today's implicit TVic global state:

```cpp
extern IPortIo* g_PortIo;	// set once by PortIoSelect() before FANCONTROL exists
```

---

## 4. The transport interface — `IPortIo`

Codebase style (tabs, `this->`, PascalCase, Win32 types). The task sketch's `open/read_port/port_allowed` names map 1:1 to `Open/ReadPort8/PortAllowed`.

```cpp
// portio_backend.h
//
// Transport seam under the EC protocol layer. Exactly the six TVicPort calls
// the app has ever used (see design doc §2.1), expressed so a backend that CAN
// fail per-operation (PawnIO: driver gone, port denied) reports it, while the
// TVic wrapper preserves its can't-fail semantics by always returning TRUE.
class IPortIo {
public:
	virtual ~IPortIo() {}

	// Open the driver and make the transport ready for port I/O. Returns FALSE
	// if the driver is missing/unloadable (or, for PawnIO, if the module blob
	// is missing or rejected). Idempotent: Open on an open transport is a no-op
	// success. NOTE: one Open() attempt only - the 180s retry policy stays in
	// WorkerThread (see doc §8.3), which owns the service stop-event interplay.
	virtual bool	Open() = 0;
	virtual void	Close() = 0;
	virtual bool	IsOpen() const = 0;

	// Byte port I/O. FALSE = the TRANSPORT failed (ioctl error, disallowed
	// port, driver gone) - not an EC protocol timeout; those stay upstairs in
	// portio.cpp. TVic backend: always TRUE (hardware I/O cannot report).
	virtual bool	ReadPort8(USHORT port, UCHAR* pdata) = 0;
	virtual bool	WritePort8(USHORT port, UCHAR data) = 0;

	// Capability query: can this backend drive the given port AT ALL?
	// TVic: always TRUE. PawnIO: mirrors the loaded module's allowlist.
	// The EC layer uses this to steer the TYPE1/TYPE2 port-pair choice (§9).
	virtual bool	PortAllowed(USHORT port) const = 0;

	// Cross-process EC-transaction bracket (PawnIO ecosystem "Access_EC"
	// mutex, §11). TVic: no-ops. Taken INSIDE the app's EcAccess mutex, once
	// per ReadByteFromEC/WriteByteToEC body. FALSE from Begin = fail the
	// transaction (caller returns false into the existing retry ladder).
	virtual bool	BeginEcTransaction() = 0;
	virtual void	EndEcTransaction() = 0;

	virtual const char*	Name() const = 0;	// "TVicPort" / "PawnIO" (for logs)
};

extern IPortIo* g_PortIo;
```

### 4.1 Why `ReadPort8`/`WritePort8` return `bool` (recommendation)

TVic's `ReadPort`/`WritePort` are `UCHAR(USHORT)` / `void` — they cannot signal failure. PawnIO's can fail (`DeviceIoControl` returns FALSE: driver unloaded mid-run, `STATUS_ACCESS_DENIED` from the module for a non-allowlisted port). Rather than invent a sentinel byte (0xFF is a *valid* EC value — `ReadEcRaw` even pre-seeds `FanCtrl = 0xFF`, `fanstuff.cpp:849`), the interface returns `bool` with an out-parameter for reads:

- **TVic backend**: `*pdata = ReadPort(port); return true;` — behavior bit-identical to today, no new failure paths.
- **PawnIO backend**: FALSE on ioctl failure or disallowed port. A FALSE inside `WaitForFlags`' poll loop returns FALSE **immediately** (no pointless 1000 ms spin on a dead driver / denied port), which upstream code already interprets as "timed out #N" — flowing into the exact existing vocabulary: `ReadByteFromEC` FALSE → `ReadEcRaw` aborts → `ReadEcStatus` retries 10× (`fanstuff.cpp:823-830`) → `ReadErrorCount`/`MaxReadErrors` → BIOS fallback and exit (runbook §4.5). `SetFan`'s capped verify/retry loop (`fanstuff.cpp:614-667`) likewise absorbs write failures. **No new error-handling machinery is introduced above the seam** (see §12 and risk R3).

The five `void WritePort` call sites in the handshake (`portio.cpp:100,109,161,170,179`) start checking the result: on FALSE, `Trace` + `return false` from `ReadByteFromEC`/`WriteByteToEC` — same contract those functions already have.

---

## 5. TVic backend — `PortIoTVic`

A 1:1 wrapper over the six imports; `TVicPort.lib` stays linked (`fancontrol.vcxproj:203`) and SAFESEH stays off (`:101-103`, LNK2026 note).

```cpp
class PortIoTVic : public IPortIo {
	bool	m_open = false;
public:
	bool	Open() override {
		if (this->m_open) return true;
		if (!OpenTVicPort()) return false;
		// fold in the hard-access dance verbatim from approot.cpp:401-403
		// (results were only ever stored in dead locals there)
		TestHardAccess();
		SetHardAccess(TRUE);
		TestHardAccess();
		this->m_open = true;
		return true;
	}
	void	Close() override { if (this->m_open) { CloseTVicPort(); this->m_open = false; } }
	bool	IsOpen() const override { return this->m_open; }
	bool	ReadPort8(USHORT port, UCHAR* pdata) override { *pdata = ReadPort(port); return true; }
	bool	WritePort8(USHORT port, UCHAR data) override { WritePort(port, data); return true; }
	bool	PortAllowed(USHORT) const override { return true; }
	bool	BeginEcTransaction() override { return true; }	// EcAccess suffices today; unchanged
	void	EndEcTransaction() override {}
	const char*	Name() const override { return "TVicPort"; }
};
```

Decisions:

- **`TestHardAccess`/`SetHardAccess` move into `PortIoTVic::Open()`** and disappear from `WorkerThread`. They are TVic-specific (grant ring-3 hard port access) and their return values are already ignored (`approot.cpp:380-382,401-403`), so folding them in is behavior-preserving and keeps `WorkerThread` backend-agnostic.
- **The 180×1 s retry loop stays in `approot.cpp`** (recommendation): it is not a property of a backend but of *startup* — it exists so a service that starts before the driver stack settles keeps trying (runbook §4.9), and it is interlocked with `g_stopEvent` (`approot.cpp:393-398`) and the SCM stop path (`:283-286`). Moving it into a backend would force the backend to know about service stop events. The loop simply retries the *selection* function instead (§8.3).
- `IsDriverOpened()` remains unused (we track our own flag; one less import to trust).
- Game Mode and `RecoverHiddenDrivers` stay conceptually attached to this backend (§10.2).

---

## 6. PawnIO backend — `PortIoPawn`

Talks to the driver with raw Win32 calls only (constraint #1). Three operations are reimplemented from PawnIOLib:

### 6.1 Open

```cpp
// TBD-impl: exact device path - the Win32 form of k_device_path from PawnIO's
// PawnIOLib.cpp (expected shape: L"\\\\.\\PawnIO"); being confirmed by the
// parallel ABI research task.
static const wchar_t k_pawnio_device[] = L"\\\\.\\PawnIO";	// TBD-impl

bool
PortIoPawn::Open() {
	if (this->m_hDev != INVALID_HANDLE_VALUE) return true;

	// 1) device open == driver presence test. Service "PawnIO" not running /
	//    not installed -> CreateFileW fails -> caller falls back to TVic.
	this->m_hDev = ::CreateFileW(k_pawnio_device,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,	// TBD-impl: match PawnIOLib's share/flags
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);	// synchronous (no OVERLAPPED)
	if (this->m_hDev == INVALID_HANDLE_VALUE) {
		debug("PortIoPawn: PawnIO device not present\r\n");
		return false;
	}

	// 2) load the signed module blob shipped next to the exe. CWD is already
	//    the exe dir (approot.cpp:365-378), but build the absolute path from
	//    GetModuleFileName anyway (belt and braces for odd service CWDs).
	//    Blob missing / unreadable / signature rejected by the driver -> Close
	//    and fail -> TVic fallback.
	if (!this->LoadModuleBlob("LpcACPIEC.bin")) {	// IOCTL_PIO_LOAD_BINARY
		this->Close();
		return false;
	}

	// 3) create/open the ecosystem EC mutex used around EC transactions (§11)
	this->m_hEcMutex = ::CreateMutexW(NULL, FALSE, L"Access_EC");	// TBD-impl: confirm
		// exact convention name/namespace ("Access_EC" vs "Global\\Access_EC")
		// against PawnIO-era FanControl/LHM source. NULL handle is tolerated:
		// Begin/EndEcTransaction degrade to no-ops with a one-shot log line.
	return true;
}
```

`LoadModuleBlob` reads the file into a heap buffer and issues one ioctl: input = the raw signed blob bytes, no output buffer. The blob is **shipped unmodified** from the upstream signed release (§13); the driver verifies the signature at load — never patch it.

### 6.2 Execute — the ABI

Per the PawnIO execute convention (numeric codes `TBD-impl` from the parallel research task):

- `IOCTL_PIO_LOAD_BINARY` — in: blob bytes; out: none.
- `IOCTL_PIO_EXECUTE_FN` — in: **bytes [0..31] = null-terminated ASCII function name (fixed 32-byte field), bytes [32..] = input `ULONG64` array**; out: `ULONG64` array; count via `lpBytesReturned`.

```cpp
#pragma pack(push, 1)
struct PIO_EXEC_IN {
	char	Name[32];	// null-terminated ASCII fn name, zero-padded
	ULONG64	Args[2];	// input cells; only the first InCount*8 bytes are sent
};
#pragma pack(pop)

// One place builds the buffer and calls DeviceIoControl; both port ops use it.
bool
PortIoPawn::Exec(const char* fn, const ULONG64* in, ULONG inCount,
                 ULONG64* out, ULONG outCount) {
	if (this->m_hDev == INVALID_HANDLE_VALUE) return false;
	PIO_EXEC_IN buf; setzero(&buf, sizeof(buf));
	strncpy_s(buf.Name, sizeof(buf.Name), fn, _TRUNCATE);
	for (ULONG i = 0; i < inCount; i++) buf.Args[i] = in[i];
	DWORD cbIn = 32 + inCount * sizeof(ULONG64);
	DWORD cbRet = 0;
	if (!::DeviceIoControl(this->m_hDev, IOCTL_PIO_EXECUTE_FN /* TBD-impl */,
			&buf, cbIn, out, outCount * sizeof(ULONG64), &cbRet, NULL))
		return false;
	return cbRet >= outCount * sizeof(ULONG64);	// got every promised cell
}
```

The 8-byte `ULONG64` cell size is architecture-neutral: the x86 process and x64 driver agree on the layout byte-for-byte, so constraint #1 costs nothing here.

### 6.3 Port I/O mapped onto the LpcACPIEC module

The stock module exports exactly what we need:

| `IPortIo` op | Module fn | In (`ULONG64[]`) | Out (`ULONG64[]`) |
|---|---|---|---|
| `ReadPort8(port, &v)` | `"ioctl_pio_read"` | `[port]` | `[value]` → `*pdata = (UCHAR)out[0]` |
| `WritePort8(port, v)` | `"ioctl_pio_write"` | `[port, value]` | none |

```cpp
bool
PortIoPawn::ReadPort8(USHORT port, UCHAR* pdata) {
	if (!this->PortAllowed(port)) return false;	// fail fast, don't burn an ioctl
	ULONG64 in = port, out = 0;
	if (!this->Exec("ioctl_pio_read", &in, 1, &out, 1)) return false;
	*pdata = (UCHAR)out;
	return true;
}

bool
PortIoPawn::WritePort8(USHORT port, UCHAR data) {
	if (!this->PortAllowed(port)) return false;
	ULONG64 in[2] = { port, data };
	return this->Exec("ioctl_pio_write", in, 2, NULL, 0);
}

bool
PortIoPawn::PortAllowed(USHORT port) const {
	// CRITICAL: mirror the stock LpcACPIEC allowlist exactly - the module
	// permits ONLY the ACPI-spec EC pair. TYPE1 (0x1600/0x1604) and the TWR
	// block window (0x1610-0x161F) are rejected here so the EC layer's
	// port-pair steering (§9) and the UseTWR->TVic rule (§8) can never be
	// silently bypassed by a code path we forgot.
	return port == 0x62 || port == 0x66;
}
```

The local `PortAllowed` pre-check is an optimization *and* a behavioral guarantee (immediate, deterministic failure instead of an ioctl round-trip that ends in `STATUS_ACCESS_DENIED`); the module remains the actual enforcement boundary.

### 6.4 Performance note

Each handshake byte costs ~5–8 port ops; under PawnIO each op is one `DeviceIoControl` (µs-scale syscall + sandboxed interpreter dispatch). The handshake's own pacing — 10 ms sleeps per `WaitForFlags` poll iteration (`portio.cpp:49,60`) and 100/150/200 ms retry backoffs upstairs — dominates by 3–4 orders of magnitude. No measurable poll-cycle impact is expected; validate in G2 (§15).

---

## 7. Where the seam lands in existing code

| Site | Today | After |
|---|---|---|
| `portio.cpp:53` (`WaitForFlags`) | `data = ReadPort(port);` | `UCHAR data; if (!g_PortIo->ReadPort8(port, &data)) return false;` — immediate fail, no timeout spin |
| `portio.cpp:100,109,161,170,179` | `void WritePort(...)` | `if (!g_PortIo->WritePort8(...)) { this->Trace("..."); return false; }` |
| `portio.cpp:127-128` (OBF drain) | `ReadPort` ×2 | `ReadPort8`; on transport failure just skip the drain (the read already failed) |
| `ReadByteFromEC`/`WriteByteToEC` entry/exit | — | `BeginEcTransaction()` / `EndEcTransaction()` bracket (§11) |
| `fanstuff.cpp:961-991` (TWR block) | direct `ReadPort`/`WritePort` | **unchanged** — TVic-only protocol; selection forces TVic when `UseTWR=1`, and `PortAllowed` would reject the ports anyway (defense in depth, §10.1) |
| `approot.cpp:386` | `RecoverHiddenDrivers()` | unchanged — always runs (§10.2) |
| `approot.cpp:388-399` | retry `OpenTVicPort()` | retry `PortIoSelect(...)` (§8.3) |
| `approot.cpp:401-403` | hard-access dance | deleted (moved into `PortIoTVic::Open`) |
| `approot.cpp:416` | `CloseTVicPort()` | `g_PortIo->Close()` |
| `approot.cpp:421-426` | error text names tvicport.sys | text updated to name both backends + the ini override |

---

## 8. Backend selection

### 8.1 Decision table

`Driver=` values are case-insensitive; anything unrecognized → `auto` + a Trace warning.

| `Driver=` | `UseTWR` | PawnIO opens + EC probe (§8.4) | Selected backend | Notes |
|---|---|---|---|---|
| `tvicport` | any | not attempted | **TVicPort** | explicit override |
| `pawnio` | 0 | ok | **PawnIO** | |
| `pawnio` | 0 | fails | **none — open failure** | strict: an explicit override is a debugging/pinning tool; silently substituting TVic would mask the misconfiguration. Flows into the existing failure UX (`approot.cpp:418-427`): retry loop, then error box / service exit code 3 |
| `pawnio` | 1 | — | **TVicPort** + logged warning | conflict: the TWR block path is impossible on the stock module (§2.3). Losing the user's chosen temp path would be a silent functional regression, so `UseTWR` wins; the log says why |
| `auto` (default / missing key) | 1 | skipped entirely | **TVicPort** | pointless to probe PawnIO — TWR needs 0x1604/0x1610-0x161F |
| `auto` | 0 | ok | **PawnIO** | the target state on validated hardware |
| `auto` | 0 | driver absent / module blob missing / load rejected / EC probe fails | **TVicPort** | logged with the specific fallback reason |

### 8.2 The config-ordering problem (important)

Selection needs `Driver=` and `UseTWR=` **before** `FANCONTROL` exists — but `ReadConfig` runs inside the `FANCONTROL` ctor, *after* the driver is opened in `WorkerThread` (`approot.cpp:388-406`), and it even creates the main dialog as a side effect (`misc.cpp:828-832`). Hoisting full `ReadConfig` above the open is far too invasive.

**Design:** a tiny standalone pre-parse in `portio_backend.cpp` — open `TPFanControl.ini` (CWD is already the exe dir, `approot.cpp:365-378`), scan for just `Driver=` and `UseTWR=` with the same `_strnicmp` + comment-skip conventions as `misc.cpp:359-367`, close. `FANCONTROL::ReadConfig` *additionally* parses `Driver=` into a new member (`char DriverChoice[16]` or an enum) so the value is visible to Trace/UI later; the two parses can't disagree because they read the same line the same way. `SaveConfig` never writes the key (§2.7) — no °F hazard, and the user's line (with comments) round-trips untouched via `misc.cpp:146-148`.

### 8.3 Retry-loop mapping (service mode, constraint: §2.4)

```cpp
// approot.cpp WorkerThread - replaces the OpenTVicPort loop at :388-399
RecoverHiddenDrivers();	// always: heals a Game-Mode crash even if PawnIO wins today,
	// so a LATER TVic fallback (or Driver=tvicport edit) still has loadable files

PortIoConfig cfg = PortIoPreparseIni("TPFanControl.ini");	// Driver= + UseTWR= (§8.2)
for (int i = 0; i < 180; i++) {
	if (g_PortIo = PortIoSelect(cfg)) { ok = true; break; }	// one full pass: PawnIO try, TVic try
	if (g_stopEvent && ::WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
	if (!g_stopEvent) ::Sleep(1000);
}
```

Each loop iteration runs **one full selection pass** (PawnIO attempt then — in `auto` — TVic attempt). This preserves the original purpose of the 180 s window for *whichever* backend is slow to appear at boot (TVic driver stack settling, or the PawnIO service starting after ours), keeps the stop-event abort semantics byte-identical, and keeps the service exit-status reporting (`approot.cpp:429-439`) untouched. A PawnIO `CreateFileW` miss costs microseconds per second — negligible.

### 8.4 The EC probe (`auto` acceptance test) and logging

Opening the PawnIO device + loading the module proves the *transport* works — not that **this machine's EC answers on 0x66/0x62** (risk R1). Before accepting PawnIO in `auto` mode, run a self-contained probe in `portio_backend.cpp` (it cannot use `ReadByteFromEC` — no `FANCONTROL` object exists yet, §8.2):

```
PawnIoProbeEc(transport):
	Begin/EndEcTransaction bracket (Access_EC) - EcAccess can't be taken here
	(no FANCONTROL yet); a concurrent second instance is excluded well enough
	by Access_EC for a read-only probe.
	1. ReadPort8(0x66, &status)          - transport check; FALSE -> probe fails
	2. wait IBF|OBF clear on 0x66        - short budget: ~250 ms, 10 ms steps
	3. WritePort8(0x66, 0x80)            - ACPI READ command
	4. wait IBF clear                    - ~250 ms
	5. WritePort8(0x62, 0x2F)            - TP_ECOFFSET_FAN (fanstuff.cpp:45)
	6. wait OBF set                      - ~250 ms
	7. ReadPort8(0x62, &value)           - any returned byte = EC ALIVE on this pair
	One retry on timeout (mirrors the EC's occasional busy phase). Total worst
	case ~1.5 s added to startup before TVic fallback - acceptable, logged.
```

Success criterion is deliberately loose (a completed handshake, not a specific value): `0x2F` is read-only-safe and mode-agnostic. On success, the probe's port pair is **pre-seeded** into the EC layer (§9).

**Logging:** the chosen backend and the *reason* are recorded twice — `debug()` from `WorkerThread` (worker-side file log, `approot.cpp:442-449`) and `FANCONTROL::Trace` once the object exists (e.g., in the ctor next to the existing "Current Config" block, `misc.cpp:738-739`):
`"PortIo backend: PawnIO (LpcACPIEC.bin, EC on 0x66/0x62)"` / `"PortIo backend: TVicPort (PawnIO probe failed: EC silent on 0x66/0x62)"` / `"PortIo backend: TVicPort (forced: UseTWR=1)"` etc.

---

## 9. EC-type constraint under PawnIO (read this section twice)

`portio.cpp` **prefers TYPE1** (`0x1604/0x1600`): both `ReadByteFromEC` and `WriteByteToEC` lazily initialize to TYPE1 (`portio.cpp:71-75, 148-152`) and only fall to TYPE2 (`0x66/0x62`) after a probe timeout while `m_ecTypeKnown` is still false (`:84-95`). The stock PawnIO module allowlists **only 0x62/0x66** — so with PawnIO active, the handshake *must* run on the TYPE2 pair, and TYPE1 must fail *cleanly and instantly*, never poisoning the transport detection.

Two mechanisms, belt and suspenders:

1. **Pre-seed (primary, fast path).** After backend selection, before any EC traffic, steer the pair. In the `FANCONTROL` ctor (right after the member zero-init, `fancontrol.cpp:92-93`):

	```cpp
	// PawnIO's module only permits the ACPI-spec EC pair; don't let the lazy
	// TYPE1 default (portio.cpp:71-75,148-152) burn a probe cycle per boot.
	if (g_PortIo && !g_PortIo->PortAllowed(ACPI_EC_TYPE1_CTRLPORT)) {
		this->EC_CTRL = ACPI_EC_TYPE2_CTRLPORT;	// 0x66
		this->EC_DATA = ACPI_EC_TYPE2_DATAPORT;	// 0x62
		this->Trace("EC ports steered to TYPE2 (backend allowlist)");
	}
	```

	(Requires the two port constants to move from `portio.cpp:24-28` into a header both files see — mechanical.) `m_ecTypeKnown` stays false until the first successful read latches it (`portio.cpp:134`) — the H-02 protection is preserved verbatim.

2. **`PortAllowed` fail-through (safety net).** If any path still drives TYPE1 first (e.g., the lazy init in `WriteByteToEC` on a hotkey racing the ctor steering — it can't, the ctor runs first, but assume nothing): `ReadPort8(0x1604, …)` returns FALSE → `WaitForFlags` returns FALSE **immediately** (no 1000 ms spin) → `ReadByteFromEC`'s existing "timed out #1" branch flips to TYPE2 while `!m_ecTypeKnown` (`portio.cpp:84-95`) → the *next* transaction succeeds on TYPE2 and latches. Net cost: one burned retry inside `ReadEcStatus`'s 10-try loop (`fanstuff.cpp:823-830`). The detect logic needs **zero changes** — the allowlist makes TYPE1 look like a permanently silent EC, which the code already handles.

Corollary (documented prominently): **on hardware whose EC does *not* respond on 0x66/0x62, the stock PawnIO module cannot drive fan control at all** — the `auto` probe (§8.4) exists precisely to detect this before PawnIO is accepted. See risk R1.

---

## 10. Subsystem impact map

### 10.1 `UseTWR` (functional preservation)

- Selection rule: `UseTWR=1` → TVic, always (table §8.1). Under `auto` PawnIO isn't even probed; under explicit `Driver=pawnio` the conflict resolves to TVic with a warning (regression-avoidance beats override strictness for this one key, since honoring `pawnio` would silently change *which temperatures the fan logic sees*).
- The TWR block code (`fanstuff.cpp:942-991`) keeps calling `ReadPort`/`WritePort` directly — a deliberate, documented bypass of the seam: it is a TVic-only protocol, the selection rule guarantees TVic is open, and leaving it textually untouched makes the no-regression claim verifiable by diff.
- If a future custom ThinkPad module allowlists `0x1600/0x1604 + 0x1610-0x161F` (risk R1 mitigation (a)), the TWR block can be ported onto the seam then — out of scope now.

### 10.2 Game Mode

- The rename hack (`fancontrol.cpp:1672-1804`) hides `TVicHW64.sys`/`TVicPort64.sys` from anti-cheat. With PawnIO active we never load those files, and PawnIO itself is Microsoft-signed — anti-cheat has nothing to object to. **Game Mode becomes TVic-backend-only:**
	- `ToggleGameMode` gains an early exit + trace when `g_PortIo->Name()` is not TVicPort; the dialog checkbox (id 7013, synced at `fancontrol.cpp:1591,1802-1803`) is disabled (`EnableWindow(FALSE)`) with its tooltip explaining "not needed under PawnIO".
	- The exit/shutdown restore calls (`fancontrol.cpp:989-990, 3646-3647`) are gated on `m_driversHidden`, which can only become true via the toggle — under PawnIO they naturally no-op. The ctor's hidden-state detection (`fancontrol.cpp:156-161`) stays: it only reads file attributes.
	- **`RecoverHiddenDrivers()` keeps running unconditionally at startup** (`approot.cpp:386`): restoring `.sys.bak` files is always safe, and a machine that boots into PawnIO today may need the TVic fallback tomorrow (PawnIO uninstalled, `Driver=tvicport` edit). Removing it would trade a working recovery path for nothing.

### 10.3 Service mode

Covered by §8.3: the 180 s loop retries the full selection each second and keeps its `g_stopEvent` abort and `SERVICE_STOPPED` reporting (`approot.cpp:429-439`) untouched. The interactive-failure message (`:421-426`) is reworded to name both drivers and the `Driver=` key. `Runs_as_service` detection (`misc.cpp:840-845`) is unaffected.

### 10.4 Sleep / resume

No backend hooks needed. The PawnIO device handle (and its per-handle module instance) survives S0/suspend like any kernel device handle; the app already defers EC traffic ~10 s after resume (`m_ecResumeDeferUntil`, `fancontrol.h:438`; runbook §4.6). If the handle ever goes bad (driver crash/stop while asleep), every op fails → the `MaxReadErrors` ladder falls back to BIOS and exits — the same contract TVic has today. No re-open-on-failure logic is added in v1 (keep the failure semantics identical; revisit only if field data demands it).

### 10.5 Clean-exit path

`fancontrol.cpp:3557-3585` (hold `EcAccess` → `SetFan("On close", FAN_CTRL_BIOS)` → recursion) is untouched: the recursion property belongs to `EcAccess` (`winstuff.cpp`), not the transport. The final `SetFan` under PawnIO runs the same bracketed transactions as any other write. `g_PortIo->Close()` happens after the dialog loop exits (`approot.cpp:416` replacement) — same ordering as `CloseTVicPort` today.

---

## 11. Concurrency: two overlapping EC mutexes

Two named mutexes now guard EC access:

| Mutex | Name | Scope | Who honors it |
|---|---|---|---|
| App-level (existing) | `Access_Thinkpad_EC` (`winstuff.h:76`) | whole multi-register transactions: 2-sample poll, write+verify, close-path (`fanstuff.cpp:816-832, 612-669, 715-758`; `fancontrol.cpp:3557-3585`) | this app's GUI + service instances |
| Ecosystem (new) | `Access_EC` (PawnIO convention; exact name/namespace `TBD-impl`, §6.1) | **one EC byte-transaction**: bracketed around each `ReadByteFromEC` / `WriteByteToEC` body via `Begin/EndEcTransaction` | other PawnIO-era EC tools (FanControl, LHM, …) |

**Composition rules (the deadlock analysis):**

1. **Strict acquisition order: `EcAccess` (outer) → `Access_EC` (inner).** The inner mutex is acquired and released entirely within one `ReadByteFromEC`/`WriteByteToEC` call, which always executes under an already-held `EcAccess` (every call site sits inside a `LockECAccess`/`FreeECAccess` or the recursive close-path hold). No code path takes `EcAccess` while holding `Access_EC`, so a lock-order cycle is impossible *within this app*.
2. **Bounded inner wait.** `BeginEcTransaction` waits ≤ ~500 ms; on timeout it returns FALSE and the transaction fails into the normal retry ladder. Rationale: a wedged third-party tool holding `Access_EC` must degrade us to "EC reads failing" (which the app already survives, up to the BIOS-fallback guard) — it must never freeze a thermal-safety app indefinitely. This intentionally mirrors `LockECAccess`'s own bounded 10×100 ms pattern (`fanstuff.cpp:786-797`).
3. **`WAIT_ABANDONED` = acquired**, same reasoning (and comment) as `MUTEXSEM::Lock` (`winstuff.cpp:56-60`): a crashed owner must not permanently poison EC access.
4. **Granularity trade-off, made explicit:** holding `Access_EC` for our *entire* multi-register poll (up to seconds with retries) would starve well-behaved PawnIO tools, while per-single-port-I/O granularity would let another process interleave *mid-handshake* and corrupt the EC state machine. Per-byte-transaction is the ecosystem-compatible middle: each `ReadByteFromEC`/`WriteByteToEC` is an atomic handshake unit. Between two of our bytes another tool may run a transaction — safe, because every transaction begins by waiting for IBF/OBF quiescence (`portio.cpp:78, 155`). Our own two-sample consistency gate (`fanstuff.cpp:823-830`, `SampleMatch`) additionally rejects torn multi-register snapshots — it exists precisely because port-level interleaving already happens today (comment at `fanstuff.cpp:818-821`).
5. **TVic backend:** `Begin/EndEcTransaction` are no-ops — behavior today, preserved. (Extending `Access_EC` to the TVic backend would be a *new* behavior with new stall modes; explicitly out of scope for the no-regression release. Revisit later if PawnIO-era tools coexisting with TVic mode becomes a real scenario.)
6. **Session-namespace caveat (pre-existing, documented not fixed):** unqualified `"Access_Thinkpad_EC"` lands in the per-session namespace, so a session-0 service and a session-1 GUI hold *different* mutexes today. For `Access_EC` we follow whatever the ecosystem convention actually is (`Global\` expected; the app already runs elevated, so `SeCreateGlobalPrivilege` is available) — `TBD-impl` verification item. Do **not** "fix" `Access_Thinkpad_EC`'s namespace in this change; it would alter GUI/service interlock behavior unrelated to the port.

---

## 12. Error-semantics design (transport failures vs EC timeouts)

Principle: **transport failures are folded into the existing EC-timeout vocabulary; nothing above `portio.cpp` learns a new failure kind.**

- Read path: `ReadPort8` FALSE inside `WaitForFlags` → immediate FALSE → `"readec: timed out #N"` traces → `ReadByteFromEC` FALSE → `ReadEcRaw` aborts (`fanstuff.cpp:852-897`) → `ReadEcStatus` 10× retry → `ReadErrorCount++` → after `MaxReadErrors`, BIOS fallback + exit (runbook §4.5). Identical ladder, identical UX.
- Write path: the five handshake `WritePort` sites gain result checks (§7) → `WriteByteToEC` FALSE → `SetFan`'s capped verify/retry (`fanstuff.cpp:614-667`) → "Result: failed" trace → mode logic unchanged. The fail-safe and stall-watchdog guards operate on the same booleans they see today.
- Distinguishing "port denied" from "driver vanished" is *not* surfaced upstream (upstream has no use for it); it **is** logged inside the backend (one-shot per failure kind) so support logs tell the difference.
- The single genuinely new decision: a `PortAllowed`-rejected port fails **without** the 1000 ms `WaitForFlags` spin. This strictly reduces stall time and is required for the §9 fail-through to be cheap.

---

## 13. Module deployment

- **Ship `LpcACPIEC.bin`** — the signed module from the upstream **PawnIO.Modules `release_0_2_9.zip`** — next to `TPFanControl.exe` (Release output dir + release zip + any installer). Ship the release `.bin` byte-for-byte; the driver verifies its signature at `IOCTL_PIO_LOAD_BINARY` time, so any patching bricks the load.
- The PawnIO **driver** itself is *not* shipped or installed by us: it is a system-wide component with its own signed installer/service ("PawnIO"). It is a deployment *prerequisite* for the PawnIO backend, exactly as the TVic `.sys` files are for TVicPort today (runbook §8 "Needs Administrator + TVicPort driver").
- **Missing blob** ⇒ `PortIoPawn::Open()` fails at the load step ⇒ `auto` falls back to TVic with the reason logged; `Driver=pawnio` fails startup with the standard error UX (§8.1). No new dialog boxes.
- Version skew: pin the tested pair (PawnIO driver version ↔ module release) in the release notes; see risk R6.
- Docs to touch when implementing: root/`fancontrol.x` READMEs, `DEVELOPER_RUNBOOK.md` §4.3/§8, `SMOKE_TEST.md` (§15 additions), and the default ini template comment block (`misc.cpp:745-814`) gains a `Driver=auto` line with a one-line comment.

---

## 14. Risk register (ranked)

| # | Risk | Likelihood | Impact | Mitigation / disposition |
|---|---|---|---|---|
| **R1** | **P15G2's EC does not answer on the legacy 0x66/0x62 pair** — the only ports the stock module allows. This fork *prefers* TYPE1 0x1600/0x1604 (`portio.cpp:24-28,71-75`) and labels 0x66/0x62 "V0.6.2 final" legacy; whether this machine's EC still services the ACPI-spec pair is **unproven**. If it doesn't, the stock PawnIO module cannot drive this laptop, period. | **Unknown — THE gate** | PawnIO backend useless on target HW | **Must-test-first** (G0, §15) before any further implementation effort. If G0 fails: (a) author a ThinkPad PawnIO module allowlisting `0x1600/0x1604 + 0x1610-0x161F` and PR it to PawnIO.Modules (upstream must sign — lead time, review risk), or (b) accept permanent TVic fallback on this hardware — `auto` already does this with zero code beyond this design. Sub-risk even if G0 passes: 0x66/0x62 is *shared with the OS ACPI EC driver* (acpi.sys transactions interleave at port level — the same contention the code already documents at `fanstuff.cpp:818-821` and mitigates with two-sample matching + `SetFan` read-back verify). Reliability on 0x66/0x62 may be *worse* than TYPE1 even when it works; G1 soak measures it. |
| **R2** | Two overlapping EC mutexes (`Access_Thinkpad_EC` + `Access_EC`) deadlock or starve | Low (by construction) | UI/worker freeze | §11: single invariant order (EcAccess outer → Access_EC inner), inner never held across an outer acquire, bounded inner wait (≤500 ms) failing into the existing retry ladder, `WAIT_ABANDONED` treated as acquired (`winstuff.cpp:56-60` precedent). Close-path recursion (`fancontrol.cpp:3557` → `SetFan` re-lock) only involves the *outer* recursive mutex — unchanged. Residual: a hostile/hung `Access_EC` holder degrades polls to failures → existing MaxReadErrors ladder, by design. |
| **R3** | Error-semantics change: TVic's `void` writes → `bool` transport ops could perturb the retry/BIOS-fallback logic | Low | behavior drift in edge paths | §4.1/§12: TVic wrapper returns constant TRUE (bit-identical behavior); PawnIO failures alias to the pre-existing "timed out" vocabulary; no upstream code learns new states. The one intentional delta (instant fail on denied port vs 1000 ms spin) strictly shortens stalls. Unit-testable off-hardware with a mock `IPortIo`. |
| **R4** | Loss of the `UseTWR` fast path under PawnIO | Certain (stock module) | none if rule holds | Mitigated by construction: `UseTWR=1` forces TVic in *all* Driver modes (§8.1), TWR code untouched (§10.1), and `PortAllowed` rejects 0x1604/0x1610-0x161F as a second fence. Residual: TWR users get no PawnIO benefit until a custom module (R1-a) exists — documented in the ini comment. |
| **R5** | Service/elevation surface changes | Low | startup failures | PawnIO's device is admin-ACL'd; the app already requires admin for TVic + Game Mode (`fancontrol.cpp:1763`, runbook §8) — **no new UAC surface**. Service start ordering (our service before PawnIO's) is absorbed by the existing 180 s retry, now per-selection-pass (§8.3). x86→x64 ioctl marshaling is safe (fixed-size ULONG64 cells, §6.2). |
| **R6** | PawnIO driver/module version skew: execute ABI, IOCTL numbers, or blob-signature policy changes across PawnIO releases | Medium over time | backend stops opening | Pin and document the tested driver+module pair (§13); all numeric ABI facts are already quarantined behind `TBD-impl` constants in one file (`portio_backend.cpp`). Failure mode is clean: open/load fails → TVic fallback + log. Add a driver-version query to the open log if the ABI exposes one (`TBD-impl`). |
| **R7** | Concurrent handles: GUI + service instances each open the device and load their own module instance | Low | open failure or cross-instance interference | Verify PawnIO supports concurrent handles with per-handle module instances (`TBD-impl`; PawnIOLib's design implies yes). EC-level interleaving between our two instances is already serialized per-transaction by `Access_EC` (and per-session by `Access_Thinkpad_EC`, with the pre-existing session-namespace caveat noted in §11.6). |

---

## 15. Validation plan

- **G0 — hardware gate (BEFORE building the full backend):** on the physical P15G2, with PawnIO + `LpcACPIEC.bin` installed, run a throwaway probe (a ~100-line console tool built from §6's `Exec` sketch, or a temporary `-probeec` switch): execute the §8.4 handshake for EC reg `0x2F` on 0x66/0x62 and compare the byte against a TVic TYPE1 read taken seconds apart (in BIOS mode both should show `0x80` set — `FAN_CTRL_BIOS`, `fancontrol.h:43`). **Pass ⇒ proceed. Fail ⇒ stop; decide R1(a) vs R1(b).**
- **G1 — soak:** 24 h Smart-mode run under PawnIO; compare EC read-error counts (`m_ecErrorsTotal`, `fancontrol.h:139`) and `SampleMatch` retry rates against a TVic baseline (quantifies the R1 contention sub-risk).
- **G2 — functional matrix (extend `SMOKE_TEST.md`):** dual-fan RPM + Fan1/Fan2 select under PawnIO; BIOS/Smart/Manual transitions; sleep/resume (both S3-style and Modern Standby paths); service install/run/stop under each `Driver=` value; `Driver=pawnio` with driver stopped (strict-failure UX); blob file deleted (auto → TVic + correct log line); `UseTWR=1` forcing TVic in all modes; Game Mode checkbox disabled under PawnIO, still functional under TVic; kill the PawnIO service mid-run → MaxReadErrors → BIOS fallback + exit; GUI + service simultaneously (mutex contention, R7).
- **Off-hardware:** a mock `IPortIo` unit-tests the §9 steering (TYPE1 rejected → TYPE2 latch), the §12 failure funnel, and the selection table §8.1 — none of that needs an EC. (Same spirit as `tests/fanlogic_tests.cpp`.)
- Regression check for constraint #2: with `Driver=tvicport`, a binary diff of EC-visible behavior (poll traces, SetFan traces) against the pre-change build should be noise-free.

## 16. Open items (`TBD-impl` — from the parallel ABI research task; none block this design)

1. Exact Win32 device path (`k_device_path` from PawnIO's PawnIOLib.cpp).
2. Numeric `CTL_CODE` values for `IOCTL_PIO_LOAD_BINARY` / `IOCTL_PIO_EXECUTE_FN`.
3. `CreateFileW` share/flags used by PawnIOLib (match them).
4. Exact ecosystem EC-mutex name/namespace (`Access_EC` vs `Global\Access_EC`) as used by PawnIO-era FanControl/LHM.
5. Concurrent-handle semantics + any version-query ioctl (R6/R7).

## 17. Change inventory (implementation checklist)

| File | Change |
|---|---|
| `portio_backend.h/.cpp` (new) | `IPortIo`, `PortIoTVic`, `PortIoPawn`, `g_PortIo`, ini pre-parse, `PortIoSelect`, `PawnIoProbeEc`, TBD-impl constants |
| `portio.cpp` | calls via `g_PortIo` (bool-aware, §7); port-pair constants exported to the shared header |
| `fancontrol.h` | `DriverChoice` member; (constants move) |
| `fancontrol.cpp` | ctor: TYPE2 pre-seed (§9), backend Trace; `ToggleGameMode` backend gate + checkbox disable (§10.2) |
| `fanstuff.cpp` | **no changes** (TWR block untouched by design) |
| `approot.cpp` | retry loop → `PortIoSelect`; hard-access dance removed; `Close()`; error text |
| `misc.cpp` | `ReadConfig`: parse `Driver=` (string, no °F conversion, not in SaveConfig's KV table); default-ini template comment |
| `fancontrol.vcxproj` | add the two new files; linkage unchanged (TVicPort.lib stays; SAFESEH stays off) |
| Packaging | ship `LpcACPIEC.bin`; docs: READMEs, `DEVELOPER_RUNBOOK.md`, `SMOKE_TEST.md` |
