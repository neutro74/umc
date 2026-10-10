let n = 0;
umc.on("tick", () => {
    n++;
    if (n % 50 === 0) umc.log("ticks", n);
});
umc.log("sandbox:", typeof require, typeof std, typeof os, typeof fetch, typeof process, typeof XMLHttpRequest);
