/* Records a demo of the Remote UI (src/web_ui.html) from the real engine: loads the plugin the way Schwung does, plays a scripted
 * set at 92 bpm (Auto, then Generate in a few styles, another sample, the Playback dials turned), and writes
 *   <out>.json   what the browser would have been sent: every param change, and rview / rclip as the host's extras pump
 *                sends them (twice a second, and 150 ms after an edit), each stamped with the time it was sent
 *   <out>.wav    what it sounded like, with a plain drum loop under it so the scratches have a song to sit in
 * scripts/build_demo.sh turns the two into a page that plays them back through the same code the Move feeds.
 * Usage: remote_demo <module dir with samples/, recipes.txt, kept.bin> <out prefix> */
#include "../src/schwung_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define BPM 92.0
#define BARS 16
#define BLOCK 128

static double g_beat;
static float host_bpm(void) { return (float)BPM; }
static int host_clock(void) { return TS_CLOCK_RUNNING; }
static double host_beat(void) { return g_beat; }

static const ts_plugin_api_v2_t *api;
static void *inst;
static FILE *js;
static int nframes_out;
static char last_rview[65536], last_rclip[65536], last_rpads[65536];

/* a JSON string of a value (the values are ours: only quotes and backslashes to escape) */
static void put_str(const char *s)
{
    fputc('"', js);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', js); fputc(*s, js); }
    fputc('"', js);
}

static void frame_begin(double t) { fprintf(js, "%s\n{\"t\":%.3f,\"p\":{", nframes_out++ ? "," : "", t); }

/* what the extras pump sends: each key only when it moved */
static void push_extras(double t)
{
    static char buf[65536];
    int any = 0;
    if (api->get_param(inst, "rview", buf, sizeof buf) > 0 && strcmp(buf, last_rview)) {
        frame_begin(t); any = 1;
        fputs("\"rview\":", js); put_str(buf);
        strcpy(last_rview, buf);
    }
    if (api->get_param(inst, "rclip", buf, sizeof buf) > 0 && strcmp(buf, last_rclip)) {
        if (!any) frame_begin(t); else fputc(',', js);
        any = 1;
        fputs("\"rclip\":", js); put_str(buf);
        strcpy(last_rclip, buf);
    }
    if (api->get_param(inst, "rpads", buf, sizeof buf) > 0 && strcmp(buf, last_rpads)) {
        if (!any) frame_begin(t); else fputc(',', js);
        any = 1;
        fputs("\"rpads\":", js); put_str(buf);
        strcpy(last_rpads, buf);
    }
    if (any) fputs("}}", js);
}

/* an edit, and the value the host would echo back for it */
static void set(double t, const char *key, const char *val)
{
    char buf[256];
    api->set_param(inst, key, val);
    if (api->get_param(inst, key, buf, sizeof buf) < 0) return;
    frame_begin(t);
    fprintf(js, "\"%s\":", key); put_str(buf);
    fputs("}}", js);
}

/* ---- a drum loop: kick, snare, hats; synthesized, so nothing is borrowed ---- */
static uint32_t rng = 1;
static float noise(void) { rng = rng * 1664525u + 1013904223u; return (float)(int32_t)rng / 2147483648.0f; }

