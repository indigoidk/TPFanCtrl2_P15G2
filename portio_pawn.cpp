// --------------------------------------------------------------
//
//  Thinkpad Fan Control - PawnIO port-I/O transport
//
// --------------------------------------------------------------
//
//  This program and source code is in the public domain.
//
// --------------------------------------------------------------

#include "_prec.h"
#include "portio_pawn.h"

#include <new>
#include <stddef.h>

// Defined in approot.h (compiled into approot.cpp): signalled on service stop. The
// PawnIO transport peeks it to abort a driver StartServiceW that a pending stop
// would otherwise wedge (see the StartServiceW pre-check below). Declared at file
// scope so the reference has EXTERNAL linkage - a block-scope extern inside the
// anonymous namespace below binds to a non-existent internal symbol (C7631).
extern HANDLE g_stopEvent;

namespace {

static const WCHAR kPawnIoDevice[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";
static const WCHAR kPawnIoService[] = L"PawnIO";
static const WCHAR kPawnIoModule[] = L"LpcACPIEC.bin";
static const WCHAR kEcMutexName[] = L"Global\\Access_EC";

static const DWORD kIoctlPioLoadBinary = 0xA1B22084;
static const DWORD kIoctlPioExecuteFn = 0xA1B22104;
static const DWORD kIoctlPioVersion = 0xA1B22184;

static const DWORD kEcMutexWaitMs = 500;
static const DWORD kServiceStartWaitMs = 3000;
static const DWORD kDeviceRetryDelayMs = 100;
static const DWORD kMaxModuleBytes = 1u * 1024u * 1024u;
static const SIZE_T kExecuteNameBytes = 32;
static const ULONG kMaxExecuteInputCells = 2;
static const ULONG kMaxExecuteOutputCells = 1;

#pragma pack(push, 1)
struct PIO_EXECUTE_INPUT {
	char	Name[32];
	ULONG64	Args[kMaxExecuteInputCells];
};
#pragma pack(pop)

static_assert(sizeof(ULONG64) == 8, "PawnIO ABI requires 8-byte ULONG64 cells");
static_assert(sizeof(((PIO_EXECUTE_INPUT*)0)->Name) == 32,
	"PawnIO ABI requires a 32-byte execute name field");
static_assert(offsetof(PIO_EXECUTE_INPUT, Args) == 32,
	"PawnIO execute arguments must start at byte 32");
static_assert(sizeof(PIO_EXECUTE_INPUT) == 32 + 2 * sizeof(ULONG64),
	"PawnIO execute input layout changed unexpectedly");

class IO_LOCK_GUARD {
public:
	explicit IO_LOCK_GUARD(CRITICAL_SECTION* lock) : m_lock(lock) {
		::EnterCriticalSection(this->m_lock);
	}

	~IO_LOCK_GUARD() {
		::LeaveCriticalSection(this->m_lock);
	}

private:
	IO_LOCK_GUARD(const IO_LOCK_GUARD&);
	IO_LOCK_GUARD& operator=(const IO_LOCK_GUARD&);

	CRITICAL_SECTION* m_lock;
};

void
PawnIoLog(const char* message) {
	DWORD error = ::GetLastError();
	::OutputDebugStringA(message);
	::SetLastError(error);
}

void
PawnIoLogError(const char* operation, DWORD error) {
	char message[224];
	::sprintf_s(message, sizeof(message),
		"PortIoPawn: %s failed (Win32 error %lu).\r\n",
		operation, (unsigned long)error);
	PawnIoLog(message);
}

void
PawnIoLogServiceStopped(DWORD serviceError) {
	char message[224];
	::sprintf_s(message, sizeof(message),
		"PortIoPawn: PawnIO service stopped during startup (service error %lu).\r\n",
		(unsigned long)serviceError);
	PawnIoLog(message);
}

HANDLE
OpenPawnIoDevice() {
	return ::CreateFileW(kPawnIoDevice,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, 0, NULL);
}

bool
StartPawnIoService() {
	SC_HANDLE manager = ::OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		DWORD error = ::GetLastError();
		PawnIoLogError("OpenSCManagerW", error);
		::SetLastError(error);
		return false;
	}

	SC_HANDLE service = ::OpenServiceW(manager, kPawnIoService,
		SERVICE_START | SERVICE_QUERY_STATUS);
	if (service == NULL) {
		DWORD error = ::GetLastError();
		PawnIoLogError("OpenServiceW(PawnIO)", error);
		::CloseServiceHandle(manager);
		::SetLastError(error);
		return false;
	}

	const ULONGLONG deadline = ::GetTickCount64() + kServiceStartWaitMs;
	bool startIssued = false;
	bool running = false;
	DWORD finalError = ERROR_SUCCESS;

	for (;;) {
		SERVICE_STATUS_PROCESS status;
		DWORD bytesNeeded = 0;
		::ZeroMemory(&status, sizeof(status));

		if (!::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
				(LPBYTE)&status, sizeof(status), &bytesNeeded)) {
			finalError = ::GetLastError();
			PawnIoLogError("QueryServiceStatusEx(PawnIO)", finalError);
			break;
		}

		if (status.dwCurrentState == SERVICE_RUNNING) {
			running = true;
			break;
		}

		if (status.dwCurrentState == SERVICE_STOPPED) {
			if (startIssued) {
				DWORD serviceError = status.dwWin32ExitCode ==
					ERROR_SERVICE_SPECIFIC_ERROR
					? status.dwServiceSpecificExitCode
					: status.dwWin32ExitCode;
				PawnIoLogServiceStopped(serviceError);
				finalError = ERROR_SERVICE_NOT_ACTIVE;
				break;
			}

			if (::GetTickCount64() >= deadline) {
				PawnIoLog("PortIoPawn: timed out waiting for the PawnIO service.\r\n");
				finalError = ERROR_TIMEOUT;
				break;
			}

			// A service stop already in flight must abort BEFORE StartServiceW:
			// starting a driver service can block ~30s and the SCM serializes it
			// behind our own in-flight STOP control, which would wedge the worker
			// past StopWorkerThread's 15s wait and provoke an unwanted restart. If the
			// stop is already signaled, skip the driver start and let the worker
			// unwind. (Layering compromise: the transport peeks the app's stop event.
			// The residual sub-ms window between this check and StartServiceW acquiring
			// the SCM lock is self-healing via the SCM restart.)
			if (::g_stopEvent && ::WaitForSingleObject(::g_stopEvent, 0) == WAIT_OBJECT_0) {
				finalError = ERROR_SERVICE_NOT_ACTIVE;
				break;
			}

			startIssued = true;
			// StartServiceW for a driver service can itself block on SCM/driver
			// initialization (up to about 30 seconds) before returning, so
			// kServiceStartWaitMs bounds only the post-return status poll, not
			// total startup latency. This is accepted for a startup path.
			if (!::StartServiceW(service, 0, NULL)) {
				DWORD error = ::GetLastError();
				if (error != ERROR_SERVICE_ALREADY_RUNNING) {
					PawnIoLogError("StartServiceW(PawnIO)", error);
					finalError = error;
					break;
				}
			}
			continue;
		}

		if (status.dwCurrentState != SERVICE_START_PENDING &&
			status.dwCurrentState != SERVICE_STOP_PENDING) {
			PawnIoLog("PortIoPawn: PawnIO service did not enter the running state.\r\n");
			finalError = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
			break;
		}

		ULONGLONG now = ::GetTickCount64();
		if (now >= deadline) {
			PawnIoLog("PortIoPawn: timed out waiting for the PawnIO service.\r\n");
			finalError = ERROR_TIMEOUT;
			break;
		}

		DWORD delay = status.dwWaitHint / 10;
		if (delay < 50)
			delay = 50;
		if (delay > 250)
			delay = 250;

		ULONGLONG remaining = deadline - now;
		if ((ULONGLONG)delay > remaining)
			delay = (DWORD)remaining;
		if (delay == 0)
			delay = 1;

		::Sleep(delay);
	}

