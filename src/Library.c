#include <windows.h>
#include <tlhelp32.h>

#include "State.h"

/* ------------------------------------------------------------------ */
/*  NT API function pointers for suspended process management         */
/* ------------------------------------------------------------------ */
typedef LONG NTSTATUS;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif

typedef NTSTATUS(NTAPI *pfnNtSuspendProcess)(HANDLE);
typedef NTSTATUS(NTAPI *pfnNtResumeProcess)(HANDLE);

static pfnNtSuspendProcess g_NtSuspendProcess = NULL;
static pfnNtResumeProcess  g_NtResumeProcess  = NULL;

/* ------------------------------------------------------------------ */
/*  Tracking which PIDs we have suspended (idempotent suspend)        */
/* ------------------------------------------------------------------ */
#define MAX_SUSPENDED_PIDS  64
static DWORD  g_suspendedPids[MAX_SUSPENDED_PIDS];
static LONG   g_suspendedPidCount = 0;

/* ------------------------------------------------------------------ */
/*  One-time initialisation called from DllMain / DLL_PROCESS_ATTACH  */
/* ------------------------------------------------------------------ */
static BOOL InitSuspendResume(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL)
        return FALSE;

    g_NtSuspendProcess = (pfnNtSuspendProcess)GetProcAddress(ntdll, "NtSuspendProcess");
    g_NtResumeProcess  = (pfnNtResumeProcess) GetProcAddress(ntdll, "NtResumeProcess");

    return (g_NtSuspendProcess != NULL && g_NtResumeProcess != NULL);
}

typedef struct PROCESS_NODE
{
    DWORD processId;
    DWORD parentProcessId;
    WCHAR imageName[MAX_PATH];
} PROCESS_NODE;

#define MAX_TRACKED_PROCESSES 2048

static volatile LONG g_webHelperDisabled = FALSE;
static HANDLE g_stopEvent = NULL;

static BOOL IsSteamClientProcess(void)
{
    WCHAR modulePath[MAX_PATH] = {};
    LPCWSTR fileName = NULL;

    if (GetModuleFileNameW(NULL, modulePath, ARRAYSIZE(modulePath)) == 0)
        return FALSE;

    fileName = wcsrchr(modulePath, L'\\');
    fileName = (fileName == NULL) ? modulePath : fileName + 1;

    return CompareStringOrdinal(fileName, -1, L"steam.exe", -1, TRUE) == CSTR_EQUAL;
}

static DWORD ReadSteamDwordValue(LPCWSTR subKey, LPCWSTR valueName, DWORD fallbackValue)
{
    DWORD value = fallbackValue;
    DWORD size = sizeof(value);
    LONG status = RegGetValueW(HKEY_CURRENT_USER, subKey, valueName, RRF_RT_REG_DWORD, NULL, &value, &size);

    if (status != ERROR_SUCCESS)
        return fallbackValue;

    return value;
}

static BOOL ReadSteamAppRunning(DWORD appId)
{
    WCHAR subKey[128] = {};
    DWORD running = FALSE;
    DWORD size = sizeof(running);

    wsprintfW(subKey, L"SOFTWARE\\Valve\\Steam\\Apps\\%lu", appId);

    return RegGetValueW(HKEY_CURRENT_USER, subKey, L"Running", RRF_RT_REG_DWORD, NULL, &running, &size) ==
               ERROR_SUCCESS &&
           running != FALSE;
}

static BOOL IsIgnoredChildProcessName(LPCWSTR imageName)
{
    /*
     * Known Steam infrastructure processes that are NOT games.
     * millennium.* processes are the Steam Deck / Steam UI framework
     * (currently also used on desktop Steam).
     */
    static const LPCWSTR kIgnoredNames[] = {
        L"steam.exe",
        L"steamwebhelper.exe",
        L"steamservice.exe",
        L"gameoverlayui.exe",
        L"gameoverlayui64.exe",
        L"crashpad_handler.exe",
        L"steamerrorreporter.exe",
        L"steamerrorreporter64.exe",
        L"millennium.crashhandler64.exe",
        L"millennium.luavm64.exe",
    };
    DWORD index = 0;

    for (; index < ARRAYSIZE(kIgnoredNames); index++)
    {
        if (CompareStringOrdinal(imageName, -1, kIgnoredNames[index], -1, TRUE) == CSTR_EQUAL)
            return TRUE;
    }

    return FALSE;
}

static DWORD SnapshotProcesses(PROCESS_NODE *processes, DWORD capacity)
{
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    PROCESSENTRY32W entry = {};
    DWORD count = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (count >= capacity)
                break;

            processes[count].processId = entry.th32ProcessID;
            processes[count].parentProcessId = entry.th32ParentProcessID;
            lstrcpynW(processes[count].imageName, entry.szExeFile, ARRAYSIZE(processes[count].imageName));
            count++;
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return count;
}

static const PROCESS_NODE *FindProcessNode(const PROCESS_NODE *processes, DWORD count, DWORD processId)
{
    DWORD index = 0;

    for (; index < count; index++)
    {
        if (processes[index].processId == processId)
            return &processes[index];
    }

    return NULL;
}

