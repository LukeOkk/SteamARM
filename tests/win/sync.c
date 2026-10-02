// Windows synchronization the way games use it, under whichever backend the
// prefix runs with (wineserver, fsync, NTSYNC: tests/win/run.sh sync_*):
// events between two threads (and how long a round trip takes),
// WaitForMultipleObjects of all objects, a mutex abandoned by its thread, a
// semaphore shared by four threads; then two processes, as services.exe
// starts a service: an overlapped ConnectNamedPipe waited together with the
// child's process handle (the pipe must win while the child lives), two
// overlapped reads of what the child writes, and the process handle when it
// exits. Prints the numbers and "== sync probe: ok" when every check holds.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PIPE_NAME "\\\\.\\pipe\\steamarm-sync-probe"

// The child: connects after half a second, writes twice, lives 1.5 s more.
static int child(void)
{
    Sleep(500);
    HANDLE p = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (p == INVALID_HANDLE_VALUE)
        return 3;
    DWORD n = 0;
    WriteFile(p, "one!", 4, &n, NULL);
    Sleep(500);
    WriteFile(p, "two!", 4, &n, NULL);
    Sleep(1000);
    CloseHandle(p);
    return 7;
}

static HANDLE ping, pong;
static volatile LONG rounds = 20000;

static DWORD WINAPI ponger(void *arg)
{
    for (LONG i = 0; i < rounds; i++) {
        if (WaitForSingleObject(ping, 5000) != WAIT_OBJECT_0)
            return 1;
        SetEvent(pong);
    }
    return 0;
}

static HANDLE mtx;
static DWORD WINAPI abandoner(void *arg)
{
    WaitForSingleObject(mtx, INFINITE);
    return 0;                                   // exits holding it
}

