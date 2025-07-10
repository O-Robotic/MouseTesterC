#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <Windows.h>
#include <hidusage.h>
#include <stdbool.h>
#include <CommCtrl.h>
#include <processthreadsapi.h>

typedef unsigned __int64 QWORD;

#define MARK_UNUSED(x) (void)x

#define BTN_CAPTURE 100
#define BTN_EXPORT 101

#define STATUS_CAPTURING "Capturing"
#define STATUS_NOT_CAPTURING "Press capture to start"

//Ideally this should never be hit when doing a normal capture, we want to avoid a potential allocation mid capture
#define DEFAULT_SAMPLE_ALLOCATION_COUNT 80000

enum CaptureMode_e
{
    WndProc = 0,
    Thread
};

struct MouseUpdate_s
{
    LONG lLastX;
    LONG lLastY;
    LARGE_INTEGER captureTimeStamp;
    HANDLE hDevice;
    USHORT usButtonFlags;
};

static HANDLE g_hProcessHeap = INVALID_HANDLE_VALUE;

static size_t g_nAllocatedMouseRecordingSlots = 0;
static size_t g_nRecordedMouseUpdates = 0;
static struct MouseUpdate_s* g_pRecordedMouseUpdates = NULL;

static bool g_bCaptureFromWndProc = false;
static bool g_bMouseRecording = false;
static bool g_bMouseRecordQueued = false;
static volatile bool g_bRunCaptureThread = false;

static HWND g_hMainWnd = NULL;
static HWND g_hStatusDisplay = NULL;
static HWND g_hCaptureButton = NULL;
static HWND g_hExportButton = NULL;
static HWND g_hModeSelectComboBox = NULL;
static HWND g_hCaptureNameBox = NULL;
static HWND g_hCPIValueBox = NULL;

LARGE_INTEGER g_CaptureStartTime = { 0 };
LARGE_INTEGER g_PerformanceFrequency = { 0 };

#ifdef _DEBUG
static void CreateConsole()
{
    (void)AllocConsole();
    FILE* fDummy;
    (void)freopen_s(&fDummy, "CONIN$", "r", stdin);
    (void)freopen_s(&fDummy, "CONOUT$", "w", stderr);
    (void)freopen_s(&fDummy, "CONOUT$", "w", stdout);
}
#endif // _DEBUG

static void RegisterForRawInput(const HWND hTargetWnd, const bool bRegister)
{
    RAWINPUTDEVICE dev = { 0 };
    dev.usUsagePage = HID_USAGE_PAGE_GENERIC;
    dev.usUsage = HID_USAGE_GENERIC_MOUSE;
    dev.dwFlags = RIDEV_INPUTSINK;
    
    if (!bRegister)
        dev.dwFlags |= RIDEV_REMOVE;

    dev.hwndTarget = hTargetWnd;

    (void)RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE));
}

static void MouseRecordingEnded()
{
    g_bRunCaptureThread = false;

    if (g_bCaptureFromWndProc)
    {
        RegisterForRawInput(g_hMainWnd, false);
        g_bCaptureFromWndProc = false;
    }

    (void)SetWindowText(g_hStatusDisplay, TEXT(STATUS_NOT_CAPTURING));
}

