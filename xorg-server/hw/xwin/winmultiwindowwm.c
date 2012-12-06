/*
 *Copyright (C) 1994-2000 The XFree86 Project, Inc. All Rights Reserved.
 *Copyright (C) Colin Harrison 2005-2009
 *
 *Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 *"Software"), to deal in the Software without restriction, including
 *without limitation the rights to use, copy, modify, merge, publish,
 *distribute, sublicense, and/or sell copies of the Software, and to
 *permit persons to whom the Software is furnished to do so, subject to
 *the following conditions:
 *
 *The above copyright notice and this permission notice shall be
 *included in all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *NONINFRINGEMENT. IN NO EVENT SHALL THE XFREE86 PROJECT BE LIABLE FOR
 *ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *Except as contained in this notice, the name of the XFree86 Project
 *shall not be used in advertising or otherwise to promote the sale, use
 *or other dealings in this Software without prior written authorization
 *from the XFree86 Project.
 *
 * Authors:	Kensuke Matsuzaki
 *              Colin Harrison
 */

/* X headers */
#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __CYGWIN__
#include <sys/select.h>
#endif
#include <fcntl.h>
#include <setjmp.h>
#define HANDLE void *
#ifdef _MSC_VER
typedef int pid_t;
#endif
#include <pthread.h>
#undef HANDLE
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xwindows.h>

/* Local headers */
#include "winwindow.h"
#include "winprefs.h"
#include "window.h"
#include "pixmapstr.h"
#include "winmsg.h"
#include "windowstr.h"
#include "winmultiwindowclass.h"

#include <shlwapi.h>
#include "taskbar.h"

#ifdef XWIN_MULTIWINDOWEXTWM
#define _WINDOWSWM_SERVER_
#include <X11/extensions/windowswmstr.h>
#else
/* We need the native HWND atom for intWM, so for consistency use the
   same name as extWM would if we were building with enabled... */
#define WINDOWSWM_NATIVE_HWND "_WINDOWSWM_NATIVE_HWND"
#endif

extern void winDebug(const char *format, ...);
extern void winReshapeMultiWindow(WindowPtr pWin);
extern void winUpdateRgnMultiWindow(WindowPtr pWin);

/*
 * Constant defines
 */

#define WIN_CONNECT_RETRIES	5
#define WIN_CONNECT_DELAY	5
#ifdef HAS_DEVWINDOWS
#define WIN_MSG_QUEUE_FNAME	"/dev/windows"
#endif
#define WIN_JMP_OKAY		0
#define WIN_JMP_ERROR_IO	2

/*
 * Local structures
 */

typedef struct _WMMsgNodeRec {
    winWMMessageRec msg;
    struct _WMMsgNodeRec *pNext;
} WMMsgNodeRec, *WMMsgNodePtr;

typedef struct _WMMsgQueueRec {
    struct _WMMsgNodeRec *pHead;
    struct _WMMsgNodeRec *pTail;
    pthread_mutex_t pmMutex;
    pthread_cond_t pcNotEmpty;
#ifdef _DEBUG
    int nQueueSize;
#endif
} WMMsgQueueRec, *WMMsgQueuePtr;

typedef struct _WMInfo {
    Display *pDisplay;
    WMMsgQueueRec wmMsgQueue;
    Atom atmWmProtos;
    Atom atmWmDelete;
    Atom atmPrivMap;
#ifdef XWIN_MULTIWINDOWINTWM
    Bool fAllowOtherWM;
#endif
} WMInfoRec, *WMInfoPtr;

typedef struct _WMProcArgRec {
    DWORD dwScreen;
    WMInfoPtr pWMInfo;
    pthread_mutex_t *ppmServerStarted;
} WMProcArgRec, *WMProcArgPtr;

typedef struct _XMsgProcArgRec {
    Display *pDisplay;
    DWORD dwScreen;
    WMInfoPtr pWMInfo;
    pthread_mutex_t *ppmServerStarted;
    HWND hwndScreen;
} XMsgProcArgRec, *XMsgProcArgPtr;

/*
 * Prototypes for local functions
 */

static void
 PushMessage(WMMsgQueuePtr pQueue, WMMsgNodePtr pNode);

static WMMsgNodePtr PopMessage(WMMsgQueuePtr pQueue, WMInfoPtr pWMInfo);

static Bool
 InitQueue(WMMsgQueuePtr pQueue);

static void
 GetWindowName(Display * pDpy, Window iWin, char **ppWindowName);

static int
 SendXMessage(Display * pDisplay, Window iWin, Atom atmType, long nData);

static void
 UpdateName(WMInfoPtr pWMInfo, Window iWindow);

static void *winMultiWindowWMProc(void *pArg);

static int
 winMultiWindowWMErrorHandler(Display * pDisplay, XErrorEvent * pErr);

static int
 winMultiWindowWMIOErrorHandler(Display * pDisplay);

static void *winMultiWindowXMsgProc(void *pArg);

static int
 winMultiWindowXMsgProcErrorHandler(Display * pDisplay, XErrorEvent * pErr);

static int
 winMultiWindowXMsgProcIOErrorHandler(Display * pDisplay);

static void
winMultiWindowThreadExit(void *arg);

static int
 winRedirectErrorHandler(Display * pDisplay, XErrorEvent * pErr);

static void
 winInitMultiWindowWM(WMInfoPtr pWMInfo, WMProcArgPtr pProcArg);

static Bool

CheckAnotherWindowManager(Display * pDisplay, DWORD dwScreen,
                          Bool fAllowOtherWM);

static void
 winApplyHints(Display * pDisplay, Window iWindow, HWND hWnd, HWND * zstyle);

void
 winUpdateWindowPosition(HWND hWnd, Bool reshape, HWND * zstyle);

/*
 * Local globals
 */

static jmp_buf g_jmpWMEntry;
static XIOErrorHandler g_winMultiWindowWMOldIOErrorHandler;
static pthread_t g_winMultiWindowWMThread;
static jmp_buf g_jmpXMsgProcEntry;
static XIOErrorHandler g_winMultiWindowXMsgProcOldIOErrorHandler;
static pthread_t g_winMultiWindowXMsgProcThread;
static Bool g_shutdown = FALSE;
static Bool redirectError = FALSE;
static Bool g_fAnotherWMRunning = FALSE;
static HMODULE                  g_hmodShell32Dll = NULL;
static HMODULE                  g_hmodOle32Dll = NULL;
static SHGETPROPERTYSTOREFORWINDOWPROC g_pSHGetPropertyStoreForWindow = NULL;
static PROPVARIANTCLEARPROC     g_pPropVariantClear = NULL;

/*
 * PushMessage - Push a message onto the queue
 */

static void
PushMessage(WMMsgQueuePtr pQueue, WMMsgNodePtr pNode)
{

    /* Lock the queue mutex */
    pthread_mutex_lock(&pQueue->pmMutex);

    pNode->pNext = NULL;

    if (pQueue->pTail != NULL) {
        pQueue->pTail->pNext = pNode;
    }
    pQueue->pTail = pNode;

    if (pQueue->pHead == NULL) {
        pQueue->pHead = pNode;
    }

#ifdef _DEBUG
    /* Increase the count of elements in the queue by one */
    ++(pQueue->nQueueSize);
#endif

    /* Release the queue mutex */
    pthread_mutex_unlock(&pQueue->pmMutex);

    /* Signal that the queue is not empty */
    pthread_cond_signal(&pQueue->pcNotEmpty);
}

#ifdef WINDBG
/*
 * QueueSize - Return the size of the queue
 */

static int
QueueSize(WMMsgQueuePtr pQueue)
{
    WMMsgNodePtr pNode;
    int nSize = 0;

    /* Loop through all elements in the queue */
    for (pNode = pQueue->pHead; pNode != NULL; pNode = pNode->pNext)
        ++nSize;

    return nSize;
}
#endif

/*
 * PopMessage - Pop a message from the queue
 */

