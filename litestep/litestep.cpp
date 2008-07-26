//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// This is a part of the Litestep Shell source code.
//
// Copyright (C) 1997-2007  Litestep Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//
//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
#include "litestep.h"
#include <WtsApi32.h>

// Misc Helpers
#include "RecoveryMenu.h"
#include "StartupRunner.h"
#include "../lsapi/lsapiInit.h"
#include "../lsapi/ThreadedBangCommand.h"
#include "../utility/macros.h"
#include "../utility/shellhlp.h"

// Services
#include "DDEService.h"
#include "DDEStub.h"
#include "TrayService.h"

// Managers
#include "MessageManager.h"
#include "ModuleManager.h"

// Misc Helpers
#include "DataStore.h"

// STL headers
#include <algorithm>

#include "../utility/core.hpp"


// namespace stuff
using std::for_each;
using std::mem_fun;


// Globals
CLiteStep gLiteStep;


//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// GetAppPath
//
static HRESULT GetAppPath(LPTSTR pszAppPath, DWORD cchAppPath)
{
    HRESULT hr = E_FAIL;

#ifdef _DEBUG
    typedef BOOL (WINAPI* IsDebuggerPresentProc)();

    IsDebuggerPresentProc fnIsDebuggerPresent = (IsDebuggerPresentProc)
        GetProcAddress(GetModuleHandle(_T("kernel32")), "IsDebuggerPresent");

    // If a debugger is attached use the current directory as base path
    if (fnIsDebuggerPresent && fnIsDebuggerPresent())
    {
        if (GetCurrentDirectory(cchAppPath, pszAppPath))
        {
            hr = S_OK;
        }
    }
    else
#endif
    // Otherwise use litestep.exe's location as base path
    if (LSGetModuleFileName(NULL, pszAppPath, cchAppPath))
    {
        PathRemoveFileSpec(pszAppPath);
        hr = S_OK;
    }

    return hr;
}


//
//
//
int StartLitestep(HINSTANCE hInst, WORD wStartFlags, LPCTSTR pszAltConfigFile)
{
    TCHAR szAppPath[MAX_PATH] = { 0 };
    TCHAR szRcPath[MAX_PATH] = { 0 };

    if (FAILED(GetAppPath(szAppPath, COUNTOF(szAppPath))))
    {
        // something really crappy is going on. 
        return -1; 
    }

    if (wStartFlags & LSF_ALTERNATE_CONFIG)
    {
        StringCchCopy(szRcPath, COUNTOF(szRcPath), pszAltConfigFile);
    }
    else
    {
        PathCombine(szRcPath, szAppPath, "step.rc");
    }

    // Tell the Welcome Screen to close
    // This has to be done before the first MessageBox call in shell mode,
    // else that box would pop up "under" the welcome screen
    HANDLE hShellReadyEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE,
        "msgina: ShellReadyEvent");

    if (hShellReadyEvent != NULL)
    {
        SetEvent(hShellReadyEvent);
        CloseHandle(hShellReadyEvent);
    }

    // If we can't find "step.rc", there's no point in proceeding
    if (!PathFileExists(szRcPath))
    {
        RESOURCE_STREX(
            hInst, IDS_LITESTEP_ERROR2, resourceTextBuffer, MAX_LINE_LENGTH,
            "Unable to find the file \"%s\".\n"
            "Please verify the location of the file, and try again.", szRcPath);

        MessageBox(NULL, resourceTextBuffer, "LiteStep",
            MB_TOPMOST | MB_ICONEXCLAMATION);

        return 2;
    }

    // Initialize the LSAPI.  Note: The LSAPI controls the bang and settings managers 
    // so they will be started at this point. 
    if (!LSAPIInitialize(szAppPath, szRcPath)) 
    { 
        //TODO: Localize this. 
        MessageBox(NULL, "Failed to initialize the LiteStep API.",
            "LiteStep", MB_TOPMOST | MB_ICONEXCLAMATION);

        return 3;
    }

    // All child processes get this variable
    VERIFY(SetEnvironmentVariable(_T("LitestepDir"), szAppPath));

    HRESULT hr = gLiteStep.Start(hInst, wStartFlags);

    return HRESULT_CODE(hr);
}


