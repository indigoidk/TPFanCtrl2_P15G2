# PawnIO Port-I/O Backend — Design Document

**Status:** REVISED post-review (v2) — implementation-ready; all v1 `TBD-impl` ABI items resolved and runtime-verified
**Scope:** `fancontrol.x/` only (the live tree). Version baseline: `2.34 P15G2 Dual` (`fancontrol.h:29`).
**Review provenance:** two-pass cross-model review (Codex findings #1–#16, Fable adjudication + gaps A–H), maintainer architecture decisions folded in. Finding tags (`[#5]`, `[gap D]`, …) refer to that review record.
**Author's note:** every claim about existing behavior is cited as `file:line` against the current tree. Line numbers drift; search by symbol if stale.

---

## Changes from v1 (review-driven)

| Change | Driver | Summary |
|---|---|---|
| **PawnIO is now the PRIMARY/default backend** | maintainer decision | The point of this work is that users never have to install the unsigned, LOLDrivers/AV/anti-cheat-flagged TVicPort driver. v1 framed PawnIO as "a second backend"; v2 frames TVicPort as the optional legacy fallback. |
| **Game Mode is removed entirely** | maintainer decision (reverses review finding #12) | Game Mode's sole purpose is hiding the flagged TVic `.sys` files from anti-cheat (`fancontrol.cpp:1668-1669` comment). Under a Microsoft-signed primary backend that purpose is obsolete. Deleting it also dissolves the Game-Mode interactions the review flagged (#5's hidden-driver danger, `RecoverHiddenDrivers`, mixed-backend hidden-file states). Full delete inventory in §10.2. |
| **TVic becomes a delay-loaded (LoadLibrary/GetProcAddress) fallback** | #7 + gap E | Today `TVicPort.lib` is a load-time import (`fancontrol.vcxproj:203`) — a missing/quarantined `TVicPort.dll` aborts exe launch before `WinMain`. v2 resolves the six used functions dynamically, drops the `.lib`, and **re-enables `/SAFESEH`** (the `.lib` was the only blocker, `fancontrol.vcxproj:101-103`). §5. |
| **Transport-lost status + un-gated exit paths + one bounded reopen** | #5 (blocking), gaps B, C | v1 falsely claimed a dead PawnIO handle funnels into "BIOS fallback + exit, same as TVic." In truth every exit path is gated on a *successful write through the dead handle* (`fancontrol.cpp:3852-3859`, `:3566-3584`, `:3817-3824`). v2 adds a distinct transport-lost state that un-gates those paths and allows exactly one reopen attempt. Codex's runtime PawnIO→TVic migration is **rejected** (silently loads the flagged driver). §12, §10.5. |
| **`StartService("PawnIO")` + auto-mode grace window** | gaps A, D (blocking) | PawnIO's service is demand-start; a `CreateFileW`-only open silently runs TVic forever on a healthy PawnIO machine, and v1's per-pass fallback let a PawnIO service 1 s late lose to TVic on boot iteration 1 — every boot. v2: `Open()` attempts SCM start; `auto` gives PawnIO an exclusive grace window before TVic is tried. §6.1, §8.3. |
| **Canonical config snapshot** | #6 (blocking) | v1's "tiny pre-parse + ReadConfig parses again" could disagree (duplicate keys, `UseTWR=2`, mid-window file edits). v2: one snapshot, one parser, passed into `FANCONTROL`; `UseTWR` truth test is `atoi() != 0` (matching `fanstuff.cpp:905`). §8.2. |
| **ABI constants inked (runtime-verified)** | #1, #4-name | Device path `\\?\GLOBALROOT\Device\PawnIO` (the `\\.\PawnIO` DOS link is **not** created by the installed driver), IOCTLs LOAD `0xA1B22084` / EXEC `0xA1B22104` / VERSION `0xA1B22184`, share flags R|W|DELETE, mutex `Global\Access_EC`. VERSION is queried and logged at open (v2.1.0 verified on the target P15G2). All v1 `TBD-impl` markers removed. §6. |
| **`Access_EC` held across logical groups; RAII guard; degrade-if-absent** | #3, #4-rest | Per-byte holds let a concurrent writer flip the persistent fan selector (EC reg 0x31) between our select and act. v2 brackets whole selector-dependent groups (one `ReadEcRaw` sample, one `SetFan` attempt) and releases across retry backoffs. v1 §11.4's claim that `SampleMatch` "rejects torn multi-register snapshots" was **false** (it compares only `FanCtrl`, `fanstuff.cpp:766-780`) and is deleted. Mutex-create failure degrades with a one-shot log — it never refuses to run (rejecting Codex's fail-selection policy). §11. |
| **Hardened EC probe + quantitative G1 gates** | #2, gap G | One-byte probe replaced by N-stable-reads with defined pass/fail; G1 soak gets numeric thresholds (MaxReadErrors-escalation ceiling, SampleMatch retry-rate ceiling). §8.4, §15. |
| **EC port pair authoritative after probe; ctor anchor fixed** | #8, gap F | The probed pair is passed into `FANCONTROL` and latched (`m_ecTypeKnown = true`) for a restricted backend; pair selection is centralized for read *and* write; the pre-seed runs after `InitializeCriticalSection(&m_logLock)` (`fancontrol.cpp:131`) and before `ReadConfig` (`:289`) — v1's ":92-93" anchor was inside the initializer list. §9. |
| **`LpcACPIEC.bin` licensing/packaging** | #14 | The module is LGPL-2.1, not public domain. Copy rule into every Win32 output/package, pinned SHA-256 verification, license text + provenance shipped separately from the app's own license. §13. |
| **Rejected review items (recorded)** | adjudication | #16 (PnP "Degraded" gating — vacuous, the design never gated on PnP state), #5's runtime driver migration, #9's phase-tracked EC resync (quiescence wait + H-01 drain + retry ladder already cover mid-handshake aborts; only the transport-lost distinction survives), #4's fail-on-mutex-failure. §18. |

---

## 1. Goal and non-negotiable constraints

Make **PawnIO** (Microsoft-signed driver executing signed, sandboxed Pawn modules) the **primary** port-I/O backend, so that a default install never needs the EnTech **TVicPort** kernel driver (unsigned, LOLDrivers-flagged, AV/anti-cheat-triggering). TVicPort remains available only as an explicitly-selectable / auto-fallback legacy path.

Hard constraints:

1. **Win32/x86 build is preserved.** The project builds Win32 only (`fancontrol.vcxproj:3-11`; runbook §5). `PawnIOLib.dll` is x64-only, so the backend talks to the driver **directly** via `CreateFileW` + `DeviceIoControl`, reimplementing the four calls we need: open, load module, execute function, query version. Device I/O is bitness-agnostic and the execute ABI is fixed-size `ULONG64` cells — x86→x64 marshaling is a non-issue (no pointers cross the boundary; METHOD_BUFFERED handles alignment).
2. **Zero functional regression *except Game Mode*, which is deliberately removed** (maintainer decision, §10.2). BIOS/Smart/Manual modes, dual-fan select/tach (`fanstuff.cpp:45-51`, `842-897`), the `UseTWR` block-read temp path (`fanstuff.cpp:905, 942-991`), service mode (`approot.cpp:200-311`), and sleep/resume (`fancontrol.h:433-441`) keep working exactly as today.
3. **Selection PawnIO-first** with the ini override `Driver=auto|pawnio|tvicport` (default `auto`): start/open/probe PawnIO, with a grace window before TVicPort is even attempted (§8). TVicPort must be loadable *late* (delay-loaded), never required for exe launch.
4. **No mid-session backend migration.** The selected backend is fixed for the process lifetime. A transport that dies mid-run leads to a clean, un-gated exit — firmware then owns the fans (§12).

---

## 2. Current state — grounded inventory

### 2.1 The TVicPort API surface actually used

`TVicPort.h` declares ~45 functions; grep of `fancontrol.x/*.cpp` shows the app calls exactly **six**:

| Function | Declared | Called from |
|---|---|---|
| `OpenTVicPort()` | `TVicPort.h:39` | `approot.cpp:389` (180×1 s retry loop) |
| `CloseTVicPort()` | `TVicPort.h:37` | `approot.cpp:416` |
| `TestHardAccess()` | `TVicPort.h:43` | `approot.cpp:401,403` |
| `SetHardAccess(BOOL)` | `TVicPort.h:45` | `approot.cpp:402` |
| `UCHAR ReadPort(USHORT)` | `TVicPort.h:47` | `portio.cpp:53,127,128,132`; `fanstuff.cpp:961,965,970,981,990` (TWR) |
| `void WritePort(USHORT, UCHAR)` | `TVicPort.h:49` | `portio.cpp:100,109,161,170,179`; `fanstuff.cpp:969,975,978` (TWR) |

`IsDriverOpened` (`TVicPort.h:41`) and every W/L/FIFO/mem/LPT variant (`TVicPort.h:51-129`) are declared but never called. Six functions is a trivially small surface to resolve via `GetProcAddress` — which is exactly what §5 does. Note the pre-existing hardening comment at `approot.cpp:10-15`: `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` deliberately does **not** cover the load-time `TVicPort.dll` import; converting to an explicit-full-path `LoadLibraryW` closes that gap too (§5).

### 2.2 The EC ACPI handshake (`portio.cpp`)

- Two candidate EC port pairs (`portio.cpp:24-28`): **TYPE1** (preferred, "V0.6.3+ V.2.2.0+"): CTRL `0x1604`, DATA `0x1600`; **TYPE2** (legacy, ACPI-spec standard): CTRL `0x66`, DATA `0x62`.
- `WaitForFlags()` (`portio.cpp:45-63`) polls the status port via `ReadPort` every 10 ms up to a timeout (default 1000 ms).
- `ReadByteFromEC` (`portio.cpp:68-136`): lazily initializes to TYPE1 on first use (`:71-75`); on the first wait timeout flips to the alternate pair **only while `m_ecTypeKnown` is false** (`:84-95`); the first successful read latches `m_ecTypeKnown = true` (`:134`, the H-02 fix). It drains a late OBF byte on the read-timeout path (`:122-129`, H-01 fix).
- `WriteByteToEC` (`portio.cpp:141-188`) mirrors the handshake and duplicates the lazy TYPE1 init (`:148-152`) because a `SetFan()` can be the very first EC access. §9 centralizes this duplication.
- All `WritePort` calls in the handshake are fire-and-forget `void`; only flag waits can fail today.

### 2.3 The `UseTWR` alternate temp path (`fanstuff.cpp:905, 942-991`)

When `UseTWR` is nonzero (parsed via `atoi` at `misc.cpp:364-367`, default 0 at `fancontrol.cpp:116`; the consuming test is `if (!this->UseTWR)` at `fanstuff.cpp:905`, i.e. **any nonzero value enables TWR** — `UseTWR=2` included, per finding #6), `ReadEcRaw` runs a block protocol with *direct* port I/O: polls `ReadPort(0x1604)` (`:961`), drains `0x161f` (`:965`), commands `WritePort(0x1610, 0x20)` (`:969`), zero-fills (`:974-976`), terminates (`:978`), reads 16 bytes from `0x1610..0x161f` (`:989-991`). These ports are **outside the stock PawnIO module's allowlist** ⇒ `UseTWR≠0` forces the TVic backend (§8.1).

### 2.4 Startup / open sequence and service interplay (`approot.cpp:349-440`)

`WorkerThread()`: `SetCurrentDirectory(<exe dir>)` (`:365-378`); `RecoverHiddenDrivers()` (`:319-347`, called at `:386` — **deleted in v2**, §10.2); 180×1 s retry of `OpenTVicPort()` (`:388-399`) aborting on the service stop event (the SCM stop handler sets it, `:285-286`); hard-access dance with dead-local results (`:401-403`); dialog loop; `CloseTVicPort()` (`:416`); interactive error box hardcoding "tvicport.sys" (`:421-426`); service exit-status reporting with specific code 3 (`:429-439`).

### 2.5 Concurrency (`winstuff.*`, `fanstuff.cpp`)

`MUTEXSEM` wraps a named Win32 mutex — default `"Access_Thinkpad_EC"` (`winstuff.h:76`, impl `winstuff.cpp:39-68`); recursive per-thread, cross-process, `WAIT_ABANDONED` treated as acquired (`winstuff.cpp:56-60`). `FANCONTROL::EcAccess` (`fancontrol.h:207`) is held across whole multi-register transactions: the two-sample poll (`fanstuff.cpp:816-832`), `SetFan` write+verify (`:612-669`), `SetHdw` (`:715-758`), and the clean-exit path, which relies on the recursion (`fancontrol.cpp:3553-3587`; runbook §8). Two pre-existing session-namespace caveats, documented **not** fixed here: unqualified `"Access_Thinkpad_EC"` is per-session, and so is the single-instance mutex `"TPFanControlMutex01"` (`approot.cpp:17`) — which is what makes a GUI+service dual-run reachable at all (gap H; feeds risk R7).

### 2.6 Game Mode — inventory of every site (all deleted in v2, §10.2)

The rename hack hides `TVicHW64.sys`/`TVicPort64.sys` from anti-cheat. Sites: `RecoverHiddenDrivers` (`approot.cpp:313-347` + call `:384-386`); `ToggleGameMode` (`fancontrol.cpp:1671-1804`, decl `fancontrol.h:278`); ctor hidden-state detection (`fancontrol.cpp:144-162`); `m_driversHidden` (`fancontrol.h:332`); destructor restore (`fancontrol.cpp:989-990`); `WM_ENDSESSION` restore (`:3646-3647`); checkbox id 7013 (tooltip `:420-423`, sync `:1591`, layout row `:2001`, command handler `:3467-3468`, resource `res\FanControl.rc:72,151`); tray-menu item 5090 (checkmark `fancontrol.cpp:895-896`, handler `:3534`, resource `res\FanControl.rc:327`); tray-tooltip "Game mode" tag (`fanstuff.cpp:305-308`).

### 2.7 Config (`misc.cpp`) — parser semantics that the snapshot must mirror

- `ReadConfig` (`misc.cpp:328+`): flat `key=value` parser. Exact rules (these define §8.2's shared parser): lines are read whole via `fgets` into a 1 KB buffer; a line is a comment **only if its first character** is `/`, `#`, or `;` (`:361-362`); keys match **anchored at column 0**, case-insensitive prefix (`_strnicmp(buf, "UseTWR=", 7)`, `:364`) — an indented key line matches nothing and is dead text; values are `atoi(buf+n)` (malformed ⇒ 0); on duplicate keys the **last occurrence wins** (each match overwrites and the loop continues). The main dialog is created inside `ReadConfig` (`:828-832`), which itself runs inside the `FANCONTROL` ctor — config is parsed after the port driver is already open.
- `SaveConfig` (`misc.cpp:39-171`): rewrites only a fixed table of integer keys (`:51-72`), passes every unmatched line through untouched (`:146-148`), atomic replace (`:162-170`).
- **°F rule** (runbook §4.8, `memory/config-ini-constraints.md`): `Fahrenheit` is derived (`misc.cpp:848`); temp-valued keys are stored Celsius-internally. `Driver=` is a plain string key with no temperature semantics: parsed by the startup snapshot only, never added to `SaveConfig`'s numeric table; the passthrough at `misc.cpp:146-148` preserves the user's line verbatim. (Same pattern as `UseTWR`, which `SaveConfig` also never writes.)

### 2.8 Exit-path gating today (the #5 / gap-B / gap-C facts)

This is the inventory that invalidated v1's "same failure contract as TVic" claim. TVic's port ops are `void`/can't-fail; a *dead transport* is a **new state PawnIO introduces**, and today's exits assume writes eventually succeed:

1. **MaxReadErrors path** (`fancontrol.cpp:3852-3859`): after `ReadErrorCount >= MaxReadErrors`, exit happens **only if** `ok = SetFan("Max. Errors", FAN_CTRL_BIOS)` succeeds — that `SetFan` rides the same dead handle, so the process loops on a dead transport forever (gap B: v1's own G2 test "kill PawnIO service → BIOS fallback → exit" was unreachable as designed).
2. **Interactive/posted close, command 5020** (`fancontrol.cpp:3553-3587`): refuses to close unless `SetFan("On close", FAN_CTRL_BIOS, true)` succeeds (`:3566`), else latches `m_needClose = true` (`:3583`); an `EcAccess.Lock(100)` miss also just latches `m_needClose` (`:3557-3563`).
3. **`m_needClose` consumption** (`fancontrol.cpp:3817-3824`): the latch is consumed **only inside the successful-read branch** of `WM__NEWDATA`. With a dead transport there is never a successful read ⇒ never consumed (gap C).
4. **Service stop** (`approot.cpp:282-311`): posts 5020 (`:289-290`) and waits 15 s (`:295`); per 1–3 the close never completes ⇒ `StopWorkerThread` times out, the SCM handler reports exit code 2 (`:238-243`) and the process **survives as a zombie**. `WM_ENDSESSION` is no rescue for services — the whole handler is skipped when `Runs_as_service` (`fancontrol.cpp:3642`).
5. Only `WM_ENDSESSION` for interactive sessions is already un-gated: its BIOS handoff is best-effort (`fancontrol.cpp:3663-3664`) and it dismisses regardless.

§12 fixes exactly these four gates and nothing else.

---

## 3. Architecture overview

A minimal transport-interface seam below `portio.cpp` (the EC protocol layer). Everything above the seam — handshake, retries, two-sample matching, mode logic, guards — is untouched except the four exit-gate lines (§12) and the group-lock brackets (§11).

```
                       FANCONTROL  (fanstuff.cpp)
     ReadEcStatus / ReadEcRaw / SetFan / SetHdw          TWR block path
        │  (EcAccess held across transactions;            (fanstuff.cpp:952-991)
        │   Access_EC held across logical groups, §11)          │
        ▼                                                       │  ReadPort/WritePort
   ReadByteFromEC / WriteByteToEC  (portio.cpp)                 │  via the SAME delay-
   ACPI handshake, centralized pair select (§9)                 │  loaded TVic shim
        │                                                       │  (§5; TVic-only ports,
        │  g_PortIo->ReadPort8/WritePort8/PortAllowed           │  selection forces TVic
        ▼                                                       ▼  when UseTWR!=0, §8)
  ┌───────────────────────────────────────────────────────────────────┐
  │            IPortIo            (new: portio_backend.h/.cpp)        │
  │  Open  Close  IsOpen  ReadPort8  WritePort8  PortAllowed          │
  │  TransportLost  TryRecover  BeginEcGroup/EndEcGroup  Name         │
  └───────────────┬──────────────────────────────┬────────────────────┘
                  │ (fallback / Driver=tvicport)  │ (PRIMARY / default)
     ┌────────────▼─────────────┐   ┌────────────▼──────────────────────┐
     │ PortIoTVic               │   │ PortIoPawn                        │
     │ delay-loaded shim:       │   │ StartService("PawnIO") best-effort│
     │ LoadLibraryW(<exedir>\   │   │ CreateFileW(\\?\GLOBALROOT\Device │
     │  TVicPort.dll) +         │   │   \PawnIO), share R|W|DELETE      │
     │ GetProcAddress x6;       │   │ IOCTL_PIO_VERSION  (0xA1B22184)   │
     │ no .lib import ->        │   │ IOCTL_PIO_LOAD_BINARY (0xA1B22084)│
     │ /SAFESEH re-enabled      │   │ IOCTL_PIO_EXECUTE_FN (0xA1B22104) │
     │ PortAllowed = true       │   │ PortAllowed = {0x62, 0x66} only   │
     └────────────┬─────────────┘   └────────────┬──────────────────────┘
                  │                              │
        TVicHW64.sys / TVicPort64.sys      PawnIO.sys  +  LpcACPIEC.bin
        (unsigned, LOLDrivers-flagged;     (MS-signed driver, signed LGPL
         optional legacy fallback ONLY)     module, EC ports 0x62/0x66)

  Backend selection (approot.cpp WorkerThread, per §8):
     Driver=tvicport ──────────────────────────────► PortIoTVic
     Driver=pawnio  ───────────────► PortIoPawn (strict; no silent fallback)
     Driver=auto (default):
        UseTWR!=0 ─────────────────────────────────► PortIoTVic (forced)
        else: grace window (PawnIO only): StartService + open + probe
                  ok ──► PortIoPawn
              after grace, per pass: PawnIO first, then TVic; 180 s overall
```

New files: `fancontrol.x/portio_backend.h` + `.cpp` (interface, both backends, the TVic shim, startup-config snapshot, selection, probe, inked ABI constants). One global, published **only after** a candidate is fully accepted (§8.3) and constant for the process lifetime:

```cpp
extern IPortIo* g_PortIo;	// published once by PortIoSelect(); never rebound (no runtime migration)
```

---

## 4. The transport interface — `IPortIo`

Codebase style (tabs, `this->`, PascalCase, Win32 types).

```cpp
// portio_backend.h
//
// Transport seam under the EC protocol layer. Covers exactly the six TVicPort
// calls the app has ever used (§2.1), expressed so a backend that CAN fail
// per-operation (PawnIO) reports it, while the TVic shim preserves its
// can't-fail semantics by always returning TRUE.
class IPortIo {
public:
	virtual ~IPortIo() {}

	// Open the driver and make the transport ready. PawnIO: best-effort SCM
	// start of the demand-start "PawnIO" service, CreateFileW, VERSION query
	// (logged), module load (§6.1). TVic: resolve the DLL + OpenTVicPort +
	// hard-access dance (§5). Idempotent. One attempt only - the startup
	// retry/grace policy lives in WorkerThread (§8.3), which owns the
	// service stop-event interplay.
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
	// Drives the EC port-pair steering (§9) and the UseTWR selection fence.
	virtual bool	PortAllowed(USHORT port) const = 0;

	// Transport health [#5]. Sticky TRUE once an ioctl on an ALLOWED port
	// failed at the driver level (handle dead, service stopped, device gone).
	// Deliberately NOT set by PortAllowed rejections (deterministic config,
	// §9 relies on those being cheap and harmless) nor by EC timeouts (the
	// EC layer owns those). TVic: always FALSE.
	virtual bool	TransportLost() const = 0;

	// ONE bounded recovery attempt per loss episode, called only at a
	// transaction boundary with no EC group held (§12.2). PawnIO: close /
	// reopen / re-VERSION / reload module / short reprobe; deliberately NO
	// StartService here (a deliberate admin stop stays stopped). Returns
	// TRUE and clears TransportLost on success. TVic: returns IsOpen().
	virtual bool	TryRecover() = 0;

	// Cross-process EC bracket (Global\Access_EC, §11), now GROUP-scoped:
	// taken around each selector-dependent logical group (one ReadEcRaw
	// sample, one SetFan attempt, one SetHdw body) and, recursively and
	// harmlessly, around each single ReadByteFromEC/WriteByteToEC as a
	// safety net for stray call sites (named Win32 mutexes are recursive
	// per-thread). FALSE from Begin = fail the group into the existing
	// retry ladder. TVic: no-ops (behavior today, preserved).
	virtual bool	BeginEcGroup() = 0;
	virtual void	EndEcGroup() = 0;

	virtual const char*	Name() const = 0;	// "PawnIO" / "TVicPort" (for logs)
};

extern IPortIo* g_PortIo;
```

### 4.1 Why `ReadPort8`/`WritePort8` return `bool`

TVic's `ReadPort`/`WritePort` are `UCHAR(USHORT)` / `void` — they cannot signal failure. PawnIO's can. Rather than invent a sentinel byte (0xFF is a *valid* EC value — `ReadEcRaw` pre-seeds `FanCtrl = 0xFF` as its own failure sentinel, `fanstuff.cpp:849`), the interface returns `bool` with an out-parameter:

- **TVic shim**: `*pdata = pReadPort(port); return true;` — bit-identical behavior, no new failure paths.
- **PawnIO**: FALSE on ioctl failure or disallowed port. A FALSE inside `WaitForFlags`' poll loop returns FALSE **immediately** (no 1000 ms spin on a dead driver or denied port), which upstream already reads as "timed out #N": `ReadByteFromEC` FALSE → `ReadEcRaw` aborts (`fanstuff.cpp:845-855`) → `ReadEcStatus` retries 10× (`:823-830`) → `ReadErrorCount`/`MaxReadErrors` (runbook §4.5). `SetFan`'s capped verify/retry (`:618-667`) absorbs write failures.
- The five `void WritePort` handshake sites (`portio.cpp:100,109,161,170,179`) start checking the result: on FALSE, `Trace` + `return false` — the contract those functions already have.

**Revision vs v1:** v1 claimed "no new error-handling machinery above the seam." That was only true while the transport stays alive. v2 introduces exactly **one** new state above the seam — `TransportLost()` — consumed at exactly four sites (§12). Everything else still aliases to the existing timeout vocabulary.

---

## 5. TVic backend — `PortIoTVic`, now a delay-loaded shim [#7, gap E]

Today `TVicPort.lib` is linked import-time (`fancontrol.vcxproj:203`), so a missing or AV-quarantined `TVicPort.dll` kills the exe before `WinMain` — unacceptable when TVic is a mere fallback. v2 removes the `.lib` and resolves the six functions at first use:

```cpp
// portio_backend.cpp - resolved once, process-lifetime
struct TVICSHIM {
	HMODULE	hDll;
	BOOL	(WINAPI* pOpenTVicPort)();
	void	(WINAPI* pCloseTVicPort)();
	BOOL	(WINAPI* pTestHardAccess)();
	void	(WINAPI* pSetHardAccess)(BOOL);
	UCHAR	(WINAPI* pReadPort)(USHORT);
	void	(WINAPI* pWritePort)(USHORT, UCHAR);
};

bool
PortIoTVic::ResolveShim() {
	if (this->m_shim.hDll) return true;
	// MUST be an absolute exe-dir path: SetDefaultDllDirectories(
	// LOAD_LIBRARY_SEARCH_SYSTEM32) at approot.cpp:15 removed the app dir
	// from name-only LoadLibrary searches, and TVicPort.dll ships NEXT TO
	// THE EXE, not in System32. Build the path from GetModuleFileNameW.
	wchar_t path[MAX_PATH];
	if (!this->BuildExeDirPath(path, MAX_PATH, L"TVicPort.dll")) return false;
	this->m_shim.hDll = ::LoadLibraryW(path);
	if (!this->m_shim.hDll) {
		debug("PortIoTVic: TVicPort.dll not present/loadable\r\n");	// clean miss, no SEH
		return false;
	}
	// resolve all six; any miss -> FreeLibrary + fail (a stub/wrong DLL)
	...
}

bool
PortIoTVic::Open() {
	if (this->m_open) return true;
	if (!this->ResolveShim()) return false;
	if (!this->m_shim.pOpenTVicPort()) return false;
	// hard-access dance folded in verbatim from approot.cpp:401-403
	// (results were only ever stored in dead locals there)
	this->m_shim.pTestHardAccess();
	this->m_shim.pSetHardAccess(TRUE);
	this->m_shim.pTestHardAccess();
	this->m_open = true;
	return true;
}
```

Decisions:

- **Full `LoadLibrary`/`GetProcAddress`, not `/DELAYLOAD`.** A `/DELAYLOAD` still links `TVicPort.lib` — which is the sole `/SAFESEH` blocker (LNK2026, `fancontrol.vcxproj:101-103`) — and surfaces a missing DLL as SEH exception `0xC06D007E` at first call. Full dynamic resolution drops the `.lib` entirely, turns a missing DLL into a clean `NULL` return, and lets us **re-enable `/SAFESEH`** — a hardening win aligned with the whole point of this change (gap E). Update the vcxproj comment at `:101-103` accordingly; delete the `<Library Include="TVicPort.lib"/>` item at `:203`; update runbook §8's "SAFESEH must stay off" gotcha.
- **The TWR block path (`fanstuff.cpp:952-991`) routes through the same shim.** Its raw `ReadPort`/`WritePort` calls (`:961,965,969,970,975,978,981,990`) become calls to same-signature shim wrappers (`UCHAR TVicShim_ReadPort(USHORT)` / `void TVicShim_WritePort(USHORT, UCHAR)`, exposed by `portio_backend.h`), keeping the block protocol textually intact — a mechanical rename, diff-verifiable. Selection guarantees TVic is open whenever this code runs (§8.1); as belt-and-braces the wrappers no-op/return 0xFF with a one-shot log if called unresolved. This replaces v1's "fanstuff.cpp: no changes" claim.
- The `TestHardAccess`/`SetHardAccess` dance moves into `Open()` and out of `WorkerThread` (results were already ignored, `approot.cpp:380-382,401-403`).
- The 180 s retry loop **stays in `approot.cpp`** — it is a property of startup, interlocked with `g_stopEvent` (`approot.cpp:393-398`) and SCM stop (`:285-286`), and now retries the selection pass (§8.3).
- **Documented tradeoff (maintainer decision):** with Game Mode removed (§10.2), TVic-fallback users get no anti-cheat cover — the flagged `.sys` files sit visible in `System32\drivers` whenever the fallback is installed. This is accepted: PawnIO is the path; the fallback exists for `UseTWR` users and PawnIO-less machines. **Alternative noted for a later maintainer decision:** drop TVic entirely (PawnIO-only), deleting the shim, the fallback rows of §8.1, and risk R4 — this design keeps that door open by isolating every TVic reference inside `portio_backend.cpp` + the TWR wrappers.
- `PortAllowed()` = TRUE, `TransportLost()` = FALSE, `TryRecover()` = `IsOpen()`, `Begin/EndEcGroup()` = no-ops (extending `Access_EC` honoring to TVic mode would be a new behavior with new stall modes; out of scope for a no-regression release).

---

## 6. PawnIO backend — `PortIoPawn`

Raw Win32 only (constraint #1). All ABI facts below are **runtime-verified on the target P15G2** (installed driver v2.1.0; source HEAD v2.2.0; ABI stable across both) — the v1 `TBD-impl` markers are resolved:

```cpp
// portio_backend.cpp - inked ABI (verified: device opened + VERSION ioctl
// returned 0x00020100 on the target machine). NOTE the \\.\PawnIO DOS
// symlink is NOT created by the installed driver (CreateFileW err=2);
// only the GLOBALROOT form works.
static const wchar_t k_pawnio_device[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr DWORD IOCTL_PIO_LOAD_BINARY = 0xA1B22084;	// devtype 0xA1B2, fn 0x821, METHOD_BUFFERED
constexpr DWORD IOCTL_PIO_EXECUTE_FN  = 0xA1B22104;	// fn 0x841
constexpr DWORD IOCTL_PIO_VERSION     = 0xA1B22184;	// fn 0x861; out: ULONG (major<<16|minor<<8|patch)
static const wchar_t k_ec_mutex_name[] = L"Global\\Access_EC";	// ecosystem convention (LpcACPIEC)
```

### 6.1 Open [gap A: start the demand-start service]

```cpp
bool
PortIoPawn::Open() {
	if (this->m_hDev != INVALID_HANDLE_VALUE) return true;

	// 0) The PawnIO service is DEMAND-START: on a healthy install nothing
	//    else starts it, so a CreateFileW-only open would fail every boot and
	//    auto would silently run TVic forever (gap A). Best-effort SCM start;
	//    the app already runs elevated (device DACL is SYSTEM/Admins-only),
	//    so SERVICE_START is available. ERROR_SERVICE_ALREADY_RUNNING is fine.
	SC_HANDLE scm = ::OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm) {
		SC_HANDLE svc = ::OpenServiceW(scm, L"PawnIO", SERVICE_START);
		if (svc) { ::StartServiceW(svc, 0, NULL); ::CloseServiceHandle(svc); }
		::CloseServiceHandle(scm);
	}

	// 1) device open == driver presence test. Share flags match upstream
	//    PawnIOLib (R|W|DELETE). ERROR_FILE_NOT_FOUND = absent (fall back /
	//    retry per §8.3); ERROR_ACCESS_DENIED = present but not elevated.
	this->m_hDev = ::CreateFileW(k_pawnio_device,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);	// synchronous
	if (this->m_hDev == INVALID_HANDLE_VALUE) {
		debug("PortIoPawn: PawnIO device not present\r\n");
		return false;
	}

	// 2) query + log the driver version [#1]. Major 2 (2.1/2.2) is verified
	//    compatible; anything else logs a one-shot warning and proceeds -
	//    LOAD/EXEC failure is the real gate and fails cleanly into fallback.
	ULONG ver = 0; DWORD cb = 0;
	if (::DeviceIoControl(this->m_hDev, IOCTL_PIO_VERSION, NULL, 0, &ver, sizeof(ver), &cb, NULL))
		debug(...);	// "PawnIO driver v%u.%u.%u"
	if ((ver >> 16) != 2)
		debug("PortIoPawn: WARNING untested driver major version\r\n");

	// 3) load the signed module blob shipped next to the exe (absolute path
	//    from GetModuleFileNameW; CWD is the exe dir per approot.cpp:365-378
	//    but belt-and-braces for odd service CWDs). One load per handle; the
	//    driver verifies the embedded signature - NEVER patch the blob.
	if (!this->LoadModuleBlob(L"LpcACPIEC.bin")) {	// IOCTL_PIO_LOAD_BINARY
		this->Close();
		return false;
	}

	// 4) ecosystem EC mutex (§11). Degrade-with-one-shot-log if creation
	//    fails: a thermal-safety app must not refuse to run over a lock
	//    nicety [#4, adjudicated policy].
	this->m_hEcMutex = ::CreateMutexW(NULL, FALSE, k_ec_mutex_name);
	if (!this->m_hEcMutex)
		debug("PortIoPawn: Global\\Access_EC unavailable - proceeding unguarded\r\n");

	this->m_transportLost = false;
	return true;
}
```

Blob format `[4-byte LE sig_len][signature][AMX module]`; the official driver enforces the signature at `IOCTL_PIO_LOAD_BINARY`. A second LOAD on the same handle returns `STATUS_ALREADY_INITIALIZED` — irrelevant here because recovery reopens a fresh handle (§12.2).

### 6.2 Execute — the ABI (inked)

- `IOCTL_PIO_EXECUTE_FN` input = bytes `[0..31]`: null-terminated ASCII function name (fixed 32-byte field; must start `"ioctl_"`, ≤31 chars) + bytes `[32..]`: `ULONG64` input array. `InputBufferLength = 32 + inCount*8`. Output = `ULONG64` array at offset 0.
- **Gotcha (verified):** on success the driver sets `Information = OutputBufferLength`, so `lpBytesReturned` always equals the *requested* output size — it is **not** a produced-cell count. Trust `out[]` only when `DeviceIoControl` returns TRUE; do not length-check `cbRet` (v1's `cbRet >= outCount*8` check was vacuous and is dropped).

```cpp
#pragma pack(push, 1)
struct PIO_EXEC_IN {
	char	Name[32];	// null-terminated ASCII fn name, zero-padded
	ULONG64	Args[2];	// input cells; only 32 + inCount*8 bytes are sent
};
#pragma pack(pop)

bool
PortIoPawn::Exec(const char* fn, const ULONG64* in, ULONG inCount,
                 ULONG64* out, ULONG outCount) {
	if (this->m_hDev == INVALID_HANDLE_VALUE) return false;
	PIO_EXEC_IN buf; setzero(&buf, sizeof(buf));
	strncpy_s(buf.Name, sizeof(buf.Name), fn, _TRUNCATE);
	for (ULONG i = 0; i < inCount; i++) buf.Args[i] = in[i];
	DWORD cbRet = 0;
	if (!::DeviceIoControl(this->m_hDev, IOCTL_PIO_EXECUTE_FN,
			&buf, 32 + inCount * sizeof(ULONG64),
			out, outCount * sizeof(ULONG64), &cbRet, NULL)) {
		this->NoteTransportFault();	// §6.4: classify + maybe latch m_transportLost
		return false;
	}
	return true;
}
```

The 8-byte `ULONG64` cells are architecture-neutral; the x86 process and x64 driver agree on layout byte-for-byte. Use Win32 `DeviceIoControl` (not `NtDeviceIoControlFile` — avoids the 32-bit `IO_STATUS_BLOCK` WOW64 pitfall).

### 6.3 Port I/O mapped onto the LpcACPIEC module

| `IPortIo` op | Module fn | In (`ULONG64[]`) | Out (`ULONG64[]`) |
|---|---|---|---|
| `ReadPort8(port, &v)` | `"ioctl_pio_read"` | `[port]` | `[value]` → `*pdata = (UCHAR)out[0]` |
| `WritePort8(port, v)` | `"ioctl_pio_write"` | `[port, value]` | none |

```cpp
bool
PortIoPawn::PortAllowed(USHORT port) const {
	// Mirror the stock LpcACPIEC allowlist exactly: ONLY the ACPI-spec EC
	// pair; anything else gets STATUS_ACCESS_DENIED from the module. TYPE1
	// (0x1600/0x1604) and the TWR window (0x1610-0x161F) are rejected HERE,
	// deterministically and without an ioctl, so the §9 steering and the
	// UseTWR->TVic rule can never be silently bypassed. A local rejection
	// does NOT latch m_transportLost (§6.4).
	return port == 0x62 || port == 0x66;
}
```

`ReadPort8`/`WritePort8` fail fast on `!PortAllowed(port)` before burning an ioctl, then call `Exec` as in v1.

### 6.4 Transport-lost detection [#5]

`NoteTransportFault()` classifies the `GetLastError()` after a failed ioctl **on an allowed port**: handle/device-level errors (`ERROR_INVALID_HANDLE`, `ERROR_FILE_NOT_FOUND`, `ERROR_DEVICE_NOT_CONNECTED`, `ERROR_OPERATION_ABORTED`, `ERROR_BAD_COMMAND`, …) latch the sticky `m_transportLost` and emit a one-shot log naming the error; a module-level `STATUS_ACCESS_DENIED` surfaced for an allowed port (unexpected — allowlist drift) logs one-shot but also latches, since the pair the app depends on is undrivable either way. `PortAllowed` pre-check rejections never reach here. EC protocol timeouts never reach here either — they are upstairs in `portio.cpp` and involve successful transports.

### 6.5 Performance note

Each handshake byte costs ~5–8 port ops; under PawnIO each op is one `DeviceIoControl` (µs-scale syscall + sandboxed interpreter dispatch). The handshake's own pacing — 10 ms `WaitForFlags` poll steps (`portio.cpp:49,60`) and the 100/150/200 ms upstream backoffs — dominates by 3–4 orders of magnitude. Validated (latency and error-rate both) in G1, §15.

---

## 7. Where the seam lands in existing code

| Site | Today | After |
|---|---|---|
| `portio.cpp:53` (`WaitForFlags`) | `data = ReadPort(port);` | `UCHAR data; if (!g_PortIo->ReadPort8(port, &data)) return false;` — immediate fail, no timeout spin |
| `portio.cpp:100,109,161,170,179` | `void WritePort(...)` | `if (!g_PortIo->WritePort8(...)) { this->Trace("..."); return false; }` |
| `portio.cpp:127-128` (H-01 OBF drain) | `ReadPort` ×2 | `ReadPort8`; on transport failure skip the drain (the read already failed) |
| `portio.cpp:71-75` + `:148-152` (duplicated lazy init) | two copies | one `EnsureEcPorts()` helper, backend-aware (§9) |
| `ReadByteFromEC`/`WriteByteToEC` entry/exit | — | recursive `BeginEcGroup()`/`EndEcGroup()` safety-net bracket (§11) |
| `fanstuff.cpp:842-897` (`ReadEcRaw`), `:618-667` (`SetFan` attempt), `:719-736` (`SetHdw`) | — | group-level `BeginEcGroup()`/`EndEcGroup()` brackets (§11) |
| `fanstuff.cpp:952-991` (TWR block) | direct `ReadPort`/`WritePort` imports | same protocol via the delay-loaded shim wrappers (§5) — mechanical rename |
| `fancontrol.cpp:3566, 3817-3824, 3852-3859` (exit gates) | gated on live-transport writes | un-gated on `TransportLost()` (§12) |
| `approot.cpp:384-386` | `RecoverHiddenDrivers()` | **deleted** (§10.2) |
| `approot.cpp:388-399` | retry `OpenTVicPort()` | deadline/grace selection loop (§8.3) |
| `approot.cpp:401-403` | hard-access dance | deleted (moved into `PortIoTVic::Open`) |
| `approot.cpp:416` | `CloseTVicPort()` | `g_PortIo->Close()` |
| `approot.cpp:421-426` | error text names tvicport.sys | text names PawnIO (primary), the module file, TVicPort (fallback), and the `Driver=` key |

---

## 8. Backend selection

### 8.1 Decision table

`Driver=` values are case-insensitive; anything unrecognized → `auto` + a logged warning. `UseTWR` truth test is `atoi() != 0` everywhere [#6].

| `Driver=` | `UseTWR` | PawnIO start+open+probe (§8.4) | Selected backend | Notes |
|---|---|---|---|---|
| `tvicport` | any | not attempted | **TVicPort** | explicit override; shim resolve failure → open-failure UX |
| `pawnio` | ==0 | ok | **PawnIO** | |
| `pawnio` | ==0 | fails | **none — open failure** | strict: an explicit override is a pinning/debug tool; substituting TVic would mask the misconfiguration. Fails **bounded** at the 180 s deadline (not "indefinitely"), then the existing failure UX: error box / service exit code 3 (`approot.cpp:418-439`) |
| `pawnio` | !=0 | — | **TVicPort** + logged warning | conflict: the TWR block protocol is impossible on the stock module (§2.3). Losing the user's chosen temp path would silently change which temperatures the fan logic sees, so `UseTWR` wins; the log says why |
| `auto` (default / missing key) | !=0 | skipped entirely | **TVicPort** | pointless to probe — TWR needs 0x1604/0x1610-0x161F |
| `auto` | ==0 | ok (any pass) | **PawnIO** | the target state; grace window applies, see §8.3 |
| `auto` | ==0 | PawnIO failing, grace expired, TVic opens | **TVicPort** | logged with the specific fallback reason |
| `auto` | ==0 | both fail through 180 s | **none — open failure** | existing failure UX |

### 8.2 Canonical config snapshot [#6 — replaces v1's dual parse]

Selection needs `Driver=` and `UseTWR=` **before** `FANCONTROL` exists, but `ReadConfig` runs inside the ctor, after the driver opens, and even creates the main dialog (`misc.cpp:828-832`). v1's answer — a pre-parse *plus* a second parse in `ReadConfig` — created a window where the two could disagree (duplicate keys, `UseTWR=2` vs an `==1` test, a file edit during the up-to-180 s retry window). One demonstrated failure: `UseTWR=2` under `auto` would have selected PawnIO while the TWR path called the unopened TVic imports — every poll fails, forced exit, reason invisible.

**v2 design — one snapshot, one parser, one owner:**

```cpp
struct PORTIO_STARTUP {
	int	DriverChoice;	// 0=auto (default), 1=pawnio, 2=tvicport
	int	UseTWR;		// raw atoi value; truth test: != 0
	USHORT	EcCtrlPort, EcDataPort;	// pair validated by the §8.4 probe (0,0 = not probed)
	bool	EcPairProbed;		// true only for an accepted restricted backend
	char	Reason[160];		// human-readable selection reason, Trace'd by the ctor
};
```

- `PortIoParseStartupConfig("TPFanControl.ini", &cfg)` runs **once** in `WorkerThread` before the selection loop. It implements exactly the `ReadConfig` line semantics inventoried in §2.7: `fgets` ≤1 KB lines; comment ⇔ first char ∈ `{/ # ;}` (`misc.cpp:361-362`); keys anchored at column 0, `_strnicmp` case-insensitive prefix (indented keys are dead text); `UseTWR=` value = `atoi(buf+7)`, malformed ⇒ 0; `Driver=` value = first token, case-insensitive match against `pawnio|tvicport|auto`, anything else ⇒ `auto` + warning recorded in `Reason`; **duplicates: last occurrence wins** (mirrors `ReadConfig`'s overwrite-and-continue loop). Missing file/keys ⇒ defaults (`auto`, 0).
- The snapshot (plus the probe result, filled in by selection) is passed **by pointer into the `FANCONTROL` ctor** (new parameter). The ctor seeds `this->UseTWR` and the EC pair from it (§9) *after* `InitializeCriticalSection(&this->m_logLock)` (`fancontrol.cpp:131`) and *before* `ReadConfig` (`:289`).
- **`ReadConfig`'s `UseTWR=` handler (`misc.cpp:364-367`) is deleted** — the snapshot is authoritative and the later parse must not override it. `ReadConfig` never learns a `Driver=` handler either; unmatched lines already fall through harmlessly. `SaveConfig` writes neither key — both round-trip via the passthrough (`misc.cpp:146-148`); no °F hazard (§2.7).
- Because the file is read once, a mid-retry-window edit can no longer desync selection from `FANCONTROL`; the edit simply takes effect next launch (documented in the ini template comment).

### 8.3 Startup selection loop [gap D grace + finding #11 ownership; replaces v1 §8.3]

```cpp
// approot.cpp WorkerThread - replaces the OpenTVicPort loop at :388-399.
// GetTickCount64 is the codebase's clock for wall deadlines (fancontrol.cpp:447,749).
PORTIO_STARTUP cfg;
PortIoParseStartupConfig("TPFanControl.ini", &cfg);	// once; §8.2

const ULONGLONG t0 = ::GetTickCount64();
const ULONGLONG tDeadline = t0 + 180ULL * 1000;	// same overall budget as today
const ULONGLONG tGraceEnd = t0 + PAWNIO_GRACE_MS;	// 30 s PawnIO-exclusive phase (auto only)

IPortIo* accepted = NULL;
for (;;) {
	// One selection pass. In auto, TVic is only eligible after the grace
	// window: a PawnIO service that is merely slow to start must not lose
	// to the flagged driver on iteration 1 - which would load TVic for the
	// WHOLE session, every boot (gap D).
	bool tvicEligible = (cfg.DriverChoice == DRV_TVICPORT) || (cfg.UseTWR != 0) ||
		(cfg.DriverChoice == DRV_AUTO && ::GetTickCount64() >= tGraceEnd);
	accepted = PortIoSelectOnePass(&cfg, tvicEligible);	// §8.1 table; RAII inside
	if (accepted) break;
	if (::GetTickCount64() >= tDeadline) break;
	// cancellation: a service stop during startup must abort promptly
	if (g_stopEvent && ::WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
	if (!g_stopEvent) ::Sleep(1000);
}
if (accepted) g_PortIo = accepted;	// publish ONLY a fully-accepted backend [#11]
```

Ownership rules inside `PortIoSelectOnePass`:

- Backends are function-local candidates guarded by a small RAII holder (`struct PORTIOCANDIDATE { IPortIo* p; ~PORTIOCANDIDATE() { if (p) p->Close(); } }`): every early return (stop event, open failure, VERSION anomaly, probe failure) closes the candidate's handle; only explicit acceptance detaches it for return. **No partially-open backend is ever reachable through `g_PortIo`**, and `g_PortIo` is written exactly once, after acceptance — fixing v1's assign-then-test `if (g_PortIo = PortIoSelect(cfg))` shape.
- Acceptance for PawnIO = `Open()` (incl. SCM start + VERSION log) **and** the §8.4 probe passed; the probed pair is recorded into `cfg.EcCtrlPort/EcDataPort/EcPairProbed`. Acceptance for TVic = `Open()` succeeded (unrestricted transport; no probe — behavior today).
- Cost accounting: a PawnIO miss per pass is microseconds (`CreateFileW` fail) plus, at most once per pass, the SCM round-trip; the grace window therefore delays a TVic-fallback boot by ≤30 s **once**, during which firmware owns the fans (safe). Machines that will never run PawnIO can set `Driver=tvicport` to skip the grace entirely — noted in the ini template comment.
- The `g_stopEvent` abort semantics and the `SERVICE_STOPPED` exit-status reporting (`approot.cpp:429-439`) are byte-identical to today.

### 8.4 The EC probe (`auto`/`pawnio` acceptance test) [#2 hardened + gap G]

Opening the device + loading the module proves the *transport* — not that **this machine's EC answers on 0x66/0x62** (risk R1). The probe is self-contained in `portio_backend.cpp` (no `FANCONTROL` exists yet):

```
PawnIoProbeEc(transport):
	round := one full ACPI READ handshake of EC reg 0x2F (TP_ECOFFSET_FAN,
	         fanstuff.cpp:45) on 0x66/0x62, inside a Global\Access_EC RAII
	         bracket (§11): quiescence wait (IBF|OBF clear, <=250 ms, 10 ms
	         steps) -> 0x80 READ cmd -> IBF clear <=250 ms -> offset 0x2F ->
	         OBF set <=250 ms -> read data byte.
	valid(round) := handshake completed AND value != 0xFF
	         (0xFF = the code's own impossible-value sentinel, fanstuff.cpp:849;
	          a dead/floating pair typically reads 0xFF at every stage)
	PASS := 3 consecutive valid rounds, within at most 6 rounds total;
	         the mutex is released and re-acquired between rounds.
	FAIL := otherwise. Worst case ~5 s; typical (healthy EC) < 100 ms.
```

Why N-stable instead of v1's single byte: the review's G0b measurement put the *ambient* single-pass failure rate (acpi.sys contention on the shared 0x62/0x66 pair, no retries) at **~19%** — a one-shot probe would randomly reject a perfectly good machine roughly one boot in five, silently loading the flagged fallback. With p≈0.81 per round, P(3-consecutive within 6) ≈ 0.95 per probe call, and the selection loop re-probes every pass during grace, so startup false-negatives decay geometrically. A genuinely dead pair yields zero valid rounds — deterministic FAIL. Success criterion stays deliberately semantic-light (a completed handshake with a plausible byte, not a specific value): reg 0x2F is read-only-safe and mode-agnostic.

On acceptance the probed pair is recorded in `PORTIO_STARTUP` and becomes authoritative for the EC layer (§9).

**Logging:** the chosen backend and reason are recorded twice — `debug()` from `WorkerThread` (`approot.cpp:442-450`) and `FANCONTROL::Trace` from the ctor via `PORTIO_STARTUP.Reason` (next to the existing "Current Config" block, `misc.cpp:738-739`). Examples: `"PortIo: PawnIO v2.1.0 (LpcACPIEC.bin, EC on 0x66/0x62, probe 3/3)"`, `"PortIo: TVicPort (fallback: PawnIO device absent after 30 s grace)"`, `"PortIo: TVicPort (forced: UseTWR=1)"`.

---

## 9. EC-type constraint under PawnIO [#8, gap F] (read this section twice)

`portio.cpp` prefers TYPE1 (`0x1604/0x1600`): both handshake functions lazily initialize to TYPE1 (`portio.cpp:71-75, 148-152`) and only fall to TYPE2 (`0x66/0x62`) after a probe timeout while `m_ecTypeKnown` is false (`:84-95`). The stock module allowlists **only 0x62/0x66** — under PawnIO the handshake *must* run on TYPE2, and TYPE1 must fail cleanly and instantly.

Mechanisms, revised from v1:

1. **Authoritative pre-seed (primary).** The §8.4 probe already did on 0x66/0x62 exactly what a first successful read does — so for a restricted backend the pair is **known**, not guessed. In the `FANCONTROL` ctor, **after** `::InitializeCriticalSection(&this->m_logLock)` (`fancontrol.cpp:131` — `Trace` records into the lock-protected tail, so this must precede any trace) and **before** `ReadConfig` (`:289`):

	```cpp
	// §8.4 probe validated the ACPI-spec pair on this boot; latch it. This
	// runs after m_logLock init (fancontrol.cpp:131) - Trace is now safe -
	// and before ReadConfig (:289). [v1 cited ":92-93", inside the member-
	// initializer list - wrong slot; gap F]
	if (pStartup && pStartup->EcPairProbed) {
		this->EC_CTRL = pStartup->EcCtrlPort;	// 0x66
		this->EC_DATA = pStartup->EcDataPort;	// 0x62
		this->m_ecTypeKnown = true;	// probe == first successful read (portio.cpp:134)
		this->Trace("EC ports fixed to probed pair (restricted backend)");
	}
	```

	Setting `m_ecTypeKnown = true` makes the pair **authoritative**: the H-02 flip logic (`portio.cpp:84-95`) is unreachable under PawnIO, so a transient stall can never steer the handshake onto the disallowed TYPE1 pair. (Under TVic nothing is pre-seeded: lazy TYPE1 init, H-02 probing, and latching behave exactly as today.)

2. **Centralized pair selection (read AND write).** The duplicated lazy init (`portio.cpp:71-75` and `:148-152`) collapses into one `EnsureEcPorts()` helper called by both `ReadByteFromEC` and `WriteByteToEC`. For an unknown pair it consults `g_PortIo->PortAllowed(ACPI_EC_TYPE1_CTRLPORT)` for its default — TYPE1 when allowed (TVic, today's behavior), TYPE2 otherwise (PawnIO without a probe result — a state only reachable if a future caller skips the probe; belt and braces). Requires the four port constants to move from `portio.cpp:24-28` into a header both files see — mechanical.
3. **`PortAllowed` fail-through (safety net).** If any path still drives TYPE1 first, `ReadPort8(0x1604, …)` returns FALSE → `WaitForFlags` fails immediately (no 1000 ms spin) → the existing "timed out #1" branch handles it. Deterministic, cheap, and does not latch `m_transportLost` (§6.4).

Corollary (unchanged, prominent): **on hardware whose EC does not respond on 0x66/0x62, the stock PawnIO module cannot drive fan control at all** — the probe exists to detect this before acceptance. See risk R1.

---

## 10. Subsystem impact map

### 10.1 `UseTWR` (functional preservation)

- Selection rule: `UseTWR != 0` → TVic, always (§8.1). Under `auto` PawnIO isn't probed; under explicit `Driver=pawnio` the conflict resolves to TVic with a warning (regression-avoidance beats override strictness — honoring `pawnio` would silently change which temperatures the fan logic sees).
- The TWR block protocol (`fanstuff.cpp:952-991`) is preserved verbatim, now calling the shim wrappers (§5). `PortIoPawn::PortAllowed` rejecting 0x1604/0x1610-0x161F remains the second fence.
- If a future custom ThinkPad module allowlists `0x1600/0x1604 + 0x1610-0x161F` (R1 mitigation (a)), the TWR block can be ported onto the seam then — out of scope now.

### 10.2 Game Mode — removed entirely [maintainer decision; reverses review #12]

Game Mode's only function is hiding the flagged TVic `.sys` files from anti-cheat scanners. With PawnIO primary the app's default install contains nothing an anti-cheat objects to, so the feature — and its whole interaction surface — is retired:

- **Delete** every site inventoried in §2.6: `RecoverHiddenDrivers` (`approot.cpp:313-347`, call `:384-386`); `ToggleGameMode` (`fancontrol.cpp:1671-1804`; decl `fancontrol.h:278`); ctor hidden-file detection (`fancontrol.cpp:144-162`); `m_driversHidden` (`fancontrol.h:332`); destructor restore (`fancontrol.cpp:989-990`); `WM_ENDSESSION` restore (`:3646-3647`); checkbox 7013 (tooltip `:420-423`, sync `:1591`, layout row `:2001`, handler `:3467-3468`, `res\FanControl.rc:72,151`); tray-menu item 5090 (`fancontrol.cpp:895-896`, `:3534`, `res\FanControl.rc:327`); tooltip tag (`fanstuff.cpp:305-308`). Update runbook §4.10 and the SMOKE_TEST Game-Mode rows.
- **Dissolved interactions:** #5's nastiest corner (runtime fallback loading TVic *while its .sys files are hidden*) becomes impossible; startup no longer touches `System32\drivers` at all; there is no mixed-backend hidden-file state to reason about.
- **Upgrade/migration note:** a machine that crashed in Game Mode under an *old* build has `.sys.bak` files and no restorer in the new build. This self-heals: the old build registered `MOVEFILE_DELAY_UNTIL_REBOOT` renames at hide time (`fancontrol.cpp:1750-1755`), which smss.exe executes on the next boot regardless of which build is installed. Residual worst case (pending-rename key lost, e.g. OS reinstall) is a manual rename, documented in the release notes. Not worth carrying `RecoverHiddenDrivers` forever.
- **Accepted loss:** TVic-fallback users get no anti-cheat cover (§5 tradeoff). They are, by definition, running the flagged driver; hiding it was always a workaround, and the supported answer is now "use PawnIO."

### 10.3 Service mode

Covered by §8.3 (deadline/grace loop keeps the `g_stopEvent` abort and `SERVICE_STOPPED` reporting untouched) and §12 (a dead transport can no longer produce the exit-code-2 zombie of gap C — service stop now completes within `StopWorkerThread`'s 15 s wait, `approot.cpp:295`). The interactive-failure text (`:421-426`) names both backends and the `Driver=` key. `Runs_as_service` detection (`misc.cpp:840-845`) is unaffected.

### 10.4 Sleep / resume

No backend hooks. The device handle (and its per-handle module instance) survives suspend like any kernel device handle; the app already defers EC traffic ~10 s after resume (`m_ecResumeDeferUntil`, `fancontrol.h:438`, gate `fancontrol.cpp:3777`; runbook §4.6). If the driver was stopped/crashed across sleep, every op fails → transport-lost → §12: one recovery attempt (fresh handle, reload, reprobe), else clean exit. This *replaces* v1's "no re-open logic in v1" stance with the minimal, bounded version the adjudication endorsed.

### 10.5 Clean-exit paths — un-gated on transport loss [#5, gaps B, C]

The recursion-based close transaction (`fancontrol.cpp:3553-3587` holding `EcAccess` → `SetFan` re-lock; runbook §8) is preserved. What changes is only the four gates from §2.8 — see §12.1 for the exact edits. `g_PortIo->Close()` replaces `CloseTVicPort()` after the dialog loop exits (`approot.cpp:416`).

---

## 11. Concurrency: two overlapping EC mutexes

| Mutex | Name | Scope | Who honors it |
|---|---|---|---|
| App-level (existing) | `Access_Thinkpad_EC` (`winstuff.h:76`) | whole multi-register transactions: 2-sample poll, write+verify, close path (`fanstuff.cpp:816-832, 612-669, 715-758`; `fancontrol.cpp:3553-3587`) | this app's GUI + service instances |
| Ecosystem (new) | **`Global\Access_EC`** (verified: LpcACPIEC convention) | **selector-dependent logical groups** (revised from v1's per-byte): one full `ReadEcRaw` sample, one `SetFan` attempt, one `SetHdw` body, the §8.4 probe | other PawnIO-era EC tools (FanControl, LHM, …) |

**Composition rules (deadlock/starvation analysis):**

1. **Strict acquisition order: `EcAccess` (outer) → `Access_EC` (inner).** Every group bracket sits inside an already-held `EcAccess` (all group call sites are within `LockECAccess`/`FreeECAccess` scopes or the recursive close-path hold). No path takes `EcAccess` while holding `Access_EC` ⇒ no lock-order cycle within this app. The §8.4 probe runs before `FANCONTROL` exists and takes only the inner mutex.
2. **Group granularity [#3 — revised from v1's per-byte].** EC reg 0x31 is a *persistent fan selector* (`fanstuff.cpp:49`): per-byte locking allowed another `Access_EC`-honoring writer to flip it between our select and our act — making `ReadEcRaw` attribute tach values to the wrong fan and letting `SetFan`'s verify (`:638`, `:648`) false-pass whenever both fans hold the same value. v2 holds across the whole selector-dependent group: `ReadEcRaw`'s full body (status + selects + tachs + temps, `fanstuff.cpp:842-897`), each `SetFan` attempt including its intra-attempt settle sleeps (`:618-667` — the sleeps at `:627,633,640,646` are part of the selector-critical sequence), each `SetHdw` body (`:719-736`).
3. **Release across retry backoffs.** The inter-attempt `Sleep(150)` in `SetFan` (`:666`) and the inter-try `Sleep(200)` in `ReadEcStatus` (`:829`) sit **outside** brackets, bounding any one hold to roughly one group (~0.4–1 s worst case with handshake timeouts) and giving other tools air between our groups.
4. **Starvation trade, stated honestly (adjudication):** group holds are *stricter* than the module ecosystem's per-op convention — we trade some cross-tool latency for selector integrity. This is a trade, not a strict win; the release-on-backoff rule is the compensating control. A wedged third-party holder degrades us to failed groups → the existing retry ladder → (if the transport is alive) today's MaxReadErrors behavior — it can never freeze a thermal-safety app indefinitely.
5. **Bounded inner wait, RAII, degrade [#4].** `BeginEcGroup` = one `WaitForSingleObject` with an absolute ≤500 ms budget (no internal retry loops); timeout ⇒ FALSE ⇒ the group fails into the normal ladder. Acquisition/release is a stack RAII guard (`ECMUTEXGUARD`) because the bracketed bodies have many early returns (`portio.cpp` alone has six). `WAIT_ABANDONED` counts as acquired — same reasoning and precedent as `MUTEXSEM::Lock` (`winstuff.cpp:56-60`). If the mutex could not be created at `Open()` (§6.1), Begin/End degrade to no-ops after a one-shot log — a thermal app does not refuse to run over a lock nicety (Codex's fail-selection policy rejected).
6. **Safety net for stray byte calls.** `ReadByteFromEC`/`WriteByteToEC` also bracket themselves; named Win32 mutexes are recursive per-thread, so inside a group this is a cheap re-acquire, and standalone call sites (e.g. the probe-adjacent read at `fancontrol.cpp:839-844`) stay covered. Balanced via the same RAII guard.
7. **`SampleMatch` scope (updated):** originally `SampleMatch` compared only `FanCtrl` (v1 §11.4's claim that it "rejects torn multi-register snapshots" was wrong *then*). It has since been extended (SampleMatch temperature-validation change) to also require the **12 temperature sensors to agree within 5 °C** between the two back-to-back samples, so it now **does** reject a torn/foreign temperature (a valid-vs-invalid slot disagreement is rejected too; only both-invalid slots are skipped). It still does **not** compare **tach** (fan-RPM) bytes — those legitimately fluctuate, so torn tach data is defended by the group hold above plus the EC's own transaction quiescence (`portio.cpp`), not by `SampleMatch`.
8. **TVic backend:** Begin/End remain no-ops — behavior today, preserved.
9. **Session-namespace caveats (pre-existing, documented not fixed):** unqualified `"Access_Thinkpad_EC"` and `"TPFanControlMutex01"` (`approot.cpp:17`) are per-session, so a session-0 service and a session-1 GUI interlock on *neither* (gap H) — which is what makes the dual-instance scenario of R7 reachable. `Global\Access_EC` (this change) is correctly global. Do **not** "fix" the two legacy names in this change; it would alter GUI/service interlock behavior unrelated to the port.

---

## 12. Error semantics: EC timeouts vs transport loss [#5 — the blocking fix]

Principle, revised: **transport failures alias into the existing EC-timeout vocabulary for all *steady-state* handling** (retries, BIOS-fallback counting, UI) — **plus exactly one new state, `TransportLost()`, consumed at exactly four sites to un-gate exiting.** No mid-session backend migration: Codex's runtime auto→TVic switch is rejected — it silently loads the flagged, possibly AV-quarantined driver at the worst possible moment, converting a clean "PawnIO died" into "the thing the user installed PawnIO to avoid just happened."

### 12.1 The four un-gating edits (all in `fancontrol.cpp`)

| # | Site | Today | After |
|---|---|---|---|
| 1 | MaxReadErrors exit (`:3852-3859`) | `ok = SetFan("Max. Errors", FAN_CTRL_BIOS); if (ok) { … WM_ENDSESSION }` — exit requires a write through the (possibly dead) handle | `if (ok \|\| g_PortIo->TransportLost()) { … WM_ENDSESSION }` — the BIOS handoff stays *attempted* (it fails fast under a lost transport: `WaitForFlags` returns immediately, §4.1), but a dead transport no longer blocks the exit. EC-timeout-with-live-transport keeps today's conservative keep-trying behavior |
| 2 | Close command 5020 (`:3566`) | `if (!this->ActiveMode \|\| this->SetFan("On close", FAN_CTRL_BIOS, true))` else `m_needClose = true` (`:3583`) | `if (!this->ActiveMode \|\| this->SetFan(...) \|\| g_PortIo->TransportLost())` — close proceeds; the handoff was attempted |
| 3 | `m_needClose` consumption (`:3817-3824`, only in the `if (ok)` branch) | dead transport ⇒ never consumed (gap C) | in the failure branch (`:3826+`): `if (m_needClose && g_PortIo->TransportLost()) ::PostMessage(hwndDialog, WM_COMMAND, 5020, 0);` — with edit 2, that repost completes |
| 4 | `WM_ENDSESSION` (`:3663-3664`) | already best-effort / un-gated | unchanged (noted for completeness) |

Net effect: service stop (posts 5020, `approot.cpp:289-290`) completes within the 15 s wait (`:295`) instead of timing out into an exit-code-2 zombie; the G2 test "stop the PawnIO service mid-run → BIOS-fallback attempt → **clean exit**" is now reachable *by design* (gap B), and once the process exits, **firmware owns the fans** — the EC's autonomous thermal control is the final backstop, same as when the app was never running.

### 12.2 The single bounded recovery

At a transaction boundary only — the `WM__NEWDATA` failure branch (`fancontrol.cpp:3826-3861`), where the poll worker has exited and no `EcAccess`/`Access_EC` is held — and **once per loss episode**:

- Trigger: `g_PortIo->TransportLost() && !recoveryAttempted`.
- `TryRecover()` (PawnIO): `Close()` → `CreateFileW` (fresh handle) → VERSION query+log → `IOCTL_PIO_LOAD_BINARY` (fresh per-handle instance, so `STATUS_ALREADY_INITIALIZED` cannot occur) → shortened §8.4 probe (2-consecutive-of-4). Success clears `m_transportLost` and re-arms the episode latch; the next successful poll resets `ReadErrorCount` as it always has (`:3814`).
- Deliberately **no `StartService`** during recovery: an admin who stopped PawnIO on purpose (or an in-flight driver upgrade) must not be fought; the recovery targets driver-crash/handle-invalidation cases where the device reappears on its own. If recovery fails, the transport stays lost and the MaxReadErrors ladder exits cleanly (§12.1).
- Budget: single pass, ≤ ~3 s, no retry loop — it can delay one poll tick, never wedge the UI thread longer than an ordinary failed `ReadEcStatus` already does.

### 12.3 What is *not* surfaced upstream

"Port denied" vs "driver vanished" vs "module rejected" remain backend-internal, logged one-shot per kind for support logs. Upstream sees booleans plus `TransportLost()`. The one behavioral delta inherited from v1 stands: a `PortAllowed`-rejected port fails **without** the 1000 ms `WaitForFlags` spin — strictly shorter stalls, and required for the §9 fail-through to be cheap.

---

## 13. Module deployment and licensing [#14]

- **Ship `LpcACPIEC.bin`** — the signed module from the upstream **PawnIO.Modules `release_0_2_9.zip`** — byte-for-byte, next to `TPFanControl.exe`. The driver verifies the embedded signature at LOAD; any patching bricks it.
- **License (corrected from v1): the module is LGPL-2.1, not public domain** — unlike this app's own public-domain/Unlicense terms (`portio.cpp:1-16` header). Distribution here is *mere aggregation* (the blob is loaded by the kernel driver, never linked into our process), so obligations are: ship the LGPL-2.1 text + a provenance/attribution notice (upstream project, release tag, unmodified statement, source URL) as a separate file (`LICENSE-LpcACPIEC.txt` or a `THIRD_PARTY_NOTICES` section) — **clearly separated from the app's own license** so neither contaminates the other.
- **Copy rule into every Win32 output/package:** a post-build copy step in `fancontrol.vcxproj` for both `Debug|Win32` and `Release|Win32` output dirs, plus the release zip / any installer manifest. A missing blob ⇒ `Open()` fails at LOAD ⇒ `auto` falls back with the reason logged; `Driver=pawnio` fails startup with the standard UX. No new dialogs.
- **Pinned SHA-256:** record the release blob's hash in the repo; the packaging script (and CI, if this tree ever grows one — runbook §8 notes there is none today) verifies the shipped file against the pin before zipping. Catches silent blob swaps/corruption and documents exactly which signed module was tested.
- The PawnIO **driver** is a system-wide prerequisite with its own signed installer/service — not shipped by us, exactly as the TVic `.sys` files are a prerequisite for the fallback today (runbook §8). Pin the tested pair (driver 2.1.0 ↔ module release_0_2_9) in the release notes (risk R6).
- Docs to touch when implementing: root/`fancontrol.x` READMEs, `DEVELOPER_RUNBOOK.md` §§4.3/4.10/5/8, `SMOKE_TEST.md` (§15 additions; delete Game-Mode rows), and the default ini template (`misc.cpp:745-814`) gains `Driver=auto` + a one-line comment (including "parsed once at startup").

---

## 14. Risk register (REVISED, ranked)

| # | Risk | Likelihood | Impact | Mitigation / disposition |
|---|---|---|---|---|
| **R1** | **P15G2's EC does not answer on 0x66/0x62** — the only ports the stock module allows; this fork prefers TYPE1 (`portio.cpp:24-28,71-75`) and labels the ACPI pair "V0.6.2 final" legacy. *Update vs v1:* the PawnIO **transport** half is now proven on the target (device+VERSION verified); the EC-answer half is still **THE gate**. | Unknown — G0 decides | PawnIO backend useless on target HW | **Must-test-first** (G0, §15). If G0 fails: (a) author a ThinkPad PawnIO module allowlisting `0x1600/0x1604 + 0x1610-0x161F` and PR it upstream (must be signed — lead time), or (b) accept TVic fallback on this hardware — `auto` does this with zero extra code. Sub-risk if G0 passes: 0x66/0x62 is shared with acpi.sys (the contention the code already documents at `fanstuff.cpp:818-821`); measured ~19% ambient single-pass failure makes G1's quantitative gates (§15) the arbiter. |
| **R2** | Transport-lost handling misfires: false-positive latch (transient error classified as fatal) exits the app early; false-negative leaves a gated exit | Low-Med | early exit (fans → firmware: safe) / zombie (the #5 bug) | §6.4 classifies conservatively (only handle/device-level errors latch); §12.2's single recovery absorbs transient handle loss; §12.1's four un-gates are the *only* consumers, all attempt the BIOS handoff first; G2 gains explicit kill/restart-service tests. Failure direction is biased safe: a wrong exit hands fans to firmware, never leaves them wedged at a fixed level (the exit paths still attempt `SetFan(BIOS)`). |
| **R3** | Two overlapping EC mutexes (`Access_Thinkpad_EC` + `Global\Access_EC`) deadlock or starve; group holds starve third-party tools | Low (by construction) | UI/worker stalls | §11: single acquisition order (outer→inner), inner never held across an outer acquire, one bounded ≤500 ms inner wait, release-on-backoff, RAII balance, `WAIT_ABANDONED` = acquired (`winstuff.cpp:56-60` precedent), degrade-if-uncreatable. Group-vs-per-byte starvation trade documented honestly (§11.4). |
| **R4** | TVic fallback availability/regression: delay-load conversion breaks the six-call contract; TWR routing changes block-path behavior; fallback users now exposed to anti-cheat (Game Mode gone) | Low / accepted | fallback broken or flagged | §5: same-signature wrappers, mechanical TWR rename (diff-verifiable), `Driver=tvicport` regression pass in G2; exposure is an explicit maintainer-accepted tradeoff, with PawnIO-only noted as the eventual simplification. |
| **R5** | Error-semantics drift above the seam (void→bool writes + the new lost state perturb the retry/fallback ladder) | Low | behavior drift in edge paths | §4.1/§12: TVic wrapper returns constant TRUE (bit-identical); PawnIO failures alias to the existing timeout vocabulary; exactly one new state with exactly four consumers, each unit-testable with a mock `IPortIo` (§15). |
| **R6** | PawnIO driver/module version skew (IOCTLs, execute ABI, signature policy) | Medium over time | backend stops opening | Constants verified against driver 2.1.0 and source 2.2.0; VERSION queried+logged at every open, major-2 accepted, others warn-and-proceed with LOAD as the true gate; failure mode is clean (open fails → fallback/UX + log). Pin the tested pair in release notes (§13). |
| **R7** | Concurrent instances: GUI + service each open the device and load a module instance; the per-session mutexes (`approot.cpp:17`; `winstuff.h:76`) don't exclude across sessions (gap H) | Low | cross-instance EC interleaving | Per-handle module instances are upstream's design; EC-level interleaving between instances is serialized per-group by `Global\Access_EC` (correctly global, §11) — strictly better than today, where cross-session instances interlock on nothing. Legacy mutex namespaces deliberately untouched (§11.9). G2 dual-run test retained. |
| **R8** | Game-Mode removal leftovers: stale `.sys.bak` from an old build's crash; users who relied on the feature | Low | TVic fallback can't load until healed | Self-heals via the old build's own `MOVEFILE_DELAY_UNTIL_REBOOT` net at next boot (`fancontrol.cpp:1750-1755`); release notes document the manual rename for the pathological case and the feature's retirement rationale (§10.2). |
| **R9** | Startup selection regressions: grace window delays a legitimate TVic boot; SCM start rejected in hardened environments; snapshot parser diverges from `ReadConfig` semantics | Low | slower first fan-takeover (firmware covers); wrong backend | Grace ≤30 s once per boot with firmware in control throughout; SCM start is best-effort (failure just falls through to `CreateFileW`); parser rules are copied from, and unit-tested against, the `ReadConfig` semantics inventoried in §2.7 [#6]. `Driver=tvicport` bypasses grace entirely. |
| **R10** | LGPL/packaging noncompliance for `LpcACPIEC.bin` | Low | licensing complaint | §13: license text + provenance shipped, aggregation-only relationship stated, SHA-256 pin ties the shipped blob to the attributed release. |

---

## 15. Validation plan

- **G0 — hardware gate (BEFORE building the full backend):** on the physical P15G2 with PawnIO + `LpcACPIEC.bin` installed, run a throwaway probe (console tool built from §6's `Exec` sketch): execute the §8.4 handshake for EC reg `0x2F` on 0x66/0x62 and compare against a TVic TYPE1 read taken seconds apart (in BIOS mode both should show bit 7 set — `FAN_CTRL_BIOS`, `fancontrol.h:43`). *The transport half (open/VERSION/marshaling) is already verified; G0 settles only the EC-answer half.* **Pass ⇒ proceed. Fail ⇒ stop; decide R1(a) vs R1(b).**
- **G1 — 24 h Smart-mode soak under PawnIO, with quantitative gates [gap G]** (default `Cycle=5` ⇒ ~17 k polls; compare a same-machine TVic baseline):
	- `MaxReadErrors` escalations (`ReadErrorCount` reaching the threshold, `fancontrol.cpp:3852`): **0 allowed**.
	- `ReadEcStatus` pool exhaustions ("failed to read reliable status values from EC", `fanstuff.cpp:834`): **0 allowed**.
	- Sample-retry ceiling: mean tries per poll ≤ **2.0** and ≥ **99%** of polls succeed within 5 of the 10 tries (`fanstuff.cpp:823-830`). (At the measured ~19% ambient per-pass failure, the expected mean is ≈1.5 — headroom, not slack.)
	- `m_ecErrorsTotal` (`fancontrol.h:139`) ≤ **2×** the TVic baseline over the same 24 h.
	- `SetFan` verify failures ("Result: FAILED!!", `fanstuff.cpp:679`): **0 allowed**.
- **G2 — functional matrix (extend `SMOKE_TEST.md`):** dual-fan RPM + Fan1/Fan2 select under PawnIO; BIOS/Smart/Manual transitions; sleep/resume (S3-style and Modern Standby); service install/run/**stop** under each `Driver=` value; `Driver=pawnio` with the service stopped and disabled (strict-failure UX at the 180 s deadline); blob deleted (`auto` → TVic + correct log); pinned-hash check trips on a modified blob; `UseTWR=1` **and** `UseTWR=2` forcing TVic in all modes [#6]; **cold boot with PawnIO demand-start: `auto` must select PawnIO, not TVic** [gaps A/D]; **stop the PawnIO service mid-run → transport-lost log → one recovery attempt → BIOS-fallback attempt → clean exit** (now reachable by design, gap B); **service stop with a dead transport → SCM reports a clean stop, no exit-code-2 zombie, process gone** [gap C]; restart the PawnIO service before MaxReadErrors → recovery succeeds and polling resumes; GUI + service simultaneously (R7); exe launches with `TVicPort.dll` deleted/quarantined [#7]; `dumpbin /headers` (or equivalent) confirms no `TVicPort.dll` import and `/SAFESEH` present [gap E]; no Game-Mode UI remnants (checkbox, menu item, tooltip tag).
- **Off-hardware (mock `IPortIo`, same spirit as `tests/fanlogic_tests.cpp`):** §9 steering (restricted backend ⇒ pair latched, TYPE1 never probed; unrestricted ⇒ H-02 behavior verbatim), the §12 failure funnel incl. all four un-gates and the once-per-episode recovery latch, the §8.1 selection table incl. grace-window timing (mockable clock), and §8.2 parser conformance (duplicates-last-wins, indented keys dead, comments, `UseTWR=2`, malformed values) against the documented `ReadConfig` semantics.
- **Regression check:** with `Driver=tvicport`, a diff of EC-visible behavior (poll traces, SetFan traces) against the pre-change build should be noise-free — *except* the removed Game-Mode lines, which must be absent.

---

## 16. ABI facts — resolved (was v1 §16 "Open items")

All five v1 `TBD-impl` items are settled and runtime-verified on the target machine (installed driver v2.1.0):

1. Device path: `L"\\\\?\\GLOBALROOT\\Device\\PawnIO"`. The `\\.\PawnIO` DOS symlink is **not** created by the installed driver (`CreateFileW` fails, error 2) — do not use it.
2. IOCTLs: LOAD `0xA1B22084`, EXECUTE `0xA1B22104`, VERSION `0xA1B22184` (device type `0xA1B2`, METHOD_BUFFERED, FILE_ANY_ACCESS); VERSION output = 4-byte `ULONG` `major<<16 | minor<<8 | patch` (observed `0x00020100`).
3. `CreateFileW` share flags: `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` (matches upstream PawnIOLib).
4. Ecosystem mutex: `CreateMutexW(NULL, FALSE, L"Global\\Access_EC")` — the LpcACPIEC-module convention.
5. Concurrency/lifecycle: one module load per handle (second LOAD ⇒ `STATUS_ALREADY_INITIALIZED`); per-handle module instances ⇒ concurrent GUI+service handles are supported; `lpBytesReturned` always equals the requested output size on success (not a produced count); device DACL is SYSTEM/Administrators-only ⇒ elevation mandatory (already required today, runbook §8); the service is **demand-start** ⇒ §6.1's SCM start + §8.3's grace window.

Remaining external verification: none blocking. Re-run the VERSION/open smoke check when the installed driver is upgraded past major 2 (R6).

---

## 17. Change inventory (implementation checklist)

| File | Change |
|---|---|
| `portio_backend.h/.cpp` (new) | `IPortIo`, `PortIoPawn`, `PortIoTVic` + `TVICSHIM` (LoadLibrary/GetProcAddress ×6, exe-dir absolute path), TWR shim wrappers, `PORTIO_STARTUP` + `PortIoParseStartupConfig` (§8.2 rules), `PortIoSelectOnePass` + RAII candidate holder, `PawnIoProbeEc`, `ECMUTEXGUARD`, inked ABI constants, `g_PortIo` |
| `portio.cpp` | ops via `g_PortIo` (bool-aware, §7); `EnsureEcPorts()` replaces the duplicated lazy init (`:71-75`,`:148-152`); port constants (`:24-28`) exported to the shared header; recursive Begin/EndEcGroup safety net |
| `fancontrol.h` | ctor takes `const PORTIO_STARTUP*`; **delete** `ToggleGameMode` (`:278`), `m_driversHidden` (`:332`) |
| `fancontrol.cpp` | ctor: authoritative pair seed after `:131`, before `:289` (§9); backend-reason Trace; **delete** Game-Mode sites (`:144-162, 420-423, 895-896, 989-990, 1591, 1671-1804, 2001, 3467-3468, 3534, 3646-3647`); §12.1 un-gates at `:3566`, `:3817-3824`(+failure branch), `:3852-3859`; §12.2 recovery call in the `WM__NEWDATA` failure branch |
| `fanstuff.cpp` | TWR block (`:952-991`): imports → shim wrappers (mechanical); group brackets in `ReadEcRaw`/`SetFan`/`SetHdw`; **delete** tray "Game mode" tag (`:305-308`) |
| `misc.cpp` | **delete** the `UseTWR=` handler (`:364-367` — snapshot is authoritative); default-ini template (`:745-814`) gains `Driver=auto` + comment |
| `approot.cpp` | **delete** `RecoverHiddenDrivers` (`:313-347`) + call (`:384-386`); retry loop (`:388-399`) → §8.3 deadline/grace selection; hard-access dance (`:401-403`) deleted; `:416` → `g_PortIo->Close()`; error text (`:421-426`) reworded |
| `res\FanControl.rc` | **delete** checkbox 7013 (`:72`, `:151`) and menu item 5090 (`:327`) |
| `fancontrol.vcxproj` | add the two new files; **remove** `TVicPort.lib` (`:203`); **re-enable `/SAFESEH`** + update the comment (`:101-103`); post-build `LpcACPIEC.bin` copy for both Win32 configs |
| Packaging / docs | ship `LpcACPIEC.bin` + `LICENSE-LpcACPIEC.txt` + SHA-256 pin (§13); READMEs, `DEVELOPER_RUNBOOK.md` §§4.3/4.10/5/8, `SMOKE_TEST.md` (§15; drop Game-Mode rows); release notes: Game-Mode retirement + migration note (§10.2) |

## 18. Rejected alternatives (record, so they aren't re-litigated)

1. **Runtime PawnIO→TVic migration on transport loss** (Codex #5 fix): rejected — it silently loads the unsigned, flagged driver mid-session, precisely the outcome this project exists to avoid; the minimal transport-lost + clean-exit design (§12) covers the failure honestly.
2. **Keep Game Mode gated to the TVic backend** (review #12 / v1 §10.2): reversed by the maintainer — the feature's sole purpose is obsolete under a signed primary backend; carrying it means carrying `RecoverHiddenDrivers`, hidden-file states, and the #5 interaction forever (§10.2).
3. **Phase-tracked EC resync on aborted handshakes** (Codex #9): rejected — the existing quiescence wait (`portio.cpp:78,155`), H-01 late-OBF drain (`:122-129`), and the retry ladder already resynchronize; only the transport-lost distinction had substance, and that is #5.
4. **Fail selection when `Global\Access_EC` cannot be created** (Codex #4): rejected — degrade-with-one-shot-log; a thermal-safety app must run (§11.5).
5. **PnP-state ("Degraded") gating** (Codex #16): vacuous — the design never gated on PnP state; nothing to fix.
6. **`/DELAYLOAD` for TVicPort** (#7 middle option): rejected in favor of full dynamic resolution — delay-load keeps the `.lib` (so `/SAFESEH` stays off) and fails as SEH `0xC06D007E` instead of a clean NULL (§5, gap E).
7. **Per-byte `Access_EC` granularity** (v1 §11): rejected — selector-flip hazard on reg 0x31; replaced by logical-group holds with release-on-backoff (§11.2-3).
8. **PawnIO-only (drop TVic now):** deferred, not rejected — kept as the maintainer's documented future simplification; this design isolates TVic so that flipping to PawnIO-only later is a deletion, not a redesign (§5).
