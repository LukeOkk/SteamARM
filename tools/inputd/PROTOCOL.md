# Controllers for guest programs: steamarm-inputd ↔ runtime

No VM means no Linux kernel, so no `/dev/input`. Games under Proton find
controllers through SDL (winebus) or evdev, and both expect kernel evdev
devices. The Mac side reads the real controllers (SDL2 → GameController /
IOKit); the runtime shows the guest `/dev/input/eventN` nodes that behave like
the ones the Linux drivers (xpad, hid-playstation, hid-sony, hid-nintendo,
hid-steam) create. Games therefore see the controller type the user picked in
the launcher, with its USB vendor/product ids and the matching button prompts.

## Directory

`/tmp/lxrt-input/` (fixed path, like `/tmp/lxrt-sig`; created by the daemon,
mode 0700). The guest path `/dev/input` names it.

    /tmp/lxrt-input/eventN        Unix stream socket, one per virtual device
    /tmp/lxrt-input/meta/eventN   its description (text, below)
    /tmp/lxrt-input/inputd.pid    the daemon's pid

N = player index 0..3 (player 1 = event0). The daemon writes `meta/eventN`
(atomically: write to a temporary name, rename) BEFORE it binds `eventN`, and
removes `eventN` before `meta/eventN` when the device goes away. Creating and
removing the socket is the hotplug signal (guests watch the directory with
inotify).

## meta/eventN

One item per line, space-separated, decimal unless noted:

    name <rest of line>                 e.g. name Microsoft X-Box 360 pad
    id <bustype> <vendor> <product> <version>   hex, e.g. id 0003 045e 028e 0114
    phys <rest of line>                 e.g. phys usb-steamarm-0/input0
    uniq <rest of line>                 may be empty
    prop <bit> ...                      INPUT_PROP_* bits (may be absent)
    key <code> ...                      EV_KEY codes the device has
    abs <code> <min> <max> <fuzz> <flat> <resolution>   one line per axis
    ff <bit> ...                        EV_FF bits (FF_RUMBLE=80 ...)
    effects <n>                         simultaneous FF effects (EVIOCGEFFECTS)

EV bits are implied: EV_SYN always, EV_KEY if any key, EV_ABS if any abs,
EV_FF if any ff.

## Stream daemon → guest

Linux `struct input_event`, 24 bytes, little endian (x86-64 and aarch64
layout): `int64 tv_sec; int64 tv_usec; uint16 type; uint16 code; int32
value`. Every frame ends with `EV_SYN/SYN_REPORT` (type 0, code 0, value 0).
The daemon writes a whole frame with one write().

Right after accepting a connection the daemon sends a snapshot of the current
state (every abs axis value, every pressed key) followed by SYN_REPORT. The
runtime consumes that snapshot at open to answer EVIOCGABS / EVIOCGKEY and
does not pass it to the guest (a Linux open delivers no events either).

## Stream guest → daemon

24-byte records:

    uint32 magic  = 0x4c424d52 ("RMBL")
    uint16 strong   rumble magnitude 0..65535 (low-frequency motor)
    uint16 weak     rumble magnitude 0..65535 (high-frequency motor)
    uint32 duration_ms   0 = until the next record
    uint32 reserved[3]

The runtime sends one when the guest plays or stops an FF effect (write of an
EV_FF event after EVIOCSFF). strong = weak = 0 stops. The daemon scales by the
player's rumble strength and ignores it when rumble is off in the launcher.

## Device layouts (what the Linux drivers expose)

