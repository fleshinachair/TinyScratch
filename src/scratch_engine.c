#include "scratch_engine.h"
#include "ts_alloc.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float x, float lo, float hi) { return !(x >= lo) ? lo : (x > hi ? hi : x); }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ------------------------------------------------------------------ */
/* WAV decoding (16-bit PCM, mono or stereo, mixed down to mono)        */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

int ts_wav_decode(const uint8_t *d, size_t n, float *dst, int cap, float dst_sr)
{
    /* dst == NULL is a dry run: validate and return the frame count only */
    if (!d || cap <= 0 || !(dst_sr > 0.0f)) return TS_WAV_ERR_FORMAT;
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
        return TS_WAV_ERR_FORMAT;

    int have_fmt = 0, have_data = 0;
    uint32_t tag = 0, ch = 0, sr = 0, bits = 0;
    size_t data_off = 0, data_len = 0;
    size_t pos = 12;

    while (pos + 8 <= n) {
        uint32_t sz = rd32(d + pos + 4);
        size_t body = pos + 8;
        size_t avail = n - body;
        if (memcmp(d + pos, "fmt ", 4) == 0) {
            if (sz < 16 || sz > avail) return TS_WAV_ERR_FORMAT;
            tag = rd16(d + body);
            ch = rd16(d + body + 2);
            sr = rd32(d + body + 4);
            bits = rd16(d + body + 14);
            have_fmt = 1;
        } else if (memcmp(d + pos, "data", 4) == 0) {
            data_off = body;
            data_len = sz > avail ? avail : sz;   /* tolerate truncated files */
            have_data = 1;
        }
        if (sz > avail) break;
        size_t adv = (size_t)sz + (sz & 1u);
        if (adv > avail) break;
        pos = body + adv;
    }

    if (!have_fmt || !have_data) return TS_WAV_ERR_FORMAT;
    if (tag != 1 || (ch != 1 && ch != 2) || bits != 16) return TS_WAV_ERR_UNSUPPORTED;
    if (sr < 4000 || sr > 192000) return TS_WAV_ERR_UNSUPPORTED;

    size_t nin = data_len / (2 * (size_t)ch);                       /* frames */
    if (nin < 64) return TS_WAV_ERR_SHORT;

    double ratio = (double)dst_sr / (double)sr;
    double want = (double)nin * ratio;
    int nout = want > (double)cap ? cap : (int)want;
    if (nout < 64) return TS_WAV_ERR_SHORT;

    if (!dst) return nout;
    const uint8_t *pcm = d + data_off;
#define S16(i) (ch == 1 ? (float)(int16_t)rd16(pcm + 2 * (size_t)(i)) * (1.0f / 32768.0f) :                 0.5f * ((float)(int16_t)rd16(pcm + 4 * (size_t)(i)) + (float)(int16_t)rd16(pcm + 4 * (size_t)(i) + 2)) * (1.0f / 32768.0f))
    for (int j = 0; j < nout; j++) {
        double sp = (double)j / ratio;
        size_t i0 = (size_t)sp;
        if (i0 >= nin) i0 = nin - 1;
        size_t i1 = i0 + 1 < nin ? i0 + 1 : i0;
        float f = (float)(sp - (double)i0);
        float a = S16(i0), b = S16(i1);
        if (dst) dst[j] = a + (b - a) * f;
    }
#undef S16
    return nout;
}

/* ------------------------------------------------------------------ */
/* Clips                                                               */
/* ------------------------------------------------------------------ */

/* The built-in clips (used when there is no clips.bin) are each one bar: the playhead's path as breakpoints over 128 units, each saying how it gets to the next (a smooth
 * ease, a straight line, a quick rise that settles, or a slow start that finishes quickly), and a pattern of cuts over
 * 16 sixteenths. The paths are classic scratch shapes: a stroke forwards and back, a
 * slow sweep, a chirp, a flare in jerks, a full orbit, a ramp that restarts, three strokes to the bar (so the sixteenths
 * play across the strokes in a different rhythm), and a backwards one. */
typedef struct { uint8_t t, v, shape; } clip_pt_t;      /* units, 0..100, how it gets to the next point */
enum { SH_EASE, SH_LIN, SH_FAST, SH_SLOW, SH_FLICK };
#define CLIP_MAX_PTS 16
#define TS_BUILTIN_CLIPS 8
#define TS_CLIP_UNITS 128                       /* the built-in clips: 128 units to the bar */

static const struct { clip_pt_t pt[CLIP_MAX_PTS]; int n; unsigned cuts; } k_clip[TS_BUILTIN_CLIPS] = {
    /* 1 baby: a stroke to every beat, the sound in for the stroke forwards and the start of the way back */
    { { {0, 6, SH_EASE}, {32, 58, SH_EASE}, {64, 6, SH_EASE}, {96, 58, SH_EASE}, {128, 6, SH_LIN} }, 5, 0xF8F8u },
    /* 2 one slow stroke up and back, cut in sixteenths with a gap on each beat but the first */
    { { {0, 5, SH_EASE}, {64, 72, SH_EASE}, {128, 5, SH_LIN} }, 3, 0xDDDDu },
    /* 3 chirps: a quick rise that settles, four to the bar, the sound in for the rise and the first of the fall */
    { { {0, 8, SH_FAST}, {11, 56, SH_SLOW}, {32, 8, SH_FAST}, {43, 56, SH_SLOW}, {64, 8, SH_FAST}, {75, 56, SH_SLOW}, {96, 8, SH_FAST}, {107, 56, SH_SLOW}, {128, 8, SH_LIN} }, 9, 0xEEEEu },
    /* 4 flares: a stroke up in jerks, twice */
    { { {0, 5, SH_EASE}, {12, 30, SH_EASE}, {18, 22, SH_EASE}, {30, 52, SH_EASE}, {38, 40, SH_EASE}, {50, 66, SH_LIN}, {64, 5, SH_EASE},
        {76, 28, SH_EASE}, {82, 20, SH_EASE}, {94, 46, SH_EASE}, {102, 36, SH_EASE}, {114, 58, SH_LIN}, {128, 5, SH_LIN} }, 13, 0xBBBBu },
    /* 5 an orbit: round once to the bar, a smooth wave */
    { { {0, 50, SH_EASE}, {32, 90, SH_EASE}, {64, 50, SH_EASE}, {96, 10, SH_EASE}, {128, 50, SH_LIN} }, 5, 0xFBF7u },
    /* 6 a transform: the record slides slowly forwards and the sound is chopped in sixteenths */
    { { {0, 6, SH_LIN}, {116, 84, SH_LIN}, {128, 6, SH_EASE} }, 3, 0xB5B5u },
    /* 7 three strokes to the bar, with the sixteenths grouped in threes across them */
    { { {0, 6, SH_EASE}, {21, 62, SH_EASE}, {43, 6, SH_EASE}, {64, 62, SH_EASE}, {85, 6, SH_EASE}, {107, 62, SH_EASE}, {128, 6, SH_LIN} }, 7, 0xDB6Du },
    /* 8 backwards: it starts high, drops quickly, comes up and slides down again */
    { { {0, 80, SH_EASE}, {24, 12, SH_FAST}, {64, 62, SH_EASE}, {100, 6, SH_SLOW}, {128, 80, SH_LIN} }, 5, 0xBEFAu },
};

static float builtin_pos(int clip, double units)
{
    if (clip < 1 || clip > TS_BUILTIN_CLIPS || !isfinite(units)) return 0.0f;
    units -= floor(units / TS_CLIP_UNITS) * TS_CLIP_UNITS;
    int c = clip - 1, n = k_clip[c].n;
    int i = 0;
    while (i + 2 < n && units >= k_clip[c].pt[i + 1].t) i++;
    const clip_pt_t *a = &k_clip[c].pt[i], *b = &k_clip[c].pt[i + 1 < n ? i + 1 : i];
    double span = (double)b->t - (double)a->t;
    double u = span > 0.0 ? (units - (double)a->t) / span : 0.0;
    u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
    switch (a->shape) {
    case SH_EASE: u = u * u * (3.0 - 2.0 * u); break;
    case SH_FAST: u = 1.0 - pow(1.0 - u, 2.4); break;
    case SH_SLOW: u = pow(u, 2.4); break;
    default: break;
    }
    return (float)(((double)a->v + ((double)b->v - (double)a->v) * u) * 0.01);
}

static int builtin_cut(int clip, double units)
{
    if (clip < 1 || clip > TS_BUILTIN_CLIPS || !isfinite(units)) return 0;
    units -= floor(units / TS_CLIP_UNITS) * TS_CLIP_UNITS;
    int step = clampi((int)(units / 8.0), 0, 15);
    return (k_clip[clip - 1].cuts >> (15 - step)) & 1u;
}

/* Each sample has its own bank of 16 clip pads; a pad holds a clip from the pool, or nothing. */
static const ts_clip_t *pool_clip(const ts_engine_t *e, int idx)
{
    return idx >= 0 && idx < TS_POOL && e->clip_data[idx].n > 0 ? &e->clip_data[idx] : NULL;
}

static const ts_clip_t *bank_clip(const ts_engine_t *e, int sample, int pad)
{
    if (!e || sample < 0 || sample >= TS_SLOTS || pad < 1 || pad > TS_NUM_CLIPS) return NULL;
    return pool_clip(e, e->bank[sample][pad - 1] - 1);
}

static void recount_clips(ts_engine_t *e)
{
    int n = 0;
    for (int i = 1; i <= TS_NUM_CLIPS; i++) n += bank_clip(e, e->p.sample, i) != NULL;
    e->nclips = n;
    e->clips_ver++;
    e->auto_bar = -1;
}

static void clips_builtin(ts_engine_t *e)
{
    memset(e->clip_data, 0, sizeof e->clip_data);
    memset(e->bank, 0, sizeof e->bank);
    memset(e->kept, 0, sizeof e->kept);
    for (int c = 0; c < TS_BUILTIN_CLIPS; c++) {
        ts_clip_t *k = &e->clip_data[c];
        k->len = TS_LANE_BEATS;
        k->tag = 0;
        k->n = k->len * TS_CLIP_RES;
        for (int i = 0; i < k->n; i++) {
            double units = (double)i * (double)TS_CLIP_UNITS / (double)k->n;
            k->pos[i] = (uint16_t)(builtin_pos(c + 1, units) * 65535.0f + 0.5f);
            k->gate[i] = (uint8_t)builtin_cut(c + 1, units);
        }
    }
    for (int s = 0; s < 2; s++) for (int c = 0; c < TS_BUILTIN_CLIPS; c++) e->bank[s][c] = c + 1;      /* Ahhh and Fresssh have them; a User sample starts with empty pads */
    recount_clips(e);
}

static const ts_clip_t *clip_at(const ts_engine_t *e, int clip)
{
    if (!e) return NULL;
    if (clip == TS_CLIP_TRY) return e->try_valid && e->clip_data[e->try_idx].n > 0 ? &e->clip_data[e->try_idx] : NULL;
    return bank_clip(e, e->p.sample, clip);
}

float ts_clip_pos(const ts_engine_t *e, int clip, double beat)
{
    const ts_clip_t *k = clip_at(e, clip);
    if (!k || !isfinite(beat)) return 0.0f;
    double x = beat - floor(beat / k->len) * k->len;
    double f = x * TS_CLIP_RES;
    int i0 = clampi((int)f, 0, k->n - 1), i1 = (i0 + 1) % k->n;
    double u = f - floor(f);
    return (float)(((double)k->pos[i0] + ((double)k->pos[i1] - (double)k->pos[i0]) * u) / 65535.0);
}

int ts_clip_cut(const ts_engine_t *e, int clip, double beat)
{
    const ts_clip_t *k = clip_at(e, clip);
    if (!k || !isfinite(beat)) return 0;
    double x = beat - floor(beat / k->len) * k->len;
    return k->gate[clampi((int)(x * TS_CLIP_RES), 0, k->n - 1)];
}

static int sample_tag(int slot) { return slot == 0 ? 1 : (slot == 1 ? 2 : 0); }
#define TAG_GENERATED 3                                     /* a clip generated for the sample it is on: its positions are the sample's own */

/* The clips auto may play: those on the selected sample's pads. */
static int auto_candidates(const ts_engine_t *e, int out[TS_NUM_CLIPS])
{
    int k = 0;
    for (int c = 1; c <= TS_NUM_CLIPS; c++) if (bank_clip(e, e->p.sample, c)) out[k++] = c;
    return k;
}

/* The example set's clips switch with a Follow Action: a random OTHER clip when the bar is up. Here the song's bars are
 * dealt out in hands of as many bars as there are clips to choose from: each hand is a shuffle of all of them, and a hand
 * never starts with the clip the last one ended on. It depends on nothing but the bar number (and the sample), so a
 * transport that jumps lands on the right clip. */
