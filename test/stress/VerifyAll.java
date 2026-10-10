import java.nio.file.*;
import java.util.stream.*;

// Loads and links every class under a directory; linking runs the bytecode verifier.
public class VerifyAll {
    public static void main(String[] args) throws Exception {
        Path root = Paths.get(args[0]);
        int ok = 0, verify = 0, other = 0;
        for (Path p : Files.walk(root).filter(f -> f.toString().endsWith(".class")).collect(Collectors.toList())) {
            String name = root.relativize(p).toString().replace('/', '.').replaceAll("\\.class$", "");
            if (name.startsWith("umc.") || name.endsWith("module-info")) continue;
            try {
                Class.forName(name, false, VerifyAll.class.getClassLoader()).getDeclaredMethods();
                ok++;
            } catch (VerifyError | ClassFormatError e) {
                if (verify++ < 15) System.out.println("VERIFY " + name + ": " + e.getMessage());
            } catch (Throwable t) {
                other++;  // missing game libraries etc., not our concern
            }
        }
        System.out.println("linked " + ok + ", verify errors " + verify + ", other " + other);
    }
}
