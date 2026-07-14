// --------------------------------------------------------------
//
//  Thinkpad Fan Control
//
// --------------------------------------------------------------
//
//	This program and source code is in the public domain.
//
//	The author claims no copyright, copyleft, license or
//	whatsoever for the program itself (with exception of
//	WinIO driver).  You may use, reuse or distribute it's 
//	binaries or source code in any desired way or form,  
//	Useage of binaries or source shall be entirely and 
//	without exception at your own risk. 
// 
// --------------------------------------------------------------

#include "_prec.h"

#include "fancontrol.h"
#include "winstuff.h"
#include "DynamicIcon.h"

extern HINSTANCE hInstApp, hInstRes;

//////////////////////////////////////////////////////////////////////////////
//                                                                          // 
//   MUTEX SEMAPHORES                                                       // 
//                                                                          // 
//                                                                          // 
//                                                                          // 
//                                                                          // 
//////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
//  Create/Delete/Lock/Unlock mutually exclusive semaphores
//-------------------------------------------------------------------------

MUTEXSEM::MUTEXSEM(int state, const char* name) {
	this->hmux = ::CreateMutex(NULL, (state ? TRUE : FALSE), name);
}

MUTEXSEM::~MUTEXSEM() {
	int ok = FALSE;

	HANDLE h = this->hmux;
	this->hmux = NULL;
	ok = ::CloseHandle(h);
}

int
MUTEXSEM::Lock(int millies) {
	int ok = FALSE;

	int rc = this->hmux ? ::WaitForSingleObject(this->hmux, millies) : WAIT_FAILED;
	// WAIT_ABANDONED ALSO grants ownership (a previous owner died without releasing).
	// Report it as acquired so the caller still balances it with Unlock(); returning
	// failure here would drop the release, leave the recursion count unbalanced, and
	// eventually deadlock EC access after any crash mid-transaction.
	ok = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);  // returns posted TRUE/FALSE

	return ok;
}

void
MUTEXSEM::Unlock() {
	int ok = ::ReleaseMutex(this->hmux);
}



//////////////////////////////////////////////////////////////////////////////
//                                                                          // 
//   TASKBARICON                                                            // 
//                                                                          // 
//                                                                          // 
//                                                                          // 
//                                                                          // 
//////////////////////////////////////////////////////////////////////////////

// (the hand-rolled NOTIFYICONDATAV5/V6 and OSVERSIONINFOV4 layouts are gone:
// at _WIN32_WINNT 0x0600 the SDK's NOTIFYICONDATA is already the full struct)

//-------------------------------------------------------------------------
//  Represent a window in the taskbar
//-------------------------------------------------------------------------
TASKBARICON::TASKBARICON(HWND hwndowner, int idicon, const char* tooltip)
	: Owner(hwndowner),
	Id(idicon),
	IconId(idicon),
	osVersion(0) {
	// ampersand must be escaped
	strcpy_s(this->Tooltip, sizeof(Tooltip), tooltip ? tooltip : "");

	this->Construct();
}

TASKBARICON::~TASKBARICON() {
	this->Destroy();
}