static unsigned mix32(unsigned x)
{
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

static void hand_of(long long hand, int k, int out[TS_NUM_CLIPS])
{
    for (int i = 0; i < k; i++) out[i] = i;
    unsigned seed = mix32((unsigned)(hand & 0xFFFFFFFF) ^ mix32((unsigned)(hand >> 32)) ^ mix32((unsigned)k * 7919u));
    for (int i = k - 1; i > 0; i--) {
        seed = mix32(seed);
        int j = (int)(seed % (unsigned)(i + 1)), t = out[i];
        out[i] = out[j];
        out[j] = t;
    }
}

int ts_clip_auto(const ts_engine_t *e, long long bar)
{
    if (!e) return 0;
    int cand[TS_NUM_CLIPS], k = auto_candidates(e, cand);
    if (k < 1) return 0;
    if (k == 1) return cand[0];
    if (k == 2) return cand[(int)(((bar % 2) + 2) % 2)];            /* only two: the other one, every bar */
    long long hand = bar >= 0 ? bar / k : -((-bar + k - 1) / k);
    int idx = (int)(bar - hand * k);
    int h[TS_NUM_CLIPS], prev[TS_NUM_CLIPS];
    hand_of(hand, k, h);
    if (idx <= 1) {
        hand_of(hand - 1, k, prev);
        if (h[0] == prev[k - 1]) {
            if (idx == 0) { int t = h[0]; h[0] = h[1]; h[1] = t; }
            else return cand[h[0]];                                     /* (the swap moved the second one to the first place) */
        }
    }
    return cand[h[idx]];
}

int ts_engine_load_clips(ts_engine_t *e, const char *path)
{
    if (!e || !path) return TS_WAV_ERR_IO;
    FILE *f = fopen(path, "rb");
    if (!f) return TS_WAV_ERR_IO;
    ts_clip_t *tmp = (ts_clip_t *)TS_CALLOC(sizeof(ts_clip_t) * TS_NUM_CLIPS);
    if (!tmp) { fclose(f); return TS_WAV_ERR_IO; }
    uint8_t head[6];
    int count = 0, err = 0;
    if (fread(head, 1, 6, f) != 6 || memcmp(head, "TSCL", 4) != 0 || head[4] != 1 || head[5] < 1 || head[5] > TS_NUM_CLIPS) err = 1;
    else count = head[5];
    for (int c = 0; c < count && !err; c++) {
        uint8_t h[4];
        if (fread(h, 1, 4, f) != 4) { err = 1; break; }
        int tag = h[0], len = h[1], n = h[2] | (h[3] << 8);
        if (tag > 2 || len < 4 || len > TS_CLIP_MAX_BEATS || n != len * TS_CLIP_RES) { err = 1; break; }
        uint8_t raw[TS_CLIP_MAX_BEATS * TS_CLIP_RES * 2];
        ts_clip_t *k = &tmp[c];
        if (fread(raw, 1, (size_t)n * 2, f) != (size_t)n * 2 || fread(k->gate, 1, (size_t)n, f) != (size_t)n) { err = 1; break; }
        for (int i = 0; i < n; i++) { k->pos[i] = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)); k->gate[i] &= 1; }
        k->len = len; k->tag = tag; k->n = n;
    }
    fclose(f);
    if (err) { TS_FREE(tmp); return TS_WAV_ERR_FORMAT; }
    memset(e->clip_data, 0, sizeof(ts_clip_t) * TS_POOL);
    memcpy(e->clip_data, tmp, sizeof(ts_clip_t) * (size_t)count);
    memset(e->ro, 0, sizeof e->ro);
    for (int c = 0; c < count; c++) e->ro[c] = 1;                                 /* these are somebody's work: they can be played, not changed or deleted */
    memset(e->bank, 0, sizeof e->bank);
    memset(e->kept, 0, sizeof e->kept);                                          /* (the ones you kept come from the library afterwards) */
    int next[TS_SLOTS] = { 0 };
    for (int c = 0; c < count; c++)                       /* each clip goes on the pads of the sample it was made for; one for anything goes on all of them */
        for (int s = 0; s < TS_SLOTS; s++)
            if ((tmp[c].tag == 0 || tmp[c].tag == sample_tag(s)) && next[s] < TS_NUM_CLIPS) e->bank[s][next[s]++] = c + 1;
    recount_clips(e);
    TS_FREE(tmp);
    return count;
}

/* ------------------------------------------------------------------ */
/* The generator                                                       */
/* ------------------------------------------------------------------ */

/* Each style writes one bar (16 sixteenths, the last left for getting back to the start) into the clip's dense data: the
 * playhead's path with its strokes, and the gate, which is open for the sound and closed for a cut or a silent pull-back.
 * Strokes are on the sixteenth grid with a little hand jitter. Every bar ends back at the clip's base position, so the
 * clip loops, and a bar of the same style is never quite the same twice. Positions are 0..1 of the sample's sound
 * (a generated clip is for any sample: it is spread over where the sample has its sound). */
enum { GS_ANY, GS_BABY, GS_CUTS, GS_MILITARY, GS_SCRIBBLE, GS_STAB, GS_CHIRP, GS_TRANSFORM, GS_FLARE, GS_CRAB, GS_ORBIT, GS_DRAG, GS_COUNT };
#define GP (TS_CLIP_RES / 4)                         /* dense points per sixteenth */

typedef struct { ts_clip_t *c; uint32_t rng; float amp_t; int shape; int bars; int fw16; int nfw; float fw_t[16], fw_d[16]; int fw_kind[16]; } gen_t;          /* amp_t: how tall the strokes are, 0..1, the same all through the clip */

/* The curve a stroke takes. The hand does not move like a sine: it bites in and settles (a fast start: 60-90% of the strokes in the
 * clips measured), now and then a plain S-curve, and rarely builds slowly. One is the clip's own (g->shape) and most strokes use it. */
static int g_shape(gen_t *g);

static float grnd(gen_t *g)
{
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}
static float gr(gen_t *g, float a, float b) { return a + (b - a) * grnd(g); }
static int gi(gen_t *g, int a, int b)
{
    int n = b - a + 1, k = (int)(grnd(g) * (float)n);
    return a + (k >= n ? n - 1 : k);
}
static int g_shape(gen_t *g) { return gi(g, 0, 9) < 8 ? g->shape : SH_EASE; }

/* The top of a stroke: the clip's own height (the same all the way through, so it holds together) with a hair of variation. */
static float g_top(gen_t *g, float base, float lo, float hi, float cl_lo, float cl_hi)
{
    return clampf(base + 1.15f * (lo + (hi - lo) * g->amp_t) * gr(g, 0.96f, 1.0f), cl_lo, cl_hi);        /* (1.15: the example clips' strokes average 0.47 of the sample) */
}

static float g_ease(int shape, float u)
{
    u = clampf(u, 0.0f, 1.0f);
    switch (shape) {
    case SH_EASE: return u * u * (3.0f - 2.0f * u);
    case SH_FAST: return 1.0f - powf(1.0f - u, 2.4f);
    case SH_SLOW: return powf(u, 2.4f);
    case SH_FLICK: return logf(1.0f + 6.0f * u) / 1.9459101f;                   /* speed 1/(1+6u): the hand's push, fastest at the start and never quite stopping */
    default: return u;
    }
}

static void g_seg(gen_t *g, float t0, float t1, float v0, float v1, int shape)       /* times in sixteenths from the clip's start */
{
    int a = (int)(t0 * (float)GP + 0.5f), b = (int)(t1 * (float)GP + 0.5f);
    if (b <= a) b = a + 1;
    for (int i = a < 0 ? 0 : a; i < b && i < g->c->n; i++) {
        float u = ((float)(i - a) + 0.5f) / (float)(b - a);
        g->c->pos[i] = (uint16_t)(clampf(v0 + (v1 - v0) * g_ease(shape, u), 0.0f, 1.0f) * 65535.0f + 0.5f);
    }
}

static void g_gate(gen_t *g, float t0, float t1, int on)
{
    int a = (int)(t0 * (float)GP + 0.5f), b = (int)(t1 * (float)GP + 0.5f);
    if (b <= a) b = a + 1;
    for (int i = a < 0 ? 0 : a; i < b && i < g->c->n; i++) g->c->gate[i] = (uint8_t)on;
}

static void g_click(gen_t *g, float t) { g_gate(g, t - 0.12f, t + 0.12f, 0); }          /* a very short cut */

/* A Euclidean rhythm: k hits spread as evenly as they can be over n steps (the idea behind many pattern generators), turned
 * round by a rotation. Hits are what makes a stab pattern or a chop sound like it was meant. */
static int euclid_hit(int i, int k, int n, int rot) { return n > 0 && ((((i + rot) % n) * k) % n) < k; }

/* What a hand does that a drawn path does not (measured on a real performance and on the example clips): the strokes of a pair are rarely
 * the same length (about 30% are) and rarely start and end on the same spot (25 to 45%), and the path drifts. So every turning point of the
 * bar's path is moved: its height from the base gets a gain of 0.8 to 1.45 and the whole path wanders a little, each stroke being re-fitted
 * between its new ends so it keeps its own shape (a warp over time would bend the strokes' speed). The bar's ends stay put, so the clip
 * still loops. Everything is seeded with the bar. */
static void g_warp(gen_t *g, float o, float base)
{
    int a = (int)(o * (float)GP), b = (int)((o + 16.0f) * (float)GP);
    if (b > g->c->n) b = g->c->n;
    int ex[64], n = 0, dir = 0;
    ex[n++] = a;
    float ext = (float)g->c->pos[a] / 65535.0f;
    int exti = a;
    for (int i = a + 1; i < b; i++) {
        float v = (float)g->c->pos[i] / 65535.0f;
        if (dir == 0) { if (fabsf(v - ext) > 0.03f) { dir = v > ext ? 1 : -1; ext = v; exti = i; } continue; }
        if ((dir == 1 && v > ext) || (dir == -1 && v < ext)) { ext = v; exti = i; continue; }
        if (fabsf(v - ext) > 0.03f) { if (n < 62 && exti > ex[n - 1] + 2) ex[n++] = exti; dir = -dir; ext = v; exti = i; }
    }
    ex[n++] = b - 1;
    float old_v[64], new_v[64], drift = 0.0f;
    for (int k = 0; k < n; k++) old_v[k] = (float)g->c->pos[ex[k]] / 65535.0f;
    for (int k = 0; k < n; k++) {
        if (k == 0 || k == n - 1) { new_v[k] = old_v[k]; continue; }
        drift = clampf(drift + gr(g, -0.045f, 0.045f), -0.12f, 0.12f);
        new_v[k] = clampf(base + (old_v[k] - base) * gr(g, 0.8f, 1.45f) + drift, 0.0f, 0.97f);
    }
    uint16_t orig[16 * GP];                                                      /* (a turning point belongs to two strokes: both read the path as it was) */
    for (int i = a; i < b; i++) orig[i - a] = g->c->pos[i];
    for (int k = 0; k + 1 < n; k++) {
        float d = old_v[k + 1] - old_v[k];
        if (fabsf(d) < 1e-4f) continue;                                          /* (a flat stretch stays flat) */
        for (int i = ex[k]; i <= ex[k + 1] && i < b; i++) {
            float u = ((float)orig[i - a] / 65535.0f - old_v[k]) / d;
            g->c->pos[i] = (uint16_t)(clampf(new_v[k] + u * (new_v[k + 1] - new_v[k]), 0.0f, 0.97f) * 65535.0f + 0.5f);
        }
    }
}

/* The fader hides the turn. Of the record's changes of direction 70 to 90% are silenced in a real performance (99% in the example clips):
 * the fader shuts a little before the turn and opens a little after it, so what you hear is the strokes, not the grunt of the reversal.
 * A scribble and a flare are the places where the turns are heard, so those keep more of them. */
static void g_mask_turns(gen_t *g, float o, int style)
{
    float keep = style == GS_SCRIBBLE ? 0.65f : (style == GS_FLARE ? 0.4f : (style == GS_ORBIT ? 0.2f : 0.07f));      /* the share of turns left audible */
    int a = (int)(o * (float)GP), b = (int)((o + 16.0f) * (float)GP);
    if (b > g->c->n) b = g->c->n;
    int dir = 0, exti = a;
    float ext = (float)g->c->pos[a] / 65535.0f;
    for (int i = a + 1; i < b; i++) {
        float v = (float)g->c->pos[i] / 65535.0f;
        if (dir == 0) { if (fabsf(v - ext) > 0.03f) { dir = v > ext ? 1 : -1; ext = v; exti = i; } continue; }
        if ((dir == 1 && v > ext) || (dir == -1 && v < ext)) { ext = v; exti = i; continue; }
        if (fabsf(v - ext) > 0.03f) {                                           /* a turn at exti */
            if (grnd(g) >= keep) {
                int w = (int)(gr(g, 0.15f, 0.30f) * (float)GP);
                for (int j = exti - w; j <= exti + w; j++) if (j >= a && j < b) g->c->gate[j] = 0;
            }
            dir = -dir; ext = v; exti = i;
        }
    }
}

/* Nobody leaves the fader open for long: the longest opening in the real performance is under half a second, and about half the time is
 * silence. A run of sound longer than four sixteenths gets a short cut or two, placed by the seed. */
static void g_limit_open(gen_t *g, float o)
{
    int a = (int)(o * (float)GP), b = (int)((o + 16.0f) * (float)GP), run = 0;
    if (b > g->c->n) b = g->c->n;
    const int limit = 4 * GP;
    for (int i = a; i < b; i++) {
        if (!g->c->gate[i]) { run = 0; continue; }
        if (++run > limit - (int)(gr(g, 0.0f, 0.8f) * (float)GP)) {
            int w = (int)(gr(g, 0.18f, 0.34f) * (float)GP);
            for (int j = i; j < i + w && j < b; j++) g->c->gate[j] = 0;
            run = 0;
        }
    }
}

/* The fader plays notes on the sixteenth grid. In the example clips 57% of the openings fall in the first quarter of a sixteenth and a sound
 * lasts about half a sixteenth: staccato sixteenths, cut in on the beat, while the turns (one sixteenth or so earlier) are hidden. That is
 * the bite and the rhythm. The record turns a little before the grid (the path is moved earlier as a whole) and the fader opens on the grid,
 * so every sound starts with the record already moving fast. Which steps sound depends on which way the record is going (a push is
 * almost always heard; a pull often is not), on the style, and on the seed; now and then a note is held over the next step (a swoop). */
typedef struct { float push, pull, len_lo, len_hi, tie; } grid_t;
static const grid_t k_grid[GS_COUNT] = {
    [GS_ANY]      = { 0.95f, 0.72f, 0.22f, 0.72f, 0.12f },
    [GS_BABY]     = { 0.95f, 0.85f, 0.28f, 0.55f, 0.15f },
    [GS_CUTS]     = { 0.95f, 0.00f, 0.30f, 0.55f, 0.10f },
    [GS_MILITARY] = { 0.92f, 0.50f, 0.28f, 0.52f, 0.12f },
    [GS_SCRIBBLE] = { 0.95f, 0.95f, 0.55f, 0.85f, 0.30f },
    [GS_FLARE]    = { 0.95f, 0.85f, 0.40f, 0.65f, 0.10f },
    [GS_ORBIT]    = { 0.95f, 0.85f, 0.40f, 0.65f, 0.15f },
    [GS_DRAG]     = { 0.92f, 0.80f, 0.35f, 0.65f, 0.20f },
};
static int grid_style(int s) { return k_grid[s].push > 0.0f; }