static WMMsgNodePtr
PopMessage(WMMsgQueuePtr pQueue, WMInfoPtr pWMInfo)
{
    WMMsgNodePtr pNode;

    /* Lock the queue mutex */
    pthread_mutex_lock(&pQueue->pmMutex);

    /* Wait for --- */
    while (pQueue->pHead == NULL) {
        pthread_cond_wait(&pQueue->pcNotEmpty, &pQueue->pmMutex);
    }

    pNode = pQueue->pHead;
    if (pQueue->pHead != NULL) {
        pQueue->pHead = pQueue->pHead->pNext;
    }

    if (pQueue->pTail == pNode) {
        pQueue->pTail = NULL;
    }

  #ifdef _DEBUG
    /* Drop the number of elements in the queue by one */
    --(pQueue->nQueueSize);

  winDebug ("Queue Size %d %d\n", pQueue->nQueueSize, QueueSize(pQueue));
  #endif

    /* Release the queue mutex */
    pthread_mutex_unlock(&pQueue->pmMutex);

    return pNode;
}

#if 0
/*
 * HaveMessage - 
 */

static Bool
HaveMessage(WMMsgQueuePtr pQueue, UINT msg, Window iWindow)
{
    WMMsgNodePtr pNode;

    for (pNode = pQueue->pHead; pNode != NULL; pNode = pNode->pNext) {
        if (pNode->msg.msg == msg && pNode->msg.iWindow == iWindow)
            return True;
    }

    return False;
}
#endif

/*
 * InitQueue - Initialize the Window Manager message queue
 */

static
    Bool
InitQueue(WMMsgQueuePtr pQueue)
{
    /* Check if the pQueue pointer is NULL */
    if (pQueue == NULL) {
        ErrorF("InitQueue - pQueue is NULL.  Exiting.\n");
        return FALSE;
    }

    /* Set the head and tail to NULL */
    pQueue->pHead = NULL;
    pQueue->pTail = NULL;

    /* There are no elements initially */
  #ifdef _DEBUG
    pQueue->nQueueSize = 0;

    winDebug("InitQueue - Queue Size %d %d\n", pQueue->nQueueSize,
             QueueSize(pQueue));
  #endif

    winDebug ("InitQueue - Calling pthread_mutex_init\n");

    /* Create synchronization objects */
    pthread_mutex_init(&pQueue->pmMutex, NULL);

    winDebug ("InitQueue - pthread_mutex_init returned\n");
    winDebug ("InitQueue - Calling pthread_cond_init\n");

    pthread_cond_init(&pQueue->pcNotEmpty, NULL);

    winDebug ("InitQueue - pthread_cond_init returned\n");

    return TRUE;
}

static
char *
Xutf8TextPropertyToString(Display * pDisplay, XTextProperty * xtp)
{
    int nNum;
    char **ppList;
    char *pszReturnData;

    if (Xutf8TextPropertyToTextList(pDisplay, xtp, &ppList, &nNum) >= Success &&
        nNum > 0 && *ppList) {
        int i;
        int iLen = 0;

        for (i = 0; i < nNum; i++)
            iLen += strlen(ppList[i]);
        pszReturnData = (char *) malloc(iLen + 1);
        pszReturnData[0] = '\0';
        for (i = 0; i < nNum; i++)
            strcat(pszReturnData, ppList[i]);
        if (ppList)
            XFreeStringList(ppList);
    }
    else {
        pszReturnData = (char *) malloc(1);
        pszReturnData[0] = '\0';
    }

    return pszReturnData;
}

/*
 * GetWindowName - Retrieve the title of an X Window
 */

static void
GetWindowName(Display * pDisplay, Window iWin, char **ppWindowName)
{
    int nResult;
    XTextProperty xtpWindowName;
    char *pszWindowName;

    winDebug ("GetWindowName\n");

    /* Intialize ppWindowName to NULL */
    *ppWindowName = NULL;

    /* Try to get window name */
    nResult = XGetWMName(pDisplay, iWin, &xtpWindowName);
    if (!nResult || !xtpWindowName.value || !xtpWindowName.nitems) {
        ErrorF("GetWindowName - XGetWMName failed.  No name.\n");
        return;
    }

    pszWindowName = Xutf8TextPropertyToString(pDisplay, &xtpWindowName);
    XFree(xtpWindowName.value);
    *ppWindowName = pszWindowName;
}

/*
 * Send a message to the X server from the WM thread
 */

static int
SendXMessage(Display * pDisplay, Window iWin, Atom atmType, long nData)
{
    XEvent e;

    /* Prepare the X event structure */
    e.type = ClientMessage;
    e.xclient.window = iWin;
    e.xclient.message_type = atmType;
    e.xclient.format = 32;
    e.xclient.data.l[0] = nData;
    e.xclient.data.l[1] = CurrentTime;

    /* Send the event to X */
    return XSendEvent(pDisplay, iWin, False, NoEventMask, &e);
}

/*
 * See if we can get the stored HWND for this window...
 */
static HWND
getHwnd(WMInfoPtr pWMInfo, Window iWindow)
{
    Atom atmType;
    int fmtRet;
    unsigned long items, remain;
    HWND *retHwnd, hWnd = NULL;

    if (XGetWindowProperty(pWMInfo->pDisplay,
                           iWindow,
                           pWMInfo->atmPrivMap,
                           0,
                           1,
                           False,
                           XA_INTEGER,
                           &atmType,
                           &fmtRet,
                           &items,
                           &remain, (unsigned char **) &retHwnd) == Success) {
        if (retHwnd) {
            hWnd = *retHwnd;
            XFree(retHwnd);
        }
    }

    /* Some sanity checks */
    if (!hWnd)
        return NULL;
    if (!IsWindow(hWnd))
        return NULL;

    return hWnd;
}

/*
 * Updates the name of a HWND according to its X WM_NAME property
 */

static void
UpdateName(WMInfoPtr pWMInfo, Window iWindow)
{
    HWND hWnd;
    XWindowAttributes attr;

    hWnd = getHwnd(pWMInfo, iWindow);
    if (!hWnd)
        return;

    /* If window isn't override-redirect */
    XGetWindowAttributes(pWMInfo->pDisplay, iWindow, &attr);
    if (!attr.override_redirect) {
        char *pszWindowName;

        /* Get the X windows window name */
        GetWindowName(pWMInfo->pDisplay, iWindow, &pszWindowName);

        if (pszWindowName) {
            /* Convert from UTF-8 to wide char */
            int iLen =
                MultiByteToWideChar(CP_UTF8, 0, pszWindowName, -1, NULL, 0);
            wchar_t *pwszWideWindowName =
                (wchar_t *) malloc(sizeof(wchar_t) * (iLen + 1));
            MultiByteToWideChar(CP_UTF8, 0, pszWindowName, -1,
                                pwszWideWindowName, iLen);

            /* Set the Windows window name */
            SetWindowTextW(hWnd, pwszWideWindowName);

            free(pwszWideWindowName);
            free(pszWindowName);
        }
    }
}

/*
 * Updates the icon of a HWND according to its X icon properties
 */

static void
UpdateIcon(WMInfoPtr pWMInfo, Window iWindow)
{
    HWND hWnd;
    HICON hIconNew = NULL;
    XWindowAttributes attr;

    hWnd = getHwnd(pWMInfo, iWindow);
    if (!hWnd)
        return;

    /* If window isn't override-redirect */
    XGetWindowAttributes(pWMInfo->pDisplay, iWindow, &attr);
    if (!attr.override_redirect) {
        XClassHint class_hint = { 0, 0 };
        char *window_name = 0;

        if (XGetClassHint(pWMInfo->pDisplay, iWindow, &class_hint)) {
            XFetchName(pWMInfo->pDisplay, iWindow, &window_name);

            hIconNew =
                (HICON) winOverrideIcon(class_hint.res_name,
                                        class_hint.res_class, window_name);

            if (class_hint.res_name)
                XFree(class_hint.res_name);
            if (class_hint.res_class)
                XFree(class_hint.res_class);
            if (window_name)
                XFree(window_name);
        }
    }

    winUpdateIcon(hWnd, pWMInfo->pDisplay, iWindow, hIconNew);
}

