// The registry survives a session that opened a window. wineserver interrupts
// client threads with a thread-directed SIGUSR1 (tgkill to another process);
// when that failed with ESRCH, a named pipe's async was used after free and
// wineserver aborted while saving the registry at the end of every such
// session, losing its changes (runtime/signal.c, stage 16). tests/win/run.sh
// checks that the value this probe writes is in the prefix's user.reg once
// wineserver has exited.
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv){
  HKEY k; DWORD d; const char *name = argc > 1 ? argv[1] : "SteamARMRegWin";
  LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\SteamARMRegWin", 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &d);
  if (!r) { r = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE*)"1", 2); RegCloseKey(k); }
  HWND w = CreateWindowA("STATIC", "regwin", WS_OVERLAPPEDWINDOW|WS_VISIBLE, 50, 50, 200, 100, 0, 0, 0, 0);
  MSG m; for (int i = 0; i < 50; i++) { while (PeekMessageA(&m,0,0,0,PM_REMOVE)) DispatchMessageA(&m); Sleep(20); }
  DestroyWindow(w);
  printf("regwin %s set=%ld window=%p\n", name, r, (void*)w); return 0; }