static void g_grid_gate(gen_t *g, float o, int style)
{
    const grid_t *k = &k_grid[style];
    int a = (int)(o * (float)GP), b = (int)((o + 16.0f) * (float)GP);
    if (b > g->c->n) b = g->c->n;
    for (int i = a; i < b; i++) g->c->gate[i] = 0;
    for (int st = 0; st < 16; st++) {
        float t = o + (float)st;
        int i0 = (int)((t + 0.05f) * (float)GP), i1 = (int)((t + 0.45f) * (float)GP);
        if (i1 >= g->c->n) break;
        int d = (int)g->c->pos[i1] - (int)g->c->pos[i0];
        if (d > -300 && d < 300) continue;                                       /* the record is (nearly) still: nothing to hear */
        if (grnd(g) >= (d > 0 ? k->push : k->pull)) continue;
        float len = gr(g, k->len_lo, k->len_hi);
        if (st < 15 && grnd(g) < k->tie) {                                       /* held over the next step, if the record keeps going the same way */
            int j0 = (int)((t + 1.05f) * (float)GP), j1 = (int)((t + 1.45f) * (float)GP);
            if (j1 < g->c->n && (((int)g->c->pos[j1] - (int)g->c->pos[j0]) > 0) == (d > 0)) len += 1.0f;
        }
        float at = grnd(g) < 0.22f ? -gr(g, 0.08f, 0.25f) : gr(g, 0.0f, 0.04f);   /* now and then a hair early, ahead of the beat */
        g_gate(g, t + at, t + len, 1);
    }
}

/* a stroke up and back, open: the baby, rubbed. The strokes fill the whole bar (2, 4 or 8 sixteenths each, so they come out even),
 * because a baby that stops to wait is a pause, not a baby. */
static float st_baby(gen_t *g, float o, float base)
{
    static const int lens[] = { 2, 2, 2, 2, 4, 4, 8 };                          /* (the example clips average 2.4 sixteenths to a stroke) */
    int n = lens[gi(g, 0, 6)];
    float top = g_top(g, base, 0.3f, 0.65f, 0.2f, 0.97f), cur = base, t = 0.0f;
    int up = 1, form = gi(g, 0, 9) < 7 ? 0 : (gi(g, 0, 9) < 8 ? 2 : 3);       /* mostly plain strokes; a shark tooth now and then; the reverse one rarely */
    if (form >= 2) {
        /* the shark tooth: a curved climb that bites in hard and then a short, straight drop with the fader shut (the reverse one climbs
         * slowly and speeds up). A pair of strokes' length, 80% climb and 20% drop. */
        while (t + 2.0f * (float)n <= 16.01f) {
            float pair = 2.0f * (float)n, climb = 0.8f * pair, tp = top * gr(g, 0.95f, 1.0f);
            g_seg(g, o + t, o + t + climb, base, tp, form == 2 ? SH_FAST : SH_SLOW);
            g_seg(g, o + t + climb, o + t + pair, tp, base, SH_LIN);
            g_gate(g, o + t + climb, o + t + pair, 0);
            t += pair;
        }
        return base;
    }
    while (t + (float)n <= 16.01f) {
        float to = up ? top * gr(g, 0.95f, 1.0f) : base;
        g_seg(g, o + t, o + t + (float)n, cur, to, g_shape(g));
        cur = to; up = !up; t += (float)n;
    }
    return cur;
}

/* forward cuts: the fader is shut on the pull-back */
static float st_cuts(gen_t *g, float o, float base)
{
    float top = g_top(g, base, 0.25f, 0.55f, 0.2f, 0.95f);
    g_gate(g, o, o + 16.0f, 0);
    int shape = g->shape;                                                       /* the forward stroke: the clip's own (a shark tooth bites in...) */
    for (int k = 0; k < 3; k++) {
        float t = 4.0f * (float)k, tp = top * gr(g, 0.95f, 1.0f);
        g_seg(g, o + t, o + t + 3.0f, base, tp, shape);                         /* the stroke forward, open... */
        g_gate(g, o + t, o + t + 3.0f, 1);
        g_seg(g, o + t + 3.0f, o + t + 4.0f, tp, base, SH_EASE);                /* ...and a quick pull-back, shut */
    }
    g_seg(g, o + 12.0f, o + 15.0f, base, top, SH_SLOW);
    g_gate(g, o + 12.0f, o + 15.0f, 1);
    return top;
}

/* forward cuts and rubs, in blocks of a beat */
static float st_military(gen_t *g, float o, float base)
{
    float top = g_top(g, base, 0.25f, 0.5f, 0.2f, 0.95f), end = base;
    int rub[4] = { 0, gi(g, 0, 1), gi(g, 0, 1), gi(g, 0, 1) };
    if (!rub[1] && !rub[2] && !rub[3]) rub[2] = 1;                          /* some of each */
    g_gate(g, o, o + 16.0f, 0);
    for (int b = 0; b < 4; b++) {
        float t = 4.0f * (float)b, len = b == 3 ? 3.0f : 4.0f, h = len * 0.5f, tp = top * gr(g, 0.95f, 1.0f);
        g_seg(g, o + t, o + t + h, base, tp, g_shape(g));
        g_seg(g, o + t + h, o + t + len, tp, base, g_shape(g));
        g_gate(g, o + t, o + t + (rub[b] ? len : h), 1);
        end = base;
    }
    return end;
}

/* very small, very quick babies, then a ramp */
static float st_scribble(gen_t *g, float o, float base)
{
    float c = g_top(g, base, 0.1f, 0.3f, 0.1f, 0.7f), a = gr(g, 0.03f, 0.07f);
    g_seg(g, o, o + 1.0f, base, c, g_shape(g));
    for (int k = 0; k < 6; k++) g_seg(g, o + 1.0f + (float)k, o + 2.0f + (float)k, c + (k % 2 ? a : -a), c + (k % 2 ? -a : a), g_shape(g));
    g_seg(g, o + 7.0f, o + 15.0f, c, clampf(c + 0.3f, 0.0f, 0.97f), SH_LIN);
    return clampf(c + 0.3f, 0.0f, 0.97f);
}

/* short, fast, forward jabs, the pull-back silent */
static float st_stab(gen_t *g, float o, float base)
{
    int k = gi(g, 6, 10), rot = gi(g, 0, 14), pick[15], n = 0;
    for (int i = 0; i < 15; i++) if (euclid_hit(i, k, 15, rot)) pick[n++] = i;
    g_gate(g, o, o + 16.0f, 0);
    float v = base;
    for (int i = 0; i < n; i++) {
        float s = (float)pick[i], d = (0.10f + 0.12f * g->amp_t) * gr(g, 0.95f, 1.0f);
        g_seg(g, o + s, o + s + 0.75f, v, v + d, SH_FAST);
        g_gate(g, o + s, o + s + 0.75f, 1);
        g_seg(g, o + s + 0.75f, o + s + 1.0f, v + d, v, SH_LIN);
        v = clampf(v + 0.05f, 0.0f, 0.7f);
    }
    return v;
}

/* quick up-and-down strokes, a click at every turn so each stroke is a note of its own */
static float st_chirp(gen_t *g, float o, float base)
{
    float top = g_top(g, base, 0.25f, 0.5f, 0.2f, 0.95f), cur = base, t = 0.0f;
    int up = 1, fast = gi(g, 0, 1);
    while (t + 1.0f <= 15.01f) {
        float len = (fast && t >= 6.0f && t < 10.0f) ? 1.0f : 2.0f, to = up ? top : base;
        if (t + len > 15.01f) break;
        g_seg(g, o + t, o + t + len, cur, to, SH_LIN);
        g_click(g, o + t + len);
        cur = to; up = !up; t += len;
    }
    return cur;
}

/* a slow stroke with the fader chopped in steady steps */
static float st_transform(gen_t *g, float o, float base)
{
    static const int hits[] = { 10, 11, 12, 13, 14 };                                  /* of 16 steps: most of them, so the chopped stroke is mostly sound */
    int k = hits[gi(g, 0, 4)], rot = gi(g, 0, 15);
    float top = g_top(g, base, 0.3f, 0.6f, 0.2f, 0.95f);
    for (int st = 0; st < 4; st++) {                                            /* a stroke a beat long, up and down, not a sweep */
        float t = 4.0f * (float)st, len = st == 3 ? 3.0f : 4.0f;
        g_seg(g, o + t, o + t + len, st % 2 ? top : base, st % 2 ? base : top, SH_EASE);
    }
    g_gate(g, o, o + 16.0f, 0);
    for (int s = 0; s < 15; s++) if (euclid_hit(s, k, 16, rot)) g_gate(g, o + (float)s, o + (float)s + 0.8f, 1);
    return base;
}

/* a stroke up and back, open, with a click in the middle of each half */
static float st_flare(gen_t *g, float o, float base)
{
    float cur = base, t = 0.0f;
    for (int k = 0; k < 2; k++, t += 6.0f) {
        float top = g_top(g, base, 0.25f, 0.6f, 0.2f, 0.95f);
        g_seg(g, o + t, o + t + 3.0f, cur, top, g_shape(g));
        g_seg(g, o + t + 3.0f, o + t + 6.0f, top, base, g_shape(g));
        g_click(g, o + t + 1.5f);
        g_click(g, o + t + 4.5f);
        cur = base;
    }
    float top = g_top(g, base, 0.3f, 0.6f, 0.2f, 0.95f);
    g_seg(g, o + 12.0f, o + 15.0f, base, top, g_shape(g));
    return top;
}

/* a slow stroke with four quick clicks to every sixteenth */
static float st_crab(gen_t *g, float o, float base)
{
    float top = g_top(g, base, 0.3f, 0.6f, 0.2f, 0.95f);
    for (int cy = 0; cy < 2; cy++) {                                            /* a beat up with the clicks, a beat back, twice */
        float t = 8.0f * (float)cy, down = cy == 1 ? 3.0f : 4.0f;
        g_seg(g, o + t, o + t + 4.0f, base, top, SH_LIN);
        g_seg(g, o + t + 4.0f, o + t + 4.0f + down, top, base, SH_EASE);
        g_gate(g, o + t, o + t + 4.0f, 0);
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 4; c++) g_gate(g, o + t + (float)s + 0.25f * (float)c, o + t + (float)s + 0.25f * (float)c + 0.15f, 1);
    }
    return base;
}

/* round and round: a smooth wave, a click in the middle of each half */
static float st_orbit(gen_t *g, float o, float base)
{
    for (int k = 0; k < 2; k++) {
        float t = 7.5f * (float)k, top = g_top(g, base, 0.4f, 0.7f, 0.3f, 0.97f);
        g_seg(g, o + t, o + t + 3.75f, base, top, SH_EASE);
        g_seg(g, o + t + 3.75f, o + t + 7.5f, top, base, SH_EASE);
        g_click(g, o + t + 1.9f);
        g_click(g, o + t + 5.6f);
    }
    return base;
}

/* a slow, low stroke up and a slower one back */
static float st_drag(gen_t *g, float o, float base)
{
    float top = g_top(g, base, 0.3f, 0.6f, 0.2f, 0.95f);
    for (int st = 0; st < 4; st++) {                                            /* slow strokes, a beat each */
        float t = 4.0f * (float)st, len = st == 3 ? 3.0f : 4.0f;
        if (st % 2) g_seg(g, o + t, o + t + len, top, base, SH_EASE);
        else g_seg(g, o + t, o + t + len, base, top, SH_SLOW);
    }
    return base;
}

typedef float (*gen_style_fn)(gen_t *, float, float);
static const gen_style_fn k_styles[GS_COUNT] = { NULL, st_baby, st_cuts, st_military, st_scribble, st_stab, st_chirp, st_transform, st_flare, st_crab, st_orbit, st_drag };
static const int k_any_weight[GS_COUNT] = { 0, 3, 2, 2, 1, 2, 2, 2, 3, 1, 2, 0 };            /* (Drag is only for when you pick it) */

/* What tends to follow what, as a scratcher would string them: a Markov chain over the styles, one state to a bar. A bar's
 * successor is drawn from its row, so a phrase hangs together (a baby leads on to cuts or a flare, a flare to an orbit or a
 * crab) and still differs from one seed to the next. A style may also repeat, with a changed shape. */
static const unsigned char k_next[GS_COUNT][GS_COUNT] = {
    [GS_BABY]      = { [GS_BABY] = 2, [GS_CUTS] = 2, [GS_FLARE] = 2, [GS_CHIRP] = 1, [GS_SCRIBBLE] = 1 },
    [GS_CUTS]      = { [GS_MILITARY] = 3, [GS_CUTS] = 1, [GS_STAB] = 1, [GS_BABY] = 1, [GS_TRANSFORM] = 1 },
    [GS_MILITARY]  = { [GS_CUTS] = 2, [GS_STAB] = 2, [GS_BABY] = 1, [GS_TRANSFORM] = 1, [GS_CHIRP] = 1 },
    [GS_SCRIBBLE]  = { [GS_BABY] = 3, [GS_FLARE] = 2, [GS_CHIRP] = 1 },
    [GS_STAB]      = { [GS_STAB] = 2, [GS_CUTS] = 2, [GS_CHIRP] = 2, [GS_CRAB] = 1, [GS_MILITARY] = 1 },
    [GS_CHIRP]     = { [GS_CHIRP] = 1, [GS_FLARE] = 2, [GS_TRANSFORM] = 3, [GS_CRAB] = 1, [GS_BABY] = 1 },
    [GS_TRANSFORM] = { [GS_FLARE] = 2, [GS_CRAB] = 2, [GS_CHIRP] = 2, [GS_ORBIT] = 1, [GS_TRANSFORM] = 1 },
    [GS_FLARE]     = { [GS_FLARE] = 2, [GS_ORBIT] = 3, [GS_CRAB] = 2, [GS_BABY] = 1, [GS_CHIRP] = 1 },
    [GS_CRAB]      = { [GS_FLARE] = 2, [GS_ORBIT] = 2, [GS_TRANSFORM] = 2, [GS_CHIRP] = 1 },
    [GS_ORBIT]     = { [GS_FLARE] = 2, [GS_BABY] = 2, [GS_SCRIBBLE] = 1, [GS_ORBIT] = 1 },
    [GS_DRAG]      = { [GS_BABY] = 3, [GS_ORBIT] = 2, [GS_CUTS] = 1, [GS_SCRIBBLE] = 1 },
};

