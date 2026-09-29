// The smallest Java program for ART under lxrun (tests/android/run.sh).
public class Hello {
    public static void main(String[] args) {
        System.out.println("Hello from Java on ART, no VM");
        System.out.println("java.vm.name=" + System.getProperty("java.vm.name")
                + " java.vm.version=" + System.getProperty("java.vm.version")
                + " os.arch=" + System.getProperty("os.arch"));
        System.out.println("args=" + args.length);
    }
}
