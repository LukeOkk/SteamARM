// Android's clipboard from a shell (tests/android/run.sh, the clipboard
// between the Mac and Android): "get" prints the primary clip's text,
// "set TEXT" makes TEXT the primary clip. IClipboard by reflection, as the
// package com.android.shell -- and as its uid, 2000: ClipboardService checks
// that the caller owns the package it names ("Calling uid 0 does not own
// package com.android.shell"), and the shell may read the clipboard from the
// background (READ_CLIPBOARD_IN_BACKGROUND). The image's own framework runs it.
import java.lang.reflect.Method;

public class Clip {
    public static void main(String[] a) throws Exception {
        // Started as root: become the shell and run again. The binder
        // connection is made before main() runs, with the uid of that
        // moment, so the second process is the one that asks.
        Class<?> os = Class.forName("android.system.Os");
        if ((Integer) os.getMethod("getuid").invoke(null) == 0) {
            os.getMethod("setgid", int.class).invoke(null, 2000);
            os.getMethod("setuid", int.class).invoke(null, 2000);
            java.util.List<String> cmd = new java.util.ArrayList<>(java.util.Arrays.asList(
                    "/system/bin/app_process64", "/system/bin", "Clip"));
            cmd.addAll(java.util.Arrays.asList(a));
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            java.io.InputStream in = p.getInputStream();
            byte[] buf = new byte[4096];
            for (int n; (n = in.read(buf)) > 0; ) System.out.write(buf, 0, n);
            System.out.flush();
            System.exit(p.waitFor());
        }
        Object binder = Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "clipboard");
        Object cb = Class.forName("android.content.IClipboard$Stub").getMethod("asInterface", Class.forName("android.os.IBinder")).invoke(null, binder);
        Class<?> clipData = Class.forName("android.content.ClipData");
        if (a[0].equals("set")) {
            Object clip = clipData.getMethod("newPlainText", CharSequence.class, CharSequence.class).invoke(null, "steamarm", a[1]);
            Method set = find(cb, "setPrimaryClip");
            set.invoke(cb, clip, "com.android.shell", 0);
            System.out.println("set " + a[1].length() + " characters");
        } else {
            Object clip = find(cb, "getPrimaryClip").invoke(cb, "com.android.shell", 0);
            if (clip == null) { System.out.println("clip: none"); System.exit(0); }
            Object item = clipData.getMethod("getItemAt", int.class).invoke(clip, 0);
            Object text = item.getClass().getMethod("getText").invoke(item);
            System.out.println("clip: " + text);
        }
        System.exit(0);
    }

    static Method find(Object o, String name) throws Exception {
        for (Method m : o.getClass().getMethods()) if (m.getName().equals(name)) { m.setAccessible(true); return m; }
        throw new NoSuchMethodException(name);
    }
}
