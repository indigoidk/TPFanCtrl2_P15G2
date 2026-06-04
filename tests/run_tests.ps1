# Compile and run the standalone fanlogic unit tests with MSVC.
# Touches nothing in the main project (no .sln/.vcxproj changes).
#
#   powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
#
# Exit code 0 = all tests passed.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio." }
$vsPath = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

Push-Location $here
try {
    $cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /EHsc /std:c++17 /W4 /Fe:fanlogic_tests.exe fanlogic_tests.cpp"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "compilation failed (exit $LASTEXITCODE)" }
    & (Join-Path $here "fanlogic_tests.exe")
    $rc = $LASTEXITCODE
    exit $rc
}
finally {
    Pop-Location
}
