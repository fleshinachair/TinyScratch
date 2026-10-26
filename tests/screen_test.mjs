// Headless test of src/scratcher.js: a mock canvas and LED sink, fed with real views the C test suite wrote to
// build/test/screen_fixtures.json.   node tests/screen_test.mjs
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const src = fs.readFileSync(path.join(root, "src", "scratcher.js"), "utf8");
let checks = 0, failed = 0;
function check(ok, msg) {
    checks++;
    if (!ok) { failed++; console.error("  FAIL " + msg); }
}

function load(send) {
    const box = {};
    if (send) box.move_midi_internal_send = send;
    vm.createContext(box);
    vm.runInContext(src, box, { filename: "scratcher.js" });
    return box;
}

function makeCtx({ values = {}, state = null, t = 1000 } = {}) {
    const ctx = {
        width: 128, height: 64, state: state || {}, t, px: new Map(), oob: 0, cleared: 0, sets: [],
        now() { return ctx.t; },
        clear() { ctx.px.clear(); ctx.cleared++; },
        setPixel(x, y, v) {
            if (!Number.isInteger(x) || !Number.isInteger(y)) { ctx.oob++; return; }
            if (x < 0 || x >= 128 || y < 0 || y >= 64) { ctx.oob++; return; }
            ctx.px.set(x + "," + y, v ? 1 : 0);
        },
        fillRect(x, y, w, h, v) {      // the host clips rectangles
            for (let i = 0; i < w; i++) for (let j = 0; j < h; j++) {
                const xx = Math.round(x) + i, yy = Math.round(y) + j;
                if (xx >= 0 && xx < 128 && yy >= 0 && yy < 64) ctx.px.set(xx + "," + yy, v ? 1 : 0);
            }
        },
        getParam(k) { return k in values ? values[k] : ""; },
        setParam(k, v) { ctx.sets.push([k, String(v)]); },
        on(x, y) { return ctx.px.get(x + "," + y) === 1; },
        count(x0, y0, x1, y1) { let n = 0; for (let x = x0; x <= x1; x++) for (let y = y0; y <= y1; y++) n += ctx.on(x, y) ? 1 : 0; return n; },
    };
    return ctx;
}

// the full-screen layout (128 x 64) for a view object; a string goes through the page's own drawPage, as the host would
// What the host hands the tick and draw hooks: a context WITHOUT the param accessors (shadow_ui.js, DRAW_PATH_HOOKS), so a setParam
// there does nothing at all. The tests call those two hooks through this, so a screen that depends on it fails here, not on the Move.
function hostCtx(ctx) {
    const h = Object.create(ctx);
    h.setParam = undefined; h.getParam = undefined; h.setValue = undefined; h.getValue = undefined; h.close = undefined;
    return h;
}

function draw(box, view) {
    const ctx = makeCtx();
    if (typeof view === "string") box.canvas_overlay.drawPage(ctx, { values: { view } });
    else box.__tinyscratch.drawView(ctx, view, false);
    return ctx;
}
function drawPage(box, view) {
    const ctx = makeCtx();
    box.canvas_overlay.drawPage(ctx, { values: { view: typeof view === "string" ? view : JSON.stringify(view) } });
    return ctx;
}
const has = (ctx, k, v) => ctx.sets.some(([a, b]) => a === k && (v === undefined || b === String(v)));

const fxPath = path.join(root, "build", "test", "screen_fixtures.json");
let fixtures = [];
try { fixtures = JSON.parse(fs.readFileSync(fxPath, "utf8")); } catch (e) { /* reported below */ }
check(fixtures.length >= 4, "the C tests wrote the view fixtures (" + fixtures.length + ")");

const box = load();
const T = box.__tinyscratch;
check(box.canvas_overlay && typeof box.canvas_overlay.drawPage === "function" && box.canvas_overlay.wantsPads === true,
    "the script registers drawPage and asks for the pads (wantsPads), or the host never sends them");
for (const h of ["onOpen", "onValues", "onMidi", "tick", "draw", "onClose"]) check(typeof box.canvas_overlay[h] === "function", "full-screen hook " + h);

// 1. every real view draws something sensible and never leaves the screen
fixtures.forEach((v, i) => {
    const ctx = draw(box, v);
    const lit = [...ctx.px.values()].filter((x) => x).length;
    check(ctx.oob === 0, `fixture ${i}: every pixel is on the screen (${ctx.oob} were not)`);
    check(lit > 350 && lit < 5500, `fixture ${i}: a drawn screen, not an empty or a solid one (${lit} pixels)`);
    check(ctx.cleared === 0, `fixture ${i}: nothing went wrong (no error screen)`);
    check(typeof v.w === "string" && v.w.length === 256 && typeof v.wr === "string" && v.wr.length === 256 && v.lnp.length === 128 && v.lng.length === 128,
        `fixture ${i}: the engine's view has the waveform (peak and body) and the clip view`);
    // the title: the name of the sample in use, in big letters, to the left of the vinyl
    check(ctx.count(0, 0, 105, 9) > 25, `fixture ${i}: the sample in use is the title (${ctx.count(0, 0, 105, 9)} lit)`);
    // the playhead is a line through the whole of the waveform band
    const x = Math.round(Number(v.x) * 105 / 127);
    let full = 0;
    for (let y = 11; y < 31; y++) full += ctx.on(x, y) ? 1 : 0;
    check(full === 20, `fixture ${i}: the playhead is a solid line at column ${x} (${full} of 20)`);
    check(ctx.count(0, 11, 105, 30) > 150, `fixture ${i}: there is a waveform`);
    // the vinyl: the white line leaves the centre towards the angle the position says
    const a = Number(v.p) * Math.PI * 2 - Math.PI / 2;
    const tipX = Math.round(117 + 7 * Math.cos(a)), tipY = Math.round(10 + 7 * Math.sin(a));
    let near = 0;
    for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) near += ctx.on(tipX + dx, tipY + dy) ? 1 : 0;
    check(near >= 1, `fixture ${i}: the vinyl's white line points to ${(Number(v.p) * 360).toFixed(0)} degrees`);
    // the clip view is framed and carries the cursor
    check(ctx.on(0, 32) && ctx.on(127, 32) && ctx.on(0, 57) && ctx.on(127, 57), `fixture ${i}: the clip view has its frame`);
    const cur = Number(v.lcol);
    let dots = 0;
    for (let y = 33; y < 57; y++) dots += ctx.on(cur, y) ? 1 : 0;
    check(dots >= 8, `fixture ${i}: the cursor is at column ${cur} of the bar (${dots} dots)`);
});

// 1b. the title is the name in use (big), not a list; a dot for each slot sits under the vinyl
{
    const base = Object.assign({}, fixtures[0], { n: "AHHH", s: 0, ld: "110000" });
    const a = draw(box, base), b = draw(box, Object.assign({}, base, { n: "FRESSSH", s: 1 })), c = draw(box, Object.assign({}, base, { n: "APOLLO11_EAG" }));
    let d1 = 0;
    for (let x = 0; x < 106; x++) for (let y = 0; y < 10; y++) d1 += a.on(x, y) !== b.on(x, y) ? 1 : 0;
    check(d1 > 30, `the title changes with the sample (${d1} pixels differ)`);
    check(a.count(0, 0, 105, 9) > 40 && a.count(0, 0, 105, 9) < 400, "...and is in big letters: AHHH is a solid block of text, not a speck");
    let over = 0;
    for (let x = 100; x < 107; x++) for (let y = 0; y < 10; y++) over += c.on(x, y) ? 1 : 0;
    check(c.count(0, 0, 105, 9) > 100 && over === 0, "a long name fits to the left of the vinyl");
    let rows = 0;
    for (let y = 0; y < 10; y++) rows += a.count(0, y, 40, y) > 0 ? 1 : 0;
    check(rows >= 9, `the letters are ten rows tall (${rows})`);
    // the dots: the slot in use is a block, the others that have a sample a single dot, empty ones nothing
    check(a.count(108, 23, 110, 25) === 9 && a.on(112, 24) && !a.on(118, 24) && !a.on(121, 24), "slot 1 in use is a block, slot 2 (which has a sample) a dot, empty slots nothing");
    check(b.count(111, 23, 113, 25) === 9 && b.on(109, 24) && b.count(108, 23, 108, 25) === 0, "moving to slot 2 moves the block");
    const none = draw(box, Object.assign({}, base, { n: "", ld: "000000" }));
    check(none.count(0, 0, 105, 9) > 25, "no sample: it says so");
    const empty = draw(box, Object.assign({}, base, { n: "EMPTY", s: 3, ld: "100000", w: "00".repeat(128), wr: "00".repeat(128) }));
    check(empty.count(0, 0, 105, 9) > 40 && empty.count(0, 11, 105, 30) < 0.4 * draw(box, base).count(0, 11, 105, 30), "an empty slot is titled EMPTY and its waveform is just the flat line");
    // recording: REC and the seconds, and a level bar under the title
    const rec = draw(box, Object.assign({}, base, { rc: 1, rt: 12, rl: 60, w: "00".repeat(128), wr: "00".repeat(128) }));
    const idle = draw(box, Object.assign({}, base, { rc: 0, rt: 0, rl: 0 }));
    check(rec.count(0, 10, 105, 10) >= 60 && rec.count(0, 10, 105, 10) <= 66 && idle.count(0, 10, 105, 10) === 0, `the level of the input is a bar under the title while recording (${rec.count(0, 10, 105, 10)} px)`);
    check(rec.count(0, 0, 105, 9) > 30 && rec.count(0, 0, 60, 9) !== idle.count(0, 0, 60, 9), "...and the title says REC and the seconds");
    const full = draw(box, Object.assign({}, base, { rc: 1, rt: 99, rl: 100 }));
    check(full.count(0, 10, 105, 10) <= 106 && full.oob === 0, "a full level does not run off the title");
}

