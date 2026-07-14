#include "_prec.h"
#include "approot.h"
#include "fancontrol.h"
#include "TVicPort.h"

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR aArgs, int) {
    hInstRes = instance;
    hInstApp = instance;

    // Harden runtime DLL loads (e.g. LoadLibraryA("msftedit.dll")) against
    // planting: search only System32 by name, never the (possibly user-writable)
    // application directory. NOTE: this does NOT cover the load-time TVicPort.dll
    // import, which the loader resolves before WinMain runs - that one is addressed
    // by installing to a protected directory / code-signing, not by this call.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

	HANDLE hLock = CreateMutex(NULL,FALSE,"TPFanControlMutex01");

  if (hLock == NULL) {
      DWORD ec = GetLastError();
      ShowError(ec, "program or service already running");

      return ec;
  }

  if (WAIT_OBJECT_0 != WaitForSingleObject(hLock,0)) {
      DWORD ec = GetLastError();
      ShowError(ec, "program or service already running");
	
      return ec;
  }

    if (aArgs && *aArgs) {
        bool install = false;
        bool uninstall = false;
        bool quiet = false;
		bool debug = false;
		bool run = false;
        char *args = aArgs;
        while (*args) {
            if (*args == '-' || *args == '/') {
                ++args;
				switch (*args) {
				case 'i':
				case 'I': install = true; break;
				case 'u':
				case 'U': uninstall = true; break;
				case 'q':
				case 'Q': quiet = true; break;
				case 'd':
				case 'D': debug = true; break;
				case 's':
				case 'S': run = true; break;
				default: ShowHelp(); return -1;
                }
                ++args;
            }
            else if (*args == ' ') {
                ++args;
            }
            else {
                ShowHelp();
                return -1;
            }
        }
        
		if (install) {
            return InstallService(quiet);
        }

        if (uninstall) {
            return UninstallService(quiet);
        }

		if (debug) {
			WorkerThread(NULL);
			return 0;
		}

		if (run) {
			// HANDLE hLockS = CreateMutex(NULL,FALSE,"TPFanControlMutex02");
			SERVICE_TABLE_ENTRY svcEntry[2];
			svcEntry[0].lpServiceName = g_ServiceName;
			svcEntry[0].lpServiceProc = ServiceMain;
			svcEntry[1].lpServiceName = NULL;
			svcEntry[1].lpServiceProc = NULL;
			StartServiceCtrlDispatcher(svcEntry);
		}
    }
    else {
		WorkerThread(NULL);
		return 0;
    }

    return 0;
}

void ShowHelp() {
    MessageBox(NULL, "Usage:\n\n-i Install service\n-u Uninstall service\n-q Quiet - Don't show possible error messages", "Usage", MB_OK);
}

DWORD InstallService(bool quiet) {
    SC_HANDLE SCMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!SCMgr) {
        DWORD ec = GetLastError();
        if (!quiet) ShowError(ec, "Could not open Service Control Manager");
        return ec;
    }

    char ExePath[MAX_PATH];
    GetModuleFileName(NULL, ExePath, sizeof(ExePath));
    // Quote the executable path so SCM parses it as a single token. Without the
    // quotes, an install path containing spaces breaks service startup and is a
    // classic unquoted-service-path weakness.
    char CmdLine[MAX_PATH + 8];
    sprintf_s(CmdLine, sizeof(CmdLine), "\"%s\" -s", ExePath);

    SC_HANDLE svc = CreateService(SCMgr, g_ServiceName, g_ServiceName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        CmdLine, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        CloseServiceHandle(SCMgr);
        DWORD ec = GetLastError();
        if (!quiet) ShowError(ec, "Could not install service");
        return ec;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(SCMgr);

    return 0;
}