//
// CLiteStep()
//
CLiteStep::CLiteStep()
: m_pRegisterShellHook(NULL),
  m_hWtsDll(NULL)
{
    m_hInstance = NULL;
    m_bAutoHideModules = false;
    m_bAppIsFullScreen = false;
    m_hMainWindow = NULL;
    WM_ShellHook = 0;
    m_pModuleManager = NULL;
    m_pDataStoreManager = NULL;
    m_pMessageManager = NULL;
    m_bSignalExit = false;
    m_pTrayService = NULL;
    m_BlockRecycle = 0;
}


//
//
//
CLiteStep::~CLiteStep()
{
}


//
// Start
//
HRESULT CLiteStep::Start(HINSTANCE hInstance, WORD wStartFlags)
{
    HRESULT hr;
    bool bUnderExplorer = false;
    
    m_hInstance = hInstance;
    
    // Initialize OLE/COM
    OleInitialize(NULL);
    
    // before anything else, start the recovery menu thread
    DWORD dwRecoveryThreadID;
    HANDLE hRecoveryThread = CreateThread(NULL, 0, RecoveryThreadProc,
        (LPVOID)m_hInstance, 0, &dwRecoveryThreadID);

    // Order of precedence: 1) shift key, 2) command line flags, 3) step.rc
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || 
        GetRCBool("LSNoStartup", TRUE) &&
        !(wStartFlags & LSF_FORCE_STARTUPAPPS))

    {
        wStartFlags &= ~LSF_RUN_STARTUPAPPS;
    }

    m_bAutoHideModules = GetRCBool("LSAutoHideModules", TRUE) ? true : false;
    
    // Check for explorer
    if (FindWindow("Shell_TrayWnd", NULL)) // Running under Exploder
    {
        if (GetRCBool("LSNoShellWarning", FALSE))
        {
            RESOURCE_STR(hInstance, IDS_LITESTEP_ERROR3,
                "Litestep is not able to load as the system shell.\n"
                "Another shell is already active.\n"
                "\n"
                "Continuing to load Litestep will disable specific system\n"
                "shell functions of Litestep and some features will not\n"
                "function properly such as icon notifications (systray),\n"
                "the desktop and some task managers.\n"
                "\n"
                "To disable this message, place 'LSNoShellWarning' in\n"
                "your step.rc.\n"
                "\n"
                "Continue to load Litestep?\n");
            RESOURCE_TITLE(hInstance, IDS_LITESTEP_TITLE_WARNING, "Warning");
            if (MessageBox(NULL, resourceTextBuffer, resourceTitleBuffer, MB_YESNO | MB_ICONEXCLAMATION | MB_TOPMOST) == IDNO)
            {
                return E_ABORT;
            }
        }
        bUnderExplorer = true;
    }

    // Register Window Class
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = CLiteStep::ExternalWndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = szMainWindowClass;
    
    if (!RegisterClassEx(&wc))
    {
        RESOURCE_MSGBOX_T(hInstance, IDS_LITESTEP_ERROR4,
                          "Error registering main Litestep window class.",
                          IDS_LITESTEP_TITLE_ERROR, "Error");
        
        return E_FAIL;
    }
    
    // Create our main window
    m_hMainWindow = CreateWindowEx(WS_EX_TOOLWINDOW,
        szMainWindowClass, szMainWindowTitle,
        0, 0, 0, 0,
        0, NULL, NULL,
        m_hInstance,
        (void*)this);
    
    // Start up everything
    if (m_hMainWindow)
    {
        // Set magic DWORD to prevent VWM from seeing main window
        SetWindowLongPtr(m_hMainWindow, GWLP_USERDATA, magicDWord);

        // Set our window in LSAPI 
        LSAPISetLitestepWindow(m_hMainWindow); 

        _RegisterShellNotifications(m_hMainWindow);

        // Set Shell Window
        if (!bUnderExplorer && (GetRCBool("LSSetAsShell", TRUE)))
        {
            typedef BOOL (WINAPI* SETSHELLWINDOWPROC)(HWND);

            SETSHELLWINDOWPROC fnSetShellWindow =
                (SETSHELLWINDOWPROC)GetProcAddress(
                    GetModuleHandle(_T("USER32.DLL")), "SetShellWindow");

            if (fnSetShellWindow)
            {
                fnSetShellWindow(m_hMainWindow);
            }
        }
        
        hr = _InitServices();
        if (SUCCEEDED(hr))
        {
            hr = _StartServices();
            // Quietly swallow service errors... in the future.. do something
        }
        
        hr = _InitManagers();
        if (SUCCEEDED(hr))
        {
            hr = _StartManagers();
            // Quietly swallow manager errors... in the future.. do something
        }
        
        // Run startup items
        if (wStartFlags & LSF_RUN_STARTUPAPPS)
        {
            DWORD dwThread;
            BOOL bForceStartup = (wStartFlags & LSF_FORCE_STARTUPAPPS);

            CloseHandle(CreateThread(NULL, 0, StartupRunner::Run,
                (LPVOID)(INT_PTR)bForceStartup, 0, &dwThread));
        }

        // On Vista, the shell is responsible for playing the startup sound
        if (IsVistaOrAbove() && StartupRunner::IsFirstRunThisSession(
            _T("LogonSoundHasBeenPlayed")))
        {
            LSPlaySystemSound(_T("WindowsLogon"));
        }

        // Undocumented call: Shell Loading Finished
        SendMessage(GetDesktopWindow(), WM_USER, 0, 0);
        
        // Main message pump
        MSG message;
        /* Note: check m_bSignalExit first, so that if MessageHandler()
         * was called externally from a response to PeekMessage() we
         * know right away if there was a WM_QUIT in the queue, and
         * subsequently do not incorrectly call GetMessage() again. */
        while (!m_bSignalExit && GetMessage(&message, 0, 0, 0) > 0)
        {
            MessageHandler(message);
        }
        
        _UnregisterShellNotifications(m_hMainWindow);

        _StopManagers();
        _CleanupManagers();
        
        _StopServices();
        _CleanupServices();
        
        // Destroy main window
        DestroyWindow(m_hMainWindow);
        m_hMainWindow = NULL;
        LSAPISetLitestepWindow(NULL);
    }
    else
    {
        RESOURCE_MSGBOX_T(hInstance, IDS_LITESTEP_ERROR5,
                          "Error creating Litestep main application window.",
                          IDS_LITESTEP_TITLE_ERROR, "Error");
    }
    
    // Unreg class
    UnregisterClass(szMainWindowClass, m_hInstance);
    
    // Uninitialize OLE/COM
    OleUninitialize();
    
    // close the recovery thread: tell the thread to quit
    PostThreadMessage(dwRecoveryThreadID, WM_QUIT, 0, 0);
    // wait until the thread is done quitting, at most three seconds though
    if (WaitForSingleObject(hRecoveryThread, 3000) == WAIT_TIMEOUT)
    {
        TerminateThread(hRecoveryThread, 0);
    }
    // close the thread handle
    CloseHandle(hRecoveryThread);
    
    return S_OK;
}

