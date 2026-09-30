// The physical keyboard layout Android uses for the Mac's keyboard, set to
// the Mac's own (scripts/android-session.py picks it from macOS's current
// input source). The keys reach Android by position (Linux key codes from
// SteamARM's X server and from steamarm-wlmac), so the characters they type
// are Android's keyboard layout's choice, and with none chosen that is US
// English: on a Spanish keyboard ñ typed ';'. No VM (AGENTS.md).
//
//   app_process64 -cp <stand-in dex> /system/bin org.steamarm.android.KeyboardLayout \
//       keyboard_layout_spanish [device name, default wayland_keyboard]
//
// It calls InputManager.setCurrentKeyboardLayoutForInputDevice for that
// device, as Settings > Physical keyboard does; InputManagerService keeps the
// choice in /data/system/input-manager-state.xml. Hidden APIs by reflection:
// the image's own framework runs it.
package org.steamarm.android;

import java.lang.reflect.Method;

public final class KeyboardLayout {
    private static final String LAYOUTS = "com.android.inputdevices/com.android.inputdevices.InputDeviceReceiver/";

    public static void main(String[] args) throws Exception {
        String layout = LAYOUTS + args[0];
        String name = args.length > 1 ? args[1] : "wayland_keyboard";
        Class<?> imClass = Class.forName("android.hardware.input.InputManager");
        Object im = imClass.getMethod("getInstance").invoke(null);
        Class<?> devClass = Class.forName("android.view.InputDevice");
        int[] ids = (int[]) devClass.getMethod("getDeviceIds").invoke(null);
        for (int id : ids) {
            Object dev = devClass.getMethod("getDevice", int.class).invoke(null, id);
            if (dev == null || !name.equals(devClass.getMethod("getName").invoke(dev))) continue;
            Object ident = devClass.getMethod("getIdentifier").invoke(dev);
            Method set = null;
            for (Method m : imClass.getMethods())
                if (m.getName().equals("setCurrentKeyboardLayoutForInputDevice")) set = m;
            if (set == null) throw new NoSuchMethodException("setCurrentKeyboardLayoutForInputDevice");
            set.invoke(im, ident, layout);
            Method get = imClass.getMethod("getCurrentKeyboardLayoutForInputDevice", ident.getClass());
            System.out.println("keyboard layout of " + name + ": " + get.invoke(im, ident));
            System.exit(0);     // the binder threads InputManager started keep the VM up otherwise (200 s)
        }
        System.out.println("no input device named " + name);
        System.exit(3);
    }
}
