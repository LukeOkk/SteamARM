use serde_json::Value as Json;
use std::env;
use std::ffi::{c_char, c_int, c_short, c_void, CStr, CString};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

const PLAYERS: usize = 4;
const CLIENTS: usize = 32;
const VALUES: usize = 48;
const RX_BYTES: usize = 24;
const SLOTS: usize = 30;

const A: usize = 0;
const B: usize = 1;
const X: usize = 2;
const Y: usize = 3;
const L: usize = 4;
const R: usize = 5;
const ZL: usize = 6;
const ZR: usize = 7;
const MINUS: usize = 8;
const PLUS: usize = 9;
const HOME: usize = 10;
const L3: usize = 11;
const R3: usize = 12;
const UP: usize = 13;
const DOWN: usize = 14;
const LEFT: usize = 15;
const RIGHT: usize = 16;
const CAPTURE: usize = 17;
const P1: usize = 18;
const LGRIP: usize = 22;
const RGRIP: usize = 23;
const LGRIP2: usize = 24;
const RGRIP2: usize = 25;
const LPAD: usize = 26;
const RPAD: usize = 27;
const LSTICK: usize = 28;

const SDL_INIT_EVENTS: u32 = 0x0000_4000;
const SDL_INIT_GAMECONTROLLER: u32 = 0x0000_2000;
const SDL_CONTROLLERDEVICEADDED: u32 = 0x653;
const SDL_CONTROLLERDEVICEREMOVED: u32 = 0x654;
const SDL_CONTROLLER_AXIS_INVALID: c_int = -1;
const SDL_CONTROLLER_BUTTON_INVALID: c_int = -1;
const SDL_CONTROLLER_AXIS_TRIGGERLEFT: c_int = 4;
const SDL_CONTROLLER_AXIS_MAX: usize = 6;
const SDL_CONTROLLER_BUTTON_MAX: usize = 21;
const SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS: &[u8] = b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS\0";
const SDL_HINT_JOYSTICK_HIDAPI: &[u8] = b"SDL_JOYSTICK_HIDAPI\0";

const POLLIN: c_short = 0x0001;
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;
const O_NOFOLLOW: c_int = 0x100;
const SIGINT: c_int = 2;
const SIGTERM: c_int = 15;
const SIGPIPE: c_int = 13;
const SIG_IGN: usize = 1;
const SIG_ERR: usize = usize::MAX;

type SdlGameController = std::ffi::c_void;
type SdlJoystickId = c_int;

#[repr(C)]
#[derive(Clone, Copy)]
struct SdlJoystickGuid {
    data: [u8; 16],
}

#[repr(C, align(8))]
struct SdlEvent {
    bytes: [u8; 56],
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

#[link(name = "SDL2")]
extern "C" {
    fn SDL_Init(flags: u32) -> c_int;
    fn SDL_Quit();
    fn SDL_SetHint(name: *const c_char, value: *const c_char) -> c_int;
    fn SDL_GetError() -> *const c_char;
    fn SDL_GetTicks64() -> u64;
    fn SDL_PumpEvents();
    fn SDL_PollEvent(event: *mut SdlEvent) -> c_int;
    fn SDL_NumJoysticks() -> c_int;
    fn SDL_IsGameController(index: c_int) -> c_int;
    fn SDL_JoystickGetDeviceGUID(index: c_int) -> SdlJoystickGuid;
    fn SDL_JoystickGetGUIDString(guid: SdlJoystickGuid, text: *mut c_char, size: c_int);
    fn SDL_JoystickGetDeviceInstanceID(index: c_int) -> SdlJoystickId;
    fn SDL_GameControllerOpen(index: c_int) -> *mut SdlGameController;
    fn SDL_GameControllerClose(controller: *mut SdlGameController);
    fn SDL_GameControllerGetAttached(controller: *mut SdlGameController) -> c_int;
    fn SDL_GameControllerGetAxisFromString(name: *const c_char) -> c_int;
    fn SDL_GameControllerGetButtonFromString(name: *const c_char) -> c_int;
    fn SDL_GameControllerGetAxis(controller: *mut SdlGameController, axis: c_int) -> i16;
    fn SDL_GameControllerGetButton(controller: *mut SdlGameController, button: c_int) -> u8;
    fn SDL_GameControllerRumble(
        controller: *mut SdlGameController,
        low_frequency: u16,
        high_frequency: u16,
        duration_ms: u32,
    ) -> c_int;
    fn SDL_GameControllerSetLED(controller: *mut SdlGameController, r: u8, g: u8, b: u8) -> c_int;
}

#[link(name = "proc")]
extern "C" {
    fn proc_pidpath(pid: c_int, buffer: *mut c_void, buffersize: u32) -> c_int;
}

extern "C" {
    fn poll(fds: *mut PollFd, nfds: u32, timeout: c_int) -> c_int;
    fn flock(fd: c_int, operation: c_int) -> c_int;
    fn fork() -> c_int;
    fn setsid() -> c_int;
    fn dup2(oldfd: c_int, newfd: c_int) -> c_int;
    fn kill(pid: c_int, sig: c_int) -> c_int;
    fn signal(sig: c_int, handler: usize) -> usize;
}

static STOPPING: AtomicBool = AtomicBool::new(false);

extern "C" fn stop_handler(_: c_int) {
    STOPPING.store(true, Ordering::Relaxed);
}

fn log_error(context: &str, error: impl std::fmt::Display) {
    eprintln!("{context}: {error}");
}

fn sdl_error() -> String {
    unsafe {
        let p = SDL_GetError();
        if p.is_null() {
            "unknown SDL error".to_owned()
        } else {
            CStr::from_ptr(p).to_string_lossy().into_owned()
        }
    }
}

fn clamp(x: f64, low: f64, high: f64) -> f64 {
    if x < low {
        low
    } else if x > high {
        high
    } else {
        x
    }
}

const SLOT_NAMES: [&str; SLOTS] = [
    "a",
    "b",
    "x",
    "y",
    "l",
    "r",
    "zl",
    "zr",
    "minus",
    "plus",
    "home",
    "l3",
    "r3",
    "dpadUp",
    "dpadDown",
    "dpadLeft",
    "dpadRight",
    "capture",
    "paddle1",
    "paddle2",
    "paddle3",
    "paddle4",
    "leftGrip",
    "rightGrip",
    "leftGrip2",
    "rightGrip2",
    "leftPad",
    "rightPad",
    "leftStick",
    "rightStick",
];

const DEFAULT_MAP: [&str; SLOTS] = [
    "a",
    "b",
    "x",
    "y",
    "leftshoulder",
    "rightshoulder",
    "lefttrigger",
    "righttrigger",
    "back",
    "start",
    "guide",
    "leftstick",
    "rightstick",
    "dpup",
    "dpdown",
    "dpleft",
    "dpright",
    "misc1",
    "paddle1",
    "paddle2",
    "paddle3",
    "paddle4",
    "paddle2",
    "paddle1",
    "paddle4",
    "paddle3",
    "touchpad",
    "rightstick",
    "left",
    "right",
];

#[derive(Clone)]
struct Config {
    guid: String,
    name: String,
    led: String,
    mapping: [String; SLOTS],
    layout: usize,
    deadzone: [f64; 2],
    range: [f64; 2],
    threshold: f64,
    strength: f64,
    invert: [bool; 4],
    rotate: [bool; 2],
    rumble: bool,
    motion: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            guid: String::new(),
            name: String::new(),
            led: String::new(),
            mapping: std::array::from_fn(|i| DEFAULT_MAP[i].to_owned()),
            layout: 2,
            deadzone: [0.1; 2],
            range: [1.0; 2],
            threshold: 0.5,
            strength: 1.0,
            invert: [false; 4],
            rotate: [false; 2],
            rumble: true,
            motion: false,
        }
    }
}