//
//
//
int CLiteStep::MessageHandler(MSG &message)
{
    if(WM_QUIT == message.message)
    {
        m_bSignalExit = true;
        return 0;
    }
    
#if !defined(LS_NO_EXCEPTION)
    try
    {
#endif /* LS_NO_EXCEPTION */
        if (NULL == message.hwnd)
        {
            // Thread message
            switch (message.message)
            {
                case LM_THREAD_BANGCOMMAND:
                {
                    ThreadedBangCommand* pInfo = \
                        (ThreadedBangCommand*)message.wParam;
                    
                    if (NULL != pInfo)
                    {
                        pInfo->Execute();
                        pInfo->Release(); // check BangCommand.cpp for the reason
                    }
                }
                break;
                
                default:
                break;
            }
        }
        else
        {
            TranslateMessage(&message);
            DispatchMessage (&message);
        }
#if !defined(LS_NO_EXCEPTION)
    }
    catch(...)
    {
        // MessageBox(m_hMainWindow, "exception", "oops", MB_OK | MB_TOPMOST | MB_ICONEXCLAMATION);
    }
#endif /* LS_NO_EXCEPTION */
    
    return 0;
}


//
// _RegisterShellNotifications
//
void CLiteStep::_RegisterShellNotifications(HWND hWnd)
{
    //
    // Configure the Win32 window manager to hide minimized windows
    // This is necessary to enable WH_SHELL-style hooks,
    // including RegisterShellHook
    //
    MINIMIZEDMETRICS mm = { 0 };
    mm.cbSize = sizeof(MINIMIZEDMETRICS);

    VERIFY(SystemParametersInfo(SPI_GETMINIMIZEDMETRICS, mm.cbSize, &mm, 0));

    if (!(mm.iArrange & ARW_HIDE))
    {
        mm.iArrange |= ARW_HIDE;
        VERIFY(SystemParametersInfo(
            SPI_SETMINIMIZEDMETRICS, mm.cbSize, &mm, 0));
    }

    //
    // Register for shell hook notifications
    //
    WM_ShellHook = RegisterWindowMessage("SHELLHOOK");

    m_pRegisterShellHook = (RSHPROC)GetProcAddress(
        GetModuleHandle(_T("SHELL32.DLL")), (LPCSTR)((long)0x00B5));

    if (m_pRegisterShellHook)
    {
        m_pRegisterShellHook(NULL, RSH_REGISTER);

        if (IsOS(OS_WINDOWS))
        {
            // c0atzin's fix for 9x
            m_pRegisterShellHook(hWnd, RSH_REGISTER);
        }
        else
        {
            m_pRegisterShellHook(hWnd, RSH_TASKMAN);
        }
    }

    //
    // Register for session change notifications
    //
    if (IsOS(OS_XPORGREATER))
    {
        ASSERT(m_hWtsDll == NULL);
        m_hWtsDll = LoadLibrary(_T("WtsApi32.dll"));

        if (m_hWtsDll)
        {
            typedef BOOL (WINAPI* WTSRSNPROC)(HWND, DWORD);

            WTSRSNPROC pWTSRegisterSessionNotification = (WTSRSNPROC)
                GetProcAddress(m_hWtsDll, "WTSRegisterSessionNotification");

            if (pWTSRegisterSessionNotification)
            {
                // This needs to be fixed: We should wait for
                // Global\TermSrvReadyEvent before calling this.
                VERIFY(pWTSRegisterSessionNotification(
                    hWnd, NOTIFY_FOR_THIS_SESSION));
            }
        }
        else
        {
            TRACE("Failed to load WtsApi32.dll");
        }
    }
}


