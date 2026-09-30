// A stand-in for native Android daemons that system_server cannot boot
// without and that cannot run under lxrun (docs/ANDROID_RUNTIME_ARCHITECTURE.md,
// "Boot to an app"; benchmarks/stage28-android-apk.txt). No VM (AGENTS.md).
//
//   app_process64 -cp <this dex>:/system/framework/services.jar /system/bin \
//       org.steamarm.android.BinderStandIn netd=android.net.INetd dnsresolver=android.net.IDnsResolver
//
// For each NAME=INTERFACE it registers a binder service NAME with
// servicemanager that speaks the image's own AIDL interface (the generated
// Stub class, found by reflection: transaction codes, descriptor, version)
// and answers every call as a daemon with nothing to do would: success, with
// empty values -- true for booleans ("done"; INTERFACE:false for false), 0 for numbers, "" for strings,
// empty arrays and lists, null for parcelables and binders. One-way calls get
// no reply, as with any binder service. Every call is logged once per method
// (logcat tag SteamARMStandIn), so what the framework asked is on record.
// "socket=NAME" accepts connections on the socket init made for the daemon
// (ANDROID_SOCKET_NAME) and drops what arrives (sink, below).
// "network=IFACE,ADDRESS/PREFIX,GATEWAY,DNS[;DNS]" registers one network with
// ConnectivityService once the system has booted (NetworkAgentStandIn).
//
// Why: netd needs netlink route and uevent sockets, iptables, BPF maps and
// traffic control, none of which a Mac has; its "netd" (INetd) and
// "dnsresolver" (IDnsResolver) services are waited for forever by
// system_server's NetworkManagementService and ConnectivityService
// (NetdService.get() loops, "WARNING: returning null INetd instance.",
// MEASURED). With this stand-in every network request is accepted and nothing
// happens; the one network there is comes from NetworkAgentStandIn.
package org.steamarm.android;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ServiceManager;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

public final class BinderStandIn extends Binder {
    private static final String TAG = "SteamARMStandIn";
    private final String name;
    private final String descriptor;
    private final Map<Integer, Method> byCode = new HashMap<>();
    private final Set<Integer> logged = new HashSet<>();
    private final int version;
    private final String hash;

    private final boolean yes;          // the answer for booleans

    private BinderStandIn(String name, String iface, boolean yes) throws Exception {
        this.name = name;
        this.yes = yes;
        Class<?> i = Class.forName(iface);
        Class<?> stub = Class.forName(iface + "$Stub");
        String d = staticString(stub, "DESCRIPTOR");
        if (d == null) d = staticString(i, "DESCRIPTOR");
        descriptor = d != null ? d : iface;
        Integer v = staticInt(i, "VERSION");
        version = v != null ? v : 1;
        String h = staticString(i, "HASH");
        hash = h != null ? h : "notfrozen";
        Map<String, Method> methods = new HashMap<>();
        for (Method m : i.getMethods()) methods.put(m.getName(), m);
        for (Field f : stub.getDeclaredFields()) {
            if (!f.getName().startsWith("TRANSACTION_") || !Modifier.isStatic(f.getModifiers())) continue;
            f.setAccessible(true);
            Method m = methods.get(f.getName().substring("TRANSACTION_".length()));
            if (m != null) byCode.put(f.getInt(null), m);
        }
        Log.i(TAG, name + ": " + descriptor + " version " + version + ", " + byCode.size() + " methods");
    }

    private static String staticString(Class<?> c, String f) {
        try {
            Field x = c.getDeclaredField(f);
            x.setAccessible(true);
            return (String) x.get(null);
        } catch (ReflectiveOperationException | ClassCastException e) {
            return null;
        }
    }

    private static Integer staticInt(Class<?> c, String f) {
        try {
            Field x = c.getDeclaredField(f);
            x.setAccessible(true);
            return x.getInt(null);
        } catch (ReflectiveOperationException | IllegalArgumentException e) {
            return null;
        }
    }

