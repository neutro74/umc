public class Interop {
    public int number = 5;
    public static int shared = 7;
    public final int fixed = 9;
    public Interop() {}
    public Interop(int n) { number = n; }
    public static int add(int a, int b) { return a + b; }
    public static int wrapped(int n) { return n * 2; }
    public String echo(String s) { return s; }
    public long longValue(long n) { return n; }
    public double doubleValue(double n) { return n; }
    public boolean booleanValue(boolean n) { return n; }
    public static void fail() { throw new IllegalArgumentException("fixture failure"); }
}
