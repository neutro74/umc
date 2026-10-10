// ==UMC==
// @name Denied
// @id denied
// @permissions player
// ==/UMC==
umc.log("has game.player:", typeof game.player, "chat:", typeof game.chat, "players:", typeof game.players, "ui:", typeof ui);
try { umc.on("render", () => {}); umc.log("render allowed"); } catch (e) { umc.log("render blocked:", e.message); }
