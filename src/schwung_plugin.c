/* Schwung plugin_api_v2 glue for TinyScratch (a turntable-scratch instrument for the Move). */
#include "schwung_plugin.h"
#include "scratch_engine.h"
#include "ts_alloc.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_BLOCK 128

/* Where samples live on a Move: next to the module, and Schwung's usual spot. */
static const char *const k_sample_dirs[] = {
    "/data/Schwung/modules/TinyScratch/samples",
    "/data/UserData/schwung/modules/sound_generators/tinyscratch/samples",
};

/* The clips, if there are any (tools/import_clips.py makes clips.bin): next to the module, and Schwung's usual spot. */
static const char *const k_clip_files[] = {
    "/data/Schwung/modules/TinyScratch/clips.bin",
    "/data/UserData/schwung/modules/sound_generators/tinyscratch/clips.bin",
};

static const char *const k_recipe_file = "/data/UserData/schwung/modules/sound_generators/tinyscratch/recipes.txt";

/* ------------------------------------------------------------------ */
/* Parameter table                                                     */
/* ------------------------------------------------------------------ */

enum { K_ENUM, K_FLOAT, K_INT, K_CANVAS, K_STRING, K_FILEPATH };
enum { P_SAMPLE, P_TURNTABLE, P_DRAG, P_GAIN, P_NOTE_GATE, P_VEL_VOL, P_NORMALISE,
       P_LOCK, P_CUT_FROM, P_PADS, P_CLIP, P_RECORD, P_CLEAR, P_GENERATE, P_GEN_STYLE, P_GEN_BARS, P_GEN_FWD, P_SWING, P_HUMANISE, P_BITE, P_GEN_FIN, P_GEN_ECHO, P_GEN_GROOVE, P_CLIP_DEPTH, P_CLIP_OFFSET, P_CUES, P_TASTE_LOG, P_CLIP_PAD, P_CLIP_DEL, P_ZOOM, P_PAGE, P_CLIP_PAGE, P_SAMPLE_FILE, P_VINYL,
       P_PAD_RAW, P_PAD_CUTS,
       P_VIEW, P_WAVE, P_FULL, P_STATUS,
       P_COUNT };

typedef struct {
    const char *key, *name;
    int kind;
    const char *const *opts;
    int nopts;
    float lo, hi, def, step;
    const char *extra;          /* raw JSON members for canvas / string entries */
} pdef_t;

static const char *const o_sample[] = { "Ahhh", "Fresssh", "User 1", "User 2", "User 3", "User 4", "User 5", "User 6" };
static const char *const o_onoff[] = { "Off", "On" };
static const char *const o_record[] = { "Idle", "Record" };
static const char *const o_clear[] = { "Idle", "Clear" };
static const char *const o_clip[] = { "Off", "Clip 1", "Clip 2", "Clip 3", "Clip 4", "Clip 5", "Clip 6", "Clip 7", "Clip 8", "Clip 9", "Clip 10",
                                      "Clip 11", "Clip 12", "Clip 13", "Clip 14", "Clip 15", "Clip 16", "Auto", "Try" };
static const char *const o_generate[] = { "Idle", "Generate" };
static const char *const o_genstyle[] = { "Improv", "Baby", "Cuts", "Military", "Scribble", "Stab", "Chirp", "Transform", "Flare", "Crab", "Orbit", "Drag", "Tear", "Stutter", "Impv 1.1", "Impv Mix" };
static const char *const o_genbars[] = { "1 bar", "2 bars", "4 bars" };

