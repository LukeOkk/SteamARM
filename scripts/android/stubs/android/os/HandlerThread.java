// Compile-time stand-in (see IBinder.java).
package android.os;

public class HandlerThread extends Thread {
    public HandlerThread(String name) { throw new RuntimeException("stub"); }
    public Looper getLooper() { throw new RuntimeException("stub"); }
}
