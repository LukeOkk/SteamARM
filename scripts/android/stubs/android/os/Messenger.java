// Compile-time stand-in (see IBinder.java).
package android.os;

public final class Messenger {
    public Messenger(Handler target) { throw new RuntimeException("stub"); }
    public void send(Message message) throws RemoteException { throw new RuntimeException("stub"); }
}