static const pdef_t k_params[P_COUNT] = {
    [P_SAMPLE]      = { "sample", "Sample", K_ENUM, o_sample, 8, 0, 7, 0, 0, NULL },
    [P_TURNTABLE]   = { "turntable", "Turntable", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.0f, 0.005f, NULL },
    [P_DRAG]        = { "drag", "Drag", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.5f, 0.01f, NULL },
    [P_GAIN]        = { "gain", "Gain", K_FLOAT, NULL, 0, -24.0f, 12.0f, 0.0f, 0.5f, NULL },
    [P_NOTE_GATE]   = { "note_gate", "Note Gate", K_ENUM, o_onoff, 2, 0, 1, 1, 0, NULL },
    [P_VEL_VOL]     = { "vel_to_vol", "Vel to Vol", K_ENUM, o_onoff, 2, 0, 1, 0, 0, NULL },
    [P_NORMALISE]   = { "normalise", "Normalise", K_ENUM, o_onoff, 2, 0, 1, 1, 0, NULL },
    [P_LOCK]        = { "lock", "Lock", K_ENUM, o_onoff, 2, 0, 1, 1, 0, NULL },
    [P_CUT_FROM]    = { "cut_from", "Cut From", K_INT, NULL, 0, 0, 127, 60, 1, NULL },
    [P_PADS]        = { "pads", "Pad Grid", K_ENUM, o_onoff, 2, 0, 1, 1, 0, NULL },
    [P_CLIP]        = { "clip", "Clip", K_ENUM, o_clip, 19, 0, 18, 17, 0, NULL },
    /* the generator: Style and Length are settings. Generate (a new clip, "Try", played at once), keeping it (tap an empty clip pad)
     * and deleting a clip you made (the Delete button and its pad) are all on the pads: generate, clip_pad and clip_delete are what
     * the Pads screen writes. */
    [P_GENERATE]    = { "generate", "Generate", K_ENUM, o_generate, 2, 0, 1, 0, 0, NULL },
    [P_GEN_STYLE]   = { "gen_style", "Style", K_ENUM, o_genstyle, 16, 0, 15, 0, 0, NULL },
    [P_GEN_BARS]    = { "gen_bars", "Length", K_ENUM, o_genbars, 3, 0, 2, 1, 0, NULL },
    [P_GEN_FWD]     = { "gen_fwd", "Forwards", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.0f, 0.05f, NULL },
    [P_SWING]       = { "swing", "Swing", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.30f, 0.02f, NULL },
    [P_HUMANISE]    = { "humanise", "Humanise", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.35f, 0.02f, NULL },
    [P_BITE]        = { "bite", "Bite", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.10f, 0.02f, NULL },
    [P_GEN_FIN]     = { "gen_fin", "Finish", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.10f, 0.05f, NULL },
    [P_GEN_ECHO]    = { "gen_echo", "Echo", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.10f, 0.05f, NULL },
    [P_GEN_GROOVE]  = { "gen_groove", "Groove", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.0f, 0.05f, NULL },
    [P_CLIP_DEPTH]  = { "clip_depth", "Clip Depth", K_FLOAT, NULL, 0, 0.0f, 2.0f, 1.0f, 0.02f, NULL },
    [P_CLIP_OFFSET] = { "clip_offset", "Clip Offset", K_FLOAT, NULL, 0, -0.5f, 0.5f, 0.0f, 0.01f, NULL },
    [P_VINYL]       = { "vinyl", "Vinyl", K_FLOAT, NULL, 0, 0.0f, 1.0f, 0.0f, 0.01f, NULL },
    [P_CUES]        = { "cues", "Cue Pads", K_ENUM, o_onoff, 2, 0, 1, 1, 0, NULL },    [P_CLIP_PAD]    = { "clip_pad", "Clip Pad", K_INT, NULL, 0, 0, 16, 0, 1, NULL },
    [P_TASTE_LOG]   = { "taste_log", "Taste Log", K_ENUM, o_onoff, 2, 0, 1, 1, 0, NULL },
    [P_CLIP_DEL]    = { "clip_delete", "Delete Clip", K_INT, NULL, 0, 0, 16, 0, 1, NULL },
    [P_ZOOM]        = { "zoom", "Zoom", K_INT, NULL, 0, 0, 3, 0, 1, NULL },
    /* clip pages: the Pads screen shows one page of 14 clip pads (page, written by the screen when < or > is pressed); clip_page is the page of the clip playing */
    [P_PAGE]        = { "page", "Page", K_INT, NULL, 0, 0, 3, 0, 1, NULL },
    [P_CLIP_PAGE]   = { "clip_page", "Clip Page", K_INT, NULL, 0, 0, 3, 0, 1, NULL },
    /* sampling: Record takes the audio input into the selected User slot (4 s at most, or until it is turned off; cropped to its sound), Clear empties it */
    [P_RECORD]      = { "record", "Record", K_ENUM, o_record, 2, 0, 1, 0, 0, NULL },
    [P_CLEAR]       = { "clear", "Clear Sample", K_ENUM, o_clear, 2, 0, 1, 0, 0, NULL },
    /* Load Sample: the Move's file browser, in the Move's own sample library (UserLibrary/Samples); the chosen file goes into the
     * selected User slot (from Ahhh or Fresssh, the first empty one) */
    [P_SAMPLE_FILE] = { "sample_file", "Load Sample", K_FILEPATH, NULL, 0, 0, 0, 0, 0,
                        "\"root\":\"/data/UserData/UserLibrary/Samples\",\"start_path\":\"/data/UserData/UserLibrary/Samples\",\"filter\":\".wav\",\"default\":\"\"" },
    /* written by the Pads screen, never saved: it is there (renewed), and which cut pads are held */
    [P_PAD_RAW]     = { "pad_raw", "Pads Screen", K_INT, NULL, 0, 0, 1, 0, 1, NULL },
    [P_PAD_CUTS]    = { "pad_cuts", "Pad Cuts", K_INT, NULL, 0, 0, 65535, 0, 1, NULL },
    /* the screen: waveform + playhead, vinyl and the clip view, as the first page of the module */
    [P_VIEW]        = { "scratcher", "Scratcher", K_CANVAS, NULL, 0, 0, 0, 0, 0,
                        "\"canvas_script\":\"scratcher.js\",\"as_page\":true,\"page_first\":true,\"show_value\":false,"
                        "\"extra_keys\":[\"view\"]" },
    /* the waveform on its own, the whole screen: a second page */
    [P_WAVE]        = { "waveform", "Waveform", K_CANVAS, NULL, 0, 0, 0, 0, 0,
                        "\"canvas_script\":\"waveform.js\",\"as_page\":true,\"show_value\":false,\"extra_keys\":[\"view\"]" },
    /* the same screen full size: it owns the pads, so it can colour them, and the knobs, which it passes on */
    [P_FULL]        = { "pads_screen", "Pads", K_CANVAS, NULL, 0, 0, 0, 0, 0,
                        "\"canvas_script\":\"scratcher.js\",\"show_value\":false,\"show_footer\":false,"
                        "\"fullscreen_live_ms\":50,\"extra_keys\":[\"view\"]" },
    /* ...and it names the Remote UI's three (rview, rclip, rpads): a key named only as an extra is what the host's extras pump sends to the browser,
     * twice a second and on every edit. "view" is on no page, so the Move never reads them; a declared parameter would be read every few seconds. */
    [P_STATUS]      = { "view", "View", K_STRING, NULL, 0, 0, 0, 0, 0, "\"access\":\"read\",\"live\":true,\"viz\":{\"extra_keys\":[\"rview\",\"rclip\",\"rpads\"]}" },
};