static BOOL IsDescendantProcess(const PROCESS_NODE *processes, DWORD count, DWORD processId, DWORD ancestorProcessId)
{
    DWORD currentProcessId = processId;
    DWORD depth = 0;

    while (currentProcessId != 0 && depth++ < count)
    {
        const PROCESS_NODE *node = NULL;

        if (currentProcessId == ancestorProcessId)
            return TRUE;

        node = FindProcessNode(processes, count, currentProcessId);
        if (node == NULL || node->parentProcessId == currentProcessId)
            break;

        currentProcessId = node->parentProcessId;
    }

    return FALSE;
}

static BOOL HasLiveGameProcess(const PROCESS_NODE *processes, DWORD count, DWORD steamProcessId)
{
    DWORD index = 0;

    if (processes == NULL || count == 0)
        return FALSE;

    for (; index < count; index++)
    {
        if (processes[index].processId == steamProcessId)
            continue;

        if (!IsDescendantProcess(processes, count, processes[index].processId, steamProcessId))
            continue;

        if (IsIgnoredChildProcessName(processes[index].imageName))
            continue;

        return TRUE;
    }

    return FALSE;
}

static void SetGameProcessPriority(const PROCESS_NODE *processes, DWORD count,
                                   DWORD steamProcessId, DWORD priorityClass)
{
    DWORD index = 0;

    if (processes == NULL || count == 0)
        return;

    for (; index < count; index++)
    {
        HANDLE processHandle = NULL;

        if (processes[index].processId == steamProcessId)
            continue;

        if (!IsDescendantProcess(processes, count, processes[index].processId, steamProcessId))
            continue;

        if (IsIgnoredChildProcessName(processes[index].imageName))
            continue;

        processHandle = OpenProcess(PROCESS_SET_INFORMATION, FALSE, processes[index].processId);
        if (processHandle != NULL)
        {
            SetPriorityClass(processHandle, priorityClass);
            CloseHandle(processHandle);
        }
    }
}

static void SetSteamEfficiencyMode(BOOL enable)
{
    /*
     * SetProcessInformation with ProcessPowerThrottling enables the
     * "Efficiency mode" (EcoQoS) badge in Task Manager and asks the
     * scheduler to deprioritise the process for background operation.
     *
     * Available since Windows 10 (build 1511+).  We load the function
     * dynamically so the DLL degrades gracefully on older systems.
     */
    typedef BOOL(WINAPI *pfnSetProcessInformation)(HANDLE, INT, LPVOID, DWORD);
    static pfnSetProcessInformation g_fn = NULL;

    if (g_fn == NULL)
    {
        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (hKernel32 != NULL)
            g_fn = (pfnSetProcessInformation)GetProcAddress(hKernel32, "SetProcessInformation");
        if (g_fn == NULL)
            return;
    }

    PROCESS_POWER_THROTTLING_STATE ppt;
    ppt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    ppt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    ppt.StateMask   = enable ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;

    g_fn(GetCurrentProcess(), ProcessPowerThrottling, &ppt, sizeof(ppt));
}

static BOOL IsAlreadySuspended(DWORD pid)
{
    LONG i;

    for (i = 0; i < g_suspendedPidCount; i++)
    {
        if (g_suspendedPids[i] == pid)
            return TRUE;
    }

    return FALSE;
}

static void AddSuspendedPid(DWORD pid)
{
    if (g_suspendedPidCount < MAX_SUSPENDED_PIDS)
        g_suspendedPids[g_suspendedPidCount++] = pid;
}

static void SuspendSteamWebHelpers(const PROCESS_NODE *processes, DWORD count, DWORD steamProcessId)
{
    DWORD index = 0;

    if (processes == NULL || count == 0)
        return;

    for (; index < count; index++)
    {
        HANDLE processHandle = NULL;

        if (CompareStringOrdinal(processes[index].imageName, -1, L"steamwebhelper.exe", -1, TRUE) != CSTR_EQUAL)
            continue;

        if (!IsDescendantProcess(processes, count, processes[index].processId, steamProcessId))
            continue;

        if (IsAlreadySuspended(processes[index].processId))
            continue;

        /*
         * Open with both SUSPEND_RESUME (to freeze threads) and
         * SET_QUOTA (to trim the working set / free RAM).
         */
        processHandle = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_SET_QUOTA, FALSE,
                                    processes[index].processId);
        if (processHandle == NULL)
        {
            OutputDebugStringW(L"umpdc: suspend+quota denied, falling back to kill");
            processHandle = OpenProcess(PROCESS_TERMINATE, FALSE, processes[index].processId);
            if (processHandle != NULL)
            {
                TerminateProcess(processHandle, EXIT_SUCCESS);
                CloseHandle(processHandle);
            }
            else
            {
                OutputDebugStringW(L"umpdc: terminate access also denied, skipping");
            }
            continue;
        }

        /* Freeze every thread so CEF doesn't notice */
        if (g_NtSuspendProcess == NULL ||
            !NT_SUCCESS(g_NtSuspendProcess(processHandle)))
        {
            CloseHandle(processHandle);

            processHandle = OpenProcess(PROCESS_TERMINATE, FALSE, processes[index].processId);
            if (processHandle != NULL)
            {
                TerminateProcess(processHandle, EXIT_SUCCESS);
                CloseHandle(processHandle);
            }
            continue;
        }

        /* Trim the working set to zero to free RAM */
        {
            typedef BOOL(WINAPI *pfnSetProcessWorkingSetSize)(HANDLE, SIZE_T, SIZE_T);
            static pfnSetProcessWorkingSetSize g_SetWsSize = NULL;

            if (g_SetWsSize == NULL)
                g_SetWsSize = (pfnSetProcessWorkingSetSize)
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                   "SetProcessWorkingSetSize");

            if (g_SetWsSize != NULL)
                g_SetWsSize(processHandle, (SIZE_T)-1, (SIZE_T)-1);
        }

        AddSuspendedPid(processes[index].processId);
        CloseHandle(processHandle);
    }
}