// 2. the waveform keeps its detail: the body solid, the peaks round it, quiet parts a thin line
{
    const peaks = "00".repeat(30) + "ff".repeat(30) + "80".repeat(30) + "10".repeat(38);
    const body = "00".repeat(30) + "a0".repeat(30) + "40".repeat(30) + "08".repeat(38);
    const v = Object.assign({}, fixtures[0], { w: peaks, wr: body, x: 127 });
    const ctx = draw(box, v);
    const col = (c) => ctx.count(c, 11, c, 30);
    check(col(10) <= 2, `silence is a thin line (${col(10)} pixels)`);
    check(col(40) > 11, `a loud part is tall (${col(40)} pixels)`);
    check(col(40) > col(70) && col(70) > col(100), `and the heights follow the sound (${col(40)}, ${col(70)}, ${col(100)})`);
    const loudCol = 37;                                          // peak 1.0, body 0.63: the peaks stand out beyond the body
    check(ctx.on(loudCol, 12) || ctx.on(loudCol, 13), "the peak is marked above the body");
    check(ctx.on(loudCol, 21) && ctx.on(loudCol, 17), "the body is solid through the middle");
}

// 3. the vinyl line at the four hands of the clock
{
    const base = Object.assign({}, fixtures[0]);
    const spots = [[0, 117, 3], [0.25, 125, 10], [0.5, 117, 17], [0.75, 109, 10]];
    for (const [p, x, y] of spots) {
        const ctx = draw(box, Object.assign({}, base, { p }));
        let near = 0;
        for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) near += ctx.on(x + dx, y + dy) ? 1 : 0;
        check(near >= 1, `vinyl at position ${p}: the line passes (${x}, ${y})`);
    }
    const a = draw(box, Object.assign({}, base, { p: 0 })), b = draw(box, Object.assign({}, base, { p: 0.5 }));
    check(a.on(117, 3) && !a.on(117, 17) && b.on(117, 17) && !b.on(117, 3), "it starts at 12 o'clock and is at 6 o'clock half way");
}

// 4. the clip view: the cuts as blocks, the path as a line, the beats ticked
{
    const lnp = "0123456789abcdef".repeat(8), lng = ("1111100000".repeat(13)).slice(0, 128);
    const v = Object.assign({}, fixtures[0], { lnp, lng, lcol: 64 });
    const ctx = draw(box, v);
    let blocks = 0, path = 0;
    for (let x = 1; x < 127; x++) { blocks += ctx.on(x, 35) ? 1 : 0; path += ctx.count(x, 41, x, 54) > 0 ? 1 : 0; }
    check(blocks > 50 && blocks < 75, `cuts show as blocks along the top of the lane (${blocks} of 126 columns)`);
    check(path >= 120, `the playhead's path runs across the whole bar (${path} of 126 columns)`);
    const quiet = draw(box, Object.assign({}, v, { lng: "0".repeat(128) }));
    let none = 0;
    for (let x = 1; x < 127; x++) none += quiet.on(x, 35) ? 1 : 0;
    check(none < 8, "no cuts, no blocks");
    const tk = draw(box, Object.assign({}, v, { lcol: -1, lnp: "f".repeat(128), lng: "0".repeat(128) }));
    check([32, 64, 96].every((x) => tk.on(x, 56) && tk.on(x, 55)), "the beats of the bar are ticked");
    check(!tk.on(16, 56) && !tk.on(48, 56), "...and only the beats");
    // a longer clip: a tick on each of its beats, and a taller one on each bar
    const l16 = draw(box, Object.assign({}, v, { lcol: -1, lb: 16, lnp: "f".repeat(128), lng: "0".repeat(128) }));
    check([8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120].every((x) => l16.on(x, 56) && l16.on(x, 55)), "a 16-beat clip: all its beats are ticked");
    check([32, 64, 96].every((x) => l16.on(x, 54)) && !l16.on(8, 54) && !l16.on(40, 54), "...and its bars (every fourth) taller");
    const l8 = draw(box, Object.assign({}, v, { lcol: -1, lb: 8, lnp: "f".repeat(128), lng: "0".repeat(128) }));
    check([16, 32, 48, 64, 80, 96, 112].every((x) => l8.on(x, 56)) && l8.on(64, 54) && !l8.on(16, 54), "an 8-beat clip: seven beat ticks, the bar in the middle taller");
    // the path really is the data: a high value is drawn higher than a low one
    const hi = draw(box, Object.assign({}, v, { lnp: "f".repeat(128), lcol: -1 })), lo = draw(box, Object.assign({}, v, { lnp: "0".repeat(128), lcol: -1 }));
    check(hi.count(20, 38, 40, 45) > 10 && hi.count(20, 52, 40, 54) < 3 && lo.count(20, 52, 40, 55) > 10 && lo.count(20, 38, 40, 45) < 3, "high is up, low is down");
    // the cursor follows the clock
    const c1 = draw(box, Object.assign({}, v, { lcol: 10 })), c2 = draw(box, Object.assign({}, v, { lcol: 100, lng: "0".repeat(128), lnp: "0".repeat(128) }));
    check(c1.count(10, 33, 10, 55) >= 8 && c2.count(100, 33, 100, 55) >= 8 && c2.count(10, 41, 10, 50) < 3, "the cursor moves with the column");
}

// 5. the clip's target is ticked under the waveform while one plays; the status line changes
{
    const v = Object.assign({}, fixtures[1], { cm: 3, cl: 3, tx: 100, x: 20 });
    const on = draw(box, v);
    check(on.on(Math.round(100 * 105 / 127), 31), "the clip's target is ticked under the waveform");
    check(!draw(box, Object.assign({}, v, { cm: 0, cl: 0 })).on(Math.round(100 * 105 / 127), 31), "...only while a clip plays");
    const a = draw(box, Object.assign({}, v, { cm: 3, cl: 3, ru: 1 })), b = draw(box, Object.assign({}, v, { cm: 0, cl: 0 }));
    let diff = 0;
    for (let x = 0; x < 128; x++) for (let y = 59; y < 64; y++) diff += a.on(x, y) !== b.on(x, y) ? 1 : 0;
    check(diff > 10, "the status line says what is set (it changes with the clip)");
    // (the first page's panel says PRESS PLAY when a clip waits for the transport: tested with the page, below)
}