DWORD UninstallService(bool quiet) {
    SC_HANDLE SCMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!SCMgr) {
        DWORD ec = GetLastError();
        if (!quiet) ShowError(ec, "Could not open Service Control Manager");
        return ec;
    }

    SC_HANDLE hdl = OpenService(SCMgr, g_ServiceName, DELETE);
    if (!hdl) {
        DWORD ec = GetLastError();
        CloseServiceHandle(SCMgr);          // don't leak the SCM handle on this path
        if (ec == ERROR_SERVICE_DOES_NOT_EXIST)
            return 0;                       // already gone: a genuine success
        if (!quiet) ShowError(ec, "Could not open service for deletion");
        return ec;                          // any other failure is NOT success
    }

    if (!DeleteService(hdl)) {
        DWORD ec = GetLastError();
        if (!quiet) ShowError(ec, "Could not delete service");
        CloseServiceHandle(hdl);            // was leaked on the failure path
        CloseServiceHandle(SCMgr);
        return ec;
    }

    CloseServiceHandle(hdl);
    CloseServiceHandle(SCMgr);

    return 0;
}

void ShowError(DWORD ec, const char *description) {
    char *msgBuf = NULL;

    DWORD n = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        ec,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &msgBuf,
        0, NULL );

    // FormatMessage can fail (leaving msgBuf untouched/NULL); never deref it blindly
    const char *msg = (n && msgBuf) ? msgBuf : "(no system message available)";

    size_t dispBuf_len = strlen(msg) + strlen(description) + 40;
    char *dispBuf = (char *)LocalAlloc(LMEM_ZEROINIT, dispBuf_len);
    if (dispBuf) {
        sprintf_s(dispBuf, dispBuf_len, "%s, error code %d: %s", description, ec, msg);
        MessageBox(NULL, dispBuf, "Error", MB_OK);
        LocalFree(dispBuf);
    }
    else {
        MessageBox(NULL, msg, "Error", MB_OK);   // allocation failed: show what we have
    }

    if (msgBuf) LocalFree(msgBuf);
}

void ShowMessage(const char *title, const char *description) { 
    MessageBox(NULL, description, title, MB_OK);
}

VOID WINAPI ServiceMain(DWORD aArgc, LPTSTR* aArgv) {
    g_SvcHandle = RegisterServiceCtrlHandler(g_ServiceName, Handler);

    g_SvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_SvcStatus.dwCurrentState = SERVICE_START_PENDING;
    g_SvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_SvcStatus.dwWin32ExitCode = NO_ERROR;
    g_SvcStatus.dwServiceSpecificExitCode = NO_ERROR;
    g_SvcStatus.dwCheckPoint = 0;
    g_SvcStatus.dwWaitHint = 0;
    SetServiceStatus(g_SvcHandle, &g_SvcStatus);

    if (!StartWorkerThread()) {
        // could not create the stop event or worker thread: report a clean
        // failure to the SCM instead of sitting in a half-started state
        g_SvcStatus.dwCurrentState = SERVICE_STOPPED;
        g_SvcStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_SvcStatus.dwServiceSpecificExitCode = 1;
        SetServiceStatus(g_SvcHandle, &g_SvcStatus);
        return;
    }

    g_SvcStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_SvcHandle, &g_SvcStatus);

    return;
}

VOID WINAPI Handler(DWORD fdwControl) {
    switch(fdwControl) {
    case SERVICE_CONTROL_STOP:
        g_SvcStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_SvcHandle, &g_SvcStatus);

        if (StopWorkerThread()) {
            g_SvcStatus.dwWin32ExitCode = NO_ERROR;
            g_SvcStatus.dwServiceSpecificExitCode = 0;
        }
        else {
            // worker did not stop: report a stop failure rather than claim a
            // clean STOPPED while it is still running EC/UI code
            g_SvcStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            g_SvcStatus.dwServiceSpecificExitCode = 2;
        }
        g_SvcStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_SvcHandle, &g_SvcStatus);

        break;

    default:
        break;
    }
}

