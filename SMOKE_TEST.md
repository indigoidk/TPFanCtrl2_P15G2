# TPFanControl — Manual Smoke-Test Checklist

This app talks to the PawnIO kernel driver, runs as a Windows service, and performs
raw embedded-controller (EC) port I/O. Those surfaces are hard to cover with unit
tests, so run this checklist before tagging a release (or after changing anything in
`approot.cpp`, `portio.cpp`, `portio_pawn.cpp`, `fanstuff.cpp`, or the close/shutdown
paths).

Run on a real ThinkPad (the target is a P15 Gen2). Use an **elevated** prompt — EC
access and service install require administrator rights. Keep the lid/fan audible so
you can hear the fan respond.

Legend: ☐ = to verify. Note the build under test: `Release\TPFanControl.exe` (commit ___).

---

## 0. Build

- ☐ `msbuild fancontrol.sln /m /p:Configuration=Release /p:Platform=Win32` → 0 errors.
- ☐ `msbuild fancontrol.sln /m /p:Configuration=Debug   /p:Platform=Win32` → 0 errors.
- ☐ Build is **warning-free** (the old `GetVersion` deprecation was removed in v2.34).
- ☐ `powershell tests\run_tests.ps1` → all fan-logic checks pass.

## 1. Normal UI run

- ☐ Launch `TPFanControl.exe`. Window appears; temperatures and both fan RPMs update.
- ☐ Title bar shows the app icon (top-left system-menu corner); caption says **v2.34**.
- ☐ Tray icon shows as a **rounded badge** with crisp digits; tooltip shows mode +
      max temp + fan + RPM; color tracks temperature thresholds.
- ☐ Switch **BIOS → Smart → Manual** via radios; the Log records each mode change.
- ☐ In Manual, drag the speed slider (accent-colored track, round thumb); fan audibly
      changes; `Switch` temp / `Fan Level` update.
- ☐ Slide to **MAX** → a TaskDialog warns; tick *"Don't ask again until the next
      start"*, cancel, slide again → second prompt honors/skips per the checkbox.
- ☐ Leave Manual and let temp exceed `ManModeExit` → it auto-reverts to Smart.
- ☐ Hover the temperature history graph → tracking tooltip shows the sample's value
      and age; a marker follows the cursor; right-click → Clear history still works.
- ☐ Tick **Show log** → the panel opens **pre-filled with recent lines** (startup,
      mode changes), and new events stream in live without flicker.
- ☐ Resize the window — controls reflow, no flicker during the drag; the log
      auto-shrink/restore works.
- ☐ Keyboard: Tab reaches the BIOS/Smart/Manual radios and the all/active filter;
      arrow keys move within each group; focus rectangles visible from launch.

## 2. Win11 UI & theming (new in v2.34)

- ☐ Toggle **Dark mode** → window, title bar, menus, tooltips, Settings and Curve
      dialogs all flip; no light remnants (check tooltips especially).
- ☐ Set `DarkMode=2` (or tick *Follow system theme* in Settings) → switching the
      Windows app theme (Settings → Personalization → Colors) re-themes the app live,
      including an *open* Settings dialog.
- ☐ Change the Windows **accent color** → section headers and the "On" state word
      update (main window immediately; open dialogs too).
- ☐ Settings: section headers are bold + accent-colored; unit hints are dimmed;
      OK/Apply/Cancel right-aligned; *"Show in taskbar"* checkbox present.
- ☐ `ShowInTaskbar=1` (or the checkbox): taskbar button appears with a severity
      overlay dot and a fan-level progress fill; Alt-Tab shows the app; toggling off
      removes the button cleanly.
- ☐ Tray: **Enter/Space** on the keyboard-focused tray icon toggles the window once
      (no double-toggle); right-click menu opens anchored at the icon; window
      fades in/out instead of snapping.
- ☐ Kill and restart `explorer.exe` (Task Manager) → the tray icon **returns** within
      a second (elevated-app UIPI fix).
- ☐ (Optional) Enable a High Contrast theme → app adopts system colors, title bar
      returns to system chrome; disable it → custom theming returns.

## 3. Interactive close (the well-exercised path)

- ☐ Exit via tray menu **End program** (or window close if configured to exit).
- ☐ Fan is set back to **BIOS (0x80)** before exit — confirm "On close ... OK" in the log.
- ☐ Process is gone (Task Manager); no orphaned tray icon after a mouse-over.

## 4. Windows shutdown / restart (deferred-restore regression)

> This exercises `WM_ENDSESSION`. The restore must happen **synchronously**, not deferred.