#if 0
/*
 * Fix up any differences between the X11 and Win32 window stacks
 * starting at the window passed in
 */
static void
PreserveWin32Stack(WMInfoPtr pWMInfo, Window iWindow, UINT direction)
{
    HWND hWnd;
    DWORD myWinProcID, winProcID;
    Window xWindow;
    WINDOWPLACEMENT wndPlace;

    hWnd = getHwnd(pWMInfo, iWindow);
    if (!hWnd)
        return;

    GetWindowThreadProcessId(hWnd, &myWinProcID);
    hWnd = GetNextWindow(hWnd, direction);

    while (hWnd) {
        GetWindowThreadProcessId(hWnd, &winProcID);
        if (winProcID == myWinProcID) {
            wndPlace.length = sizeof(WINDOWPLACEMENT);
            GetWindowPlacement(hWnd, &wndPlace);
            if (!(wndPlace.showCmd == SW_HIDE ||
                  wndPlace.showCmd == SW_MINIMIZE)) {
                xWindow = (Window) GetProp(hWnd, WIN_WID_PROP);
                if (xWindow) {
                    if (direction == GW_HWNDPREV)
                        XRaiseWindow(pWMInfo->pDisplay, xWindow);
                    else
                        XLowerWindow(pWMInfo->pDisplay, xWindow);
                }
            }
        }
        hWnd = GetNextWindow(hWnd, direction);
    }
}
#endif                          /* PreserveWin32Stack */

/*
 * winMultiWindowWMProc
 */

static void *
winMultiWindowWMProc(void *pArg)
{
    WMProcArgPtr pProcArg = (WMProcArgPtr) pArg;
    WMInfoPtr pWMInfo = pProcArg->pWMInfo;

    pthread_cleanup_push(&winMultiWindowThreadExit, NULL);
  
    /* Initialize the Window Manager */
    winInitMultiWindowWM(pWMInfo, pProcArg);

   winDebug ("winMultiWindowWMProc ()\n");

    /* Loop until we explicitly break out */
    for (;;) {
        WMMsgNodePtr pNode;

#ifdef XWIN_MULTIWINDOWINTWM
        if (g_fAnotherWMRunning) {      /* Another Window manager exists. */
            Sleep(1000);
            continue;
        }
#endif

        /* Pop a message off of our queue */
        pNode = PopMessage(&pWMInfo->wmMsgQueue, pWMInfo);
        if (pNode == NULL) {
            /* Bail if PopMessage returns without a message */
            /* NOTE: Remember that PopMessage is a blocking function. */
            ErrorF("winMultiWindowWMProc - Queue is Empty?  Exiting.\n");
            pthread_exit(NULL);
        }

        winDebug ("winMultiWindowWMProc - %d ms MSG: %d ID: %d\n",
                  GetTickCount(), (int) pNode->msg.msg, (int) pNode->msg.dwID);

        /* Branch on the message type */
        switch (pNode->msg.msg) {
        case WM_WM_RAISE:
            winDebug ("\tWM_WM_RAISE\n");
            /* Raise the window */
            XRaiseWindow(pWMInfo->pDisplay, pNode->msg.iWindow);
            break;

        case WM_WM_LOWER:
            winDebug ("\tWM_WM_LOWER\n");

            /* Lower the window */
            XLowerWindow(pWMInfo->pDisplay, pNode->msg.iWindow);
            break;

        case WM_WM_MAP:
            winDebug ("\tWM_WM_MAP\n");
            /* Put a note as to the HWND associated with this Window */
            XChangeProperty(pWMInfo->pDisplay, pNode->msg.iWindow, pWMInfo->atmPrivMap, XA_INTEGER,     //pWMInfo->atmPrivMap,
                            32,
                            PropModeReplace,
                            (unsigned char *) &(pNode->msg.hwndWindow), 1);
            UpdateName(pWMInfo, pNode->msg.iWindow);
            UpdateIcon(pWMInfo, pNode->msg.iWindow);
            {
                HWND zstyle = HWND_NOTOPMOST;
                winApplyHints (pWMInfo->pDisplay, pNode->msg.iWindow, pNode->msg.hwndWindow, &zstyle);
                winUpdateWindowPosition (pNode->msg.hwndWindow, TRUE, &zstyle);
            }
            break;

        case WM_WM_MAP2:
            winDebug ("\tWM_WM_MAP2\n");

            XChangeProperty(pWMInfo->pDisplay, pNode->msg.iWindow, pWMInfo->atmPrivMap, XA_INTEGER,     //pWMInfo->atmPrivMap,
                            32,
                            PropModeReplace,
                            (unsigned char *) &(pNode->msg.hwndWindow), 1);
            break;

        case WM_WM_MAP3:
            winDebug ("\tWM_WM_MAP3\n");

            /* Put a note as to the HWND associated with this Window */
            XChangeProperty(pWMInfo->pDisplay, pNode->msg.iWindow, pWMInfo->atmPrivMap, XA_INTEGER,     //pWMInfo->atmPrivMap,
                            32,
                            PropModeReplace,
                            (unsigned char *) &(pNode->msg.hwndWindow), 1);
            UpdateName(pWMInfo, pNode->msg.iWindow);
            UpdateIcon(pWMInfo, pNode->msg.iWindow);
            {
                HWND zstyle = HWND_NOTOPMOST;

                winApplyHints(pWMInfo->pDisplay, pNode->msg.iWindow,
                              pNode->msg.hwndWindow, &zstyle);
                winUpdateWindowPosition(pNode->msg.hwndWindow, TRUE, &zstyle);
            }
            break;

        case WM_WM_UNMAP:
            winDebug ("\tWM_WM_UNMAP\n");

            /* Unmap the window */
            XUnmapWindow(pWMInfo->pDisplay, pNode->msg.iWindow);
            break;

        case WM_WM_KILL:
            winDebug ("\tWM_WM_KILL\n");
            {
                int i, n, found = 0;
                Atom *protocols;

                /* --- */
                if (XGetWMProtocols(pWMInfo->pDisplay,
                                    pNode->msg.iWindow, &protocols, &n)) {
                    for (i = 0; i < n; ++i)
                        if (protocols[i] == pWMInfo->atmWmDelete)
                            ++found;

                    XFree(protocols);
                }

                /* --- */
                if (found)
                    SendXMessage(pWMInfo->pDisplay,
                                 pNode->msg.iWindow,
                                 pWMInfo->atmWmProtos, pWMInfo->atmWmDelete);
                else
                    XKillClient(pWMInfo->pDisplay, pNode->msg.iWindow);
            }
            break;

        case WM_WM_ACTIVATE:
            winDebug ("\tWM_WM_ACTIVATE\n");

            /* Set the input focus */
            XSetInputFocus(pWMInfo->pDisplay,
                           pNode->msg.iWindow,
                           RevertToPointerRoot, CurrentTime);
            break;

        case WM_WM_NAME_EVENT:
            UpdateName(pWMInfo, pNode->msg.iWindow);
            break;

        case WM_WM_ICON_EVENT:
            UpdateIcon(pWMInfo, pNode->msg.iWindow);
            break;

        case WM_WM_CHANGE_STATE:
            /* Minimize the window in Windows */
            winMinimizeWindow(pNode->msg.iWindow);
            break;

        default:
            ErrorF("winMultiWindowWMProc - Unknown Message.  Exiting.\n");
            pthread_exit(NULL);
            break;
        }

        /* Free the retrieved message */
        free(pNode);

        /* Flush any pending events on our display */
        XFlush(pWMInfo->pDisplay);
    }

    /* Free the condition variable */
    pthread_cond_destroy(&pWMInfo->wmMsgQueue.pcNotEmpty);

    /* Free the mutex variable */
    pthread_mutex_destroy(&pWMInfo->wmMsgQueue.pmMutex);

    /* Free the passed-in argument */
    free(pProcArg);

    winDebug("-winMultiWindowWMProc ()\n");

    pthread_cleanup_pop(0);

    return NULL;
}

