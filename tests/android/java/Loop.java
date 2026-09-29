// A CPU loop with a known answer, run in rounds so a JIT has something to
// compile. The same class on the Mac's own JVM gives the reference values
// (tests/android/run.sh compares them).
public class Loop {
    // xorshift + multiply-accumulate over n steps: long arithmetic, shifts
    // and a data-dependent branch, enough live values to use many registers.
    static long work(long seed, int n) {
        long a = seed, b = seed ^ 0x9E3779B97F4A7C15L, c = 1, d = 7, e = 11, f = 13;
        for (int i = 0; i < n; i++) {
            a ^= a << 13; a ^= a >>> 7; a ^= a << 17;
            b += a * 0x2545F4914F6CDD1DL;
            c = c * 31 + (b >>> 3);
            if ((a & 1) == 0) d += c ^ e; else e -= d + f;
            f = (f << 1) ^ (a >>> 11) ^ i;
        }
        return a ^ b ^ c ^ d ^ e ^ f;
    }

    public static void main(String[] args) {
        int n = args.length > 0 ? Integer.parseInt(args[0]) : 20000000;
        int rounds = args.length > 1 ? Integer.parseInt(args[1]) : 5;
        long expect = Long.MIN_VALUE;
        boolean ok = true;
        for (int r = 0; r < rounds; r++) {
            long t0 = System.nanoTime();
            long v = work(42, n);
            long t1 = System.nanoTime();
            if (r == 0) expect = v; else if (v != expect) ok = false;
            System.out.println("round " + r + " n=" + n + " result=" + v
                    + " ms=" + String.format("%.1f", (t1 - t0) / 1e6));
        }
        long s = 0;
        for (int k = 1; k <= 8; k++)
            s ^= work(k, 100000);
        System.out.println("seeds1-8 xor=" + s);
        System.out.println(ok ? "LOOP OK" : "LOOP MISMATCH");
    }
}