#[derive(Clone, Copy)]
struct Layout {
    kind: &'static str,
    name: &'static str,
    id: &'static str,
}

const LAYOUTS: [Layout; 11] = [
    Layout {
        kind: "xbox360",
        name: "Microsoft X-Box 360 pad",
        id: "0003 045e 028e 0114",
    },
    Layout {
        kind: "xboxone",
        name: "Microsoft X-Box One S pad",
        id: "0003 045e 02ea 0408",
    },
    Layout {
        kind: "xboxseries",
        name: "Microsoft Xbox Series S|X Controller",
        id: "0003 045e 0b12 0507",
    },
    Layout {
        kind: "xboxelite2",
        name: "Microsoft X-Box One Elite 2 pad",
        id: "0003 045e 0b00 0511",
    },
    Layout {
        kind: "ds3",
        name: "Sony PLAYSTATION(R)3 Controller",
        id: "0003 054c 0268 8111",
    },
    Layout {
        kind: "ds4",
        name: "Sony Interactive Entertainment Wireless Controller",
        id: "0003 054c 09cc 8111",
    },
    Layout {
        kind: "dualsense",
        name: "Sony Interactive Entertainment DualSense Wireless Controller",
        id: "0003 054c 0ce6 8111",
    },
    Layout {
        kind: "dualsenseedge",
        name: "Sony Interactive Entertainment DualSense Edge Wireless Controller",
        id: "0003 054c 0df2 8111",
    },
    Layout {
        kind: "steamcontroller",
        name: "Valve Software Steam Controller",
        id: "0003 28de 1102 0111",
    },
    Layout {
        kind: "switchpro",
        name: "Nintendo Switch Pro Controller",
        id: "0003 057e 2009 8111",
    },
    Layout {
        kind: "steamcontroller2",
        name: "Steam Controller",
        id: "0003 28de 1302 0100",
    },
];

fn layout_id(kind: &str) -> Option<usize> {
    match kind {
        "Xbox" => return Some(2),
        "ProController" => return Some(9),
        _ => {}
    }
    LAYOUTS.iter().position(|layout| layout.kind == kind)
}

fn string_value<'a>(obj: &'a Json, key: &str) -> Option<&'a str> {
    obj.get(key).and_then(Json::as_str)
}

fn truncated(value: &str, max_bytes: usize) -> String {
    let mut end = value.len().min(max_bytes);
    while !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].to_owned()
}

fn assign_string(target: &mut String, obj: &Json, key: &str, capacity: usize) {
    if let Some(value) = string_value(obj, key) {
        *target = truncated(value.split('\0').next().unwrap_or_default(), capacity - 1);
    }
}

fn json_number(value: Option<&Json>, fallback: f64) -> f64 {
    match value {
        Some(Json::Number(n)) => n.as_f64().unwrap_or(fallback),
        Some(Json::Bool(true)) => 1.0,
        Some(Json::Bool(false)) => 0.0,
        _ => fallback,
    }
}

fn config_from_json(player: &Json) -> Config {
    let mut config = Config::default();
    assign_string(&mut config.guid, player, "deviceGUID", 64);
    assign_string(&mut config.name, player, "deviceName", 256);
    assign_string(&mut config.led, player, "ledColor", 32);
    let kind = string_value(player, "controllerType").unwrap_or("xboxseries");
    if let Some(id) = layout_id(kind) {
        config.layout = id;
    }
    if let Some(mapping) = player.get("mapping") {
        for (i, key) in SLOT_NAMES.iter().enumerate() {
            if let Some(value) = mapping.get(key).and_then(Json::as_str) {
                config.mapping[i] = truncated(value.split('\0').next().unwrap_or_default(), 63);
            }
        }
    }
    let deadzones = ["deadzoneLeft", "deadzoneRight"];
    let ranges = ["rangeLeft", "rangeRight"];
    let rotations = ["rotateL", "rotateR"];
    let inversions = ["invertLX", "invertLY", "invertRX", "invertRY"];
    for i in 0..2 {
        config.deadzone[i] = clamp(json_number(player.get(deadzones[i]), 0.1), 0.0, 1.0);
        config.range[i] = clamp(json_number(player.get(ranges[i]), 1.0), 0.0, 100.0);
        config.rotate[i] = json_number(player.get(rotations[i]), 0.0) != 0.0;
    }
    for i in 0..4 {
        config.invert[i] = json_number(player.get(inversions[i]), 0.0) != 0.0;
    }
    config.threshold = clamp(json_number(player.get("triggerThreshold"), 0.5), 0.0, 1.0);
    config.strength = clamp(json_number(player.get("rumbleStrength"), 1.0), 0.0, 1.0);
    config.rumble = json_number(player.get("rumble"), 1.0) != 0.0;
    config.motion = json_number(player.get("motion"), 0.0) != 0.0;
    config
}