/* The pages, in the order the host shows them: the picture, then
 *   Main        the generator: the Pads screen (the first dial), Load Sample, and what Generate makes (Style, Length, Forwards, Finish, Echo, Groove)
 *   Playback    how clips play: Swing, Humanise, Clip Depth, Clip Offset, Drag, Gain
 *   Pad Play    how the pads play: Note Gate, Vel to Vol, Cue Pads; and Taste Log (a log of the clips you keep, pass over and delete, local to the Move)
 *   Waveform    the sample full size, always the last page
 * Choosing the sample, the clip and everything else about Generate is done on the pads, so none of it is a dial. The first dial opens
 * the Pads screen (and does nothing while you are on it). What the Pads screen writes (the raw pads, the cuts, the sample, the clip)
 * is set from the screen itself, which can set any parameter: it is not listed. (The Turntable has no dial: the clips move the playhead.
 * Recording and unloading a sample are on the pads. Lock, Normalise, Cut From and Pad Grid keep their defaults: they are saved with the
 * project and not on a dial.) */
static const int pg_root[] = { P_VIEW, P_FULL, P_SAMPLE_FILE, P_GEN_STYLE, P_GEN_BARS, P_GEN_FWD, P_GEN_FIN, P_GEN_ECHO, P_GEN_GROOVE };
static const int kn_root[] = { P_FULL, P_SAMPLE_FILE, P_GEN_STYLE, P_GEN_BARS, P_GEN_FWD, P_GEN_FIN, P_GEN_ECHO, P_GEN_GROOVE };
static const int pg_playback[] = { P_SWING, P_HUMANISE, P_BITE, P_CLIP_DEPTH, P_CLIP_OFFSET, P_DRAG, P_GAIN, P_VINYL };
static const int pg_padplay[] = { P_NOTE_GATE, P_VEL_VOL, P_CUES, P_TASTE_LOG };
static const int pg_wave[] = { P_WAVE };

