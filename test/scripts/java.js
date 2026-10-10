// ==UMC==
// @id java-check
// @permissions java
// ==/UMC==
function assert(ok, message) { if (!ok) throw Error(message); }
function throws(fn, message) { let threw = false; try { fn(); } catch (e) { threw = true; } assert(threw, message); }
const Type = java.type("Interop");
const object = Type.new("(I)V", 42);
assert(object.get("number", "I") === 42, "constructor and field");
object.set("number", "I", 11);
assert(object.get("number", "I") === 11, "field write");
assert(Type.get("shared", "I") === 7, "static field");
Type.set("shared", "I", 12);
assert(Type.get("shared", "I") === 12, "static field write");
assert(Type.call("add", "(II)I", 3, 4) === 7, "static method");
assert(object.call("echo", "(Ljava/lang/String;)Ljava/lang/String;", "a\u0000😀") === "a\u0000😀", "Unicode");
assert(object.call("longValue", "(J)J", 9007199254740993n) === 9007199254740993n, "lossless long");
assert(object.call("doubleValue", "(D)D", 1.25) === 1.25, "double");
assert(object.call("booleanValue", "(Z)Z", true) === true, "boolean");
throws(() => Type.call("fail", "()V"), "Java exception");
throws(() => object.set("fixed", "I", 1), "final field");
throws(() => object.call("add", "(II)I", 1, 2), "static through instance");
throws(() => Type.call("echo", "(Ljava/lang/String;)Ljava/lang/String;", "a"), "instance through type");
throws(() => Type.call("add", "(II)I", 1), "argument count");
throws(() => Type.call("add", "bad", 1, 2), "malformed descriptor");
throws(() => object.call("echo", "(Ljava/lang/String;)Ljava/lang/String;", object), "reference type mismatch");
throws(() => java.type("does.not.Exist"), "missing class");
for (const [kind, values] of [["Z", [true, false]], ["B", [-2, 3]], ["C", [65, 66]], ["S", [-200, 300]], ["I", [1, 2]], ["J", [1n, 9007199254740993n]], ["F", [1.5, 2.5]], ["D", [1.25, 2.75]], ["Ljava/lang/String;", ["a", "😀"]]]) {
    const array = java.array(kind, values);
    assert(array.length() === 2 && array.at(0) === values[0], "array " + kind);
    array.put(0, values[1]);
    assert(array.at(0) === values[1], "array write " + kind);
    throws(() => array.at(-1), "array bounds");
}
const nested = java.array("[I", [java.array("I", [5])]);
assert(nested.at(0).at(0) === 5, "nested arrays");
// The mapped legacy class must accept readable names on both its type and instance.
const Player = java.type("net.minecraft.client.entity.EntityPlayerSP");
const player = Player.new("()V");
throws(() => player.get("posX", "J"), "mapped field descriptor mismatch");
player.call("sendChatMessage", "(Ljava/lang/String;)V", "mapped bridge");
java.hook("Interop", "add", "(II)I", e => { java.type("Interop").set("shared", "I", e.args[0]); e.return(123); });
java.hook("Interop", "wrapped", "(I)I", e => { e.setArg(0, e.args[0] + 1); e.return(e.proceed() + 10); });
java.hook("Interop", "add", "(II)I", e => {});
const temporary = java.hook("Interop", "doubleValue", "(D)D", e => { e.return(0); });
assert(java.unhook(temporary) && !java.unhook(temporary), "explicit hook removal");
java.hook("Interop", "fail", "()V", e => {
    try { e.proceed(); } catch (error) { Type.set("shared", "I", 44); }
});
umc.on("tick", () => {});
umc.log("Java bridge checks passed");