#[derive(Clone, Copy)]
struct Value {
    kind: i32,
    code: i32,
    min: i32,
    max: i32,
    fuzz: i32,
    flat: i32,
    value: i32,
    slot: usize,
}

impl Value {
    fn new(kind: i32, code: i32, slot: usize, min: i32, max: i32, fuzz: i32, flat: i32) -> Self {
        Self {
            kind,
            code,
            min,
            max,
            fuzz,
            flat,
            value: 0,
            slot,
        }
    }
}

#[derive(Default)]
struct Client {
    stream: Option<UnixStream>,
    rx: [u8; RX_BYTES],
    used: usize,
}

struct Device {
    listener: Option<UnixListener>,
    controller: *mut SdlGameController,
    instance: SdlJoystickId,
    config: Config,
    values: Vec<Value>,
    clients: [Client; CLIENTS],
    raw_axes: [i16; SDL_CONTROLLER_AXIS_MAX],
    raw_buttons: [u8; SDL_CONTROLLER_BUTTON_MAX],
    strong: u16,
    weak: u16,
    renew: u64,
}

impl Device {
    fn new() -> Self {
        Self {
            listener: None,
            controller: std::ptr::null_mut(),
            instance: -1,
            config: Config::default(),
            values: Vec::with_capacity(VALUES),
            clients: std::array::from_fn(|_| Client::default()),
            raw_axes: [0; SDL_CONTROLLER_AXIS_MAX],
            raw_buttons: [0; SDL_CONTROLLER_BUTTON_MAX],
            strong: 0,
            weak: 0,
            renew: 0,
        }
    }
}

#[derive(Clone)]
struct ScriptEvent {
    ms: u64,
    slot: String,
    value: i32,
}

struct Daemon {
    directory: PathBuf,
    config_path: PathBuf,
    configs: [Config; PLAYERS],
    devices: [Device; PLAYERS],
    fake_count: usize,
    script: Vec<ScriptEvent>,
    script_index: usize,
    script_start: u64,
    script_started: bool,
    pid_file: Option<File>,
    sdl_initialized: bool,
}

impl Daemon {
    fn new() -> Self {
        let state = default_state_root();
        let config_path = state.join("launcher/controllers.json");
        Self {
            directory: PathBuf::from("/tmp/lxrt-input"),
            config_path,
            configs: std::array::from_fn(|_| Config::default()),
            devices: std::array::from_fn(|_| Device::new()),
            fake_count: 0,
            script: Vec::new(),
            script_index: 0,
            script_start: 0,
            script_started: false,
            pid_file: None,
            sdl_initialized: false,
        }
    }

