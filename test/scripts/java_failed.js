// ==UMC==
// @id java-failed
// @permissions java
// ==/UMC==
java.hook("Interop", "add", "(II)I", e => { e.return(999); });
throw Error("load failure fixture");