// 6. full screen: the pads
{
    const sent = [];
    let accept = true;
    const b2 = load((pkt) => { sent.push(pkt.slice()); return accept; });
    const T2 = b2.__tinyscratch, C = T2.COL;
    const view = Object.assign({}, fixtures[0], { s: 1, ld: "110000", cm: 3, cl: 3, g: 0, nh: 0, tt: 0.5, dr: 0.5, gn: 0, ng: 1, vv: 0, no: 1 });
    const ctx = makeCtx({ values: { view: JSON.stringify(view) } });
    b2.canvas_overlay.onOpen(ctx);
    const per = [];
    for (let t = 0; t < 6; t++) { const n = sent.length; ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); per.push(sent.length - n); }
    check(per.every((n) => n <= 8), "never more than 8 LED packets per tick: " + per);
    const last = new Map();
    for (const p of sent) last.set(p[2], p[3]);
    check(last.size === 32, "all 32 pads painted after a few ticks (" + last.size + ")");
    check(sent.every((p) => p.length === 4 && p[0] === 0x09 && p[1] === 0x90 && p[2] >= 68 && p[2] <= 99 && p[3] >= 0 && p[3] < 128), "LED packets are [0x09, 0x90, 68..99, palette]");
    for (let i = 0; i < 8; i++) check(last.get(68 + i) === C.cut, `cut pad ${i + 1} is ${last.get(68 + i)} (want ${C.cut}): the bottom row`);
    const second = [C.sample, C.white, C.sampleEmpty, C.sampleEmpty, C.sampleEmpty, C.sampleEmpty];
    for (let c = 0; c < 6; c++) check(last.get(76 + c) === second[c], `sample pad ${c + 1}: ${last.get(76 + c)} (want ${second[c]}: ${c === 1 ? "in use" : (c === 0 ? "loaded" : "empty")})`);
    check(last.get(82) === C.off && last.get(83) === C.rec && last.get(90) === C.gen && last.get(91) === C.clipAuto, `the unused pad ${last.get(82)}, the Record pad ${last.get(83)}, Generate ${last.get(90)}, Auto (the last clip pad) ${last.get(91)}`);
    const noteOf = (n) => (n <= 8 ? 91 + n : 75 + n);                      // clips 1-8 are the top row, 9-16 the row below it
    for (let n = 1; n <= 16; n++) {
        const want = n === 16 ? C.clipAuto : (n === 15 ? C.gen : (n > 8 ? C.off : (n === 3 ? C.white : C.clip)));
        check(last.get(noteOf(n)) === want, `clip pad ${n}: ${last.get(noteOf(n))} (want ${want}: eight clips, clip 3 picked; empty pads are dark)`);
    }
    const n0 = sent.length;
    ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx));
    check(sent.length - n0 <= 1, "a steady screen sends at most the one-pad-per-tick refresh (" + (sent.length - n0) + ")");

    // the view changes: the pads follow
    b2.canvas_overlay.onValues(ctx, { values: { view: JSON.stringify(Object.assign({}, view, { s: 2, cm: 6, cl: 6 })) } });
    for (let t = 0; t < 40; t++) { ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); }
    const l2 = new Map();
    for (const p of sent.slice(-400)) l2.set(p[2], p[3]);
    check(l2.get(78) === C.white && l2.get(77) === C.sample && l2.get(82) === C.off && l2.get(83) === C.rec && l2.get(90) === C.gen && l2.get(91) === C.clipAuto && l2.get(noteOf(6)) === C.white && l2.get(noteOf(3)) === C.clip,
        "sample 3 picked, clip 6: the pads follow the view");
    const settle = (v) => {
        b2.canvas_overlay.onValues(ctx, { values: { view: JSON.stringify(Object.assign({}, view, v)) } });
        for (let t = 0; t < 40; t++) { ctx.t += 20; if (t % 10 === 0) b2.canvas_overlay.draw(hostCtx(ctx)); b2.canvas_overlay.tick(hostCtx(ctx)); }
        const m = new Map();
        for (const p of sent.slice(-400)) m.set(p[2], p[3]);
        return m;
    };
    // auto: its pad is lit, and the clip playing now shows
    const l3 = settle({ cm: 17, cl: 4 });
    check(l3.get(91) === C.white && l3.get(noteOf(4)) === C.clipNow && l3.get(noteOf(1)) === C.clip, "Auto: its pad is lit and the clip playing now is marked");
    // generate: the try clip is playing; the empty clip pads stay dark (tap one to keep the clip there)
    const l4 = settle({ cm: 18, cl: 18, tv: 1, tg: 3 });
    check(l4.get(90) === C.white && l4.get(91) === C.clipAuto && [...Array(8).keys()].every((c) => l4.get(92 + c) === C.clip) && [...Array(6).keys()].every((c) => l4.get(84 + c) === C.off),
        "Generate: its pad is lit, the clips are lit and the empty pads are dark");
    // fewer clips than pads (the example set's six): the pads for the others are dark
    const l5 = settle({ cm: 2, cl: 2, cf: "1111110000000000", nc: 6 });
    check(l5.get(noteOf(1)) === C.clip && l5.get(noteOf(2)) === C.white && l5.get(noteOf(6)) === C.clip && l5.get(noteOf(7)) === C.off && l5.get(noteOf(8)) === C.off, "six clips: the other pads are dark");
    // a clip you made, held: red until it is deleted (the built-in clips are never red)
    settle({ cm: 2, cl: 2, cf: "1111111110000000", kp: "0000000010000000" });
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, noteOf(9), 100] });
    const l6 = settle({ cm: 2, cl: 2, cf: "1111111110000000", kp: "0000000010000000" });
    check(l6.get(noteOf(9)) === C.cut && l6.get(noteOf(2)) === C.white && l6.get(noteOf(1)) === C.clip, "a clip you made, held, is red; the others keep their colours");
    b2.canvas_overlay.onMidi(ctx, { data: [0x80, noteOf(9), 0] });
    const l7 = settle({ cm: 2, cl: 2, cf: "1111111110000000", kp: "0000000010000000" });
    check(l7.get(noteOf(9)) === C.clip && l7.get(noteOf(2)) === C.white, "...and back to normal when it is let go");

    // a pressed cut pad lights while it is held, and the grid is repainted after any note (the firmware darkens a pad you press)
    const nb = sent.length;
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 68 + 3, 100] });
    for (let t = 0; t < 8; t++) { ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); }
    const press = new Map();
    for (const p of sent.slice(nb)) press.set(p[2], p[3]);
    check(press.get(71) === C.cutHot && press.size === 32, `a held cut pad is lit ${press.get(71)} and the whole grid is repainted (${press.size} pads)`);
    b2.canvas_overlay.onMidi(ctx, { data: [0x80, 68 + 3, 0] });
    for (let t = 0; t < 8; t++) { ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); }
    const rel = new Map();
    for (const p of sent.slice(sent.length - 200)) rel.set(p[2], p[3]);
    check(rel.get(71) === C.cut, "...and goes back when it is let go");
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 36 + 5, 100] });
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 36 + 5, 0] });
    check(ctx.state.ts.held[5] === undefined, "the drum layout (36-67) is the same grid, and a note on with velocity 0 is a note off");
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 36 + 5, 100] });
    check(ctx.state.ts.held[5] === true, "(and held again)");
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 10, 100] });
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 101, 100] });
    check(Object.keys(ctx.state.ts.held).length === 1, "notes that are not pads do not light one");

    // a refused packet is retried
    accept = false;
    const nr = sent.length;
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 68, 100] });
    for (let t = 0; t < 40; t++) { ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); }
    check(sent.slice(nr).filter((p) => p[2] === 68).length >= 2, "a refused LED write is retried, not forgotten");
    accept = true;
    for (let t = 0; t < 40; t++) { ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); }
    const n1 = sent.length;
    ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx));
    check(sent.length - n1 <= 1, "...and once it lands only the slow refresh remains");

    // the LED rate holds even when the host calls tick far faster than the frame rate
    const fast = [];
    const b3 = load((pkt) => { fast.push(pkt.slice()); return true; });
    const c3 = makeCtx({ values: { view: JSON.stringify(view) } });
    b3.canvas_overlay.onOpen(c3);
    for (let t = 0; t < 400; t++) { c3.t += 1; b3.canvas_overlay.tick(hostCtx(c3)); }
    check(fast.length <= 8 * 20 + 4 && fast.length >= 32, `400 ticks in 0.4 s sent ${fast.length} packets (paced at 8 per 20 ms)`);

    // and with no LED path at all, nothing throws
    const b4 = load(null);
    const c4 = makeCtx({ values: { view: JSON.stringify(view) } });
    let threw = null;
    try { b4.canvas_overlay.onOpen(c4); b4.canvas_overlay.tick(hostCtx(c4)); b4.canvas_overlay.draw(hostCtx(c4)); b4.canvas_overlay.onClose(c4); } catch (e) { threw = e; }
    check(!threw, "no move_midi_internal_send: the screen still runs");
}