/* a page is a level of the hierarchy; its first `nknobs` params are its dials (a page has eight at most; the waveform has none) */
typedef struct { const char *id, *label; const int *params; int nparams, nknobs; } page_t;
#define N_(a) (int)(sizeof(a) / sizeof((a)[0]))
static const page_t k_pages[] = {
    { "playback", "Playback", pg_playback, N_(pg_playback), N_(pg_playback) },
    { "pad_play", "Pad Play", pg_padplay, N_(pg_padplay), N_(pg_padplay) },
    { "wave", "Waveform", pg_wave, N_(pg_wave), 0 },
};

static float pget(const ts_params_t *p, int i)
{
    switch (i) {
    case P_SAMPLE: return (float)p->sample;
    case P_TURNTABLE: return p->turntable;
    case P_DRAG: return p->drag;
    case P_GAIN: return p->gain_db;
    case P_NOTE_GATE: return (float)p->note_gate;
    case P_VEL_VOL: return (float)p->vel_to_vol;
    case P_NORMALISE: return (float)p->normalise;
    case P_LOCK: return (float)p->lock;
    case P_CUT_FROM: return (float)p->cut_from;
    case P_PADS: return (float)p->pads;
    case P_CLIP: return (float)p->clip;
    case P_CLIP_PAGE: return (float)p->clip_page;
    case P_GEN_STYLE: return (float)p->gen_style;
    case P_GEN_BARS: return (float)p->gen_bars;
    case P_GEN_FWD: return p->gen_fwd;
    case P_SWING: return p->swing;
    case P_HUMANISE: return p->humanise;
    case P_BITE: return p->bite;
    case P_GEN_FIN: return p->gen_fin;
    case P_GEN_ECHO: return p->gen_echo;
    case P_GEN_GROOVE: return p->gen_groove;
    case P_CLIP_DEPTH: return p->clip_depth;
    case P_CLIP_OFFSET: return p->clip_offset;
    case P_VINYL: return p->vinyl;
    case P_CUES: return (float)p->cues;
    case P_TASTE_LOG: return (float)p->taste_log;    default: return 0.0f;
    }
}

static void pset(ts_params_t *p, int i, float v)
{
    if (!isfinite(v)) return;
    switch (i) {
    case P_SAMPLE: p->sample = (int)lrintf(v); break;
    case P_TURNTABLE: p->turntable = v; break;
    case P_DRAG: p->drag = v; break;
    case P_GAIN: p->gain_db = v; break;
    case P_NOTE_GATE: p->note_gate = v >= 0.5f; break;
    case P_VEL_VOL: p->vel_to_vol = v >= 0.5f; break;
    case P_NORMALISE: p->normalise = v >= 0.5f; break;
    case P_LOCK: p->lock = v >= 0.5f; break;
    case P_CUT_FROM: p->cut_from = (int)lrintf(v); break;
    case P_PADS: p->pads = v >= 0.5f; break;
    case P_CLIP: p->clip = (int)lrintf(v); break;
    case P_CLIP_PAGE: p->clip_page = (int)lrintf(v); break;
    case P_GEN_STYLE: p->gen_style = (int)lrintf(v); break;
    case P_GEN_BARS: p->gen_bars = (int)lrintf(v); break;
    case P_GEN_FWD: p->gen_fwd = v; break;
    case P_SWING: p->swing = v; break;
    case P_HUMANISE: p->humanise = v; break;
    case P_BITE: p->bite = v; break;
    case P_GEN_FIN: p->gen_fin = v; break;
    case P_GEN_ECHO: p->gen_echo = v; break;
    case P_GEN_GROOVE: p->gen_groove = v; break;
    case P_CLIP_DEPTH: p->clip_depth = v; break;
    case P_CLIP_OFFSET: p->clip_offset = v; break;
    case P_VINYL: p->vinyl = v; break;
    case P_CUES: p->cues = v >= 0.5f; break;
    case P_TASTE_LOG: p->taste_log = v >= 0.5f; break;    default: break;                                     /* the screen, the status string and the Save button are not values */
    }
}

/* ------------------------------------------------------------------ */
/* JSON (table-driven so module.json cannot drift from the code)       */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; int cap, len; } jb_t;

static void jb_put(jb_t *j, const char *fmt, ...)
{
    if (j->len < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, (size_t)(j->cap - j->len), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= j->cap - j->len) j->len = -1;
    else j->len += n;
}