//
// _UnregisterShellNotifications
//
void CLiteStep::_UnregisterShellNotifications(HWND hWnd)
{
    if (m_hWtsDll)
    {
        typedef BOOL (WINAPI* WTSURSNPROC)(HWND);

        WTSURSNPROC pWTSUnRegisterSessionNotification = (WTSURSNPROC)
            GetProcAddress(m_hWtsDll, "WTSUnRegisterSessionNotification");

        if (pWTSUnRegisterSessionNotification)
        {
            VERIFY(pWTSUnRegisterSessionNotification(hWnd));
        }

        VERIFY(FreeLibrary(m_hWtsDll));
        m_hWtsDll = NULL;
    }

    if (m_pRegisterShellHook)
    {
        m_pRegisterShellHook(hWnd, RSH_UNREGISTER);
    }
}


//
//
//
LRESULT CALLBACK CLiteStep::ExternalWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static CLiteStep* pLiteStep = NULL;
    
    if (uMsg == WM_CREATE)
    {
        pLiteStep = static_cast<CLiteStep*>(
            reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
        
        ASSERT(NULL != pLiteStep);
    }
    
    if (pLiteStep)
    {
        return pLiteStep->InternalWndProc(hWnd, uMsg, wParam, lParam);
    }
    
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}


//
//
//
LRESULT CLiteStep::InternalWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT lReturn = FALSE;
    
    switch (uMsg)
    {
        case WM_KEYDOWN:
        case WM_SYSCOMMAND:
        {
            switch (wParam)
            {
                case LM_SHUTDOWN:
                case SC_CLOSE:
                {
                    ParseBangCommand(hWnd, "!ShutDown", NULL);
                }
                break;
                
                default:
                {
                    lReturn = DefWindowProc(hWnd, uMsg, wParam, lParam);
                }
                break;
            }
        }
        break;
        
        case WM_QUERYENDSESSION:
        case WM_ENDSESSION:
        {
            lReturn = TRUE;
        }
        break;
        
        case LM_SYSTRAYREADY:
        {
            if (m_pTrayService)
            {
                lReturn = (LRESULT)m_pTrayService->SendSystemTray();
            }
        }
        break;
        
        case LM_SAVEDATA:
        {
            WORD wIdent = HIWORD(wParam);
            WORD wLength = LOWORD(wParam);
            void *pvData = (void *)lParam;
            if ((pvData != NULL) && (wLength > 0))
            {
                if (m_pDataStoreManager == NULL)
                {
                    m_pDataStoreManager = new DataStore();
                }
                if (m_pDataStoreManager)
                {
                    lReturn = m_pDataStoreManager->StoreData(wIdent, pvData, wLength);
                }
            }
        }
        break;
        
        case LM_RESTOREDATA:
        {
            WORD wIdent = HIWORD(wParam);
            WORD wLength = LOWORD(wParam);
            void *pvData = (void *)lParam;
            if ((pvData != NULL) && (wLength > 0))
            {
                if (m_pDataStoreManager)
                {
                    lReturn = m_pDataStoreManager->ReleaseData(wIdent, pvData, wLength);
                    if (m_pDataStoreManager->Count() == 0)
                    {
                        delete m_pDataStoreManager;
                        m_pDataStoreManager = NULL;
                    }
                }
            }
        }
        break;
        
        case LM_GETLSOBJECT:
        case LM_WINDOWLIST:
        case LM_MESSAGEMANAGER:
        case LM_DATASTORE:
        {
            ; // Obsolete Message, return 0
        }
        break;
        
        case LM_ENUMREVIDS:
        {
            HRESULT hr = E_FAIL;
            
            if (m_pMessageManager)
            {
                hr = _EnumRevIDs((LSENUMREVIDSPROC)wParam, lParam);
            }
            
            return hr;
        }
        break;
        
        case LM_ENUMMODULES:
        {
            HRESULT hr = E_FAIL;
            
            if (m_pModuleManager)
            {
                hr = m_pModuleManager->EnumModules((LSENUMMODULESPROC)wParam,
                    lParam);
            }
            
            return hr;
        }
        break;
        
        case LM_RECYCLE:
        {
            switch (wParam)
            {
                case LR_RECYCLE:
                {
                    _Recycle();
                }
                break;
                
                case LR_LOGOFF:
                {
                    if (ExitWindowsEx(EWX_LOGOFF, 0))
                    {
                        PostQuitMessage(0);
                    }
                }
                break;
                
                case LR_QUIT:
                {
                    PostQuitMessage(0);
                }
                break;
                
                default:  // wParam == LR_MSSHUTDOWN
                {
                    LSShutdownDialog(m_hMainWindow);
                }
                break;
            }
        }
        break;
        
        case LM_RELOADMODULE:
        {
            if (m_pModuleManager)
            {
                if (lParam & LMM_HINSTANCE)
                {
                    // not sure if this feature is needed... if a module
                    // wants to reload it shouldn't need the core to do that
                    m_pModuleManager->ReloadModule((HINSTANCE)wParam);
                }
                else
                {
                    LPCSTR pszPath = (LPCSTR)wParam;
                    
                    if (IsValidStringPtr(pszPath))
                    {
                        m_pModuleManager->QuitModule(pszPath);
                        m_pModuleManager->LoadModule(pszPath, (DWORD)lParam);
                    }
                }
            }
        }
        break;
        
        case LM_UNLOADMODULE:
        {
            if (m_pModuleManager)
            {
                if (lParam & LMM_HINSTANCE)
                {
                    m_pModuleManager->QuitModule((HINSTANCE)wParam);
                }
                else
                {
                    LPCSTR pszPath = (LPCSTR)wParam;
                    
                    if (IsValidStringPtr(pszPath))
                    {
                        m_pModuleManager->QuitModule(pszPath);
                    }
                }
            }
        }
        break;
        
        case LM_BANGCOMMAND:
        {
            PLMBANGCOMMAND plmbc = (PLMBANGCOMMAND)lParam;
            
            if (IsValidReadPtr(plmbc))
            {
                if (plmbc->cbSize == sizeof(LMBANGCOMMAND))
                {
                    lReturn = ParseBangCommand(plmbc->hWnd, plmbc->szCommand, plmbc->szArgs);
                }
            }
        }
        break;
        
        case WM_COPYDATA:
        {
            PCOPYDATASTRUCT pcds = (PCOPYDATASTRUCT)lParam;
            
            switch (pcds->dwData)
            {
                case LM_BANGCOMMAND:
                {
                    lReturn = SendMessage(hWnd, LM_BANGCOMMAND, 0, (LPARAM)pcds->lpData);
                }
                break;
                
                default:
                break;
            }
        }
        break;
        
        case LM_REGISTERMESSAGE:     // Message Handler Message
        {
            if (m_pMessageManager)
            {
                m_pMessageManager->AddMessages((HWND)wParam, (UINT *)lParam);
            }
        }
        break;
        
        case LM_UNREGISTERMESSAGE:     // Message Handler Message
        {
            if (m_pMessageManager)
            {
                m_pMessageManager->RemoveMessages((HWND)wParam, (UINT *)lParam);
            }
        }
        break;

        case WM_WTSSESSION_CHANGE:
        {
            lReturn = _HandleSessionChange((DWORD)wParam, (DWORD)lParam);
        }
        break;

        default:
        {
            if (uMsg == WM_ShellHook)
            {
                WORD wHookCode  = (LOWORD(wParam) & 0x00FF);
                WORD wExtraBits = (LOWORD(wParam) & 0xFF00);

                // most shell hook messages pass an HWND as lParam
                HWND hWndMessage = (HWND)lParam;

                // Convert to an LM_SHELLHOOK message
                uMsg   = LM_SHELLHOOK + wHookCode;
                wParam = (WPARAM)hWndMessage;
                lParam = wExtraBits;

                if (uMsg == LM_WINDOWACTIVATED)
                {
                    /*
                     * Note: The ShellHook will always set the HighBit when there
                     * is any full screen app on the desktop, even if it does not
                     * have focus.  Because of this, we have no easy way to tell
                     * if the currently activated app is full screen or not.
                     * This is worked around by checking the window's actual size
                     * against the screen size.  The correct behavior for this is
                     * to hide when a full screen app is active, and to show when
                     * a non full screen app is active.
                     */

                    if (!m_bAppIsFullScreen && lParam != 0 && 
                        _IsFullScreenActiveOnPrimaryMonitor())
                    {
                        _HandleFullScreenApp(true);
                    }
                    else if (m_bAppIsFullScreen && 
                        (lParam == 0 || !_IsFullScreenActiveOnPrimaryMonitor()))
                    {
                        _HandleFullScreenApp(false);
                    }
                }
            }

            // WM_APP, LM_XYZ, and registered messages are all >= WM_USER
            if (uMsg >= WM_USER)
            {
                if (m_pMessageManager &&
                    m_pMessageManager->HandlerExists(uMsg))
                {
                    lReturn =
                        m_pMessageManager->SendMessage(uMsg, wParam, lParam);
                    
                    break;
                }
            }

            lReturn = DefWindowProc (hWnd, uMsg, wParam, lParam);
        }
        break;
    }

    return lReturn;
}