    fn load_config(&mut self) -> bool {
        let data = match fs::read(&self.config_path) {
            Ok(data) if data.len() <= 262_144 => data,
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                self.configs = std::array::from_fn(|_| Config::default());
                return true;
            }
            _ => {
                eprintln!("invalid controllers JSON; retaining configuration");
                return false;
            }
        };
        let root: Json = match serde_json::from_slice::<Json>(&data) {
            Ok(root) if root.is_object() => root,
            _ => {
                eprintln!("invalid controllers JSON; retaining configuration");
                return false;
            }
        };
        let mut next: [Config; PLAYERS] = std::array::from_fn(|_| Config::default());
        if let Some(players) = root.get("players").and_then(Json::as_array) {
            for (index, player) in players.iter().take(PLAYERS).enumerate() {
                next[index] = config_from_json(player);
            }
        }
        self.configs = next;
        true
    }

    fn path_for(&self, prefix: &str, index: usize) -> PathBuf {
        self.directory.join(format!("{prefix}{index}"))
    }

    fn make_layout(&mut self, player: usize) {
        let d = &mut self.devices[player];
        let layout = d.config.layout;
        let playstation = (4..=7).contains(&layout);
        d.values.clear();
        let slots = [A, B, X, Y, L, R, MINUS, PLUS, HOME, L3, R3];
        let codes = [304, 305, 307, 308, 310, 311, 314, 315, 316, 317, 318];
        for (i, (&slot, &code)) in slots.iter().zip(codes.iter()).enumerate() {
            let button_code = if (playstation || layout == 9) && (i == 2 || i == 3) {
                if i == 2 {
                    308
                } else {
                    307
                }
            } else {
                code
            };
            d.values.push(Value::new(1, button_code, slot, 0, 1, 0, 0));
        }
        if layout >= 4 {
            d.values.push(Value::new(1, 312, ZL, 0, 1, 0, 0));
            d.values.push(Value::new(1, 313, ZR, 0, 1, 0, 0));
        }
        if layout == 2 || layout == 9 || layout == 10 {
            let code = if layout == 2 {
                167
            } else if layout == 9 {
                309
            } else {
                294
            };
            d.values.push(Value::new(1, code, CAPTURE, 0, 1, 0, 0));
        }
        if layout == 3 {
            for i in 0..4 {
                d.values
                    .push(Value::new(1, 708 + i, P1 + i as usize, 0, 1, 0, 0));
            }
        }
        if layout == 8 {
            for (code, slot) in [(289, LPAD), (290, RPAD), (336, LGRIP), (337, RGRIP)] {
                d.values.push(Value::new(1, code, slot, 0, 1, 0, 0));
            }
        }
        if layout == 10 {
            for (code, slot) in [
                (548, LGRIP),
                (549, RGRIP),
                (550, LGRIP2),
                (551, RGRIP2),
                (289, LPAD),
                (290, RPAD),
            ] {
                d.values.push(Value::new(1, code, slot, 0, 1, 0, 0));
            }
        }
        if layout == 4 || layout == 8 || layout == 10 {
            for i in 0..4 {
                d.values
                    .push(Value::new(1, 544 + i, UP + i as usize, 0, 1, 0, 0));
            }
        }
        let axes = [0, 1, 3, 4];
        for i in 0..4 {
            let (min, max) = if playstation {
                (0, 255)
            } else if layout < 4 {
                (-32768, 32767)
            } else {
                (-32767, 32767)
            };
            let (fuzz, flat) = if layout < 4 {
                (16, 128)
            } else if layout == 9 {
                (250, 500)
            } else {
                (0, 0)
            };
            d.values
                .push(Value::new(3, axes[i], 100 + i, min, max, fuzz, flat));
        }
        if layout != 9 {
            let top = if layout == 10 {
                32767
            } else if layout > 0 && layout < 4 {
                1023
            } else {
                255
            };
            let axis_l = if layout == 8 || layout == 10 { 21 } else { 2 };
            let axis_r = if layout == 8 || layout == 10 { 20 } else { 5 };
            d.values.push(Value::new(3, axis_l, ZL, 0, top, 0, 0));
            d.values.push(Value::new(3, axis_r, ZR, 0, top, 0, 0));
        }
        if layout == 10 {
            for i in 0..4 {
                d.values
                    .push(Value::new(3, 16 + i, 106 + i as usize, -32767, 32767, 0, 0));
            }
        } else if layout != 4 {
            let (min, max) = if layout == 8 {
                (-32767, 32767)
            } else {
                (-1, 1)
            };
            d.values.push(Value::new(3, 16, 104, min, max, 0, 0));
            d.values.push(Value::new(3, 17, 105, min, max, 0, 0));
        }
    }

    fn control(&self, player: usize, name: &str) -> f64 {
        let d = &self.devices[player];
        let name = name.split('\0').next().unwrap_or_default();
        let Ok(name_c) = CString::new(name) else {
            return 0.0;
        };
        unsafe {
            let axis = SDL_GameControllerGetAxisFromString(name_c.as_ptr());
            if axis != SDL_CONTROLLER_AXIS_INVALID {
                let value = if !d.controller.is_null() {
                    SDL_GameControllerGetAxis(d.controller, axis) as i32
                } else {
                    *d.raw_axes.get(axis as usize).unwrap_or(&0) as i32
                };
                return clamp(
                    value as f64 / if value < 0 { 32768.0 } else { 32767.0 },
                    if axis >= SDL_CONTROLLER_AXIS_TRIGGERLEFT {
                        0.0
                    } else {
                        -1.0
                    },
                    1.0,
                );
            }
            let button = SDL_GameControllerGetButtonFromString(name_c.as_ptr());
            if button == SDL_CONTROLLER_BUTTON_INVALID {
                return 0.0;
            }
            if !d.controller.is_null() {
                SDL_GameControllerGetButton(d.controller, button) as f64
            } else {
                *d.raw_buttons.get(button as usize).unwrap_or(&0) as f64
            }
        }
    }

    fn state_values(&self, player: usize) -> Vec<i32> {
        let d = &self.devices[player];
        let mut slots = [0.0f64; SLOTS];
        for (i, value) in slots.iter_mut().enumerate().take(LSTICK) {
            *value = self.control(player, &d.config.mapping[i]);
        }
        let mut axes = [0.0f64; 4];
        for stick in 0..2 {
            let x_name = format!("{}x", d.config.mapping[LSTICK + stick]);
            let y_name = format!("{}y", d.config.mapping[LSTICK + stick]);
            let mut x = self.control(player, &x_name);
            let mut y = self.control(player, &y_name);
            let magnitude = x.hypot(y);
            let deadzone = d.config.deadzone[stick];
            if magnitude <= deadzone || deadzone >= 1.0 {
                x = 0.0;
                y = 0.0;
            } else {
                let scale = (magnitude.min(1.0) - deadzone) / (1.0 - deadzone) / magnitude
                    * d.config.range[stick];
                x *= scale;
                y *= scale;
            }
            if d.config.invert[stick * 2] {
                x = -x;
            }
            if d.config.invert[stick * 2 + 1] {
                y = -y;
            }
            if d.config.rotate[stick] {
                let old = x;
                x = -y;
                y = old;
            }
            axes[stick * 2] = clamp(x, -1.0, 1.0);
            axes[stick * 2 + 1] = clamp(y, -1.0, 1.0);
        }
        d.values
            .iter()
            .map(|value| {
                let slot = value.slot;
                if (100..104).contains(&slot) {
                    let x = axes[slot - 100];
                    (if value.min == 0 {
                        128.0 + x * if x < 0.0 { 128.0 } else { 127.0 }
                    } else {
                        x * if x < 0.0 {
                            -value.min as f64
                        } else {
                            value.max as f64
                        }
                    })
                    .round() as i32
                } else if slot >= 106 {
                    0
                } else if slot >= 104 {
                    let x = if slot == 104 {
                        (slots[RIGHT] > 0.5) as i32 - (slots[LEFT] > 0.5) as i32
                    } else {
                        (slots[DOWN] > 0.5) as i32 - (slots[UP] > 0.5) as i32
                    };
                    x * value.max
                } else if value.kind == 1 {
                    (slots[slot]
                        > if slot == ZL || slot == ZR {
                            d.config.threshold
                        } else {
                            0.5
                        }) as i32
                } else {
                    (clamp(slots[slot], 0.0, 1.0) * value.max as f64).round() as i32
                }
            })
            .collect()
    }

    fn frame(&mut self, player: usize, snapshot_client: Option<usize>) {
        let values = self.state_values(player);
        let d = &mut self.devices[player];
        let (sec, usec) = wall_time();
        let mut bytes = Vec::with_capacity((VALUES + 1) * RX_BYTES);
        for (i, current) in values.iter().copied().enumerate() {
            let v = d.values[i];
            let include = if snapshot_client.is_some() {
                v.kind == 3 || current != 0
            } else {
                current != v.value
            };
            if include {
                push_event(&mut bytes, sec, usec, v.kind as u16, v.code as u16, current);
            }
            if snapshot_client.is_none() {
                d.values[i].value = current;
            }
        }
        if bytes.is_empty() && snapshot_client.is_none() {
            return;
        }
        push_event(&mut bytes, sec, usec, 0, 0, 0);
        if let Some(client) = snapshot_client {
            self.send_frame(player, client, &bytes);
        } else {
            for client in 0..CLIENTS {
                if self.devices[player].clients[client].stream.is_some() {
                    self.send_frame(player, client, &bytes);
                }
            }
        }
    }

    fn send_frame(&mut self, player: usize, client: usize, bytes: &[u8]) {
        let Some(stream) = self.devices[player].clients[client].stream.as_mut() else {
            return;
        };
        let result = loop {
            match stream.write(bytes) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                result => break result,
            }
        };
        if !matches!(result, Ok(written) if written == bytes.len()) {
            self.devices[player].clients[client].stream = None;
            self.devices[player].clients[client].used = 0;
        }
    }

    fn destroy_device(&mut self, player: usize) {
        let event = self.path_for("event", player);
        let meta = self.directory.join(format!("meta/event{player}"));
        let d = &mut self.devices[player];
        d.renew = 0;
        d.strong = 0;
        d.weak = 0;
        if d.listener.is_some() {
            let _ = fs::remove_file(event);
            d.listener = None;
            let _ = fs::remove_file(meta);
        }
        for client in &mut d.clients {
            client.stream = None;
            client.used = 0;
        }
        if !d.controller.is_null() {
            unsafe {
                SDL_GameControllerRumble(d.controller, 0, 0, 0);
                SDL_GameControllerClose(d.controller);
            }
            d.controller = std::ptr::null_mut();
        }
    }

    fn create_device(&mut self, player: usize) -> io::Result<()> {
        self.make_layout(player);
        let values = self.state_values(player);
        for (v, value) in self.devices[player].values.iter_mut().zip(values) {
            v.value = value;
        }
        let temp = self.directory.join(format!("meta/.event{player}"));
        let meta = self.directory.join(format!("meta/event{player}"));
        let socket_path = self.path_for("event", player);
        let d = &self.devices[player];
        let layout = LAYOUTS[d.config.layout];
        let mut text = format!(
            "name {}\nid {}\nphys usb-steamarm-{player}/input0\nuniq {}\nkey",
            layout.name, layout.id, d.config.guid
        );
        for value in d.values.iter().filter(|value| value.kind == 1) {
            text.push_str(&format!(" {}", value.code));
        }
        text.push('\n');
        for value in d.values.iter().filter(|value| value.kind == 3) {
            text.push_str(&format!(
                "abs {} {} {} {} {} 0\n",
                value.code, value.min, value.max, value.fuzz, value.flat
            ));
        }
        text.push_str(if d.config.layout < 4 {
            "ff 80 81 88 89 90 96\neffects 16\n"
        } else {
            "ff 80\neffects 16\n"
        });
        fs::write(&temp, text)?;
        if let Err(error) = fs::rename(&temp, &meta) {
            let _ = fs::remove_file(&temp);
            return Err(error);
        }
        let listener = match UnixListener::bind(&socket_path) {
            Ok(listener) => listener,
            Err(error) => {
                let _ = fs::remove_file(&socket_path);
                let _ = fs::remove_file(&meta);
                return Err(error);
            }
        };
        if let Err(error) = listener.set_nonblocking(true) {
            drop(listener);
            let _ = fs::remove_file(&socket_path);
            let _ = fs::remove_file(&meta);
            return Err(error);
        }
        self.devices[player].listener = Some(listener);
        let led = self.devices[player].config.led.as_bytes();
        if !self.devices[player].controller.is_null() && led.len() == 7 && led[0] == b'#' {
            let parsed = (
                u8::from_str_radix(std::str::from_utf8(&led[1..3]).unwrap_or(""), 16),
                u8::from_str_radix(std::str::from_utf8(&led[3..5]).unwrap_or(""), 16),
                u8::from_str_radix(std::str::from_utf8(&led[5..7]).unwrap_or(""), 16),
            );
            if let (Ok(r), Ok(g), Ok(b)) = parsed {
                unsafe {
                    SDL_GameControllerSetLED(self.devices[player].controller, r, g, b);
                }
            }
        }
        Ok(())
    }

    fn rebuild(&mut self, force: bool) -> io::Result<()> {
        let count = unsafe { SDL_NumJoysticks() }.max(0) as usize;
        let mut chosen = vec![false; count];
        let mut indices = [None; PLAYERS];
        for pass in [true, false] {
            for player in 0..PLAYERS {
                let guid = &self.configs[player].guid;
                let explicit = !guid.is_empty() && guid != "auto";
                if explicit != pass {
                    continue;
                }
                for index in 0..count {
                    if chosen[index] || unsafe { SDL_IsGameController(index as c_int) } == 0 {
                        continue;
                    }
                    let mut guid_buf = [0i8; 33];
                    unsafe {
                        let device_guid = SDL_JoystickGetDeviceGUID(index as c_int);
                        SDL_JoystickGetGUIDString(
                            device_guid,
                            guid_buf.as_mut_ptr(),
                            guid_buf.len() as c_int,
                        );
                    }
                    let candidate = unsafe { CStr::from_ptr(guid_buf.as_ptr()) }.to_string_lossy();
                    if explicit && !candidate.eq_ignore_ascii_case(guid) {
                        continue;
                    }
                    chosen[index] = true;
                    indices[player] = Some(index as c_int);
                    break;
                }
            }
        }
        for player in 0..PLAYERS {
            let index = indices[player];
            let instance = index
                .map(|i| unsafe { SDL_JoystickGetDeviceInstanceID(i) })
                .unwrap_or(-1);
            let current = self.devices[player].controller;
            if !force
                && !current.is_null()
                && self.devices[player].instance == instance
                && unsafe { SDL_GameControllerGetAttached(current) } != 0
            {
                continue;
            }
            self.destroy_device(player);
            self.devices[player].config = self.configs[player].clone();
            if let Some(index) = index {
                let controller = unsafe { SDL_GameControllerOpen(index) };
                self.devices[player].controller = controller;
                self.devices[player].instance = instance;
                if !controller.is_null() {
                    if let Err(error) = self.create_device(player) {
                        self.destroy_device(player);
                        return Err(error);
                    }
                }
            }
        }
        Ok(())
    }

    fn rumble(&mut self, player: usize, record: &[u8; RX_BYTES]) {
        if &record[..4] != b"RMBL" {
            return;
        }
        let strong = u16::from_le_bytes([record[4], record[5]]) as u32;
        let weak = u16::from_le_bytes([record[6], record[7]]) as u32;
        let duration = u32::from_le_bytes([record[8], record[9], record[10], record[11]]);
        eprintln!("rumble strong={strong} weak={weak} ms={duration}");
        let d = &mut self.devices[player];
        if !d.controller.is_null() && d.config.rumble {
            d.strong = (strong as f64 * d.config.strength).round() as u16;
            d.weak = (weak as f64 * d.config.strength).round() as u16;
            let indefinite = duration == 0 || duration == 65535;
            unsafe {
                SDL_GameControllerRumble(
                    d.controller,
                    d.strong,
                    d.weak,
                    if indefinite { 10000 } else { duration },
                );
            }
            d.renew = if indefinite && (strong != 0 || weak != 0) {
                ticks() + 9000
            } else {
                0
            };
        }
    }

    fn apply_script(&mut self) {
        if !self.script_started {
            return;
        }
        while self.script_index < self.script.len()
            && ticks().saturating_sub(self.script_start) >= self.script[self.script_index].ms
        {
            let script = self.script[self.script_index].clone();
            self.script_index += 1;
            let mut name = script.slot.as_str();
            for i in 0..LSTICK {
                if name == SLOT_NAMES[i] {
                    name = &self.devices[0].config.mapping[i];
                    break;
                }
            }
            let Ok(name_c) = CString::new(name) else {
                continue;
            };
            unsafe {
                let axis = SDL_GameControllerGetAxisFromString(name_c.as_ptr());
                let button = SDL_GameControllerGetButtonFromString(name_c.as_ptr());
                if axis != SDL_CONTROLLER_AXIS_INVALID {
                    self.devices[0].raw_axes[axis as usize] =
                        clamp(script.value as f64, -32768.0, 32767.0) as i16;
                } else if button != SDL_CONTROLLER_BUTTON_INVALID {
                    self.devices[0].raw_buttons[button as usize] = u8::from(script.value != 0);
                }
            }
            self.frame(0, None);
        }
    }

    fn setup_directory(&mut self) -> Result<(), SetupError> {
        create_private_dir(&self.directory).map_err(SetupError::Io)?;
        let pid_path = self.directory.join("inputd.pid");
        let mut file = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .mode(0o600)
            .custom_flags(O_NOFOLLOW)
            .open(&pid_path)
            .map_err(SetupError::Io)?;
        if unsafe { flock(file.as_raw_fd(), LOCK_EX | LOCK_NB) } < 0 {
            return Err(SetupError::AlreadyRunning);
        }
        let mut old_pid = String::new();
        let _ = file.read_to_string(&mut old_pid);
        if let Ok(pid) = old_pid.trim().parse::<i32>() {
            let mut path_buf = [0i8; 4096];
            let alive = unsafe { kill(pid, 0) } == 0;
            let path_len = if alive {
                unsafe { proc_pidpath(pid, path_buf.as_mut_ptr().cast(), path_buf.len() as u32) }
            } else {
                0
            };
            if path_len > 0 {
                let proc_path = unsafe { CStr::from_ptr(path_buf.as_ptr()) }.to_string_lossy();
                if Path::new(proc_path.as_ref())
                    .file_name()
                    .is_some_and(|name| name == "steamarm-inputd")
                {
                    return Err(SetupError::AlreadyRunning);
                }
            }
        }
        self.pid_file = Some(file);
        let pid_file = self.pid_file.as_mut().unwrap();
        pid_file.set_len(0).map_err(SetupError::Io)?;
        pid_file.seek(SeekFrom::Start(0)).map_err(SetupError::Io)?;
        writeln!(pid_file, "{}", process::id()).map_err(SetupError::Io)?;
        let meta_dir = self.directory.join("meta");
        create_private_dir(&meta_dir).map_err(SetupError::Io)?;
        for player in 0..PLAYERS {
            let _ = fs::remove_file(self.path_for("event", player));
            let _ = fs::remove_file(meta_dir.join(format!("event{player}")));
            let _ = fs::remove_file(meta_dir.join(format!(".event{player}")));
        }
        Ok(())
    }

    fn init_sdl(&mut self) -> Result<(), String> {
        unsafe {
            SDL_SetHint(
                SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS.as_ptr().cast(),
                b"1\0".as_ptr().cast(),
            );
            SDL_SetHint(
                SDL_HINT_JOYSTICK_HIDAPI.as_ptr().cast(),
                b"1\0".as_ptr().cast(),
            );
            if SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0 {
                return Err(sdl_error());
            }
        }
        self.sdl_initialized = true;
        Ok(())
    }

    fn run_loop(&mut self) -> i32 {
        let mut status = 0;
        if self.fake_count > 0 {
            for player in 0..self.fake_count {
                self.devices[player].config = self.configs[player].clone();
                if let Err(error) = self.create_device(player) {
                    log_error("create device", error);
                    status = 1;
                    STOPPING.store(true, Ordering::Relaxed);
                    break;
                }
            }
        } else if let Err(error) = self.rebuild(true) {
            log_error("create device", error);
            return 1;
        }

        let mut previous = file_stamp(&self.config_path);
        let mut existed = previous.is_some();
        let mut check = ticks().saturating_add(1000);
        while !STOPPING.load(Ordering::Relaxed) {
            unsafe {
                SDL_PumpEvents();
            }
            let mut hotplug = false;
            loop {
                let mut event = SdlEvent { bytes: [0; 56] };
                if unsafe { SDL_PollEvent(&mut event) } == 0 {
                    break;
                }
                let event_type = u32::from_ne_bytes(event.bytes[0..4].try_into().unwrap());
                if event_type == SDL_CONTROLLERDEVICEADDED
                    || event_type == SDL_CONTROLLERDEVICEREMOVED
                {
                    hotplug = true;
                }
            }

            if ticks() >= check {
                let current = file_stamp(&self.config_path);
                let exists = current.is_some();
                let changed = exists != existed || (exists && current != previous);
                if changed && self.load_config() {
                    if self.fake_count > 0 {
                        for player in 0..self.fake_count {
                            let layout = self.devices[player].config.layout;
                            self.destroy_device(player);
                            self.devices[player].config = self.configs[player].clone();
                            self.devices[player].config.layout = layout;
                            if let Err(error) = self.create_device(player) {
                                log_error("rebuild fake device", error);
                                status = 1;
                                STOPPING.store(true, Ordering::Relaxed);
                                break;
                            }
                        }
                    } else if let Err(error) = self.rebuild(true) {
                        log_error("rebuild controllers", error);
                        status = 1;
                        STOPPING.store(true, Ordering::Relaxed);
                    }
                    previous = current;
                    existed = exists;
                }
                check = ticks().saturating_add(1000);
            }
            if hotplug && self.fake_count == 0 {
                if let Err(error) = self.rebuild(false) {
                    log_error("rebuild controllers", error);
                    status = 1;
                    break;
                }
            }
            self.apply_script();

            let mut polls = Vec::<PollFd>::with_capacity(PLAYERS * (CLIENTS + 1));
            let mut targets = Vec::<PollTarget>::with_capacity(PLAYERS * (CLIENTS + 1));
            for player in 0..PLAYERS {
                let d = &mut self.devices[player];
                if d.listener.is_none() {
                    continue;
                }
                self.frame(player, None);
                let d = &mut self.devices[player];
                if d.renew != 0 && ticks() >= d.renew {
                    if !d.controller.is_null() {
                        unsafe {
                            SDL_GameControllerRumble(d.controller, d.strong, d.weak, 10000);
                        }
                    }
                    d.renew = ticks().saturating_add(9000);
                }
                polls.push(PollFd {
                    fd: d.listener.as_ref().unwrap().as_raw_fd(),
                    events: POLLIN,
                    revents: 0,
                });
                targets.push(PollTarget {
                    player,
                    client: None,
                });
                for (index, client) in d.clients.iter().enumerate() {
                    if let Some(stream) = &client.stream {
                        polls.push(PollFd {
                            fd: stream.as_raw_fd(),
                            events: POLLIN,
                            revents: 0,
                        });
                        targets.push(PollTarget {
                            player,
                            client: Some(index),
                        });
                    }
                }
            }
            let ready = unsafe { poll(polls.as_mut_ptr(), polls.len() as u32, 4) };
            if ready < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                log_error("poll", error);
                status = 1;
                break;
            }
            for (index, poll_fd) in polls.iter().enumerate() {
                if poll_fd.revents == 0 {
                    continue;
                }
                let target = targets[index];
                if let Some(client) = target.client {
                    self.read_client(target.player, client);
                } else {
                    self.accept_client(target.player);
                }
            }
        }
        status
    }

    fn accept_client(&mut self, player: usize) {
        let accepted = match self.devices[player].listener.as_ref().unwrap().accept() {
            Ok((stream, _)) => stream,
            Err(_) => return,
        };
        if accepted.set_nonblocking(true).is_err() {
            return;
        }
        let Some(index) = self.devices[player]
            .clients
            .iter()
            .position(|client| client.stream.is_none())
        else {
            return;
        };
        self.devices[player].clients[index].stream = Some(accepted);
        self.devices[player].clients[index].used = 0;
        self.frame(player, Some(index));
        if player == 0 && !self.script_started {
            self.script_start = ticks();
            self.script_started = true;
        }
    }

    fn read_client(&mut self, player: usize, client: usize) {
        for _ in 0..64 {
            let outcome = {
                let c = &mut self.devices[player].clients[client];
                let Some(stream) = c.stream.as_mut() else {
                    return;
                };
                match stream.read(&mut c.rx[c.used..]) {
                    Ok(0) => ReadOutcome::Closed,
                    Ok(n) => {
                        c.used += n;
                        if c.used == RX_BYTES {
                            let record = c.rx;
                            c.used = 0;
                            ReadOutcome::Record(record)
                        } else {
                            ReadOutcome::Continue
                        }
                    }
                    Err(error)
                        if error.kind() == io::ErrorKind::WouldBlock
                            || error.kind() == io::ErrorKind::Interrupted =>
                    {
                        ReadOutcome::Continue
                    }
                    Err(_) => ReadOutcome::Closed,
                }
            };
            match outcome {
                ReadOutcome::Record(record) => self.rumble(player, &record),
                ReadOutcome::Continue => return,
                ReadOutcome::Closed => {
                    self.devices[player].clients[client].stream = None;
                    self.devices[player].clients[client].used = 0;
                    return;
                }
            }
        }
    }

    fn cleanup(&mut self) {
        for player in 0..PLAYERS {
            self.destroy_device(player);
        }
        if self.pid_file.is_some() {
            let _ = fs::remove_file(self.directory.join("inputd.pid"));
            self.pid_file = None;
            let _ = fs::remove_dir(self.directory.join("meta"));
        }
        if self.sdl_initialized {
            unsafe {
                SDL_Quit();
            }
            self.sdl_initialized = false;
        }
    }
}

