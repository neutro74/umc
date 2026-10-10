// ==UMC==
// @name Legacy Check
// @id legacy-check
// @permissions player players chat
// ==/UMC==
let reported = false;
umc.on("tick", () => {
    if (reported) return;
    reported = true;
    const me = game.player();
    umc.log("legacy player:", me.name, me.x, me.y, me.z, me.yaw, me.pitch, me.health, me.onGround, me.id);
    umc.log("legacy players:", game.players().length);
    umc.log("legacy chat:", game.chat("hello from umc"));
});