// 6b. full screen: the pads are read raw, so the octave of the Move does not matter
{
    const b2 = load(() => true);
    const view = Object.assign({}, fixtures[0], { s: 1, cm: 3, cl: 3, g: 0, nh: 0 });
    const ctx = makeCtx({ values: { view: JSON.stringify(view) } });
    b2.canvas_overlay.onOpen(ctx);
    check(has(ctx, "pad_raw", "1"), "opening the Pads screen tells the DSP the pads are read raw");
    const n0 = ctx.sets.filter(([k]) => k === "pad_raw").length;
    const values = () => b2.canvas_overlay.onValues(ctx, { values: { view: JSON.stringify(view) } });
    for (let t = 0; t < 10; t++) { ctx.t += 20; values(); }
    check(ctx.sets.filter(([k]) => k === "pad_raw").length === n0, "...and not every time the view changes");
    for (let t = 0; t < 30; t++) { ctx.t += 20; values(); }
    check(ctx.sets.filter(([k]) => k === "pad_raw").length >= n0 + 1, "...but again every few tenths of a second, from the view's events, so the DSP knows it is still there");
    // the host gives tick and draw no setParam: they must not be what keeps the DSP told (this is the bug that put the pads back on notes)
    const m0 = ctx.sets.length;
    for (let t = 0; t < 200; t++) { ctx.t += 20; b2.canvas_overlay.tick(hostCtx(ctx)); b2.canvas_overlay.draw(hostCtx(ctx)); }
    check(ctx.sets.length === m0, "tick and draw set nothing (they cannot: the host takes setParam away from them)");
    ctx.t += 1000;
    let held = false;
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 68 + 3, 100] });
    held = has(ctx, "pad_raw");
    b2.canvas_overlay.onMidi(ctx, { data: [0x80, 68 + 3, 0] });
    check(held && ctx.sets.length > m0, "a pad press also says it, so a quiet view cannot let the raw pads lapse while you play");
    const cuts = () => ctx.sets.filter(([k]) => k === "pad_cuts").slice(-1)[0];
    const pad = (n, down) => b2.canvas_overlay.onMidi(ctx, { data: [down ? 0x90 : 0x80, n, down ? 100 : 0] });
    pad(68, true);
    check(cuts() && cuts()[1] === "1", "the bottom-left pad: cut 1 held (mask 1)");
    pad(69, true);
    check(cuts()[1] === "3", "a second pad: both held (mask 3)");
    pad(68, false);
    check(cuts()[1] === "2", "the first let go (mask 2)");
    pad(69, false);
    check(cuts()[1] === "0", "all let go (mask 0)");
    pad(68 + 7, true);
    check(cuts()[1] === String(1 << 7), "the 8th cut pad is the right end of the bottom row");
    pad(68 + 7, false);
    ctx.sets.length = 0;
    pad(68 + 8 + 2, true);
    check(has(ctx, "sample", "2") && !has(ctx, "pad_cuts"), "second row, third pad: sample 3 (User 1), and it is not a cut");
    ctx.sets.length = 0;
    pad(68 + 8 + 6, true);
    check(ctx.sets.length === 0, "second row, seventh: not used");
    pad(68 + 8 + 7, true);
    check(has(ctx, "record") && !has(ctx, "generate") && !has(ctx, "clip_pad") && !has(ctx, "sample", "7"), "second row, eighth: Record");
    pad(68 + 16 + 6, true);
    check(has(ctx, "generate", "1") && !has(ctx, "clip_pad"), "the pad before the last of the clip rows (second row from the top) is Generate");
    pad(68 + 16 + 7, true);
    check(has(ctx, "clip", "17"), "the last of the clip pads (second row from the top, right end) is Auto");
    pad(68 + 24 + 1, true);
    check(has(ctx, "clip_pad", "2"), "top row, second pad: clip 2");
    pad(68 + 16 + 5, true);
    check(has(ctx, "clip_pad", "14"), "the row below the top, before Generate: clip 14");
    ctx.sets.length = 0;
    pad(68 + 24 + 3, false);
    check(ctx.sets.length === 0, "letting go of a pad does nothing");
    pad(36 + 24 + 4, true);
    check(has(ctx, "clip_pad", "5"), "the drum layout's numbers are the same pads");
    // a tap on a clip pad only plays it
    ctx.sets.length = 0;
    pad(68 + 24 + 2, true);
    pad(68 + 24 + 2, false);
    check(has(ctx, "clip_pad", "3") && !has(ctx, "clip_delete"), "a tap on a clip pad plays it and does not delete");
    ctx.sets.length = 0;
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 20, 100] });
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 105, 100] });
    check(!has(ctx, "clip") && !has(ctx, "pad_cuts") && !has(ctx, "sample"), "notes that are not pads do nothing");
    // the screen shows the pick at once
    pad(68 + 24 + 5, true);
    b2.canvas_overlay.draw(hostCtx(ctx));
    ctx.sets.length = 0;
    b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 3, 127] });
    b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 118, 127] });
    check(ctx.sets.length === 0, "other buttons do nothing");
    // closing hands the notes back
    ctx.sets.length = 0;
    b2.canvas_overlay.onClose(ctx);
    check(has(ctx, "pad_raw", "0") && has(ctx, "pad_cuts", "0"), "closing the screen lets the DSP read notes again and lets go of any cut");
    // the status line names the last raw pad, and says whether the LED writes land
    const b3 = load(() => true);
    const c3 = makeCtx({ values: { view: JSON.stringify(view) } });
    b3.canvas_overlay.onOpen(c3);
    b3.canvas_overlay.onMidi(c3, { data: [0x90, 68 + 12, 100] });
    for (let t = 0; t < 10; t++) { c3.t += 20; b3.canvas_overlay.tick(hostCtx(c3)); }
    b3.canvas_overlay.draw(hostCtx(c3));
    check(c3.count(0, 59, 127, 63) > 20, "the full screen has its status line, with the pad and the LED state");
    // RAW or NOTE: whether the DSP is reading the pads raw
    const rawOn = load(() => true), rawOff = load(() => true);
    const cr = makeCtx({ values: { view: JSON.stringify(Object.assign({}, view, { rw: 1 })) } }), cn = makeCtx({ values: { view: JSON.stringify(Object.assign({}, view, { rw: 0 })) } });
    rawOn.canvas_overlay.onOpen(cr); rawOff.canvas_overlay.onOpen(cn);
    rawOn.canvas_overlay.draw(hostCtx(cr)); rawOff.canvas_overlay.draw(hostCtx(cn));
    let rdiff = 0;
    for (let x = 80; x < 128; x++) for (let y = 59; y < 64; y++) rdiff += cr.on(x, y) !== cn.on(x, y) ? 1 : 0;
    check(rdiff > 6, "the status line says RAW or NOTE: whether the DSP reads the pads raw (" + rdiff + " pixels differ)");
    const noLed = load(null), c4 = makeCtx({ values: { view: JSON.stringify(view) } });
    noLed.canvas_overlay.onOpen(c4);
    for (let t = 0; t < 4; t++) { c4.t += 20; noLed.canvas_overlay.tick(hostCtx(c4)); }
    noLed.canvas_overlay.draw(hostCtx(c4));
    const withLed = load(() => true), c5 = makeCtx({ values: { view: JSON.stringify(view) } });
    withLed.canvas_overlay.onOpen(c5);
    for (let t = 0; t < 4; t++) { c5.t += 20; withLed.canvas_overlay.tick(hostCtx(c5)); }
    withLed.canvas_overlay.draw(hostCtx(c5));
    let diff = 0;
    for (let x = 90; x < 128; x++) for (let y = 59; y < 64; y++) diff += c4.on(x, y) !== c5.on(x, y) ? 1 : 0;
    check(diff > 5, "the status line tells apart 'no LED path' from 'LEDs working'");
}

// 6d. the zoomed view: the jog wheel, and the drawing
{
    const b2 = load(() => true);
    const ramp = Array.from({ length: 128 }, (_, i) => Math.round(i * 2).toString(16).padStart(2, "0")).join("");
    const gates = Array.from({ length: 128 }, (_, i) => (i >= 40 && i < 80 ? "1" : "0")).join("");
    const zview = Object.assign({}, fixtures[0], { zm: 2, zs: 3.2, zw: 2, zp: ramp, zg: gates, cm: 17, cl: 3, ru: 1 });
    const ctx = makeCtx({ values: { view: JSON.stringify(zview) } });
    b2.canvas_overlay.onOpen(ctx);
    check(has(ctx, "zoom", "0"), "the Pads screen opens in the normal view");
    const jog = (v) => b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 14, v] });
    ctx.sets.length = 0;
    jog(1);
    check(ctx.sets.length === 0, "one detent of the jog wheel does nothing yet");
    jog(1);
    check(has(ctx, "zoom", "1"), "two detents to the right zoom in");
    jog(1); jog(1); jog(1); jog(1); jog(1); jog(1);
    check(has(ctx, "zoom", "3") && !has(ctx, "zoom", "4"), "...up to the closest view, and no further");
    jog(127); jog(127);
    check(ctx.sets.slice(-1)[0][1] === "2", "two to the left zoom out a step");
    jog(127); jog(127); jog(127); jog(127); jog(127); jog(127);
    check(ctx.sets.slice(-1)[0][1] === "0", "...to the normal view, and no further");
    ctx.sets.length = 0;
    jog(0);
    b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 15, 1] });
    check(ctx.sets.length === 0, "other controls do not zoom");

    // drawing it
    const b3 = load(() => true);
    const c3 = makeCtx({ values: { view: JSON.stringify(zview) } });
    b3.canvas_overlay.onOpen(c3);
    b3.canvas_overlay.onValues(c3, { values: { view: JSON.stringify(zview) } });
    b3.canvas_overlay.draw(hostCtx(c3));
    check(c3.oob === 0 && c3.cleared >= 1, "the zoomed view draws");
    let curve = 0;
    for (let y = 9; y < 44; y++) curve += c3.on(5, y) ? 1 : 0;
    let curve2 = 0;
    for (let y = 9; y < 44; y++) curve2 += c3.on(120, y) ? 1 : 0;
    let low = -1, high = -1;
    for (let y = 9; y < 44; y++) { if (c3.on(5, y)) low = y; if (c3.on(120, y)) high = y; }
    check(low > 36 && high < 20, "the playhead's path is drawn from the data (a ramp: low on the left (row " + low + "), high on the right (row " + high + "))");
    check(c3.count(50, 47, 50, 54) === 8 && c3.count(10, 47, 10, 53) === 0 && c3.count(100, 47, 100, 53) === 0, "the notes are blocks where the sound is cut in");
    check(c3.count(32, 8, 32, 46) > 30 && c3.on(32, 54), "the 'now' cursor is a line a quarter of the way in");
    check(c3.count(51, 8, 51, 43) === 36, "a bar line (beat 4 here, at 3.2 + 0.8 beats in) is solid");
    let dotted = 0;
    for (let y = 8; y < 44; y++) dotted += c3.on(115, y) ? 1 : 0;
    check(dotted > 2 && dotted < 20, "...and a beat line is dotted (" + dotted + " dots)");
    check(c3.count(0, 56, 127, 63) > 30, "with the bar and beat, and what the jog wheel does");
    // anything it is fed
    const garbage = [{ zm: 2 }, { zm: 2, zp: "zz", zg: 7, zs: "x", zw: -4 }, { zm: 2, zp: "ff".repeat(300), zg: "1".repeat(500), zs: 1e9, zw: 1e9 }, { zm: 2, zp: "", zg: "", zs: NaN, zw: NaN }];
    garbage.forEach((g, i) => {
        const bx = load(() => true), cx = makeCtx({ values: { view: JSON.stringify(Object.assign({}, fixtures[0], g)) } });
        let threw = null;
        try { bx.canvas_overlay.onOpen(cx); bx.canvas_overlay.onValues(cx, { values: { view: JSON.stringify(Object.assign({}, fixtures[0], g)) } }); bx.canvas_overlay.draw(hostCtx(cx)); } catch (e) { threw = e; }
        check(!threw && cx.oob === 0, `zoom garbage ${i}: no exception, nothing off screen`);
    });
    b3.canvas_overlay.onClose(c3);
    check(has(c3, "zoom", "0"), "closing the screen puts the view back to normal");
}