Codes: BTN_SOUTH/A 304, BTN_EAST/B 305, BTN_C 306, BTN_NORTH/X 307,
BTN_WEST/Y 308, BTN_Z 309, BTN_TL 310, BTN_TR 311, BTN_TL2 312, BTN_TR2 313,
BTN_SELECT 314, BTN_START 315, BTN_MODE 316, BTN_THUMBL 317, BTN_THUMBR 318,
BTN_THUMB 289, BTN_THUMB2 290, BTN_GEAR_DOWN 336, BTN_GEAR_UP 337,
BTN_DPAD_UP 544, _DOWN 545, _LEFT 546, _RIGHT 547, KEY_RECORD 167,
BTN_TRIGGER_HAPPY1..8 704..711, BTN_BASE 294, BTN_GRIPL 548, BTN_GRIPR 549,
BTN_GRIPL2 550, BTN_GRIPR2 551. ABS_X 0, ABS_Y 1, ABS_Z 2, ABS_RX 3,
ABS_RY 4, ABS_RZ 5, ABS_HAT0X 16, ABS_HAT0Y 17, ABS_HAT1X 18, ABS_HAT1Y 19,
ABS_HAT2X 20, ABS_HAT2Y 21.
Y axes grow downwards (as SDL's). FF: FF_RUMBLE 80, FF_PERIODIC 81,
FF_SQUARE 88, FF_TRIANGLE 89, FF_SINE 90, FF_GAIN 96.

Note the Xbox driver's historical codes: X (west) is 307 and Y (north) is
308; the PlayStation/Nintendo drivers are positional: north 307, west 308.

| type | name | id (bus vendor product version) |
|---|---|---|
| xbox360 | Microsoft X-Box 360 pad | 0003 045e 028e 0114 |
| xboxone | Microsoft X-Box One S pad | 0003 045e 02ea 0408 |
| xboxseries | Microsoft Xbox Series S\|X Controller | 0003 045e 0b12 0507 |
| xboxelite2 | Microsoft X-Box One Elite 2 pad | 0003 045e 0b00 0511 |
| ds3 | Sony PLAYSTATION(R)3 Controller | 0003 054c 0268 8111 |
| ds4 | Sony Interactive Entertainment Wireless Controller | 0003 054c 09cc 8111 |
| dualsense | Sony Interactive Entertainment DualSense Wireless Controller | 0003 054c 0ce6 8111 |
| dualsenseedge | Sony Interactive Entertainment DualSense Edge Wireless Controller | 0003 054c 0df2 8111 |
| steamcontroller | Valve Software Steam Controller | 0003 28de 1102 0111 |
| switchpro | Nintendo Switch Pro Controller | 0003 057e 2009 8111 |
| steamcontroller2 | Steam Controller | 0003 28de 1302 0100 |

Xbox family (xpad): keys 304 A(south) 305 B(east) 307 X(west) 308 Y(north)
310 LB 311 RB 314 View/Back 315 Menu/Start 316 Guide 317 L3 318 R3;
xboxseries adds 167 (Share); xboxelite2 adds 708..711 (paddles P1..P4).
abs X Y RX RY -32768..32767 fuzz 16 flat 128; Z RZ (triggers) 0..255 for
xbox360, 0..1023 for the others; HAT0X HAT0Y -1..1.
ff 80 81 88 89 90 96, effects 16.

PlayStation DS4 / DualSense / Edge (hid-playstation): keys 304 ✕ 305 ○ 307 △
308 □ 310 L1 311 R1 312 L2 313 R2 314 Share/Create 315 Options 316 PS 317 L3
318 R3; abs X Y RX RY 0..255 (centre 128), Z RZ 0..255, HAT0X HAT0Y -1..1;
ff 80, effects 16.

DS3 (hid-sony): same keys plus the D-pad as 544..547 (no hat); abs X Y RX RY
0..255, Z RZ 0..255; ff 80.

Switch Pro (hid-nintendo): keys 304 B(south) 305 A(east) 307 X(north)
308 Y(west) 310 L 311 R 312 ZL 313 ZR 314 − 315 + 316 Home 309 Capture
317 L3 318 R3; abs X Y RX RY -32767..32767 fuzz 250 flat 500, HAT0X HAT0Y
-1..1; ff 80.

Steam Controller (hid-steam): keys 304 A 305 B 307 X 308 Y 310 LB 311 RB
312 LT(click) 313 RT(click) 314 Back 315 Start 316 Steam 317 stick click
318 right-pad click 289 left-pad touch 290 right-pad touch 336 left grip
337 right grip 544..547 left-pad D-pad; abs X Y (stick) -32767..32767,
RX RY (right pad) -32767..32767, HAT0X HAT0Y (left pad) -32767..32767,
HAT2Y (left trigger) HAT2X (right trigger) 0..255; ff 80.

Steam Controller (2026) (hid-steam "IBEX", Linux 7.3 patches: wired 28de:1302,
BLE 1303, wireless puck 1304): keys 304 A 305 B 307 X 308 Y 310 LB 311 RB
312 LT(full press) 313 RT(full press) 314 View 315 Menu 316 Steam 294 quick
access 317 L3 318 R3 548 L4 (upper left grip) 549 R4 550 L5 (lower left)
551 R5 289 left pad click 290 right pad click 544..547 D-pad; abs X Y RX RY
-32767..32767, HAT0X/Y (left pad) HAT1X/Y (right pad) -32767..32767 (0 when
not touched; the daemon has no finger positions to forward, so they rest at
0), HAT2Y (left trigger) HAT2X (right trigger) 0..32767; ff 80. The
bcdDevice/version the real device reports is not known here; 0100 is used.