    @Override
    public String getInterfaceDescriptor() {
        return descriptor;
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code == INTERFACE_TRANSACTION) {
            reply.writeString(descriptor);
            return true;
        }
        Method m = byCode.get(code);
        if (m == null) {
            if (code == 0x00fffffe || code == 0x00fffffd) {      // getInterfaceVersion / getInterfaceHash
                reply.writeNoException();
                if (code == 0x00fffffe) reply.writeInt(version); else reply.writeString(hash);
                return true;
            }
            if (logged.add(code)) Log.w(TAG, name + ": unknown transaction " + code);
            return false;
        }
        if (logged.add(code)) Log.i(TAG, name + ": " + m.getName() + " -> default answer");
        if ((flags & FLAG_ONEWAY) != 0) return true;
        reply.writeNoException();
        String n = m.getName();
        if (n.equals("getInterfaceVersion")) { reply.writeInt(version); return true; }
        if (n.equals("getInterfaceHash")) { reply.writeString(hash); return true; }
        writeDefault(reply, m.getReturnType(), yes);
        return true;
    }

    private static void writeDefault(Parcel reply, Class<?> t, boolean yes) {
        if (t == void.class) return;
        if (t == boolean.class) { reply.writeInt(yes ? 1 : 0); return; }
        if (t == long.class) { reply.writeLong(0); return; }
        if (t == float.class) { reply.writeFloat(0); return; }
        if (t == double.class) { reply.writeDouble(0); return; }
        if (t.isPrimitive()) { reply.writeInt(0); return; }             // int, byte, char, short
        if (t == String.class) { reply.writeString(""); return; }
        if (IBinder.class.isAssignableFrom(t) || android.os.IInterface.class.isAssignableFrom(t)) {
            reply.writeStrongBinder(null);
            return;
        }
        // Arrays, lists and maps: length 0. A parcelable: the null marker 0.
        reply.writeInt(0);
    }

    // A socket init made for the daemon (ANDROID_SOCKET_<name>, as the boot
    // passes it): connections are accepted and whatever arrives is read and
    // dropped. NsdService waits in system_server's start-up until it has
    // connected to netd's "mdns" socket (NativeDaemonConnector), and asks
    // nothing more until an app uses network service discovery.
    private static void sink(final String name, int fdNum) throws Exception {
        java.io.FileDescriptor fd = new java.io.FileDescriptor();
        Method set = java.io.FileDescriptor.class.getDeclaredMethod("setInt$", int.class);  // libcore
        set.invoke(fd, fdNum);
        final android.net.LocalServerSocket server = new android.net.LocalServerSocket(fd);
        Thread t = new Thread(new Runnable() {
            public void run() {
                while (true) {
                    try {
                        final android.net.LocalSocket s = server.accept();
                        Log.i(TAG, "socket " + name + ": a client connected");
                        new Thread(new Runnable() {
                            public void run() {
                                byte[] b = new byte[4096];
                                try {
                                    java.io.InputStream in = s.getInputStream();
                                    while (in.read(b) >= 0) { }
                                } catch (java.io.IOException e) {
                                    // the client is gone
                                }
                            }
                        }, "sink-" + name).start();
                    } catch (java.io.IOException e) {
                        Log.w(TAG, "socket " + name + ": " + e);
                        return;
                    }
                }
            }
        }, "accept-" + name);
        t.setDaemon(true);
        t.start();
    }

    public static void main(String[] args) throws Exception {
        for (String a : args) {
            int eq = a.indexOf('=');
            String name = a.substring(0, eq), iface = a.substring(eq + 1);
            if (name.equals("socket")) {
                String env = System.getenv("ANDROID_SOCKET_" + iface);
                if (env != null) sink(iface, Integer.parseInt(env));
                continue;
            }
            if (name.equals("network")) {           // NetworkAgentStandIn: the Mac's network
                NetworkAgentStandIn.start(iface);
                continue;
            }
            // INTERFACE[:false]: booleans answer true ("it worked": netd's
            // isAlive, firewall and bandwidth calls) unless the interface's
            // booleans are questions a daemon with nothing would answer no
            // (vold's isCheckpointing, needsCheckpoint, supportsCheckpoint).
            boolean yes = !iface.endsWith(":false");
            if (!yes) iface = iface.substring(0, iface.length() - ":false".length());
            ServiceManager.addService(name, new BinderStandIn(name, iface, yes));
            Log.i(TAG, "registered " + name);
        }
        System.out.println("BinderStandIn: serving " + String.join(" ", args));
        Object forever = new Object();
        synchronized (forever) {
            while (true) forever.wait();      // binder threads serve (app_process starts the pool)
        }
    }
}
