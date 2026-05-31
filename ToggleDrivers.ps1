# ToggleDrivers.ps1
# Hides or restores TVicPort driver files to avoid Riot Vanguard detection.
# Double-click to toggle: first run hides, second run restores.
# Requires Administrator — will self-elevate via UAC prompt.

#region Self-elevate
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe `
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" `
        -Verb RunAs
    exit
}
#endregion

Add-Type -AssemblyName System.Windows.Forms

$driversDir = "C:\Windows\System32\drivers"
$names = @("TVicHW64.sys", "TVicPort64.sys")

function Show-Msg($text, $title, $icon) {
    [System.Windows.Forms.MessageBox]::Show(
        $text, $title,
        [System.Windows.Forms.MessageBoxButtons]::OK,
        $icon) | Out-Null
}

# Determine current state
$origCount = 0
$bakCount  = 0
foreach ($n in $names) {
    if (Test-Path (Join-Path $driversDir $n))        { $origCount++ }
    if (Test-Path (Join-Path $driversDir "$n.bak"))  { $bakCount++  }
}

if ($origCount -eq $names.Count) {
    # --- HIDE: rename .sys -> .sys.bak ---
    $errors = @()
    foreach ($n in $names) {
        $src = Join-Path $driversDir $n
        $dst = "$src.bak"
        try   { Rename-Item -LiteralPath $src -NewName "$n.bak" -Force -ErrorAction Stop }
        catch { $errors += "$n : $_" }
    }
    if ($errors.Count -eq 0) {
        Show-Msg "TVic drivers hidden.`n`nSafe to launch Riot games.`nRun this script again after you finish to restore them." `
                 "TVic Driver Toggle — Hidden" `
                 ([System.Windows.Forms.MessageBoxIcon]::Information)
    } else {
        Show-Msg ("Errors renaming files:`n" + ($errors -join "`n")) `
                 "TVic Driver Toggle — Error" `
                 ([System.Windows.Forms.MessageBoxIcon]::Error)
    }

} elseif ($bakCount -eq $names.Count) {
    # --- RESTORE: rename .sys.bak -> .sys ---
    $errors = @()
    foreach ($n in $names) {
        $src = Join-Path $driversDir "$n.bak"
        try   { Rename-Item -LiteralPath $src -NewName $n -Force -ErrorAction Stop }
        catch { $errors += "$n.bak : $_" }
    }
    if ($errors.Count -eq 0) {
        Show-Msg "TVic drivers restored.`n`nTPFanControl can run normally." `
                 "TVic Driver Toggle — Restored" `
                 ([System.Windows.Forms.MessageBoxIcon]::Information)
    } else {
        Show-Msg ("Errors renaming files:`n" + ($errors -join "`n")) `
                 "TVic Driver Toggle — Error" `
                 ([System.Windows.Forms.MessageBoxIcon]::Error)
    }

} else {
    # Mixed / unexpected state
    $status = ($names | ForEach-Object {
        $o = Test-Path (Join-Path $driversDir $_)
        $b = Test-Path (Join-Path $driversDir "$_.bak")
        "$_  orig=$o  bak=$b"
    }) -join "`n"
    Show-Msg "Drivers are in a mixed state — fix manually:`n`n$status" `
             "TVic Driver Toggle — Mixed State" `
             ([System.Windows.Forms.MessageBoxIcon]::Warning)
}
