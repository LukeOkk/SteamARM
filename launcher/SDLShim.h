// C shim over SDL2's game-controller API, imported into Swift as the bridging
// header. Everything crosses the boundary as plain ints, strings and opaque
// pointers so the Swift side never touches SDL's unions or enums.
#ifndef STEAMARM_SDLSHIM_H
#define STEAMARM_SDLSHIM_H

#define SDL_MAIN_HANDLED 1
#include <SDL.h>
#include <string.h>

// Event kinds reported by shim_poll.
#define SHIM_EV_NONE 0
#define SHIM_EV_ADDED 1     /* which = device index */
#define SHIM_EV_REMOVED 2   /* which = instance id */
#define SHIM_EV_BUTTON 3    /* which = instance id, code = button, value = 0/1 */
#define SHIM_EV_AXIS 4      /* which = instance id, code = axis, value = -32768..32767 */
#define SHIM_EV_REMAPPED 5

static inline int shim_init(void) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    return SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}

static inline void shim_quit(void) { SDL_Quit(); }

static inline int shim_num_joysticks(void) { return SDL_NumJoysticks(); }

static inline int shim_is_controller(int index) {
    return SDL_IsGameController(index) ? 1 : 0;
}

// GUID as SDL's 32-char hex string (the key of the controller database).
static inline void shim_device_guid(int index, char *buf, int len) {
    SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(index);
    SDL_JoystickGetGUIDString(g, buf, len);
}

static inline const char *shim_device_name(int index) {
    const char *n = SDL_GameControllerNameForIndex(index);
    if (!n) n = SDL_JoystickNameForIndex(index);
    return n ? n : "Mando";
}

static inline void *shim_open(int index) { return SDL_GameControllerOpen(index); }

static inline void shim_close(void *gc) {
    if (gc) SDL_GameControllerClose((SDL_GameController *)gc);
}

static inline int shim_instance_id(void *gc) {
    SDL_Joystick *j = SDL_GameControllerGetJoystick((SDL_GameController *)gc);
    return j ? SDL_JoystickInstanceID(j) : -1;
}

static inline int shim_button(void *gc, int button) {
    return SDL_GameControllerGetButton((SDL_GameController *)gc,
                                       (SDL_GameControllerButton)button);
}

static inline int shim_axis(void *gc, int axis) {
    return SDL_GameControllerGetAxis((SDL_GameController *)gc,
                                     (SDL_GameControllerAxis)axis);
}

static inline int shim_rumble(void *gc, int low, int high, int ms) {
    return SDL_GameControllerRumble((SDL_GameController *)gc, (Uint16)low,
                                    (Uint16)high, (Uint32)ms);
}

static inline int shim_has_rumble(void *gc) {
    return SDL_GameControllerHasRumble((SDL_GameController *)gc) ? 1 : 0;
}

static inline const char *shim_button_name(int button) {
    const char *s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)button);
    return s ? s : "";
}

static inline const char *shim_axis_name(int axis) {
    const char *s = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)axis);
    return s ? s : "";
}

static inline int shim_button_count(void) { return SDL_CONTROLLER_BUTTON_MAX; }
static inline int shim_axis_count(void) { return SDL_CONTROLLER_AXIS_MAX; }

// Pops one event; returns its SHIM_EV_* kind (0 when the queue is empty).
static inline int shim_poll(int *which, int *code, int *value) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED:
            *which = e.cdevice.which; *code = 0; *value = 0;
            return SHIM_EV_ADDED;
        case SDL_CONTROLLERDEVICEREMOVED:
            *which = e.cdevice.which; *code = 0; *value = 0;
            return SHIM_EV_REMOVED;
        case SDL_CONTROLLERDEVICEREMAPPED:
            *which = e.cdevice.which; *code = 0; *value = 0;
            return SHIM_EV_REMAPPED;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            *which = e.cbutton.which; *code = e.cbutton.button;
            *value = e.cbutton.state == SDL_PRESSED;
            return SHIM_EV_BUTTON;
        case SDL_CONTROLLERAXISMOTION:
            *which = e.caxis.which; *code = e.caxis.axis; *value = e.caxis.value;
            return SHIM_EV_AXIS;
        default:
            break;
        }
    }
    return SHIM_EV_NONE;
}

#endif