// 6c. the pad lights are handed back when this screen is no longer the one in front
{
    const view = Object.assign({}, fixtures[0], { s: 1, cm: 3, cl: 3 });
    const run = (setup) => {
        const sent = [];
        const bx = load((pkt) => { sent.push(pkt.slice()); return true; });
        const cx = makeCtx({ values: { view: JSON.stringify(view) } });
        bx.canvas_overlay.onOpen(cx);
        bx.canvas_overlay.draw(hostCtx(cx));
        for (let t = 0; t < 40; t++) { cx.t += 20; bx.canvas_overlay.tick(hostCtx(cx)); }
        sent.length = 0;
        setup(bx, cx);
        return { sent, bx, cx };
    };
    const allOff = (sent) => sent.length === 32 && sent.every((p) => p[0] === 0x09 && p[1] === 0x90 && p[3] === 0) && new Set(sent.map((p) => p[2])).size === 32;
    let r = run((bx, cx) => bx.canvas_overlay.onClose(cx));
    check(allOff(r.sent), "closing the screen takes its colours off all 32 pads (" + r.sent.length + " packets)");
    r = run((bx, cx) => bx.canvas_overlay.onMidi(cx, { data: [0xB0, 51, 127] }));
    check(allOff(r.sent), "Back takes them off too, as the screen is left");
    r = run((bx, cx) => { bx.canvas_overlay.onMidi(cx, { data: [0xB0, 51, 0] }); });
    check(r.sent.length === 0, "(letting go of Back does nothing)");
    r = run((bx, cx) => { cx.t += 2000; bx.canvas_overlay.tick(hostCtx(cx)); });
    check(allOff(r.sent), "a screen that has not been drawn for a while is not the one in front: it takes its colours off");
    r.sent.length = 0;
    for (let t = 0; t < 40; t++) { r.cx.t += 20; r.bx.canvas_overlay.tick(r.cx); }
    check(r.sent.length === 0, "...once, and then it sends nothing at all");
    r.bx.canvas_overlay.draw(r.cx);
    for (let t = 0; t < 10; t++) { r.cx.t += 20; r.bx.canvas_overlay.tick(r.cx); }
    check(r.sent.length >= 32 && r.sent.some((p) => p[3] !== 0), "when it is drawn again it paints the pads again");
    r = run((bx, cx) => { bx.canvas_overlay.onMidi(cx, { data: [0xB0, 51, 127] }); bx.canvas_overlay.onMidi(cx, { data: [0xB0, 51, 127] }); bx.canvas_overlay.onClose(cx); bx.canvas_overlay.onClose(cx); });
    check(r.sent.length === 32, "letting go twice sends the packets once");
    const nolight = load(null), cn = makeCtx({ values: { view: JSON.stringify(view) } });
    let threw = null;
    try { nolight.canvas_overlay.onOpen(cn); nolight.canvas_overlay.onMidi(cn, { data: [0xB0, 51, 127] }); nolight.canvas_overlay.onClose(cn); } catch (e) { threw = e; }
    check(!threw, "with no LED path, letting go does not throw");
}

