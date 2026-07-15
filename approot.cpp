#include "_prec.h"
#include "approot.h"
#include "fancontrol.h"
#include "portio_pawn.h"

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR aArgs, int) {
    hInstRes = instance;
    hInstApp = instance;

    // Harden name-based runtime DLL loads (e.g. LoadLibraryA("msftedit.dll"))
    // against planting: search only System32, never the application directory.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

	// Detect a service launch (-s / -S) up front. A service runs on the invisible
	// session-0 desktop, where a modal MessageBox would block forever with nobody
	// to dismiss it - fatal now that SCM auto-restart can relaunch us into a still
	// single-instance-locked mutex (e.g. a zombie worker from a timed-out stop).
	// The full arg parse happens below; this is only enough to keep startup quiet.
	bool serviceLaunch = false;
	if (aArgs) {
		for (const char *p = aArgs; *p; ++p)
			if ((*p == '-' || *p == '/') && (p[1] == 's' || p[1] == 'S'))
				serviceLaunch = true;
	}

	HANDLE hLock = CreateMutex(NULL,FALSE,"TPFanControlMutex01");

  if (hLock == NULL) {
      DWORD ec = GetLastError();
      if (!serviceLaunch) ShowError(ec, "program or service already running");

      return ec;
  }

  if (WAIT_OBJECT_0 != WaitForSingleObject(hLock,0)) {
      DWORD ec = GetLastError();
      if (!serviceLaunch) ShowError(ec, "program or service already running");

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

    // Depend on the PawnIO driver so the SCM starts it (it is DEMAND_START) BEFORE
    // our ServiceMain/worker run. Our Open() then finds PawnIO already RUNNING and
    // never calls StartServiceW itself - eliminating the ROOT of the STOP-vs-
    // StartServiceW deadlock: a worker StartServiceW blocked behind our own in-flight
    // STOP control (the SCM globally serializes service controls) would otherwise
    // wedge the 15s stop-wait into an unwanted restart. The pre-check in
    // portio_pawn.cpp remains as defense-in-depth for a PawnIO crash in the gap
    // between the SCM's dependency-start and our Open(). Double-null-terminated
    // single-entry dependency list.
    static const char kPawnIoDependency[] = "PawnIO\0";

    SC_HANDLE svc = CreateService(SCMgr, g_ServiceName, g_ServiceName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        CmdLine, NULL, NULL, kPawnIoDependency, NULL, NULL);

    if (!svc) {
        CloseServiceHandle(SCMgr);
        DWORD ec = GetLastError();
        if (!quiet) ShowError(ec, "Could not install service");
        return ec;
    }

    // Auto-recovery: if the service exits with a failure code (e.g. the PawnIO
    // transport was permanently lost), have the SCM restart it on an escalating
    // backoff - 30s, then 2min, then 10min repeated for all later failures (the SCM
    // repeats the last action once the failure count exceeds cActions). This heals
    // fast when the transport returns quickly, yet won't hammer the SCM/event log
    // every ~3.5min if PawnIO is gone for good. The counter resets after a day of
    // health. Deliberately unbounded (no SC_ACTION_NONE): a hard cap would defeat
    // recovery after a later PawnIO reinstall/upgrade.
    SC_ACTION restartActions[3];
    restartActions[0].Type = SC_ACTION_RESTART; restartActions[0].Delay = 30000;    // 30 s
    restartActions[1].Type = SC_ACTION_RESTART; restartActions[1].Delay = 120000;   // 2 min
    restartActions[2].Type = SC_ACTION_RESTART; restartActions[2].Delay = 600000;   // 10 min (repeats)
    SERVICE_FAILURE_ACTIONS sfa;
    ZeroMemory(&sfa, sizeof(sfa));
    sfa.dwResetPeriod = 86400;     // 1 day
    sfa.cActions = 3;
    sfa.lpsaActions = restartActions;
    BOOL faOk = ChangeServiceConfig2(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);
    DWORD faErr = faOk ? 0 : GetLastError();   // capture now: the next call clobbers GetLastError
    // By default failure actions fire only on a hard process crash; also honor them
    // for our graceful STOPPED-with-non-zero-exit-code failure path (the actual
    // dead-transport signal). Without this flag the restart never fires.
    SERVICE_FAILURE_ACTIONS_FLAG sfaFlag;
    sfaFlag.fFailureActionsOnNonCrashFailures = TRUE;
    BOOL flagOk = ChangeServiceConfig2(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &sfaFlag);
    DWORD flagErr = flagOk ? 0 : GetLastError();
    DWORD cfgErr = faErr ? faErr : flagErr;   // first failure, if either call failed
    if (cfgErr && !quiet) {
        // The base service installed, but transport-loss auto-restart is a core
        // recovery guarantee - surface its absence rather than degrade silently.
        ShowError(cfgErr,
            "Service installed, but the auto-restart recovery policy could not be configured");
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(SCMgr);

    // Non-zero on a recovery-config failure so `-i` (including -q) does not report
    // success without the auto-restart guarantee. The base service is left installed
    // and functional (fan control still works); only the SCM auto-restart policy is
    // missing. Re-running `-i` will NOT reconfigure it (CreateService then returns
    // ERROR_SERVICE_EXISTS); uninstall (`-u`) then reinstall (`-i`) to reapply it.
    return cfgErr;
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

// Serialize who owns the terminal service-lifecycle transition. A dead-transport
// self-exit (WorkerThread) and an operator stop (Handler) can race: without this,
// both could write SERVICE_STOPPED (a second terminal SetServiceStatus is
// undefined), and the worker's failure-code STOPPED would queue an SCM restart the
// Handler's later clean STOPPED cannot cancel - restarting a service the operator
// asked to stop. The first to claim ownership (atomic CAS, no lock held across the
// worker wait) issues the single terminal status; the loser does nothing.
enum { OWNER_NONE = 0, OWNER_WORKER = 1, OWNER_HANDLER = 2 };
static LONG g_lifecycleOwner = OWNER_NONE;
static bool ClaimLifecycle(LONG who) {
    return ::InterlockedCompareExchange(&g_lifecycleOwner, who, OWNER_NONE) == OWNER_NONE;
}
static void ReportStopped(DWORD win32ExitCode, DWORD specificExitCode) {
    g_SvcStatus.dwCurrentState = SERVICE_STOPPED;
    g_SvcStatus.dwCheckPoint = 0;
    g_SvcStatus.dwWaitHint = 0;   // terminal state: clear any STOP_PENDING wait hint
    g_SvcStatus.dwWin32ExitCode = win32ExitCode;
    g_SvcStatus.dwServiceSpecificExitCode = specificExitCode;
    ::SetServiceStatus(g_SvcHandle, &g_SvcStatus);
}

VOID WINAPI ServiceMain(DWORD aArgc, LPTSTR* aArgv) {
    g_SvcHandle = RegisterServiceCtrlHandler(g_ServiceName, Handler);
    if (!g_SvcHandle)
        // Without a status handle we can neither report status nor be controlled;
        // bail before starting the worker (which would otherwise see g_SvcHandle==NULL
        // and misclassify this session-0 run as interactive desktop mode).
        return;

    g_SvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_SvcStatus.dwCurrentState = SERVICE_START_PENDING;
    // Do NOT accept STOP yet: g_stopEvent and the worker thread don't exist until
    // StartWorkerThread below. Accepting a stop here would let the Handler report a
    // terminal STOPPED and close the status context while ServiceMain then creates
    // the worker and reports RUNNING on that closed context.
    g_SvcStatus.dwControlsAccepted = 0;
    g_SvcStatus.dwWin32ExitCode = NO_ERROR;
    g_SvcStatus.dwServiceSpecificExitCode = NO_ERROR;
    g_SvcStatus.dwCheckPoint = 0;
    g_SvcStatus.dwWaitHint = 0;
    SetServiceStatus(g_SvcHandle, &g_SvcStatus);

    if (!StartWorkerThread()) {
        // could not create the stop event or worker thread: report a clean
        // failure to the SCM instead of sitting in a half-started state
        if (ClaimLifecycle(OWNER_HANDLER))
            ReportStopped(ERROR_SERVICE_SPECIFIC_ERROR, 1);
        return;
    }

    // Stop infrastructure now exists; only now advertise STOP and go RUNNING.
    g_SvcStatus.dwCurrentState = SERVICE_RUNNING;
    g_SvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    SetServiceStatus(g_SvcHandle, &g_SvcStatus);

    // Release the worker only now that RUNNING is published, so a fast-fail worker
    // can't report a terminal STOPPED before this RUNNING write (which would then
    // land on a closed status context, or leave the SCM at RUNNING with a dead
    // worker). On the near-impossible ResumeThread failure we'd be stuck RUNNING
    // with a suspended worker, so fail out cleanly instead.
    if (::ResumeThread(g_workerThread) == (DWORD)-1) {
        if (ClaimLifecycle(OWNER_HANDLER))
            ReportStopped(ERROR_SERVICE_SPECIFIC_ERROR, 1);
    }

    return;
}

VOID WINAPI Handler(DWORD fdwControl) {
    switch(fdwControl) {
    case SERVICE_CONTROL_STOP:
        // Claim operator-stop ownership first. If the worker already claimed a
        // self-exit (dead transport), let its terminal STOPPED stand: don't publish
        // STOP_PENDING (which would regress an already-written STOPPED) or a second
        // terminal status.
        if (!ClaimLifecycle(OWNER_HANDLER))
            break;

        g_SvcStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_SvcStatus.dwCheckPoint = 1;
        g_SvcStatus.dwWaitHint = 20000;   // StopWorkerThread waits up to 15s; give the SCM headroom
        SetServiceStatus(g_SvcHandle, &g_SvcStatus);

        if (StopWorkerThread())
            ReportStopped(NO_ERROR, 0);
        else
            // worker did not stop: report a stop failure rather than claim a
            // clean STOPPED while it is still running EC/UI code
            ReportStopped(ERROR_SERVICE_SPECIFIC_ERROR, 2);

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
    // Create SUSPENDED: the worker must not run (and possibly fast-fail all the way
    // to its terminal STOPPED report) before ServiceMain has published RUNNING.
    // ServiceMain resumes it right after that report. See ServiceMain / BLOCKING-1.
    g_workerThread = (HANDLE)_beginthreadex(NULL, 0, WorkerThreadTramp, NULL, CREATE_SUSPENDED, NULL);
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
    // Abort the startup transport-open retry if we are still in it (g_dialogWnd not
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
	bool dialogCreated = false;
	g_PortIo = CreatePawnIoTransport();

	if (g_PortIo) {
		for (int i = 0; i < 180; i++) {
			if (g_PortIo->Open()) {
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
	}
	if (ok) {
		LoadLibraryA("msftedit.dll");  // register RICHEDIT50W (RichEdit 4.1) for the temp list
		FANCONTROL fc(hInstApp);

        g_dialogWnd = fc.GetDialogWnd();
		dialogCreated = (g_dialogWnd != NULL);

		// If an SCM stop arrived during FANCONTROL construction - before g_dialogWnd
		// was published - StopWorkerThread's one-shot 5020 post went to a NULL window
		// and was lost. Re-post it now so we unwind promptly instead of running until
		// the SCM's 15s stop-wait times out, which reports a stop failure that (with
		// the new failure actions) would restart a service the operator asked to stop.
		if (g_stopEvent && ::WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0
			&& g_dialogWnd && ::IsWindow(g_dialogWnd))
			::PostMessage(g_dialogWnd, WM_COMMAND, 5020, 0);

		fc.ProcessDialog();

		// the dialog loop has exited; clear the shared handle before fc's
		// destructor tears the window down so a concurrent service stop can't
		// post to a window that is about to be (or already) destroyed
		g_dialogWnd = NULL;
		g_PortIo->Close();
	}
	else {
		// a service can't meaningfully show a modal box (and shouldn't block on one);
		// only pop the error in interactive mode
		if (!g_SvcHandle)
			::MessageBox(HWND_DESKTOP,
						"Error during initialization of the PawnIO port transport.\r\n"
						"PawnIO may not be installed, LpcACPIEC.bin may be missing next to the executable,\r\n"
						"or TPFanControl may not be running as Administrator.",
						"Fan Control",
						MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}

	// Free the transport ONLY on the !ok path: there Open() never succeeded, so
	// no FANCONTROL and no per-poll EC worker thread was ever created and the
	// delete is race-free. On the ok path the worker thread can, in a rare
	// pathological case, outlive its bounded join timeout (the exit handlers
	// proceed on timeout, fancontrol.cpp), and deleting the transport under a
	// still-live worker would be a use-after-free - the old global-function
	// TVicPort path made the identical race harmless, a heap-owned transport does
	// not. On that path g_PortIo->Close() (above) already released the driver +
	// mutex handles; the small object is reclaimed by the OS at process exit, and
	// g_PortIo is left non-NULL so any late worker read fails cleanly via the
	// closed transport instead of NULL-dereferencing.
	if (!ok && g_PortIo) {
		delete g_PortIo;
		g_PortIo = NULL;
	}

	// If running as a service and this exit was NOT triggered by an SCM stop (the
	// control handler reports STOPPED itself in that case), report STOPPED now so a
	// dead worker — e.g. the EC port never opened — doesn't leave the service stuck
	// in RUNNING.
	bool stopRequested = g_stopEvent && ::WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
	if (g_SvcHandle && !stopRequested && ClaimLifecycle(OWNER_WORKER)) {
		// A self-exit with the PawnIO transport permanently lost is a FAILURE:
		// report a non-zero exit code so the SCM's restart action fires and we come
		// back on a fresh transport. A normal exit (transport alive) stays clean.
		// Ownership was just claimed, so the Handler won't also write a terminal
		// status (and a normal SCM stop set stopRequested, skipping this entirely).
		bool transportLost = (g_PortIo && g_PortIo->TransportLost());
		// A window that never came up (dialogCreated false - e.g. a session-0
		// desktop-heap exhaustion) is an abnormal exit, not a clean stop: report a
		// failure so the SCM restart heals the (usually transient) condition instead
		// of the service silently vanishing.
		bool clean = ok && dialogCreated && !transportLost;
		ReportStopped(clean ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR,
			clean ? 0 : (transportLost ? 4 : 3));
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