static int pick_style(gen_t *g)
{
    int tot = 0;
    for (int s = 1; s < GS_COUNT; s++) tot += k_any_weight[s];
    int r = gi(g, 0, tot - 1);
    for (int s = 1; s < GS_COUNT; s++) { if (r < k_any_weight[s]) return s; r -= k_any_weight[s]; }
    return GS_BABY;
}

static int next_style(gen_t *g, int from)
{
    int tot = 0;
    for (int s = 1; s < GS_COUNT; s++) tot += k_next[from][s];
    if (tot == 0) return pick_style(g);
    int r = gi(g, 0, tot - 1);
    for (int s = 1; s < GS_COUNT; s++) { if (r < k_next[from][s]) return s; r -= k_next[from][s]; }
    return GS_BABY;
}

/* The shape of a phrase. Four bars are A A B A and two are A B (for "any") or A A: the bars with the same letter are exactly the
 * same, which is what makes a phrase hold together; B is a variation: another style that follows A in the chain for "any", or
 * the same style with another throw of the dice. */
static void gen_plan(gen_t *g, int style, int bars, int plan[4], int grp[4])
{
    int a = style == GS_ANY ? pick_style(g) : style;
    int b = style == GS_ANY ? next_style(g, a) : style;
    static const int group4[4] = { 0, 0, 1, 0 };
    for (int i = 0; i < 4; i++) { plan[i] = a; grp[i] = 0; }
    if (bars == 4) { for (int i = 0; i < 4; i++) { grp[i] = group4[i]; plan[i] = group4[i] ? b : a; } }
    else if (bars == 2 && style == GS_ANY) { plan[1] = b; grp[1] = 1; }
}

/* Where a sample has its attacks: the start of its sound, and every place its level climbs fast out of a dip (a new word, the next hit),
 * read from the 128-column overview. Positions 0..1, a column before the attack, at most `cap` of them. A DJ puts the record there so a
 * stroke begins on the attack; the real performances and the example clips turn where the sample is quiet and about to get loud. */
static int slot_onsets(const ts_slot_t *s, float *out, int cap)
{
    if (!s || !s->loaded || cap < 1) return 0;
    float e[TS_VIEW_COLS], mx = 0.0f;
    for (int i = 0; i < TS_VIEW_COLS; i++) { e[i] = (float)s->rms[i] / 255.0f; mx = fmaxf(mx, e[i]); }
    if (mx < 0.05f) return 0;
    int n = 0, last = (int)(s->lo * (float)TS_VIEW_COLS);
    out[n++] = clampf((float)last / (float)TS_VIEW_COLS - 0.01f, 0.0f, 0.9f);                 /* the sound's own start */
    for (int i = last + 6; i < TS_VIEW_COLS - 5 && n < cap; i++) {              /* (a strong rise out of a real dip: a held vowel's wobble is not an attack) */
        float rise = e[i + 4] - e[i];
        if (rise > 0.25f * mx && e[i] < 0.6f * e[i + 4] && e[i] <= e[i - 1] + 0.01f && i - last >= 16) {
            out[n++] = clampf((float)i / (float)TS_VIEW_COLS - 0.01f, 0.0f, 0.9f);
            last = i;
            i += 4;
        }
    }
    return n;
}

/* "Any": a phrase, the way the example clips are made. They are neither one technique repeated nor techniques glued together: a two-bar
 * phrase of strokes of every length (a sixteenth 22%, two 24%, three 21%, four 14%, so nothing pulses on the beat: the clips hardly match
 * themselves a beat later), pushes up by about half the sample and pulls back that far or right back to the start (about half the time),
 * and then the same phrase again with some strokes changed (the clips match themselves two bars later, not exactly). Every stroke is a
 * flick; the fader is laid on afterwards, on the grid. */
typedef struct { int dur; float to; int shape; int fw; } pstroke_t;         /* fw: 1 a forward (let go: the word plays), 2 the quick silent pull after it */
static int phrase_build(gen_t *g, int len16, float base, pstroke_t *sl, int cap)
{
    static const int w[4] = { 22, 24, 21, 14 };
    /* Forwards are planned first, on a beat: in the example clips the record is let go on beat 3 (sixteenth 8), for exactly a beat, or on
     * beat 4 (sixteenth 12), to the bar line, then pulled back with the fader shut; never on an odd sixteenth. About three bars in four
     * have one. The record moves a hair before the beat and the fader opens on it, like every other stroke (the path is moved earlier). */
    int fw_at[4] = { -1, -1, -1, -1 };
    for (int b = 0; b < len16 / 16 && b < 4; b++)
        if (grnd(g) < 0.75f) fw_at[b] = b * 16 + (grnd(g) < 0.55f ? 8 : 12);
    int ns = 0, t = 0, up = 1;
    float cur = base;
    while (t < len16 && ns + 3 < cap) {
        int bar = t / 16, fa = bar < 4 ? fw_at[bar] : -1;
        if (fa == t) {                                                           /* the forward: at about the sample's own speed, through most of it */
            int fd = (t % 16) == 8 ? 4 : 3, pd = (t % 16) == 8 ? gi(g, 1, 2) : 1;
            if (t + fd + pd > len16) pd = len16 - t - fd;
            float span = clampf(0.88f * (float)fd / (float)(g->fw16 > 0 ? g->fw16 : 4), 0.4f, 0.93f - base);
            sl[ns].dur = fd; sl[ns].to = clampf(base + span, base + 0.3f, 0.97f); sl[ns].shape = SH_LIN; sl[ns].fw = 1; ns++;
            if (pd > 0) { sl[ns].dur = pd; sl[ns].to = base; sl[ns].shape = SH_FAST; sl[ns].fw = 2; ns++; }
            t += fd + pd; cur = base; up = 1;
            continue;
        }
        int left = len16 - t, r = gi(g, 0, 80), d = 1;
        for (int k = 0; k < 4; k++) { if (r < w[k]) { d = k + 1; break; } r -= w[k]; }
        if (d > left) d = left;
        if (fa > t && t + d >= fa) {                                             /* arrive at the start, at the start of the sound, on the beat */
            d = fa - t;
            if (up && d >= 2) d -= 1;                                            /* (a push and then a pull back: the pull lands on the beat) */
            else if (up) up = 0;                                                 /* (a sixteenth to spare: a short pull, back to the start) */
        }
        int last = t + d >= len16;
        if (last && up && left >= 2) { d = left - 1; last = 0; }                 /* (the phrase ends on a pull back to the start) */
        int to_start = !up && fa > t && t + d == fa;
        float span = 0.24f + 0.46f * powf(grnd(g), 0.9f);
        float to;
        if (last || to_start) to = base;
        else if (up) to = clampf(cur + span, base + 0.12f, 0.97f);
        else to = grnd(g) < 0.5f ? clampf(base + gr(g, -0.01f, 0.04f), 0.0f, 0.97f) : clampf(cur - span, base, 0.97f);
        sl[ns].dur = d; sl[ns].to = to; sl[ns].shape = grnd(g) < 0.88f ? SH_FLICK : SH_EASE; sl[ns].fw = 0;
        ns++; cur = to; up = !up; t += d;
    }
    return ns;
}

static void gen_phrase(gen_t *g, int bars, const float *bases)
{
    int P = bars >= 2 ? 2 : 1;                                                   /* a phrase is two bars (one in a one-bar clip) */
    pstroke_t sl[64], v[64];
    int ns = phrase_build(g, P * 16, bases[0], sl, 64);
    for (int rep = 0; rep * P < bars; rep++) {
        float base = bases[rep * P];
        for (int k = 0; k < ns; k++) v[k] = sl[k];
        if (rep > 0)                                                             /* the answer: the same phrase, a few strokes changed */
            for (int k = 0; k + 1 < ns; k++) {
                if (v[k].fw || v[k + 1].fw) continue;                            /* (a forward stays as it is) */
                if (grnd(g) < 0.45f) v[k].to = clampf(v[k].to + gr(g, -0.2f, 0.2f), base, 0.97f);
                if (k + 2 < ns && grnd(g) < 0.12f && v[k].dur + v[k + 1].dur >= 2) { int a = v[k].dur; v[k].dur = v[k + 1].dur; v[k + 1].dur = a; }
            }
        float t = 16.0f * (float)(rep * P), cur = (float)g->c->pos[(int)(t * (float)GP) % g->c->n] / 65535.0f;
        if (rep == 0) cur = base;
        for (int k = 0; k < ns; k++) {
            float to = k == ns - 1 ? bases[((rep + 1) * P) % bars] : (v[k].fw ? v[k].to : (v[k].to < base && k % 2 ? base : v[k].to));
            g_seg(g, t, t + (float)v[k].dur, cur, to, v[k].shape);
            if (v[k].fw && g->nfw < 16) { g->fw_t[g->nfw] = t; g->fw_d[g->nfw] = (float)v[k].dur; g->fw_kind[g->nfw] = v[k].fw; g->nfw++; }
            cur = to; t += (float)v[k].dur;
        }
    }
}

static void gen_once(ts_clip_t *out, int style, uint32_t seed, int bars, const float *onsets, int n_on, int fw16)
{
    if (!out) return;
    bars = bars <= 1 ? 1 : (bars >= 4 ? 4 : 2);
    style = clampi(style, 0, GS_COUNT - 1);
    memset(out, 0, sizeof *out);
    gen_t g;
    g.c = out;
    g.bars = bars;
    g.fw16 = fw16;
    g.nfw = 0;
    g.rng = mix32(seed) | 1u;
    int n = bars * 16 * GP;
    out->n = n;
    out->len = bars * 4;
    out->tag = n_on > 0 ? TAG_GENERATED : 0;
    float base0 = gr(&g, 0.04f, 0.16f);
    uint16_t b16 = (uint16_t)(base0 * 65535.0f + 0.5f);
    for (int i = 0; i < n; i++) { out->pos[i] = b16; out->gate[i] = 1; }
    static const float heights[3] = { 0.15f, 0.5f, 0.85f };
    g.amp_t = heights[gi(&g, 0, 2)];                                            /* how tall the strokes are, for the whole clip */
    { int r = gi(&g, 0, 19); g.shape = r < 17 ? SH_FLICK : (r < 19 ? SH_EASE : SH_SLOW); }  /* and how they are shaped: almost always a flick */
    int plan[4], grp[4];
    gen_plan(&g, style, bars, plan, grp);
    float bases[4];                                                             /* where each bar's strokes start from: an attack of the sample, if there is one to find */
    for (int bar = 0; bar < bars; bar++) {
        if (n_on <= 0) { bases[bar] = base0; continue; }
        gen_t t = g;
        t.rng = mix32(seed ^ ((uint32_t)(grp[bar] * 2 + 1) * 0x9E3779B9u) ^ ((uint32_t)plan[bar] * 0x85EBCA6Bu) ^ 0xBA5E0000u) | 1u;     /* (the same letter, the same place) */
        bases[bar] = clampf(onsets[gi(&t, 0, n_on - 1)] + gr(&t, 0.02f, 0.07f), 0.0f, 0.9f);        /* (a little way into the attack: the turn is on the rise) */
    }
    if (style == GS_ANY) {                                                      /* a phrase, not techniques */
        for (int bar = 0; bar < bars; bar++) { plan[bar] = GS_ANY; grp[bar] = bar; }
        gen_phrase(&g, bars, bases);
    }
    for (int bar = 0; bar < bars && style != GS_ANY; bar++) {
        int s = plan[bar];
        g.rng = mix32(seed ^ ((uint32_t)(grp[bar] * 2 + 1) * 0x9E3779B9u) ^ ((uint32_t)s * 0x85EBCA6Bu)) | 1u;      /* the same letter, the same bar */
        float o = 16.0f * (float)bar, base = bases[bar], next = bases[(bar + 1) % bars];
        { uint16_t bb = (uint16_t)(base * 65535.0f + 0.5f); for (int i = (int)(o * (float)GP); i < (int)((o + 16.0f) * (float)GP) && i < n; i++) out->pos[i] = bb; }
        float end = k_styles[s](&g, o, base);
        if (s != GS_STAB && s != GS_CRAB) {                                     /* (a stab's jabs and a crab's clicks keep their exact places) */
            g_warp(&g, o, base);
            int ie = (int)((o + 15.0f) * (float)GP) - 1;
            if (ie >= 0 && ie < out->n) end = (float)out->pos[ie] / 65535.0f;   /* where the warped path really is */
        }
        if (fabsf(end - next) > 0.04f) {                                        /* a stroke that did not get back: on to the next start, silently */
            g_seg(&g, o + 15.0f, o + 16.0f, end, next, SH_EASE);
            g_gate(&g, o + 15.0f, o + 15.85f, 0);
        }
        if (!grid_style(s) && s != GS_STAB && s != GS_CRAB) { g_mask_turns(&g, o, s); g_limit_open(&g, o); }
    }
    /* now and then the last half bar runs out: a stroke on to the end of the sample, so its tail plays, and a silent jump back */
    g.rng = mix32(seed ^ 0xA5A5F00Du) | 1u;
    int last = plan[bars - 1];
    if (last != GS_STAB && last != GS_CRAB && gi(&g, 0, 4) == 0) {
        float o = 16.0f * (float)(bars - 1);
        int i0 = (int)((o + 8.0f) * (float)GP);
        float from = (float)out->pos[i0] / 65535.0f, to = gr(&g, 0.93f, 0.99f);
        g_seg(&g, o + 8.0f, o + 15.0f, from, to, gi(&g, 0, 1) ? SH_LIN : SH_SLOW);
        g_gate(&g, o + 8.0f, o + 15.0f, 1);
        g_seg(&g, o + 15.0f, o + 16.0f, to, bases[0], SH_FAST);
        g_gate(&g, o + 15.0f, o + 15.9f, 0);
    }
    /* The record turns a little ahead of the grid: the whole path moves earlier by a quarter of a sixteenth or so (it loops, so it wraps),
     * then the grid styles get their fader, on the grid. */
    {
        gen_t t = g;
        t.rng = mix32(seed ^ 0x51DE5EEDu) | 1u;
        int sh = (int)(gr(&t, 0.18f, 0.34f) * (float)GP);
        static uint16_t tmp[TS_CLIP_MAX_BEATS * TS_CLIP_RES];
        static uint8_t tg[TS_CLIP_MAX_BEATS * TS_CLIP_RES];
        for (int i = 0; i < n; i++) { tmp[i] = out->pos[(i + sh) % n]; tg[i] = out->gate[(i + sh) % n]; }
        memcpy(out->pos, tmp, sizeof(uint16_t) * (size_t)n);
        for (int i = 0; i < n; i++) {                                           /* a stab's jabs and a crab's clicks belong to their strokes: they move with them */
            int s = plan[i / (16 * GP) < bars ? i / (16 * GP) : bars - 1];
            if (s == GS_STAB || s == GS_CRAB) out->gate[i] = tg[i];
        }
    }
    for (int bar = 0; bar < bars; bar++) {
        int s = plan[bar];
        if (!grid_style(s)) continue;
        g.rng = mix32(seed ^ ((uint32_t)(grp[bar] * 2 + 1) * 0x9E3779B9u) ^ ((uint32_t)s * 0x85EBCA6Bu) ^ 0x6A7Eu) | 1u;     /* (the same letter, the same notes) */
        g_grid_gate(&g, 16.0f * (float)bar, s);
        for (int f = 0; f < g.nfw; f++) {                                       /* a forward is one long sound; the pull after it is silent */
            if (g.fw_t[f] < 16.0f * (float)bar || g.fw_t[f] >= 16.0f * (float)(bar + 1)) continue;
            if (g.fw_kind[f] == 1) g_gate(&g, g.fw_t[f], g.fw_t[f] + g.fw_d[f] - 0.3f, 1);
            else g_gate(&g, g.fw_t[f] - 0.3f, g.fw_t[f] + g.fw_d[f], 0);
        }
        g_mask_turns(&g, 16.0f * (float)bar, s);
    }

}

