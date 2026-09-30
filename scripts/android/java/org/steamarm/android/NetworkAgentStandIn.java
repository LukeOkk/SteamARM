// One network for Android: the Mac's (docs/ANDROID_RUNTIME_ARCHITECTURE.md,
// "Network"). No VM (AGENTS.md).
//
// netd is a stand-in here (BinderStandIn), so no network agent ever appears
// and ConnectivityService reports no network at all: apps that ask
// (ConnectivityManager.getActiveNetwork, and every store and account client
// does) see none, although their sockets reach the Internet through the Mac
// and their names resolve through scripts/android_dnsproxy.py. This registers
// what an Ethernet driver's factory would: a NetworkAgent for "eth0" with
// the Internet capability, the Mac's name servers and a default route, then
// marks it connected. ConnectivityService programs it into netd (the
// stand-in accepts every call), makes it the default network and has the
// network stack validate it with its usual HTTP probes, which then go out
// through the Mac.
//
// The framework's NetworkAgent class needs a Context; this speaks the same
// protocol to ConnectivityService directly (IConnectivityManager.
// registerNetworkAgent and its AsyncChannel messages, Android 11), with the
// hidden classes reached by reflection: the image's own framework is used.
//
//   BinderStandIn ... network=eth0,10.0.2.15/24,10.0.2.2,<dns>[;<dns>...]
package org.steamarm.android;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Message;
import android.os.Messenger;
import android.os.ServiceManager;
import android.util.Log;

import java.lang.reflect.Method;
import java.net.InetAddress;

final class NetworkAgentStandIn {
    private static final String TAG = "SteamARMNetwork";
    // com.android.internal.util.Protocol / AsyncChannel / NetworkAgent (R)
    private static final int ASYNC_BASE = 0x00011000;
    private static final int CMD_CHANNEL_FULL_CONNECTION = ASYNC_BASE + 1;
    private static final int CMD_CHANNEL_FULLY_CONNECTED = ASYNC_BASE + 2;
    private static final int CMD_CHANNEL_DISCONNECTED = ASYNC_BASE + 4;
    private static final int AGENT_BASE = 0x00081000;
    private static final int EVENT_NETWORK_INFO_CHANGED = AGENT_BASE + 1;
    private static final int TYPE_ETHERNET = 9;                 // ConnectivityManager
    private static final int TRANSPORT_ETHERNET = 3;            // NetworkCapabilities
    private static final int[] CAPABILITIES = {
        12,     // NET_CAPABILITY_INTERNET
        11,     // NET_CAPABILITY_NOT_METERED
        13,     // NET_CAPABILITY_NOT_RESTRICTED
        14,     // NET_CAPABILITY_TRUSTED
        15,     // NET_CAPABILITY_NOT_VPN
        18,     // NET_CAPABILITY_NOT_ROAMING
        20,     // NET_CAPABILITY_NOT_CONGESTED
        21,     // NET_CAPABILITY_NOT_SUSPENDED
    };

    private static Messenger connectivity;          // ConnectivityService's side, once connected
    private static Messenger mine;
    private static Object info;                     // android.net.NetworkInfo

    static void start(final String spec) {
        Thread t = new Thread(new Runnable() {
            public void run() {
                try {
                    register(spec.split(","));
                } catch (Throwable e) {
                    Log.w(TAG, "no network: " + e);
                }
            }
        }, "network-agent");
        t.setDaemon(true);
        t.start();
    }

    private static Object call(Object target, String name, Object... args) throws Exception {
        Class<?> c = target instanceof Class ? (Class<?>) target : target.getClass();
        for (Method m : c.getMethods()) {
            if (!m.getName().equals(name) || m.getParameterTypes().length != args.length) continue;
            Class<?>[] p = m.getParameterTypes();
            boolean fits = true;
            for (int i = 0; i < p.length && fits; i++) {
                if (args[i] == null) continue;
                Class<?> want = p[i].isPrimitive() ? boxed(p[i]) : p[i];
                fits = want.isInstance(args[i]);
            }
            if (!fits) continue;
            m.setAccessible(true);          // e.g. IConnectivityManager.Stub.Proxy is a private class
            return m.invoke(target instanceof Class ? null : target, args);
        }
        throw new NoSuchMethodException(c.getName() + "." + name + "/" + args.length);
    }

    private static Class<?> boxed(Class<?> p) {
        if (p == int.class) return Integer.class;
        if (p == boolean.class) return Boolean.class;
        if (p == long.class) return Long.class;
        return p;
    }