static void ResumeAllSuspended(void)
{
    LONG i;

    for (i = 0; i < g_suspendedPidCount; i++)
    {
        HANDLE processHandle = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, g_suspendedPids[i]);
        if (processHandle != NULL)
        {
            if (g_NtResumeProcess != NULL)
                g_NtResumeProcess(processHandle);
            CloseHandle(processHandle);
        }
    }

    g_suspendedPidCount = 0;
}

static DWORD WINAPI MonitorThreadProc(LPVOID parameter)
{
    DWORD steamProcessId = GetCurrentProcessId();
    PROCESS_NODE *processes = NULL;
    UNREFERENCED_PARAMETER(parameter);

    if (WaitForSingleObject(g_stopEvent, 5000) != WAIT_TIMEOUT)
        return 0;

    processes = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*processes) * MAX_TRACKED_PROCESSES);

    while (WaitForSingleObject(g_stopEvent, 4000) == WAIT_TIMEOUT)
    {
        DWORD runningAppId = 0;
        BOOL appMarkedRunning = FALSE;
        BOOL liveGameProcess = FALSE;
        DWORD count = 0;
        BOOL shouldDisable;

        /* AUTO mode: detect whether a game is running. */
        runningAppId = ReadSteamDwordValue(L"SOFTWARE\\Valve\\Steam", L"RunningAppID", 0);

        if (runningAppId != 0)
            appMarkedRunning = ReadSteamAppRunning(runningAppId);

        if (processes != NULL)
        {
            count = SnapshotProcesses(processes, MAX_TRACKED_PROCESSES);
            liveGameProcess = HasLiveGameProcess(processes, count, steamProcessId);
        }

        shouldDisable = runningAppId != 0 && appMarkedRunning;

        if (shouldDisable)
        {
            InterlockedExchange(&g_webHelperDisabled, TRUE);
            if (processes != NULL)
            {
                OutputDebugStringW(L"umpdc: game detected, suspending webhelpers");
                SuspendSteamWebHelpers(processes, count, steamProcessId);

                SetGameProcessPriority(processes, count, steamProcessId,
                                       ABOVE_NORMAL_PRIORITY_CLASS);
            }

            SetSteamEfficiencyMode(TRUE);
        }
        else if (InterlockedCompareExchange(&g_webHelperDisabled, FALSE, TRUE))
        {
            OutputDebugStringW(L"umpdc: game ended, resuming webhelpers");
            ResumeAllSuspended();

            if (processes != NULL)
                SetGameProcessPriority(processes, count, steamProcessId,
                                       NORMAL_PRIORITY_CLASS);

            SetSteamEfficiencyMode(FALSE);
        }
    }

    if (processes != NULL)
        HeapFree(GetProcessHeap(), 0, processes);

    ResumeAllSuspended();
    return 0;
}

static HANDLE g_monitorThreadHandle = NULL;

BOOL WINAPI DllMain(HINSTANCE instanceHandle, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instanceHandle);

        if (!IsSteamClientProcess())
            return TRUE;

        InitSuspendResume();

        OutputDebugStringW(L"umpdc: loaded in steam.exe, monitor thread starting");

        g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (g_stopEvent == NULL)
            return TRUE;

        g_monitorThreadHandle = CreateThread(NULL, 0, MonitorThreadProc, instanceHandle, 0, NULL);
        if (g_monitorThreadHandle == NULL)
            return TRUE;

        return TRUE;
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_stopEvent != NULL)
            SetEvent(g_stopEvent);

        if (g_monitorThreadHandle != NULL)
        {
            WaitForSingleObject(g_monitorThreadHandle, 6000);
            CloseHandle(g_monitorThreadHandle);
            g_monitorThreadHandle = NULL;
        }

        ResumeAllSuspended();

        if (g_stopEvent != NULL)
        {
            CloseHandle(g_stopEvent);
            g_stopEvent = NULL;
        }
    }

    return TRUE;
}
