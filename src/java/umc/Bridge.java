package umc;

/**
 * Defined into the bootstrap loader by the agent so every class loader can see it.
 * Hooked methods call {@link #enter} first and return early unless it hands back CONTINUE.
 */
public final class Bridge {
    public static final Object CONTINUE = new Object();

    private Bridge() {}

    public static native Object enter(int hook, Object self, Object[] args);

    public static int asInt(Object o) {
        if (o instanceof Number) return ((Number) o).intValue();
        if (o instanceof Boolean) return ((Boolean) o) ? 1 : 0;
        if (o instanceof Character) return (Character) o;
        return 0;
    }

    public static long asLong(Object o) {
        return o instanceof Number ? ((Number) o).longValue() : asInt(o);
    }

    public static float asFloat(Object o) {
        return o instanceof Number ? ((Number) o).floatValue() : asInt(o);
    }

    public static double asDouble(Object o) {
        return o instanceof Number ? ((Number) o).doubleValue() : asInt(o);
    }
}
