// SteamARM pointer tests (tests/x11/run-confine.sh). mousedelta N DX DY [fixed]: posts N mouse-moved events carrying (DX,DY) deltas at the cursor's current location, 8 ms apart; prints the cursor location before and after.
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static CGPoint where(void) { CGEventRef e = CGEventCreate(NULL); CGPoint p = CGEventGetLocation(e); CFRelease(e); return p; }
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 50, dx = argc > 2 ? atoi(argv[2]) : 10, dy = argc > 3 ? atoi(argv[3]) : 0, fixed = argc > 4;
    CGPoint p0 = where();
    for (int i = 0; i < n; i++) {
        CGPoint p = where();
        CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, fixed ? p : CGPointMake(p.x + dx, p.y + dy), kCGMouseButtonLeft);
        CGEventSetIntegerValueField(e, kCGMouseEventDeltaX, dx);
        CGEventSetIntegerValueField(e, kCGMouseEventDeltaY, dy);
        CGEventPost(kCGHIDEventTap, e); CFRelease(e);
        usleep(8000);
    }
    usleep(100000);
    CGPoint p1 = where();
    printf("cursor %.0f,%.0f -> %.0f,%.0f after %d moves of %d,%d\n", p0.x, p0.y, p1.x, p1.y, n, dx, dy);
    return 0;
}
