// Compile-time stand-in (see IBinder.java).
package android.os;

public class Handler {
    public Handler(Looper looper) { throw new RuntimeException("stub"); }
    public void handleMessage(Message msg) { throw new RuntimeException("stub"); }
}
