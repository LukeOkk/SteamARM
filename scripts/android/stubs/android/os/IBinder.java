// Compile-time stand-in for the framework class, with only what SteamARM's
// Java stand-ins call. The dex is built without it; the image's real class is
// used at run time.
package android.os;

public interface IBinder {
    int FIRST_CALL_TRANSACTION = 0x00000001;
    int INTERFACE_TRANSACTION = ('_' << 24) | ('N' << 16) | ('T' << 8) | 'F';
    int FLAG_ONEWAY = 0x00000001;
}