#[derive(Clone, Copy)]
struct PollTarget {
    player: usize,
    client: Option<usize>,
}

enum ReadOutcome {
    Record([u8; RX_BYTES]),
    Continue,
    Closed,
}

enum SetupError {
    AlreadyRunning,
    Io(io::Error),
}

fn default_state_root() -> PathBuf {
    if let Some(base) = env::var_os("STEAMARM_STATE") {
        return PathBuf::from(base);
    }
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    let newer = home.join("Developer/SteamARM-roots");
    let older = home.join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn create_private_dir(path: &Path) -> io::Result<()> {
    match fs::create_dir(path) {
        Ok(()) => fs::set_permissions(path, fs::Permissions::from_mode(0o700)),
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists && path.is_dir() => Ok(()),
        Err(error) => Err(error),
    }
}

fn ticks() -> u64 {
    unsafe { SDL_GetTicks64() }
}

fn wall_time() -> (u64, u64) {
    let duration = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    (duration.as_secs(), duration.subsec_micros() as u64)
}

fn push_event(out: &mut Vec<u8>, sec: u64, usec: u64, kind: u16, code: u16, value: i32) {
    out.extend_from_slice(&sec.to_le_bytes());
    out.extend_from_slice(&usec.to_le_bytes());
    out.extend_from_slice(&kind.to_le_bytes());
    out.extend_from_slice(&code.to_le_bytes());
    out.extend_from_slice(&value.to_le_bytes());
}

fn file_stamp(path: &Path) -> Option<(u64, SystemTime)> {
    let meta = fs::metadata(path).ok()?;
    Some((meta.len(), meta.modified().ok()?))
}

fn load_script(path: &Path) -> io::Result<Vec<ScriptEvent>> {
    let text = fs::read_to_string(path)?;
    let mut script = Vec::new();
    let mut last = 0;
    for line in text.lines() {
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        if line.len() > 255 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "script line too long",
            ));
        }
        let parts = line.split_whitespace().collect::<Vec<_>>();
        if parts.len() != 3 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid script record",
            ));
        }
        let ms = parts[0]
            .parse::<u64>()
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid script time"))?;
        let value = parts[2]
            .parse::<i32>()
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid script value"))?;
        if ms < last || parts[1].len() > 63 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "script times must be ordered",
            ));
        }
        script.push(ScriptEvent {
            ms,
            slot: parts[1].to_owned(),
            value,
        });
        last = ms;
    }
    Ok(script)
}

