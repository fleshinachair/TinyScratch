/*
 * TinyScratch - the Waveform screen: the sample on its own, full size.
 *
 *   top      the sample's name, the speed of the playhead (1.0X is the sample at its own pitch, a minus is backwards)
 *            and where the playhead is
 *   middle   the whole waveform, a column to every pixel: the body of the sound solid, the peaks drawn round it as one
 *            outline, the playhead cut through it. A bar above it lights while the sound is cut in
 *   bottom   a mark for where the clip is sending the playhead, and a ruler in tenths of the sample
 *
 * The DSP serves the picture as one JSON string ("view"); this file only draws it.
 */
(function () {
    "use strict";

    var FONT = {
        " ": [0, 0, 0, 0, 0], "0": [7, 5, 5, 5, 7], "1": [2, 6, 2, 2, 7], "2": [7, 1, 7, 4, 7],
        "3": [7, 1, 3, 1, 7], "4": [5, 5, 7, 1, 1], "5": [7, 4, 7, 1, 6], "6": [3, 4, 7, 5, 7],
        "7": [7, 1, 2, 2, 2], "8": [7, 5, 7, 5, 7], "9": [7, 5, 7, 1, 6],
        "A": [2, 5, 7, 5, 5], "B": [6, 5, 6, 5, 6], "C": [3, 4, 4, 4, 3], "D": [6, 5, 5, 5, 6],
        "E": [7, 4, 6, 4, 7], "F": [7, 4, 6, 4, 4], "G": [3, 4, 5, 5, 3], "H": [5, 5, 7, 5, 5],
        "I": [7, 2, 2, 2, 7], "J": [1, 1, 1, 5, 2], "K": [5, 5, 6, 5, 5], "L": [4, 4, 4, 4, 7],
        "M": [5, 7, 7, 5, 5], "N": [6, 5, 5, 5, 5], "O": [2, 5, 5, 5, 2], "P": [6, 5, 6, 4, 4],
        "Q": [2, 5, 5, 6, 3], "R": [6, 5, 6, 5, 5], "S": [3, 4, 2, 1, 6], "T": [7, 2, 2, 2, 2],
        "U": [5, 5, 5, 5, 7], "V": [5, 5, 5, 5, 2], "W": [5, 5, 7, 7, 5], "X": [5, 5, 2, 5, 5],
        "Y": [5, 5, 2, 2, 2], "Z": [7, 1, 2, 4, 7],
        "-": [0, 0, 7, 0, 0], ".": [0, 0, 0, 0, 2], "/": [1, 1, 2, 4, 4], "+": [0, 2, 7, 2, 0],
        ":": [0, 2, 0, 2, 0], "?": [6, 1, 2, 0, 2], "_": [0, 0, 0, 0, 7], "%": [5, 1, 2, 4, 5]
    };

    /* the host puts a header and a footer round a page, so this canvas is 128 x 46: nothing below row 45 is seen */
    var TOP = 9, H = 29, MID = TOP + 14, HALF = 13;      /* the band: rows 9..37 */

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function num(v, dflt) { v = Number(v); return isFinite(v) ? v : dflt; }
    function hex2(s, i) { var v = parseInt(String(s).substr(i * 2, 2), 16); return v >= 0 ? v / 255 : 0; }

    function parse(raw) {
        if (!raw || typeof raw !== "string") return null;
        try { var o = JSON.parse(raw); return o && typeof o === "object" ? o : null; } catch (e) { return null; }
    }

    function px(ctx, x, y) {
        x = Math.round(x); y = Math.round(y);
        if (x >= 0 && x < 128 && y >= 0 && y < 64) ctx.setPixel(x, y, 1);
    }

    function text(ctx, x, y, s) {
        var str = String(s).toUpperCase();
        for (var i = 0; i < str.length; i++) {
            var rows = FONT[str[i]] || FONT["?"];
            for (var r = 0; r < 5; r++)
                for (var b = 0; b < 3; b++)
                    if (rows[r] & (4 >> b)) px(ctx, x + i * 4 + b, y + r);
        }
    }

    function line(ctx, x0, y0, x1, y1) {
        x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
        var dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (var n = 0; n < 200; n++) {
            px(ctx, x0, y0);
            if (x0 === x1 && y0 === y1) break;
            var e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    function drawWaveform(ctx, v) {
        var name = typeof v.n === "string" ? v.n.slice(0, 11) : "";
        text(ctx, 1, 1, name);
        var pct = Math.round(clamp(num(v.p, 0), 0, 1) * 100) + "%";
        text(ctx, 127 - pct.length * 4, 1, pct);
        var spd = num(v.vl, 0), mag = Math.abs(spd);
        text(ctx, 52, 1, (spd < -0.05 ? "-" : "") + (mag > 99 ? "99" : mag.toFixed(1)) + "X");

        var pk = typeof v.w === "string" ? v.w : "", rm = typeof v.wr === "string" ? v.wr : "";
        var ptop = MID, pbot = MID;
        for (var c = 0; c < 128; c++) {
            var a = Math.round(hex2(pk, c) * HALF), b = Math.min(a, Math.round(hex2(rm, c) * HALF));
            if (c % 3 === 0) px(ctx, c, MID);                                       /* the centre line, dotted, where it is quiet */
            if (b > 0) ctx.fillRect(c, MID - b, 1, b * 2 + 1, 1);
            var top = MID - a, bot = MID + a;
            if (c > 0) { line(ctx, c - 1, ptop, c, top); line(ctx, c - 1, pbot, c, bot); }
            else { px(ctx, c, top); px(ctx, c, bot); }
            ptop = top; pbot = bot;
        }

        var x = Math.round(clamp(num(v.x, 0), 0, 127));                             /* the playhead */
        ctx.fillRect(x - 1, TOP, 3, H, 0);
        ctx.fillRect(x, TOP - 2, 1, H + 2, 1);
        ctx.fillRect(x - 2, TOP - 2, 5, 1, 1);
        ctx.fillRect(x - 2, TOP + H - 1, 5, 1, 1);

        if (v.g) ctx.fillRect(0, 7, 128, 1, 1);                                     /* the sound is cut in */

        if (num(v.cl, 0) > 0) {                                                     /* where it is being sent */
            var t = Math.round(clamp(num(v.tx, 0), 0, 127));
            ctx.fillRect(t - 2, TOP + H, 5, 1, 1);
            ctx.fillRect(t - 1, TOP + H + 1, 3, 1, 1);
            px(ctx, t, TOP + H + 2);
        }
        for (var i = 0; i <= 10; i++) {                                             /* the ruler: tenths of the sample */
            var rx = Math.round(i * 127 / 10), tall = i % 5 === 0;
            px(ctx, rx, 43);
            if (tall) { px(ctx, rx, 44); px(ctx, rx, 42); }
        }
    }

    globalThis.canvas_overlay = {
        drawPage: function (ctx, args) {
            try {
                var v = parse(args && args.values ? args.values.view : null);
                if (!v) { text(ctx, 2, 20, "TINYSCRATCH: WAITING FOR AUDIO"); return; }
                drawWaveform(ctx, v);
            } catch (e) {
                /* Schwung switches a canvas script off for good the first time a hook throws */
                try { ctx.clear(); text(ctx, 0, 0, "ERR"); text(ctx, 0, 8, String(e && e.message ? e.message : e).slice(0, 31)); } catch (e2) { /* nothing left to try */ }
            }
        }
    };

    globalThis.__tinywave = { parse: parse, drawWaveform: drawWaveform };
})();
