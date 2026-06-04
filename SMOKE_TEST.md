# TPFanControl — Manual Smoke-Test Checklist

This app touches kernel drivers (TVicPort), a Windows service, raw embedded-controller
(EC) port I/O, and renames driver files for Game Mode. Those surfaces are hard to cover
with unit tests, so run this checklist before tagging a release (or after changing
anything in `approot.cpp`, `portio.cpp`, `fanstuff.cpp`, or the close/shutdown paths).

Run on a real ThinkPad (the target is a P15 Gen2). Use an **elevated** prompt — EC
access and service install require administrator rights. Keep the lid/fan audible so
you can hear the fan respond.

Legend: ☐ = to verify. Note the build under test: `Release\TPFanControl.exe` (commit ___).

---

## 0. Build

- ☐ `msbuild fancontrol.sln /m /p:Configuration=Release /p:Platform=Win32` → 0 errors.
- ☐ `msbuild fancontrol.sln /m /p:Configuration=Debug   /p:Platform=Win32` → 0 errors.
- ☐ Only the known pre-existing warning (`GetVersion` deprecation in `SystemTraySDK.cpp`).

## 1. Normal UI run

- ☐ Launch `TPFanControl.exe`. Window appears; temperatures and both fan RPMs update.
- ☐ Tray icon shows; tooltip shows max temp + RPM; color tracks temperature thresholds.
- ☐ Switch **BIOS → Smart → Manual** via radios; the Log records each mode change.
- ☐ In Manual, drag the speed slider; fan audibly changes; `Switch` temp/`Fan Level` update.
- ☐ Leave Manual and let temp exceed `ManModeExit` → it auto-reverts to Smart.
- ☐ Toggle Dark mode, Show log, Temp hex — UI re-themes/relays without artifacts.
- ☐ Resize the window — controls reflow; the log auto-shrink/restore works.

## 2. Interactive close (the well-exercised path)

- ☐ Exit via tray menu **End program** (or window close if configured to exit).
- ☐ Fan is set back to **BIOS (0x80)** before exit — confirm "On close ... OK" in the log.
- ☐ Process is gone (Task Manager); no orphaned tray icon after a mouse-over.

## 3. Windows shutdown / restart (deferred-restore regression)

> This exercises `WM_ENDSESSION`. The restore must happen **synchronously**, not deferred.

- ☐ Set Manual or Smart so fan control is active (fan not BIOS-default).
- ☐ Initiate **Restart**. Before it tears down, the app must set fan back to BIOS.
- ☐ After reboot, inspect `TPFanControl.log` (if `Log2File=1`): an **"On shutdown ... OK"**
      line is present, and there is **no** "Delaying close" left hanging.
- ☐ Fan is not stuck at a fixed manual level after the reboot/login.

## 4. Game Mode (driver hide/restore)

- ☐ Enable **Game mode (Hide Drivers)**. Balloon confirms; `TVicHW64.sys` and
      `TVicPort64.sys` in `C:\Windows\System32\drivers` become `*.sys.bak`.
- ☐ Disable Game mode. Both `.sys` files are restored; no `.bak` left behind.
- ☐ Enable Game mode, then exit the app cleanly → drivers are restored on exit.
- ☐ Partial-failure rollback (optional, advanced): make the 2nd rename fail (e.g. hold
      one file open) and confirm the **first rename is rolled back** — you never end up
      with one `.sys` and one `.sys.bak`.

## 5. Service lifecycle (the focus area)

> Service runs headless; the worker opens the EC port (retries up to 180 s) then runs
> the same control loop. Verify install/start/stop/uninstall and a stop *during* startup.

- ☐ **Install:** `TPFanControl.exe -i` (elevated). Service `TPFanControl` appears in
      `services.msc`. Its **ImagePath is quoted** — check:
      `sc qc TPFanControl` shows `BINARY_PATH_NAME : "C:\...\TPFanControl.exe" -s`
      (quotes present even if the path has spaces).
- ☐ **Start:** `sc start TPFanControl` (or via services.msc). State → RUNNING within a
      few seconds. Fan control is active (audible / EC reads happening).
- ☐ **Stop:** `sc stop TPFanControl`. Stops **promptly** (well under 15 s), state →
      STOPPED. No hang, no lingering process.
- ☐ **Stop during startup:** stop the service immediately after start, *while* it is
      still in the 180 s `OpenTVicPort` retry (e.g. temporarily rename the TVic driver so
      the port won't open). It must still stop promptly (the stop event aborts the retry)
      and not hang on an INFINITE wait.
- ☐ **Start failure reporting:** if the worker/event can't be created, `sc start` reports
      a failure (state STOPPED with a specific exit code) rather than a stuck START_PENDING.
- ☐ **Uninstall:** `TPFanControl.exe -u`. Service removed from `services.msc`.
- ☐ After stop/uninstall: no leaked handles to the EC; relaunching the UI works normally.

## 6. EC robustness

- ☐ Temperatures look sane (no wildly wrong / stale values jumping around).
- ☐ Under load (stress the CPU/GPU) the fan ramps according to the Smart table.
- ☐ Pull the log: no sustained "readec: timed out" / "failed to read reliable status"
      spam. Occasional retries are normal; a continuous stream is not.
- ☐ Force many consecutive read errors (rare) → app falls back to BIOS mode and closes
      rather than driving the fan on stale data.

## 7. Config safety

- ☐ Change settings via the in-app Settings dialog → `TPFanControl.ini` is updated and
      the original is never lost even if a save is interrupted (atomic replace).
- ☐ Hand-edit `IgnoreSensors=pci,aps` (lower case as the ini documents) → those sensors
      are excluded from the max-temp calculation.
- ☐ A config with an excessive number of `level=` / `level2=` lines does not crash
      (parsing is bounded to the array size).

---

### Notes / sign-off

- Tester / date / machine: ______________________
- Build / commit: ______________________
- Result: ☐ pass ☐ pass-with-notes ☐ fail — details: ______________________
