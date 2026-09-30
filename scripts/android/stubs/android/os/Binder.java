// Compile-time stand-in (see IBinder.java).
package android.os;

public class Binder implements IBinder {
    public String getInterfaceDescriptor() { throw new RuntimeException("stub"); }
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) { throw new RuntimeException("stub"); }
}