// _beginthreadex trampoline: unlike _beginthread, the returned handle stays
// valid until we CloseHandle it, so service stop can wait on it safely.
static unsigned __stdcall WorkerThreadTramp(void* p) {
    WorkerThread(p);
    return 0;
}

bool StartWorkerThread() {
    if (!g_stopEvent) {
        g_stopEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);   // manual-reset
        if (!g_stopEvent) {
            debug("StartWorkerThread: CreateEvent failed\r\n");
            return false;
        }
    }
    g_workerThread = (HANDLE)_beginthreadex(NULL, 0, WorkerThreadTramp, NULL, 0, NULL);
    if (!g_workerThread) {
        debug("StartWorkerThread: _beginthreadex failed\r\n");
        ::CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
        return false;
    }
    return true;
}

// Returns true only if the worker actually exited. On a timeout the worker is
// still running EC/UI code, so we must NOT close its handle or let the caller
// report the service as cleanly stopped.
bool StopWorkerThread() {
    // Abort the startup OpenTVicPort retry if we are still in it (g_dialogWnd not
    // yet set), so a stop arriving during boot doesn't hang on the wait below.
    if (g_stopEvent)
        ::SetEvent(g_stopEvent);

    // Ask the UI to close cleanly only if the window actually exists yet.
    if (g_dialogWnd && ::IsWindow(g_dialogWnd))
        ::PostMessage(g_dialogWnd, WM_COMMAND, 5020, 0);

    bool stopped = true;
    if (g_workerThread) {
        // Bounded wait so service stop can never hang forever.
        if (::WaitForSingleObject(g_workerThread, 15000) == WAIT_OBJECT_0) {
            ::CloseHandle(g_workerThread);   // _beginthreadex handle is owned by us
            g_workerThread = NULL;
        }
        else {
            debug("StopWorkerThread: worker did not exit within 15s\r\n");
            stopped = false;                 // leave the handle: thread still alive
        }
    }

    // only release the stop event once the worker (which may read it) is gone
    if (stopped && g_stopEvent) {
        ::CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }
    return stopped;
}

// Crash recovery for Game Mode: if a previous session hid the TVic kernel drivers
// (renamed *.sys -> *.sys.bak) but crashed or lost power before the clean-exit
// restore ran, the drivers can't load and the OpenTVicPort retry below would fail
// for 180s and then give up - bricking the app. Rename them back FIRST. Per-file
// and unconditional so a partial (one .sys, one .bak) crash state heals too; Game
// Mode is not persisted across runs, so "visible on startup" is the intended state.
static void RecoverHiddenDrivers() {
	static const char* const sys[2] = {
		"C:\\Windows\\System32\\drivers\\TVicHW64.sys",
		"C:\\Windows\\System32\\drivers\\TVicPort64.sys"
	};
	// 32-bit process: bypass WOW64 redirection so System32 is the real one.
	typedef BOOL (WINAPI *PFN_Disable)(PVOID*);
	typedef BOOL (WINAPI *PFN_Revert)(PVOID);
	HMODULE hK = ::GetModuleHandleA("kernel32.dll");
	PFN_Disable pfnOff = hK ? (PFN_Disable)::GetProcAddress(hK, "Wow64DisableWow64FsRedirection") : NULL;
	PFN_Revert  pfnOn  = hK ? (PFN_Revert) ::GetProcAddress(hK, "Wow64RevertWow64FsRedirection")  : NULL;
	PVOID fsOld = NULL;
	bool redir = pfnOff && pfnOff(&fsOld);

	for (int i = 0; i < 2; i++) {
		char bak[MAX_PATH];
		sprintf_s(bak, sizeof(bak), "%s.bak", sys[i]);
		bool hasBak = ::GetFileAttributesA(bak)    != INVALID_FILE_ATTRIBUTES;
		bool hasSys = ::GetFileAttributesA(sys[i]) != INVALID_FILE_ATTRIBUTES;
		if (hasBak && !hasSys)
			debug(::MoveFileExA(bak, sys[i], 0)
				? "RecoverHiddenDrivers: restored a TVic driver from .sys.bak\r\n"
				: "RecoverHiddenDrivers: FAILED to restore a TVic driver from .sys.bak\r\n");
		else if (hasBak && hasSys)
			::DeleteFileA(bak);   // stale .bak alongside a present .sys: clean it up
	}

	if (redir) pfnOn(fsOld);
}