/*
 * X message procedure
 */

static void *
winMultiWindowXMsgProc(void *pArg)
{
    winWMMessageRec msg;
    XMsgProcArgPtr pProcArg = (XMsgProcArgPtr) pArg;
    char pszDisplay[512];
    int iRetries;
    XEvent event;
    Atom atmWmName;
    Atom atmWmHints;
    Atom atmWmChange;
    Atom atmNetWmIcon;
    int iReturn;
    XIconSize *xis;

    pthread_cleanup_push(&winMultiWindowThreadExit, NULL);

    winDebug("winMultiWindowXMsgProc - Hello\n");

    /* Check that argument pointer is not invalid */
    if (pProcArg == NULL) {
        ErrorF("winMultiWindowXMsgProc - pProcArg is NULL.  Exiting.\n");
        pthread_exit(NULL);
    }

    winDebug("winMultiWindowXMsgProc - Calling pthread_mutex_lock ()\n");

    /* Grab the server started mutex - pause until we get it */
    iReturn = pthread_mutex_lock(pProcArg->ppmServerStarted);
    if (iReturn != 0) {
        ErrorF("winMultiWindowXMsgProc - pthread_mutex_lock () failed: %d.  "
               "Exiting.\n", iReturn);
        pthread_exit(NULL);
    }

    winDebug("winMultiWindowXMsgProc - pthread_mutex_lock () returned.\n");

    /* Release the server started mutex */
    pthread_mutex_unlock(pProcArg->ppmServerStarted);

    winDebug("winMultiWindowXMsgProc - pthread_mutex_unlock () returned.\n");

    /* Install our error handler */
    XSetErrorHandler(winMultiWindowXMsgProcErrorHandler);
    g_winMultiWindowXMsgProcThread = pthread_self();
    g_winMultiWindowXMsgProcOldIOErrorHandler =
        XSetIOErrorHandler(winMultiWindowXMsgProcIOErrorHandler);

    /* Set jump point for IO Error exits */
    iReturn = setjmp(g_jmpXMsgProcEntry);

    /* Check if we should continue operations */
    if (iReturn != WIN_JMP_ERROR_IO && iReturn != WIN_JMP_OKAY) {
        /* setjmp returned an unknown value, exit */
        ErrorF("winInitMultiWindowXMsgProc - setjmp returned: %d.  Exiting.\n",
               iReturn);
        pthread_exit(NULL);
    }
    else if (iReturn == WIN_JMP_ERROR_IO) {
        ErrorF("winInitMultiWindowXMsgProc - Caught IO Error.  Exiting.\n");
        pthread_exit(NULL);
    }

    /* Setup the display connection string x */
    winGetDisplayName(pszDisplay, (int)pProcArg->dwScreen);

    /* Print the display connection string */
    winDebug("winMultiWindowXMsgProc - DISPLAY=%s\n", pszDisplay);

    /* Use our generated cookie for authentication */
    winSetAuthorization();

    /* Initialize retry count */
    iRetries = 0;

    /* Open the X display */
    do {
        /* Try to open the display */
        pProcArg->pDisplay = XOpenDisplay(pszDisplay);
        if (pProcArg->pDisplay == NULL) {
            winDebug("winMultiWindowXMsgProc - Could not open display, try: %d, "
                     "sleeping: %d\n", iRetries + 1, WIN_CONNECT_DELAY);
            ++iRetries;
            sleep(WIN_CONNECT_DELAY);
            continue;
        }
        else
            break;
    }
    while (pProcArg->pDisplay == NULL && iRetries < WIN_CONNECT_RETRIES);

    /* Make sure that the display opened */
    if (pProcArg->pDisplay == NULL) {
        ErrorF("winMultiWindowXMsgProc - Failed opening the display.  "
               "Exiting.\n");
        pthread_exit(NULL);
    }

    winDebug("winMultiWindowXMsgProc - XOpenDisplay () returned and "
             "successfully opened the display.\n");

    /* Check if another window manager is already running */
#ifdef XWIN_MULTIWINDOWINTWM
    g_fAnotherWMRunning =
        CheckAnotherWindowManager(pProcArg->pDisplay, pProcArg->dwScreen,
                                  pProcArg->pWMInfo->fAllowOtherWM);
#else
    g_fAnotherWMRunning =
        CheckAnotherWindowManager(pProcArg->pDisplay, pProcArg->dwScreen,
                                  FALSE);
#endif

    if (g_fAnotherWMRunning
#ifdef XWIN_MULTIWINDOWINTWM
        && !pProcArg->pWMInfo->fAllowOtherWM
#endif
      ) {
        ErrorF("winMultiWindowXMsgProc - "
               "another window manager is running.  Exiting.\n");
        pthread_exit(NULL);
    }

    /* Set up the supported icon sizes */
    xis = XAllocIconSize();
    if (xis) {
        xis->min_width = xis->min_height = 16;
        xis->max_width = xis->max_height = 48;
        xis->width_inc = xis->height_inc = 16;
        XSetIconSizes(pProcArg->pDisplay,
                      RootWindow(pProcArg->pDisplay, pProcArg->dwScreen),
                      xis, 1);
        XFree(xis);
    }

    atmWmName = XInternAtom(pProcArg->pDisplay, "WM_NAME", False);
    atmWmHints = XInternAtom(pProcArg->pDisplay, "WM_HINTS", False);
    atmWmChange = XInternAtom(pProcArg->pDisplay, "WM_CHANGE_STATE", False);
    atmNetWmIcon = XInternAtom(pProcArg->pDisplay, "_NET_WM_ICON", False);

    /*
       iiimxcf had a bug until 2009-04-27, assuming that the
       WM_STATE atom exists, causing clients to fail with
       a BadAtom X error if it doesn't.

       Since this is on in the default Solaris 10 install,
       workaround this by making sure it does exist...
     */
    XInternAtom(pProcArg->pDisplay, "WM_STATE", 0);

    /* Loop until we explicitly break out */
    while (1) {
        if (g_shutdown)
            break;

#ifdef XWIN_MULTIWINDOWINTWM
        if (pProcArg->pWMInfo->fAllowOtherWM && !XPending(pProcArg->pDisplay)) {
            if (CheckAnotherWindowManager
                (pProcArg->pDisplay, pProcArg->dwScreen, TRUE)) {
                if (!g_fAnotherWMRunning) {
                    g_fAnotherWMRunning = TRUE;
                    SendMessage(*(HWND *) pProcArg->hwndScreen, WM_UNMANAGE, 0,
                                0);
                }
            }
            else {
                if (g_fAnotherWMRunning) {
                    g_fAnotherWMRunning = FALSE;
                    SendMessage(*(HWND *) pProcArg->hwndScreen, WM_MANAGE, 0,
                                0);
                }
            }
            Sleep(500);
            continue;
        }
#endif

        /* Fetch next event */
        XNextEvent(pProcArg->pDisplay, &event);

        /* Branch on event type */
        if (event.type == MapNotify /* CreateNotify */) {
            XWindowAttributes attr;

            XSelectInput(pProcArg->pDisplay,
                         event.xcreatewindow.window, PropertyChangeMask);

            /* Get the window attributes */
            XGetWindowAttributes(pProcArg->pDisplay,
                                 event.xcreatewindow.window, &attr);

            if (!attr.override_redirect && attr.class != InputOnly)
                XSetWindowBorderWidth(pProcArg->pDisplay,
                                      event.xcreatewindow.window, 0);
        }
        else if (event.type == MapNotify) {
            /* Fake a reparentNotify event as SWT/Motif expects a
               Window Manager to reparent a top-level window when
               it is mapped and waits until they do.

               We don't actually need to reparent, as the frame is
               a native window, not an X window

               We do this on MapNotify, not MapRequest like a real
               Window Manager would, so we don't have do get involved
               in actually mapping the window via it's (non-existent)
               parent...

               See sourceware bugzilla #9848
             */

            XWindowAttributes attr;
            Window root;
            Window parent;
            Window *children;
            unsigned int nchildren;

            if (XGetWindowAttributes(event.xmap.display,
                                     event.xmap.window,
                                     &attr) &&
                XQueryTree(event.xmap.display,
                           event.xmap.window,
                           &root, &parent, &children, &nchildren)) {
                if (children)
                    XFree(children);

                /*
                   It's a top-level window if the parent window is a root window
                   Only non-override_redirect windows can get reparented
                 */
                if ((attr.root == parent) && !event.xmap.override_redirect) {
                    XEvent event_send;

                    event_send.type = ReparentNotify;
                    event_send.xreparent.event = event.xmap.window;
                    event_send.xreparent.window = event.xmap.window;
                    event_send.xreparent.parent = parent;
                    event_send.xreparent.x = attr.x;
                    event_send.xreparent.y = attr.y;
                    event_send.xreparent.override_redirect = False;

                    XSendEvent(event.xmap.display,
                               event.xmap.window,
                               True, StructureNotifyMask, &event_send);
                }
            }
        }
        else if (event.type == ConfigureNotify) {
            if (!event.xconfigure.send_event) {
                /*
                   Java applications using AWT on JRE 1.6.0 break with non-reparenting WMs AWT
                   doesn't explicitly know about (See sun bug #6434227)

                   XDecoratedPeer.handleConfigureNotifyEvent() only processes non-synthetic
                   ConfigureNotify events to update window location if it's identified the
                   WM as a non-reparenting WM it knows about (compiz or lookingglass)

                   Rather than tell all sorts of lies to get XWM to recognize us as one of
                   those, simply send a synthetic ConfigureNotify for every non-synthetic one
                 */
                XEvent event_send = event;

                event_send.xconfigure.send_event = TRUE;
                event_send.xconfigure.event = event.xconfigure.window;
                XSendEvent(event.xconfigure.display,
                           event.xconfigure.window,
                           True, StructureNotifyMask, &event_send);
            }
        }
        else if (event.type == PropertyNotify) {
            if (event.xproperty.atom == atmWmName) {
                memset(&msg, 0, sizeof(msg));

                msg.msg = WM_WM_NAME_EVENT;
                msg.iWindow = event.xproperty.window;

                /* Other fields ignored */
                winSendMessageToWM(pProcArg->pWMInfo, &msg);
            }
            else if ((event.xproperty.atom == atmWmHints) ||
                     (event.xproperty.atom == atmNetWmIcon)) {
                memset(&msg, 0, sizeof(msg));
                msg.msg = WM_WM_ICON_EVENT;
                msg.iWindow = event.xproperty.window;

                /* Other fields ignored */
                winSendMessageToWM(pProcArg->pWMInfo, &msg);
            }
        }
        else if (event.type == ClientMessage
                 && event.xclient.message_type == atmWmChange
                 && event.xclient.data.l[0] == IconicState) {
            winDebug("winMultiWindowXMsgProc - WM_CHANGE_STATE - IconicState\n");

            memset(&msg, 0, sizeof(msg));

            msg.msg = WM_WM_CHANGE_STATE;
            msg.iWindow = event.xclient.window;

            winSendMessageToWM(pProcArg->pWMInfo, &msg);
        }
    }

    XCloseDisplay(pProcArg->pDisplay);
    pthread_cleanup_pop(0);
    return NULL;
}

