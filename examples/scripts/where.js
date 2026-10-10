// Reports your position and facing every few seconds, and says hello in chat once.
let ticks = 0;
let greeted = false;

umc.on("tick", () => {
    const me = game.player();
    if (!me) return;

    if (!greeted) {
        greeted = true;
        game.chat("UMC script loaded on Minecraft " + game.version);
    }
    if (++ticks % 100 === 0) {
        const others = game.players().filter(p => p.uuid !== me.uuid).length;
        umc.log(`${me.name} at ${me.x.toFixed(1)}, ${me.y.toFixed(1)}, ${me.z.toFixed(1)} ` +
                `yaw ${me.yaw.toFixed(0)} pitch ${me.pitch.toFixed(0)} health ${me.health} others ${others}`);
    }
});