void WorkerThread(void *dummy) {
	char curdir[MAX_PATH]= "";
	
	//   #ifdef _DEBUG   
	//   Sleep(30000);
	//   #endif

	hInstRes=GetModuleHandle(NULL);
	hInstApp=hInstRes;

	// explicit registration of the classes the dialogs use (trackbar, tooltips,
	// progress live in the Win95 set; buttons/edits/statics in the standard set)
	INITCOMMONCONTROLSEX icc = { sizeof(icc),
		ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
	::InitCommonControlsEx(&icc);

	// Change to the directory where the exe resides
	char exepath[MAX_PATH];
	*exepath = '\0';
	if (GetModuleFileName(NULL, exepath, MAX_PATH))	{
		char *p = exepath + strlen(exepath) - 1;
		while (p > exepath) {
			if (*p == '\\')	{
				*p = '\0';
				::SetCurrentDirectory(exepath);
				break;
			}
			--p;
		}
	}

    bool ok = false;
	bool HardAccess = false;
	bool NewHardAccess = true;

	// Undo a Game-Mode crash before we touch the port: if the TVic drivers were
	// left renamed to .sys.bak, restore them now or OpenTVicPort can't load them.
	RecoverHiddenDrivers();

    for (int i = 0; i < 180; i++) {
        if (OpenTVicPort()) {
            ok = true;
            break;
        }
        // wait 1s, but abort immediately if a service stop was requested while
        // we are still trying to open the port (before the dialog window exists)
        if (g_stopEvent && ::WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0)
            break;
        if (!g_stopEvent)
            ::Sleep(1000);
    }
	if (ok) {	
		HardAccess = TestHardAccess();
		SetHardAccess(NewHardAccess);
		HardAccess = TestHardAccess();

		LoadLibraryA("msftedit.dll");  // register RICHEDIT50W (RichEdit 4.1) for the temp list
		FANCONTROL fc(hInstApp);

        g_dialogWnd = fc.GetDialogWnd();

		fc.ProcessDialog();

		// the dialog loop has exited; clear the shared handle before fc's
		// destructor tears the window down so a concurrent service stop can't
		// post to a window that is about to be (or already) destroyed
		g_dialogWnd = NULL;
		CloseTVicPort();
	}
	else {
		// a service can't meaningfully show a modal box (and shouldn't block on one);
		// only pop the error in interactive mode
		if (!g_SvcHandle)
			::MessageBox(HWND_DESKTOP,
						"Error during initialization of Port Driver.\r\n"
						"(tvicport.sys missing in app folder or failed to load)",
						"Fan Control",
						MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}

	// If running as a service and this exit was NOT triggered by an SCM stop (the
	// control handler reports STOPPED itself in that case), report STOPPED now so a
	// dead worker — e.g. the EC port never opened — doesn't leave the service stuck
	// in RUNNING.
	bool stopRequested = g_stopEvent && ::WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
	if (g_SvcHandle && !stopRequested) {
		g_SvcStatus.dwCurrentState = SERVICE_STOPPED;
		g_SvcStatus.dwWin32ExitCode = ok ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
		g_SvcStatus.dwServiceSpecificExitCode = ok ? 0 : 3;
		::SetServiceStatus(g_SvcHandle, &g_SvcStatus);
	}
}

void debug(const char *msg) {
	FILE *flog;

    errno_t errflog = fopen_s(&flog,"fancontrol_debug.log", "ab");
	if (!errflog) {
		fwrite(msg, strlen(msg), 1, flog); 
		fclose(flog);
	}
}