- ☐ Set Manual or Smart so fan control is active (fan not BIOS-default).
- ☐ Initiate **Restart**. Before it tears down, the app must set fan back to BIOS.
- ☐ After reboot, inspect `TPFanControl.log` (if `Log2File=1`): an **"On shutdown ... OK"**
      line is present, and there is **no** "Delaying close" left hanging.
- ☐ Fan is not stuck at a fixed manual level after the reboot/login.

## 5. Sleep / Modern Standby (new in v2.34)

> Default `SuspendMode=1`: BIOS during sleep, mode restored after a ~10 s EC settle.

- ☐ In **Manual** mode, sleep the machine (power menu → Sleep). Log shows
      *"Sleep transition: handing fan control to BIOS"* (via the Modern Standby
      watcher or APM path).
- ☐ Wake it. Log shows *"Resume detected - deferring EC access (10s)"*, then
      ~10 s later *"Resume settle complete - restoring pre-sleep mode"*; the Manual
      radio is selected again and the fan level is re-asserted.
- ☐ Close the lid, wait for standby, open it → lid handling and sleep handling
      don't fight (mode ends up back where it was).
- ☐ Temperatures and RPMs are sane immediately after the settle window (no
      garbage readings logged during the first polls).
- ☐ With `SuspendMode=0`, sleep/wake changes nothing (feature fully opt-out).

## 6. Safety guards

- ☐ **Fail-safe**: set `FailsafeTemp` just above idle temp, load the CPU → at the
      threshold the fan forces to max and the log shows *"Fail-safe TRIPPED"*;
      cooling ~3 °C below releases it.
- ☐ **Stall watchdog** (passive): grep the log for *"Fan stall"* — should appear
      **only** if the EC genuinely dropped a command; none expected in normal runs.
- ☐ **Emergency hibernate** (optional, careful): set `CriticalTemp=70` (just above
      a loaded temp), stress briefly → after 3 polls at/above it the log records
      *"CRITICAL ... hibernating"* and the machine hibernates; on resume it does
      **not** immediately re-hibernate (re-arms only 5 °C below). Reset to 0/95 after.

## 7. PawnIO backend

- ☐ With PawnIO installed, `LpcACPIEC.bin` beside the exe, and an elevated launch,
      the app opens promptly and reports sane temperatures/RPMs.
- ☐ Temporarily move `LpcACPIEC.bin` away and launch interactively → startup retries,
      then the error names PawnIO, the missing module, and Administrator privileges;
      no windowless background process remains. Restore the module afterward.
- ☐ Set `UseTWR=1` → the log reports that TWR is unsupported exactly once, then
      standard per-register temperature reads continue normally.
- ☐ `dumpbin /imports Release\TPFanControl.exe` shows no TVicPort import, and
      `dumpbin /loadconfig` shows a populated Safe Exception Handler Table.

## 8. Service lifecycle

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
      still in the 180 s PawnIO-open retry (e.g. temporarily move `LpcACPIEC.bin` so
      the transport won't open). It must still stop promptly (the stop event aborts
      the retry) and not hang on an INFINITE wait. Restore the module afterward.
- ☐ **Start failure reporting:** if the worker/event can't be created, `sc start` reports
      a failure (state STOPPED with a specific exit code) rather than a stuck START_PENDING.
- ☐ **Uninstall:** `TPFanControl.exe -u`. Service removed from `services.msc`.
- ☐ After stop/uninstall: no leaked handles to the EC; relaunching the UI works normally.

## 9. EC robustness

- ☐ Temperatures look sane (no wildly wrong / stale values jumping around).
- ☐ Under load (stress the CPU/GPU) the fan ramps according to the Smart table.
- ☐ Pull the log: no sustained "readec: timed out" / "failed to read reliable status"
      spam. Occasional retries are normal; a continuous stream is not.
- ☐ Force many consecutive read errors (rare) → the Status field shows
      *"EC read error n/m - showing last good reading"*, then the app falls back to
      BIOS mode and closes rather than driving the fan on stale data.
- ☐ During an error streak, restore from the tray → readouts show the **last good
      reading**, not blanks.

## 10. Config safety

- ☐ Change settings via the in-app Settings dialog → `TPFanControl.ini` is updated and
      the original is never lost even if a save is interrupted (atomic replace).
- ☐ Hand-edit `IgnoreSensors=pci,aps` (lower case as the ini documents) → those sensors
      are excluded from the max-temp calculation.
- ☐ A config with an excessive number of `level=` / `level2=` lines does not crash
      (parsing is bounded to the array size).
- ☐ **First run with no ini** (rename `TPFanControl.ini`, launch): a default ini is
      written **and the window + tray icon appear** on that first run.

---

### Notes / sign-off

- Tester / date / machine: ______________________
- Build / commit: ______________________
- Result: ☐ pass ☐ pass-with-notes ☐ fail — details: ______________________