	::CloseServiceHandle(service);
	::CloseServiceHandle(manager);
	::SetLastError(finalError);
	return running;
}

bool
BuildModulePath(WCHAR** modulePath) {
	if (modulePath == NULL) {
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	*modulePath = NULL;

	const SIZE_T maxPathChars = 32768;
	SIZE_T capacity = MAX_PATH;

	for (;;) {
		if (capacity == 0 || capacity > maxPathChars ||
			capacity > ((SIZE_T)-1) / sizeof(WCHAR) ||
			capacity > MAXDWORD) {
			PawnIoLog("PortIoPawn: executable path is too long.\r\n");
			::SetLastError(ERROR_FILENAME_EXCED_RANGE);
			return false;
		}

		WCHAR* path = (WCHAR*)::HeapAlloc(::GetProcessHeap(), 0,
			capacity * sizeof(WCHAR));
		if (path == NULL) {
			PawnIoLogError("HeapAlloc(executable path)", ERROR_NOT_ENOUGH_MEMORY);
			::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
			return false;
		}

		::SetLastError(ERROR_SUCCESS);
		DWORD length = ::GetModuleFileNameW(NULL, path, (DWORD)capacity);
		if (length == 0) {
			DWORD error = ::GetLastError();
			PawnIoLogError("GetModuleFileNameW", error);
			::HeapFree(::GetProcessHeap(), 0, path);
			::SetLastError(error);
			return false;
		}

		if ((SIZE_T)length >= capacity) {
			::HeapFree(::GetProcessHeap(), 0, path);
			if (capacity == maxPathChars) {
				PawnIoLog("PortIoPawn: executable path is too long.\r\n");
				::SetLastError(ERROR_FILENAME_EXCED_RANGE);
				return false;
			}
			capacity = capacity > maxPathChars / 2
				? maxPathChars : capacity * 2;
			continue;
		}

		WCHAR* slash = NULL;
		for (DWORD i = length; i != 0; --i) {
			if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
				slash = path + i - 1;
				break;
			}
		}

		if (slash == NULL) {
			PawnIoLog("PortIoPawn: executable path has no directory component.\r\n");
			::HeapFree(::GetProcessHeap(), 0, path);
			::SetLastError(ERROR_BAD_PATHNAME);
			return false;
		}

		SIZE_T directoryChars = (SIZE_T)(slash - path) + 1;
		SIZE_T fileNameChars = (SIZE_T)::lstrlenW(kPawnIoModule);
		if (directoryChars > (SIZE_T)-1 - fileNameChars - 1) {
			PawnIoLog("PortIoPawn: module path size overflow.\r\n");
			::HeapFree(::GetProcessHeap(), 0, path);
			::SetLastError(ERROR_ARITHMETIC_OVERFLOW);
			return false;
		}

		SIZE_T required = directoryChars + fileNameChars + 1;
		if (required > capacity) {
			::HeapFree(::GetProcessHeap(), 0, path);
			if (required > maxPathChars) {
				PawnIoLog("PortIoPawn: module path is too long.\r\n");
				::SetLastError(ERROR_FILENAME_EXCED_RANGE);
				return false;
			}
			capacity = required;
			continue;
		}

		::CopyMemory(path + directoryChars, kPawnIoModule,
			(fileNameChars + 1) * sizeof(WCHAR));
		*modulePath = path;
		return true;
	}
}

