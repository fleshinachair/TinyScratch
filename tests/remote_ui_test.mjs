/*
 * The Remote UI (src/web_ui.html), in a real browser, against the real engine.
 *
 * scripts/build_demo.sh records sixteen bars of the engine (tools/remote_demo.c): what the host would send the page (rview / rclip
 * twice a second, and the params), and where the playhead really was every 20 ms. The page plays the recording back as the host
 * sends it, runs the clip itself between reads, and draws the record from that. This holds what the page drew to where the
 * playhead was, at the same moment: the record has to move with the scratch, not half a second behind it.
 *
 * Needs Playwright (and its Chromium); skipped without it. Run: node tests/remote_ui_test.mjs [demo.html]
 */
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { resolve } from "node:path";

const require = createRequire(import.meta.url);
let chromium;
try { ({ chromium } = require("playwright")); } catch (e) { console.log("   SKIPPED: playwright not found"); process.exit(0); }

const page = resolve(process.argv[2] || "build/demo/tinyscratch_remote_demo.html");
if (!existsSync(page)) { console.error("no demo page: run scripts/build_demo.sh first"); process.exit(1); }

let fails = 0;
function check(ok, what) { console.log((ok ? "  ok   " : "  FAIL ") + what); if (!ok) fails++; }

const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const pg = await browser.newPage({ viewport: { width: 1280, height: 900 } });
const errors = [];
pg.on("pageerror", (e) => errors.push(e.message));
pg.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
await pg.goto("file://" + page);
await pg.click("#demoGo");

/* where the page says the playhead is, each frame, against the sound's clock, for 20 s */
const samples = await pg.evaluate(() => new Promise((done) => {
  const out = [], T = window.__tinyscratch, t0 = performance.now();
  (function tick() {
    const a = T.audio();
    if (a && a.currentTime > 0.6) out.push([a.currentTime, T.sim.pos, T.sim.clip ? 1 : 0]);
    if (performance.now() - t0 < 20000) requestAnimationFrame(tick); else done(out);
  })();
}));
const truth = await pg.evaluate(() => ({ ms: window.TS_DEMO.truth_ms, p: window.TS_DEMO.truth }));
await browser.close();

/* the error, with the page's guess shifted by lag seconds (0: the same moment) */
function errorsAt(lag) {
  const errs = [];
  for (const [t, p, clip] of samples) {
    if (!clip) continue;
    const i = (t + lag) * 1000 / truth.ms, i0 = Math.floor(i), u = i - i0;
    if (i0 < 0 || i0 + 1 >= truth.p.length) continue;
    errs.push(Math.abs(p - (truth.p[i0] + (truth.p[i0 + 1] - truth.p[i0]) * u)));
  }
  errs.sort((a, b) => a - b);
  return { n: errs.length, median: errs[errs.length >> 1] || 1, p90: errs[Math.floor(errs.length * 0.9)] || 1 };
}
const now = errorsAt(0), late = errorsAt(-0.5);
let best = { lag: 0, median: 9 };
for (let lag = -0.6; lag <= 0.6; lag += 0.01) { const e = errorsAt(lag); if (e.median < best.median) best = { lag, median: e.median }; }

console.log(`   ${samples.length} frames drawn, ${now.n} while a clip played`);
console.log(`   error at the same moment: median ${(now.median * 100).toFixed(1)}% of the sample, 90th percentile ${(now.p90 * 100).toFixed(1)}%`);
console.log(`   (half a second late, as the host's reads alone would be: median ${(late.median * 100).toFixed(1)}%; the best fit is ${(best.lag * 1000).toFixed(0)} ms)`);
check(errors.length === 0, "no errors in the page" + (errors.length ? ": " + errors.slice(0, 3).join(" | ") : ""));
check(now.n > 300, "the page drew a clip playing for most of the 20 s");
check(now.median < 0.04, "the record is where the playhead is: median error under 4% of the sample");
check(now.p90 < 0.15, "...and 90% of the time under 15%");
check(now.median < late.median / 2, "much closer than drawing each read as it arrives");
check(Math.abs(best.lag) <= 0.08, "in time: the best fit is within 80 ms of the same moment");
console.log(fails ? `${fails} failed` : "remote UI: all passed");
process.exit(fails ? 1 : 0);