/*
 * winInitWM - Entry point for the X server to spawn
 * the Window Manager thread.  Called from
 * winscrinit.c/winFinishScreenInitFB ().
 */

Bool
winInitWM(void **ppWMInfo,
          pthread_t * ptWMProc,
          pthread_t * ptXMsgProc,
          pthread_mutex_t * ppmServerStarted,
          int dwScreen, HWND hwndScreen, BOOL allowOtherWM)
{
    WMProcArgPtr pArg = (WMProcArgPtr) malloc(sizeof(WMProcArgRec));
    WMInfoPtr pWMInfo = (WMInfoPtr) malloc(sizeof(WMInfoRec));
    XMsgProcArgPtr pXMsgArg = (XMsgProcArgPtr) malloc(sizeof(XMsgProcArgRec));

    /* Bail if the input parameters are bad */
    if (pArg == NULL || pWMInfo == NULL || pXMsgArg == NULL) {
        ErrorF("winInitWM - malloc failed.\n");
        free(pArg);
        free(pWMInfo);
        free(pXMsgArg);
        return FALSE;
    }

    /* Zero the allocated memory */
    ZeroMemory(pArg, sizeof(WMProcArgRec));
    ZeroMemory(pWMInfo, sizeof(WMInfoRec));
    ZeroMemory(pXMsgArg, sizeof(XMsgProcArgRec));

    /* Set a return pointer to the Window Manager info structure */
    *ppWMInfo = pWMInfo;
#ifdef XWIN_MULTIWINDOWINTWM
    pWMInfo->fAllowOtherWM = allowOtherWM;
#endif

    /* Setup the argument structure for the thread function */
    pArg->dwScreen = dwScreen;
    pArg->pWMInfo = pWMInfo;
    pArg->ppmServerStarted = ppmServerStarted;

    /* Intialize the message queue */
    if (!InitQueue(&pWMInfo->wmMsgQueue)) {
        ErrorF("winInitWM - InitQueue () failed.\n");
        return FALSE;
    }

    /* Spawn a thread for the Window Manager */
    if (pthread_create(ptWMProc, NULL, winMultiWindowWMProc, pArg)) {
        /* Bail if thread creation failed */
        ErrorF("winInitWM - pthread_create failed for Window Manager.\n");
        return FALSE;
    }

    /* Spawn the XNextEvent thread, will send messages to WM */
    pXMsgArg->dwScreen = dwScreen;
    pXMsgArg->pWMInfo = pWMInfo;
    pXMsgArg->ppmServerStarted = ppmServerStarted;
    pXMsgArg->hwndScreen = hwndScreen;
    if (pthread_create(ptXMsgProc, NULL, winMultiWindowXMsgProc, pXMsgArg)) {
        /* Bail if thread creation failed */
        ErrorF("winInitWM - pthread_create failed on XMSG.\n");
        return FALSE;
    }

    winDebug("winInitWM - Returning.\n");

    return TRUE;
}

/*
 * Window manager thread - setup
 */