static HANDLE sem;
static volatile LONG inside, most;
static DWORD WINAPI worker(void *arg)
{
    for (int i = 0; i < 200; i++) {
        if (WaitForSingleObject(sem, 5000) != WAIT_OBJECT_0)
            return 1;
        LONG n = InterlockedIncrement(&inside);
        LONG m;
        while (n > (m = most) && InterlockedCompareExchange(&most, n, m) != m)
            ;
        Sleep(0);
        InterlockedDecrement(&inside);
        ReleaseSemaphore(sem, 1, NULL);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child"))
        return child();
    setvbuf(stdout, NULL, _IONBF, 0);            // what was reached shows even if a wait never returns
    int ok = 1;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);

    ping = CreateEventA(NULL, FALSE, FALSE, NULL);
    pong = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, ponger, NULL, 0, NULL);
    QueryPerformanceCounter(&t0);
    LONG done = 0;
    for (; done < rounds; done++) {
        SetEvent(ping);
        if (WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0)
            break;
    }
    QueryPerformanceCounter(&t1);
    WaitForSingleObject(th, 5000);
    double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart / (done ? done : 1);
    printf("event ping-pong: %ld round trips, %.1f us each\n", done, us);
    if (done != rounds) ok = 0;

    HANDLE e1 = CreateEventA(NULL, TRUE, TRUE, NULL), e2 = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE s1 = CreateSemaphoreA(NULL, 1, 1, NULL);
    HANDLE all[3] = { e1, e2, s1 };
    DWORD r1 = WaitForMultipleObjects(3, all, TRUE, 50);
    LONG prev = -1;
    BOOL s_untouched = ReleaseSemaphore(s1, 1, &prev) == FALSE;   // still 1 of 1: release fails
    SetEvent(e2);
    DWORD r2 = WaitForMultipleObjects(3, all, TRUE, 1000);
    BOOL s_taken = ReleaseSemaphore(s1, 1, &prev) && prev == 0;
    printf("wait all: %s while one is unsignaled (semaphore %s), %s once all are\n",
           r1 == WAIT_TIMEOUT ? "times out" : "WRONG", s_untouched ? "untouched" : "TAKEN",
           r2 == WAIT_OBJECT_0 && s_taken ? "takes all" : "WRONG");
    if (r1 != WAIT_TIMEOUT || !s_untouched || r2 != WAIT_OBJECT_0 || !s_taken) ok = 0;

    mtx = CreateMutexA(NULL, FALSE, NULL);
    th = CreateThread(NULL, 0, abandoner, NULL, 0, NULL);
    WaitForSingleObject(th, 5000);
    DWORD r3 = WaitForSingleObject(mtx, 1000);
    printf("abandoned mutex: %s\n", r3 == WAIT_ABANDONED ? "WAIT_ABANDONED" : "WRONG");
    if (r3 != WAIT_ABANDONED) ok = 0;
    ReleaseMutex(mtx);

    sem = CreateSemaphoreA(NULL, 2, 2, NULL);
    HANDLE w[4];
    for (int i = 0; i < 4; i++) w[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    DWORD r4 = WaitForMultipleObjects(4, w, TRUE, 20000);
    DWORD codes = 0;
    for (int i = 0; i < 4; i++) { DWORD c = 1; GetExitCodeThread(w[i], &c); codes |= c; }
    printf("semaphore of 2 shared by 4 threads: at most %ld inside, %s\n", most,
           r4 == WAIT_OBJECT_0 && !codes ? "all finished" : "WRONG");
    if (r4 != WAIT_OBJECT_0 || codes || most > 2) ok = 0;

    // Two processes.
    {
        HANDLE pipe = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_WAIT, 1, 256, 256, 10000, NULL);
        OVERLAPPED ov = {0};
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        char self[MAX_PATH], cmd[MAX_PATH + 16];
        GetModuleFileNameA(NULL, self, sizeof self);
        snprintf(cmd, sizeof cmd, "\"%s\" child", self);
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi = {0};
        DWORD t0 = GetTickCount();
        BOOL started = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        printf("two processes: child %s\n", started ? "started" : "NOT started");
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD err = connected ? 0 : GetLastError();
        HANDLE hs[2] = {ov.hEvent, pi.hProcess};
        DWORD w1 = err == ERROR_IO_PENDING ? WaitForMultipleObjects(2, hs, FALSE, 10000)
                                           : (err == ERROR_PIPE_CONNECTED || connected ? WAIT_OBJECT_0 : WAIT_FAILED);
        DWORD t1 = GetTickCount() - t0;
        printf("two processes: connect wait returned %lu after %lu ms\n", w1, t1);
        char buf[2][8] = {{0}, {0}};
        DWORD got[2] = {0, 0}, st[2] = {0, 0};
        for (int i = 0; i < 2; i++) {
            ResetEvent(ov.hEvent);
            if (!ReadFile(pipe, buf[i], 4, &got[i], &ov)) {
                if (GetLastError() == ERROR_IO_PENDING) {
                    WaitForSingleObject(ov.hEvent, 10000);
                    if (!GetOverlappedResult(pipe, &ov, &got[i], FALSE))
                        st[i] = GetLastError();
                } else
                    st[i] = GetLastError();
            }
        }
        DWORD t2 = GetTickCount() - t0;
        printf("two processes: reads done after %lu ms\n", t2);
        DWORD w2 = WaitForSingleObject(pi.hProcess, 10000), code = 0;
        DWORD t3 = GetTickCount() - t0;
        GetExitCodeProcess(pi.hProcess, &code);
        int good = started && w1 == WAIT_OBJECT_0 && t1 >= 300 && t1 < 5000 &&
                   got[0] == 4 && got[1] == 4 && !memcmp(buf[0], "one!", 4) && !memcmp(buf[1], "two!", 4) &&
                   w2 == WAIT_OBJECT_0 && code == 7 && t3 >= 1500;
        printf("two processes: pipe connect won the wait %s (%lu, after %lu ms), reads %lu+%lu bytes (errors %lu, %lu) by %lu ms, "
               "child ended %lu ms with code %lu: %s\n", w1 == WAIT_OBJECT_0 ? "yes" : "NO", w1, t1, got[0], got[1], st[0], st[1],
               t2, t3, code, good ? "right" : "WRONG");
        if (!good) ok = 0;
    }

    printf("== sync probe: %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