static void jb_param(jb_t *j, const pdef_t *d)
{
    static const char *const types[] = { "enum", "float", "int", "canvas", "string", "filepath" };
    jb_put(j, "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\"", d->key, d->name, types[d->kind]);
    if (d->kind == K_CANVAS || d->kind == K_STRING || d->kind == K_FILEPATH) {
        if (d->extra) jb_put(j, ",%s", d->extra);
        jb_put(j, "}");
        return;
    }
    if (d->kind == K_ENUM) {
        jb_put(j, ",\"options\":[");
        for (int i = 0; i < d->nopts; i++) jb_put(j, "%s\"%s\"", i ? "," : "", d->opts[i]);
        jb_put(j, "]");
    } else {
        jb_put(j, ",\"min\":%g,\"max\":%g", (double)d->lo, (double)d->hi);
    }
    jb_put(j, ",\"default\":%g", (double)d->def);
    if (d->kind == K_FLOAT) jb_put(j, ",\"step\":%g", (double)d->step);
    jb_put(j, "}");
}

int ts_plugin_json_chain_params(char *buf, int cap)
{
    jb_t j = { buf, cap, 0 };
    if (!buf || cap < 2) return -1;
    jb_put(&j, "[");
    for (int i = 0; i < P_COUNT; i++) {
        if (i) jb_put(&j, ",");
        jb_param(&j, &k_params[i]);
    }
    jb_put(&j, "]");
    return j.len;
}

static void jb_keys(jb_t *j, const int *keys, int n)
{
    for (int k = 0; k < n; k++) jb_put(j, "%s\"%s\"", k ? "," : "", k_params[keys[k]].key);
}

int ts_plugin_json_ui_hierarchy(char *buf, int cap)
{
    jb_t j = { buf, cap, 0 };
    if (!buf || cap < 2) return -1;
    /* params are keys into chain_params (the way the other Schwung modules do it) */
    jb_put(&j, "{\"levels\":{\"root\":{\"label\":\"TinyScratch\",\"params\":[");
    jb_keys(&j, pg_root, N_(pg_root));
    for (int i = 0; i < N_(k_pages); i++) jb_put(&j, ",{\"level\":\"%s\",\"label\":\"%s\"}", k_pages[i].id, k_pages[i].label);
    jb_put(&j, "],\"knobs\":[");
    jb_keys(&j, kn_root, N_(kn_root));
    jb_put(&j, "]}");
    for (int i = 0; i < N_(k_pages); i++) {
        jb_put(&j, ",\"%s\":{\"label\":\"%s\",\"params\":[", k_pages[i].id, k_pages[i].label);
        jb_keys(&j, k_pages[i].params, k_pages[i].nparams);
        jb_put(&j, "],\"knobs\":[");
        jb_keys(&j, k_pages[i].params, k_pages[i].nknobs < 8 ? k_pages[i].nknobs : 8);          /* (a page has eight dials at most) */
        jb_put(&j, "]}");
    }
    jb_put(&j, "}}");
    return j.len;
}

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    ts_engine_t *eng;
    ts_params_t p;              /* the parameters as the host set them */
} ts_inst_t;

static const ts_host_api_t *g_host;

static void *ts_create_instance(const char *module_dir, const char *json_defaults)
{
    (void)json_defaults;                                 /* the host restores state through set_param */
    ts_inst_t *in = (ts_inst_t *)TS_CALLOC(sizeof(*in));
    if (!in) return NULL;
    float sr = (g_host && g_host->sample_rate > 0) ? (float)g_host->sample_rate : TS_SAMPLE_RATE;
    in->eng = ts_engine_create(sr);
    if (!in->eng) { TS_FREE(in); return NULL; }
    in->p = in->eng->p;

    char dir[512];
    if (module_dir && *module_dir) {
        snprintf(dir, sizeof dir, "%s/samples", module_dir);
        ts_engine_load_dir(in->eng, dir);
    }
    for (size_t i = 0; i < sizeof k_sample_dirs / sizeof k_sample_dirs[0]; i++) ts_engine_load_dir(in->eng, k_sample_dirs[i]);
    if (module_dir && *module_dir) { snprintf(dir, sizeof dir, "%s/samples", module_dir); ts_engine_set_save_dir(in->eng, dir); }      /* recordings are kept here */
    else ts_engine_set_save_dir(in->eng, k_sample_dirs[1]);

    int clips = TS_WAV_ERR_IO;
    if (module_dir && *module_dir) {
        snprintf(dir, sizeof dir, "%s/clips.bin", module_dir);
        clips = ts_engine_load_clips(in->eng, dir);
    }
    for (size_t i = 0; clips < 0 && i < sizeof k_clip_files / sizeof k_clip_files[0]; i++) clips = ts_engine_load_clips(in->eng, k_clip_files[i]);

    /* the clips you kept: a library in the module's folder, so every instance (every track) starts with them */
    if (module_dir && *module_dir) { snprintf(dir, sizeof dir, "%s/recipes.txt", module_dir); ts_engine_set_recipes_path(in->eng, dir); }
    else ts_engine_set_recipes_path(in->eng, k_recipe_file);
    ts_engine_load_recipes(in->eng);
    in->p = in->eng->p;
    return in;
}

