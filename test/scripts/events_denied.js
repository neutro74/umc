// ==UMC==
// @name Events Denied
// @id events-denied
// @permissions player
// ==/UMC==
for (const event of ["chat", "send", "attack"]) {
    try { umc.on(event, () => {}); umc.log(event, "allowed"); } catch (e) { umc.log(event, "blocked:", e.message); }
}