static void HandleButtonPressRawInput(const RAWINPUT* const pRawInput, const LARGE_INTEGER* const pTimestamp)
{
    if (pRawInput->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
    {
        if (g_bMouseRecordQueued)
        {
            g_bMouseRecording = true;
            g_bMouseRecordQueued = false;
            g_CaptureStartTime = *pTimestamp;
            (void)SetWindowText(g_hStatusDisplay, TEXT(STATUS_CAPTURING));
#ifdef _DEBUG
            printf("Mouse recording started\n");
#endif // DEBUG
        }
    }
    else if (pRawInput->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
    {
        if (g_bMouseRecording)
        {
            g_bMouseRecording = false;
            MouseRecordingEnded();

#ifdef _DEBUG
            printf("Mouse recording stopped\n");
#endif // DEBUG
        }
    }
}

static void HandleMouseUpdate(const RAWINPUT* const rawEvent, const LARGE_INTEGER* const pUpdateTimestamp)
{
    if (rawEvent->data.mouse.usButtonFlags)
    {
        HandleButtonPressRawInput(rawEvent, pUpdateTimestamp);
    }

    if (g_bMouseRecording)
    {
        if (g_nRecordedMouseUpdates == g_nAllocatedMouseRecordingSlots)
        {
            const size_t nNewBuffSize = g_nAllocatedMouseRecordingSlots * 2;
            struct MouseUpdate_s* newMem = (struct MouseUpdate_s*)HeapReAlloc(g_hProcessHeap, 0, g_pRecordedMouseUpdates, 
                sizeof(struct MouseUpdate_s) * nNewBuffSize);

            if (!newMem)
                return;

            g_nAllocatedMouseRecordingSlots = nNewBuffSize;
            g_pRecordedMouseUpdates = newMem;
        }

        struct MouseUpdate_s* pMouseUpdate = &g_pRecordedMouseUpdates[g_nRecordedMouseUpdates];
        g_nRecordedMouseUpdates++;

        pMouseUpdate->lLastX = rawEvent->data.mouse.lLastX;
        pMouseUpdate->lLastY = rawEvent->data.mouse.lLastY;
        pMouseUpdate->captureTimeStamp = *pUpdateTimestamp;
        pMouseUpdate->hDevice = rawEvent->header.hDevice;
        pMouseUpdate->usButtonFlags = rawEvent->data.mouse.usButtonFlags;
    }
}

static void HandleRawInputUpdate(const RAWINPUT* const rawInput, LARGE_INTEGER* pTimestamp)
{
    if (rawInput->header.dwType == RIM_TYPEMOUSE)
    {
        HandleMouseUpdate(rawInput, pTimestamp);
#ifdef DEBUG
        printf("LastX %d LastY %d\n", rawInput.data.mouse.lLastX, rawInput.data.mouse.lLastY)
#endif // DEBUG
    }
}

static void HandleRawInputHandle(HRAWINPUT hRawInput)
{
    RAWINPUT rawInput;
    UINT dataSize = sizeof(RAWINPUT);
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    (void)GetRawInputData(hRawInput, RID_INPUT, &rawInput, &dataSize, sizeof(RAWINPUTHEADER));
    HandleRawInputUpdate(&rawInput, &timestamp);
}

static DWORD WINAPI ReadRawInputAsThread(LPVOID lpThreadParam)
{
    (void)lpThreadParam;
    HWND dummyWnd = CreateWindowEx(0, TEXT("Message"), NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    RAWINPUT RawInputBuffer[32];
    UINT buffSize = sizeof(RawInputBuffer);
    RegisterForRawInput(dummyWnd, true);
    for (;;)
    {
        if(GetQueueStatus(QS_RAWINPUT))
        {
            if (!g_bRunCaptureThread)
                break;

            LARGE_INTEGER timestamp;
            QueryPerformanceCounter(&timestamp);

            const UINT rawInputBufferCount = GetRawInputBuffer(RawInputBuffer, &buffSize, sizeof(RAWINPUTHEADER));

            if (rawInputBufferCount == -1)
                continue;

            RAWINPUT* pCurrentEvent = RawInputBuffer;
            for (size_t i = 0; i < rawInputBufferCount; i++)
            {
                HandleRawInputUpdate(pCurrentEvent, &timestamp);
                pCurrentEvent = NEXTRAWINPUTBLOCK(pCurrentEvent);
            }
        }
    }
   
    RegisterForRawInput(dummyWnd, false);
    DestroyWindow(dummyWnd);
    return 0;
}

static void CaptureButtonPressed()
{
    LRESULT index = SendMessage(g_hModeSelectComboBox, CB_GETCURSEL, 0, 0);

    switch (index)
    {
    case WndProc:
    {
        g_bCaptureFromWndProc = true;
        RegisterForRawInput(g_hMainWnd, true);
        break;
    }
    case Thread:
    {
        g_bRunCaptureThread = true;
        const HANDLE hThread = CreateThread(NULL, 4096, ReadRawInputAsThread, NULL, CREATE_SUSPENDED, NULL);
        if (!hThread)
        {
            MessageBox(g_hMainWnd, TEXT("Failed to create the capture thread."), TEXT("CreateThread Fail"), MB_OK);
            break;
        }

        SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
        ResumeThread(hThread);
        break;
    }
    }

    if (!g_bMouseRecordQueued && !g_bMouseRecording)
    {
        g_bMouseRecordQueued = true;
        g_nRecordedMouseUpdates = 0;
        SetWindowText(g_hStatusDisplay, TEXT("Press left mouse anywhere to begin, capture will end on left mouse release"));
    }
}

static void ExportButtonPressed()
{
    char szExportFilePath[MAX_PATH];
    SYSTEMTIME currentTime;
    GetSystemTime(&currentTime);
    char* pszCPI = NULL;
    FILE* pFile = NULL;

    const int nCPILength = GetWindowTextLengthA(g_hCPIValueBox);

    if (nCPILength)
        pszCPI = (char*)HeapAlloc(g_hProcessHeap, 0, nCPILength + 1);
    else
    {
        MessageBox(g_hMainWnd, TEXT("Please set your mouse CPI and re-export"), TEXT("No CPI Set"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (!pszCPI)
        return;

    GetWindowTextA(g_hCPIValueBox, pszCPI, nCPILength + 1);

    const int nCaptureNameLen = GetWindowTextLengthA(g_hCaptureNameBox) + 1;
    char* const pszCaptureName = (char* const)HeapAlloc(g_hProcessHeap, 0, nCaptureNameLen);
    if (!pszCaptureName)
        goto cleanup;
        
    GetWindowTextA(g_hCaptureNameBox, pszCaptureName, nCaptureNameLen);

    (void)snprintf(szExportFilePath, sizeof(szExportFilePath), "%s-%d-%d-%d_%d-%d-%d.csv", pszCaptureName,
        currentTime.wYear, currentTime.wMonth, currentTime.wDay, currentTime.wHour, currentTime.wMinute, currentTime.wSecond);

    pFile = fopen(szExportFilePath, "wb+");

    if (!pFile) 
    {
        MessageBox(g_hMainWnd, TEXT("Failed to open output file"), TEXT("Data Save Failed"), MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    (void)fprintf(pFile, "%s\n", pszCaptureName);
    (void)fprintf(pFile, "%s\n", pszCPI);
    (void)fprintf(pFile, "xCount,yCount,Time (ms),buttonflags\n");

    for (size_t i = 0; i < g_nRecordedMouseUpdates; i++)
    {
        const struct MouseUpdate_s* const pMouseUpdate = &g_pRecordedMouseUpdates[i];
        
        LARGE_INTEGER ticksSinceStart;
        ticksSinceStart.QuadPart = pMouseUpdate->captureTimeStamp.QuadPart - g_CaptureStartTime.QuadPart;
        const double timeMs = ticksSinceStart.QuadPart * 1000.0 / g_PerformanceFrequency.QuadPart;
        (void)fprintf(pFile,"%d,%d,%f,%hd\n", pMouseUpdate->lLastX, -(pMouseUpdate->lLastY), timeMs, pMouseUpdate->usButtonFlags);
    }

    //There are no control paths that can return early after a file is successfully opened
    (void)fclose(pFile);

cleanup:
    if(pszCaptureName)
        HeapFree(g_hProcessHeap, 0, pszCaptureName);

    if(pszCPI)
        HeapFree(g_hProcessHeap, 0, pszCPI);

    return;
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {

        case BTN_CAPTURE:
        {
            switch (HIWORD(wParam))
            {
            case BN_CLICKED:
                CaptureButtonPressed();
                break;
            default:
                break;
            }

            break;
        }

        case BTN_EXPORT:
        {
            switch (HIWORD(wParam))
            {
            case BN_CLICKED:
                ExportButtonPressed();
                break;
            default:
                break;
            }
        }
        default:
            break;
        }
        return 0;
    }
   
    case WM_INPUT:
    {
        HandleRawInputHandle((HRAWINPUT)lParam);
        return 0;
    }
    case WM_CLOSE:
    {
        (void)DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
    {
        PostQuitMessage(0);
        return 0;
    }
    default:
        //Drop the default window shit when recording
        if (g_bMouseRecording)
            return 0;
           
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    MARK_UNUSED(hPrevInstance);
    MARK_UNUSED(pCmdLine);

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    g_hProcessHeap = GetProcessHeap();
    (void)QueryPerformanceFrequency(&g_PerformanceFrequency);
    
    g_nAllocatedMouseRecordingSlots = DEFAULT_SAMPLE_ALLOCATION_COUNT;
    g_pRecordedMouseUpdates = (struct MouseUpdate_s*)HeapAlloc(g_hProcessHeap, 0, sizeof(struct MouseUpdate_s) * g_nAllocatedMouseRecordingSlots);

    if (!g_pRecordedMouseUpdates)
        return 1;

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = TEXT("MainWnd");
    wc.hInstance = hInstance;

    RegisterClass(&wc);

    g_hMainWnd = CreateWindow(TEXT("MainWnd"), TEXT("Mouse Tester C"), WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT, 450, 220, NULL, NULL, hInstance, NULL);
    g_hCaptureButton = CreateWindow(TEXT("BUTTON"), TEXT("Capture"), WS_VISIBLE | WS_CHILD, 10, 10, 100, 50, g_hMainWnd, (HMENU)BTN_CAPTURE, hInstance, NULL);
    g_hStatusDisplay = CreateWindow(TEXT("STATIC"), TEXT(STATUS_NOT_CAPTURING), WS_VISIBLE | WS_CHILD | SS_LEFT, 120, 10, 300, 50, g_hMainWnd, NULL, hInstance, NULL);
    g_hExportButton = CreateWindow(TEXT("BUTTON"), TEXT("Export"), WS_VISIBLE | WS_CHILD, 10, 70, 100, 25, g_hMainWnd, (HMENU)BTN_EXPORT, hInstance, NULL);

    (void)CreateWindow(TEXT("STATIC"), TEXT("Capture Mode"), WS_VISIBLE | WS_CHILD | SS_LEFT, 120, 70, 90, 25, g_hMainWnd, NULL, hInstance, NULL);
    g_hModeSelectComboBox = CreateWindow(WC_COMBOBOX, TEXT(""), CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE, 220, 70, 100, 255, g_hMainWnd, NULL, hInstance, NULL);

    (void)CreateWindow(WC_STATIC, TEXT("Capture Name"), WS_VISIBLE | WS_CHILD | SS_LEFT, 10, 105, 100, 25, g_hMainWnd, NULL, hInstance, NULL);
    g_hCaptureNameBox = CreateWindow(WC_EDIT, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER, 120, 105, 300, 25, g_hMainWnd, NULL, hInstance, NULL);
    SetWindowText(g_hCaptureNameBox, TEXT("MouseCapture"));

    (void)CreateWindow(WC_STATIC, TEXT("CPI"), WS_VISIBLE | WS_CHILD | SS_LEFT, 10, 140, 100, 25, g_hMainWnd, NULL, hInstance, NULL);
    g_hCPIValueBox = CreateWindow(WC_EDIT, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 120, 140, 100, 25, g_hMainWnd, NULL, hInstance, NULL);
    SetWindowText(g_hCPIValueBox, TEXT("1600"));

    SendMessage(g_hModeSelectComboBox, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)TEXT("WndProc"));
    SendMessage(g_hModeSelectComboBox, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)TEXT("Thread"));
    SendMessage(g_hModeSelectComboBox, CB_SETCURSEL, (WPARAM)0, (LPARAM)0);

    if (!g_hMainWnd || !g_hCaptureButton || !g_hStatusDisplay || !g_hExportButton || !g_hModeSelectComboBox || !g_hCaptureNameBox || !g_hCPIValueBox)
        return 1;

#ifdef _DEBUG
    CreateConsole();
#endif

    (void)ShowWindow(g_hMainWnd, nCmdShow);

    MSG currentMessage = { 0 };
    while (GetMessage(&currentMessage, NULL, 0, 0))
    {
        TranslateMessage(&currentMessage);
        DispatchMessage(&currentMessage);
    }
    
    (void)HeapFree(g_hProcessHeap, 0, g_pRecordedMouseUpdates);
    g_nAllocatedMouseRecordingSlots = 0;
    g_nRecordedMouseUpdates = 0;
    g_bMouseRecording = false;

    UnregisterClass(wc.lpszClassName, hInstance);

    return 0;
}