// 7. full screen: the knobs are passed on
{
    const b2 = load(() => true);
    const view = Object.assign({}, fixtures[0], { s: 2, cm: 3, tt: 0.5, dr: 0.5, gn: 0, ng: 1, vv: 0, no: 1 });
    const ctx = makeCtx({ values: { view: JSON.stringify(view) } });
    b2.canvas_overlay.onOpen(ctx);
    const knob = (n, v) => b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 71 + n, v] });
    ctx.sets.length = 0;
    knob(0, 1); knob(0, 127);
    check(ctx.sets.length === 0, "dial 1 opens this screen from the first page: it sets nothing here");
    ctx.sets.length = 0;
    for (const n of [3, 4, 5, 6, 7]) { knob(n, 1); knob(n, 127); }
    check(ctx.sets.length === 0, "the dials after the last one are nothing");
    ctx.sets.length = 0;
    knob(1, 1);
    check(!has(ctx, "gen_style"), "Style waits for a second detent");
    knob(1, 1);
    check(has(ctx, "gen_style", "1"), "...then goes one style on (Any, then Baby): " + JSON.stringify(ctx.sets));
    for (let i = 0; i < 100; i++) knob(1, 1);
    check(ctx.sets.filter(([k]) => k === "gen_style").slice(-1)[0][1] === "11", "it stops at Drag");
    for (let i = 0; i < 100; i++) knob(1, 127);
    check(ctx.sets.filter(([k]) => k === "gen_style").slice(-1)[0][1] === "0", "...and at Any");
    ctx.sets.length = 0;
    knob(2, 1); knob(2, 1);
    check(!has(ctx, "gen_bars"), "Length waits for three detents");
    knob(2, 1);
    check(has(ctx, "gen_bars", "2"), "...then 4 bars (from 2): " + JSON.stringify(ctx.sets));
    for (let i = 0; i < 30; i++) knob(2, 127);
    check(ctx.sets.filter(([k]) => k === "gen_bars").slice(-1)[0][1] === "0", "...down to 1 bar");
    ctx.sets.length = 0;
    for (let n = 0; n < 8; n++) for (let i = 0; i < 4; i++) knob(n, 1);
    const written = new Set(ctx.sets.map(([k]) => k));
    check(["gen_style", "gen_bars"].every((k) => written.has(k)) && written.size === 2,
        "the two dials do something, and nothing else is written: " + [...written].join(" "));
    // a dial says what it did, on the bottom line
    {
        const c = makeCtx({ values: {} }), bt = load(() => true);
        const vw = Object.assign({}, fixtures[0], { cm: 0, gs: 0, gb: 1, cd: 1, co: 0, tt: 0.5, dr: 0.5, gn: 0, rc: 0 });
        bt.canvas_overlay.onOpen(c);
        bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(vw) } });
        const bottom = () => JSON.stringify([...c.px.entries()].filter(([k, v]) => v && +k.split(",")[1] >= 58));
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        const before = bottom();
        for (let i = 0; i < 2; i++) bt.canvas_overlay.onMidi(c, { data: [0xB0, 72, 1] });          // dial 2: Style
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        const during = bottom();
        check(before !== during && c.count(0, 58, 127, 63) > 20, "turning a dial puts its name and value on the bottom line");
        c.t += 2500; c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        check(bottom() !== during, "...and it goes again after a moment");
    }
    // Style and Length are on the Pads view all the time
    {
        const bt = load(() => true), a = makeCtx({ values: {} }), b = makeCtx({ values: {} });
        for (const [cx, g] of [[a, { gs: 3, gb: 0 }], [b, { gs: 9, gb: 2 }]]) {
            bt.canvas_overlay.onOpen(cx);
            bt.canvas_overlay.onValues(cx, { values: { view: JSON.stringify(Object.assign({}, fixtures[0], { cm: 0, rw: 1 }, g)) } });
            bt.canvas_overlay.draw(hostCtx(cx));
        }
        const row = (cx) => JSON.stringify([...cx.px.entries()].filter(([k, v]) => v && +k.split(",")[1] >= 58 && +k.split(",")[0] >= 60));
        check(row(a) !== row(b) && a.count(60, 58, 127, 63) > 15, "the style and the length Generate will use are shown on the Pads view");
    }
    // the Move lights a pressed pad (and the one an octave away) itself: whatever it writes is put back at once
    {
        const sentF = [], bf = load((pkt) => { sentF.push(pkt.slice()); return true; }), cf = makeCtx({ values: {} });
        const snap = {};
        for (let n = 68; n < 100; n++) snap[n] = 0;
        bf.shadow_get_pad_led_snapshot = () => snap;
        bf.canvas_overlay.onOpen(cf);
        bf.canvas_overlay.onValues(cf, { values: { view: JSON.stringify(Object.assign({}, fixtures[0], { cm: 0, rc: 0 })) } });
        for (let t = 0; t < 20; t++) { cf.t += 20; bf.canvas_overlay.draw(hostCtx(cf)); bf.canvas_overlay.tick(hostCtx(cf)); }
        const quiet = sentF.length;
        for (let t = 0; t < 10; t++) { cf.t += 20; bf.canvas_overlay.tick(hostCtx(cf)); }
        const n0 = sentF.length;
        snap[68 + 8 + 7] = 11;                               // the Move lights the last pad of a row green
        cf.t += 5; bf.canvas_overlay.tick(hostCtx(cf));
        const put = sentF.slice(n0).filter((p) => p[2] === 68 + 8 + 7);
        check(put.length >= 1 && put[0][3] === bf.__tinyscratch.COL.rec, "a pad the Move lit is put back in the same tick (" + JSON.stringify(put) + ")");
        // a press repaints that pad and the other end of its row straight away
        const n1 = sentF.length;
        bf.canvas_overlay.onMidi(cf, { data: [0x90, 68 + 16 + 0, 100] });
        const now1 = sentF.slice(n1).map((p) => p[2]);
        check(now1.includes(68 + 16) && now1.includes(68 + 16 + 7), "a press repaints the pad and its twin at the other end of the row at once (" + now1 + ")");
        bf.canvas_overlay.onMidi(cf, { data: [0x80, 68 + 16 + 0, 0] });
        const n2 = sentF.length;
        bf.canvas_overlay.onMidi(cf, { data: [0x90, 68 + 16 + 3, 100] });
        check(!sentF.slice(n2).some((p) => p[2] === 68 + 16 + 4), "(a pad in the middle of a row has no twin)");
    }
    // leaving the screen hands the pads back as the Move had them, and a slot change (the Move has the display) does it at once
    {
        const sentR = [], br = load((pkt) => { sentR.push(pkt.slice()); return true; }), cr = makeCtx({ values: {} });
        const snap = {};
        for (let n = 68; n < 100; n++) snap[n] = n % 7 + 1;
        br.shadow_get_pad_led_snapshot = () => snap;
        let mode = 1;
        br.shadow_get_display_mode = () => mode;
        br.canvas_overlay.onOpen(cr);
        br.canvas_overlay.onValues(cr, { values: { view: JSON.stringify(Object.assign({}, fixtures[0], { cm: 0 })) } });
        for (let t = 0; t < 20; t++) { cr.t += 20; br.canvas_overlay.draw(hostCtx(cr)); br.canvas_overlay.tick(hostCtx(cr)); }
        const n0 = sentR.length;
        mode = 0;                                            // another slot: the Move has the display
        cr.t += 20; br.canvas_overlay.tick(hostCtx(cr));
        const back = sentR.slice(n0);
        check(back.length === 32 && back.every((p) => p[3] === p[2] % 7 + 1), "the Move has the display: every pad goes back to the colour the Move had (" + back.length + " packets)");
        const n1 = sentR.length;
        for (let t = 0; t < 10; t++) { cr.t += 20; br.canvas_overlay.tick(hostCtx(cr)); }
        check(sentR.length === n1, "...once, and then nothing is sent");
        mode = 1;
        br.canvas_overlay.draw(hostCtx(cr));
        for (let t = 0; t < 10; t++) { cr.t += 20; br.canvas_overlay.tick(hostCtx(cr)); }
        check(sentR.length > n1 + 31, "back on the screen, the pads are painted again");
        const n2 = sentR.length;
        br.canvas_overlay.onClose(cr);
        const closed = sentR.slice(n2);
        check(closed.length >= 32 && closed.slice(-32).every((p) => p[3] === p[2] % 7 + 1), "closing the screen hands them back the same way");
    }
    // touching a dial says what it is (and what it is set to) before it is turned
    {
        const bt = load(() => true), c = makeCtx({ values: {} });
        const vw = Object.assign({}, fixtures[0], { cm: 0, gs: 3, gb: 1, cd: 1, co: 0 });
        bt.canvas_overlay.onOpen(c);
        bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(vw) } });
        const bar = () => { c.clear(); bt.canvas_overlay.draw(hostCtx(c)); return c.count(0, 57, 127, 63); };
        const plain = bar();
        bt.canvas_overlay.onMidi(c, { data: [0x90, 1, 127] });                       // the touch of dial 2: Style
        const touched = bar();
        check(touched > plain + 150, "touching a dial puts its name on the bottom line, white on black (" + plain + " -> " + touched + ")");
        const sets0 = c.sets.length;
        bt.canvas_overlay.onMidi(c, { data: [0x80, 1, 0] });
        check(bar() < touched - 100 && c.sets.length === sets0, "...and it goes when the finger does, setting nothing");
        bt.canvas_overlay.onMidi(c, { data: [0x90, 7, 127] });
        const none = bar();
        check(none > plain + 100, "a touch of a dial with nothing on it says so");
        bt.canvas_overlay.onMidi(c, { data: [0x80, 7, 0] });
        const ls0 = c.sets.length;
        for (let n = 0; n < 8; n++) { bt.canvas_overlay.onMidi(c, { data: [0x90, n, 127] }); bt.canvas_overlay.onMidi(c, { data: [0x80, n, 0] }); }
        check(c.sets.length === ls0, "touches never set a parameter or play a pad");
    }
    // the Auto pad is a switch: tap it while Auto plays and Auto is off
    {
        const bt = load(() => true), c = makeCtx({ values: {} });
        const auto = 68 + 16 + 7;
        bt.canvas_overlay.onOpen(c);
        bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(Object.assign({}, fixtures[0], { cm: 3 })) } });
        c.sets.length = 0;
        bt.canvas_overlay.onMidi(c, { data: [0x90, auto, 100] }); bt.canvas_overlay.onMidi(c, { data: [0x80, auto, 0] });
        check(has(c, "clip", "17"), "the Auto pad turns Auto on");
        bt.canvas_overlay.onMidi(c, { data: [0x90, auto, 100] }); bt.canvas_overlay.onMidi(c, { data: [0x80, auto, 0] });
        check(has(c, "clip", "0"), "...and tapped again turns it off");
        bt.canvas_overlay.onMidi(c, { data: [0x90, auto, 100] }); bt.canvas_overlay.onMidi(c, { data: [0x80, auto, 0] });
        check(c.sets.filter(([k]) => k === "clip").slice(-1)[0][1] === "17", "...and again on");
    }
    // an imported clip is read-only: held, it says so and stays
    {
        const bt = load(() => true), c = makeCtx({ values: {} });
        const vw = Object.assign({}, fixtures[0], { s: 0, cm: 0, cf: "1100000000000000", kp: "0000000000000000", ro: "1100000000000000" });
        const feed = () => bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(vw) } });
        bt.canvas_overlay.onOpen(c);
        feed();
        c.sets.length = 0;
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        const idle = c.count(0, 56, 127, 57);
        bt.canvas_overlay.onMidi(c, { data: [0x90, 68 + 24, 100] });
        c.t += 700; feed();
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        check(c.count(0, 56, 127, 57) === idle, "no countdown for a read-only clip");
        c.t += 1500; feed();
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        check(!has(c, "clip_delete") && c.count(0, 57, 127, 63) > 30, "held to the end: nothing is deleted, and the screen says why");
        bt.canvas_overlay.onMidi(c, { data: [0x80, 68 + 24, 0] });
    }
    // Record is the last pad of the second row: tap to start, tap to stop
    {
        const bt = load(() => true), c = makeCtx({ values: {} });
        const vw = Object.assign({}, fixtures[0], { cm: 0, rc: 0 });
        bt.canvas_overlay.onOpen(c);
        bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(vw) } });
        const rec = 68 + 8 + 7;
        c.sets.length = 0;
        bt.canvas_overlay.onMidi(c, { data: [0x90, rec, 100] }); bt.canvas_overlay.onMidi(c, { data: [0x80, rec, 0] });
        check(has(c, "record", "1"), "the Record pad starts a recording");
        bt.canvas_overlay.onMidi(c, { data: [0x90, rec, 100] }); bt.canvas_overlay.onMidi(c, { data: [0x80, rec, 0] });
        check(has(c, "record", "0"), "...and a second tap stops it");
        check(bt.__tinyscratch.padColor({ view: Object.assign({}, vw, { rc: 1 }), held: {}, hold: null }, 15) === bt.__tinyscratch.COL.cut &&
              bt.__tinyscratch.padColor({ view: vw, held: {}, hold: null }, 15) === bt.__tinyscratch.COL.rec, "it is red while it records");
    }
    // a User sample held for a moment is unloaded; Ahhh and Fresssh never are
    {
        const bt = load(() => true), c = makeCtx({ values: {} });
        const vw = Object.assign({}, fixtures[0], { s: 0, ld: "111100", cm: 0, cf: "11111111", kp: "00000000" });
        const feed = () => bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(vw) } });
        bt.canvas_overlay.onOpen(c);
        feed();
        const down = (n) => bt.canvas_overlay.onMidi(c, { data: [0x90, n, 100] });
        const up = (n) => bt.canvas_overlay.onMidi(c, { data: [0x80, n, 0] });
        const pad3 = 68 + 8 + 3;                           // User 2: the fourth sample pad
        c.sets.length = 0;
        down(pad3);
        check(has(c, "sample", "3") && !has(c, "clear"), "pressing a loaded sample pad picks it");
        c.t += 700; feed();
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        check(!has(c, "clear") && c.count(0, 56, 127, 57) > 20, "held for a moment: a countdown is shown");
        up(pad3);
        c.t += 2000; feed();
        check(!has(c, "clear"), "let go before the end: it stays");
        down(pad3);
        c.t += 1700; feed();
        check(has(c, "clear", "1"), "held to the end: it is unloaded");
        up(pad3);
        for (const n of [68 + 8, 68 + 8 + 1]) {
            c.sets.length = 0;
            down(n);
            c.t += 3000; feed();
            check(!has(c, "clear"), "Ahhh and Fresssh held for ever are not unloaded");
            up(n);
        }
        c.sets.length = 0;
        down(68 + 8 + 5);                                  // User 4 is empty
        c.t += 3000; feed();
        check(!has(c, "clear"), "an empty slot has nothing to unload");
        up(68 + 8 + 5);
    }
    // a clip you made, held for a moment: it is deleted (a countdown shows), and letting go first keeps it
    {
        const bt = load(() => true), c = makeCtx({ values: {} });
        const vw = Object.assign({}, fixtures[0], { cm: 2, cl: 2, cf: "1111111110000000", kp: "0000000010000000" });
        const feed = () => bt.canvas_overlay.onValues(c, { values: { view: JSON.stringify(vw) } });
        bt.canvas_overlay.onOpen(c);
        feed();
        const down = (n) => bt.canvas_overlay.onMidi(c, { data: [0x90, n, 100] });
        const up = (n) => bt.canvas_overlay.onMidi(c, { data: [0x80, n, 0] });
        const nine = 68 + 16;                              // clip 9: the row below the top, first pad
        c.sets.length = 0;
        down(nine);
        check(has(c, "clip_pad", "9") && !has(c, "clip_delete"), "pressing a clip you made plays it");
        c.t += 700; feed();
        c.clear(); bt.canvas_overlay.draw(hostCtx(c));
        check(!has(c, "clip_delete") && c.count(0, 56, 127, 57) > 20, "held for a moment: not deleted, and a countdown is shown");
        up(nine);
        c.t += 2000; feed();
        check(!has(c, "clip_delete"), "let go before the end: the clip is kept");
        down(nine);
        c.t += 1700; feed();
        check(has(c, "clip_delete", "9"), "held to the end: the clip is deleted");
        const n = c.sets.filter(([k]) => k === "clip_delete").length;
        c.t += 1700; feed();
        check(c.sets.filter(([k]) => k === "clip_delete").length === n, "...once");
        up(nine);
        c.sets.length = 0;
        down(68 + 24 + 1);
        c.t += 3000; feed();
        check(has(c, "clip_delete", "2"), "a built-in clip, held, is deleted too (any clip can go)");
        up(68 + 24 + 1);
        c.sets.length = 0;
        down(68 + 16 + 2);                                 // an empty pad (clip 11)
        c.t += 3000; feed();
        check(!has(c, "clip_delete"), "an empty pad has nothing to delete");
        up(68 + 16 + 2);
    }
    ctx.sets.length = 0;
    b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 3, 127] });                // the jog wheel's click, other CCs: nothing
    b2.canvas_overlay.onMidi(ctx, { data: [0x90, 3, 127] });
    b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 79, 1] });
    b2.canvas_overlay.onMidi(ctx, { data: [0xB0, 70, 1] });
    b2.canvas_overlay.onMidi(ctx, { data: [] });
    b2.canvas_overlay.onMidi(ctx, {});
    check(ctx.sets.length === 0, "anything else is left alone");
    // full screen draws the same picture
    ctx.t += 50;
    b2.canvas_overlay.draw(hostCtx(ctx));
    check(ctx.count(0, 0, 127, 63) > 350 && ctx.cleared >= 1, "the full-screen draw paints the picture");
    const empty = makeCtx();
    b2.canvas_overlay.draw(hostCtx(empty));
    check(empty.count(0, 20, 127, 40) > 30, "before the first view it says it is waiting");
    b2.canvas_overlay.onClose(ctx);
    check(ctx.state.ts === null, "closing lets go of its state");
}

