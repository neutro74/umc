// ==UMC==
// @name Events Check
// @id events-check
// @permissions chat combat
// ==/UMC==
let attacks = 0;
umc.on("chat", e => {
    umc.log("saw chat:", e.text);
    if (e.text.includes("secret")) e.cancel();
});
umc.on("send", e => {
    umc.log("saw send:", e.text, e.command);
    if (e.text.startsWith(".help")) e.cancel();
});
umc.on("attack", e => {
    attacks++;
    umc.log("saw attack on", e.target.name);
    if (attacks % 2 === 0) e.cancel();
});