static void ts_destroy_instance(void *h)
{
    ts_inst_t *in = (ts_inst_t *)h;
    if (!in) return;
    ts_engine_destroy(in->eng);
    TS_FREE(in);
}

static int find_param(const char *key)
{
    for (int i = 0; i < P_COUNT; i++)
        if (strcmp(key, k_params[i].key) == 0) return i;
    return -1;
}

/* ---- saved state ----
 * get_param("state") is what Schwung stores with a project or preset; set_param("state") gives it back. A flat object
 * of strings: every parameter by key (enums as their index). */

/* written by the screens or one-shots, never saved with the project */
static int is_transient(int i)
{
    return i == P_PAD_RAW || i == P_PAD_CUTS || i == P_RECORD || i == P_CLEAR || i == P_GENERATE ||
           i == P_CLIP_PAD || i == P_CLIP_DEL || i == P_ZOOM || i == P_PAGE;
}

static int build_state(const ts_inst_t *in, char *buf, int cap)
{
    jb_t j = { buf, cap, 0 };
    jb_put(&j, "{\"v\":\"1\"");
    for (int i = 0; i < P_COUNT; i++) {
        if (k_params[i].kind == K_CANVAS || k_params[i].kind == K_STRING || k_params[i].kind == K_FILEPATH || is_transient(i)) continue;
        float v = pget(&in->p, i);
        if (k_params[i].kind == K_FLOAT) jb_put(&j, ",\"%s\":\"%.4f\"", k_params[i].key, (double)v);
        else jb_put(&j, ",\"%s\":\"%d\"", k_params[i].key, (int)v);
    }
    jb_put(&j, "}");
    return j.len;
}

/* the string value of "key" in a flat object of strings; 0 if it is not there */
static int state_get(const char *json, const char *key, char *out, size_t cap)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    while (p && p != json && p[-1] != ',' && p[-1] != '{') p = strstr(p + 1, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return *p == '"';
}

static void restore_state(ts_inst_t *in, const char *json)
{
    size_t jl = strlen(json);
    while (jl && isspace((unsigned char)json[jl - 1])) jl--;
    if (jl < 2 || jl > 8192 || json[0] != '{' || json[jl - 1] != '}') return;      /* whole objects only */
    char val[1024];
    if (!state_get(json, "v", val, sizeof val) || strcmp(val, "1") != 0) return;   /* not ours, or newer */

    ts_params_t p = in->p;
    for (int i = 0; i < P_COUNT; i++) {
        if (k_params[i].kind == K_CANVAS || k_params[i].kind == K_STRING || k_params[i].kind == K_FILEPATH || is_transient(i)) continue;
        if (!state_get(json, k_params[i].key, val, sizeof val)) continue;
        char *end = NULL;
        float v = strtof(val, &end);
        if (end != val) pset(&p, i, v);
    }
    ts_engine_set_params(in->eng, &p);

    in->p = in->eng->p;
}