//
// _HandleSessionChange
// Handler for WM_WTSSESSION_CHANGE messages
//
LRESULT CLiteStep::_HandleSessionChange(DWORD dwCode, DWORD /* dwSession */)
{
    if (dwCode == WTS_SESSION_LOCK)
    {
        LSPlaySystemSound(_T("WindowsLogoff"));
    }
    else if (dwCode == WTS_SESSION_UNLOCK)
    {
        LSPlaySystemSound(_T("WindowsLogon"));
    }

    return 0;
}


//
// _InitServies()
//
HRESULT CLiteStep::_InitServices()
{
    IService* pService = NULL;
    
    //
    // DDE Service
    //
    if (GetRCBool("LSUseSystemDDE", TRUE))
    {
        // M$ DDE
        pService = new DDEStub();
    }
    else
    {
        // liteman
        pService = new DDEService();
    }
    
    if (pService)
    {
        m_Services.push_back(pService);
    }
    else
    {
        return E_OUTOFMEMORY;
    }
    
    //
    // Tray Service
    //
    if (GetRCBool("LSDisableTrayService", FALSE))
    {
        m_pTrayService = new TrayService();
        
        if (m_pTrayService)
        {
            m_Services.push_back(m_pTrayService);
        }
        else
        {
            return E_OUTOFMEMORY;
        }
    }
    
    return S_OK;
}


