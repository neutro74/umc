// ==UMC==
// @name         Coords HUD
// @id           coords-hud
// @version      1.0.0
// @author       neutro74
// @description  A small panel with your coordinates and the direction you're facing.
// @permissions  player render
// ==/UMC==
// A small HUD panel with your coordinates, facing and FPS-independent tick counter.
let ticks = 0;
umc.on("tick", () => { ticks++; });

const compass = yaw => ["S", "SW", "W", "NW", "N", "NE", "E", "SE"][Math.round((((yaw % 360) + 360) % 360) / 45) % 8];

umc.on("render", () => {
    const me = game.player();
    if (!me) return;
    const lines = [
        "UMC",
        `XYZ ${me.x.toFixed(1)} ${me.y.toFixed(1)} ${me.z.toFixed(1)}`,
        `Facing ${compass(me.yaw)}  (${me.yaw.toFixed(0)}, ${me.pitch.toFixed(0)})`,
        `Tick ${ticks}`,
    ];
    const width = Math.max(...lines.map(l => ui.width(l))) + 10;
    ui.rect(4, 4, width, lines.length * 11 + 6, 0x90101418);
    ui.rect(4, 4, 2, lines.length * 11 + 6, 0xFFF5A524);
    lines.forEach((l, i) => ui.text(l, 10, 8 + i * 11, i === 0 ? 0xFFF5A524 : 0xFFE6E8EB));
});
