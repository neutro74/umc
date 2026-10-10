package o;

// Stand-in for net.minecraft.client.Minecraft with obfuscated names.
public class a {
    public int c;
    private String state = "idle";

    // Branches, a switch and a try/catch exercise the stack map and alignment rewriting.
    public void b() {
        c++;
        switch (c % 4) {
            case 0: state = "a"; break;
            case 1: state = "b"; break;
            case 2: state = "c"; break;
            default: state = "d";
        }
        try {
            if (state.length() > 5) throw new IllegalStateException();
        } catch (IllegalStateException e) {
            state = "?";
        }
    }

    public static int d(int x, long y, String s) {
        return x * 2 + (int) y + s.length();
    }
}