bool
ReadModuleBlob(BYTE** blob, DWORD* blobSize) {
	if (blob == NULL || blobSize == NULL) {
		::SetLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	*blob = NULL;
	*blobSize = 0;

	WCHAR* path = NULL;
	if (!BuildModulePath(&path))
		return false;

	HANDLE file = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	DWORD openError = file == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
	::HeapFree(::GetProcessHeap(), 0, path);
	if (file == INVALID_HANDLE_VALUE) {
		PawnIoLogError("CreateFileW(LpcACPIEC.bin)", openError);
		::SetLastError(openError);
		return false;
	}

	LARGE_INTEGER size;
	if (!::GetFileSizeEx(file, &size)) {
		DWORD error = ::GetLastError();
		PawnIoLogError("GetFileSizeEx(LpcACPIEC.bin)", error);
		::CloseHandle(file);
		::SetLastError(error);
		return false;
	}

	if (size.QuadPart > 0 &&
		(ULONGLONG)size.QuadPart > (ULONGLONG)kMaxModuleBytes) {
		PawnIoLog("PortIoPawn: LpcACPIEC.bin exceeds the 1 MiB size limit.\r\n");
		::CloseHandle(file);
		::SetLastError(ERROR_FILE_TOO_LARGE);
		return false;
	}

	if (size.QuadPart <= 0 ||
		(ULONGLONG)size.QuadPart > (ULONGLONG)MAXDWORD ||
		(ULONGLONG)size.QuadPart > (ULONGLONG)((SIZE_T)-1)) {
		PawnIoLog("PortIoPawn: LpcACPIEC.bin has an invalid or unsupported size.\r\n");
		::CloseHandle(file);
		::SetLastError(ERROR_BAD_LENGTH);
		return false;
	}

	DWORD length = (DWORD)size.QuadPart;
	BYTE* data = (BYTE*)::HeapAlloc(::GetProcessHeap(), 0, (SIZE_T)length);
	if (data == NULL) {
		PawnIoLogError("HeapAlloc(LpcACPIEC.bin)", ERROR_NOT_ENOUGH_MEMORY);
		::CloseHandle(file);
		::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}

	DWORD totalRead = 0;
	while (totalRead < length) {
		DWORD remaining = length - totalRead;
		DWORD request = remaining > 1024 * 1024 ? 1024 * 1024 : remaining;
		DWORD readNow = 0;
		BOOL readOk = ::ReadFile(file, data + totalRead, request, &readNow, NULL);
		if (!readOk || readNow == 0) {
			DWORD error = readOk ? ERROR_HANDLE_EOF : ::GetLastError();
			PawnIoLogError("ReadFile(LpcACPIEC.bin)", error);
			::HeapFree(::GetProcessHeap(), 0, data);
			::CloseHandle(file);
			::SetLastError(error);
			return false;
		}
		totalRead += readNow;
	}

	::CloseHandle(file);
	*blob = data;
	*blobSize = length;
	return true;
}

} // namespace