fn parse_args(daemon: &mut Daemon) -> Result<bool, i32> {
    let args = env::args().skip(1).collect::<Vec<_>>();
    let mut foreground = false;
    let mut fake = None;
    let mut script_file = None;
    let mut i = 0;
    while i < args.len() {
        if args[i] == "--foreground" {
            foreground = true;
            i += 1;
            continue;
        }
        if i + 1 >= args.len() {
            usage();
            return Err(2);
        }
        let key = args[i].as_str();
        let value = &args[i + 1];
        match key {
            "--dir" => {
                if value.len() > 80 {
                    eprintln!("socket directory too long");
                    return Err(2);
                }
                daemon.directory = PathBuf::from(value);
            }
            "--config" => daemon.config_path = PathBuf::from(value),
            "--fake" => fake = Some(value.clone()),
            "--script" => script_file = Some(PathBuf::from(value)),
            _ => {
                usage();
                return Err(2);
            }
        }
        i += 2;
    }
    if !daemon.load_config() {
        eprintln!("using default configuration");
    }
    if let Some(fake) = fake.as_deref() {
        let kinds = fake.split(',').collect::<Vec<_>>();
        if kinds.is_empty() || kinds.len() > PLAYERS {
            eprintln!("invalid fake controller list");
            return Err(2);
        }
        for (i, kind) in kinds.iter().enumerate() {
            let Some(id) = layout_id(kind) else {
                eprintln!("invalid fake controller list");
                return Err(2);
            };
            daemon.configs[i].layout = id;
        }
        daemon.fake_count = kinds.len();
    }
    if let Some(script_file) = script_file {
        if daemon.fake_count == 0 {
            eprintln!("invalid fake script");
            return Err(2);
        }
        daemon.script = match load_script(&script_file) {
            Ok(script) => script,
            Err(_) => {
                eprintln!("invalid fake script");
                return Err(2);
            }
        };
    }
    Ok(foreground)
}