// 8. it copes with anything
{
    const garbage = [
        "", "null", "[]", "{", "not json", "{}", '{"v":3}', JSON.stringify({ w: 5, x: "a", p: "z", sn: 7, ld: 3, lnp: {}, lng: [], wr: null, gn: "x" }),
        JSON.stringify({ x: 1e9, tx: -1e9, p: 99, lp: -7, s: 99, w: "z".repeat(600), wr: "f".repeat(600), lnp: "g".repeat(500), lng: "1".repeat(500), lcol: 1e9, sn: "A|B|C|D|E|F|G|H", ac: 99, pr: -5 }),
        JSON.stringify({ x: NaN, p: NaN, gn: Infinity, lnp: "0", lng: "1", w: "f", wr: "f", lcol: NaN }),
        JSON.stringify(Object.assign({}, fixtures[0], { sn: "|||||", ld: "" })),
    ];
    garbage.forEach((g, i) => {
        let ctx = null, threw = null;
        try { ctx = draw(box, g); } catch (e) { threw = e; }
        check(!threw, `garbage ${i}: no exception (${threw && threw.message})`);
        check(ctx && ctx.oob === 0 && ctx.cleared === 0, `garbage ${i}: nothing off screen, no error screen`);
        const b2 = load(() => true);
        const c = makeCtx({ values: { view: typeof g === "string" ? g : JSON.stringify(g) } });
        let t2 = null;
        try { b2.canvas_overlay.onOpen(c); b2.canvas_overlay.tick(hostCtx(c)); b2.canvas_overlay.draw(hostCtx(c)); b2.canvas_overlay.onValues(c, { values: { view: g } }); b2.canvas_overlay.onMidi(c, { data: [0xB0, 72, 1] }); b2.canvas_overlay.draw(hostCtx(c)); } catch (e) { t2 = e; }
        check(!t2, `garbage ${i}: full screen does not throw either (${t2 && t2.message})`);
    });
    const wait = draw(box, "");
    check(wait.count(0, 20, 127, 40) > 30, "no view yet: it says it is waiting");
    let threw = null;
    try { box.canvas_overlay.drawPage(makeCtx(), {}); box.canvas_overlay.drawPage(makeCtx()); box.canvas_overlay.drawPage(makeCtx(), { values: null }); } catch (e) { threw = e; }
    check(!threw, "drawPage with no arguments does not throw (the host switches a script off for good if it does)");
    let escaped = null;
    try { box.canvas_overlay.drawPage({ setPixel() { throw new Error("boom"); }, fillRect() { throw new Error("boom"); }, clear() {} }, { values: { view: JSON.stringify(fixtures[0]) } }); } catch (e) { escaped = e; }
    check(!escaped, "an error inside drawing is caught and shown, not thrown");
    let esc2 = null;
    try { box.canvas_overlay.onMidi(null, null); box.canvas_overlay.tick({}); box.canvas_overlay.onOpen({ state: {} }); } catch (e) { esc2 = e; }
    check(!esc2, "full-screen hooks given nothing do not throw");
}

// 9. the labels and the pad grid agree with the engine's
{
    check(T.padIndex(67) === 31 && T.padIndex(68) === 0 && T.padIndex(99) === 31 && T.padIndex(36) === 0 && T.padIndex(35) === -1 && T.padIndex(100) === -1, "pad notes: 36-67 and 68-99 are the same 32 pads");
    check(T.KNOBS.length === 3 && T.KNOBS[0] === null && T.KNOBS[1].key === "gen_style" && T.KNOBS[2].key === "gen_bars", "the dials");
}