/* How much of a clip is heard: the sound is cut in and the playhead is moving (a stopped playhead is silence, whatever the fader does). */
static float gen_audible(const ts_clip_t *c)
{
    int heard = 0, run = 0, gap = 0;
    for (int i = 0; i < c->n; i++) {
        int j = (i + 1) % c->n;
        float d = fabsf((float)c->pos[j] - (float)c->pos[i]) / 65535.0f * (float)TS_CLIP_RES;      /* sample lengths to the beat */
        int on = c->gate[i] != 0 && d > 0.05f;
        heard += on;
        run = on ? 0 : run + 1;
        if (run >= TS_CLIP_RES) gap = 1;                                         /* a whole beat of nothing */
    }
    float a = c->n > 0 ? (float)heard / (float)c->n : 0.0f;
    return gap ? a * 0.5f : a;                                                   /* (counts as too quiet: thrown again) */
}

void ts_clip_generate(ts_clip_t *out, int style, uint32_t seed, int bars) { ts_clip_generate_for(out, style, seed, bars, NULL); }

void ts_clip_generate_for(ts_clip_t *out, int style, uint32_t seed, int bars, const ts_slot_t *slot)
{
    if (!out) return;
    float onsets[8];
    int n_on = slot_onsets(slot, onsets, 8);
    /* how long a forward takes: most of the sound at about the sample's own speed, at 120 bpm (a sixteenth is 1/8 s); fixed, so a recipe
     * makes the same clip whatever the tempo it is loaded at */
    int fw16 = 4;
    if (slot && slot->loaded && slot->len > 0) fw16 = clampi((int)(0.8f * (slot->hi - slot->lo) * (float)slot->len / TS_SAMPLE_RATE / 0.125f + 0.5f), 3, 7);
    static const float need[GS_COUNT] = { 0.42f, 0.45f, 0.40f, 0.40f, 0.50f, 0.28f, 0.42f, 0.38f, 0.45f, 0.45f, 0.45f, 0.45f };
    style = clampi(style, 0, GS_COUNT - 1);
    static ts_clip_t best;                                                      /* (called from the thread that sets parameters, one at a time) */
    float best_a = -1.0f;
    for (int attempt = 0; attempt < 8; attempt++) {                             /* a seed that comes out mostly silent is thrown again: same seed, same clip, always */
        gen_once(out, style, attempt ? mix32(seed ^ ((uint32_t)attempt * 0x9E3779B9u)) : seed, bars, onsets, n_on, fw16);
        float a = gen_audible(out);
        if (a >= need[style]) return;
        if (a > best_a) { best_a = a; best = *out; }
    }
    *out = best;
}

static uint32_t gen_seed(ts_engine_t *e)
{
    return mix32((uint32_t)(e->free_clock * 977.0) ^ mix32(++e->gen_count * 2654435761u) ^ e->rng);
}

int ts_engine_generate(ts_engine_t *e)
{
    if (!e) return -1;
    int style = clampi(e->p.gen_style, 0, TS_GEN_STYLES - 1), bars = e->p.gen_bars <= 0 ? 1 : (e->p.gen_bars == 1 ? 2 : 4);
    int idx = e->try_valid && e->try_idx == TS_POOL ? TS_POOL + 1 : TS_POOL;               /* the one that is not playing */
    uint32_t seed = gen_seed(e);
    ts_clip_generate_for(&e->clip_data[idx], style, seed, bars, &e->slot[e->p.sample]);
    __sync_synchronize();
    e->try_recipe.used = 1; e->try_recipe.style = style; e->try_recipe.bars = bars; e->try_recipe.seed = seed;
    e->try_idx = idx;
    e->try_valid = 1;
    e->p.clip = TS_CLIP_TRY;
    return style;
}

static int free_pool(const ts_engine_t *e)
{
    for (int i = 0; i < TS_POOL; i++) if (e->clip_data[i].n == 0) return i;
    return -1;
}

static int first_free_pad(const ts_engine_t *e, int sample)
{
    for (int i = 1; i <= TS_CLIP_PADS; i++) if (e->bank[sample][i - 1] == 0) return i;
    return -1;
}

/* the pad where this sample already has this recipe, or -1: the same clip is never kept twice */
static int find_recipe(const ts_engine_t *e, int sample, int style, int bars, uint32_t seed)
{
    for (int pad = 1; pad <= TS_NUM_CLIPS; pad++) {
        int idx = e->bank[sample][pad - 1] - 1;
        if (idx >= 0 && e->kept[idx].used && e->kept[idx].style == style && e->kept[idx].bars == bars && e->kept[idx].seed == seed) return pad;
    }
    return -1;
}

static int install_clip(ts_engine_t *e, int sample, int pad, const ts_clip_t *src, const ts_recipe_t *r)
{
    int idx = free_pool(e);
    if (idx < 0) return -1;
    ts_clip_t *d = &e->clip_data[idx];
    d->n = 0;                                                                   /* empty to the audio thread until it is whole */
    __sync_synchronize();
    memcpy(d->pos, src->pos, sizeof d->pos);
    memcpy(d->gate, src->gate, sizeof d->gate);
    d->len = src->len;
    d->tag = src->tag;
    __sync_synchronize();
    d->n = src->n;
    e->kept[idx] = *r;
    e->kept[idx].used = 1;
    __sync_synchronize();
    e->bank[sample][pad - 1] = idx + 1;                                         /* on its pad last */
    e->removed[sample][pad - 1] = 0;
    recount_clips(e);
    e->recipes_dirty = 1;
    return 0;
}

int ts_engine_keep(ts_engine_t *e, int pad)
{
    if (!e || !e->try_valid) return -1;
    int sample = e->p.sample;
    int have = find_recipe(e, sample, e->try_recipe.style, e->try_recipe.bars, e->try_recipe.seed);
    if (have > 0) { e->p.clip = have; return have; }                            /* already kept: go to it, do not keep it twice */
    if (pad <= 0) pad = first_free_pad(e, sample);
    if (pad < 1 || pad > TS_CLIP_PADS || e->bank[sample][pad - 1] != 0) return -1;
    if (install_clip(e, sample, pad, &e->clip_data[e->try_idx], &e->try_recipe) != 0) return -1;
    e->p.clip = pad;
    return pad;
}

void ts_engine_clip_pad(ts_engine_t *e, int n)
{
    if (!e || n < 1 || n > TS_CLIP_PADS) return;
    if (bank_clip(e, e->p.sample, n)) e->p.clip = n;                            /* a clip: play it */
    else if (e->p.clip == TS_CLIP_TRY && e->try_valid) ts_engine_keep(e, n);    /* an empty pad, with the clip just generated playing: keep it here */
    else e->p.clip = n;                                                         /* an empty pad: nothing plays */
}

/* a clip off its pad: one you kept leaves the pool; a built-in or imported one is shared by the samples, so only this pad lets go
 * of it (and the library remembers that). The other pads stay where they are. 0 if there was a clip. */
static int drop_pad(ts_engine_t *e, int sample, int pad, int all)
{
    int idx = e->bank[sample][pad - 1] - 1;
    if (idx < 0 || (!all && !e->kept[idx].used) || e->ro[idx]) return -1;       /* (an imported clip is read-only) */
    e->bank[sample][pad - 1] = 0;                                               /* off its pad first */
    __sync_synchronize();
    if (e->kept[idx].used) {
        e->clip_data[idx].n = 0;
        memset(&e->kept[idx], 0, sizeof e->kept[idx]);
    } else {
        e->removed[sample][pad - 1] = 1;
    }
    if (e->p.sample == sample && e->p.clip == pad) e->p.clip = TS_CLIP_AUTO;
    recount_clips(e);
    e->recipes_dirty = 1;
    return 0;
}
static int drop_kept(ts_engine_t *e, int sample, int pad) { return drop_pad(e, sample, pad, 0); }

int ts_engine_clear_clip(ts_engine_t *e, int pad)
{
    if (!e) return -1;
    if (pad <= 0) pad = e->p.clip;
    if (pad < 1 || pad > TS_NUM_CLIPS) return -1;
    return drop_pad(e, e->p.sample, pad, 1);
}

/* A kept clip back on its pad: from its saved data if there is any (so a clip you chose sounds the same whatever the generator has become
 * since), otherwise made again from its recipe. */
static int restore_kept_from(ts_engine_t *e, int sample, int pad, int style, uint32_t seed, int bars, const ts_clip_t *data)
{
    if (!e || sample < 0 || sample >= TS_SLOTS) return -1;
    if (find_recipe(e, sample, clampi(style, 0, TS_GEN_STYLES - 1), bars, seed) > 0) return -2;            /* it is there already */
    if (pad < 1 || pad > TS_CLIP_PADS || e->bank[sample][pad - 1] != 0) pad = first_free_pad(e, sample);     /* its pad is taken: the next free one */
    if (pad < 0) return -1;
    static ts_clip_t tmp;                                                       /* (called from the thread that sets parameters, one at a time) */
    if (data) tmp = *data;
    else ts_clip_generate_for(&tmp, clampi(style, 0, TS_GEN_STYLES - 1), seed, bars, &e->slot[sample]);
    ts_recipe_t r = { 1, clampi(style, 0, TS_GEN_STYLES - 1), tmp.len / 4, seed };
    if (install_clip(e, sample, pad, &tmp, &r) != 0) return -1;
    e->recipes_dirty = 0;                                                       /* it came from the library */
    return pad;
}

int ts_engine_restore_kept(ts_engine_t *e, int sample, int pad, int style, uint32_t seed, int bars)
{
    return restore_kept_from(e, sample, pad, style, seed, bars, NULL);
}

/* ---- kept.bin: the kept clips' own data, next to recipes.txt ----
 * "TSKP" 1, then a 16-bit count, then for each clip: sample, pad, style, bars (a byte each), the seed (32 bits), the length in beats and the
 * tag (a byte each), the point count (16 bits), the positions (16 bits each) and the gate (a byte each). Little-endian. */
static void kept_path(const ts_engine_t *e, char *out, size_t cap)
{
    snprintf(out, cap, "%s", e->recipes_path);
    char *cut = strrchr(out, '/'), *bs = strrchr(out, '\\');
    if (bs && (!cut || bs > cut)) cut = bs;
    if (cut) cut[1] = 0; else out[0] = 0;
    size_t len = strlen(out);
    snprintf(out + len, cap - len, "kept.bin");
}

static void save_kept(ts_engine_t *e)
{
    char path[520], tmp[530];
    kept_path(e, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int count = 0;
    for (int s = 0; s < TS_SLOTS; s++) for (int pad = 1; pad <= TS_NUM_CLIPS; pad++) { int idx = e->bank[s][pad - 1] - 1; count += idx >= 0 && e->kept[idx].used; }
    uint8_t h[6] = { 'T', 'S', 'K', 'P', 1, 0 };
    int ok = fwrite(h, 1, 5, f) == 5 && fputc(count & 255, f) != EOF && fputc(count >> 8, f) != EOF;
    for (int s = 0; s < TS_SLOTS && ok; s++)
        for (int pad = 1; pad <= TS_NUM_CLIPS && ok; pad++) {
            int idx = e->bank[s][pad - 1] - 1;
            if (idx < 0 || !e->kept[idx].used) continue;
            const ts_clip_t *c = &e->clip_data[idx];
            const ts_recipe_t *r = &e->kept[idx];
            uint8_t hd[12] = { (uint8_t)s, (uint8_t)pad, (uint8_t)r->style, (uint8_t)r->bars,
                               (uint8_t)(r->seed & 255), (uint8_t)((r->seed >> 8) & 255), (uint8_t)((r->seed >> 16) & 255), (uint8_t)(r->seed >> 24),
                               (uint8_t)c->len, (uint8_t)c->tag, (uint8_t)(c->n & 255), (uint8_t)(c->n >> 8) };
            ok = fwrite(hd, 1, 12, f) == 12;
            for (int i = 0; i < c->n && ok; i++) ok = fputc(c->pos[i] & 255, f) != EOF && fputc(c->pos[i] >> 8, f) != EOF;
            ok = ok && fwrite(c->gate, 1, (size_t)c->n, f) == (size_t)c->n;
        }
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return; }
    remove(path);
    if (rename(tmp, path) != 0) remove(tmp);
}

