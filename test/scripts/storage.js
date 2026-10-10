// ==UMC==
// @name Storage Check
// @id storage-check
// @permissions storage
// ==/UMC==
const attempt = (label, fn) => {
    try { umc.log(label, "->", JSON.stringify(fn())); }
    catch (e) { umc.log(label, "-> blocked:", e.message); }
};
attempt("write", () => fs.write("notes.txt", "hello"));
attempt("append", () => fs.append("notes.txt", " world"));
attempt("read", () => fs.read("notes.txt"));
attempt("read missing", () => fs.read("nope.txt"));
attempt("exists", () => fs.exists("notes.txt"));
attempt("list", () => fs.list());
attempt("parent dir", () => fs.read("../secret"));
attempt("subfolder", () => fs.write("a/b.txt", "x"));
attempt("absolute", () => fs.read("/etc/passwd"));
attempt("hidden", () => fs.write(".hidden", "x"));
attempt("dotdot name", () => fs.write("a..b", "x"));
attempt("too big", () => fs.write("big.bin", "x".repeat(2 * 1024 * 1024)));
attempt("remove", () => fs.remove("notes.txt"));
attempt("gone", () => fs.exists("notes.txt"));