class PortIoPawn : public IPortIo {
public:
	PortIoPawn();
	virtual ~PortIoPawn();

	virtual bool Open();
	virtual void Close();
	virtual bool IsOpen() const;
	virtual bool ReadPort8(USHORT port, UCHAR* pdata);
	virtual bool WritePort8(USHORT port, UCHAR data);
	virtual bool PortAllowed(USHORT port) const;
	virtual bool BeginEcTransaction();
	virtual void EndEcTransaction();
	virtual bool TransportLost() const;
	virtual const char* Name() const;

private:
	PortIoPawn(const PortIoPawn&);
	PortIoPawn& operator=(const PortIoPawn&);

	bool OpenDevice(bool allowServiceStart);
	bool OpenDeviceAndInitialize(bool allowServiceStart);
	void CloseDevice();
	void CloseEcMutex();
	bool LoadModuleBlob();
	void QueryAndLogVersion();
	bool ExecuteOnce(const char* functionName,
		const ULONG64* input, ULONG inputCount,
		ULONG64* output, ULONG outputCount, DWORD* error,
		bool* localFailure);
	bool ExecuteWithRecovery(const char* functionName,
		const ULONG64* input, ULONG inputCount,
		ULONG64* output, ULONG outputCount);
	void LatchTransportLost(const char* reason, DWORD error);

	HANDLE		m_device;
	HANDLE		m_ecMutex;
	ULONG		m_driverVersion;
	mutable volatile LONG m_transportLost;
	mutable CRITICAL_SECTION m_ioLock;
};

PortIoPawn::PortIoPawn()
	: m_device(INVALID_HANDLE_VALUE),
	  m_ecMutex(NULL),
	  m_driverVersion(0),
	  m_transportLost(0) {
	::InitializeCriticalSection(&this->m_ioLock);
}

PortIoPawn::~PortIoPawn() {
	this->Close();
	::DeleteCriticalSection(&this->m_ioLock);
}