//
// _StartServices()
//
HRESULT CLiteStep::_StartServices()
{
    // use std::transform to add error checking to this
    for_each(m_Services.begin(), m_Services.end(), mem_fun(&IService::Start));
    return S_OK;
}


//
// _StopServices()
//
HRESULT CLiteStep::_StopServices()
{
    for_each(m_Services.begin(), m_Services.end(), mem_fun(&IService::Stop));
    return S_OK;
}


//
// _CleanupServices()
//
void CLiteStep::_CleanupServices()
{
    std::for_each(m_Services.begin(), m_Services.end(),
        std::mem_fun(&IService::Release));
    
    m_Services.clear();
}


//
// _InitManagers()
//
HRESULT CLiteStep::_InitManagers()
{
    HRESULT hr = S_OK;
    
    m_pMessageManager = new MessageManager();
    
    m_pModuleManager = new ModuleManager();
    
    // Note:
    // - The DataStore manager is dynamically initialized/started.
    // - The Bang and Settings managers are located in LSAPI, and
    //   are instantiated via LSAPIInit.
    
    return hr;
}


//
// _StartManagers
//
HRESULT CLiteStep::_StartManagers()
{
    HRESULT hr = S_OK;
    
    // Load modules
    m_pModuleManager->Start(this);
    
    // Note:
    // - MessageManager has/needs no Start method.
    // - The DataStore manager is dynamically initialized/started.
    
    return hr;
}