/* the saved data of one kept clip, found by its recipe; 1 if there is some */
static int kept_find(const uint8_t *d, size_t n, int sample, int style, int bars, uint32_t seed, ts_clip_t *out)
{
    if (!d || n < 7 || memcmp(d, "TSKP", 4) != 0 || d[4] != 1) return 0;
    int count = d[5] | (d[6] << 8);
    size_t at = 7;
    for (int k = 0; k < count && at + 12 <= n; k++) {
        const uint8_t *h = d + at;
        int len = h[8], tag = h[9], pts = h[10] | (h[11] << 8);
        size_t size = 12 + (size_t)pts * 3;
        if (at + size > n || pts != len * TS_CLIP_RES || len < 4 || len > TS_CLIP_MAX_BEATS || tag > TAG_GENERATED) return 0;      /* a broken file: none of it */
        uint32_t sd = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
        if (h[0] == sample && h[2] == style && h[3] == bars && sd == seed) {
            memset(out, 0, sizeof *out);
            out->len = len; out->tag = tag; out->n = pts;
            for (int i = 0; i < pts; i++) out->pos[i] = (uint16_t)(h[12 + 2 * i] | (h[13 + 2 * i] << 8));
            for (int i = 0; i < pts; i++) out->gate[i] = h[12 + 2 * pts + i] ? 1 : 0;
            return 1;
        }
        at += size;
    }
    return 0;
}

void ts_engine_select_sample(ts_engine_t *e, int s)
{
    if (!e || s < 0 || s >= TS_SLOTS || s == e->p.sample) return;
    e->bank_last[e->p.sample] = e->p.clip;                                      /* remember what this one had... */
    e->p.sample = s;
    e->p.clip = e->bank_last[s];                                                /* ...and bring back what the next had */
    recount_clips(e);
}

void ts_engine_set_recipes_path(ts_engine_t *e, const char *path)
{
    if (e) snprintf(e->recipes_path, sizeof e->recipes_path, "%s", path ? path : "");
}

int ts_engine_load_recipes(ts_engine_t *e)
{
    if (!e || !e->recipes_path[0]) return 0;
    FILE *f = fopen(e->recipes_path, "rb");
    if (!f) return 0;
    char line[128], kp[520];
    int n = 0, dropped = 0, missing = 0;
    uint8_t *kd = NULL;
    size_t kn = 0;
    kept_path(e, kp, sizeof kp);
    FILE *kf = fopen(kp, "rb");
    if (kf) {
        long sz = -1;
        if (fseek(kf, 0, SEEK_END) == 0) sz = ftell(kf);
        if (sz > 0 && sz < 4L * 1024 * 1024 && fseek(kf, 0, SEEK_SET) == 0 && (kd = (uint8_t *)TS_CALLOC((size_t)sz)) != NULL) kn = fread(kd, 1, (size_t)sz, kf);
        fclose(kf);
    }
    static ts_clip_t saved;
    while (fgets(line, sizeof line, f) && n < TS_POOL) {
        int sample, pad, style, bars;
        unsigned long seed;
        if (line[0] == '-') {                                                   /* "- sample pad": a built-in or imported clip taken off its pad */
            if (sscanf(line, "- %d %d", &sample, &pad) == 2 && sample >= 0 && sample < TS_SLOTS && pad >= 1 && pad <= TS_NUM_CLIPS &&
                e->bank[sample][pad - 1] > 0 && !e->kept[e->bank[sample][pad - 1] - 1].used && !e->ro[e->bank[sample][pad - 1] - 1]) {
                e->bank[sample][pad - 1] = 0;
                e->removed[sample][pad - 1] = 1;
            }
            continue;
        }
        int got = sscanf(line, "%d %d %d %d %lu", &sample, &pad, &style, &bars, &seed);
        if (got != 5) {                                                         /* an older library: no sample column, so the first sample's */
            sample = 0;
            if (sscanf(line, "%d %d %d %lu", &pad, &style, &bars, &seed) != 4) continue;
        }
        if ((bars == 1 || bars == 2 || bars == 4) && style >= 0 && style < TS_GEN_STYLES && sample >= 0 && sample < TS_SLOTS) {
            int have = kept_find(kd, kn, sample, style, bars, (uint32_t)seed, &saved);
            int r = restore_kept_from(e, sample, pad, style, (uint32_t)seed, bars, have ? &saved : NULL);
            missing |= r > 0 && !have;                                          /* (made again from its recipe: its data is written next time) */
            if (r > 0) n++;
            else if (r == -2) dropped = 1;                                      /* a copy of one it already has: left out */
        }
    }
    fclose(f);
    TS_FREE(kd);
    recount_clips(e);
    e->recipes_dirty = dropped || missing;                                      /* (the library is rewritten without the copies, and with the data) */
    return n;
}

static void save_recipes(ts_engine_t *e)
{
    if (!e->recipes_path[0]) return;
    char tmp[500];
    snprintf(tmp, sizeof tmp, "%s.tmp", e->recipes_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = 1;
    for (int s = 0; s < TS_SLOTS && ok; s++)                                     /* the clips taken off their pads first, so they are gone before the kept ones are placed */
        for (int pad = 1; pad <= TS_NUM_CLIPS && ok; pad++)
            if (e->removed[s][pad - 1] && e->bank[s][pad - 1] == 0) ok = fprintf(f, "- %d %d\n", s, pad) > 0;
    for (int s = 0; s < TS_SLOTS && ok; s++)
        for (int pad = 1; pad <= TS_NUM_CLIPS && ok; pad++) {
            int idx = e->bank[s][pad - 1] - 1;
            if (idx >= 0 && e->kept[idx].used)
                ok = fprintf(f, "%d %d %d %d %lu\n", s, pad, e->kept[idx].style, e->kept[idx].bars, (unsigned long)e->kept[idx].seed) > 0;
        }
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return; }
    remove(e->recipes_path);                                                    /* (a rename does not replace a file on every system) */
    if (rename(tmp, e->recipes_path) != 0) remove(tmp);
    save_kept(e);
}

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

void ts_params_default(ts_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->sample = 0;
    p->turntable = 0.0f;
    p->drag = 0.5f;
    p->gain_db = 0.0f;
    p->note_gate = 1;
    p->vel_to_vol = 0;
    p->normalise = 1;
    p->lock = 1;
    p->cut_from = TS_CUT_NOTE;
    p->pads = 1;
    p->clip = TS_CLIP_AUTO;              /* the clips in turn: a routine from the first press */
    p->gen_style = 0;
    p->gen_bars = 1;                     /* two bars */
    p->clip_depth = 1.0f;
    p->clip_offset = 0.0f;
}

void ts_params_sanitize(ts_params_t *p)
{
    p->sample = clampi(p->sample, 0, TS_SLOTS - 1);
    p->turntable = isfinite(p->turntable) ? clampf(p->turntable, 0.0f, 1.0f) : 0.0f;
    p->drag = isfinite(p->drag) ? clampf(p->drag, 0.0f, 1.0f) : 0.5f;
    p->gain_db = isfinite(p->gain_db) ? clampf(p->gain_db, -24.0f, 12.0f) : 0.0f;
    p->note_gate = p->note_gate ? 1 : 0;
    p->vel_to_vol = p->vel_to_vol ? 1 : 0;
    p->normalise = p->normalise ? 1 : 0;
    p->lock = p->lock ? 1 : 0;
    p->cut_from = clampi(p->cut_from, 0, 127);
    p->pads = p->pads ? 1 : 0;
    p->clip = clampi(p->clip, 0, TS_CLIP_TRY);
    p->gen_style = clampi(p->gen_style, 0, TS_GEN_STYLES - 1);
    p->gen_bars = clampi(p->gen_bars, 0, 2);
    p->clip_depth = isfinite(p->clip_depth) ? clampf(p->clip_depth, 0.0f, 2.0f) : 1.0f;
    p->clip_offset = isfinite(p->clip_offset) ? clampf(p->clip_offset, -0.5f, 0.5f) : 0.0f;
}

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

ts_engine_t *ts_engine_create(float sample_rate)
{
    if (!(sample_rate >= 8000.0f && sample_rate <= 192000.0f)) sample_rate = TS_SAMPLE_RATE;
    ts_engine_t *e = (ts_engine_t *)TS_CALLOC(sizeof(*e));
    if (!e) return NULL;
    e->sr = sample_rate;
    ts_params_default(&e->p);
    e->bpm = 120.0f;
    e->rng = 0x2545F491u;
    e->last_vel = 127;
    e->lane_col = -1;
    e->clip_now = 1;
    e->lane_len = TS_LANE_BEATS;
    clips_builtin(e);
    e->try_idx = TS_POOL;
    for (int i = 0; i < TS_SLOTS; i++) e->bank_last[i] = TS_CLIP_AUTO;
    e->rec_cap = (int)((float)TS_REC_SECONDS * sample_rate);
    e->rec_buf = (float *)TS_CALLOC((size_t)e->rec_cap * sizeof(float));
    if (!e->rec_buf) e->rec_cap = 0;                                         /* no recording, but everything else works */
    return e;
}

void ts_engine_destroy(ts_engine_t *e)
{
    if (!e) return;
    for (int i = 0; i < TS_SLOTS; i++) TS_FREE(e->slot[i].data);
    for (int i = 0; i < TS_SLOTS + 2; i++) TS_FREE(e->garbage[i]);
    TS_FREE(e->pend.data);
    TS_FREE(e->rec_buf);
    TS_FREE(e);
}

void ts_engine_set_params(ts_engine_t *e, const ts_params_t *in)
{
    if (!e || !in) return;
    ts_params_t n = *in;
    ts_params_sanitize(&n);
    if (n.turntable != e->p.turntable) {                           /* a knob turn is a hand on the record: it takes over from the clip for a moment */
        e->man = n.turntable;
        e->hand_hold = (int)(0.6f * e->sr);
    }
    int switched = n.sample != e->p.sample;
    if (switched) {                                       /* another sample: its own clip pads, and the clip it last had */
        e->bank_last[e->p.sample] = e->p.clip;
        if (n.clip == e->p.clip) n.clip = e->bank_last[n.sample];        /* (unless the caller chose a clip in the same breath) */
    }
    e->p = n;
    if (switched) recount_clips(e);
}

void ts_engine_set_tempo(ts_engine_t *e, float bpm)
{
    if (e) e->bpm = clampf(bpm, 20.0f, 300.0f);
}

void ts_engine_sync_beat(ts_engine_t *e, double beat)
{
    if (!e) return;
    if (beat >= 0.0 && beat < 1e9) {
        e->transport = 1;
        e->clock = beat;
    } else {
        e->transport = 0;
    }
}

int ts_pad_index(int note)
{
    return note >= TS_PAD_FIRST && note <= TS_PAD_LAST ? (note - TS_PAD_FIRST) & 31 : -1;
}

void ts_engine_note(ts_engine_t *e, int note, int velocity, int on)
{
    if (!e || note < 0 || note > 127) return;
    if (on) e->last_note = note;
    if (e->pad_raw > 0) return;                                 /* the Pads screen is reporting the pads: these notes are theirs, moved about */
    int pad = e->p.pads ? ts_pad_index(note) : -1;
    if (pad >= 0) {                                             /* the pad grid */
        int row = pad / 8, col = pad % 8;
        if (row < 1) {                                          /* the bottom row cuts */
            if (on) {
                if (e->notes_held == 0) e->held_clock = 0.0;
                if (e->notes_held < 64) e->notes_held++;
                e->last_vel = clampi(velocity, 1, 127);
            } else if (e->notes_held > 0) {
                e->notes_held--;
            }
        } else if (!on) {
            return;
        } else if (row == 1) {                                  /* second row: the samples (the last two pads are not used) */
            if (col < TS_SLOTS) ts_engine_select_sample(e, col);
        } else {                                                /* the top two rows: the clips, then Generate and Auto */
            int n = (3 - row) * 8 + col + 1;                    /* the top row is clips 1-8, the one below 9-14, then Generate (15) and Auto (16) */
            if (n == TS_CLIP_PADS + 1) ts_engine_generate(e);
            else if (n > TS_CLIP_PADS + 1) e->p.clip = TS_CLIP_AUTO;
            else ts_engine_clip_pad(e, n);
        }
        return;
    }
    if (note >= e->p.cut_from) {
        if (on) {
            if (e->notes_held == 0) e->held_clock = 0.0;
            if (e->notes_held < 64) e->notes_held++;
            e->last_vel = clampi(velocity, 1, 127);
        } else if (e->notes_held > 0) {
            e->notes_held--;
        }
    }
}

void ts_engine_all_notes_off(ts_engine_t *e)
{
    if (e) e->notes_held = 0;
}

void ts_engine_pad_raw(ts_engine_t *e, int on)
{
    if (!e) return;
    if (on) {
        if (e->pad_raw <= 0) e->notes_held = 0;                 /* notes held from before are not coming off now */
        e->pad_raw = (int)(1.5f * e->sr);
    } else {
        e->pad_raw = 0;
        e->pad_cuts = 0;
        e->pad_held = 0;
    }
}

void ts_engine_set_zoom(ts_engine_t *e, int zoom)
{
    if (e) e->zoom = clampi(zoom, 0, 3);
}

void ts_engine_pad_cuts(ts_engine_t *e, unsigned mask)
{
    if (!e || e->pad_raw <= 0) return;
    mask &= 0xFFFFu;
    int held = __builtin_popcount(mask);
    if (held > 0 && e->pad_held == 0 && e->notes_held == 0) e->held_clock = 0.0;      /* the first cut starts the clip */
    if (held > e->pad_held) e->last_vel = 100;
    e->pad_cuts = mask;
    e->pad_held = held;
}

/* ------------------------------------------------------------------ */
/* Samples                                                             */
/* ------------------------------------------------------------------ */

static void display_name(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    for (; *in && n + 1 < cap && n < 11; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '.') break;                                                  /* drop the extension */
        out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : ((c >= 32 && c < 127 && c != 34 && c != 92) ? (char)c : '_');
    }
    out[n] = 0;
}