static float drums(double beat, double dt_beat)
{
    static double kick_t = 1e9, snare_t = 1e9, hat_t = 1e9, prev = -1;
    static float hp = 0, hp_in = 0;
    const double step = floor(beat * 4.0), pstep = floor(prev * 4.0);
    if (step != pstep) {
        const int s16 = (int)fmod(step, 32.0);                                  /* two bars of sixteenths */
        static const char kick[] = "x.....x...x.....x.....x..x....x.", snare[] = "....x.......x.......x.......x..x";
        if (kick[s16] == 'x') kick_t = 0;
        if (snare[s16] == 'x') snare_t = 0;
        if (s16 % 2 == 0) hat_t = 0;
    }
    prev = beat;
    const double spb = 60.0 / BPM, dt = dt_beat * spb;
    float out = 0;
    if (kick_t < 0.4) { const double f = 45.0 + 110.0 * exp(-kick_t * 30.0); static double ph; ph += 2 * M_PI * f / SR; out += 0.9f * (float)(sin(ph) * exp(-kick_t * 7.0)); kick_t += dt; }
    if (snare_t < 0.3) { out += 0.35f * noise() * (float)exp(-snare_t * 18.0) + 0.25f * (float)(sin(2 * M_PI * 190.0 * snare_t) * exp(-snare_t * 25.0)); snare_t += dt; }
    if (hat_t < 0.08) { const float n = noise(); hp = 0.6f * (hp + n - hp_in); hp_in = n; out += 0.12f * hp * (float)exp(-hat_t * 60.0); hat_t += dt; }
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: remote_demo <module dir> <out prefix>\n"); return 2; }
    static ts_host_api_t host;
    host.api_version = 2; host.sample_rate = SR; host.frames_per_block = BLOCK;
    host.get_bpm = host_bpm; host.get_clock_status = host_clock; host.get_beat_position = host_beat;
    api = move_plugin_init_v2(&host);
    inst = api->create_instance(argv[1], NULL);
    if (!inst) { fprintf(stderr, "remote_demo: no instance\n"); return 1; }

    char path[1024];
    snprintf(path, sizeof path, "%s.json", argv[2]);
    js = fopen(path, "w");
    snprintf(path, sizeof path, "%s.wav", argv[2]);
    FILE *wav = fopen(path, "wb");
    if (!js || !wav) { fprintf(stderr, "remote_demo: cannot write %s\n", argv[2]); return 1; }

    const double spb = 60.0 / BPM, total = BARS * 4 * spb;
    const int total_frames = (int)(total * SR);
    int16_t *pcm = calloc((size_t)total_frames, sizeof *pcm);
    fprintf(js, "{\"bpm\":%.1f,\"bars\":%d,\"seconds\":%.3f,\"frames\":[", BPM, BARS, total);

    /* the seed: every value the panel shows, as the host sends it when the browser subscribes */
    static const char *const keys[] = { "sample", "clip", "gen_style", "gen_bars", "gen_fwd", "gen_fin", "gen_echo", "gen_groove", "swing", "humanise",
                                        "bite", "clip_depth", "clip_offset", "drag", "gain", "note_gate", "vel_to_vol", "cues", "taste_log", "page" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char buf[64];
        if (api->get_param(inst, keys[i], buf, sizeof buf) < 0) continue;
        frame_begin(0); fprintf(js, "\"%s\":", keys[i]); put_str(buf); fputs("}}", js);
    }

    /* the set: (bar, key, value); the dials that turn are ramped below */
    struct { double bar; const char *key, *val; } script[] = {
        { 0, "sample", "0" }, { 0, "clip", "17" }, { 0, "gen_echo", "0.3" },
        { 3.9, "gen_style", "0" }, { 3.95, "gen_bars", "1" }, { 4, "generate", "1" },
        { 7.9, "gen_style", "1" }, { 8, "generate", "1" },
        { 10, "sample", "1" }, { 10, "clip", "17" },
        { 13.9, "gen_style", "8" }, { 13.95, "gen_bars", "0" }, { 14, "generate", "1" },
    };
    const int nscript = (int)(sizeof script / sizeof script[0]);
    int next = 0;
    double next_pump = 0, edit_pump = -1;
    double last_hu = -1, last_sw = -1, last_bite = -1;

    /* the truth: where the playhead really was, every 20 ms (tests/remote_ui_test.mjs holds the page's own guess to it) */
    const int truth_every = SR / 50;
    double *truth = calloc((size_t)(total_frames / truth_every + 2), sizeof *truth);
    int ntruth = 0;

    int16_t out[BLOCK * 2];
    for (int f = 0; f < total_frames; f += BLOCK) {
        const double t = (double)f / SR;
        g_beat = t / spb;
        const double bar = g_beat / 4.0;
        while (next < nscript && script[next].bar <= bar) { set(t, script[next].key, script[next].val); edit_pump = t + 0.15; next++; }
        /* bars 11-13: Humanise and Swing up, Bite in; bar 15: back down */
        double hu = bar < 11 ? 0.35 : (bar < 13 ? 0.35 + 0.5 * (bar - 11) / 2 : (bar < 15 ? 0.85 : 0.35));
        double sw = bar < 11.5 ? 0.30 : (bar < 13 ? 0.30 + 0.37 * (bar - 11.5) / 1.5 : 0.67);
        double bite = bar < 12 ? 0.10 : (bar < 13 ? 0.10 + 0.4 * (bar - 12) : 0.5);
        hu = round(hu / 0.02) * 0.02; sw = round(sw / 0.02) * 0.02; bite = round(bite / 0.02) * 0.02;
        char v[32];
        if (hu != last_hu) { snprintf(v, sizeof v, "%.2f", hu); set(t, "humanise", v); last_hu = hu; edit_pump = t + 0.15; }
        if (sw != last_sw) { snprintf(v, sizeof v, "%.2f", sw); set(t, "swing", v); last_sw = sw; edit_pump = t + 0.15; }
        if (bite != last_bite) { snprintf(v, sizeof v, "%.2f", bite); set(t, "bite", v); last_bite = bite; edit_pump = t + 0.15; }

        const int n = total_frames - f < BLOCK ? total_frames - f : BLOCK;
        api->render_block(inst, out, n);
        for (int i = 0; i < n; i++) {
            const double b = (double)(f + i) / SR / spb;
            float x = (float)out[2 * i] / 32768.0f * 0.8f + 0.55f * drums(b, 1.0 / SR / spb);
            x = x > 1 ? 1 : (x < -1 ? -1 : x);
            pcm[f + i] = (int16_t)lrintf(x * 32000.0f);
        }
        if ((f / truth_every) != ((f + n) / truth_every) || f == 0) {
            static char tb[65536];
            const char *q = api->get_param(inst, "rview", tb, sizeof tb) > 0 ? strstr(tb, "\"p\":") : NULL;
            truth[ntruth++] = q ? atof(q + 4) : 0.0;
        }
        const double tn = (double)(f + n) / SR;
        if (tn >= next_pump || (edit_pump >= 0 && tn >= edit_pump)) {          /* the pump: a heartbeat, and soon after an edit */
            push_extras(tn);
            if (tn >= next_pump) next_pump = tn + 0.5;
            if (edit_pump >= 0 && tn >= edit_pump) edit_pump = -1;
        }
    }
    fputs("\n],\"truth_ms\":20,\"truth\":[", js);
    for (int i = 0; i < ntruth; i++) fprintf(js, "%s%.4f", i ? "," : "", truth[i]);
    fputs("]}\n", js);
    free(truth);
    fclose(js);

    /* the sound: mono, 16 bit */
    const uint32_t bytes = (uint32_t)total_frames * 2;
    uint8_t hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0 };
    uint32_t v32;
    v32 = 36 + bytes; memcpy(hdr + 4, &v32, 4);
    v32 = SR; memcpy(hdr + 24, &v32, 4);
    v32 = SR * 2; memcpy(hdr + 28, &v32, 4);
    hdr[32] = 2; hdr[34] = 16;
    memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &bytes, 4);
    fwrite(hdr, 1, 44, wav);
    fwrite(pcm, 2, (size_t)total_frames, wav);
    fclose(wav);
    free(pcm);
    api->destroy_instance(inst);
    fprintf(stderr, "remote_demo: %d frames, %.1f s\n", nframes_out, total);
    return 0;
}
