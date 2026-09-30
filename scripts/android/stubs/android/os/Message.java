// Compile-time stand-in (see IBinder.java).
package android.os;

public final class Message {
    public int what;
    public int arg1;
    public int arg2;
    public Object obj;
    public Messenger replyTo;
    public static Message obtain() { throw new RuntimeException("stub"); }
}