/* The overview the screen draws (peak and body per column), the extent of the sound, and the gain that brings it to -1 dBFS. */
static void analyse(ts_slot_t *s, const float *buf, int frames)
{
    float peak = 0.0f;
    for (int i = 0; i < frames; i++) peak = fmaxf(peak, fabsf(buf[i]));
    for (int c = 0; c < TS_VIEW_COLS; c++) {
        int a = (int)((long long)frames * c / TS_VIEW_COLS), b = (int)((long long)frames * (c + 1) / TS_VIEW_COLS);
        if (b <= a) b = a + 1;
        float m = 0.0f;
        double sq = 0.0;
        int cnt = 0;
        for (int i = a; i < b && i < frames; i++) { m = fmaxf(m, fabsf(buf[i])); sq += (double)buf[i] * buf[i]; cnt++; }
        float inv = peak > 1e-6f ? 1.0f / peak : 0.0f;
        s->pk[c] = (uint8_t)clampi((int)(255.0f * fminf(m * inv, 1.0f) + 0.5f), 0, 255);
        s->rms[c] = (uint8_t)clampi((int)(255.0f * fminf((cnt ? (float)sqrt(sq / cnt) : 0.0f) * inv, 1.0f) + 0.5f), 0, 255);
    }
    int first = 0, last = frames - 1;                                      /* the sound's extent: clips stay inside it */
    if (peak > 1e-4f) {
        while (first < frames - 1 && fabsf(buf[first]) < 0.04f * peak) first++;
        while (last > first && fabsf(buf[last]) < 0.04f * peak) last--;
    }
    s->lo = (float)first / (float)frames;
    s->hi = (float)(last + 1) / (float)frames;
    if (s->hi - s->lo < 0.1f) { s->lo = 0.0f; s->hi = 1.0f; }
    s->norm = peak > 1e-4f ? 0.891f / peak : 1.0f;                       /* -1 dBFS */
}

int ts_engine_load_wav_mem(ts_engine_t *e, int slot, const char *name, const uint8_t *d, size_t n)
{
    if (!e || slot < 0 || slot >= TS_SLOTS) return TS_WAV_ERR_FORMAT;
    int cap = (int)(TS_MAX_SECONDS * e->sr);
    int frames = ts_wav_decode(d, n, NULL, cap, e->sr);          /* a dry run sizes it exactly */
    if (frames < 0) return frames;
    float *buf = (float *)TS_CALLOC((size_t)frames * sizeof(float));
    if (!buf) return TS_WAV_ERR_IO;
    int r = ts_wav_decode(d, n, buf, frames, e->sr);
    if (r != frames) { TS_FREE(buf); return r < 0 ? r : TS_WAV_ERR_FORMAT; }

    ts_slot_t *s = &e->slot[slot];
    analyse(s, buf, frames);
    float *old = s->data;
    s->data = buf;
    s->len = frames;
    display_name(s->name, sizeof s->name, name && *name ? name : "SAMPLE");
    s->loaded = 1;
    TS_FREE(old);
    return TS_WAV_OK;
}

static int load_file(ts_engine_t *e, int slot, const char *dir, const char *file)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return TS_WAV_ERR_IO;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 44 || size > 16L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return TS_WAV_ERR_IO; }
    uint8_t *buf = (uint8_t *)TS_CALLOC((size_t)size);
    if (!buf) { fclose(f); return TS_WAV_ERR_IO; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    int r = got == (size_t)size ? ts_engine_load_wav_mem(e, slot, file, buf, got) : TS_WAV_ERR_IO;
    TS_FREE(buf);
    if (r == TS_WAV_OK) {
        snprintf(e->slot[slot].file, sizeof e->slot[slot].file, "%s", file);
        if (slot >= 2 && strlen(file) == 9 && !strncmp(file, "user", 4) && file[4] == '1' + (slot - 2) && !strcmp(file + 5, ".wav"))
            snprintf(e->slot[slot].name, sizeof e->slot[slot].name, "USER %d", slot - 1);       /* what you recorded */
    }
    return r;
}

/* Files in the samples folder that were not made here (a wav you copied in) are never deleted: unloading one only adds its name to
 * unloaded.txt, and the next start leaves it alone. */
static int unloaded_listed(const ts_engine_t *e, const char *name)
{
    if (!e->save_dir[0]) return 0;
    char path[600], line[128];
    snprintf(path, sizeof path, "%s/unloaded.txt", e->save_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof line, f) && !found) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == 10 || line[n - 1] == 13)) line[--n] = 0;
        found = n && strcmp(line, name) == 0;
    }
    fclose(f);
    return found;
}

static void unloaded_add(const ts_engine_t *e, const char *name)
{
    if (!e->save_dir[0] || !name[0] || unloaded_listed(e, name)) return;
    char path[600];
    snprintf(path, sizeof path, "%s/unloaded.txt", e->save_dir);
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s%c", name, 10);
    fclose(f);
}

static int is_wav(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char *x = name + n - 4;
    return x[0] == '.' && (x[1] | 32) == 'w' && (x[2] | 32) == 'a' && (x[3] | 32) == 'v';
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int ts_engine_load_dir(ts_engine_t *e, const char *dir)
{
    if (!e || !dir || !*dir) return 0;
    int mask = 0;
    if (!e->slot[0].loaded && load_file(e, 0, dir, "ahhh.wav") == TS_WAV_OK) mask |= 1;
    if (!e->slot[1].loaded && load_file(e, 1, dir, "fresssh.wav") == TS_WAV_OK) mask |= 2;
    for (int u = 0; u < 4; u++) {                                              /* user1.wav .. user4.wav: User 1 .. 4 */
        char nm[16];
        snprintf(nm, sizeof nm, "user%d.wav", u + 1);
        if (!e->slot[2 + u].loaded && load_file(e, 2 + u, dir, nm) == TS_WAV_OK) mask |= 1 << (2 + u);
    }

    char names[64][TS_NAME_LEN];
    int n = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && n < 64) {
            const char *nm = de->d_name;
            if (!is_wav(nm) || strlen(nm) >= TS_NAME_LEN || !strcmp(nm, "ahhh.wav") || !strcmp(nm, "fresssh.wav") || unloaded_listed(e, nm)) continue;
            if (strlen(nm) == 9 && !strncmp(nm, "user", 4) && nm[4] >= '1' && nm[4] <= '4' && !strcmp(nm + 5, ".wav")) continue;      /* those have their slots */
            strcpy(names[n++], nm);
        }
        closedir(d);
    }
    qsort(names, (size_t)n, TS_NAME_LEN, cmp_names);
    int slot = 2;
    for (int i = 0; i < n && slot < TS_SLOTS; i++) {
        int already = 0;
        for (int k = 0; k < TS_SLOTS; k++) already |= e->slot[k].loaded && strcmp(e->slot[k].file, names[i]) == 0;
        if (already) continue;                                                  /* the same file found again by another folder */
        while (slot < TS_SLOTS && e->slot[slot].loaded) slot++;                  /* the first directory with samples wins */
        if (slot >= TS_SLOTS) break;
        if (load_file(e, slot, dir, names[i]) == TS_WAV_OK) { mask |= 1 << slot; slot++; }
    }
    return mask;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

static int save_wav(const char *path, const float *d, int n, int sr)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t bytes = (uint32_t)n * 2u;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36u + bytes;
    h[4] = (uint8_t)v; h[5] = (uint8_t)(v >> 8); h[6] = (uint8_t)(v >> 16); h[7] = (uint8_t)(v >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;                                   /* PCM, mono */
    v = (uint32_t)sr;
    h[24] = (uint8_t)v; h[25] = (uint8_t)(v >> 8); h[26] = (uint8_t)(v >> 16); h[27] = (uint8_t)(v >> 24);
    v = (uint32_t)sr * 2u;
    h[28] = (uint8_t)v; h[29] = (uint8_t)(v >> 8); h[30] = (uint8_t)(v >> 16); h[31] = (uint8_t)(v >> 24);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)bytes; h[41] = (uint8_t)(bytes >> 8); h[42] = (uint8_t)(bytes >> 16); h[43] = (uint8_t)(bytes >> 24);
    int ok = fwrite(h, 1, 44, f) == 44;
    for (int i = 0; i < n && ok; i += 1024) {
        int16_t blk[1024];
        int m = n - i < 1024 ? n - i : 1024;
        for (int k = 0; k < m; k++) {
            float x = d[i + k] * 32767.0f;
            blk[k] = (int16_t)(x > 32767.0f ? 32767 : (x < -32768.0f ? -32768 : x));
        }
        ok = fwrite(blk, 2, (size_t)m, f) == (size_t)m;
    }
    fclose(f);
    return ok ? 0 : -1;
}

void ts_engine_set_save_dir(ts_engine_t *e, const char *dir)
{
    if (!e) return;
    snprintf(e->save_dir, sizeof e->save_dir, "%s", dir ? dir : "");
}

int ts_engine_rec_start(ts_engine_t *e)
{
    if (!e || !e->rec_buf) return -1;
    int slot = e->p.sample;
    if (slot < 2) {
        slot = -1;
        for (int i = 2; i < TS_SLOTS && slot < 0; i++) if (!e->slot[i].loaded) slot = i;
        if (slot < 0) slot = 2;
    }
    e->rec_state = 0;
    e->rec_slot = slot;
    e->rec_len = 0;
    e->in_peak = 0.0f;
    ts_engine_select_sample(e, slot);
    __sync_synchronize();
    e->rec_state = 1;
    return slot;
}

void ts_engine_rec_stop(ts_engine_t *e)
{
    if (e && e->rec_state == 1) e->rec_state = 2;
}

void ts_engine_feed_input(ts_engine_t *e, const float *mono, int n)
{
    if (!e || !mono || n <= 0) return;
    float pk = 0.0f;
    for (int i = 0; i < n; i++) { float a = fabsf(mono[i]); if (a > pk) pk = a; }
    e->in_peak = pk > e->in_peak * 0.92f ? pk : e->in_peak * 0.92f;           /* a level meter that falls back */
    if (e->rec_state != 1 || !e->rec_buf) return;
    int room = e->rec_cap - e->rec_len, k = n < room ? n : room;
    if (k > 0) { memcpy(e->rec_buf + e->rec_len, mono, (size_t)k * sizeof(float)); e->rec_len += k; }
    if (e->rec_len >= e->rec_cap) { __sync_synchronize(); e->rec_state = 2; }
}

int ts_engine_load_file(ts_engine_t *e, const char *path)
{
    if (!e || !path || !*path || e->pend_ready) return -1;
    int slot = e->p.sample;
    if (slot < 2) {
        slot = -1;
        for (int i = 2; i < TS_SLOTS && slot < 0; i++) if (!e->slot[i].loaded) slot = i;
        if (slot < 0) slot = 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 44 || size > 16L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -3; }
    uint8_t *raw = (uint8_t *)TS_CALLOC((size_t)size);
    if (!raw) { fclose(f); return -4; }
    size_t got = fread(raw, 1, (size_t)size, f);
    fclose(f);
    int cap = (int)((float)TS_MAX_SECONDS * e->sr);
    int frames = got == (size_t)size ? ts_wav_decode(raw, got, NULL, cap, e->sr) : TS_WAV_ERR_IO;
    float *data = frames > 0 ? (float *)TS_CALLOC((size_t)frames * sizeof(float)) : NULL;
    if (data && ts_wav_decode(raw, got, data, frames, e->sr) != frames) { TS_FREE(data); data = NULL; }
    TS_FREE(raw);
    if (!data) return -5;
    ts_slot_t t;
    memset(&t, 0, sizeof t);
    analyse(&t, data, frames);
    t.data = data;
    t.len = frames;
    t.loaded = 1;
    const char *base = path, *sl = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (sl && sl + 1 > base) base = sl + 1;
    if (bs && bs + 1 > base) base = bs + 1;
    display_name(t.name, sizeof t.name, *base ? base : "SAMPLE");
    snprintf(t.file, sizeof t.file, "user%d.wav", slot - 1);
    if (e->save_dir[0]) {                                                       /* kept, so it is there next time */
        char out[600];
        snprintf(out, sizeof out, "%s/user%d.wav", e->save_dir, slot - 1);
        save_wav(out, data, frames, (int)e->sr);
    }
    e->pend = t;
    e->pend_slot = slot;
    ts_engine_select_sample(e, slot);
    __sync_synchronize();
    e->pend_ready = 1;
    return slot;
}

void ts_engine_clear_slot(ts_engine_t *e, int slot)
{
    if (e && slot >= 2 && slot < TS_SLOTS) e->clear_req = slot + 1;
}

