// Compile-time stand-in (see IBinder.java).
package android.os;

public final class ServiceManager {
    public static void addService(String name, IBinder service) { throw new RuntimeException("stub"); }
}