static void ts_set_param(void *h, const char *key, const char *val)
{
    ts_inst_t *in = (ts_inst_t *)h;
    if (!in || !key || !val) return;
    ts_engine_service(in->eng);                          /* finishes a recording, frees what the audio thread let go of */
    if (strcmp(key, "state") == 0) { restore_state(in, val); return; }
    if (strcmp(key, "scratch") == 0) {                   /* the Remote UI's hand on the record, "pos" or "pos ms" (when it was there): not a parameter */
        char *end = NULL;
        const float pos = strtof(val, &end);
        if (end == val) return;
        char *end2 = NULL;
        const double t = strtod(end, &end2);
        if (end2 != end) ts_engine_scratch_at(in->eng, pos, t); else ts_engine_scratch(in->eng, pos);
        return;
    }
    int i = find_param(key);
    if (i < 0) return;                                   /* (an unknown key changes nothing: the Remote UI's rui_pull is one) */
    const pdef_t *d = &k_params[i];
    if (i == P_SAMPLE_FILE) {                                                          /* the file browser's choice: a path */
        ts_engine_load_file(in->eng, val);
        in->p = in->eng->p;
        ts_engine_service(in->eng);
        return;
    }

    float v = 0.0f;
    int ok = 0;
    if (d->kind == K_ENUM) {
        for (int o = 0; o < d->nopts; o++)
            if (strcmp(val, d->opts[o]) == 0) { v = (float)o; ok = 1; break; }
        if (!ok && i == P_GEN_STYLE && strcmp(val, "Any") == 0) { v = 0.0f; ok = 1; }        /* (what Improv was called) */
    }
    if (!ok) {
        char *end = NULL;
        v = strtof(val, &end);
        ok = end != val;
    }
    if (!ok) return;

    if (i == P_RECORD) {                                                               /* the audio input into the selected User slot */
        if (v >= 0.5f) ts_engine_rec_start(in->eng); else ts_engine_rec_stop(in->eng);
        ts_engine_service(in->eng);
        in->p = in->eng->p;
        return;
    }
    if (i == P_CLEAR) {
        if (v >= 0.5f) ts_engine_clear_slot(in->eng, in->eng->p.sample);
        ts_engine_service(in->eng);
        return;
    }
    if (i == P_SAMPLE_FILE) return;                                                    /* (a path: handled before the number is read) */
    if (i == P_ZOOM) { ts_engine_set_zoom(in->eng, (int)lrintf(v)); return; }           /* the Pads screen's zoomed view */
    if (i == P_PAGE) { ts_engine_set_page(in->eng, (int)lrintf(v)); return; }           /* the page of clip pads the screen shows */
    if (i == P_GENERATE) {                                                            /* a new clip to try */
        if (v >= 0.5f) ts_engine_generate(in->eng);
        in->p = in->eng->p;
        return;
    }
    if (i == P_CLIP_PAD || i == P_CLIP_DEL) {
        int n = (int)lrintf(v);
        if (i == P_CLIP_PAD && n >= 1) ts_engine_clip_pad(in->eng, n);
        else if (i == P_CLIP_DEL && n >= 1) ts_engine_clear_clip(in->eng, n);
        in->p = in->eng->p;
        ts_engine_service(in->eng);                                                   /* writes the library of kept clips */
        return;
    }
    if (i == P_PAD_RAW) { ts_engine_pad_raw(in->eng, v >= 0.5f); return; }            /* the Pads screen, not settings */
    if (i == P_PAD_CUTS) { ts_engine_pad_cuts(in->eng, v >= 0.0f && v < 65536.0f ? (unsigned)v : 0u); return; }

    pset(&in->p, i, v);
    ts_engine_set_params(in->eng, &in->p);
    in->p = in->eng->p;                                  /* keep the sanitised values */
}

static int out_str(char *buf, int cap, const char *s)
{
    int n = (int)strlen(s);
    if (!buf || n >= cap) return -1;
    memcpy(buf, s, (size_t)n + 1);
    return n;
}

static int ts_get_param(void *h, const char *key, char *buf, int cap)
{
    ts_inst_t *in = (ts_inst_t *)h;
    if (!in || !key || !buf || cap <= 0) return -1;
    ts_engine_service(in->eng);                          /* the host polls the view: a recording that ran out is finished here */

    if (strcmp(key, "chain_params") == 0) return ts_plugin_json_chain_params(buf, cap);
    if (strcmp(key, "ui_hierarchy") == 0) return ts_plugin_json_ui_hierarchy(buf, cap);
    if (strcmp(key, "name") == 0) return out_str(buf, cap, "TinyScratch");
    if (strcmp(key, "view") == 0) return ts_engine_view(in->eng, buf, cap);
    if (strcmp(key, "rview") == 0) return ts_engine_rview(in->eng, buf, cap);     /* the Remote UI's (web_ui.html): see scratch_engine.h */
    if (strcmp(key, "rclip") == 0) return ts_engine_rclip(in->eng, buf, cap);
    if (strcmp(key, "rpads") == 0) return ts_engine_rpads(in->eng, buf, cap);
    if (strcmp(key, "state") == 0) {
        char st[2048];
        int n = build_state(in, st, sizeof st);
        return n < 0 ? -1 : out_str(buf, cap, st);
    }

    int i = find_param(key);
    if (i < 0) return -1;
    const pdef_t *d = &k_params[i];
    if (d->kind == K_CANVAS || d->kind == K_STRING || d->kind == K_FILEPATH) return out_str(buf, cap, "");
    float v = i == P_RECORD ? (float)(in->eng->rec_state == 1) : pget(&in->p, i);
    /* enums are reported as their index (the other modules' UIs parseInt it); set_param takes the index or the label */
    char s[32];
    if (d->kind == K_INT || d->kind == K_ENUM) snprintf(s, sizeof s, "%d", (int)v);
    else snprintf(s, sizeof s, "%.3f", (double)v);
    return out_str(buf, cap, s);
}

