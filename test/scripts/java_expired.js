// ==UMC==
// @id java-expired
// @permissions java
// ==/UMC==
let saved;
java.hook("Interop", "booleanValue", "(Z)Z", e => {
    if (saved) {
        let rejected = false;
        try { saved.proceed(); } catch (error) { rejected = true; }
        if (!rejected) throw Error("expired call was usable");
        java.type("Interop").set("shared", "I", 99);
    }
    saved = e;
});
