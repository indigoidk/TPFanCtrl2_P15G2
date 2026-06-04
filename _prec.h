//systemheaders in one file for using precompiled headers.

// Target Windows Vista (0x0600) minimum: the app already calls Vista+ APIs
// unconditionally (RegisterPowerSettingNotification, DWM immersive dark mode),
// so it cannot run lower anyway, and this exposes GetTickCount64 etc.
#define _WIN32_WINNT 0x0600
//only most neccessary things from windows
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <process.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tchar.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "winuser.h"
#include "windows.h"
#include <richedit.h>
