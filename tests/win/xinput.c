// A controller as a Windows game sees it: XInput under Proton, fed by
// /dev/input/event0 (runtime/evdev.c) through winebus/SDL. tests/win/run.sh
// runs it against tests/elf/fake_inputd.py, which toggles A and the left stick.
#include <windows.h>
#include <xinput.h>
#include <stdio.h>

int main(void)
{
    int connected = 0, a_down = 0, a_up = 0, lx_left = 0, rumble = 0, b_down = 0;
    for (int i = 0; i < 300 && !(a_down && a_up && lx_left); i++) {
        XINPUT_STATE s;
        if (XInputGetState(0, &s) == ERROR_SUCCESS) {
            if (!connected) {
                XINPUT_CAPABILITIES caps;
                if (XInputGetCapabilities(0, 0, &caps) == ERROR_SUCCESS)
                    printf("pad 0: type %d subtype %d\n", caps.Type, caps.SubType);
                XINPUT_VIBRATION v = { 40000, 20000 };
                rumble = XInputSetState(0, &v) == ERROR_SUCCESS;
            }
            connected = 1;
            if (s.Gamepad.wButtons & XINPUT_GAMEPAD_A) a_down = 1;
            else if (a_down) a_up = 1;
            if (s.Gamepad.wButtons & XINPUT_GAMEPAD_B) b_down = 1;
            if (s.Gamepad.sThumbLX < -15000) lx_left = 1;
        }
        Sleep(50);
    }
    XINPUT_VIBRATION stop = { 0, 0 };
    XInputSetState(0, &stop);
    printf("connected %d, A down %d, A up %d, left stick %d, rumble %d, B down %d\n",
           connected, a_down, a_up, lx_left, rumble, b_down);
    int ok = connected && a_down && a_up && lx_left;
    printf(ok ? "== xinput probe: ok\n" : "== xinput probe: FAIL\n");
    // Also in probe-result.txt (the working directory): under Steam's launch
    // path the console output goes to Wine's console, not a log.
    FILE *rf = fopen("probe-result.txt", "a");
    if (rf) {
        fprintf(rf, "connected %d, A down %d, A up %d, left stick %d, rumble %d, B down %d\n%s",
                connected, a_down, a_up, lx_left, rumble, b_down,
                ok ? "== xinput probe: ok\n" : "== xinput probe: FAIL\n");
        fclose(rf);
    }
    fflush(stdout);
    return !ok;
}