static void
winInitMultiWindowWM(WMInfoPtr pWMInfo, WMProcArgPtr pProcArg)
{
    int iRetries = 0;
    char pszDisplay[512];
    int iReturn;

    winDebug("winInitMultiWindowWM - Hello\n");

    /* Check that argument pointer is not invalid */
    if (pProcArg == NULL) {
        ErrorF("winInitMultiWindowWM - pProcArg is NULL.  Exiting.\n");
        pthread_exit(NULL);
    }

    winDebug("winInitMultiWindowWM - Calling pthread_mutex_lock ()\n");

    /* Grab our garbage mutex to satisfy pthread_cond_wait */
    iReturn = pthread_mutex_lock(pProcArg->ppmServerStarted);
    if (iReturn != 0) {
        ErrorF("winInitMultiWindowWM - pthread_mutex_lock () failed: %d.  "
               "Exiting.\n", iReturn);
        pthread_exit(NULL);
    }

    winDebug("winInitMultiWindowWM - pthread_mutex_lock () returned.\n");

    /* Release the server started mutex */
    pthread_mutex_unlock(pProcArg->ppmServerStarted);

    winDebug("winInitMultiWindowWM - pthread_mutex_unlock () returned.\n");

    /* Install our error handler */
    XSetErrorHandler(winMultiWindowWMErrorHandler);
    g_winMultiWindowWMThread = pthread_self();
    g_winMultiWindowWMOldIOErrorHandler =
        XSetIOErrorHandler(winMultiWindowWMIOErrorHandler);

    /* Set jump point for IO Error exits */
    iReturn = setjmp(g_jmpWMEntry);

    /* Check if we should continue operations */
    if (iReturn != WIN_JMP_ERROR_IO && iReturn != WIN_JMP_OKAY) {
        /* setjmp returned an unknown value, exit */
        ErrorF("winInitMultiWindowWM - setjmp returned: %d.  Exiting.\n",
               iReturn);
        pthread_exit(NULL);
    }
    else if (iReturn == WIN_JMP_ERROR_IO) {
        ErrorF("winInitMultiWindowWM - Caught IO Error.  Exiting.\n");
        pthread_exit(NULL);
    }

    /* Setup the display connection string x */
    winGetDisplayName(pszDisplay, (int)pProcArg->dwScreen);

    /* Print the display connection string */
    winDebug("winInitMultiWindowWM - DISPLAY=%s\n", pszDisplay);

    /* Use our generated cookie for authentication */
    winSetAuthorization();

    /* Open the X display */
    do {
        /* Try to open the display */
        pWMInfo->pDisplay = XOpenDisplay(pszDisplay);
        if (pWMInfo->pDisplay == NULL) {
            ErrorF("winInitMultiWindowWM - Could not open display, try: %d, "
                   "sleeping: %d\n", iRetries + 1, WIN_CONNECT_DELAY);
            ++iRetries;
            sleep(WIN_CONNECT_DELAY);
            continue;
        }
        else
            break;
    }
    while (pWMInfo->pDisplay == NULL && iRetries < WIN_CONNECT_RETRIES);

    /* Make sure that the display opened */
    if (pWMInfo->pDisplay == NULL) {
        ErrorF("winInitMultiWindowWM - Failed opening the display.  "
               "Exiting.\n");
        pthread_exit(NULL);
    }

    winDebug("winInitMultiWindowWM - XOpenDisplay () returned and "
             "successfully opened the display.\n");

    /* Create some atoms */
    pWMInfo->atmWmProtos = XInternAtom(pWMInfo->pDisplay,
                                       "WM_PROTOCOLS", False);
    pWMInfo->atmWmDelete = XInternAtom(pWMInfo->pDisplay,
                                       "WM_DELETE_WINDOW", False);

    pWMInfo->atmPrivMap = XInternAtom(pWMInfo->pDisplay,
                                      WINDOWSWM_NATIVE_HWND, False);

    if (1) {
        Cursor cursor = XCreateFontCursor(pWMInfo->pDisplay, XC_left_ptr);

        if (cursor) {
            XDefineCursor(pWMInfo->pDisplay,
                          DefaultRootWindow(pWMInfo->pDisplay), cursor);
            XFreeCursor(pWMInfo->pDisplay, cursor);
        }
    }
}

/*
 * winSendMessageToWM - Send a message from the X thread to the WM thread
 */

void
winSendMessageToWM(void *pWMInfo, winWMMessagePtr pMsg)
{
    WMMsgNodePtr pNode;

    winDebug("winSendMessageToWM ()\n");

    pNode = (WMMsgNodePtr) malloc(sizeof(WMMsgNodeRec));
    if (pNode != NULL) {
        memcpy(&pNode->msg, pMsg, sizeof(winWMMessageRec));
        PushMessage(&((WMInfoPtr) pWMInfo)->wmMsgQueue, pNode);
    }
}

/*
 * Window manager error handler
 */

static int
winMultiWindowWMErrorHandler(Display * pDisplay, XErrorEvent * pErr)
{
    char pszErrorMsg[100];

    if (pErr->request_code == X_ChangeWindowAttributes
        && pErr->error_code == BadAccess) {
        ErrorF("winMultiWindowWMErrorHandler - ChangeWindowAttributes "
               "BadAccess.\n");
        return 0;
    }

    XGetErrorText(pDisplay, pErr->error_code, pszErrorMsg, sizeof(pszErrorMsg));
    ErrorF("winMultiWindowWMErrorHandler - ERROR: %s\n"
           "  errorCode %d\n"
           "  serial %d\n"
           "  resourceID 0x%x\n"
           "  majorCode %d\n"
           "  minorCode %d\n"
           , pszErrorMsg
           , pErr->error_code
           , pErr->serial
           , pErr->resourceid
           , pErr->request_code
           , pErr->minor_code);

    return 0;
}

/*
 * Window manager IO error handler
 */

static int
winMultiWindowWMIOErrorHandler(Display * pDisplay)
{
    ErrorF("winMultiWindowWMIOErrorHandler!\n\n");

    if (pthread_equal(pthread_self(), g_winMultiWindowWMThread)) {
        if (g_shutdown)
            pthread_exit(NULL);

        /* Restart at the main entry point */
        longjmp(g_jmpWMEntry, WIN_JMP_ERROR_IO);
    }

    if (g_winMultiWindowWMOldIOErrorHandler)
        g_winMultiWindowWMOldIOErrorHandler(pDisplay);

    return 0;
}

/*
 * X message procedure error handler
 */

static int
winMultiWindowXMsgProcErrorHandler(Display * pDisplay, XErrorEvent * pErr)
{
    char pszErrorMsg[100];

    XGetErrorText(pDisplay, pErr->error_code, pszErrorMsg, sizeof(pszErrorMsg));
    ErrorF("winMultiWindowXMsgProcErrorHandler - ERROR: %s\n"
           "  errorCode %d\n"
           "  serial %d\n"
           "  resourceID 0x%x\n"
           "  majorCode %d\n"
           "  minorCode %d\n"
           , pszErrorMsg
           , pErr->error_code
           , pErr->serial
           , pErr->resourceid
           , pErr->request_code
           , pErr->minor_code);

    return 0;
}

/*
 * X message procedure IO error handler
 */

static int
winMultiWindowXMsgProcIOErrorHandler(Display * pDisplay)
{
    ErrorF("winMultiWindowXMsgProcIOErrorHandler!\n\n");

    if (pthread_equal(pthread_self(), g_winMultiWindowXMsgProcThread)) {
        /* Restart at the main entry point */
        longjmp(g_jmpXMsgProcEntry, WIN_JMP_ERROR_IO);
    }

    if (g_winMultiWindowXMsgProcOldIOErrorHandler)
        g_winMultiWindowXMsgProcOldIOErrorHandler(pDisplay);

    return 0;
}

/*
 * winMultiWindowThreadExit - Thread exit handler
 */

static void
winMultiWindowThreadExit(void *arg)
{
    AbortDDX(EXIT_ERR_ABORT);

    /* multiwindow client thread has exited, stop server as well */
    TerminateProcess(GetCurrentProcess(),1);
}
/*
 * Catch RedirectError to detect other window manager running
 */

static int
winRedirectErrorHandler(Display * pDisplay, XErrorEvent * pErr)
{
    redirectError = TRUE;
    return 0;
}

/*
 * Check if another window manager is running
 */

static Bool
CheckAnotherWindowManager(Display * pDisplay, DWORD dwScreen,
                          Bool fAllowOtherWM)
{
    /*
       Try to select the events which only one client at a time is allowed to select.
       If this causes an error, another window manager is already running...
     */
    redirectError = FALSE;
    XSetErrorHandler(winRedirectErrorHandler);
    XSelectInput(pDisplay, RootWindow(pDisplay, dwScreen),
                 ResizeRedirectMask | SubstructureRedirectMask |
                 ButtonPressMask);
    XSync(pDisplay, 0);
    XSetErrorHandler(winMultiWindowXMsgProcErrorHandler);

    /*
       Side effect: select the events we are actually interested in...

       If other WMs are not allowed, also select one of the events which only one client
       at a time is allowed to select, so other window managers won't start...
     */
    XSelectInput(pDisplay, RootWindow(pDisplay, dwScreen),
                 SubstructureNotifyMask | (!fAllowOtherWM ? ButtonPressMask :
                                           0));
    XSync(pDisplay, 0);
    return redirectError;
}

/*
 * Notify the MWM thread we're exiting and not to reconnect
 */

void
winDeinitMultiWindowWM(void)
{
    winDebug("winDeinitMultiWindowWM - Noting shutdown in progress\n");
    g_shutdown = TRUE;
}