static int ts_get_error(void *h, char *buf, int cap)
{
    (void)h;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}

/* Notes at C3 and above cut the sound in while they are held. With Pad Grid on, the Move's pads (notes 36..99) are a
 * fixed grid of cuts, samples and clips instead (see scratch_engine.h). */
static void ts_on_midi(void *h, const uint8_t *msg, int len, int source)
{
    (void)source;
    ts_inst_t *in = (ts_inst_t *)h;
    if (!in || !msg || len < 2) return;
    int status = msg[0] & 0xF0;
    if (status == 0xB0 && len >= 3 && msg[1] == 123) {             /* all notes off */
        ts_engine_all_notes_off(in->eng);
        return;
    }
    if (len < 3) return;
    int on = status == 0x90 && msg[2] > 0;
    int off = status == 0x80 || (status == 0x90 && msg[2] == 0);
    if (!on && !off) return;
    ts_engine_note(in->eng, msg[1] & 0x7F, msg[2] & 0x7F, on);
    in->p = in->eng->p;                                            /* a pad changed the sample or the clip */
}

static void ts_render_block(void *h, int16_t *out, int frames)
{
    ts_inst_t *in = (ts_inst_t *)h;
    if (!in || !out || frames <= 0) return;

    if (g_host) {
        float bpm = g_host->get_bpm ? g_host->get_bpm() : 0.0f;
        if (bpm >= 20.0f && bpm <= 300.0f) ts_engine_set_tempo(in->eng, bpm);
        double beat = -1.0;
        if (g_host->get_clock_status && g_host->get_beat_position && g_host->get_clock_status() == TS_CLOCK_RUNNING)
            beat = g_host->get_beat_position();
        ts_engine_sync_beat(in->eng, beat);
    }

    float tmp[TS_BLOCK];
    int left = frames;
    int16_t *o = out;
    /* while recording, the Move's audio input (stereo int16 in the shared memory, like the output) is fed in, mixed to mono */
    const int16_t *src = NULL;
    if (in->eng->rec_state == 1 && g_host && g_host->mapped_memory && g_host->audio_in_offset > 0 &&
        frames <= (g_host->frames_per_block > 0 ? g_host->frames_per_block : TS_BLOCK))
        src = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    while (left > 0) {
        int n = left < TS_BLOCK ? left : TS_BLOCK;
        if (src) {
            float inb[TS_BLOCK];
            int base = frames - left;
            for (int i = 0; i < n; i++) inb[i] = 0.5f * ((float)src[2 * (base + i)] + (float)src[2 * (base + i) + 1]) * (1.0f / 32768.0f);
            ts_engine_feed_input(in->eng, inb, n);
        }
        ts_engine_render(in->eng, tmp, n);
        for (int i = 0; i < n; i++) {
            float x = tmp[i] * 32767.0f;
            if (!isfinite(x)) x = 0.0f;
            else if (x > 32767.0f) x = 32767.0f;
            else if (x < -32768.0f) x = -32768.0f;
            int16_t s = (int16_t)lrintf(x);
            o[2 * i] = s;
            o[2 * i + 1] = s;
        }
        o += 2 * n;
        left -= n;
    }
}

static const ts_plugin_api_v2_t g_api = {
    TS_PLUGIN_API_V2,
    ts_create_instance,
    ts_destroy_instance,
    ts_on_midi,
    ts_set_param,
    ts_get_param,
    ts_get_error,
    ts_render_block,
};

const ts_plugin_api_v2_t *move_plugin_init_v2(const ts_host_api_t *host)
{
    g_host = host;
    return &g_api;
}
