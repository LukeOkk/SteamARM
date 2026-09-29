// Where ART put its heap, seen from Java (tests/android/run.sh,
// benchmarks/stage25-art-x86-fex.txt). ART stores object references in 32
// bits, the low half of the object's address (no heap poisoning in this
// build), so the reference in an Object[] slot, read as an int through
// sun.misc.Unsafe, is the object's address; ART needs it below 4 GiB. On
// x86-64 under FEX that is a guest address in FEX's low window.
// Unsafe is reached by reflection only: javac --release 8 does not expose it.
import java.lang.reflect.Field;
import java.lang.reflect.Method;

public class HeapRef {
    public static void main(String[] args) throws Exception {
        Class<?> uc = Class.forName("sun.misc.Unsafe");
        Field f = uc.getDeclaredField("theUnsafe");
        f.setAccessible(true);
        Object u = f.get(null);
        Method baseOf = uc.getMethod("arrayBaseOffset", Class.class);
        Method getInt = uc.getMethod("getInt", Object.class, long.class);
        Object[] slot = new Object[1];
        long base = ((Integer) baseOf.invoke(u, Object[].class)).longValue();
        long max = 0;
        StringBuilder sb = new StringBuilder();
        Object[] kinds = { new Object(), new byte[16], new byte[4 << 20], "a string", HeapRef.class, Object.class };
        String[] names = { "Object", "byte[16]", "byte[4 MiB]", "String literal", "Class", "Object.class (boot image)" };
        for (int i = 0; i < kinds.length; i++) {
            slot[0] = kinds[i];
            long ref = ((Integer) getInt.invoke(u, slot, base)).intValue() & 0xffffffffL;
            max = Math.max(max, ref);
            sb.append(names[i]).append(" at 0x").append(Long.toHexString(ref)).append('\n');
        }
        System.out.print(sb);
        System.out.println(max != 0 && max < (1L << 32) ? "HEAP BELOW 4 GiB" : "HEAP NOT BELOW 4 GiB");
    }
}