//
// _StopManagers()
//
HRESULT CLiteStep::_StopManagers()
{
    HRESULT hr = S_OK;
    
    m_pModuleManager->Stop();
    
    // Clean up as modules might not have
    m_pMessageManager->ClearMessages();
    
    // Note:
    // - The DataStore manager is persistent.
    // - The Message manager can not be "stopped", just cleared.
    
    return hr;
}


//
// _CleanupManagers
//
void CLiteStep::_CleanupManagers()
{
    if (m_pModuleManager)
    {
        delete m_pModuleManager;
        m_pModuleManager = NULL;
    }
    
    if (m_pMessageManager)
    {
        delete m_pMessageManager;
        m_pMessageManager = NULL;
    }
    
    if (m_pDataStoreManager)
    {
        delete m_pDataStoreManager;
        m_pDataStoreManager = NULL;
    }
}


//
// _Recycle
//
void CLiteStep::_Recycle()
{
    Block block(m_BlockRecycle);
    
    /* Do not allow recursive recycles.  This may happen if some
     * one is heavy fingered on their recycle hotkey, and multiple
     * LM_RECYCLE messages are posted to the queue. */
    if(block.IsBlocked())
    {
        return;
    }
    
    _StopManagers();
    
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
    {
        RESOURCE_MSGBOX(m_hInstance, IDS_LITESTEP_ERROR6,
                        "Recycle has been paused, click OK to continue.", "LiteStep");
    }
    
    // Re-initialize the bang and settings manager in LSAPI 
    LSAPIReloadBangs(); 
    LSAPIReloadSettings(); 

    /* Read in our locally affected settings */ 
    m_bAutoHideModules = GetRCBool("LSAutoHideModules", TRUE) ? true : false; 
    
    _StartManagers();
}