    private static String prop(String name) {
        try {
            return (String) call(Class.forName("android.os.SystemProperties"), "get", name);
        } catch (Exception e) {
            return "";
        }
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static Object detailed(String name) throws Exception {
        return Enum.valueOf((Class) Class.forName("android.net.NetworkInfo$DetailedState"), name);
    }

    private static void register(String[] spec) throws Exception {
        String iface = spec[0], addr = spec[1], gateway = spec[2];
        String[] dns = spec.length > 3 && !spec[3].isEmpty() ? spec[3].split(";") : new String[0];
        // ConnectivityService takes agents at any time, but its network
        // stack and the default-network requests are there once the system
        // has booted, as for a driver's agent after boot.
        IBinder cs = null;
        for (int i = 0; i < 1200 && (cs == null || !"1".equals(prop("sys.boot_completed"))); i++) {
            if (cs == null) cs = ServiceManager.getService("connectivity");
            Thread.sleep(500);
        }
        if (cs == null) throw new IllegalStateException("no connectivity service");

        Class<?> infoClass = Class.forName("android.net.NetworkInfo");
        info = infoClass.getConstructor(int.class, int.class, String.class, String.class)
                .newInstance(TYPE_ETHERNET, 0, "Ethernet", "");
        call(info, "setIsAvailable", true);
        // CONNECTING until ConnectivityService has connected its channel,
        // then CONNECTED, as NetworkAgent.markConnected() does.
        call(info, "setDetailedState", detailed("CONNECTING"), null, null);

        Class<?> lpClass = Class.forName("android.net.LinkProperties");
        Object lp = lpClass.getConstructor().newInstance();
        call(lp, "setInterfaceName", iface);
        Object la = Class.forName("android.net.LinkAddress").getConstructor(String.class).newInstance(addr);
        call(lp, "addLinkAddress", la);
        Class<?> prefix = Class.forName("android.net.IpPrefix");
        Object any = prefix.getConstructor(String.class).newInstance("0.0.0.0/0");
        Object route = Class.forName("android.net.RouteInfo")
                .getConstructor(prefix, InetAddress.class, String.class)
                .newInstance(any, InetAddress.getByName(gateway), iface);
        call(lp, "addRoute", route);
        for (String d : dns) call(lp, "addDnsServer", InetAddress.getByName(d));

        Object nc = Class.forName("android.net.NetworkCapabilities").getConstructor().newInstance();
        call(nc, "addTransportType", TRANSPORT_ETHERNET);
        for (int c : CAPABILITIES) call(nc, "addCapability", c);
        call(nc, "setLinkUpstreamBandwidthKbps", 100000);
        call(nc, "setLinkDownstreamBandwidthKbps", 100000);

        Object config = call(Class.forName("android.net.NetworkAgentConfig$Builder").getConstructor().newInstance(),
                "build");

        HandlerThread thread = new HandlerThread("network-agent");
        thread.start();
        mine = new Messenger(new Handler(thread.getLooper()) {
            @Override
            public void handleMessage(Message msg) {
                try {
                    if (msg.what == CMD_CHANNEL_FULL_CONNECTION) {
                        connectivity = msg.replyTo;
                        send(CMD_CHANNEL_FULLY_CONNECTED, 0, null);
                        call(info, "setDetailedState", detailed("CONNECTED"), null, null);
                        send(EVENT_NETWORK_INFO_CHANGED, 0, info);
                        Log.i(TAG, "ConnectivityService connected to the agent");
                    } else if (msg.what == CMD_CHANNEL_DISCONNECTED) {
                        Log.w(TAG, "ConnectivityService let the network go");
                        connectivity = null;
                    }
                } catch (Exception e) {
                    Log.w(TAG, "message " + msg.what + ": " + e);
                }
            }
        });

        Object icm = call(Class.forName("android.net.IConnectivityManager$Stub"), "asInterface", cs);
        Object network = call(icm, "registerNetworkAgent", mine, info, lp, nc, 50, config, -1);
        Log.i(TAG, "registered " + iface + " (" + addr + ", gateway " + gateway + ", dns " +
                String.join(" ", dns) + "): network " + network);
        System.out.println("NetworkAgentStandIn: network " + network + " on " + iface);
    }

    private static void send(int what, int arg1, Object obj) throws Exception {
        Messenger to = connectivity;
        if (to == null) return;
        Message m = Message.obtain();
        m.what = what;
        m.arg1 = arg1;
        m.obj = obj;
        m.replyTo = mine;               // ConnectivityService finds its agent by this
        to.send(m);
    }
}