/* Windows window styles */
#define HINT_NOFRAME	(1l<<0)
#define HINT_BORDER	(1L<<1)
#define HINT_SIZEBOX	(1l<<2)
#define HINT_CAPTION	(1l<<3)
#define HINT_NOMAXIMIZE (1L<<4)
#define HINT_NOMINIMIZE (1L<<5)
#define HINT_NOSYSMENU  (1L<<6)
#define HINT_SKIPTASKBAR (1L<<7)
/* These two are used on their own */
#define HINT_MAX	(1L<<0)
#define HINT_MIN	(1L<<1)

static void
winApplyHints(Display * pDisplay, Window iWindow, HWND hWnd, HWND * zstyle)
{
    static Atom windowState, motif_wm_hints, windowType;
    static Atom hiddenState, fullscreenState, belowState, aboveState,
        skiptaskbarState;
    static Atom dockWindow;
    static int generation;
    Atom type, *pAtom = NULL;
    int format;
    unsigned long hint = 0, maxmin = 0, nitems = 0, left = 0;
    unsigned long style, exStyle;
    WindowPtr pWin = GetProp (hWnd, WIN_WINDOW_PROP);
    MwmHints *mwm_hint = NULL;
    WinXSizeHints SizeHints;

    if (!hWnd)
        return;
    if (!IsWindow(hWnd))
        return;

    if (generation != serverGeneration) {
        generation = serverGeneration;
        windowState = XInternAtom(pDisplay, "_NET_WM_STATE", False);
        motif_wm_hints = XInternAtom(pDisplay, "_MOTIF_WM_HINTS", False);
        windowType = XInternAtom(pDisplay, "_NET_WM_WINDOW_TYPE", False);
        hiddenState = XInternAtom(pDisplay, "_NET_WM_STATE_HIDDEN", False);
        fullscreenState =
            XInternAtom(pDisplay, "_NET_WM_STATE_FULLSCREEN", False);
        belowState = XInternAtom(pDisplay, "_NET_WM_STATE_BELOW", False);
        aboveState = XInternAtom(pDisplay, "_NET_WM_STATE_ABOVE", False);
        dockWindow = XInternAtom(pDisplay, "_NET_WM_WINDOW_TYPE_DOCK", False);
        skiptaskbarState =
            XInternAtom(pDisplay, "_NET_WM_STATE_SKIP_TASKBAR", False);
    }

    if (XGetWindowProperty(pDisplay, iWindow, windowState, 0L,
                           1L, False, XA_ATOM, &type, &format,
                           &nitems, &left,
                           (unsigned char **) &pAtom) == Success) {
        if (pAtom && nitems == 1) {
            if (*pAtom == skiptaskbarState)
                hint |= HINT_SKIPTASKBAR;
            if (*pAtom == hiddenState)
                maxmin |= HINT_MIN;
            else if (*pAtom == fullscreenState)
                maxmin |= HINT_MAX;
            if (*pAtom == belowState)
                *zstyle = HWND_BOTTOM;
            else if (*pAtom == aboveState)
                *zstyle = HWND_TOPMOST;
        }
        if (pAtom)
            XFree(pAtom);
    }

    nitems = left = 0;
    if (XGetWindowProperty(pDisplay, iWindow, motif_wm_hints, 0L,
                           PropMwmHintsElements, False, motif_wm_hints, &type,
                           &format, &nitems, &left,
                           (unsigned char **) &mwm_hint) == Success) {
        if (mwm_hint && nitems == PropMwmHintsElements &&
            (mwm_hint->flags & MwmHintsDecorations)) {
            if (!mwm_hint->decorations)
                hint |= HINT_NOFRAME;
            else if (!(mwm_hint->decorations & MwmDecorAll)) {
                if (mwm_hint->decorations & MwmDecorBorder)
                    hint |= HINT_BORDER;
                if (mwm_hint->decorations & MwmDecorHandle)
                    hint |= HINT_SIZEBOX;
                if (mwm_hint->decorations & MwmDecorTitle)
                    hint |= HINT_CAPTION;
                if (!(mwm_hint->decorations & MwmDecorMenu))
                    hint |= HINT_NOSYSMENU;
                if (!(mwm_hint->decorations & MwmDecorMinimize))
                    hint |= HINT_NOMINIMIZE;
                if (!(mwm_hint->decorations & MwmDecorMaximize))
                    hint |= HINT_NOMAXIMIZE;
            }
            else {
                /*
                   MwmDecorAll means all decorations *except* those specified by other flag
                   bits that are set.  Not yet implemented.
                 */
            }
        }
        if (mwm_hint)
            XFree(mwm_hint);
    }

    nitems = left = 0;
    pAtom = NULL;
    if (XGetWindowProperty(pDisplay, iWindow, windowType, 0L,
                           1L, False, XA_ATOM, &type, &format,
                           &nitems, &left,
                           (unsigned char **) &pAtom) == Success) {
        if (pAtom && nitems == 1) {
            if (*pAtom == dockWindow) {
                hint = (hint & ~HINT_NOFRAME) | HINT_SIZEBOX;   /* Xming puts a sizebox on dock windows */
                *zstyle = HWND_TOPMOST;
            }
        }
        if (pAtom)
            XFree(pAtom);
    }

    {
        XSizeHints *normal_hint = XAllocSizeHints();
        long supplied;

        if (normal_hint &&
            (XGetWMNormalHints(pDisplay, iWindow, normal_hint, &supplied) ==
             Success)) {
            if (normal_hint->flags & PMaxSize) {
                /* Not maximizable if a maximum size is specified */
                hint |= HINT_NOMAXIMIZE;

                if (normal_hint->flags & PMinSize) {
                    /*
                       If both minimum size and maximum size are specified and are the same,
                       don't bother with a resizing frame
                     */
                    if ((normal_hint->min_width == normal_hint->max_width)
                        && (normal_hint->min_height == normal_hint->max_height))
                        hint = (hint & ~HINT_SIZEBOX);
                }
            }
        }
        XFree(normal_hint);
    }

    /*
       Override hint settings from above with settings from config file and set
       application id for grouping.
     */
    {
        XClassHint class_hint = { 0, 0 };
        char *application_id = 0;

        if (XGetClassHint(pDisplay, iWindow, &class_hint)) {
            char *window_name = 0;
            char *application_id = 0;
            XFetchName(pDisplay, iWindow, &window_name);

            style =
                winOverrideStyle(class_hint.res_name, class_hint.res_class,
                                 window_name);

#define APPLICATION_ID_FORMAT	"%s.vcxsrv.%s"
#define APPLICATION_ID_UNKNOWN "unknown"
            if (class_hint.res_class) {
                asprintf(&application_id, APPLICATION_ID_FORMAT, XVENDORNAME,
                         class_hint.res_class);
            }
            else {
                asprintf(&application_id, APPLICATION_ID_FORMAT, XVENDORNAME,
                         APPLICATION_ID_UNKNOWN);
            }
            winSetAppUserModelID(hWnd, application_id);

            if (class_hint.res_name)
                XFree(class_hint.res_name);
            if (class_hint.res_class)
                XFree(class_hint.res_class);
            if (application_id)
                free(application_id);
            if (window_name)
                XFree(window_name);
        }
        else {
            style = STYLE_NONE;
        }
    }

    if (style & STYLE_TOPMOST)
        *zstyle = HWND_TOPMOST;
    else if (style & STYLE_MAXIMIZE)
        maxmin = (hint & ~HINT_MIN) | HINT_MAX;
    else if (style & STYLE_MINIMIZE)
        maxmin = (hint & ~HINT_MAX) | HINT_MIN;
    else if (style & STYLE_BOTTOM)
        *zstyle = HWND_BOTTOM;

    if (maxmin & HINT_MAX)
        SendMessage(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    else if (maxmin & HINT_MIN)
        SendMessage(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);

    if (style & STYLE_NOTITLE)
        hint =
            (hint & ~HINT_NOFRAME & ~HINT_BORDER & ~HINT_CAPTION) |
            HINT_SIZEBOX;
    else if (style & STYLE_OUTLINE)
        hint =
            (hint & ~HINT_NOFRAME & ~HINT_CAPTION) | HINT_BORDER;
    else if (style & STYLE_NOFRAME)
        hint =
            (hint & ~HINT_BORDER & ~HINT_CAPTION) | HINT_NOFRAME;

    /* Now apply styles to window */
    style = GetWindowLongPtr(hWnd, GWL_STYLE);
    if (!style)
        return;                 /* GetWindowLongPointer returns 0 on failure, we hope this isn't a valid style */

    style &= ~WS_CAPTION & ~WS_SIZEBOX; /* Just in case */

    if (!(hint & ~HINT_SKIPTASKBAR))    /* No hints, default */
        style = style | WS_CAPTION;
    else if (hint & HINT_NOFRAME)       /* No frame, no decorations */
        style = style & ~WS_CAPTION & ~WS_SIZEBOX;
    else
        style = style | ((hint & HINT_BORDER) ? WS_BORDER : 0) |
            ((hint & HINT_SIZEBOX) ? WS_SIZEBOX : 0) |
            ((hint & HINT_CAPTION) ? WS_CAPTION : 0);

    if (hint & HINT_NOMAXIMIZE)
        style = style & ~WS_MAXIMIZEBOX;

    if (hint & HINT_NOMINIMIZE)
        style = style & ~WS_MINIMIZEBOX;

    if (hint & HINT_NOSYSMENU)
        style = style & ~WS_SYSMENU;

    if (hint & HINT_SKIPTASKBAR)
        style = style & ~WS_MINIMIZEBOX;        /* window will become lost if minimized */

    if (!IsWindow (hWnd))
    {
        ErrorF("Windows window 0x%x has become invalid, so returning without applying hints\n",hWnd);
        return;
    }
                
    if (winMultiWindowGetWMNormalHints(pWin, &SizeHints))
    {
        if (!(SizeHints.max_width&&SizeHints.max_height&&(SizeHints.min_width == SizeHints.max_width)&&(SizeHints.min_height == SizeHints.max_height) ))
            style|=WS_SIZEBOX;
    }
    else
        style|=WS_SIZEBOX;
    SetWindowLongPtr(hWnd, GWL_STYLE, style);

    exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    if (hint & HINT_SKIPTASKBAR)
        exStyle = (exStyle & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW;
    else
        exStyle = (exStyle & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW;
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, exStyle);

    winDebug
        ("winApplyHints: iWindow 0x%08x hints 0x%08x style 0x%08x exstyle 0x%08x\n",
         iWindow, hint, style, exStyle);
}

void
winUpdateWindowPosition(HWND hWnd, Bool reshape, HWND * zstyle)
{
    int iX, iY, iWidth, iHeight;
    int iDx, iDy;
    RECT rcNew;
    WindowPtr pWin = GetProp(hWnd, WIN_WINDOW_PROP);
    DrawablePtr pDraw = NULL;

    if (!pWin)
        return;
    pDraw = &pWin->drawable;
    if (!pDraw)
        return;

    /* Get the X and Y location of the X window */
    iX = pWin->drawable.x + GetSystemMetrics(SM_XVIRTUALSCREEN);
    iY = pWin->drawable.y + GetSystemMetrics(SM_YVIRTUALSCREEN);

    /* Get the height and width of the X window */
    iWidth = pWin->drawable.width;
    iHeight = pWin->drawable.height;

    /* Setup a rectangle with the X window position and size */
    SetRect(&rcNew, iX, iY, iX + iWidth, iY + iHeight);

    AdjustWindowRectEx(&rcNew, GetWindowLongPtr(hWnd, GWL_STYLE), FALSE,
                       GetWindowLongPtr(hWnd, GWL_EXSTYLE));

    /* Don't allow window decoration to disappear off to top-left as a result of this adjustment */
    if (rcNew.left < GetSystemMetrics(SM_XVIRTUALSCREEN)) {
        iDx = GetSystemMetrics(SM_XVIRTUALSCREEN) - rcNew.left;
        rcNew.left += iDx;
        rcNew.right += iDx;
    }

    if (rcNew.top < GetSystemMetrics(SM_YVIRTUALSCREEN)) {
        iDy = GetSystemMetrics(SM_YVIRTUALSCREEN) - rcNew.top;
        rcNew.top += iDy;
        rcNew.bottom += iDy;
    }

    /* Position the Windows window */
    SetWindowPos(hWnd, *zstyle, rcNew.left, rcNew.top,
                 rcNew.right - rcNew.left, rcNew.bottom - rcNew.top, 0);

    if (reshape) {
        winReshapeMultiWindow(pWin);
        winUpdateRgnMultiWindow(pWin);
    }
}

void
winTaskbarInit (void)
{
  /*
    Load libraries and get function pointers to SHGetPropertyStoreForWindow
    and PropVariantClear for winSetAppID()
  */

  /*
    SHGetPropertyStoreForWindow is only supported since Windows 7. On previous
    versions the pointer will be NULL and taskbar grouping is not supported.
    winSetAppID() will do nothing in this case.
  */
  g_hmodShell32Dll = LoadLibrary ("shell32.dll");
  if (g_hmodShell32Dll == NULL)
    {
      ErrorF ("winTaskbarInit - Could not load shell32.dll\n");
      return;
    }

  g_pSHGetPropertyStoreForWindow = (SHGETPROPERTYSTOREFORWINDOWPROC) GetProcAddress (g_hmodShell32Dll, "SHGetPropertyStoreForWindow");
  if (g_pSHGetPropertyStoreForWindow == NULL)
    {
      ErrorF ("winTaskbarInit - Could not get SHGetPropertyStoreForWindow address\n");
      return;
    }

  /*
    PropVariantClear is supported since NT4, but we have no propidl.h to
    provide a prototype for it
  */
  g_hmodOle32Dll = LoadLibrary ("ole32.dll");
  if (g_hmodOle32Dll == NULL)
    {
      ErrorF ("winTaskbarInit - Could not load ole32.dll\n");
      return;
    }

  g_pPropVariantClear = (PROPVARIANTCLEARPROC) GetProcAddress (g_hmodOle32Dll, "PropVariantClear");
  if (g_pPropVariantClear == NULL)
    {
      ErrorF ("winTaskbarInit - Could not get g_pPropVariantClear address\n");
      return;
    }
}

void
winTaskbarDestroy (void)
{
  if (g_hmodOle32Dll != NULL)
    {
      FreeLibrary (g_hmodOle32Dll);
      g_hmodOle32Dll = NULL;
      g_pPropVariantClear = NULL;
    }
  if (g_hmodShell32Dll != NULL)
    {
      FreeLibrary (g_hmodShell32Dll);
      g_hmodShell32Dll = NULL;
      g_pSHGetPropertyStoreForWindow = NULL;
    }
}

void
winSetAppID (HWND hWnd, const char* AppID)
{
  PROPVARIANT pv;
  IPropertyStore *pps = NULL;
  HRESULT hr;

  if (g_pSHGetPropertyStoreForWindow == NULL ||
      g_pPropVariantClear == NULL)
    {
      return;
    }

  winDebug ("winSetAppID - hwnd 0x%08x appid '%s'\n", hWnd, AppID);

  hr = g_pSHGetPropertyStoreForWindow (hWnd, &IID_IPropertyStore, (void**)&pps);
  if(SUCCEEDED(hr) && pps)
    {
      memset(&pv, 0, sizeof(PROPVARIANT));
      if(AppID)
        {
          pv.vt = VT_LPWSTR;
          hr = SHStrDupA(AppID, &pv.pwszVal);
        }

      if(SUCCEEDED(hr))
        {
          hr = pps->lpVtbl->SetValue(pps, &PKEY_AppUserModel_ID, &pv);
          g_pPropVariantClear(&pv);
        }
      pps->lpVtbl->Release(pps);
    }
}