//
// _EnumRevIDs
//
HRESULT CLiteStep::_EnumRevIDs(LSENUMREVIDSPROC pfnCallback, LPARAM lParam) const
{
    HRESULT hr = E_FAIL;
    
    MessageManager::windowSetT setWindows;
    
    if (m_pMessageManager->GetWindowsForMessage(LM_GETREVID, setWindows))
    {
        hr = S_OK;
        
#if !defined(LS_NO_EXCEPTION)
        try
        {
#endif /* LS_NO_EXCEPTION */
            for (MessageManager::windowSetT::iterator iter = setWindows.begin();
                 iter != setWindows.end(); ++iter)
            {
                // Using MAX_LINE_LENGTH to be on the safe side. Modules
                // should assume a length of 64 or so.
                char szBuffer[MAX_LINE_LENGTH] = { 0 };
                
                if (SendMessage(*iter, LM_GETREVID, 0, (LPARAM)&szBuffer) > 0)
                {
                    if (!pfnCallback(szBuffer, lParam))
                    {
                        hr = S_FALSE;
                        break;
                    }
                }
            }
#if !defined(LS_NO_EXCEPTION)
        }
        catch (...)
        {
            hr = E_UNEXPECTED;
        }
#endif /* LS_NO_EXCEPTION */
    }
    
    return hr;
}

bool CLiteStep::_IsFullScreenActiveOnPrimaryMonitor()
{
    /**
     * When this function is called the window that is going fullscreen might
     * not have finished resizing yet.  Calling GetForgroundWindow to get the 
     * handle of the top most window and then calling GetWindowRect with that 
     * handle will return the fullscreen size of that window most of the time
     * but not always.  Using GetWindowPlacement and the rcNormalPosition you
     * get from that works better than GetWindowRect, especially for the Remote
     * Desktop program in WinXP SP2.  Not even that seems to be enough on some
     * computers though.  The most reliable solution found this far is to call
     * Sleep(1) at the beginning of the function to make sure the window that
     * might be fullscreen has time to finish resizing.
     */

    Sleep(1); //Give the window some time to finish resizing

    bool isFullScreen = false;
    HWND hWnd = GetForegroundWindow();
    HMONITOR hmon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONULL);
    POINT p = {0, 0};

    if (IsWindow(hWnd) && hmon == MonitorFromPoint(p, MONITOR_DEFAULTTONULL))
    {
        RECT rWnd, rScreen = {0};
        MONITORINFO mi = {sizeof(mi)};

        rScreen.right = GetSystemMetrics(SM_CXSCREEN);
        rScreen.bottom = GetSystemMetrics(SM_CYSCREEN);

        if (GetMonitorInfo(hmon, &mi))
        {
            rScreen.right = mi.rcMonitor.right - mi.rcMonitor.left;
            rScreen.bottom = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }

        // A window might still not be in its full screen state when we
        // get here (wp.showCmd is sometimes equal to SW_SHOWMINIMIZED),
        // so calling GetWindowRect will not always give us the expected
        // dimensions. Using GetWindowPlacement and its rcNormalPosition
        // RECT will however. It gets us the size the window will have
        // after it has finished resizing.

        WINDOWPLACEMENT wp = {0};
        wp.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(hWnd, &wp))
        {
            CopyRect(&rWnd, &wp.rcNormalPosition);

            // If the window does not have WS_EX_TOOLWINDOW set then the
            // coordinates are workspace coordinates and we must fix this.
            if (0 == (WS_EX_TOOLWINDOW & GetWindowLongPtr(hWnd, GWL_EXSTYLE)))
            {
                RECT rWA = {0};
                VERIFY(SystemParametersInfo(SPI_GETWORKAREA, 0, &rWA, 0));

                rWnd.left += rWA.left;
                rWnd.right += rWA.left;
                rWnd.top += rWA.top;
                rWnd.bottom += rWA.top;
            }
        }

        if (EqualRect(&rScreen, &rWnd))
        {
            isFullScreen = true;
        }
    }

    return isFullScreen;
}

void CLiteStep::_HandleFullScreenApp(bool isFullscreen)
{
    m_bAppIsFullScreen = isFullscreen;

    if (m_pTrayService)
    {
        m_pTrayService->NotifyRudeApp(isFullscreen);
    }

    if (m_bAutoHideModules)
    {
        if (isFullscreen)
        {
            ParseBangCommand(m_hMainWindow, "!HIDEMODULES", NULL);
        }
        else
        {
            ParseBangCommand(m_hMainWindow, "!SHOWMODULES", NULL);
        }
    }
}