// 10. the waveform screen
{
    const wsrc = fs.readFileSync(path.join(root, "src", "waveform.js"), "utf8");
    const wbox = {};
    vm.createContext(wbox);
    vm.runInContext(wsrc, wbox, { filename: "waveform.js" });
    check(wbox.canvas_overlay && typeof wbox.canvas_overlay.drawPage === "function", "waveform.js registers drawPage");
    const wdraw = (view) => {
        const ctx = makeCtx();
        wbox.canvas_overlay.drawPage(ctx, { values: { view: typeof view === "string" ? view : JSON.stringify(view) } });
        return ctx;
    };
    fixtures.forEach((v, i) => {
        const ctx = wdraw(v);
        const lit = [...ctx.px.values()].filter((x) => x).length;
        check(ctx.oob === 0 && ctx.cleared === 0, `waveform fixture ${i}: on the screen, no error screen`);
        check(lit > 250 && lit < 7000, `waveform fixture ${i}: a drawn screen (${lit} pixels)`);
        const x = Math.round(Number(v.x));
        let full = 0;
        for (let y = 7; y < 38; y++) full += ctx.on(x, y) ? 1 : 0;
        check(full >= 31, `waveform fixture ${i}: the playhead runs the height of the band at column ${x} (${full})`);
        let low = 0;
        for (const [k, on] of ctx.px) if (on && Number(k.split(",")[1]) >= 46) low++;
        check(low === 0, `waveform fixture ${i}: nothing below row 45, where the host's footer hides it (${low} pixels)`);
    });
    // the detail: a column to every pixel, the outline continuous, the body solid
    const peaks = "00".repeat(30) + "ff".repeat(30) + "80".repeat(30) + "10".repeat(38);
    const body = "00".repeat(30) + "a0".repeat(30) + "40".repeat(30) + "08".repeat(38);
    const v = Object.assign({}, fixtures[0], { w: peaks, wr: body, x: 127, g: 0, cl: 0 });
    const ctx = wdraw(v);
    const col = (c) => ctx.count(c, 9, c, 37);
    check(col(10) <= 2, `silence is a thin line (${col(10)})`);
    check(col(40) > 15 && col(40) > col(70) && col(70) > col(100), `the heights follow the sound at full height (${col(40)}, ${col(70)}, ${col(100)})`);
    check(ctx.on(40, 10) && ctx.on(40, 36), "the peaks reach nearly to the top and the bottom of the band");
    let gaps = 0;
    gaps = col(29) >= 5 && col(30) >= 5 ? 0 : 1;
    check(gaps === 0, "the outline is joined up where the sound steps from quiet to loud");
    // the top bar lights while the sound is cut in; the target mark under the band; the ruler; the readout
    check(!wdraw(v).on(60, 7) && wdraw(Object.assign({}, v, { g: 1 })).count(0, 7, 127, 7) === 128, "the top bar lights while the sound is cut in");
    const tg = wdraw(Object.assign({}, v, { cl: 3, tx: 90 })), no = wdraw(Object.assign({}, v, { cl: 0, tx: 90 }));
    check(tg.count(88, 38, 92, 38) >= 5 && no.count(88, 38, 92, 40) === 0, "a mark shows where the clip is sending the playhead (and only with a clip)");
    check([0, 13, 25, 38, 51, 64, 76, 89, 102, 114, 127].every((x) => ctx.on(x, 43)) && ctx.on(0, 44) && ctx.on(64, 44) && !ctx.on(13, 44), "the ruler marks tenths of the sample, taller at the ends and the middle");
    const sp = wdraw(Object.assign({}, v, { vl: 1.5 })), sn = wdraw(Object.assign({}, v, { vl: -1.5 })), sz = wdraw(Object.assign({}, v, { vl: 0 }));
    let d1 = 0, d2 = 0;
    for (let x = 48; x < 80; x++) for (let y = 0; y < 7; y++) { d1 += sp.on(x, y) !== sz.on(x, y) ? 1 : 0; d2 += sp.on(x, y) !== sn.on(x, y) ? 1 : 0; }
    check(d1 > 3 && d2 > 0, "the speed is read out, and backwards has a minus");
    const nm = wdraw(Object.assign({}, v, { n: "EAGLE" }));
    check(nm.count(0, 0, 40, 6) > 10, "the sample's name is at the top");
    // it copes with anything
    for (const g of ["", "null", "[]", "{", "{}", JSON.stringify({ w: 5, x: "a", p: "z", n: 7, vl: "x", tx: NaN, cl: 99, g: 1 }),
                     JSON.stringify({ x: 1e9, tx: -1e9, p: 99, w: "z".repeat(600), wr: "f".repeat(600), vl: 1e12, n: "A".repeat(500), cl: 1, g: 1 }),
                     JSON.stringify({ x: NaN, p: NaN, vl: Infinity })]) {
        let c = null, threw = null;
        try { c = wdraw(g); } catch (e) { threw = e; }
        check(!threw && c && c.oob === 0 && c.cleared === 0, `waveform garbage ${JSON.stringify(g).slice(0, 24)}: no exception, nothing off screen`);
    }
    check(wdraw("").count(0, 20, 127, 40) > 30, "waveform, no view yet: it says it is waiting");
    let esc = null;
    try { wbox.canvas_overlay.drawPage({ setPixel() { throw new Error("boom"); }, fillRect() { throw new Error("boom"); }, clear() {} }, { values: { view: JSON.stringify(fixtures[0]) } }); wbox.canvas_overlay.drawPage(makeCtx()); wbox.canvas_overlay.drawPage(null, null); } catch (e) { esc = e; }
    check(!esc, "an error inside drawing is caught, not thrown");
}

// 11. as the module's page the picture is 128 x 46: the host's header and footer hide the rest
{
    fixtures.forEach((v, i) => {
        const ctx = drawPage(box, v);
        let low = 0, lit = 0;
        for (const [k, on] of ctx.px) if (on) { lit++; if (Number(k.split(",")[1]) >= 46) low++; }
        check(ctx.oob === 0 && ctx.cleared === 0, `page fixture ${i}: on the screen, no error screen`);
        check(low === 0, `page fixture ${i}: nothing below row 45 (${low} pixels would be hidden)`);
        check(lit > 350, `page fixture ${i}: a drawn page (${lit} pixels)`);
        check(ctx.count(0, 0, 105, 9) > 25, `page fixture ${i}: the sample in use is the title`);
        const x = Math.round(Number(v.x) * 105 / 127);
        let full = 0;
        for (let y = 11; y < 29; y++) full += ctx.on(x, y) ? 1 : 0;
        check(full === 18, `page fixture ${i}: the playhead is a solid line through the waveform (${full} of 18)`);
        check(ctx.on(0, 30) && ctx.on(83, 30) && ctx.on(0, 45) && ctx.on(83, 45), `page fixture ${i}: the clip view has its frame, 84 wide`);
        const cur = Math.round(Number(v.lcol) * 83 / 127);
        let dots = 0;
        for (let y = 31; y < 45; y++) dots += ctx.on(cur, y) ? 1 : 0;
        check(dots >= 5, `page fixture ${i}: the cursor is in the clip view (${dots} dots at column ${cur})`);
        check(ctx.count(86, 31, 127, 37) > 8, `page fixture ${i}: the panel names the clip`);
    });
    const base = Object.assign({}, fixtures[0], { cm: 9, cl: 4, ru: 1, g: 0, nh: 0, ln: 0 });
    const run = drawPage(box, base), idle = drawPage(box, Object.assign({}, base, { ru: 0 }));
    check(idle.count(86, 38, 127, 43) > 25 && run.count(86, 38, 127, 43) < 5, "the panel says PRESS PLAY when a clip waits for the transport");
    check(drawPage(box, Object.assign({}, base, { ru: 0, nh: 1 })).count(86, 38, 127, 43) < 5, "...but not while a cut is held");
    const cut = drawPage(box, Object.assign({}, base, { g: 1 }));
    check(cut.count(86, 38, 127, 43) > 8, "...and says CUT while the sound is cut in");
    const tryA = drawPage(box, Object.assign({}, base, { cm: 18, cl: 18, tg: 1, ln: 0 })), tryB = drawPage(box, Object.assign({}, base, { cm: 18, cl: 18, tg: 5, ln: 0 }));
    let styleDiff = 0;
    for (let x = 86; x < 128; x++) for (let y = 31; y < 38; y++) styleDiff += tryA.on(x, y) !== tryB.on(x, y) ? 1 : 0;
    check(styleDiff > 5 && tryA.count(86, 31, 127, 37) > 8, "a generated clip says TRY and its style on the panel (" + styleDiff + " pixels differ between styles)");
    const noted = drawPage(box, Object.assign({}, base, { ln: 68 }));
    let more = 0;
    for (let x = 108; x < 128; x++) for (let y = 32; y < 37; y++) more += noted.on(x, y) && !run.on(x, y) ? 1 : 0;
    check(more > 5, "the last note that arrived is shown (N68), to see what the pads send");
    // the lane lines up with the data in the compact version too
    const lnp = "0123456789abcdef".repeat(8), lng = ("1111100000".repeat(13)).slice(0, 128);
    const lane = drawPage(box, Object.assign({}, base, { lnp, lng, lcol: -1 }));
    let blocks = 0, path = 0;
    for (let x = 1; x < 83; x++) { blocks += lane.on(x, 31) ? 1 : 0; path += lane.count(x, 34, x, 42) > 0 ? 1 : 0; }
    check(blocks > 25 && blocks < 55, `cuts show as blocks along the top of the lane (${blocks} of 82 columns)`);
    check(path >= 80, `the path runs across the lane (${path} of 82 columns)`);
    check([1, 2, 3].every((b) => lane.on(Math.round(b * 32 * 83 / 127), 44)), "the beats are ticked");
    const hi = drawPage(box, Object.assign({}, base, { lnp: "f".repeat(128), lng: "0".repeat(128), lcol: -1 })), lo = drawPage(box, Object.assign({}, base, { lnp: "0".repeat(128), lng: "0".repeat(128), lcol: -1 }));
    check(hi.count(20, 34, 60, 36) > 20 && hi.count(20, 41, 60, 42) < 3 && lo.count(20, 41, 60, 42) > 20 && lo.count(20, 34, 60, 36) < 3, "high is up, low is down");
}

console.log(`screen: ${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
