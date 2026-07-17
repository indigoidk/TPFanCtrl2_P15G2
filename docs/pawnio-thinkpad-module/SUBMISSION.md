# LpcACPIEC_ThinkPad — upstream submission notes

**Status: DRAFT.** Not compiled, hardware-tested, packaged, or signed. This is a
proposal for an upstream [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules)
contribution that would let TPFanControl's PawnIO backend reach the legacy ThinkPad
TYPE1 EC pair and the H8S TWR register row (the app's `UseTWR` path), which the stock
`LpcACPIEC` module's 0x62/0x66-only allowlist blocks. Every "Open question" below must be
confirmed by a human/maintainer before this is submitted or loaded on real hardware.

> Premise correction (vs. the app's own notes): stock `LpcACPIEC.p` does **not** acquire a
> mutex or implement an EC command/data handshake — each IOCTL is one byte-port operation,
> and `\BaseNamedObjects\Access_EC` coordination is the **caller's** responsibility. This
> draft keeps that contract.

## Design decision

Add this as a **separate** `LpcACPIEC_ThinkPad` module while leaving the generic
`LpcACPIEC` module unchanged. The ThinkPad variant is a superset that still contains the
standard ACPI pair 0x62/0x66, so one TPFanControl backend can select either its standard or
TYPE1 interface without loading a different module. Keeping it separate avoids silently
extending the capability of every existing `LpcACPIEC` consumer. A run-time "ThinkPad
profile" switch inside the generic module would not be a security boundary: any caller able
to invoke the module could select it.

## Port policy

| Ports | Reads | Writes | Purpose |
|---|---:|---:|---|
| 0x0062, 0x0066 | Yes | Any byte | Stock ACPI EC data and command/status |
| 0x1600, 0x1604 | Yes | Any byte | Legacy ThinkPad TYPE1 data and command/status |
| 0x1610 | Yes | Only 0x20 | TWR0 / function byte used by TPFanControl |
| 0x1611–0x161F | Yes | Only 0x00 | Remaining TWR request bytes and response row |

The range 0x1610–0x161F is intentional: TPFanControl writes or reads every physical byte in
that sixteen-port row. The unused holes 0x1601–0x1603 and 0x1605–0x160F remain denied.

TPFanControl's `UseTWR` sequence polls 0x1604, writes 0x20 to 0x1610, writes zero to
0x1611–0x161F, waits for status 0x50, then reads the entire 0x1610–0x161F row
([reference](https://github.com/Shuzhengz/TPFanCtrl2/blob/main/fancontrol/fanstuff.cpp#L767-L843)).
Independent code in tp_smapi identifies these as the ThinkPad H8S LPC channel 3 STR3/TWR
registers
([reference](https://github.com/linux-thinkpad/tp_smapi/blob/master/thinkpad_ec.c#L47-L59)).
The TWR write-value restrictions are deliberate hardening: allowing arbitrary values at
0x1610 would expose other H8S system-management function codes, not merely temperature
acquisition.

## PawnIO ABI basis

Retains the stock `LpcACPIEC` public ABI:

- `NTSTATUS:main()` is the module load entry point.
- `DEFINE_IOCTL_SIZED` creates public `ioctl_`-prefixed IOCTL functions and validates their
  Pawn-cell counts. `ioctl_pio_read` takes 1 in / 1 out cell; `ioctl_pio_write` takes 2 in.
- Port I/O uses the official `io_in_byte` / `io_out_byte` natives.
- Modules compile for 64-bit Pawn cells. Ports are masked to 16 bits and write values to
  8 bits, matching stock behavior and the byte-width hardware natives.

References: [stock LpcACPIEC](https://github.com/namazso/PawnIO.Modules/blob/main/LpcACPIEC.p),
[IOCTL macros (pawnio.inc)](https://github.com/namazso/PawnIO.Modules/blob/main/include/pawnio.inc),
[port-I/O natives (native.inc)](https://github.com/namazso/PawnIO.Modules/blob/main/include/native.inc),
[driver dispatch (vm.cpp)](https://github.com/namazso/PawnIO/blob/master/PawnIO/src/vm.cpp).

## Serialization and EC handshakes

Stock `LpcACPIEC` does not acquire `Access_EC` and does not implement an EC handshake; it
does one byte read/write and documents that the caller should hold `\BaseNamedObjects\Access_EC`.
PawnIO serializes one public-function execution inside a loaded VM context — that does **not**
serialize a sequence of separate IOCTLs across handles or processes. Consequently
TPFanControl must hold the shared `Access_EC` mutant across the **entire** logical
transaction (all status polling, TWR writes, TWR reads); once-per-byte is insufficient.
This matters because tp_smapi warns that releasing the lock after writing TWR15 but before
the EC starts its reply can hang some firmware
([reference](https://github.com/linux-thinkpad/tp_smapi/blob/master/thinkpad_ec.c#L123-L181)).
The TYPE1 0x1600/0x1604 wait/command/data handshake likewise stays in user mode, as it does
for the stock 0x62/0x66 interface.

## Security rationale and residual risk

Materially narrower than arbitrary port I/O:

- Every accessible port is explicitly enumerated, except the necessary sixteen-byte TWR row.
- Ports between the TYPE1 pair and the TWR row remain inaccessible; every unrelated x86 I/O
  port returns `STATUS_ACCESS_DENIED`.
- TWR writes are limited to the single request pattern observed in TPFanControl instead of
  exposing arbitrary H8S function codes.
- The expanded capability is isolated in a separately named module.

These ports are nevertheless **not "safe" in an absolute sense**. The EC controls thermal,
fan, battery, power, and other platform behavior; raw access can conflict with ACPI/firmware,
leave a transaction incomplete, wedge the EC, or cause incorrect cooling decisions. The
module also does not authenticate callers, validate ordinary EC register numbers/commands,
prove the machine is a supported ThinkPad, enforce TWR order/completion, coordinate with
firmware/drivers that ignore `Access_EC`, or recover a transaction if the caller exits
mid-sequence. The module signature establishes that PawnIO trusts the code and capability;
it does not make every invocation harmless.

A stronger future design would expose a single high-level `ioctl_thinkpad_twr_read_20`
operation performing the complete request, status checks, timeouts, and response read inside
one Pawn invocation — but only after the exact protocol, timing facilities, supported models,
and failure recovery are reviewed.

## Building and testing

From a PawnIO.Modules checkout, place the source at the repo root and compile:

    pawncc LpcACPIEC_ThinkPad.p -C64 -iinclude

`-C64` is mandatory (64-bit Pawn cells). Produces `LpcACPIEC_ThinkPad.amx`. Official
instructions: <https://github.com/namazso/PawnIO.Modules/wiki/Getting-started-with-PawnIO>.

For development only (test-signed/unrestricted driver + Windows test-signing + reboot):

    PawnIOUtil test LpcACPIEC_ThinkPad.amx

**Negative tests** should verify denial of: representative unrelated ports (e.g. 0x60, 0x64,
0x80); holes inside 0x1600–0x161F; TWR0 values other than 0x20; and nonzero writes to
TWR1–TWR15. **Hardware tests** must record exact ThinkPad model, machine type, BIOS version,
and EC firmware version, and cover interruption/error paths and coexistence with the ACPI EC
driver — not only successful sensor reads.

## Signing and release

`pawncc` creates an unsigned `.amx`; it does **not** create a driver-loadable release module.
A maintainer-side operation signs it:

    PawnIOUtil sign input.amx output.bin maintainer-private-key.pem

The official driver accepts only module blobs signed by the private-key holder for its
trusted public key; a contributor's unrelated key will not produce a loadable module. This
module signature is separate from Microsoft's signature on `PawnIO.sys` (Microsoft signs the
kernel driver; the PawnIO maintainer signs the module blob the driver trusts). Per the
[contribution guidelines](https://github.com/namazso/PawnIO.Modules/wiki/Contribution-guidelines),
official modules are merged into PawnIO.Modules and then periodically built, signed, and
released — contributors submit source + references, not a self-described "official signed"
binary.

## Relationship to LpcACPIEC (preferred upstream layout)

1. Keep `LpcACPIEC.p` unchanged.
2. Add `LpcACPIEC_ThinkPad.p` as an explicitly broader hardware profile.
3. Retain identical `ioctl_pio_read` / `ioctl_pio_write` signatures.
4. Document that applications must not load both modules for the same logical EC transaction.
5. Require application-side ThinkPad/model detection until a trustworthy Pawn-side
   supported-hardware check exists.

If maintainers prefer shared implementation, the two modules could be emitted from common
helpers with build-time port-profile selection (a run-time IOCTL-selectable profile is less
attractive — it does not reduce the capability available to a caller). Changing
`is_port_write_allowed` to match `is_port_read_allowed` would create a literal raw-port
superset and is **not** recommended (it would allow arbitrary TWR function codes).

## Suggested PR

**Title:** Add scoped ThinkPad LPC3 EC access module

**Body:** Add a separate `LpcACPIEC_ThinkPad` module for TPFanControl and similar ThinkPad
software. It preserves the existing `ioctl_pio_read`/`ioctl_pio_write` ABI and the stock
0x62/0x66 ACPI EC pair, and additionally permits the legacy ThinkPad TYPE1 pair at
0x1600/0x1604 and the sixteen-byte H8S LPC3 TWR row at 0x1610–0x161F. Unrelated ports
(including holes in 0x1600–0x161F) remain denied; TWR writes are restricted to the exact
`UseTWR` request (0x20 at TWR0, zero at TWR1–TWR15), avoiding arbitrary H8S function codes.
The ABI and `Access_EC` caller-coordination contract remain compatible with `LpcACPIEC`.
Reference implementations: TPFanControl's `UseTWR` path and tp_smapi's H8S LPC3
implementation. Testing: CI/warning-free compile TODO; negative-allowlist tests TODO;
hardware models/BIOS/EC revisions TODO.

## Open questions requiring maintainer or hardware confirmation

1. Which exact ThinkPad models and EC firmware revisions support TYPE1 and the 0x20 TWR
   request? tp_smapi's model matrix is not a definitive TPFanControl compatibility list.
2. No authoritative Lenovo spec was found establishing that TWR function 0x20 universally
   returns thermal sensors. The observed TPFanControl behavior is confirmed; its universal
   meaning is not.
3. Confirm that writing zero to TWR15 is correct for function 0x20 on every target firmware
   (tp_smapi supports an explicit TWR15 value but otherwise defaults it to 0x01).
4. Decide whether the raw multi-IOCTL TWR sequence is acceptable given the firmware lock
   warning, or whether upstream should require a single atomic high-level IOCTL before
   accepting TWR access.
5. PawnIO's current API was not found to provide a simple SMBIOS/DMI native; decide whether
   separate-module selection plus user-mode DMI checking is sufficient supported-hardware
   detection.
6. Confirm that restricting TWR write values is acceptable for an upstream module intended
   specifically for TPFanControl (broader consumers' documented function codes must be
   reviewed rather than permitting all byte values).
7. Replace `<SUBMITTER>` in the source copyright header.
8. Before submission, byte-compare the base against release 0.2.9 (`git show 0.2.9:LpcACPIEC.p`);
   this draft was checked against current upstream `main`, and the tagged 0.2.9 `LpcACPIEC.p`
   was not independently byte-compared in this environment.

---
*Drafted by Codex (gpt-5.6-sol, ultra) with web research against upstream PawnIO sources, at
the request of the TPFanControl P15G2 fork maintainer. Reviewed before commit; the open
questions above are the explicit human/maintainer confirmation gates.*