void ts_engine_service(ts_engine_t *e)
{
    if (!e) return;
    if (e->recipes_dirty) { e->recipes_dirty = 0; save_recipes(e); }               /* the kept clips changed: write the library */
    for (int i = 0; i < TS_SLOTS + 2; i++) {                                    /* what the render thread replaced */
        float *g = __atomic_exchange_n(&e->garbage[i], NULL, __ATOMIC_ACQ_REL);
        TS_FREE(g);
    }
    if (e->pend_ready) return;                                                  /* the last one has not been swapped in yet */

    int req = e->clear_req;
    if (req > 0) {
        e->clear_req = 0;
        int slot = req - 1;
        if (slot >= 2 && slot < TS_SLOTS && e->slot[slot].loaded) {
            for (int pad = 1; pad <= TS_NUM_CLIPS; pad++) drop_kept(e, slot, pad);   /* the clips kept for it go with it: the next sample starts clean */
            memset(e->removed[slot], 0, sizeof e->removed[slot]);
            char was[TS_NAME_LEN];                                               /* (read before the render thread swaps the empty slot in) */
            snprintf(was, sizeof was, "%s", e->slot[slot].file);
            memset(&e->pend, 0, sizeof e->pend);                                 /* a slot with nothing in it */
            e->pend_slot = slot;
            __sync_synchronize();
            e->pend_ready = 1;
            if (e->save_dir[0]) {
                char ours[32], path[600];
                snprintf(ours, sizeof ours, "user%d.wav", slot - 1);
                if (!was[0] || strcmp(was, ours) == 0) {          /* made here (a recording, a file you picked): its copy goes */
                    snprintf(path, sizeof path, "%s/%s", e->save_dir, ours);
                    remove(path);
                } else {
                    unloaded_add(e, was);                                                       /* a file you put there: kept, and not loaded again */
                }
            }
        }
        return;
    }

    if (e->rec_state == 2) {
        int n = e->rec_len, slot = e->rec_slot;
        if (n >= TS_REC_MIN && slot >= 2 && slot < TS_SLOTS) {
            float *buf = (float *)TS_CALLOC((size_t)n * sizeof(float));
            if (buf) {
                memcpy(buf, e->rec_buf, (size_t)n * sizeof(float));
                ts_slot_t t;
                memset(&t, 0, sizeof t);
                analyse(&t, buf, n);
                t.data = buf;
                t.len = n;
                t.loaded = 1;
                snprintf(t.name, sizeof t.name, "USER %d", slot - 1);
                snprintf(t.file, sizeof t.file, "user%d.wav", slot - 1);
                if (e->save_dir[0]) {
                    char path[600];
                    snprintf(path, sizeof path, "%s/user%d.wav", e->save_dir, slot - 1);
                    save_wav(path, buf, n, (int)e->sr);
                }
                e->pend = t;
                e->pend_slot = slot;
                __sync_synchronize();
                e->pend_ready = 1;
            }
        }
        e->rec_state = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

static float read_hermite(const float *d, int len, double pos)
{
    double lo = 1.0, hi = (double)(len - 3);
    if (hi < lo) return len > 0 ? d[0] : 0.0f;
    if (!(pos >= lo)) pos = lo;
    if (pos > hi) pos = hi;
    int i = (int)pos;
    float f = (float)(pos - (double)i);
    float xm1 = d[i - 1], x0 = d[i], x1 = d[i + 1], x2 = d[i + 2];
    float c = 0.5f * (x1 - xm1);
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + 0.5f * (x2 - x0);
    float b = w + a;
    return ((a * f - b) * f + c) * f + x0;
}

void ts_engine_render(ts_engine_t *e, float *out, int frames)
{
    if (!e || !out || frames <= 0) return;
    e->view_seq++;
    if (e->pend_ready) {                                              /* a slot the service call built: swapped in here, never while it is being read */
        __sync_synchronize();
        ts_slot_t *d = &e->slot[e->pend_slot];
        float *old = d->data;
        *d = e->pend;
        memset(&e->pend, 0, sizeof e->pend);
        if (old) {                                                    /* freed by the service call: not here */
            int parked = 0;
            for (int i = 0; i < TS_SLOTS + 2 && !parked; i++)
                if (__atomic_load_n(&e->garbage[i], __ATOMIC_ACQUIRE) == NULL) { __atomic_store_n(&e->garbage[i], old, __ATOMIC_RELEASE); parked = 1; }
        }
        __sync_synchronize();
        e->pend_ready = 0;
    }
    const ts_params_t *p = &e->p;
    const ts_slot_t *s = &e->slot[p->sample];                          /* an empty slot is silent */
    const int len = s->loaded ? s->len : 0;

    const float drag = p->drag;
    const float tau = (0.003f + 0.08f * drag * drag) * e->sr;                // 3 .. 83 ms
    const float alpha = 1.0f - expf(-1.0f / tau);
    const float ramp = 1.0f / (0.002f * e->sr);                             // a cut takes 2 ms
    const float gain = powf(10.0f, p->gain_db / 20.0f) * (p->normalise && s->loaded ? s->norm : 1.0f);
    const double spb = (double)e->bpm / (60.0 * (double)e->sr);
    if (e->pad_raw > 0) {                                                   /* the Pads screen has to keep saying it is there */
        e->pad_raw -= frames;
        if (e->pad_raw <= 0) { e->pad_raw = 0; e->pad_cuts = 0; e->pad_held = 0; }
    }

    for (int i = 0; i < frames; i++) {
        e->free_clock += spb;
        e->clock += spb;
        /* The clips and the cuts keep to one musical clock. With Lock on it is the transport's, and while that is
         * stopped nothing moves by itself: a held cut starts the clip from its beginning, and letting go stops it. */
        const int running = !p->lock || e->transport;
        const int held = e->notes_held > 0 || e->pad_held > 0;
        if (!p->lock) e->mclock = e->free_clock;
        else if (e->transport) e->mclock = e->clock;
        else if (held) { e->held_clock += spb; e->mclock = e->held_clock; }
        e->running = running;
        const int live = running || held;
        int clip = p->clip;
        if (clip == TS_CLIP_AUTO) {                                         /* a random other clip each bar, kept for the bar */
            long long bar = (long long)floor(e->mclock / (double)TS_LANE_BEATS);
            if (bar != e->auto_bar || e->auto_sample != p->sample || e->auto_nc != (int)e->clips_ver) {
                e->auto_bar = bar; e->auto_sample = p->sample; e->auto_nc = (int)e->clips_ver;
                e->auto_clip = ts_clip_auto(e, bar);
            }
            clip = e->auto_clip;
            e->clip_now = clip;
        } else if (clip > 0) {
            e->clip_now = clip;
        }
        const ts_clip_t *ck = clip_at(e, clip);
        if (!ck) clip = 0;
        /* a clip made for this sample says where in it; any other is spread over where this sample has its sound */
        const int native = ck && ck->tag != 0 && (ck->tag == sample_tag(p->sample) || ck->tag == TAG_GENERATED);

        if (e->hand_hold > 0) e->hand_hold--;
        if (e->hand_hold > 0 || !live) e->target = e->man;
        else if (clip > 0) {
            double v = (double)ts_clip_pos(e, clip, e->mclock) * (double)p->clip_depth + (double)p->clip_offset;     /* the Depth and Offset dials */
            v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
            e->target = native ? v : (double)s->lo + (double)(s->hi - s->lo) * v;
        }
        else e->target = e->man;
        double before = e->pos;
        e->pos += (double)alpha * (e->target - e->pos);
        e->vel_out = len > 0 ? (float)((e->pos - before) * (double)(len - 1)) : 0.0f;

        float smp = 0.0f;
        if (len > 0) smp = read_hermite(s->data, len, e->pos * (double)(len - 1));

        /* open: the clip says so while it runs, and a held cutting note or pad shuts it (the pads are the fader on top of the clip);
         * with no clip a held note opens the sound, and Note Gate off means "always heard". */
        int open;
        if (running && clip > 0) open = ts_clip_cut(e, clip, e->mclock) && !(p->note_gate && held);        /* the pads are the fader: hold one to cut the clip out */
        else if (live && clip > 0) open = ts_clip_cut(e, clip, e->mclock);          /* a pad held with the transport stopped runs the clip, cuts and all */
        else open = !p->note_gate || held;
        float want = open ? 1.0f : 0.0f;
        if (e->gate < want) { e->gate += ramp; if (e->gate > want) e->gate = want; }
        else if (e->gate > want) { e->gate -= ramp; if (e->gate < want) e->gate = want; }
        float vol = p->vel_to_vol ? (float)e->last_vel * (1.0f / 127.0f) : 1.0f;

        out[i] = smp * e->gate * vol * gain;

        /* the clip view's cursor, and with no clip the record of what the playhead did, over whatever the last pass left */
        const int lane_len = clip > 0 ? ck->len : TS_LANE_BEATS;
        e->lane_len = lane_len;
        int col = clampi((int)(fmod(e->mclock, (double)lane_len) * (double)TS_LANE / (double)lane_len), 0, TS_LANE - 1);
        if (!live || clip > 0) {
            e->lane_col = col;
        } else if (col != e->lane_col) {
            e->lane_col = col;
            e->lane_pos[col] = (uint8_t)clampi((int)(e->pos * 15.0 + 0.5), 0, 15);
            e->lane_gate[col] = 0;
        }
        if (live && clip == 0 && e->gate > 0.5f) e->lane_gate[col] = 1;
    }
}

/* ------------------------------------------------------------------ */
/* What the screen shows                                               */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; int cap, len; } jw_t;

static void jw(jw_t *j, const char *fmt, ...)
{
    if (j->len < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, (size_t)(j->cap - j->len), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= j->cap - j->len) j->len = -1;
    else j->len += n;
}

int ts_engine_view(const ts_engine_t *e, char *buf, int cap)
{
    if (!e || !buf || cap < 2) return -1;
    jw_t j = { buf, cap, 0 };
    const ts_params_t *p = &e->p;
    const ts_slot_t *s = &e->slot[p->sample];
    const int shown = p->sample;
    int x = clampi((int)(e->pos * (TS_VIEW_COLS - 1) + 0.5), 0, TS_VIEW_COLS - 1);
    int tx = clampi((int)(e->target * (TS_VIEW_COLS - 1) + 0.5), 0, TS_VIEW_COLS - 1);
    const ts_clip_t *shown_clip = p->clip ? clip_at(e, e->clip_now) : NULL;
    const int lane_beats = shown_clip ? shown_clip->len : TS_LANE_BEATS;

    jw(&j, "{\"v\":3,\"n\":\"%s\",\"s\":%d,\"sn\":\"", s->loaded ? s->name : "EMPTY", shown);
    for (int i = 0; i < TS_SLOTS; i++) {
        const ts_slot_t *t = &e->slot[i];
        char fallback[16];
        snprintf(fallback, sizeof fallback, "USER %d", i - 1);
        jw(&j, "%s%s", i ? "|" : "", t->loaded ? t->name : (i == 0 ? "AHHH" : (i == 1 ? "FRESSSH" : fallback)));
    }
    jw(&j, "\",\"ld\":\"");
    for (int i = 0; i < TS_SLOTS; i++) jw(&j, "%c", e->slot[i].loaded ? '1' : '0');
    jw(&j, "\",\"x\":%d,\"tx\":%d,\"p\":%.4f,\"tt\":%.3f,\"vl\":%.2f,\"dr\":%.2f,\"gn\":%.1f,\"ng\":%d,\"vv\":%d,\"no\":%d,\"lk\":%d,"
           "\"cn\":%d,\"g\":%d,\"nh\":%d,\"tp\":%d,\"lcol\":%d,\"cl\":%d,\"cm\":%d,\"ru\":%d,\"ln\":%d,\"rw\":%d,\"lb\":%d,\"nc\":%d,\"rc\":%d,\"rt\":%d,\"rl\":%d,\"tv\":%d,\"tg\":%d,\"sq\":%u,\"cd\":%.2f,\"co\":%.2f,\"gs\":%d,\"gb\":%d,\"w\":\"",
       x, tx, e->pos, (double)p->turntable, (double)e->vel_out, (double)p->drag, (double)p->gain_db, p->note_gate, p->vel_to_vol, p->normalise, p->lock,
       p->cut_from, e->gate > 0.5f, e->notes_held + e->pad_held, e->transport, e->lane_col, p->clip ? e->clip_now : 0, p->clip, e->running, e->last_note, e->pad_raw > 0, lane_beats, e->nclips,
       e->rec_state == 1, e->rec_state == 1 ? (int)((float)e->rec_len * 10.0f / e->sr) : 0, clampi((int)(e->in_peak * 100.0f + 0.5f), 0, 100),
       e->try_valid, e->try_valid ? e->try_recipe.style : 0, e->view_seq,
       (double)p->clip_depth, (double)p->clip_offset, p->gen_style, p->gen_bars);
    for (int c = 0; c < TS_VIEW_COLS && j.len >= 0; c++) jw(&j, "%02x", s->loaded ? s->pk[c] : 0);       /* the waveform: peak, then body */
    jw(&j, "\",\"wr\":\"");
    for (int c = 0; c < TS_VIEW_COLS && j.len >= 0; c++) jw(&j, "%02x", s->loaded ? s->rms[c] : 0);

    jw(&j, "\",\"lnp\":\"");                                                   /* the clip view: playhead, then cuts, a bar of columns */
    const ts_clip_t *pk = shown_clip;                                          /* with a clip the view is the plan, like the clip in Ableton */
    for (int k = 0; k < TS_LANE && j.len >= 0; k++)
        jw(&j, "%x", pk ? clampi((int)(ts_clip_pos(e, e->clip_now, ((double)k + 0.5) * pk->len / TS_LANE) * 15.0f + 0.5f), 0, 15) : e->lane_pos[k]);
    jw(&j, "\",\"lng\":\"");
    for (int k = 0; k < TS_LANE && j.len >= 0; k++) {
        int on = pk ? 0 : e->lane_gate[k];
        if (pk) {                                                              /* a note anywhere in the column cuts it in */
            int a = k * pk->n / TS_LANE, b = (k + 1) * pk->n / TS_LANE;
            for (int i = a; i < b && !on; i++) on = pk->gate[i];
        }
        jw(&j, "%c", on ? '1' : '0');
    }
    jw(&j, "\",\"cf\":\"");                                                   /* which clip pads have a clip, and which of those you made */
    for (int k = 0; k < TS_NUM_CLIPS && j.len >= 0; k++) jw(&j, "%c", bank_clip(e, p->sample, k + 1) ? '1' : '0');
    jw(&j, "\",\"kp\":\"");
    for (int k = 0; k < TS_NUM_CLIPS && j.len >= 0; k++) jw(&j, "%c", (e->bank[p->sample][k] > 0 && e->kept[e->bank[p->sample][k] - 1].used) ? '1' : '0');
    jw(&j, "\",\"ro\":\"");                                                  /* and which are read-only (imported) */
    for (int k = 0; k < TS_NUM_CLIPS && j.len >= 0; k++) jw(&j, "%c", (e->bank[p->sample][k] > 0 && e->ro[e->bank[p->sample][k] - 1]) ? '1' : '0');
    jw(&j, "\",\"zm\":%d", e->zoom);
    if (e->zoom > 0) {                       /* the zoomed view: the next few beats of the clip, round the clock, a column each */
        const double w = e->zoom == 1 ? 4.0 : (e->zoom == 2 ? 2.0 : 1.0), start = e->mclock - 0.25 * w;
        char gates[TS_LANE + 1];
        jw(&j, ",\"zs\":%.3f,\"zw\":%.1f,\"zp\":\"", start, w);
        for (int c = 0; c < TS_LANE && j.len >= 0; c++) {
            double beat = start + ((double)c + 0.5) * w / (double)TS_LANE;
            int clip = p->clip == TS_CLIP_AUTO ? ts_clip_auto(e, (long long)floor(beat / (double)TS_LANE_BEATS)) : p->clip;     /* in auto, the next bar's clip when the window reaches it */
            jw(&j, "%02x", clip > 0 ? clampi((int)(ts_clip_pos(e, clip, beat) * 255.0f + 0.5f), 0, 255) : 0);
            gates[c] = clip > 0 && ts_clip_cut(e, clip, beat) ? '1' : '0';
        }
        gates[TS_LANE] = 0;
        jw(&j, "\",\"zg\":\"%s\"", gates);
    }
    jw(&j, "}");
    if (j.len < 0) buf[0] = 0;
    return j.len;
}
