package net.minecraft.client.main;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

// The game classes live in the default package, like unobfuscated-name-free 1.8 jars, so reach them reflectively.
public class Main {
    public static volatile boolean eventsEnabled;
    public static void startEvents() { eventsEnabled = true; }
    public static void main(String[] args) throws Exception {
        Class.forName("fa");  // the game loads its chat classes at startup
        Class<?> mcClass = Class.forName("ave");
        Object mc = mcClass.getMethod("a").invoke(null);
        Method tick = mcClass.getMethod("s");
        Object player = mcClass.getField("h").get(mc);
        Object world = mcClass.getField("f").get(mc);
        ((java.util.List<Object>) world.getClass().getSuperclass().getField("k").get(world)).add(player);
        Object chat = Class.forName("bao").getDeclaredConstructor().newInstance();
        Object controller = Class.forName("bda").getDeclaredConstructor().newInstance();
        Method show = chat.getClass().getMethod("a", Class.forName("eu"), int.class);
        Method send = player.getClass().getMethod("e", String.class);
        Method attack = controller.getClass().getMethod("a", Class.forName("wn"), Class.forName("pk"));
        Constructor<?> text = Class.forName("fa").getConstructor(String.class);
        String[] chats = {"hello world", "secret stuff", "gg"};
        String[] sends = {".help", "normal message", "/home"};
        long end = System.currentTimeMillis() + 60_000;
        int n = 0;
        while (System.currentTimeMillis() < end) {
            tick.invoke(mc);
            if (eventsEnabled && ++n % 50 == 0) {   // the test explicitly starts rounds after loading handlers
                int round = n / 50;
                show.invoke(chat, text.newInstance(chats[round % chats.length]), 0);
                send.invoke(player, sends[round % sends.length]);
                attack.invoke(controller, player, player);
            }
            Thread.sleep(10);
        }
    }
}