BOOL
TASKBARICON::Construct() {
	// at _WIN32_WINNT 0x0600 NOTIFYICONDATA is already the Vista-size struct,
	// so the old oversized-NOTIFYICONDATAV5-cbSize trick is no longer needed
	NOTIFYICONDATA nof = NULLSTRUCT;

	this->osVersion = 0;
	this->m_trayV4 = FALSE;

	nof.cbSize = sizeof(nof);
	nof.hWnd = this->Owner;
	nof.uID = this->Id;
	// NIF_SHOWTIP keeps the standard (rich multi-line) tooltip working once
	// the icon runs the VERSION_4 protocol
	nof.uFlags = NIF_MESSAGE | NIF_SHOWTIP;
	nof.uCallbackMessage = WM__TASKBAR;

	if (this->IconId) {
		// LoadIconMetric picks the best-fitting frame and scales cleanly on
		// HiDPI taskbars; keep the classic LoadImage as the fallback
		HICON h = NULL;
		if (FAILED(::LoadIconMetric(hInstRes, MAKEINTRESOURCEW(this->IconId), LIM_SMALL, &h)) || !h)
			h = (HICON)::LoadImage(hInstRes, MAKEINTRESOURCE(this->IconId), IMAGE_ICON,
				::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
		nof.hIcon = h;
		if (nof.hIcon) nof.uFlags |= NIF_ICON;
	}

	if (strlen(this->Tooltip)) {
		lstrcpyn(nof.szTip, this->Tooltip, sizeof(nof.szTip) - 1);
		nof.uFlags |= NIF_TIP;
	}

	this->UpAndRunning = ::Shell_NotifyIcon(NIM_ADD, &nof);

	if (this->UpAndRunning) {
		this->osVersion = 5;
		// modern callback protocol: keyboard activation (NIN_KEYSELECT) and a
		// WM_CONTEXTMENU anchored at the icon; when the shell refuses, the
		// flag stays FALSE and the legacy mouse-message protocol is used
		nof.uVersion = NOTIFYICON_VERSION_4;
		this->m_trayV4 = ::Shell_NotifyIcon(NIM_SETVERSION, &nof) ? TRUE : FALSE;
	}

	if (nof.hIcon) {
		::DestroyIcon(nof.hIcon);
		nof.hIcon = NULL;
	}

	return this->UpAndRunning;
}

void
TASKBARICON::Destroy(BOOL keep) {
	NOTIFYICONDATA nof = NULLSTRUCT;

	nof.cbSize = sizeof(nof);
	nof.hWnd = this->Owner;
	nof.uID = this->Id;
	::Shell_NotifyIcon(NIM_DELETE, &nof);

	if (!keep) {
		this->Owner = 0;
		this->Id = 0;
		this->IconId = 0;
		strcpy_s(this->Tooltip, sizeof(Tooltip), "");
	}
}

BOOL
TASKBARICON::IsUpAndRunning() {
	return this->UpAndRunning;
}

BOOL
TASKBARICON::HasExtendedFeatures(void) {
	return this->osVersion >= 5;  //maybee we want to implement version 6 from up vista
}


BOOL
TASKBARICON::RebuildIfNecessary(BOOL force) {
	char tt[256];

	strcpy_s(tt, sizeof(tt), this->Tooltip); // avoid selfassignment

	if (force || !this->SetTooltip(tt)) {
		this->Destroy(TRUE);
		this->Construct();
	}

	return this->SetTooltip(tt);
}


int
TASKBARICON::SetIcon(int iconid) {
	BOOL ok;
	NOTIFYICONDATA nof = NULLSTRUCT;

	this->IconId = iconid;

	nof.cbSize = sizeof(nof);
	nof.hWnd = this->Owner;
	nof.uID = this->Id;
	nof.uFlags = NIF_ICON | NIF_SHOWTIP;   // SHOWTIP must persist across NIM_MODIFY
	if (FAILED(::LoadIconMetric(hInstRes, MAKEINTRESOURCEW(this->IconId), LIM_SMALL, &nof.hIcon)) || !nof.hIcon)
		nof.hIcon = (HICON)
			::LoadImage(hInstRes, MAKEINTRESOURCE(this->IconId), IMAGE_ICON,
				::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

	ok = ::Shell_NotifyIcon(NIM_MODIFY, &nof);

	if (nof.hIcon) {
		::DestroyIcon(nof.hIcon);
		nof.hIcon = NULL;
	}

	// try to rebuild if SetIcon failed
	if (!ok)
		this->RebuildIfNecessary(TRUE);

	return ok;
}

int
TASKBARICON::GetIcon(void) {
	return this->IconId;
}

int
TASKBARICON::SetTooltip(const char* tooltip) {
	BOOL ok = 0;


	if (strcmp(this->Tooltip, tooltip) != 0) {
		strcpy_s(this->Tooltip, sizeof(Tooltip), tooltip);


		NOTIFYICONDATA nof = NULLSTRUCT;

		nof.cbSize = sizeof(nof);
		nof.hWnd = this->Owner;
		nof.uID = this->Id;
		nof.uFlags = NIF_TIP | NIF_SHOWTIP;   // SHOWTIP must persist across NIM_MODIFY
		lstrcpyn(nof.szTip, this->Tooltip, sizeof(nof.szTip) - 1);


		ok = ::Shell_NotifyIcon(NIM_MODIFY, &nof);

		// try to rebuild if SetTooltip failed
		if (!ok && !this->InsideTooltipRebuild) {
			this->InsideTooltipRebuild = TRUE;
			this->RebuildIfNecessary(TRUE);
			this->InsideTooltipRebuild = FALSE;
		}
	}
	return ok;
}


int
TASKBARICON::SetBalloon(ULONG flags, const char* title, const char* text, int timeout) {
	BOOL ok;

	NOTIFYICONDATA nof = NULLSTRUCT;

	nof.cbSize = sizeof(NOTIFYICONDATA);
	nof.hWnd = this->Owner;
	nof.uID = this->Id;
	nof.uFlags = NIF_INFO | NIF_SHOWTIP;
	nof.dwInfoFlags = flags;
	nof.uTimeout = timeout;
	lstrcpyn(nof.szInfo, text, sizeof(nof.szInfo) - 1);
	lstrcpyn(nof.szInfoTitle, title, sizeof(nof.szInfoTitle) - 1);

	// brand the toast with the live severity icon at toast size instead of the
	// generic blue (i); fall back to the caller's plain flags when the load
	// fails. The quiet-time bit is free politeness on Win7+.
	HICON hBig = NULL;
	if (this->IconId &&
			SUCCEEDED(::LoadIconMetric(hInstRes, MAKEINTRESOURCEW(this->IconId),
				LIM_LARGE, &hBig)) && hBig) {
		nof.hBalloonIcon = hBig;
		nof.dwInfoFlags = (flags & ~NIIF_ICON_MASK) | NIIF_USER | NIIF_LARGE_ICON;
	}
	nof.dwInfoFlags |= NIIF_RESPECT_QUIET_TIME;

	ok = Shell_NotifyIcon(NIM_MODIFY, &nof);

	if (hBig)
		::DestroyIcon(hBig);   // safe: the shell copies the balloon icon

	if (!ok)
		this->RebuildIfNecessary(TRUE);

	return ok;
}







//////////////////////////////////////////////////////////////////////////////
//                                                                          // 
//   MENU                                                                   // 
//                                                                          // 
//                                                                          // 
//                                                                          // 
//                                                                          // 
//////////////////////////////////////////////////////////////////////////////

MENU::MENU(HWND w)
	: hMenu(::GetMenu(w)),
	hWndOwner(w),
	IsLoaded(FALSE) {

}

MENU::MENU(HMENU hmenu)
	: hMenu(hmenu),
	hWndOwner(NULL),
	IsLoaded(FALSE) {

}

MENU::MENU(int id, HINSTANCE hdll)
	: hMenu(::LoadMenu(((ULONG)hdll == (ULONG)-1) ? hInstRes : hdll, MAKEINTRESOURCE(id))),
	hWndOwner(NULL),
	IsLoaded(TRUE) {

}


void
MENU::EnableMenuItem(int id, int status) {
	::EnableMenuItem(*this, id,
		MF_BYCOMMAND | (status ? MF_ENABLED : MF_GRAYED));
}

void
MENU::DisableMenuItem(int id) {
	this->EnableMenuItem(id, FALSE);
}

int
MENU::DeleteMenuItem(int id, BOOL idispos) {
	int rc = ::DeleteMenu(*this, id, idispos ? MF_BYPOSITION : MF_BYCOMMAND);
	if (this->hWndOwner)
		::DrawMenuBar(this->hWndOwner);

	return rc;
}

void
MENU::CheckMenuItem(int id, int status) {
	::CheckMenuItem(*this, id,
		MF_BYCOMMAND | (status ? MF_CHECKED : MF_UNCHECKED));
}

void
MENU::UncheckMenuItem(int id) {
	this->CheckMenuItem(id, FALSE);
}

void
MENU::SetDefaultItem(int id) {
	::SetMenuDefaultItem(*this, id, FALSE);   // FALSE = by command id
}

BOOL
MENU::IsFlags(int id, int flags) {
	return ((::GetMenuState(*this, id, MF_BYCOMMAND) & flags) != 0);
}

BOOL
MENU::IsMenuItemSeparator(int pos) {
	MENUITEMINFO mii = { sizeof(mii), MIIM_TYPE, };
	::GetMenuItemInfo(*this, pos, TRUE, &mii);
	return mii.fType == MFT_SEPARATOR;
}

BOOL
MENU::IsMenuItemEnabled(int id) {
	return !this->IsFlags(id, MF_DISABLED | MF_GRAYED);
}

BOOL
MENU::IsMenuItemDisabled(int id) {
	return this->IsFlags(id, MF_DISABLED | MF_GRAYED);
}

BOOL
MENU::IsMenuItemChecked(int id) {
	return this->IsFlags(id, MF_CHECKED);
}

int
MENU::GetNumMenuItems() {
	return ::GetMenuItemCount(*this);
}


//--------------------------------------------------------------------
//  return the sub-menu handle of a menu item at a given position
//--------------------------------------------------------------------
HMENU
MENU::GetSubmenuFromPos(int pos) {
	HMENU rc = NULL;

	rc = (HMENU)GetSubMenu(*this, pos);

	return rc;
}


//--------------------------------------------------------------------
//  return the item pos of a menu entry (search by id)
//--------------------------------------------------------------------
int
MENU::GetMenuPosFromID(int id) {
	int rc = -1;

	int i, mid, numof = ::GetMenuItemCount(*this);

	for (i = 0; i < numof; i++) {
		mid = ::GetMenuItemID(*this, i);
		if (mid == id) {
			rc = i;
			break;
		}
	}

	return rc;
}


//-------------------------------------------------------------------------
//  
//-------------------------------------------------------------------------
BOOL
MENU::InsertItem(const char* text, int id, int pos) {
	MENUITEMINFO mi = NULLSTRUCT;
	mi.cbSize = sizeof(mi);
	mi.fMask = MIIM_TYPE | MIIM_ID;
	mi.wID = id;

	if (!text) {
		mi.fType = MFT_SEPARATOR;
	}
	else {
		mi.fMask |= MIIM_DATA;
		mi.fType = MFT_STRING;
		mi.dwTypeData = (char*)text;
	}

	return ::InsertMenuItem(*this, pos, TRUE, &mi);
}


//-------------------------------------------------------------------------
//  
//-------------------------------------------------------------------------
int
MENU::Popup(HWND hwndowner, POINT* ppoint, BOOL synchtrack) {
	POINT point;
	HMENU hmenu, hmenuShow;

	if (ppoint)
		point = *ppoint;
	else
		::GetCursorPos(&point);

	hmenu = CreateMenu();
	::AppendMenu(hmenu, MF_POPUP | MF_STRING, (UINT)
		this->hMenu, "BLUB");
	hmenuShow = ::GetSubMenu(hmenu, 0);
	RECT r = { 0, 0, 10, 10 };

	if (hwndowner)
		::SetForegroundWindow(hwndowner);


	ULONG flags = TPM_LEFTALIGN | TPM_LEFTBUTTON;

	if (synchtrack & 1)
		flags |= TPM_RETURNCMD;

	if (synchtrack & TPM_RIGHTALIGN)
		flags |= TPM_RIGHTALIGN;

	int rc = ::TrackPopupMenu(hmenuShow, flags,
		point.x, point.y, 0,
		hwndowner, &r);

	if (hwndowner)
		::PostMessage(hwndowner, WM_NULL, 0, 0);

	::RemoveMenu(hmenu, 0, MF_BYPOSITION);
	::DestroyMenu(hmenu);

	return rc;
}
