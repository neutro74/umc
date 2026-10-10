// ==UMC==
// @name         Installable Test
// @id           coords-hud
// @version      1.0.0
// @author       tests
// @description  Reads the player on each tick.
// @permissions  player render
// ==/UMC==
umc.on("tick", () => { game.player(); });