fn daemonize() -> io::Result<bool> {
    let pid = unsafe { fork() };
    if pid < 0 {
        return Err(io::Error::last_os_error());
    }
    if pid > 0 {
        return Ok(false);
    }
    if unsafe { setsid() } < 0 {
        return Err(io::Error::last_os_error());
    }
    let stdin = File::open("/dev/null")?;
    if unsafe { dup2(stdin.as_raw_fd(), 0) } < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(true)
}

fn install_signals() {
    STOPPING.store(false, Ordering::Relaxed);
    unsafe {
        signal(SIGPIPE, SIG_IGN);
        let handler = stop_handler as *const () as usize;
        if signal(SIGTERM, handler) == SIG_ERR {
            eprintln!("inputd: cannot handle SIGTERM");
        }
        if signal(SIGINT, handler) == SIG_ERR {
            eprintln!("inputd: cannot handle SIGINT");
        }
    }
}

fn usage() {
    eprintln!("Usage: steamarm-inputd [--dir DIR] [--config PATH] [--fake TYPE[,TYPE...]] [--script FILE] [--foreground]");
}

fn run() -> i32 {
    let mut daemon = Daemon::new();
    let foreground = match parse_args(&mut daemon) {
        Ok(foreground) => foreground,
        Err(code) => return code,
    };
    if !foreground {
        match daemonize() {
            Ok(false) => return 0,
            Ok(true) => {}
            Err(error) => {
                log_error("daemonize", error);
                return 1;
            }
        }
    }
    install_signals();
    match daemon.setup_directory() {
        Ok(()) => {}
        Err(SetupError::AlreadyRunning) => {
            eprintln!("steamarm-inputd already running");
            return 0;
        }
        Err(SetupError::Io(error)) => {
            log_error("input directory", error);
            daemon.cleanup();
            return 1;
        }
    }
    if let Err(error) = daemon.init_sdl() {
        eprintln!("SDL_Init: {error}");
        daemon.cleanup();
        return 1;
    }
    let status = daemon.run_loop();
    daemon.cleanup();
    status
}

fn main() {
    process::exit(run());
}
