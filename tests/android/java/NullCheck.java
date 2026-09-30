// ART's implicit null checks under FEX (benchmarks/stage28-android-apk.txt):
// compiled code (JIT or dex2oat) reads a field through a null reference
// without testing it; the load faults and ART's SIGSEGV handler turns the
// fault into a NullPointerException. The interpreter tests explicitly. Run
// both ways: dalvikvm64 -Xint, and the default (JIT after warm-up) or AOT.
public class NullCheck {
    int value = 1;

    static int read(NullCheck n) {
        return n.value;               // implicit null check in compiled code
    }

    public static void main(String[] args) {
        int rounds = args.length > 0 ? Integer.parseInt(args[0]) : 20000;
        NullCheck some = new NullCheck();
        long sum = 0;
        int caught = 0;
        for (int i = 0; i < rounds; i++) {
            try {
                sum += read((i % 100) == 99 ? null : some);
            } catch (NullPointerException e) {
                caught++;
            }
        }
        System.out.println("NullCheck: sum " + sum + " caught " + caught + " of " + rounds / 100);
    }
}
