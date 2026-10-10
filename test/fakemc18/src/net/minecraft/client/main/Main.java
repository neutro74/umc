package net.minecraft.client.main;

import java.lang.reflect.Method;

// The game classes live in the default package, like unobfuscated-name-free 1.8 jars, so reach them reflectively.
public class Main {
    public static void main(String[] args) throws Exception {
        Class.forName("fa");  // the game loads its chat classes at startup
        Class<?> mcClass = Class.forName("ave");
        Object mc = mcClass.getMethod("a").invoke(null);
        Method tick = mcClass.getMethod("s");
        Object player = mcClass.getField("h").get(mc);
        Object world = mcClass.getField("f").get(mc);
        ((java.util.List<Object>) world.getClass().getSuperclass().getField("k").get(world)).add(player);
        long end = System.currentTimeMillis() + 60_000;
        while (System.currentTimeMillis() < end) { tick.invoke(mc); Thread.sleep(10); }
    }
}