bool
PortIoPawn::OpenDevice(bool allowServiceStart) {
	if (this->m_device != INVALID_HANDLE_VALUE)
		return true;

	HANDLE device = OpenPawnIoDevice();
	if (device != INVALID_HANDLE_VALUE) {
		this->m_device = device;
		return true;
	}

	DWORD error = ::GetLastError();
	if (!allowServiceStart || error != ERROR_FILE_NOT_FOUND) {
		PawnIoLogError("CreateFileW(PawnIO)", error);
		::SetLastError(error);
		return false;
	}

	// PawnIO is demand-start. The SCM attempt is best effort; device retries
	// still run because the service can finish starting while SCM status lags.
	StartPawnIoService();

	for (unsigned int attempt = 0; attempt != 2; ++attempt) {
		if (attempt != 0)
			::Sleep(kDeviceRetryDelayMs);

		device = OpenPawnIoDevice();
		if (device != INVALID_HANDLE_VALUE) {
			this->m_device = device;
			return true;
		}

		error = ::GetLastError();
		if (error != ERROR_FILE_NOT_FOUND)
			break;
	}

	PawnIoLogError("CreateFileW(PawnIO) after service start", error);
	::SetLastError(error);
	return false;
}

bool
PortIoPawn::LoadModuleBlob() {
	if (this->m_device == INVALID_HANDLE_VALUE) {
		::SetLastError(ERROR_INVALID_HANDLE);
		return false;
	}

	BYTE* blob = NULL;
	DWORD blobSize = 0;
	if (!ReadModuleBlob(&blob, &blobSize))
		return false;

	DWORD bytesReturned = 0;
	BOOL loaded = ::DeviceIoControl(this->m_device, kIoctlPioLoadBinary,
		blob, blobSize, NULL, 0, &bytesReturned, NULL);
	DWORD error = loaded ? ERROR_SUCCESS : ::GetLastError();
	::HeapFree(::GetProcessHeap(), 0, blob);

	if (!loaded) {
		PawnIoLogError("IOCTL_PIO_LOAD_BINARY", error);
		::SetLastError(error);
		return false;
	}

	return true;
}

void
PortIoPawn::QueryAndLogVersion() {
	this->m_driverVersion = 0;
	if (this->m_device == INVALID_HANDLE_VALUE)
		return;

	ULONG version = 0;
	DWORD bytesReturned = 0;
	if (!::DeviceIoControl(this->m_device, kIoctlPioVersion,
			NULL, 0, &version, sizeof(version), &bytesReturned, NULL)) {
		DWORD error = ::GetLastError();
		PawnIoLogError("IOCTL_PIO_VERSION", error);
		return;
	}

	if (bytesReturned != sizeof(version)) {
		PawnIoLog("PortIoPawn: IOCTL_PIO_VERSION returned an invalid size.\r\n");
		return;
	}

	this->m_driverVersion = version;
	ULONG major = (version >> 16) & 0xffff;
	ULONG minor = (version >> 8) & 0xff;
	ULONG patch = version & 0xff;
	char message[192];
	::sprintf_s(message, sizeof(message),
		"PortIoPawn: PawnIO driver v%lu.%lu.%lu (0x%08lX).\r\n",
		(unsigned long)major, (unsigned long)minor, (unsigned long)patch,
		(unsigned long)version);
	PawnIoLog(message);

	if (major != 2 || (minor != 1 && minor != 2))
		PawnIoLog("PortIoPawn: warning: this PawnIO driver version is untested.\r\n");
}

bool
PortIoPawn::OpenDeviceAndInitialize(bool allowServiceStart) {
	if (!this->OpenDevice(allowServiceStart))
		return false;

	if (!this->LoadModuleBlob()) {
		DWORD error = ::GetLastError();
		this->CloseDevice();
		::SetLastError(error);
		return false;
	}

	// VERSION is diagnostic rather than an acceptance gate. The signed module
	// load and actual execute calls are the authoritative compatibility checks.
	this->QueryAndLogVersion();
	return true;
}

bool
PortIoPawn::Open() {
	IO_LOCK_GUARD guard(&this->m_ioLock);

	if (this->TransportLost()) {
		::SetLastError(ERROR_DEVICE_NOT_CONNECTED);
		return false;
	}

	if (this->m_ecMutex == NULL) {
		this->m_ecMutex = ::CreateMutexW(NULL, FALSE, kEcMutexName);
		if (this->m_ecMutex == NULL) {
			DWORD error = ::GetLastError();
			PawnIoLogError("CreateMutexW(Global\\Access_EC); continuing unguarded", error);
		}
	}

	if (this->m_device != INVALID_HANDLE_VALUE)
		return true;

	if (!this->OpenDeviceAndInitialize(true)) {
		DWORD error = ::GetLastError();
		// Public Open has all-or-nothing ownership semantics. In particular, a
		// failed explicit reopen must not retain a mutex from an older session.
		this->CloseDevice();
		this->CloseEcMutex();
		::SetLastError(error);
		return false;
	}

	return true;
}

