/*
 * TinyScratch - the main screen.
 *
 *   top      the name of the sample in use, in big letters (and under the vinyl a dot for each of the eight slots)
 *   middle   the waveform (the body, with the peaks around it) and the playhead through it, and the vinyl: its white
 *            line goes once round as the playhead crosses the sample
 *   lane     the clip view: one bar, a column each, like the clip in Ableton. The playhead's path as a line, the cuts
 *            as blocks, a cursor for now. With a clip it is the clip's plan; with none, what the playhead just did
 *   bottom   what is set (as the module's page there is no room: a small panel beside the lane says it)
 *
 * Two ways in. As the module's first page (drawPage) it only draws. Opened full screen (the Pads knob) it also owns the
 * pads, so it can colour them, and it passes the eight knobs on: the pad grid is
 *     top row      clips 1-8        third row   Ahhh, Fresssh, User 1-4, Auto (the clips in turn), no clip
 *     bottom two   cuts
 * The DSP serves the picture as one JSON string ("view"); this file only draws it.
 */
(function () {
    "use strict";

    var STYLES = ["IMPROV", "BABY", "CUTS", "MILITARY", "SCRIBBLE", "STAB", "CHIRP", "TRANSFORM", "FLARE", "CRAB", "ORBIT", "DRAG", "TEAR"];
    var CLIP_PADS = 14;                                 /* clip pads 1-14; the next two places are Generate (15) and Auto (16) */
    var PAGES = 4;                                      /* pages of clip pads: < and > turn them (the Move's left and right buttons, CC 62 and 63) */
    var LEFT_CC = 62, RIGHT_CC = 63;
    var GROOVES = ["BOOM", "FLOOR", "BRKS", "HALF", "FUNK", "DEMBO", "OPEN"];     /* the seven grooves, as the engine numbers them */
    var PAGE_CLIP = [23, 16, 26, 8];                    /* a clip pad's colour on each page (the palette: purple, azure, magenta, yellow) */
    var CLIP_AUTO = 17, CLIP_TRY = 18;               /* the clip parameter: 0 off, 1..16 a clip, 17 auto, 18 the clip just generated */
    var HOLD_MS = 1500;                                /* hold a clip you made, or a User sample, this long to delete / unload it (the Delete button never reaches a screen) */
    var TOAST_MS = 1600;                               /* how long a dial's new value stays on the screen */
    var STYLE_SHORT = ["IMPRO", "BABY", "CUTS", "MILIT", "SCRIB", "STAB", "CHIRP", "TRANS", "FLARE", "CRAB", "ORBIT", "DRAG", "TEAR"];
    var BARS = [1, 2, 4];
    var BACK_CC = 51;                                  /* the Move's Back button */
    var JOG_CC = 14;                                   /* the jog wheel: right zooms the clip view in, left out */
    var STALE_MS = 1500;                               /* not drawn for this long: this screen is no longer the one showing */

    var PAD_FIRST = 68;                 /* the pads as raw ids; a track may send them as 36..67 instead */
    var DRUM_FIRST = 36;
    var KNOB_CC = 71;                   /* the eight knobs */
    var RAW_PERIOD_MS = 400;                            /* how often the full screen says it is there (the DSP waits 1.5 s) */
    var FIGHT_TICKS = 8;                 /* a pad the Move has just lit itself (it flashes a pressed pad, and its twin an octave away) is put back for this many ticks */
    var LEDS_PER_TICK = 8, LED_PERIOD_MS = 20;      /* Schwung: more than ~60 LED packets in a frame overflows the USB buffer */

    /* Move palette indices (Schwung shared/constants.mjs) */
    /* Move palette indices: the numbers are indices into the Move's own colour table, which is not the order of Schwung's names for
     * them (checked against what the Move really lit: 120 white, 127 red, 7 yellow, 123 grey, 11 and 13 greens, 23 purple, 3 orange). */
    var COL = { off: 0, white: 120, cut: 127, cutHot: 120, clip: 23, clipNow: 13, sample: 7, sampleEmpty: 123, clipAuto: 11, clipNo: 127, gen: 3, cue: 13 };

    /* The eight dials on the Pads screen, in the order that reads: what Generate makes (Style, Length), then how it plays (Drag, Humanise),
     * then what Generate adds (Groove, Echo, Forwards, Finish). Swing is on the Playback page. The sample, the clip and Generate are on the pads; Load Sample and the rest of
     * the settings (Bite, Clip Depth, Gain, ...) are on the module's own pages. */
    var KNOBS = [
        { key: "gen_style", v: "gs", min: 0, max: 12, steps: true, detents: 2 },
        { key: "gen_bars", v: "gb", min: 0, max: 2, steps: true, detents: 3 },
        { key: "drag", v: "dr", min: 0, max: 1, step: 0.02 },                          /* how quickly the playhead follows: digital to analogue */
        { key: "humanise", v: "hu", min: 0, max: 1, step: 0.02 },                      /* every clip, as it plays: looser timing, cuts, strokes */
        { key: "gen_groove", v: "gg", min: 0, max: 1, step: 0.05 },                    /* how far Improv keeps to a drum groove: room for a beat, rests, a fill */
        { key: "gen_echo", v: "ec", min: 0, max: 1, step: 0.05 },                      /* how often a bar holds an echo scratch: a stab four times, each lower */
        { key: "gen_fwd", v: "gf", min: 0, max: 1, step: 0.05 },                       /* how often Improv lets the record go through the word */
        { key: "gen_fin", v: "fn", min: 0, max: 1, step: 0.05 }                        /* how often a clip ends with the whole sample played through */
    ];

    /* ---- a 3x5 font, so a sample name and a waveform fit on 128x64 ---- */
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

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function num(v, dflt) { v = Number(v); return isFinite(v) ? v : dflt; }
    function fit(s, chars) { s = String(s); return s.length > chars ? s.slice(0, chars) : s; }
    function hex2(s, i) { var v = parseInt(String(s).substr(i * 2, 2), 16); return v >= 0 ? v / 255 : 0; }

    function parse(raw) {
        if (!raw || typeof raw !== "string") return null;
        try { var o = JSON.parse(raw); return o && typeof o === "object" ? o : null; } catch (e) { return null; }
    }

    function px(ctx, x, y, c) {
        x = Math.round(x); y = Math.round(y);
        if (x >= 0 && x < 128 && y >= 0 && y < 64) ctx.setPixel(x, y, c === undefined ? 1 : c);
    }

    function text(ctx, x, y, s, on) {
        var str = String(s).toUpperCase(), c = on === undefined ? 1 : on;
        for (var i = 0; i < str.length; i++) {
            var rows = FONT[str[i]] || FONT["?"];
            for (var r = 0; r < 5; r++)
                for (var b = 0; b < 3; b++)
                    if (rows[r] & (4 >> b)) px(ctx, x + i * 4 + b, y + r, c);
        }
    }

    function line(ctx, x0, y0, x1, y1, c) {
        x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
        var dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (var n = 0; n < 400; n++) {
            px(ctx, x0, y0, c);
            if (x0 === x1 && y0 === y1) break;
            var e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    function box(ctx, x, y, w, h) {
        ctx.fillRect(x, y, w, 1, 1); ctx.fillRect(x, y + h - 1, w, 1, 1);
        ctx.fillRect(x, y, 1, h, 1); ctx.fillRect(x + w - 1, y, 1, h, 1);
    }

    function circle(ctx, cx, cy, r) {
        for (var i = 0; i < 48; i++) {
            var a = i * Math.PI * 2 / 48;
            px(ctx, cx + r * Math.cos(a), cy + r * Math.sin(a));
        }
    }

    /* ---- the picture ---- */

    /* the sample in use, as a title in double-size letters, and under the vinyl a dot for each of the eight slots: a block for
     * the one in use, a pixel for each other that has a sample */
    function textBig(ctx, x, y, s) {
        var str = String(s).toUpperCase();
        for (var i = 0; i < str.length; i++) {
            var rows = FONT[str[i]] || FONT["?"];
            for (var r = 0; r < 5; r++)
                for (var b = 0; b < 3; b++)
                    if (rows[r] & (4 >> b)) ctx.fillRect(x + i * 8 + b * 2, y + r * 2, 2, 2, 1);
        }
    }

    function drawTitle(ctx, v) {
        if (num(v.rc, 0)) {                                       /* recording: REC and the seconds so far, and the level of the input */
            textBig(ctx, 0, 0, "REC " + (clamp(num(v.rt, 0), 0, 99) / 10).toFixed(1));
            ctx.fillRect(0, 10, Math.round(clamp(num(v.rl, 0), 0, 100) * 1.05), 1, 1);
        } else {
            textBig(ctx, 0, 0, fit(typeof v.n === "string" && v.n ? v.n : "EMPTY", 13));
        }
        var loaded = typeof v.ld === "string" ? v.ld : "", cur = clamp(Math.round(num(v.s, 0)), 0, 7);
        for (var i = 0; i < 8; i++) {
            var x = 111 + i * 2;
            if (i === cur) ctx.fillRect(x - 1, 23, 3, 3, 1);
            else if (loaded[i] === "1") px(ctx, x, 24);
        }
    }

    /* Two layouts. As the module's page the picture is a canvas inside the host's header and footer: 128 x 46, with no room
     * for a status line, so the lane gives up its right side to a small panel. Full screen it has all 128 x 64. */
    var FULL = { waveTop: 11, waveH: 20, vinylY: 10, laneY: 32, laneH: 26, laneW: 128, blockY: 34, blockH: 4, pathHi: 41, pathLo: 55, tickY: 56 };
    var PAGE = { waveTop: 11, waveH: 18, vinylY: 10, laneY: 30, laneH: 16, laneW: 84, blockY: 31, blockH: 2, pathHi: 34, pathLo: 42, tickY: 44 };
    var WAVE_X = 0, WAVE_W = 106;

    /* with no clip playing, the bottom row of pads are cues (the engine says so with cs and the clip being off) */
    function cueMode(v) { return num(v.cm, 0) === 0 && num(v.cs, 0) === 1; }

    /* the waveform: the body of the sound solid, the peaks drawn round it as one outline, and the playhead cut out of it so it always reads */
    function drawWave(ctx, v, L) {
        var pk = typeof v.w === "string" ? v.w : "", rm = typeof v.wr === "string" ? v.wr : "";
        var mid = L.waveTop + Math.floor(L.waveH / 2), half = Math.floor(L.waveH / 2) - 1;
        var ptop = mid, pbot = mid;
        for (var c = 0; c < WAVE_W; c++) {
            var i = Math.min(127, Math.floor(c * 128 / WAVE_W));
            var a = Math.round(hex2(pk, i) * half), b = Math.min(a, Math.round(hex2(rm, i) * half));
            px(ctx, WAVE_X + c, mid);
            if (b > 0) ctx.fillRect(WAVE_X + c, mid - b, 1, b * 2 + 1, 1);
            if (c > 0) { line(ctx, WAVE_X + c - 1, ptop, WAVE_X + c, mid - a); line(ctx, WAVE_X + c - 1, pbot, WAVE_X + c, mid + a); }
            else { px(ctx, WAVE_X + c, mid - a); px(ctx, WAVE_X + c, mid + a); }
            ptop = mid - a; pbot = mid + a;
        }
        var x = WAVE_X + Math.round(clamp(num(v.x, 0), 0, 127) * (WAVE_W - 1) / 127);
        ctx.fillRect(x - 1, L.waveTop, 3, L.waveH, 0);
        ctx.fillRect(x, L.waveTop, 1, L.waveH, 1);
        ctx.fillRect(x - 2, L.waveTop, 5, 1, 1);
        ctx.fillRect(x - 2, L.waveTop + L.waveH - 1, 5, 1, 1);
        if (num(v.cl, 0) > 0) {                               /* where the clip is sending it */
            var t = WAVE_X + Math.round(clamp(num(v.tx, 0), 0, 127) * (WAVE_W - 1) / 127);
            ctx.fillRect(t - 1, L.waveTop + L.waveH, 3, 1, 1);
        } else if (cueMode(v) && typeof v.cu === "string") {  /* where each cue pad drops the needle; the one playing is wider */
            for (var k = 0; k < 8; k++) {
                var cx = WAVE_X + Math.round(clamp(hex2(v.cu, k) * 255, 0, 127) * (WAVE_W - 1) / 127), now = num(v.cq, -1) === k;
                ctx.fillRect(now ? cx - 1 : cx, L.waveTop + L.waveH, now ? 3 : 1, 1, 1);
            }
        }
    }

    /* the vinyl: the white line starts at 12 o'clock and goes once round as the playhead crosses the sample */
    function drawVinyl(ctx, v, cx, cy) {
        circle(ctx, cx, cy, 10);
        circle(ctx, cx, cy, 4);
        px(ctx, cx, cy);
        var a = clamp(num(v.p, 0), 0, 1) * Math.PI * 2 - Math.PI / 2;
        line(ctx, cx + 2 * Math.cos(a), cy + 2 * Math.sin(a), cx + 9 * Math.cos(a), cy + 9 * Math.sin(a));
    }

    /* the clip view: one bar of columns. The playhead's path, the cuts as blocks above it, a cursor, a tick on every beat */
    function drawLane(ctx, v, L) {
        var w = L.laneW, y0 = L.laneY;
        function lx(k) { return Math.round(k * (w - 1) / 127); }
        box(ctx, 0, y0, w, L.laneH);
        var lb = clamp(Math.round(num(v.lb, 4)), 4, 16), cpb = 128 / lb;              /* the clip is 4 to 16 beats long */
        for (var b = 1; b < lb; b++) {                                                  /* a tick on each beat, a taller one on each bar */
            px(ctx, lx(b * cpb), L.tickY); px(ctx, lx(b * cpb), L.tickY - 1);
            if (b % 4 === 0) px(ctx, lx(b * cpb), L.tickY - 2);
        }
        var pos = typeof v.lnp === "string" ? v.lnp : "", gate = typeof v.lng === "string" ? v.lng : "";
        /* With Humanise on the DSP also says what is played this pass (lhp, lhg): the written path becomes a dotted ghost and the cuts a row of
         * ticks above, and the line and the blocks are what you hear, so you can see how far it has moved them. */
        var played = typeof v.lhp === "string" && v.lhp.length >= 128, hpos = played ? v.lhp : pos, hgate = played && typeof v.lhg === "string" ? v.lhg : gate;
        var lo = L.pathLo, hi = L.pathHi, prevY = -1;
        for (var k = 0; k < 128 && k < hpos.length; k++) {
            var y = lo - (parseInt(hpos[k], 16) || 0) * (lo - hi) / 15, x = lx(k);
            if (played && k < pos.length) {
                px(ctx, x, lo - (parseInt(pos[k], 16) || 0) * (lo - hi) / 15);                       /* the path as written, dotted */
                if (gate[k] === "1" && k % 2 === 0) px(ctx, x, L.blockY - 1);                          /* ...and where its cuts were */
            }
            if (k > 0 && prevY >= 0) line(ctx, lx(k - 1), prevY, x, y);
            else px(ctx, x, y);
            prevY = y;
            if (hgate[k] === "1") ctx.fillRect(x, L.blockY, Math.max(1, (k < 127 ? lx(k + 1) : x + 1) - x), L.blockH, 1);      /* a cut: the sound is in */
        }
        var cur = clamp(Math.round(num(v.lcol, -1)), -1, 127);
        if (cur >= 0) for (var yy = y0 + 1; yy < y0 + L.laneH - 1; yy += 2) px(ctx, lx(cur), yy);
    }

    /* the style the clip on the screen was made in (a kept clip keeps its recipe), or -1 when it has none (a built-in one, or hand playing) */
    function clipStyle(v) {
        var cy = Math.round(num(v.cy, -1));
        return num(v.cm, 0) > 0 && cy >= 0 && cy <= 12 ? cy : -1;
    }

    /* The pages of clip pads, a mark each, top right: the page the pads show is solid, a page with clips is a box and an empty one a dot. While the
     * clip playing is on another page its mark blinks, so you know where it is without reading. */
    function drawPages(ctx, v, extra) {
        var pg = clamp(Math.round(num(v.pg, 0)), 0, PAGES - 1), cp = clamp(Math.round(num(v.cp, 0)), 0, PAGES - 1), pf = typeof v.pf === "string" ? v.pf : "";
        var playing = num(v.cm, 0) > 0 && cp !== pg, on = !((extra && extra.now) > 0) || Math.floor((extra.now || 0) / 300) % 2 === 0;
        for (var i = 0; i < PAGES; i++) {
            var x = 108 + i * 5, y = 28;
            if (i === pg) ctx.fillRect(x, y, 4, 4, 1);
            else if (playing && i === cp) { if (on) ctx.fillRect(x, y, 4, 4, 1); else box(ctx, x, y, 4, 4); }
            else if (pf[i] === "1") box(ctx, x, y, 4, 4);
            else px(ctx, x + 1, y + 1);
        }
    }

    function clipLabel(v, plain) {
        var cm = clamp(Math.round(num(v.cm, 0)), 0, 18), cl = clamp(Math.round(num(v.cl, 0)), 0, 18);
        var ln = Math.round(num(v.ln, 0)), tg = clamp(Math.round(num(v.tg, 0)), 0, 12), cy = clipStyle(v);
        var cp = clamp(Math.round(num(v.cp, 0)), 0, PAGES - 1), pgl = cp > 0 ? (cp + 1) + "." : "";                  /* a clip on another page than the first: CLIP 2.3 */
        var head = cm === CLIP_TRY ? "TRY " + STYLES[tg].slice(0, 5) : (cm ? (cm === CLIP_AUTO ? "AUTO " : "CLIP ") + pgl + cl + (cy >= 0 ? " " + STYLE_SHORT[cy] : "") : "HAND");
        return head + (ln > 0 && !plain ? " N" + ln : "");                                          /* N: the last note that arrived */
    }

    /* besides its style, which of Echo, Forwards and Finish are really in the clip (cx, from the generator: a bit each; -1, or none of them: nothing) */
    /* ...and the Groove it was made with, when it was not 0 (GRV60). When the two do not fit beside the label, the extras are initials: E F N. */
    function clipExtras(v, leftLen) {
        var x = Math.round(num(v.cx, -1)), g = clamp(Math.round(num(v.cg, 0)), 0, 100), ct = clamp(Math.round(num(v.ct, 0)), 0, GROOVES.length);
        var words = x < 0 ? [] : [x & 1 ? "ECHO" : "", x & 2 ? "FWD" : "", x & 4 ? "FIN" : ""].filter(Boolean);
        var name = g > 0 && ct > 0 ? GROOVES[ct - 1] : "";                                    /* (GRV60 FUNK: which of the seven grooves it kept to) */
        function build(initials, withName) {
            var parts = initials ? words.map(function (w) { return w[0]; }) : words.slice();
            if (g > 0) parts.push("GRV" + g + (withName && name ? " " + name : ""));
            return parts.join(" ");
        }
        var full = build(false, true);
        if (leftLen !== undefined) {
            var fits = function (t) { return (leftLen + 1 + t.length) * 4 <= 126; };
            if (!fits(full)) full = build(true, true);
            if (!fits(full)) full = build(true, false);
        }
        return full;
    }

    /* the transport is stopped and a clip is waiting for it: nothing plays by itself */
    function waiting(v) { return clamp(Math.round(num(v.cm, 0)), 0, 18) > 0 && !v.ru && !num(v.nh, 0); }

    /* what to say on the bottom line instead of the usual: a dial's new value, or the countdown to a deletion */
    function bottomNotice(ctx, extra, v) {
        if (extra.hold) {
            var f = clamp(((extra.now || 0) - extra.hold.since) / HOLD_MS, 0, 1);
            ctx.fillRect(0, 56, Math.round(128 * f), 2, 1);
            return extra.hold.kind === "sample" ? "HOLD TO UNLOAD USER " + (extra.hold.n - 1) : "HOLD TO DELETE CLIP " + extra.hold.n;
        }
        if (extra.toast) return extra.toast;
        if (extra.touchText) return extra.touchText;
        /* a blank User slot is picked: the file browser is on the module's menu, not something a pad can open */
        if (v && typeof v.ld === "string" && num(v.s, 0) >= 2 && v.ld[num(v.s, 0)] !== "1") return "EMPTY: HOLD ITS PAD TO RECORD";
        return "";
    }

    function drawView(ctx, v, compact, extra) {
        extra = extra || {};
        var pad = extra.pad, cc = extra.cc, leds = extra.leds;
        var L = compact ? PAGE : FULL;
        drawTitle(ctx, v);
        drawWave(ctx, v, L);
        drawVinyl(ctx, v, 117, L.vinylY);
        drawLane(ctx, v, L);
        if (compact) {                                        /* the panel beside the lane */
            text(ctx, 86, L.laneY + 2, clipLabel(v, clipStyle(v) >= 0), 1);
            text(ctx, 86, L.laneY + 9, waiting(v) ? "PRESS PLAY" : (v.g ? "CUT" : ""), 1);
            return;
        }
        drawPages(ctx, v, extra);
        /* P: the last raw pad pressed. RAW: the DSP is reading the pads raw (the Move's octave does not matter); NOTE: it is
         * reading the notes the Move sends for them, which move with the octave. LED: whether the pad lights are being written. */
        var notice = bottomNotice(ctx, extra, v);
        var inv = !!extra.touchText && notice === extra.touchText;                              /* a touched dial's name: white on black stands out */
        if (inv) ctx.fillRect(0, 57, 128, 7, 1);
        var made = clipStyle(v) >= 0;
        text(ctx, 1, 59, notice || (clipLabel(v) + (made ? "" : (pad !== undefined && pad >= 0 ? " P" + pad : "") + (cc !== undefined && cc >= 0 ? " C" + cc : ""))), inv ? 0 : 1);
        /* a clip with a recipe: its style is in the label and here is what else it was made with; otherwise what Generate makes (the dials 1 and 2), then how the pads are being read */
        var right = made ? clipExtras(v, clipLabel(v).length) : (STYLE_SHORT[clamp(Math.round(num(v.gs, 0)), 0, 12)] + " " + BARS[clamp(Math.round(num(v.gb, 1)), 0, 2)] + "B " + (v.rw ? "RAW" : "NOTE") + (leds ? " " + leds : ""));
        if (!notice) text(ctx, 128 - 4 * right.length, 59, right, 1);
    }

    /* The zoomed view (the Pads screen, jog wheel right): a few beats of the clip, scrolling past a fixed "now". The DSP sends
     * the window, a column each: the playhead's path (two hex digits a column) and the cuts. A line for every beat, a solid
     * one on each bar. In Auto the next bar's clip comes in at the bar line. */
    function drawZoom(ctx, v, extra) {
        extra = extra || {};
        var pad = extra.pad, leds = extra.leds;
        var zp = typeof v.zp === "string" ? v.zp : "", zg = typeof v.zg === "string" ? v.zg : "";
        var w = clamp(num(v.zw, 2), 0.5, 8), start = num(v.zs, 0), cur = 32, per = 128 / w;
        text(ctx, 1, 1, clipLabel(v) + "  ZOOM " + (w === Math.round(w) ? String(w) : w.toFixed(1)) + " BEATS", 1);
        box(ctx, 0, 7, 128, 38);
        for (var b = Math.ceil(start - 1e-9); b < start + w; b++) {
            var x = Math.round((b - start) * per), solid = ((b % 4) + 4) % 4 === 0;
            if (x < 1 || x > 126) continue;
            for (var y = 8; y < 44; y += solid ? 1 : 3) px(ctx, x, y);
        }
        var prev = -1, n = Math.min(128, Math.floor(zp.length / 2));
        for (var c = 0; c < n; c++) {
            var val = parseInt(zp.substr(c * 2, 2), 16) / 255;
            if (!(val >= 0)) val = 0;
            var yy = Math.round(42 - val * 33);
            if (c > 0 && prev >= 0) line(ctx, c - 1, prev, c, yy); else px(ctx, c, yy);
            px(ctx, c, yy + 1);
            prev = yy;
        }
        for (var k = 0; k < 128; k++) {                                       /* the notes: a block wherever the sound is cut in */
            px(ctx, k, 54);
            if (zg[k] === "1") ctx.fillRect(k, 47, 1, 8, 1);
        }
        ctx.fillRect(cur - 1, 47, 3, 8, 0);
        for (var j = 8; j < 55; j++) px(ctx, cur, j);                          /* now */
        var now = start + 0.25 * w, bar = Math.floor(now / 4) + 1, beat = Math.floor(((now % 4) + 4) % 4) + 1;
        var notice = bottomNotice(ctx, extra, v);
        var inv = !!extra.touchText && notice === extra.touchText;
        if (inv) ctx.fillRect(0, 57, 128, 7, 1);
        var zl = "BAR " + bar + " BEAT " + beat + (pad !== undefined && pad >= 0 ? "  P" + pad : "");
        text(ctx, 1, 59, notice || zl, inv ? 0 : 1);
        var right = clipStyle(v) >= 0 ? clipExtras(v, zl.length) : STYLE_SHORT[clamp(Math.round(num(v.gs, 0)), 0, 12)] + " " + BARS[clamp(Math.round(num(v.gb, 1)), 0, 2)] + "B " + (leds ? leds : "JOG");
        if (!notice) text(ctx, 128 - 4 * right.length, 59, right, 1);
    }

    /* ---- pad LEDs ---- */

    function padIndex(note) {
        if (note >= PAD_FIRST && note < PAD_FIRST + 32) return note - PAD_FIRST;
        if (note >= DRUM_FIRST && note < DRUM_FIRST + 32) return note - DRUM_FIRST;
        return -1;
    }

    /* the grid as the DSP reads it: the bottom row cuts; the second row the samples, Auto and Record; the top two rows the
     * 15 clips of the selected sample (1-8 on the top row; an empty pad is dark: it is where a generated clip is kept) and Generate.
     * A clip you made, held, goes red until it is deleted. */
    function padColor(st, idx) {
        var row = Math.floor(idx / 8), col = idx % 8, v = st.view || {}, cm = num(v.cm, 0);
        if (row === 0) return st.held[idx] ? COL.cutHot : (cueMode(v) ? COL.cue : COL.cut);
        if (row === 1) {
            {
                if (st.hold && st.hold.ok && st.hold.kind === "sample" && st.hold.n === col) return COL.cut;     /* held to unload: red */
                if (st.recHold === idx && num(v.rc, 0)) return COL.cut;                           /* held to record: red while it records */
                if (num(v.s, 0) === col) return COL.white;
                return typeof v.ld === "string" && v.ld[col] === "1" ? COL.sample : COL.sampleEmpty;
            }
            return COL.off;                                                           /* (the last two pads of this row are not used) */
        }
        var n = (3 - row) * 8 + col + 1, cf = typeof v.cf === "string" ? v.cf : "", kp = typeof v.kp === "string" ? v.kp : "";
        if (n === CLIP_PADS + 1) return cm === CLIP_TRY ? COL.white : COL.gen;         /* Generate */
        if (n > CLIP_PADS + 1) return cm === CLIP_AUTO ? COL.white : COL.clipAuto;    /* Auto, the last pad of the clip rows */
        if (cf[n - 1] !== "1") return COL.off;                                        /* an empty pad is dark, until a clip is kept on it */
        if (st.hold && st.hold.ok && st.hold.kind === "clip" && st.hold.n === n) return COL.cut;     /* held to delete: red */
        var pg = clamp(Math.round(num(v.pg, 0)), 0, PAGES - 1), here = clamp(Math.round(num(v.cp, 0)), 0, PAGES - 1) === pg;      /* (a clip playing on another page is not lit on this one) */
        if (cm === n && here) return COL.white;
        return cm === CLIP_AUTO && here && num(v.cl, 0) === n ? COL.clipNow : PAGE_CLIP[pg];      /* in auto, the one playing now */
    }

    function flushLeds(st) {
        var send = globalThis.move_midi_internal_send;
        if (typeof send !== "function") { st.ledNone++; return 0; }
        var sent = 0;
        for (var k = 0; k < 32 && sent < LEDS_PER_TICK; k++) {
            var i = (st.ledScan + k) % 32, want = padColor(st, i);
            if (st.led[i] === want) continue;
            var ok = false;
            try { ok = send([0x09, 0x90, PAD_FIRST + i, want]); } catch (e) { ok = false; }
            if (ok === false) st.ledFail++; else st.ledOk++;
            st.led[i] = ok === false ? -1 : want;          /* a refused packet is retried next tick */
            sent++;
        }
        st.ledScan = (st.ledScan + 1) % 32;
        return sent;
    }

    /* The pad lights are the Move's. Whenever this screen stops being the one in front (it is closed, Back is pressed, or it has
     * stopped being drawn) the colours it painted are taken off, once, so they do not stay on over another view. Leaving
     * them dark lets the Move repaint them. */
    /* One pad now, not at the pace of the rest. */
    function sendLedNow(st, i) {
        var send = globalThis.move_midi_internal_send;
        if (typeof send !== "function") return;
        var want = padColor(st, i), ok = false;
        try { ok = send([0x09, 0x90, PAD_FIRST + i, want]); } catch (e) { ok = false; }
        st.led[i] = ok === false ? -1 : want;
    }

    /* The Move lights a pad you press (green) and the pad an octave away with it: the first and last of a row. This screen cannot stop
     * that (a canvas only observes the pads), so it watches what the Move wrote (the shim's snapshot follows the Move's own writes, not
     * ours) and puts every pad the Move changed back, straight away, for a few ticks. */
    function fightMove(st) {
        var snap = typeof globalThis.shadow_get_pad_led_snapshot === "function" ? globalThis.shadow_get_pad_led_snapshot() : null;
        var sent = 0, i;
        if (snap) {
            for (i = 0; i < 32; i++) {
                var v = snap[PAD_FIRST + i];
                if (typeof v !== "number") continue;
                if (st.moveSeen[i] >= 0 && v !== st.moveSeen[i]) st.fight[i] = FIGHT_TICKS;
                st.moveSeen[i] = v;
            }
        }
        for (i = 0; i < 32 && sent < 2 * LEDS_PER_TICK; i++) {
            if (st.fight[i] > 0) { st.fight[i]--; sendLedNow(st, i); sent++; }
        }
    }

    /* Leaving the screen hands the pads back as the Move had them: outside overtake the shim's snapshot follows the Move's own writes, so
     * it is what to repaint (a pad the Move never lit is dark). Without it, dark. */
    function releaseLeds(st) {
        if (st.released) return;
        st.released = true;
        var send = globalThis.move_midi_internal_send;
        var snap = typeof globalThis.shadow_get_pad_led_snapshot === "function" ? globalThis.shadow_get_pad_led_snapshot() : null;
        for (var i = 0; i < 32; i++) {
            st.led[i] = -1;
            var c = snap && typeof snap[PAD_FIRST + i] === "number" ? snap[PAD_FIRST + i] : 0;
            if (typeof send === "function") { try { send([0x09, 0x90, PAD_FIRST + i, c]); } catch (e) { /* nothing to do */ } }
        }
    }

    /* Leaving without a goodbye. The host runs this screen's tick only while it is the view in front, and changing slot or leaving the module
     * can change the view without calling onClose, so nothing of ours runs any more and the pads keep our colours. What the host does call,
     * every frame, whatever is on the screen, is host_pad_block (it restates that the pads are not blocked). So that call is wrapped, once
     * per script context (as TinySID does), and each time it comes: if this screen has not been drawn for a moment, the pads go back as the
     * Move had them. */
    var LEFT_MS = 400;
    function wallNow() { return typeof globalThis.__tsClock === "function" ? globalThis.__tsClock() : Date.now(); }
    /* Is this screen on the display right now? True only while it is being drawn, so it can never be true for a screen that has gone. */
    function screenUp(st) { return !st.released && st.lastDrawWall > 0 && wallNow() - st.lastDrawWall <= LEFT_MS && onScreen(); }
    function watchPads(st) {
        globalThis.__tsPadHook = function () {
            if (!st.released && st.lastDrawWall && wallNow() - st.lastDrawWall > LEFT_MS) releaseLeds(st);
        };
        /* The pads are ours while the screen is up. The Move's firmware hears every pad the host merely observes, and what a pad does there
         * depends on a view we cannot see behind this one: in the Set overview a tap opens another Set, in Session view it launches a
         * clip. Nothing is lost by withholding them: with this screen up the DSP ignores pad notes and takes every pad from here. */
        globalThis.__tsPadWant = function () { return screenUp(st); };
        if (typeof globalThis.host_pad_block !== "function" || globalThis.__tsPadWatch) return;
        var original = globalThis.host_pad_block;
        globalThis.__tsPadWatch = true;
        globalThis.host_pad_block = function () {
            var args = Array.prototype.slice.call(arguments);
            try { if (typeof globalThis.__tsPadWant === "function" && globalThis.__tsPadWant()) args[0] = 1; } catch (e) { /* the host's own value stands */ }
            var r = original.apply(this, args);
            try { if (typeof globalThis.__tsPadHook === "function") globalThis.__tsPadHook(); } catch (e) { /* never in the host's way */ }
            return r;
        };
    }

    /* The host's own test for "is this screen up": it survives the Move taking the display back (another slot, Menu). */
    function onScreen() {
        return typeof globalThis.shadow_get_display_mode !== "function" || globalThis.shadow_get_display_mode() === 1;
    }

    function newState(now) {
        var st = { view: null, held: {}, led: [], ledScan: 0, refresh: 0, lastLedMs: -1000, acc: [0, 0, 0, 0, 0, 0, 0, 0], now: now || 0,
            lastPad: -1, rawMs: -1000, ledOk: 0, ledFail: 0, ledNone: 0, released: false, lastDraw: 0, zoom: 0, jogAcc: 0, lastCc: -1, hold: null, toast: null, touch: -1, recHold: -1, moveSeen: [], fight: [] };
        for (var i = 0; i < 32; i++) { st.led[i] = -1; st.moveSeen[i] = -1; st.fight[i] = 0; }
        return st;
    }

    function state(ctx) {
        if (!ctx.state.ts) ctx.state.ts = newState(typeof ctx.now === "function" ? ctx.now() : 0);
        return ctx.state.ts;
    }

    function setNow(ctx, key, value) {
        var s = String(value);
        if (typeof ctx.setParam === "function") ctx.setParam(key, s);
        else if (typeof ctx.setParamImmediate === "function") ctx.setParamImmediate(key, s, 25);
    }

    function rel(v) { return v >= 1 && v <= 63 ? v : (v >= 65 && v <= 127 ? v - 128 : 0); }

    /* what a dial says when it is turned: its name and its new value, on the screen for a moment */
    function knobText(k, val) {
        var sign = val >= 0 ? "+" : "";
        switch (k.key) {
        case "clip_depth": return "CLIP DEPTH " + Math.round(val * 100) + "%";
        case "clip_offset": return "CLIP OFFSET " + sign + val.toFixed(2);
        case "gen_style": return "STYLE " + STYLES[clamp(Math.round(val), 0, 12)];
        case "gen_bars": return "LENGTH " + BARS[clamp(Math.round(val), 0, 2)] + (Math.round(val) ? " BARS" : " BAR");
        case "gen_fwd": return "FORWARDS " + Math.round(val * 100) + "%";
        case "swing": return "SWING " + Math.round(val * 100) + "%";
        case "drag": return "DRAG " + Math.round(val * 100) + "%";
        case "gen_groove": return "GROOVE " + Math.round(val * 100) + "%";
        case "humanise": return "HUMANISE " + Math.round(val * 100) + "%";
        case "gen_fin": return "FINISH " + Math.round(val * 100) + "%";
        case "gen_echo": return "ECHO " + Math.round(val * 100) + "%";
        default: return String(k.key).toUpperCase();
        }
    }

    /* what a touched dial is, and what it is set to now: said before it is turned, so the wrong one is not */
    function touchText(v, n) {
        var k = KNOBS[n];
        if (!k) return "DIAL " + (n + 1) + ": NOTHING";
        return knobText(k, num(v ? v[k.v] : 0, 0));
    }

    function turnKnob(ctx, st, n, delta) {
        var k = KNOBS[n], v = st.view;
        if (!k || !v) return;
        var cur = num(v[k.v], 0), nv;
        if (k.steps) {                               /* a few detents to a step, so a stray touch does not jump */
            var per = k.detents || 3;
            st.acc[n] += delta;
            var step = Math.trunc(st.acc[n] / per);
            if (!step) return;
            st.acc[n] -= step * per;
            nv = clamp(Math.round(cur) + step, k.min, k.max);
            setNow(ctx, k.key, String(nv));
        } else {
            nv = clamp(Math.round((cur + delta * k.step) / k.step) * k.step, k.min, k.max);
            setNow(ctx, k.key, nv.toFixed(3));
        }
        v[k.v] = nv;
        st.toast = { text: knobText(k, nv), until: ctx.now() + TOAST_MS };
    }

    /* A clip pad you made, held: after a moment it is deleted. (The Delete button itself is read by the host from the shim and
     * never sent to a screen, so this is the gesture.) A tap still just plays the clip. */
    function checkHold(ctx, st) {
        if (!st.hold) return;
        if (!st.held[st.hold.idx]) { st.hold = null; return; }
        var now = ctx.now();
        if (now - st.hold.since >= HOLD_MS) {
            var h = st.hold;
            if (!h.ok) st.toast = { text: "AHHH AND FRESSSH STAY", until: now + TOAST_MS };
            else if (h.kind === "sample") {
                setNow(ctx, "sample", String(h.n));
                setNow(ctx, "clear", "1");
                st.toast = { text: "USER " + (h.n - 1) + " UNLOADED", until: now + TOAST_MS };
            } else {
                setNow(ctx, "clip_delete", String(h.n));
                st.toast = { text: "CLIP " + h.n + " DELETED", until: now + TOAST_MS };
            }
            st.hold = null;
            for (var i = 0; i < 32; i++) st.led[i] = -1;
        }
    }

    /* Still here: the DSP lets go of the raw pads if this stops. It is said from the event hooks (onValues, which the host runs every
     * time the view changes, and onMidi), NOT from tick: the host hands tick and draw a context without setParam, so a call there
     * does nothing, silently. */
    function keepAlive(ctx, st) {
        if (!ctx || typeof ctx.now !== "function") return;
        var now = ctx.now();
        if (now - st.rawMs >= RAW_PERIOD_MS) { st.rawMs = now; setNow(ctx, "pad_raw", "1"); }
    }

    /* A pad, read raw (so the same pad whatever octave the Move is in): the bottom row is the cuts, held; the second row picks
     * the sample, then Auto; the top two rows are the clips (the last pad Generate): tap one to play it, tap an empty one to keep the
     * clip just generated there, hold one you made to delete it. The DSP is told the held cuts as a
     * mask of the 8 pads, so a message that is lost or doubled is put right by the next. */
    function padEvent(ctx, st, idx, down) {
        var row = Math.floor(idx / 8), col = idx % 8, v = st.view;
        if (row === 0) {
            var mask = 0;
            for (var i = 0; i < 8; i++) if (st.held[i]) mask |= 1 << i;
            setNow(ctx, "pad_cuts", String(mask));
            return;
        }
        if (!down) {
            if (st.recHold === idx) { setNow(ctx, "record", "0"); if (v) v.rc = 0; st.recHold = -1; }     /* let go: the recording stops (and is cropped and kept) */
            return;
        }
        if (row === 1) {
            {
                setNow(ctx, "sample", String(col));
                if (v) v.s = col;
                var ld = v && typeof v.ld === "string" ? v.ld : "";
                if (col < 2 || ld[col] === "1") st.hold = { kind: "sample", n: col, idx: idx, since: ctx.now(), ok: col >= 2 };       /* held: a User sample is unloaded */
                else {                                                           /* an empty User slot, held: it records for as long as it is held */
                    setNow(ctx, "record", "1");
                    if (v) v.rc = 1;
                    st.recHold = idx;
                }
            }
            return;                                                             /* (hold an empty slot to record) */
        }
        var n = (3 - row) * 8 + col + 1;                       /* the top row is clips 1-8, the one below 9-14, then Generate and Auto */
        if (n === CLIP_PADS + 1) { setNow(ctx, "generate", "1"); if (v) v.cm = CLIP_TRY; return; }
        if (n > CLIP_PADS + 1) {                                /* Auto: on, and off again when it is tapped while it is on */
            var next = v && num(v.cm, 0) === CLIP_AUTO ? 0 : CLIP_AUTO;
            setNow(ctx, "clip", String(next));
            if (v) v.cm = next;
            return;
        }
        setNow(ctx, "clip_pad", String(n));
        if (v) { v.cm = n; v.cp = num(v.pg, 0); }
        var cf = v && typeof v.cf === "string" ? v.cf : "";
        if (cf[n - 1] === "1") st.hold = { kind: "clip", n: n, idx: idx, since: ctx.now(), ok: true };     /* held: the clip is deleted */
    }

    var hooks = {
        /* the module's first page: only the picture */
        drawPage: function (ctx, args) {
            var v = parse(args && args.values ? args.values.view : null);
            if (!v) { text(ctx, 2, 20, "TINYSCRATCH: WAITING FOR AUDIO"); return; }
            drawView(ctx, v, true);
        },

        /* full screen: the same picture, and the pads */
        onOpen: function (ctx) {
            var st = state(ctx);
            st.now = ctx.now();
            st.view = parse(ctx.getParam("view"));
            st.held = {};
            for (var i = 0; i < 32; i++) st.led[i] = -1;
            setNow(ctx, "pad_raw", "1");                     /* from here the pads are read raw, whatever octave the Move is in */
            st.rawMs = st.now;
            st.lastDraw = 0;                                 /* (watching for a screen that stops being drawn starts with the first draw) */
            st.lastDrawWall = 0;
            watchPads(st);
            st.released = false;
            st.zoom = 0;
            st.hold = null;
            st.toast = null;
            st.touch = -1;
            setNow(ctx, "zoom", "0");                        /* the normal view to begin with */
        },

        onValues: function (ctx, args) {
            var st = state(ctx);
            var v = parse(args && args.values ? args.values.view : null);
            if (v) st.view = v;
            keepAlive(ctx, st);
            checkHold(ctx, st);
        },

        onMidi: function (ctx, args) {
            var st = state(ctx);
            var d = args && args.data ? args.data : [];
            var status = (d[0] || 0) & 0xF0, num1 = d[1] || 0, val = d[2] || 0;
            st.now = ctx.now();
            keepAlive(ctx, st);
            checkHold(ctx, st);
            var noteOn = status === 0x90 && val > 0, noteOff = status === 0x80 || (status === 0x90 && val === 0);
            if ((noteOn || noteOff) && num1 >= 0 && num1 < 8) {                  /* a dial touched or let go (notes 0-7) */
                if (noteOn) st.touch = num1; else if (st.touch === num1) st.touch = -1;
                return;
            }
            if (noteOn || noteOff) {
                for (var i = 0; i < 32; i++) st.led[i] = -1;           /* the firmware darkens a pad you press: paint the grid again */
                var idx = padIndex(num1);
                if (idx >= 0) {
                    if (noteOn) st.held[idx] = true; else { delete st.held[idx]; if (st.hold && st.hold.idx === idx) st.hold = null; }
                    st.lastPad = idx;
                    padEvent(ctx, st, idx, noteOn);
                    var twin = (idx % 8 === 0 || idx % 8 === 7) ? idx ^ 7 : -1;                  /* the other end of the row: an octave away */
                    st.fight[idx] = FIGHT_TICKS;
                    sendLedNow(st, idx);
                    if (twin >= 0) { st.fight[twin] = FIGHT_TICKS; sendLedNow(st, twin); }
                }
                return;
            }
            if (status === 0xB0 && num1 === BACK_CC && val > 0) { releaseLeds(st); return; }     /* Back leaves this screen */
            if (status === 0xB0 && num1 === JOG_CC) {                            /* the jog wheel: two detents to a step, right zooms in */
                var d = rel(val);
                if (d) {
                    st.jogAcc += d;
                    var stepz = Math.trunc(st.jogAcc / 2);
                    if (stepz) {
                        st.jogAcc -= stepz * 2;
                        var z = clamp(st.zoom + (stepz > 0 ? 1 : -1), 0, 3);
                        if (z !== st.zoom) { st.zoom = z; setNow(ctx, "zoom", String(z)); if (st.view) st.view.zm = z; }
                    }
                }
                return;
            }
            if (status === 0xB0 && val > 0 && (num1 === LEFT_CC || num1 === RIGHT_CC) && st.view) {      /* < and >: turn the page of clip pads */
                var cur = clamp(Math.round(num(st.view.pg, 0)), 0, PAGES - 1), np = clamp(cur + (num1 === RIGHT_CC ? 1 : -1), 0, PAGES - 1);
                if (np !== cur) {
                    setNow(ctx, "page", String(np));
                    st.view.pg = np;
                    st.hold = null;
                    for (var li = 0; li < 32; li++) st.led[li] = -1;
                }
                var pfl = typeof st.view.pf === "string" ? st.view.pf : "";
                st.toast = { text: "CLIPS PAGE " + (np + 1) + " OF " + PAGES + (np > 0 && pfl[np] !== "1" ? " (EMPTY)" : ""), until: ctx.now() + TOAST_MS };
                return;
            }
            if (status === 0xB0 && val > 0 && !(num1 >= KNOB_CC && num1 < KNOB_CC + 8) && num1 !== JOG_CC) st.lastCc = num1;     /* which button it was */
            if (status === 0xB0 && num1 >= KNOB_CC && num1 < KNOB_CC + 8) {
                var delta = rel(val);
                if (delta) turnKnob(ctx, st, num1 - KNOB_CC, delta);
            }
        },

        tick: function (ctx) {
            var st = state(ctx), now = ctx.now();
            st.now = now;
            if (!onScreen() || (st.lastDraw && now - st.lastDraw > STALE_MS)) { st.touch = -1; st.hold = null; releaseLeds(st); return; }       /* not drawn for a while: another view is in front */
            fightMove(st);
            /* LED traffic is paced by the clock, not the frame rate: at most 8 packets per 20 ms */
            if (now - st.lastLedMs >= LED_PERIOD_MS) {
                st.lastLedMs = now;
                st.led[st.refresh] = -1;
                st.refresh = (st.refresh + 1) % 32;
                flushLeds(st);
            }
        },

        draw: function (ctx) {
            var st = state(ctx);
            st.lastDraw = ctx.now();
            st.lastDrawWall = wallNow();
            st.released = false;
            ctx.clear();
            if (!st.view) { text(ctx, 2, 28, "TINYSCRATCH: WAITING FOR AUDIO"); return; }
            var now = ctx.now();
            var extra = { pad: st.lastPad, cc: st.lastCc, now: now, touchText: st.touch >= 0 ? touchText(st.view, st.touch) : "", hold: st.hold && st.hold.ok ? st.hold : null, toast: st.toast && now < st.toast.until ? st.toast.text : "",
                          leds: st.ledNone ? "NO LED" : (st.ledFail ? "LED ERR" : "") };
            if (num(st.view.zm, 0) > 0 && typeof st.view.zp === "string") drawZoom(ctx, st.view, extra);
            else drawView(ctx, st.view, false, extra);
        },

        onClose: function (ctx) {
            if (ctx && ctx.state && ctx.state.ts) releaseLeds(ctx.state.ts);
            setNow(ctx, "zoom", "0");
            setNow(ctx, "pad_cuts", "0");
            setNow(ctx, "pad_raw", "0");                       /* the notes are the notes again */
            ctx.state.ts = null;
        }
    };

    /* Schwung switches a canvas script off for good the first time a hook throws: catch it, keep running, and put the
     * message on the screen so it can be read. */
    function guarded(name, fn) {
        return function (ctx, args) {
            try { return fn(ctx, args); } catch (e) {
                var msg = name + ": " + String(e && e.message ? e.message : e);
                if (name === "drawPage" || name === "draw") {
                    try { ctx.clear(); text(ctx, 0, 0, "ERR"); text(ctx, 0, 8, fit(msg, 31)); text(ctx, 0, 14, msg.slice(31, 62)); } catch (e2) { /* nothing left to try */ }
                }
                return undefined;
            }
        };
    }

    globalThis.canvas_overlay = {
        /* Pad presses reach a canvas only if it asks (shadow_ui.js raises host_pad_observe for overlay.wantsPads). They are
         * observed, never blocked: the track still gets the note, so the cuts play while this screen colours the pads. */
        wantsPads: true
    };
    for (var hook in hooks) if (Object.prototype.hasOwnProperty.call(hooks, hook)) globalThis.canvas_overlay[hook] = guarded(hook, hooks[hook]);

    /* for tests: the pure helpers, without a host */
    globalThis.__tinyscratch = { parse: parse, drawView: drawView, padColor: padColor, padIndex: padIndex, newState: newState, flushLeds: flushLeds,
        COL: COL, KNOBS: KNOBS };
})();
