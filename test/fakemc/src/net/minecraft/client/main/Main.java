package net.minecraft.client.main;

import o.a;

public class Main {
    public static void main(String[] args) throws Exception {
        a mc = new a();
        long end = System.currentTimeMillis() + 60_000;
        int last = -1;
        while (System.currentTimeMillis() < end) {
            mc.b();
            if (mc.c != last && mc.c % 20 == 0) System.out.println("tick " + mc.c + " score " + a.d(5, 10L, "abc"));
            last = mc.c;
            Thread.sleep(10);
        }
    }
}