void
PortIoPawn::CloseDevice() {
	HANDLE device = this->m_device;
	this->m_device = INVALID_HANDLE_VALUE;
	this->m_driverVersion = 0;
	if (device != INVALID_HANDLE_VALUE)
		::CloseHandle(device);
}

void
PortIoPawn::CloseEcMutex() {
	if (this->m_ecMutex != NULL) {
		::CloseHandle(this->m_ecMutex);
		this->m_ecMutex = NULL;
	}
}

void
PortIoPawn::Close() {
	IO_LOCK_GUARD guard(&this->m_ioLock);
	// The application stops its EC worker before final Close(). In-operation
	// recovery intentionally calls CloseDevice() only, preserving any active
	// Global\Access_EC transaction until its matching EndEcTransaction().
	this->CloseDevice();
	this->CloseEcMutex();
}

bool
PortIoPawn::IsOpen() const {
	IO_LOCK_GUARD guard(&this->m_ioLock);
	return this->m_device != INVALID_HANDLE_VALUE;
}

bool
PortIoPawn::PortAllowed(USHORT port) const {
	return port == 0x62 || port == 0x66;
}

bool
PortIoPawn::ExecuteOnce(const char* functionName,
	const ULONG64* input, ULONG inputCount,
	ULONG64* output, ULONG outputCount, DWORD* error,
	bool* localFailure) {
	if (error == NULL || localFailure == NULL)
		return false;
	*error = ERROR_INVALID_PARAMETER;
	*localFailure = true;

	if (this->m_device == INVALID_HANDLE_VALUE || functionName == NULL) {
		*error = this->m_device == INVALID_HANDLE_VALUE
			? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER;
		return false;
	}

	SIZE_T nameLength = ::strlen(functionName);
	if (nameLength < 6 || nameLength >= kExecuteNameBytes ||
		::memcmp(functionName, "ioctl_", 6) != 0 ||
		inputCount > kMaxExecuteInputCells ||
		outputCount > kMaxExecuteOutputCells ||
		(inputCount != 0 && input == NULL) ||
		(outputCount != 0 && output == NULL) ||
		inputCount > (MAXDWORD - kExecuteNameBytes) / sizeof(ULONG64) ||
		outputCount > MAXDWORD / sizeof(ULONG64)) {
		return false;
	}

	SIZE_T inputBytes = kExecuteNameBytes +
		(SIZE_T)inputCount * sizeof(ULONG64);
	SIZE_T outputBytes = (SIZE_T)outputCount * sizeof(ULONG64);
	if (inputBytes > sizeof(PIO_EXECUTE_INPUT) ||
		inputBytes > MAXDWORD || outputBytes > MAXDWORD)
		return false;

	PIO_EXECUTE_INPUT request;
	::ZeroMemory(&request, sizeof(request));
	::CopyMemory(request.Name, functionName, nameLength);
	for (ULONG i = 0; i < inputCount; ++i)
		::CopyMemory(&request.Args[i], &input[i], sizeof(ULONG64));

	*localFailure = false;
	DWORD bytesReturned = 0;
	BOOL executed = ::DeviceIoControl(this->m_device, kIoctlPioExecuteFn,
		&request, (DWORD)inputBytes,
		outputCount == 0 ? NULL : output, (DWORD)outputBytes,
		&bytesReturned, NULL);
	if (!executed) {
		*error = ::GetLastError();
		return false;
	}

	// PawnIO reports the requested output size, not a produced-cell count.
	// Successful DeviceIoControl is the only validity signal for output[].
	*error = ERROR_SUCCESS;
	return true;
}

void
PortIoPawn::LatchTransportLost(const char* reason, DWORD error) {
	if (::InterlockedExchange(&this->m_transportLost, 1) == 0) {
		char message[256];
		::sprintf_s(message, sizeof(message),
			"PortIoPawn: transport lost after %s (Win32 error %lu).\r\n",
			reason, (unsigned long)error);
		PawnIoLog(message);
	}
	::SetLastError(error);
}

bool
PortIoPawn::ExecuteWithRecovery(const char* functionName,
	const ULONG64* input, ULONG inputCount,
	ULONG64* output, ULONG outputCount) {
	if (this->TransportLost())
		return false;

	DWORD error = ERROR_SUCCESS;
	bool localFailure = false;
	if (this->ExecuteOnce(functionName, input, inputCount,
			output, outputCount, &error, &localFailure))
		return true;
	if (localFailure) {
		::SetLastError(error);
		return false;
	}

	if (error == ERROR_ACCESS_DENIED) {
		PawnIoLog("PortIoPawn: PawnIO module rejected a port operation.\r\n");
		::SetLastError(error);
		return false;
	}

	PawnIoLogError("IOCTL_PIO_EXECUTE_FN; attempting one reopen", error);

	// Keep Global\Access_EC open and owned across recovery. A port operation is
	// commonly inside a logical EC transaction, and closing that mutex here
	// would silently drop the caller's cross-process exclusion.
	this->CloseDevice();
	if (!this->OpenDeviceAndInitialize(false)) {
		DWORD reopenError = ::GetLastError();
		this->LatchTransportLost("the bounded PawnIO reopen", reopenError);
		return false;
	}

	if (this->ExecuteOnce(functionName, input, inputCount,
			output, outputCount, &error, &localFailure))
		return true;
	if (localFailure) {
		::SetLastError(error);
		return false;
	}

	if (error == ERROR_ACCESS_DENIED) {
		PawnIoLog("PortIoPawn: PawnIO module rejected the retried port operation.\r\n");
		::SetLastError(error);
		return false;
	}

	this->CloseDevice();
	this->LatchTransportLost("the single retried port operation", error);
	return false;
}

bool
PortIoPawn::ReadPort8(USHORT port, UCHAR* pdata) {
	if (pdata == NULL || !this->PortAllowed(port) || this->TransportLost())
		return false;

	IO_LOCK_GUARD guard(&this->m_ioLock);
	if (this->TransportLost() || this->m_device == INVALID_HANDLE_VALUE)
		return false;

	ULONG64 input[1];
	ULONG64 output[1];
	input[0] = (ULONG64)port;
	output[0] = 0;

	if (!this->ExecuteWithRecovery("ioctl_pio_read",
			input, 1, output, 1))
		return false;

	*pdata = (UCHAR)output[0];
	return true;
}

bool
PortIoPawn::WritePort8(USHORT port, UCHAR data) {
	if (!this->PortAllowed(port) || this->TransportLost())
		return false;

	IO_LOCK_GUARD guard(&this->m_ioLock);
	if (this->TransportLost() || this->m_device == INVALID_HANDLE_VALUE)
		return false;

	ULONG64 input[2];
	input[0] = (ULONG64)port;
	input[1] = (ULONG64)data;
	return this->ExecuteWithRecovery("ioctl_pio_write",
		input, 2, NULL, 0);
}

bool
PortIoPawn::BeginEcTransaction() {
	if (this->m_ecMutex == NULL)
		return true;

	DWORD waitResult = ::WaitForSingleObject(this->m_ecMutex, kEcMutexWaitMs);
	if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
		return true;
	if (waitResult == WAIT_FAILED)
		PawnIoLogError("WaitForSingleObject(Global\\Access_EC)", ::GetLastError());
	return false;
}

void
PortIoPawn::EndEcTransaction() {
	if (this->m_ecMutex == NULL)
		return;
	::ReleaseMutex(this->m_ecMutex);
}

bool
PortIoPawn::TransportLost() const {
	return ::InterlockedCompareExchange(&this->m_transportLost, 0, 0) != 0;
}

const char*
PortIoPawn::Name() const {
	return "PawnIO (LpcACPIEC)";
}

IPortIo* g_PortIo = NULL;

IPortIo*
CreatePawnIoTransport() {
	return new (std::nothrow) PortIoPawn;
}
