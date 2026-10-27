/* TinyScratch unit tests. Single translation unit: the engine and plugin glue are #included so every
 * allocation goes through the checking allocator below (redzones + leak accounting), which keeps working
 * where ASan is unavailable.
 *
 * Usage: test_engine [repo_root]   (default ".") */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

/* ---- checking allocator ------------------------------------------------ */

#define RZ 64
static atomic_size_t g_allocs, g_frees, g_live;
static atomic_int g_fail;
static int g_checks;

static void *t_calloc(size_t n)
{
    uint8_t *raw = (uint8_t *)calloc(1, RZ + n + RZ);
    if (!raw) return NULL;
    memcpy(raw, &n, sizeof n);
    memset(raw + sizeof n, 0xAB, RZ - sizeof n);
    memset(raw + RZ + n, 0xAB, RZ);
    g_allocs++;
    g_live += n;
    return raw + RZ;
}

static void t_free(void *p)
{
    if (!p) return;
    uint8_t *raw = (uint8_t *)p - RZ;
    size_t n;
    memcpy(&n, raw, sizeof n);
    for (size_t i = sizeof n; i < RZ; i++)
        if (raw[i] != 0xAB) { fprintf(stderr, "  REDZONE underrun at %p\n", p); g_fail++; break; }
    for (size_t i = 0; i < RZ; i++)
        if (raw[RZ + n + i] != 0xAB) { fprintf(stderr, "  REDZONE overrun at %p\n", p); g_fail++; break; }
    g_frees++;
    g_live -= n;
    memset(raw, 0xDD, RZ + n + RZ);
    free(raw);
}

#define TS_CALLOC(n) t_calloc(n)
#define TS_FREE(p) t_free(p)

#define TS_BUILTINS_ON_PADS 1          /* the tests are written round the eight hand-written clips on Ahhh's and Fresssh's pads */
#include "../src/scratch_engine.c"
#include "../src/schwung_plugin.c"

/* ---- mini harness ------------------------------------------------------ */

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_fail++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
        }                                                                  \
    } while (0)

static const char *g_root = ".";
#define TDIR "build/test"

static uint8_t g_wav[1 << 20];

/* little-endian WAV builder; pcm may be NULL for zero-filled data */
static size_t make_wav(uint8_t *out, const int16_t *pcm, int frames, int sr, int ch, int bits, int tag)
{
    size_t bytes = (size_t)frames * (size_t)ch * (size_t)(bits / 8);
    uint8_t *o = out;
#define W32(v) do { uint32_t x = (uint32_t)(v); *o++ = x; *o++ = x >> 8; *o++ = x >> 16; *o++ = x >> 24; } while (0)
#define W16(v) do { uint32_t x = (uint32_t)(v); *o++ = x; *o++ = x >> 8; } while (0)
    memcpy(o, "RIFF", 4); o += 4; W32(36 + bytes);
    memcpy(o, "WAVEfmt ", 8); o += 8; W32(16);
    W16(tag); W16(ch); W32(sr); W32(sr * ch * bits / 8); W16(ch * bits / 8); W16(bits);
    memcpy(o, "data", 4); o += 4; W32(bytes);
#undef W32
#undef W16
    if (pcm) memcpy(o, pcm, bytes); else memset(o, 0, bytes);
    return (size_t)(o - out) + bytes;
}

enum { SMP_RAMP, SMP_SINE, SMP_DC };

/* a ramp from -amp to +amp, a 440 Hz sine, or a constant */
static void make_sample(int16_t *pcm, int n, int kind, float amp)
{
    for (int i = 0; i < n; i++) {
        float v = kind == SMP_RAMP ? amp * (2.0f * (float)i / (float)(n - 1) - 1.0f)
                : kind == SMP_SINE ? amp * sinf(6.2831853f * 440.0f * (float)i / 44100.0f) : amp;
        pcm[i] = (int16_t)(v * 32767.0f);
    }
}

static int load_sample(ts_engine_t *e, int slot, const char *name, int kind, float amp, int n)
{
    static int16_t pcm[44100 * 4];
    make_sample(pcm, n, kind, amp);
    size_t sz = make_wav(g_wav, pcm, n, 44100, 1, 16, 1);
    return ts_engine_load_wav_mem(e, slot, name, g_wav, sz);
}

static void render_n(ts_engine_t *e, float *out, int n)
{
    for (int i = 0; i < n; i += 128) ts_engine_render(e, out + i, n - i < 128 ? n - i : 128);
}

static double rms_of(const float *x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / (n > 0 ? n : 1));
}

static float peak_of(const float *x, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; i++) m = fmaxf(m, fabsf(x[i]));
    return m;
}

/* an engine with one sample in slot 0, plain notes, the cut gate open and the loudness untouched: just the playhead */
static ts_engine_t *plain_engine(int kind, float amp)
{
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "test.wav", kind, amp, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.note_gate = 0; p.normalise = 0; p.drag = 0.0f; p.pads = 0; p.clip = 0;
    ts_engine_set_params(e, &p);
    return e;
}

static int balanced(const char *s)
{
    int d = 0, q = 0;
    for (; *s; s++) {
        if (*s == '"' && (s[-1] != '\\')) q = !q;
        if (q) continue;
        if (*s == '{' || *s == '[') d++;
        if (*s == '}' || *s == ']') d--;
        if (d < 0) return 0;
    }
    return d == 0 && !q;
}

/* ---- 1. WAV decoding ---------------------------------------------------- */

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s ? *s : 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static float rng_f(uint32_t *s) { return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f); }

static void test_wav(void)
{
    printf("wav decoding\n");
    static int16_t pcm[44100];
    make_sample(pcm, 1000, SMP_SINE, 0.5f);
    size_t sz = make_wav(g_wav, pcm, 1000, 44100, 1, 16, 1);
    static float out[3000];
    CHECK(ts_wav_decode(g_wav, sz, NULL, 3000, 44100.0f) == 1000, "dry run gives the frame count");
    CHECK(ts_wav_decode(g_wav, sz, out, 3000, 44100.0f) == 1000 && fabsf(out[10] - pcm[10] / 32768.0f) < 1e-6f, "decodes");
    sz = make_wav(g_wav, pcm, 1000, 22050, 1, 16, 1);
    CHECK(ts_wav_decode(g_wav, sz, out, 3000, 44100.0f) == 2000, "22.05 kHz is resampled to 44.1 kHz");
    CHECK(ts_wav_decode(g_wav, sz, out, 500, 44100.0f) == 500, "the capacity caps the length");
    static int16_t st[2000];
    for (int i = 0; i < 1000; i++) { st[2 * i] = pcm[i]; st[2 * i + 1] = (int16_t)-pcm[i] / 2; }       /* left, and right a half-strength opposite */
    sz = make_wav(g_wav, st, 1000, 44100, 2, 16, 1);
    CHECK(ts_wav_decode(g_wav, sz, NULL, 3000, 44100.0f) == 1000 && ts_wav_decode(g_wav, sz, out, 3000, 44100.0f) == 1000, "stereo gives the frame count, not the sample count");
    CHECK(fabsf(out[10] - 0.5f * (st[20] + st[21]) / 32768.0f) < 1e-6f && fabsf(out[300] - 0.5f * (st[600] + st[601]) / 32768.0f) < 1e-6f, "...mixed down to mono (the mean of left and right)");
    sz = make_wav(g_wav, st, 600, 44100, 3, 16, 1);
    CHECK(ts_wav_decode(g_wav, sz, NULL, 3000, 44100.0f) == TS_WAV_ERR_UNSUPPORTED, "three channels are refused");
    sz = make_wav(g_wav, pcm, 500, 44100, 1, 8, 1);
    CHECK(ts_wav_decode(g_wav, sz, NULL, 3000, 44100.0f) == TS_WAV_ERR_UNSUPPORTED, "8-bit is refused");
    sz = make_wav(g_wav, pcm, 10, 44100, 1, 16, 1);
    CHECK(ts_wav_decode(g_wav, sz, NULL, 3000, 44100.0f) == TS_WAV_ERR_SHORT, "too short");
    sz = make_wav(g_wav, pcm, 1000, 44100, 1, 16, 3);
    CHECK(ts_wav_decode(g_wav, sz, NULL, 3000, 44100.0f) == TS_WAV_ERR_UNSUPPORTED, "float is refused");
    CHECK(ts_wav_decode(g_wav, 20, NULL, 3000, 44100.0f) == TS_WAV_ERR_FORMAT && ts_wav_decode(NULL, 100, NULL, 3000, 44100.0f) == TS_WAV_ERR_FORMAT &&
          ts_wav_decode(g_wav, sz, NULL, 0, 44100.0f) == TS_WAV_ERR_FORMAT, "garbage and bad arguments");
    sz = make_wav(g_wav, pcm, 1000, 44100, 1, 16, 1);
    g_wav[40] = 0xFF; g_wav[41] = 0xFF; g_wav[42] = 0xFF; g_wav[43] = 0x7F;    /* a data size that lies */
    CHECK(ts_wav_decode(g_wav, sz, out, 3000, 44100.0f) == 1000, "a data chunk longer than the file is read as far as it goes");
    for (size_t n = 0; n < sz; n += 7) {                                      /* every truncation: never reads past the end */
        uint8_t *c = (uint8_t *)TS_CALLOC(n ? n : 1);
        memcpy(c, g_wav, n);
        ts_wav_decode(c, n, out, 3000, 44100.0f);
        TS_FREE(c);
    }
}

/* ---- 2. parameters ------------------------------------------------------- */

static void test_defaults(void)
{
    printf("parameters\n");
    ts_params_t p;
    ts_params_default(&p);
    CHECK(p.sample == 0 && p.turntable == 0.0f && p.drag == 0.5f && p.gain_db == 0.0f && p.note_gate == 1 && p.vel_to_vol == 0 && p.normalise == 1 &&
          p.lock == 1 && p.clip == TS_CLIP_AUTO && p.cut_from == 60 && p.pads == 1,
          "the defaults (drag at the sweet spot, gain 0 dB, note gate and normalise on, cut from C3), with the clips scratching");

    ts_params_t w;
    memset(&w, 0, sizeof w);
    w.sample = 99; w.turntable = NAN; w.drag = 9.0f; w.gain_db = -500.0f; w.note_gate = 7; w.cut_from = 999; w.pads = 5; w.lock = 3; w.clip_depth = INFINITY; w.clip_offset = NAN;
    ts_params_sanitize(&w);
    CHECK(w.sample == 7 && w.turntable == 0.0f && w.drag == 1.0f && w.gain_db == -24.0f && w.note_gate == 1 && w.cut_from == 127 && w.pads == 1 && w.lock == 1 &&
          w.clip_depth == 1.0f && w.clip_offset == 0.0f,
          "every value is clamped, NaN and infinity become something sane");
    w.gain_db = 99.0f;
    ts_params_sanitize(&w);
    CHECK(w.gain_db == 12.0f, "gain tops out at +12 dB");
    ts_engine_set_params(NULL, &p);
    ts_engine_set_params(NULL, NULL);
}

/* ---- 3. samples ----------------------------------------------------------- */

static void put_file(const char *dir, const char *name, const void *data, size_t n)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, n, f); fclose(f); }
}

static void test_samples(void)
{
    printf("samples\n");
    size_t a0 = g_allocs, f0 = g_frees;
    ts_engine_t *e = ts_engine_create(44100.0f);
    CHECK(e && !e->slot[0].loaded, "slots start empty");
    CHECK(load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.1f, 44100) == TS_WAV_OK && e->slot[0].loaded && e->slot[0].len == 44100, "load");
    CHECK(strcmp(e->slot[0].name, "AHHH") == 0, "named in capitals, without the extension: %s", e->slot[0].name);
    CHECK(fabsf(e->slot[0].norm - 0.891f / 0.1f) < 0.05f, "normalise gain brings a 0.1 peak to -1 dBFS (%.2f)", (double)e->slot[0].norm);
    CHECK(load_sample(e, 1, "A \"quoted\\name\" that is much too long.wav", SMP_DC, 0.5f, 44100) == TS_WAV_OK &&
          strlen(e->slot[1].name) <= 11 && !strchr(e->slot[1].name, '"') && !strchr(e->slot[1].name, '\\'), "names are short and safe for the screen: %s", e->slot[1].name);
    CHECK(load_sample(e, 2, "", SMP_DC, 0.5f, 44100) == TS_WAV_OK && strcmp(e->slot[2].name, "SAMPLE") == 0, "an empty name gets one");
    CHECK(load_sample(e, 9, "x.wav", SMP_DC, 0.5f, 100) != TS_WAV_OK && load_sample(e, -1, "x.wav", SMP_DC, 0.5f, 100) != TS_WAV_OK, "bad slots");
    CHECK(ts_engine_load_wav_mem(e, 3, "bad.wav", (const uint8_t *)"RIFFnonsense", 12) != TS_WAV_OK && !e->slot[3].loaded, "a bad file loads nothing");

    /* the overview: silent for the first half, loud in the second */
    static int16_t pcm[44100];
    for (int i = 0; i < 44100; i++) pcm[i] = i < 22050 ? 0 : (int16_t)(20000 * sinf(0.2f * (float)i));
    size_t sz = make_wav(g_wav, pcm, 44100, 44100, 1, 16, 1);
    ts_engine_load_wav_mem(e, 3, "half.wav", g_wav, sz);
    int quiet = 1, loud = 0;
    for (int c = 0; c < 62; c++) quiet &= e->slot[3].pk[c] == 0;
    for (int c = 66; c < 128; c++) loud += e->slot[3].pk[c] >= 200;
    CHECK(quiet && loud > 50, "the waveform overview follows the sample");

    /* another engine sample rate: the sample is resampled to it */
    static int16_t big[44100 * 2];
    make_sample(big, 44100 * 2, SMP_DC, 0.3f);
    ts_engine_t *s = ts_engine_create(8000.0f);
    sz = make_wav(g_wav, big, 44100 * 2, 44100, 1, 16, 1);
    CHECK(ts_engine_load_wav_mem(s, 0, "long.wav", g_wav, sz) == TS_WAV_OK && abs(s->slot[0].len - 16000) <= 1, "an 8 kHz engine resamples (%d frames)", s->slot[0].len);
    ts_engine_destroy(s);

    ts_engine_destroy(e);
    CHECK(g_allocs - a0 == g_frees - f0, "no leak");

    /* a folder: slot 0 is ahhh.wav, slot 1 fresssh.wav, the first four others (alphabetically) are User 1-4 */
    const char *dir = TDIR "/samples_t";
    MKDIR(dir);
    static const char *const names[] = { "ahhh.wav", "fresssh.wav", "zeta.wav", "alpha.wav", "gamma.wav", "beta.wav", "delta.wav", "notes.txt", "bad.wav" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        remove(path);
    }
    make_sample(pcm, 44100, SMP_DC, 0.4f);
    sz = make_wav(g_wav, pcm, 44100, 44100, 1, 16, 1);
    for (size_t i = 0; i < 7; i++) put_file(dir, names[i], g_wav, sz);
    put_file(dir, "notes.txt", "hello", 5);
    put_file(dir, "bad.wav", "not a wav at all, but long enough to be tried, honestly yes", 60);
    e = ts_engine_create(44100.0f);
    int mask = ts_engine_load_dir(e, dir);
    CHECK(mask == 0x7F, "seven slots filled, the eighth left empty (mask %x)", mask);
    CHECK(!strcmp(e->slot[0].name, "AHHH") && !strcmp(e->slot[1].name, "FRESSSH") && !strcmp(e->slot[2].name, "ALPHA") && !strcmp(e->slot[3].name, "BETA") &&
          !strcmp(e->slot[4].name, "DELTA") && !strcmp(e->slot[5].name, "GAMMA"), "the slots: %s %s %s %s %s %s", e->slot[0].name, e->slot[1].name,
          e->slot[2].name, e->slot[3].name, e->slot[4].name, e->slot[5].name);
    CHECK(ts_engine_load_dir(e, dir) == 0, "a second look finds nothing new to load");
    CHECK(ts_engine_load_dir(e, TDIR "/no_such_folder") == 0 && ts_engine_load_dir(e, "") == 0 && ts_engine_load_dir(e, NULL) == 0 && ts_engine_load_dir(NULL, dir) == 0,
          "missing folders are fine");
    ts_engine_destroy(e);

    remove(TDIR "/samples_t/ahhh.wav");
    e = ts_engine_create(44100.0f);
    mask = ts_engine_load_dir(e, dir);
    CHECK(!(mask & 1) && !e->slot[0].loaded && (mask & 2), "without ahhh.wav slot 0 stays empty");
    ts_params_t p;
    ts_params_default(&p);
    p.sample = 0; p.note_gate = 0;
    ts_engine_set_params(e, &p);
    static float buf[2048];
    render_n(e, buf, 2048);
    CHECK(peak_of(buf, 2048) == 0.0f, "an empty slot is silent (it does not borrow another sample)");
    ts_engine_destroy(e);

    e = ts_engine_create(44100.0f);
    ts_engine_set_params(e, &p);
    render_n(e, buf, 2048);
    char js[2048];
    CHECK(peak_of(buf, 2048) == 0.0f, "no samples at all: silence");
    CHECK(ts_engine_view(e, js, sizeof js) > 0 && strstr(js, "\"n\":\"EMPTY\"") && strstr(js, "\"ld\":\"00000000\""), "...and the screen says EMPTY");
    ts_engine_destroy(e);
}

/* ---- 4. the playhead -------------------------------------------------------- */

static void test_playhead(void)
{
    printf("playhead\n");
    /* position maps linearly onto the sample: on a ramp the output is the position */
    ts_engine_t *e = plain_engine(SMP_RAMP, 0.8f);
    ts_params_t p = e->p;
    static float buf[8192];
    int mapped = 1;
    for (int k = 0; k <= 10; k++) {
        p.turntable = (float)k / 10.0f;
        ts_engine_set_params(e, &p);
        render_n(e, buf, 4096);                                     /* far longer than the 3 ms of Drag */
        float want = 0.8f * (2.0f * p.turntable - 1.0f);
        mapped &= fabsf(buf[4095] - want) < 0.01f;
    }
    CHECK(mapped, "Turntable 0 is the start of the sample and 1 the end");
    CHECK(fabs(e->pos - 1.0) < 1e-3, "the playhead is where the knob is");

    /* Drag: the playhead follows with a one-pole lag */
    double tau_ms[3];
    for (int k = 0; k < 3; k++) {
        ts_engine_destroy(e);
        e = plain_engine(SMP_RAMP, 0.8f);
        p = e->p;
        p.drag = k == 0 ? 0.0f : (k == 1 ? 0.5f : 1.0f);
        ts_engine_set_params(e, &p);
        p.turntable = 1.0f;
        ts_engine_set_params(e, &p);
        int n = 0;
        float one[1];
        while (e->pos < 0.6321 && n < 44100 * 2) { ts_engine_render(e, one, 1); n++; }
        tau_ms[k] = (double)n * 1000.0 / 44100.0;
    }
    CHECK(tau_ms[0] > 2.0 && tau_ms[0] < 4.5 && tau_ms[1] > 20.0 && tau_ms[1] < 26.0 && tau_ms[2] > 75.0 && tau_ms[2] < 92.0 && tau_ms[0] < tau_ms[1] && tau_ms[1] < tau_ms[2],
          "Drag sets the lag: %.1f ms at 0, %.1f ms at the default 0.5, %.1f ms at 1", tau_ms[0], tau_ms[1], tau_ms[2]);
    ts_engine_destroy(e);

    /* faster movement is higher pitch: a 440 Hz sine with the playhead moved at 1x, 2x, 0.5x and backwards at 1.5x */
    struct { float speed; int rev; } cases[] = { { 1.0f, 0 }, { 2.0f, 0 }, { 0.5f, 0 }, { 1.5f, 1 } };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        e = plain_engine(SMP_SINE, 0.8f);
        p = e->p;
        double pos = cases[c].rev ? 0.95 : 0.02;
        double step = (double)cases[c].speed * 128.0 / 44099.0 * (cases[c].rev ? -1.0 : 1.0);
        static float tone[44100];
        int n = 0;
        for (int b = 0; b < 340; b++) {
            pos += step;
            if (pos < 0.01 || pos > 0.97) break;
            p.turntable = (float)pos;
            ts_engine_set_params(e, &p);
            if (b < 6) { float skip[128]; ts_engine_render(e, skip, 128); continue; }     /* let the lag settle */
            ts_engine_render(e, tone + n, 128);
            n += 128;
        }
        int ups = 0;
        for (int i = 1; i < n; i++) ups += tone[i - 1] <= 0.0f && tone[i] > 0.0f;
        float f = n > 0 ? (float)ups * 44100.0f / (float)n : 0.0f;
        float want = 440.0f * cases[c].speed;
        CHECK(n >= 3000 && fabsf(f - want) / want < 0.07f, "%.1fx%s: %.0f Hz (want %.0f)", (double)cases[c].speed, cases[c].rev ? " backwards" : "", (double)f, (double)want);
        ts_engine_destroy(e);
    }

    /* a still playhead is a still sample: no tone, just the value it sits on */
    e = plain_engine(SMP_SINE, 0.8f);
    p = e->p;
    p.turntable = 0.25f;
    ts_engine_set_params(e, &p);
    render_n(e, buf, 8192);
    CHECK(fabsf(buf[4096] - buf[8191]) < 1e-4f && fabsf(e->vel_out) < 1e-6f, "held still it makes no sound of its own");
    ts_engine_destroy(e);
}

/* ---- 5. cuts: the notes -------------------------------------------------------- */

static void test_cuts(void)
{
    printf("cuts and notes\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.normalise = 0; p.turntable = 0.5f; p.drag = 0.0f; p.pads = 0;   /* plain notes */
    ts_engine_set_params(e, &p);
    static float buf[8192];

    render_n(e, buf, 2048);
    CHECK(peak_of(buf, 2048) == 0.0f, "Note Gate on: silent until a note is held");
    ts_engine_note(e, 60, 100, 1);
    render_n(e, buf, 2048);
    CHECK(fabsf(buf[2047] - 0.5f) < 1e-4f && buf[0] < 0.5f, "C3 cuts the sound in");
    int n05 = 0, n95 = 0;
    for (int i = 0; i < 400; i++) { if (buf[i] < 0.05f * 0.5f) n05 = i; if (buf[i] < 0.95f * 0.5f) n95 = i; }
    CHECK(n95 - n05 > 60 && n95 - n05 < 100, "a cut ramps in 2 ms, not a click and not a smear (%d samples)", n95 - n05);
    ts_engine_note(e, 60, 0, 0);
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f && buf[0] > 0.45f, "and out");

    ts_engine_note(e, 72, 100, 1);
    ts_engine_note(e, 84, 100, 1);
    ts_engine_note(e, 72, 0, 0);
    render_n(e, buf, 2048);
    CHECK(fabsf(buf[2047] - 0.5f) < 1e-4f, "two notes held, one let go: still cut in");
    ts_engine_note(e, 84, 0, 0);
    ts_engine_note(e, 84, 0, 0);                                    /* a stray note-off */
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f && e->notes_held == 0, "...both let go: out, and a stray note-off cannot go below zero");
    ts_engine_note(e, 100, 100, 1);
    ts_engine_all_notes_off(e);
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f && e->notes_held == 0, "all notes off lets go of every cut");

    /* notes below C3 do nothing: they do not cut */
    ts_engine_note(e, 5, 100, 1);
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f && e->notes_held == 0, "a low note is not a cut");
    ts_engine_note(e, 59, 100, 1);
    CHECK(e->notes_held == 0, "B2 is the last note that does not cut");
    ts_engine_note(e, 3, 0, 0);
    CHECK(e->notes_held == 0, "a note-off changes nothing");
    ts_engine_note(e, -1, 100, 1);
    ts_engine_note(e, 200, 100, 1);
    ts_engine_note(NULL, 60, 100, 1);
    CHECK(e->notes_held == 0, "notes outside 0..127 are ignored");

    /* the lowest cutting note is a setting */
    p = e->p;
    p.cut_from = 36;
    ts_engine_set_params(e, &p);
    ts_engine_note(e, 40, 100, 1);
    render_n(e, buf, 2048);
    CHECK(fabsf(buf[2047] - 0.5f) < 1e-4f, "Cut From 36: note 40 cuts");
    ts_engine_note(e, 40, 0, 0);
    ts_engine_note(e, 35, 100, 1);
    CHECK(e->notes_held == 0, "...and 35 does not");
    p = e->p;
   
    p.cut_from = 60;
    ts_engine_set_params(e, &p);

    /* Note Gate off: the sound is always there */
    p.note_gate = 0;
    ts_engine_set_params(e, &p);
    render_n(e, buf, 4096);
    CHECK(fabsf(buf[4095] - 0.5f) < 1e-4f, "Note Gate off: heard without any note");
    p.note_gate = 1;
    ts_engine_set_params(e, &p);
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f, "...and on again");

    /* velocity to volume */
    p.vel_to_vol = 1;
    ts_engine_set_params(e, &p);
    float lvl[3];
    int vels[3] = { 127, 64, 20 };
    for (int k = 0; k < 3; k++) {
        ts_engine_note(e, 60, vels[k], 1);
        render_n(e, buf, 2048);
        lvl[k] = buf[2047];
        ts_engine_note(e, 60, 0, 0);
        render_n(e, buf, 2048);
    }
    CHECK(fabsf(lvl[0] - 0.5f) < 1e-3f && fabsf(lvl[1] - 0.5f * 64.0f / 127.0f) < 1e-3f && fabsf(lvl[2] - 0.5f * 20.0f / 127.0f) < 1e-3f,
          "Vel to Vol: %.3f %.3f %.3f", (double)lvl[0], (double)lvl[1], (double)lvl[2]);
    p.vel_to_vol = 0;
    ts_engine_set_params(e, &p);
    ts_engine_note(e, 60, 20, 1);
    render_n(e, buf, 2048);
    CHECK(fabsf(buf[2047] - 0.5f) < 1e-3f, "...off: every note is full volume");
    ts_engine_note(e, 60, 0, 0);
    ts_engine_destroy(e);
}

/* the clip on pad n of the selected sample's bank, and whether you made it */
static const ts_clip_t *PADCLIP(const ts_engine_t *e, int pad) { return bank_clip(e, e->p.sample, pad); }
static int PADKEPT(const ts_engine_t *e, int pad)
{
    int i = pad >= 1 && pad <= TS_NUM_CLIPS ? e->bank[e->p.sample][pad - 1] - 1 : -1;
    return i >= 0 && e->kept[i].used;
}

/* ---- 6. the pad grid ------------------------------------------------------------ */

static void test_pads(void)
{
    printf("pad grid\n");
    CHECK(ts_pad_index(35) == -1 && ts_pad_index(36) == 0 && ts_pad_index(43) == 7 && ts_pad_index(67) == 31 && ts_pad_index(68) == 0 && ts_pad_index(75) == 7 &&
          ts_pad_index(99) == 31 && ts_pad_index(100) == -1 && ts_pad_index(-5) == -1 && ts_pad_index(0) == -1, "pads are notes 36..99: the drum layout 36..67 and the pad ids 68..99 are the same grid");

    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t base;
    ts_params_default(&base);
    base.normalise = 0; base.turntable = 0.5f; base.drag = 0.0f; base.clip = 0;
    CHECK(base.pads == 1, "the pad grid is on by default");
    int cut_ok = 1, sample_ok = 1, auto_ok = 1, gen_ok = 1, clip_ok = 1, empty_ok = 1, keep_ok = 1, off_ok = 1, bad = 0;
    for (int first = 36; first <= 68; first += 32)
        for (int pad = 0; pad < 32; pad++) {
            int note = first + pad, row = pad / 8, col = pad % 8;
            ts_engine_set_params(e, &base);
            ts_engine_all_notes_off(e);
            e->try_valid = 0;
            ts_params_t q = e->p;
            q.sample = 3; q.clip = 5;
            ts_engine_set_params(e, &q);
            ts_engine_note(e, note, 90, 1);
            if (row < 1) {                                                  /* the bottom row: cut pads */
                cut_ok &= e->notes_held == 1 && e->last_vel == 90 && e->p.sample == 3 && e->p.clip == 5;
                ts_engine_note(e, note, 0, 0);
                cut_ok &= e->notes_held == 0;
            } else if (row == 1) {                                          /* the eight samples */
                sample_ok &= e->p.sample == col && e->notes_held == 0;
                int s = e->p.sample, l = e->p.clip;
                ts_engine_note(e, note, 0, 0);
                off_ok &= e->p.sample == s && e->p.clip == l && e->notes_held == 0;
            } else {                                                        /* the top two rows: the clips */
                int n = (3 - row) * 8 + col + 1;                            /* the top row is clips 1-8 */
                if (n == TS_CLIP_PADS + 1) {                                /* then Generate */
                    gen_ok &= e->p.clip == TS_CLIP_TRY && e->try_valid && e->p.sample == 3 && e->notes_held == 0;
                } else if (n > TS_CLIP_PADS + 1) {                          /* and Auto, the last pad of the clip rows */
                    auto_ok &= e->p.clip == TS_CLIP_AUTO && e->p.sample == 3;
                } else if (PADCLIP(e, n)) {
                    clip_ok &= e->p.clip == n && e->p.sample == 3 && e->notes_held == 0;
                } else {                                                    /* an empty pad: it is selected, and nothing plays; nothing is kept... */
                    empty_ok &= e->p.clip == n && PADCLIP(e, n) == NULL;
                    ts_engine_set_params(e, &q);
                    ts_engine_generate(e);                                  /* ...even with a clip generated, unless it is the one playing... */
                    e->p.clip = 5;
                    ts_engine_note(e, note, 90, 1);
                    empty_ok &= e->p.clip == n && PADCLIP(e, n) == NULL;
                    ts_engine_set_params(e, &q);
                    ts_engine_generate(e);                                  /* ...when it is, it is kept here */
                    ts_engine_note(e, note, 90, 1);
                    keep_ok &= e->p.clip == n && PADCLIP(e, n) != NULL && PADKEPT(e, n);
                    ts_engine_clear_clip(e, n);
                    keep_ok &= PADCLIP(e, n) == NULL && !PADKEPT(e, n);
                }
                int cl = e->p.clip;
                ts_engine_note(e, note, 0, 0);
                off_ok &= e->p.clip == cl;
            }
            bad += e->notes_held < 0 || e->notes_held > 1;
        }
    CHECK(cut_ok, "the bottom row (8 pads) is cuts, with their velocity");
    CHECK(sample_ok, "second row, left to right: Ahhh, Fresssh, User 1 to 6");
    CHECK(auto_ok, "Auto (a random clip every bar) is the last pad of the clip rows; the last two pads of the sample row are not used");
    CHECK(gen_ok, "the pad before Auto is Generate: a new clip to try, and it plays");
    CHECK(clip_ok, "the top two rows pick clips 1 to 14, 1-8 on the top row");
    CHECK(empty_ok, "an empty clip pad is selected and nothing plays; a generated clip is not kept there unless it is the one playing");
    CHECK(keep_ok, "...which then keeps it there (and a clip you made can be deleted)");
    CHECK(off_ok && bad == 0, "letting go of a pad changes nothing");

    /* notes outside the pads keep the plain rule */
    ts_engine_set_params(e, &base);
    ts_engine_note(e, 20, 100, 1);
    CHECK(e->notes_held == 0, "a note below the pads (20) does not cut");
    ts_engine_note(e, 100, 100, 1);
    CHECK(e->notes_held == 1, "a note above them (100) cuts");
    ts_engine_note(e, 100, 0, 0);
    ts_engine_note(e, 35, 100, 1);
    CHECK(e->notes_held == 0, "35 is not a pad (and does not cut)");

    /* with the grid off the pads are plain notes again */
    ts_params_t q = e->p;
    q.pads = 0;
    ts_engine_set_params(e, &q);
    ts_engine_note(e, 40, 100, 1);
    CHECK(e->notes_held == 0, "grid off: note 40 is below C3: no cut");
    ts_engine_note(e, 70, 100, 1);
    CHECK(e->notes_held == 1, "...and 70 cuts");
    ts_engine_note(e, 70, 0, 0);

    /* a cut pad is heard */
    ts_engine_set_params(e, &base);
    static float buf[4096];
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f, "(silent)");
    ts_engine_note(e, 36 + 3, 100, 1);
    render_n(e, buf, 2048);
    CHECK(fabsf(buf[2047] - 0.5f) < 1e-4f, "a cut pad cuts the sound in");
    ts_engine_note(e, 36 + 3, 0, 0);
    ts_engine_destroy(e);
}

/* ---- 7. gain and normalise ------------------------------------------------------- */

static void test_gain(void)
{
    printf("gain and normalise\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "quiet.wav", SMP_DC, 0.1f, 44100);
    load_sample(e, 1, "loud.wav", SMP_DC, 0.9f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.note_gate = 0; p.drag = 0.0f; p.turntable = 0.5f;
    static float buf[4096];
    float level[2][2];
    for (int norm = 0; norm < 2; norm++)
        for (int s = 0; s < 2; s++) {
            p.normalise = norm; p.sample = s;
            ts_engine_set_params(e, &p);
            render_n(e, buf, 4096);
            level[norm][s] = buf[4095];
        }
    CHECK(fabsf(level[0][0] - 0.1f) < 1e-3f && fabsf(level[0][1] - 0.9f) < 1e-3f, "Normalise off: as recorded");
    CHECK(fabsf(level[1][0] - 0.891f) < 2e-3f && fabsf(level[1][1] - 0.891f) < 2e-3f, "Normalise on: both play at -1 dBFS (%.3f, %.3f)", (double)level[1][0], (double)level[1][1]);
    p.normalise = 0; p.sample = 1;
    float db[4] = { 6.0f, -6.0f, 12.0f, -24.0f };
    for (int k = 0; k < 4; k++) {
        p.gain_db = db[k];
        ts_engine_set_params(e, &p);
        render_n(e, buf, 4096);
        float want = 0.9f * powf(10.0f, db[k] / 20.0f);
        CHECK(fabsf(buf[4095] - want) < 2e-3f * fmaxf(1.0f, want), "Gain %+.0f dB: %.3f (want %.3f)", (double)db[k], (double)buf[4095], (double)want);
    }
    p.gain_db = -100.0f;
    ts_engine_set_params(e, &p);
    CHECK(e->p.gain_db == -24.0f, "the range is -24 .. +12 dB");
    ts_engine_destroy(e);
}

static int hexv(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

/* ---- 9. the screen's view -------------------------------------------------------------------- */

static int json_num(const char *js, const char *key, double *out)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    *out = atof(p + strlen(pat));
    return 1;
}

static int json_str(const char *js, const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return 1;
}

static void test_view(void)
{
    printf("the screen's view\n");
    ts_engine_t *e = plain_engine(SMP_SINE, 0.8f);
    load_sample(e, 2, "My Voice.wav", SMP_DC, 0.3f, 44100);
    ts_params_t p = e->p;
    p.turntable = 0.25f; p.drag = 0.3f; p.gain_db = -3.0f; p.note_gate = 1; p.vel_to_vol = 1; p.cut_from = 48;
    ts_engine_set_params(e, &p);
    static float buf[4096];
    render_n(e, buf, 4096);
    char js[2400];
    int n = ts_engine_view(e, js, sizeof js);
    CHECK(n > 900 && n < 1500 && balanced(js) && js[0] == '{' && js[n - 1] == '}' && (int)strlen(js) == n, "valid JSON, %d bytes", n);
    double v;
    char s[512];
    CHECK(json_num(js, "x", &v) && v == 32.0 && json_num(js, "tx", &v) && v == 32.0 && json_num(js, "p", &v) && fabs(v - 0.25) < 0.001 &&
          json_num(js, "tt", &v) && fabs(v - 0.25) < 0.001, "the playhead column and position");
    CHECK(json_num(js, "dr", &v) && fabs(v - 0.3) < 0.01 && json_num(js, "gn", &v) && fabs(v + 3.0) < 0.01 && json_num(js, "ng", &v) && v == 1 && json_num(js, "vv", &v) && v == 1 &&
          json_num(js, "no", &v) && v == 0 && json_num(js, "lk", &v) && v == 1 && json_num(js, "cn", &v) && v == 48, "the controls");
    CHECK(json_num(js, "tp", &v) && v == 0 && !strstr(js, "\"lf\"") && !strstr(js, "\"ls\""), "no transport, and nothing of an LFO");
    CHECK(json_str(js, "n", s, sizeof s) && !strcmp(s, "TEST") && json_str(js, "sn", s, sizeof s) && !strcmp(s, "TEST|FRESSSH|MY VOICE|USER 2|USER 3|USER 4|USER 5|USER 6") &&
          json_str(js, "ld", s, sizeof s) && !strcmp(s, "10100000"), "the sample's name, the eight tabs and which are loaded: %s", s);
    CHECK(json_str(js, "w", s, sizeof s) && strlen(s) == 2 * TS_VIEW_COLS && strspn(s, "0123456789abcdef") == strlen(s) &&
          json_str(js, "wr", s, sizeof s) && strlen(s) == 2 * TS_VIEW_COLS && strspn(s, "0123456789abcdef") == strlen(s), "the waveform: 128 columns of peak and of body, two hex digits each");
    CHECK(json_str(js, "lnp", s, sizeof s) && strlen(s) == TS_LANE && json_str(js, "lng", s, sizeof s) && strlen(s) == TS_LANE && strspn(s, "01") == TS_LANE,
          "the clip view: a bar of columns");

    /* the waveform keeps the shape of the sound: a sine's peak and body are in the ratio 1 : 0.707 */
    json_str(js, "w", s, sizeof s);
    char body[512];
    json_str(js, "wr", body, sizeof body);
    int pk = 0, rm = 0;
    sscanf(s + 2 * 40, "%2x", &pk);
    sscanf(body + 2 * 40, "%2x", &rm);
    CHECK(pk > 240 && abs(rm - (int)(0.7071 * pk)) < 10, "peak %d, body %d: a sine's rms is 0.707 of its peak", pk, rm);

    /* a sample that is silent then loud shows exactly that */
    ts_engine_destroy(e);
    e = ts_engine_create(44100.0f);
    static int16_t half[44100];
    for (int i = 0; i < 44100; i++) half[i] = i < 22050 ? 0 : (int16_t)(20000 * sinf(0.2f * (float)i));
    size_t sz = make_wav(g_wav, half, 44100, 44100, 1, 16, 1);
    ts_engine_load_wav_mem(e, 0, "half.wav", g_wav, sz);
    ts_engine_view(e, js, sizeof js);
    json_str(js, "w", s, sizeof s);
    int quiet = 1, loud = 0;
    for (int c = 0; c < 62; c++) { int a; sscanf(s + 2 * c, "%2x", &a); quiet &= a == 0; }
    for (int c = 66; c < 128; c++) { int a; sscanf(s + 2 * c, "%2x", &a); loud += a > 200; }
    CHECK(quiet && loud > 55, "silent first half, loud second half");
    ts_engine_destroy(e);

    /* the clip view: with the transport running a column is a place in the bar; the path and the cuts are written where they happen */
    e = plain_engine(SMP_DC, 0.5f);
    p = e->p;
    p.note_gate = 1; p.lock = 1;
    ts_engine_set_params(e, &p);
    CHECK(ts_engine_view(e, js, sizeof js) > 0 && json_str(js, "lnp", s, sizeof s) && strspn(s, "0") == TS_LANE && json_str(js, "lng", s, sizeof s) && strspn(s, "0") == TS_LANE,
          "an empty lane is flat");
    double spb = 120.0 / (60.0 * 44100.0);
    const double LB = (double)TS_LANE_BEATS;
    for (int bar = 0; bar < 2; bar++) {
        double beat = (double)bar * LB;
        while (beat < (bar + 1) * LB) {
            float b128[128];
            double beats_now = beat;
            p.turntable = (float)((beats_now - bar * LB) / LB);           /* the playhead goes up through the pass */
            ts_engine_set_params(e, &p);
            ts_engine_sync_beat(e, beat);
            /* the second pass has the cut held for beat 1..2; the first has none */
            if (bar == 1 && beats_now >= LB + 1.0 && e->notes_held == 0) ts_engine_note(e, 60, 100, 1);
            if (bar == 1 && beats_now >= LB + 2.0 && e->notes_held == 1) ts_engine_note(e, 60, 0, 0);
            ts_engine_render(e, b128, 128);
            beat += 128.0 * spb;
        }
    }
    ts_engine_view(e, js, sizeof js);
    char lnp[160], lng[160];
    json_str(js, "lnp", lnp, sizeof lnp);
    json_str(js, "lng", lng, sizeof lng);
    CHECK(json_num(js, "tp", &v) && v == 1, "(the transport is running)");
    int cut_in = 0, cut_out = 0, rise = 1;
    for (int k = 36; k < 60; k++) cut_in += lng[k] == '1';
    for (int k = 0; k < TS_LANE; k++) if (k < 28 || k > 68) cut_out += lng[k] == '1';
    for (int k = 12; k < TS_LANE; k++) rise &= hexv(lnp[k]) >= hexv(lnp[k - 6]) - 1;
    CHECK(cut_in >= 18 && cut_out <= 3, "the cut written into beat 2 of the bar (columns 32-63): %d inside, %d outside", cut_in, cut_out);
    CHECK(rise && lnp[TS_LANE - 4] > '9' && lnp[2] < '3', "the playhead's path climbs through the bar");
    /* the next bar goes by with no cut: it is overwritten, so the lane always shows the last pass */
    for (double beat = 2.0 * LB; beat < 3.0 * LB; beat += 128.0 * spb) {
        float b128[128];
        p.turntable = (float)((beat - 2.0 * LB) / LB);
        ts_engine_set_params(e, &p);
        ts_engine_sync_beat(e, beat);
        ts_engine_render(e, b128, 128);
    }
    ts_engine_view(e, js, sizeof js);
    json_str(js, "lng", lng, sizeof lng);
    int left = 0;
    for (int k = 0; k < TS_LANE; k++) left += lng[k] == '1';
    CHECK(left == 0, "the next pass with no cut clears it (%d columns still cut)", left);
    json_num(js, "lcol", &v);
    CHECK(v >= 0 && v < TS_LANE, "and the cursor is in the lane (%g)", v);
    ts_engine_destroy(e);

    /* it never overflows: every buffer size either holds a whole object or says so and is empty */
    e = plain_engine(SMP_SINE, 0.5f);
    int fits = 0, broke = 0;
    for (int cap = 2; cap < 1700; cap += 13) {
        char *c = (char *)TS_CALLOC((size_t)cap);                   /* exact size: an overrun hits a redzone */
        int r = ts_engine_view(e, c, cap);
        if (r < 0) broke += c[0] != 0;
        else { fits++; broke += !balanced(c) || (int)strlen(c) != r; }
        TS_FREE(c);
    }
    CHECK(fits > 20 && broke == 0, "all buffer sizes are safe (%d fit)", fits);
    CHECK(ts_engine_view(NULL, js, sizeof js) == -1 && ts_engine_view(e, NULL, 100) == -1 && ts_engine_view(e, js, 1) == -1, "bad arguments");
    ts_engine_destroy(e);
}

/* ---- 10c. clips ------------------------------------------------------------------------------------ */

static void test_clips(void)
{
    printf("clips\n");
    /* the built-in clips are one bar: 128 units, 32 to the beat */
    const int UN = 128;
    ts_engine_t *ce = ts_engine_create(44100.0f);
    CHECK(ce->nclips == TS_BUILTIN_CLIPS && ce->clip_data[0].len == 4 && ce->clip_data[0].n == 4 * TS_CLIP_RES && ce->clip_data[8].n == 0, "with no clips.bin there are eight built-in one-bar clips, and eight free pads");
    int range_ok = 1, loop_ok = 1, cuts_ok = 1, distinct = 1, moves_ok = 1;
    for (int c = 1; c <= TS_BUILTIN_CLIPS; c++) {
        float lo = 9.0f, hi = -9.0f, travel = 0.0f, prev = ts_clip_pos(ce, c, 0.0);
        int open = 0;
        for (int u = 0; u < UN * 4; u++) {
            float v = ts_clip_pos(ce, c, u * 0.25 / 32.0);
            range_ok &= v >= 0.0f && v <= 1.0f;
            lo = fminf(lo, v); hi = fmaxf(hi, v);
            travel += fabsf(v - prev);
            prev = v;
        }
        for (int u = 0; u < UN; u++) open += ts_clip_cut(ce, c, (u + 0.5) / 32.0);
        loop_ok &= fabsf(ts_clip_pos(ce, c, 0.0) - ts_clip_pos(ce, c, UN / 32.0)) < 1e-3f && ts_clip_pos(ce, c, 3.3 / 32.0) == ts_clip_pos(ce, c, (3.3 + 5 * UN) / 32.0);
        cuts_ok &= open >= 76 && open <= 116;              /* 10 to 14 of the 16 sixteenths */
        moves_ok &= hi - lo > 0.3f && travel > 1.0f;                                     /* a real path, not a wobble */
        for (int d = c + 1; d <= TS_BUILTIN_CLIPS; d++) {
            int same = 1;
            for (int u = 0; u < UN; u += 4) same &= fabsf(ts_clip_pos(ce, c, u / 32.0) - ts_clip_pos(ce, d, u / 32.0)) < 1e-6f && ts_clip_cut(ce, c, u / 32.0) == ts_clip_cut(ce, d, u / 32.0);
            distinct &= !same;
        }
    }
    CHECK(range_ok, "every clip stays on the sample (0..1)");
    CHECK(loop_ok, "...and loops: the end is the start, and a pass later is the same place");
    CHECK(cuts_ok, "...with cuts for most of the bar, and gaps");
    CHECK(moves_ok, "...a path that really travels (at least a third of the sample, and over its length more than the whole sample)");
    CHECK(distinct, "eight different clips");
    CHECK(ts_clip_pos(ce, 0, 5.0) == 0.0f && ts_clip_pos(ce, 99, 5.0) == 0.0f && ts_clip_cut(ce, 0, 5.0) == 0 && ts_clip_cut(ce, -1, 5.0) == 0 && isfinite(ts_clip_pos(ce, 3, -7.5)) &&
          ts_clip_pos(ce, 3, 1e9) >= 0.0f && ts_clip_pos(NULL, 1, 0.0) == 0.0f && ts_clip_pos(ce, 3, NAN) == 0.0f, "bad clips and places are harmless");
    ts_engine_destroy(ce);

    /* the engine: a clip drives the playhead and the cuts while the transport runs, with no note */
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "ramp.wav", SMP_RAMP, 0.8f, 44100);
    ts_params_t p;
    ts_params_default(&p); p.swing = 0.0f;
    CHECK(p.clip == TS_CLIP_AUTO && p.lock == 1, "defaults: the clips in turn, locked to the transport");
    {
        char jn[8192];
        double ln = -1;
        ts_engine_view(e, jn, sizeof jn);
        CHECK(json_num(jn, "ln", &ln) && ln == 0, "no note yet: 0");
        ts_engine_note(e, 77, 100, 1);
        ts_engine_note(e, 77, 0, 0);
        ts_engine_view(e, jn, sizeof jn);
        CHECK(json_num(jn, "ln", &ln) && ln == 77, "the view tells which note arrived last, to see what the pads send");
    }
    p.normalise = 0; p.drag = 0.0f; p.pads = 0; p.clip = 3; p.turntable = 0.0f;
    ts_engine_set_params(e, &p);
    static float buf[128 * 2000];
    double spb = 120.0 / (60.0 * 44100.0);
    float blk[128];
    for (int b = 0; b < 200; b++) ts_engine_render(e, blk, 128);              /* the transport is stopped: nothing */
    float quiet = 0.0f;
    for (int i = 0; i < 128; i++) quiet = fmaxf(quiet, fabsf(blk[i]));
    CHECK(quiet == 0.0f && e->running == 0 && fabs(e->pos - e->man) < 1e-6, "stopped transport: silent, and the playhead sits at the Turntable");

    int heard = 0, gaps = 0, moves = 0;
    float pmin = 9.0f, pmax = -9.0f;
    for (int b = 0; b < 1900; b++) {
        ts_engine_sync_beat(e, 128.0 * spb * b);
        ts_engine_render(e, buf + b * 128, 128);
        heard += fabsf(buf[b * 128 + 127]) > 0.01f;
        gaps += buf[b * 128 + 127] == 0.0f;
        pmin = fminf(pmin, (float)e->pos); pmax = fmaxf(pmax, (float)e->pos);
        moves += fabs(e->pos - e->target) < 0.2;
    }
    CHECK(e->running == 1 && e->clip_now == 3, "the transport runs: the clip plays (clip %d)", e->clip_now);
    CHECK(heard > 700 && gaps > 150, "it cuts the sound in and out by itself, no note held (%d in, %d out of 1900 blocks)", heard, gaps);
    CHECK(pmax - pmin > 0.3f, "and the playhead travels (%.2f..%.2f)", (double)pmin, (double)pmax);
    CHECK(moves > 1500, "(it follows its target)");

    /* it is the plan that the view shows, with the cursor on the transport */
    char js[8192], s[600];
    ts_engine_view(e, js, sizeof js);
    json_str(js, "lnp", s, sizeof s);
    int plan_ok = 1;
    for (int k = 0; k < TS_LANE; k++) plan_ok &= hexv(s[k]) == clampi((int)(ts_clip_pos(e, 3, (k + 0.5) / 32.0) * 15.0f + 0.5f), 0, 15);
    CHECK(plan_ok, "the clip view shows the clip's path");
    json_str(js, "lng", s, sizeof s);
    int cut_plan = 1;
    for (int k = 0; k < TS_LANE; k++) cut_plan &= (s[k] == '1') == (ts_clip_cut(e, 3, (k + 0.5) / 32.0) != 0);
    double cl = 0, ru = 0;
    CHECK(cut_plan && json_num(js, "cl", &cl) && cl == 3 && json_num(js, "ru", &ru) && ru == 1, "...and its cuts, and which clip, and that it runs");

    /* stopping the transport stops it */
    ts_engine_sync_beat(e, -1.0);
    for (int b = 0; b < 100; b++) ts_engine_render(e, buf, 128);
    float after = 0.0f;
    for (int i = 0; i < 128; i++) after = fmaxf(after, fabsf(buf[i]));
    CHECK(after == 0.0f && e->running == 0, "transport stopped: it stops, and stays stopped");

    /* a held cut plays the clip from its start, even with the transport stopped, and letting go ends it */
    ts_engine_note(e, 60, 100, 1);
    float first_pos = -1.0f, held_peak = 0.0f;
    for (int b = 0; b < 300; b++) {
        ts_engine_render(e, buf + b * 128, 128);
        if (b == 0) first_pos = (float)e->target;
        held_peak = fmaxf(held_peak, fabsf(buf[b * 128 + 127]));
    }
    CHECK(held_peak > 0.01f && fabs(first_pos - ts_clip_pos(e, 3, 0.1 / 32.0)) < 0.02f && e->mclock > 0.5 && e->mclock < 3.0,
          "a held cut plays the clip from its start (clock %.2f beats)", e->mclock);
    ts_engine_note(e, 60, 0, 0);
    for (int b = 0; b < 100; b++) ts_engine_render(e, buf, 128);
    after = 0.0f;
    for (int i = 0; i < 128; i++) after = fmaxf(after, fabsf(buf[i]));
    CHECK(after == 0.0f, "let go: silent again");
    ts_engine_note(e, 60, 100, 1);                                          /* pressed again: from the start again */
    ts_engine_render(e, blk, 128);
    CHECK(e->mclock < 0.05, "...and the next press starts the clip from its beginning again (%.3f)", e->mclock);
    ts_engine_note(e, 60, 0, 0);

    /* Off: only notes cut */
    p.clip = 0;
    ts_engine_set_params(e, &p);
    int silent = 1;
    for (int b = 0; b < 300; b++) {
        ts_engine_sync_beat(e, 128.0 * spb * b);
        ts_engine_render(e, blk, 128);
        silent &= blk[127] == 0.0f;
    }
    CHECK(silent && fabs(e->pos - e->man) < 1e-6, "Clip off with the transport running: silent until a note, and the playhead is the Turntable's");
    ts_engine_note(e, 60, 100, 1);
    for (int b = 0; b < 30; b++) ts_engine_render(e, blk, 128);
    CHECK(fabsf(blk[127]) > 0.01f, "...and a note cuts it in");
    ts_engine_note(e, 60, 0, 0);

    /* Lock off: the clip runs on its own, whatever the transport does */
    p.clip = 3; p.lock = 0;
    ts_engine_set_params(e, &p);
    ts_engine_sync_beat(e, -1.0);
    heard = 0;
    for (int b = 0; b < 400; b++) { ts_engine_render(e, blk, 128); heard += fabsf(blk[127]) > 0.01f; }
    CHECK(heard > 100 && e->running == 1, "Lock off: it plays by itself");
    p.lock = 1;

    /* auto: like a Follow Action set to Other: another random clip every bar */
    p.clip = TS_CLIP_AUTO;
    ts_engine_set_params(e, &p);
    int engine_ok = 1;
    for (long long bar = 0; bar < 40; bar++) {
        ts_engine_sync_beat(e, (double)bar * 4.0 + 1.0);
        ts_engine_render(e, blk, 128);
        engine_ok &= e->clip_now == ts_clip_auto(e, bar);
    }
    CHECK(engine_ok, "auto plays ts_clip_auto's clip in every bar, wherever the transport jumps to");
    int range_ok2 = 1, nodup = 1, ascending = 0, prevc = 0, hands_ok = 1;
    for (long long bar = 0; bar < 400; bar++) {
        int c = ts_clip_auto(e, bar);
        range_ok2 &= c >= 1 && c <= TS_NUM_CLIPS;
        if (bar > 0) { nodup &= c != prevc; ascending += c == (prevc % TS_NUM_CLIPS) + 1; }
        prevc = c;
    }
    for (long long hand = 0; hand < 50; hand++) {
        unsigned seen_mask = 0;
        for (int i = 0; i < TS_NUM_CLIPS; i++) seen_mask |= 1u << ts_clip_auto(e, hand * TS_NUM_CLIPS + i);
        hands_ok &= seen_mask == 0x1FEu;
    }
    CHECK(range_ok2 && nodup, "never the same clip twice running");
    CHECK(hands_ok, "all eight are heard before any is heard again");
    CHECK(ascending < 100, "...in a random order, not one after the other (%d of 399 bars go on to the next number)", ascending);
    CHECK(ts_clip_auto(e, 123) == ts_clip_auto(e, 123) && ts_clip_auto(e, -1) >= 1 && ts_clip_auto(e, -1) <= 8 && ts_clip_auto(e, -9) >= 1 && ts_clip_auto(e, 1LL << 40) >= 1 &&
          ts_clip_auto(e, 7) != ts_clip_auto(e, 8), "it depends on nothing but the bar number (so a jump in the transport lands on the right clip)");

    /* a knob turn is a hand on the record, and the clip has it back after */
    p.clip = 3;
    ts_engine_set_params(e, &p);
    ts_engine_sync_beat(e, 4.0);
    ts_engine_render(e, blk, 128);
    p.turntable = 0.95f;
    ts_engine_set_params(e, &p);
    float hi2 = 0.0f;
    for (int b = 0; b < 60; b++) { ts_engine_sync_beat(e, 4.0 + 128.0 * spb * b); ts_engine_render(e, blk, 128); }
    hi2 = (float)e->pos;
    CHECK(hi2 > 0.9f, "the Turntable knob takes over from the clip for a moment (%.2f)", (double)hi2);
    for (int b = 60; b < 600; b++) { ts_engine_sync_beat(e, 4.0 + 128.0 * spb * b); ts_engine_render(e, blk, 128); }
    CHECK(fabs(e->target - ts_clip_pos(e, 3, e->mclock)) < 1e-6, "...and the clip has it back after");
    CHECK(e->slot[0].lo == 0.0f && e->slot[0].hi == 1.0f, "a sample that sounds all the way through: the clip has all of it");
    ts_engine_destroy(e);

    /* a sample with silence at each end: the clip scratches the sound, not the silence round it */
    e = ts_engine_create(44100.0f);
    static int16_t gap[44100];
    for (int i = 0; i < 44100; i++) gap[i] = (i > 13230 && i < 30870) ? (int16_t)(20000.0f * sinf(0.2f * (float)i)) : 0;
    size_t gsz = make_wav(g_wav, gap, 44100, 44100, 1, 16, 1);
    ts_engine_load_wav_mem(e, 0, "gap.wav", g_wav, gsz);
    CHECK(fabsf(e->slot[0].lo - 0.30f) < 0.01f && fabsf(e->slot[0].hi - 0.70f) < 0.01f, "the sound is found: %.3f to %.3f", (double)e->slot[0].lo, (double)e->slot[0].hi);
    ts_params_default(&p);
    p.drag = 0.0f; p.pads = 0; p.clip = TS_CLIP_AUTO;
    ts_engine_set_params(e, &p);
    float tmin = 9.0f, tmax = -9.0f;
    for (int b = 0; b < 4000; b++) {
        ts_engine_sync_beat(e, 128.0 * spb * b);
        ts_engine_render(e, blk, 128);
        tmin = fminf(tmin, (float)e->target); tmax = fmaxf(tmax, (float)e->target);
    }
    CHECK(tmin >= e->slot[0].lo - 1e-4f && tmax <= e->slot[0].hi + 1e-4f && tmax - tmin > 0.2f,
          "every clip stays inside it (%.2f..%.2f) and still travels", (double)tmin, (double)tmax);
    ts_engine_destroy(e);
}

/* ---- 10d. raw pads: the Pads screen reports which pad, whatever octave the Move is in ------------------ */

static void test_raw_pads(void)
{
    printf("raw pads\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.normalise = 0; p.turntable = 0.5f; p.drag = 0.0f; p.pads = 1; p.clip = 0; p.note_gate = 1;
    ts_engine_set_params(e, &p);
    static float buf[44100 * 3];

    ts_engine_pad_cuts(e, 0x0001);
    CHECK(e->pad_held == 0, "cut pads are not heard from a screen that has not said it is there");
    ts_engine_pad_raw(e, 1);
    CHECK(e->pad_raw > 0, "the Pads screen says it is there");
    ts_engine_note(e, 60, 100, 1);
    ts_engine_note(e, 70, 100, 1);
    ts_engine_note(e, 45, 100, 1);
    CHECK(e->notes_held == 0 && e->p.clip == 0 && e->p.sample == 0, "...and the notes the Move sends for the pads, moved about by its octave, are ignored");
    CHECK(e->last_note == 45, "(the last one is still shown, to see what it sends)");
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f, "...so nothing is heard from them");

    ts_engine_pad_cuts(e, 0x0001);
    render_n(e, buf, 2048);
    CHECK(e->pad_held == 1 && buf[2047] > 0.45f, "a cut pad held on the Pads screen cuts the sound in");
    ts_engine_pad_cuts(e, 0x8003);
    CHECK(e->pad_held == 3, "three held");
    ts_engine_pad_cuts(e, 0x10000 | 0x0004);
    CHECK(e->pad_held == 1 && e->pad_cuts == 0x0004, "only the 16 cut pads count");
    ts_engine_pad_cuts(e, 0);
    render_n(e, buf, 2048);
    CHECK(e->pad_held == 0 && buf[2047] == 0.0f, "let go: silent");

    /* it holds only while it is renewed: a screen that goes away cannot leave the notes ignored, or a cut stuck */
    ts_engine_pad_cuts(e, 0x0001);
    for (int i = 0; i < 6; i++) { render_n(e, buf, 11025); ts_engine_pad_raw(e, 1); }        /* 1.5 s, renewed each quarter second */
    CHECK(e->pad_raw > 0 && e->pad_held == 1, "renewed, it stays");
    render_n(e, buf, 44100);
    CHECK(e->pad_raw > 0, "(for a second after the last)");
    render_n(e, buf, 44100);
    CHECK(e->pad_raw == 0 && e->pad_held == 0 && e->pad_cuts == 0, "not renewed, it lets go by itself, and the held cut with it");
    render_n(e, buf, 2048);
    CHECK(buf[2047] == 0.0f, "...silent");
    ts_engine_note(e, 70, 100, 1);
    CHECK(e->notes_held == 1, "the notes count again");
    ts_engine_note(e, 70, 0, 0);

    ts_engine_pad_raw(e, 1);
    ts_engine_pad_cuts(e, 0x0003);
    ts_engine_pad_raw(e, 0);
    CHECK(e->pad_raw == 0 && e->pad_held == 0, "switched off at once when the screen closes");

    /* with a clip and the transport stopped, the first held cut starts the clip from its start */
    p.clip = 3;
    ts_engine_set_params(e, &p);
    e->mclock = 1.7;
    ts_engine_pad_raw(e, 1);
    ts_engine_pad_cuts(e, 0x0010);
    float blk[128];
    ts_engine_render(e, blk, 128);
    CHECK(e->mclock < 0.05, "a held cut pad plays the clip from its beginning (%.3f beats)", e->mclock);
    ts_engine_pad_cuts(e, 0);
    ts_engine_destroy(e);
}

/* ---- 10e. clips.bin: the example set's clips, imported ---------------------------------------------- */

static void put_clip(FILE *f, int tag, int len, float (*pos_of)(double), int (*gate_of)(double))
{
    int n = len * TS_CLIP_RES;
    uint8_t h[4] = { (uint8_t)tag, (uint8_t)len, (uint8_t)(n & 255), (uint8_t)(n >> 8) };
    fwrite(h, 1, 4, f);
    for (int i = 0; i < n; i++) {
        uint16_t v = (uint16_t)(pos_of((double)i / TS_CLIP_RES) * 65535.0f);
        uint8_t b[2] = { (uint8_t)(v & 255), (uint8_t)(v >> 8) };
        fwrite(b, 1, 2, f);
    }
    for (int i = 0; i < n; i++) { uint8_t g = (uint8_t)gate_of((double)i / TS_CLIP_RES); fwrite(&g, 1, 1, f); }
}
static float pos_up8(double beat) { return (float)(0.1 + 0.6 * fmod(beat, 8.0) / 8.0); }          /* 0.1 up to 0.7 over 8 beats */
static float pos_mid16(double beat) { return (float)(0.5 + 0.4 * sin(beat * 0.7)); }
static float pos_flat(double beat) { (void)beat; return 0.25f; }
static int gate_alt(double beat) { return ((int)floor(beat)) % 2 == 0; }                            /* on for the first of every two beats */
static int gate_all(double beat) { (void)beat; return 1; }

static void test_clip_files(void)
{
    printf("clips.bin\n");
    const char *path = "build/test/clips_test.bin";
    FILE *f = fopen(path, "wb");
    if (!f) { CHECK(0, "cannot write %s", path); return; }
    fwrite("TSCL\x01\x03", 1, 6, f);
    put_clip(f, 1, 8, pos_up8, gate_alt);                    /* for Ahhh, two bars */
    put_clip(f, 2, 16, pos_mid16, gate_all);                 /* for Fresssh, four bars */
    put_clip(f, 0, 4, pos_flat, gate_alt);                   /* for anything, one bar */
    fclose(f);

    ts_engine_t *e = ts_engine_create(44100.0f);
    CHECK(ts_engine_load_clips(e, path) == 3, "three clips loaded");
    CHECK(e->clip_data[0].len == 8 && e->clip_data[0].tag == 1 && e->clip_data[1].len == 16 && e->clip_data[1].tag == 2 && e->clip_data[2].len == 4 && e->clip_data[2].tag == 0,
          "with their lengths and which sample each is for");

    /* each clip is on the pads of the sample it was made for; one for anything is on all of them; the others are empty */
    CHECK(e->bank[0][0] == 1 && e->bank[0][1] == 3 && e->bank[0][2] == 0 && e->bank[1][0] == 2 && e->bank[1][1] == 3 && e->bank[1][2] == 0 &&
          e->bank[2][0] == 3 && e->bank[2][1] == 0 && e->bank[5][0] == 3 && e->bank[5][1] == 0,
          "Ahhh's pads hold the Ahhh clip, then the any-sample one; Fresssh's the Fresssh clip, then the same; a user sample only the any-sample one");
    CHECK(e->p.sample == 0 && e->nclips == 2, "(the selected sample's bank has two clips)");
    CHECK(fabsf(ts_clip_pos(e, 1, 0.0) - 0.1f) < 1e-3f && fabsf(ts_clip_pos(e, 1, 4.0) - 0.4f) < 1e-3f && fabsf(ts_clip_pos(e, 1, 8.0 + 4.0) - 0.4f) < 1e-3f,
          "Ahhh, pad 1: the playhead is the data, and the clip loops over its own length (8 beats)");
    CHECK(ts_clip_cut(e, 1, 0.5) == 1 && ts_clip_cut(e, 1, 1.5) == 0 && ts_clip_cut(e, 1, 8.5) == 1, "...and the cuts are the notes");
    CHECK(ts_clip_pos(e, 3, 1.0) == 0.0f && ts_clip_cut(e, 3, 1.0) == 0, "(pad 3 has nothing)");
    {                                                                          /* no clip is read-only: an imported one can be deleted, from its pad only, and it stays gone */
        ts_engine_t *e2 = ts_engine_create(44100.0f);
        ts_engine_load_clips(e2, path);
        CHECK(ts_engine_clear_clip(e2, 1) == 0 && !bank_clip(e2, 0, 1) && bank_clip(e2, 0, 2) && e2->removed[0][0] && bank_clip(e2, 1, 1),
              "an imported clip can be deleted: from that pad of that sample only");
        ts_engine_destroy(e2);
    }
    ts_params_t p = e->p;
    p.sample = 1;
    ts_engine_set_params(e, &p);
    CHECK(e->nclips == 2 && fabsf(ts_clip_pos(e, 1, 3.3) - ts_clip_pos(e, 1, 3.3 + 16.0)) < 1e-4f && fabsf(ts_clip_pos(e, 1, 3.3) - ts_clip_pos(e, 1, 3.3 + 8.0)) > 1e-3f && ts_clip_cut(e, 1, 15.9) == 1,
          "Fresssh, pad 1: a 16-beat clip loops over 16 beats");

    /* which clips auto plays: the ones on the selected sample's pads */
    int seen[3][4] = { { 0 } }, nodup = 1;
    for (int slot = 0; slot < 3; slot++) {
        p = e->p;
        p.sample = slot == 2 ? 2 : slot;
        ts_engine_set_params(e, &p);
        int prev = 0;
        for (long long bar = 0; bar < 200; bar++) {
            int c = ts_clip_auto(e, bar);
            seen[slot][c]++;
            if (slot < 2 && bar > 0) nodup &= c != prev;
            prev = c;
        }
    }
    CHECK(seen[0][1] > 0 && seen[0][2] > 0 && seen[0][3] == 0 && seen[1][1] > 0 && seen[1][2] > 0 && seen[1][3] == 0, "Ahhh and Fresssh: auto plays the two clips on their pads");
    CHECK(seen[2][1] == 200 && seen[2][0] == 0, "a user sample has one: auto plays that one");
    CHECK(nodup, "with two to choose from it is the other one every bar");

    /* playing: Legato, the position the song has reached; a clip made for the sample plays where it says, one for anything is spread */
    static int16_t gap[44100];
    for (int i = 0; i < 44100; i++) gap[i] = (i > 13230 && i < 30870) ? (int16_t)(20000.0f * sinf(0.2f * (float)i)) : 0;      /* the sound is 0.3..0.7 */
    size_t gsz = make_wav(g_wav, gap, 44100, 44100, 1, 16, 1);
    ts_engine_load_wav_mem(e, 0, "ahhh.wav", g_wav, gsz);
    ts_engine_load_wav_mem(e, 1, "fresssh.wav", g_wav, gsz);
    ts_engine_load_wav_mem(e, 2, "user.wav", g_wav, gsz);
    ts_params_default(&p); p.swing = 0.0f;
    p.drag = 0.0f; p.pads = 0; p.normalise = 0; p.clip = 1; p.sample = 0;
    ts_engine_set_params(e, &p);
    float blk[128];
    double spb = 120.0 / (60.0 * 44100.0);
    ts_engine_sync_beat(e, 21.0);                             /* the song is at beat 21: 5 beats into the 8-beat loop */
    ts_engine_render(e, blk, 128);
    CHECK(fabs(e->target - ts_clip_pos(e, 1, 21.0 + 128 * spb)) < 1e-4 && fabs(e->target - 0.475) < 0.01,
          "Legato: Ahhh's clip plays from where the song is (beat 21 is 5 beats into its 8), the playhead at %.3f", e->target);
    p.clip = 2;
    ts_engine_set_params(e, &p);
    ts_engine_sync_beat(e, 21.0);
    ts_engine_render(e, blk, 128);
    CHECK(e->target >= 0.3 - 1e-3 && e->target <= 0.7 + 1e-3 && fabs(e->target - (e->slot[0].lo + (e->slot[0].hi - e->slot[0].lo) * ts_clip_pos(e, 2, 21.0 + 128 * spb))) < 1e-3,
          "a clip for any sample is spread over where this one has its sound (%.3f)", e->target);
    p.sample = 1; p.clip = 1;
    ts_engine_set_params(e, &p);
    ts_engine_sync_beat(e, 21.0);
    ts_engine_render(e, blk, 128);
    CHECK(fabs(e->target - ts_clip_pos(e, 1, 21.0 + 128 * spb)) < 1e-4, "a clip made for this sample plays exactly where it says (%.3f)", e->target);

    /* the view: the clip's own length, how many there are, which pads have one */
    char js[8192], s[600];
    double v = 0;
    p.sample = 1; p.clip = 1;
    ts_engine_set_params(e, &p);
    ts_engine_sync_beat(e, 6.0);
    ts_engine_render(e, blk, 128);
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "lb", &v) && v == 16 && json_num(js, "nc", &v) && v == 2, "the view says the clip is 16 beats, and that there are 2 clips");
    json_str(js, "cf", s, sizeof s);
    CHECK(!strncmp(s, "1100000000000000", 16), "...and which pads have a clip (%s)", s);
    json_num(js, "lcol", &v);
    CHECK(fabs(v - 6.0 / 16.0 * 128.0) < 3, "...and the cursor is 6 beats into its 16 (column %g)", v);
    json_str(js, "lng", s, sizeof s);
    int on = 0;
    for (int k = 0; k < TS_LANE; k++) on += s[k] == '1';
    CHECK(on == TS_LANE, "...and the cuts are the clip's notes");
    p.sample = 0; p.clip = 1;
    ts_engine_set_params(e, &p);
    ts_engine_render(e, blk, 128);
    ts_engine_view(e, js, sizeof js);
    json_str(js, "lng", s, sizeof s);
    on = 0;
    for (int k = 0; k < TS_LANE; k++) on += s[k] == '1';
    CHECK(on > 56 && on < 72, "(Ahhh's notes are on for half of its 8 beats: %d of 128 columns)", on);

    /* a pad with nothing on it plays nothing */
    p.clip = 8;
    ts_engine_set_params(e, &p);
    ts_engine_render(e, blk, 128);
    CHECK(e->lane_len == TS_LANE_BEATS && ts_clip_cut(e, 8, 1.0) == 0, "an empty pad plays nothing");

    /* bad files are refused and the clips already there stay */
    const char *bad = "build/test/clips_bad.bin";
    struct { const char *name; const char *bytes; size_t n; } cases[] = {
        { "wrong magic", "TSCX\x01\x01\x04\x04\x00\x02", 10 },
        { "wrong version", "TSCL\x09\x01\x04\x04\x00\x02", 10 },
        { "no clips", "TSCL\x01\x00", 6 },
        { "too many clips", "TSCL\x01\x11", 6 },
        { "too short a clip", "TSCL\x01\x01\x00\x03\x80\x01", 10 },
        { "a length that does not fit its data", "TSCL\x01\x01\x00\x04\x00\x01", 10 },
        { "a tag that is not one", "TSCL\x01\x01\x07\x04\x00\x02", 10 },
        { "cut short", "TSCL\x01\x01\x00\x04\x00\x02\x00\x00", 12 },
        { "empty", "", 0 },
    };
    int refused = 1;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        FILE *bf = fopen(bad, "wb");
        fwrite(cases[i].bytes, 1, cases[i].n, bf);
        fclose(bf);
        int r = ts_engine_load_clips(e, bad);
        if (r >= 0 || e->clip_data[0].len != 8 || e->clip_data[1].len != 16 || e->bank[0][0] != 1) { refused = 0; fprintf(stderr, "  (%s: %d)\n", cases[i].name, r); }
    }
    CHECK(refused, "bad or truncated files are refused and the clips already there stay");
    CHECK(ts_engine_load_clips(e, "build/test/nonexistent.bin") < 0 && ts_engine_load_clips(NULL, path) < 0 && ts_engine_load_clips(e, NULL) < 0 && e->clip_data[2].len == 4, "a missing file is no clips, not a crash");
    ts_engine_destroy(e);
}

/* ---- 10f. a file loads once, however many folders are searched ----------------------------------------- */

static void test_dedupe(void)
{
    printf("samples, once\n");
    const char *dir = "build/test/dedupe";                    /* scripts/test.sh makes it */
    static int16_t pcm[4410];
    make_sample(pcm, 4410, SMP_SINE, 0.4f);
    size_t sz = make_wav(g_wav, pcm, 4410, 44100, 1, 16, 1);
    const char *names[] = { "ahhh.wav", "fresssh.wav", "apollo.wav", "bravo.wav" };
    for (int i = 0; i < 4; i++) {
        char path[300];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "wb");
        if (!f) { CHECK(0, "cannot write %s", path); return; }
        fwrite(g_wav, 1, sz, f);
        fclose(f);
    }
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_load_dir(e, dir);
    ts_engine_load_dir(e, dir);                               /* the module's folder is also one of the fallback folders */
    ts_engine_load_dir(e, "build/test/dedupe/../dedupe");
    int user = 0;
    for (int i = 2; i < TS_SLOTS; i++) user += e->slot[i].loaded;
    CHECK(e->slot[0].loaded && e->slot[1].loaded && user == 2, "the same folder found again loads nothing more (User slots filled: %d)", user);
    CHECK(strcmp(e->slot[2].name, "APOLLO") == 0 && strcmp(e->slot[3].name, "BRAVO") == 0 && !e->slot[4].loaded && !e->slot[5].loaded,
          "so the files are listed once each: %s, %s", e->slot[2].name, e->slot[3].name);
    ts_engine_destroy(e);
}

/* ---- 10h. the generator: clips from the vocabulary of scratching ----------------------------------------------- */

static void clip_stats(const ts_clip_t *c, double *duty, double *travel, double *lo, double *hi, int *jumps)
{
    int open = 0;
    double tr = 0.0, mn = 9.0, mx = -9.0;
    int j = 0;
    for (int i = 0; i < c->n; i++) {
        open += c->gate[i];
        double v = c->pos[i] / 65535.0;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (i > 0) { double d = fabs(v - c->pos[i - 1] / 65535.0); tr += d; j += d > 0.2; }
    }
    *duty = (double)open / c->n; *travel = tr; *lo = mn; *hi = mx; *jumps = j;
}

static void test_generator(void)
{
    printf("the generator\n");
    static ts_clip_t a, b, c;
    ts_clip_generate(&a, GS_BABY, 12345, 2);
    ts_clip_generate(&b, GS_BABY, 12345, 2);
    ts_clip_generate(&c, GS_BABY, 12346, 2);
    CHECK(a.n == 2 * 16 * (TS_CLIP_RES / 4) && a.len == 8 && a.tag == 0 && memcmp(&a, &b, sizeof a) == 0, "the same style, seed and length make the same clip, for any sample (%d points, %d beats)", a.n, a.len);
    CHECK(memcmp(a.pos, c.pos, sizeof a.pos) != 0, "another seed makes another clip");
    int lens_ok = 1;
    for (int bars = 0; bars < 6; bars++) {
        ts_clip_generate(&a, GS_ANY, 99, bars);
        int want = bars <= 1 ? 1 : (bars >= 4 ? 4 : 2);
        lens_ok &= a.len == want * 4 && a.n == want * 4 * TS_CLIP_RES;
    }
    CHECK(lens_ok, "one, two or four bars, nothing else");

    /* every style: a real path, cut in and out, back where it started so it loops, no wild jumps */
    static const char *names[GS_COUNT] = { "any", "baby", "cuts", "military", "scribble", "stab", "chirp", "transform", "flare", "crab", "orbit", "drag" };
    int shape_ok = 1, loop_ok = 1, jump_ok = 1, duty_ok = 1, range_ok = 1;
    double dsum = 0.0;
    int ndraw = 0, distinct_ok = 1;
    for (int st = 0; st < GS_COUNT; st++) {
        double dmin = 9.0, dmax = -9.0;
        for (uint32_t seed = 1; seed <= 40; seed++) {
            int bars = st % 3 == 0 ? 1 : (st % 3 == 1 ? 2 : 4);
            ts_clip_generate(&a, st, seed * 7919u, bars);
            double duty, travel, lo, hi;
            int jumps;
            clip_stats(&a, &duty, &travel, &lo, &hi, &jumps);
            range_ok &= lo >= 0.0 && hi <= 1.0 && hi - lo > 0.12;
            shape_ok &= travel > 0.35 * bars;
            loop_ok &= fabs((double)a.pos[0] - (double)a.pos[a.n - 1]) / 65535.0 < 0.2;
            jump_ok &= jumps <= bars * 12;                      /* a point-to-point jump of more than 0.2: only the quick stabs */
            duty_ok &= duty > 0.08;                                  /* (a baby is open all the way through) */
            dmin = fmin(dmin, duty); dmax = fmax(dmax, duty);
            dsum += duty; ndraw++;
            if (seed > 1) { ts_clip_generate(&b, st, (seed - 1) * 7919u, bars); distinct_ok &= memcmp(a.pos, b.pos, (size_t)a.n * 2) != 0 || memcmp(a.gate, b.gate, (size_t)a.n) != 0; }
        }
        if (!(dmin > 0.05)) fprintf(stderr, "  (%s: duty %.2f..%.2f)\n", names[st], dmin, dmax);
    }
    CHECK(range_ok, "every style stays on the sample (0..1) and covers some of it");
    CHECK(shape_ok, "...and really travels");
    CHECK(loop_ok, "...and ends where it starts, so it loops");
    CHECK(jump_ok, "...with no wild jumps (only the stabs are quick)");
    CHECK(duty_ok && dsum / ndraw > 0.3 && dsum / ndraw < 0.9, "...cut in for a good part of the time (on average %.0f%%)", 100.0 * dsum / ndraw);
    CHECK(distinct_ok, "...and every seed is different");

    /* a clip is mostly sound: the fader is open and the playhead is moving for most of it, and it never sits still and silent for a beat or more */
    double worst_mean = 1.0, worst_one = 1.0;
    int long_gap = 0;
    for (int st = 0; st < GS_COUNT; st++) {
        double sum = 0.0;
        for (uint32_t seed = 1; seed <= 60; seed++) {
            ts_clip_generate(&a, st, seed * 104729u, 2);
            double au = gen_audible(&a);
            sum += au;
            worst_one = fmin(worst_one, au);
            int run = 0;
            for (int i = 0; i < a.n; i++) {
                int j = (i + 1) % a.n;
                double d = fabs((double)a.pos[j] - (double)a.pos[i]) / 65535.0 * TS_CLIP_RES;
                if (a.gate[i] && d > 0.05) run = 0; else if (++run >= TS_CLIP_RES) { long_gap++; break; }
            }
        }
        worst_mean = fmin(worst_mean, sum / 60.0);
    }
    CHECK(worst_mean > 0.35 && worst_one > 0.2 && long_gap == 0, "...and mostly sound, with no silent gap of a beat (worst style averages %.0f%%, worst clip %.0f%%, %d long gaps)",
          100.0 * worst_mean, 100.0 * worst_one, long_gap);

    /* the styles are different from one another */
    int style_distinct = 1;
    static ts_clip_t s1, s2;
    for (int x = 1; x < GS_COUNT; x++)
        for (int y = x + 1; y < GS_COUNT; y++) {
            ts_clip_generate(&s1, x, 5, 1);
            ts_clip_generate(&s2, y, 5, 1);
            style_distinct &= memcmp(s1.gate, s2.gate, (size_t)s1.n) != 0 || memcmp(s1.pos, s2.pos, (size_t)s1.n * 2) != 0;
        }
    CHECK(style_distinct, "the eleven styles make different clips");
    double d1, t1, l1, h1;
    int j1;
    ts_clip_generate(&s1, GS_BABY, 3, 1);
    clip_stats(&s1, &d1, &t1, &l1, &h1, &j1);
    double d2;
    ts_clip_generate(&s2, GS_STAB, 3, 1);
    clip_stats(&s2, &d2, &t1, &l1, &h1, &j1);
    CHECK(d1 > 0.4 && d1 < 0.9 && d2 < 0.5, "a baby is open a good part of the time but not all of it (%.0f%%); stabs are short (%.0f%%)", 100 * d1, 100 * d2);
    ts_clip_generate(&s1, GS_CUTS, 3, 1);
    int closed_back = 1;                                         /* forward cuts: the sound is shut while the playhead goes back */
    for (int i = 1; i < s1.n - (TS_CLIP_RES / 4); i++) if (s1.pos[i] + 300 < s1.pos[i - 1]) closed_back &= s1.gate[i] == 0 || i > s1.n - 2 * (TS_CLIP_RES / 4);
    CHECK(closed_back, "cuts: the fader is shut on the pull-back");
    ts_clip_generate(&s1, GS_ANY, 8, 4);
    ts_clip_generate(&s2, GS_ANY, 8, 4);
    CHECK(memcmp(&s1, &s2, sizeof s1) == 0 && s1.len == 16, "any, four bars: a phrase of different styles (and the same every time for a seed)");
    /* the Euclidean rhythm: k hits spread evenly over n steps, whatever the rotation */
    int euclid_ok = 1;
    for (int n = 4; n <= 16; n++)
        for (int k = 1; k <= n; k++)
            for (int rot = 0; rot < n; rot++) {
                int hits = 0;
                for (int i = 0; i < n; i++) hits += euclid_hit(i, k, n, rot);
                euclid_ok &= hits == k;
            }
    CHECK(euclid_ok, "a Euclidean rhythm has exactly k hits in n steps, for every rotation");
    int e38[8] = { 0 };
    for (int i = 0; i < 8; i++) e38[i] = euclid_hit(i, 3, 8, 0);
    CHECK(e38[0] && !e38[1] && !e38[2] && e38[3] && !e38[4] && !e38[5] && e38[6] && !e38[7], "3 hits in 8 steps: x..x..x.");

    /* a phrase of "any" follows the chain: each bar's style is one that can follow the one before; and all of them turn up */
    int chain_ok = 1, seen_style[GS_COUNT] = { 0 }, repeats = 0, pairs = 0;
    for (uint32_t seed = 1; seed <= 400; seed++) {
        gen_t gp;
        gp.c = &s1;
        gp.rng = mix32(seed) | 1u;
        int plan[4], grp[4];
        gen_plan(&gp, GS_ANY, 4, plan, grp);
        for (int bar = 0; bar < 4; bar++) {
            chain_ok &= plan[bar] >= 1 && plan[bar] < GS_COUNT;
            seen_style[plan[bar]]++;
        }
        chain_ok &= plan[0] == plan[1] && plan[0] == plan[3] && k_next[plan[0]][plan[2]] > 0;     /* A A B A, and B follows A in the chain */
        repeats += plan[2] == plan[0];
        pairs++;
    }
    int all_seen = 1;
    for (int st = 1; st < GS_COUNT; st++) all_seen &= seen_style[st] > 0 || st == GS_DRAG;          /* (a drag is a whole bar of two slow strokes: 'any' keeps away from it) */
    CHECK(chain_ok && all_seen, "'any' makes a phrase A A B A where B is a style that follows A, as a scratcher would string them, and uses every style");
    CHECK(repeats < pairs / 4, "...with B a different style most of the time (%d of %d are the same)", repeats, pairs);

    /* a style's phrase holds together: the bars with the same letter are exactly the same; B is not */
    int hold_ok = 1, b_differs = 0;
    static ts_clip_t ph;
    for (uint32_t seed = 1; seed <= 60; seed++) {
        ts_clip_generate(&ph, 1 + (int)(seed % 10), seed * 104729u, 4);
        const int bn = 16 * (TS_CLIP_RES / 4), hn = bn / 2;          /* (the last bar may run out to the end of the sample in its second half) */
        const int b15 = bn - TS_CLIP_RES / 2;                       /* (the record turns a little ahead of the grid, so a bar's end leads into the next bar) */
        hold_ok &= memcmp(ph.pos, ph.pos + bn, (size_t)b15 * 2) == 0 && memcmp(ph.gate, ph.gate + bn, (size_t)b15) == 0 &&
                   memcmp(ph.pos, ph.pos + 3 * bn, (size_t)(hn - TS_CLIP_RES / 2) * 2) == 0;    /* (bar 4's second half may run out; its fader may differ where a turn is read late) */
        b_differs += memcmp(ph.pos, ph.pos + 2 * bn, (size_t)bn * 2) != 0 || memcmp(ph.gate, ph.gate + 2 * bn, (size_t)bn) != 0;
    }
    CHECK(hold_ok && b_differs > 55, "in a four-bar clip of a style bars 1, 2 and 4 are exactly the same, and bar 3 is not (%d of 60)", b_differs);
    /* "any" is a phrase the way the example clips are: no pulse on the beat, and its two bars come back varied */
    {
        double pulse = 0.0, again = 0.0;
        int exact = 0;
        for (uint32_t seed = 1; seed <= 60; seed++) {
            ts_clip_generate(&ph, GS_ANY, seed * 104729u, 4);
            double mean = 0.0, sq = 0.0, c1 = 0.0, c8 = 0.0;
            for (int i = 0; i < ph.n; i++) mean += ph.pos[i];
            mean /= ph.n;
            for (int i = 0; i < ph.n; i++) {
                double dv = ph.pos[i] - mean;
                sq += dv * dv; c1 += dv * (ph.pos[(i + TS_CLIP_RES) % ph.n] - mean); c8 += dv * (ph.pos[(i + 8 * TS_CLIP_RES) % ph.n] - mean);
            }
            pulse += fabs(c1 / sq); again += c8 / sq; exact += memcmp(ph.pos, ph.pos + 8 * TS_CLIP_RES, (size_t)8 * TS_CLIP_RES * 2) == 0;
        }
        int fwd = 0;                                                             /* forwards: a long push, heard, so the word plays */
        for (uint32_t seed = 1; seed <= 40; seed++) {
            ts_clip_generate(&ph, GS_ANY, seed * 7919u, 4);
            int found = 0;
            for (int i = 0; i + 3 * (TS_CLIP_RES / 4) < ph.n && !found; i += TS_CLIP_RES / 8) {
                int j = i + 3 * (TS_CLIP_RES / 4), op = 0;
                for (int q = i; q < j; q++) op += ph.gate[q] != 0;
                found = (int)ph.pos[j] - (int)ph.pos[i] > (int)(0.5 * 65535) && op > (j - i) * 6 / 10;
            }
            fwd += found;
        }
        CHECK(fwd > 25, "'any' lets the record go now and then: a long push through most of the sample, heard (%d of 40 clips)", fwd);
        int none = 0, all = 0;                                                    /* the Forwards setting: 0 none, 1 every bar */
        for (uint32_t seed = 1; seed <= 40; seed++) {
            for (int pass = 0; pass < 2; pass++) {
                ts_clip_generate_for(&ph, GS_ANY, seed * 7919u, 4, NULL, pass ? 1.0f : 0.0f, 120.0f, 0.0f, 0.0f);
                int found = 0;
                for (int i = 0; i + 3 * (TS_CLIP_RES / 4) < ph.n; i += TS_CLIP_RES / 16) {
                    int j = i + 3 * (TS_CLIP_RES / 4), op = 0;
                    for (int q = i; q < j; q++) op += ph.gate[q] != 0;
                    found += (int)ph.pos[j] - (int)ph.pos[i] > (int)(0.55 * 65535) && op > (j - i) * 7 / 10;      /* (a forward: far, and held open) */
                }
                if (pass) all += found > 0; else none += found == 0;
            }
        }
        CHECK(none >= 28 && all == 40, "Forwards at 0: hardly a long held push (%d of 40 clips without one); at 100%% every clip has forwards (%d of 40)", none, all);
        CHECK(pulse / 60 < 0.35 && again / 60 > 0.5 && exact < 30, "'any' has no beat pulse (%.2f), and its two-bar phrase comes back (%.2f) with changes (%d of 60 exact)", pulse / 60, again / 60, exact);
    }
    ts_clip_generate(&ph, GS_BABY, 77, 2);
    CHECK(memcmp(ph.pos, ph.pos + 16 * (TS_CLIP_RES / 4), (size_t)8 * (TS_CLIP_RES / 4) * 2) == 0, "a two-bar clip of one style is one bar, twice (bar the run-out at the end)");
    /* the strokes of a clip are not all the same: their height swells and falls, and where they come back to wanders */
    int varied = 0, counted = 0;
    for (uint32_t seed = 1; seed <= 40; seed++) {
        ts_clip_generate(&ph, GS_BABY, seed * 31u, 2);
        int top_lo = 65535, top_hi = 0, in_peak = 0;
        for (int i = 1; i < ph.n - 1; i++)
            if (ph.pos[i] >= ph.pos[i - 1] && ph.pos[i] > ph.pos[i + 1] && ph.pos[i] > 9000) { top_lo = top_lo < ph.pos[i] ? top_lo : ph.pos[i]; top_hi = top_hi > ph.pos[i] ? top_hi : ph.pos[i]; in_peak++; }
        if (in_peak >= 3) { counted++; varied += (top_hi - top_lo) > 4000; }
    }
    CHECK(counted > 20 && varied * 10 > counted * 7, "a baby's strokes are not all the same height (%d of %d clips vary)", varied, counted);
    ts_clip_generate(NULL, 1, 1, 1);
    ts_clip_generate(&s1, 999, 1, 99);
    ts_clip_generate(&s1, -5, 1, -3);
    CHECK(s1.n == 512 && s1.len == 4, "bad arguments are clamped");

    /* in the engine: Generate makes the try clip, plays it, never touches the others, and never the one playing */
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, TDIR "/rec/recipes.txt");
    remove(TDIR "/rec/recipes.txt");
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.pads = 0; p.drag = 0.0f; p.note_gate = 0; p.normalise = 0;
    CHECK(p.gen_style == 0 && p.gen_bars == 1, "defaults: any style, two bars");
    p.gen_style = GS_FLARE; p.gen_bars = 2;
    ts_engine_set_params(e, &p);
    ts_clip_t before[TS_BUILTIN_CLIPS];
    memcpy(before, e->clip_data, sizeof before);
    CHECK(ts_clip_pos(e, TS_CLIP_TRY, 1.0) == 0.0f && !e->try_valid, "no try clip yet: nothing there");
    CHECK(ts_engine_generate(e) == GS_FLARE && e->try_valid && e->p.clip == TS_CLIP_TRY && e->try_recipe.style == GS_FLARE && e->try_recipe.bars == 4 && e->clip_data[e->try_idx].len == 16,
          "Generate makes a clip in the style and length asked for, and selects it");
    int first_idx = e->try_idx;
    ts_engine_generate(e);
    CHECK(e->try_idx != first_idx && memcmp(before, e->clip_data, sizeof before) == 0, "the next one is built in the other buffer (the one playing is never rewritten), and the real clips are untouched");
    CHECK(e->try_recipe.seed != 0 && ts_clip_pos(e, TS_CLIP_TRY, 5.0) >= 0.0f, "the try clip plays like any other");

    float blk[128];
    double spb = 120.0 / (60.0 * 44100.0);
    int heard = 0, gaps = 0;
    float pmin = 9.0f, pmax = -9.0f;
    for (int b2 = 0; b2 < 3000; b2++) {
        ts_engine_sync_beat(e, 128.0 * spb * b2);
        ts_engine_render(e, blk, 128);
        heard += fabsf(blk[127]) > 0.01f;
        gaps += blk[127] == 0.0f;
        pmin = fminf(pmin, (float)e->pos); pmax = fmaxf(pmax, (float)e->pos);
    }
    CHECK(heard > 300 && gaps > 300 && pmax - pmin > 0.2f, "it plays: cut in and out, the playhead travelling (%d in, %d out, %.2f..%.2f)", heard, gaps, (double)pmin, (double)pmax);
    char js[8192], cf[64];
    double v = 0;
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "cl", &v) && v == TS_CLIP_TRY && json_num(js, "tv", &v) && v == 1 && json_num(js, "tg", &v) && v == GS_FLARE && json_num(js, "lb", &v) && v == 16, "the screen: which clip, that there is a try clip, its style, and its length (16 beats)");

    /* keeping: on the next free pad, or an empty pad; a clip pad plays what is on it */
    int pad = ts_engine_keep(e, 0);
    CHECK(pad == 9 && PADCLIP(e, 9) && PADKEPT(e, 9) && e->p.clip == 9 && e->nclips == 9, "Keep puts the try clip on the first free pad (9), selects it, and it counts");
    CHECK(memcmp(PADCLIP(e, 9)->pos, e->clip_data[e->try_idx].pos, sizeof PADCLIP(e, 9)->pos) == 0, "it is the same clip");
    ts_engine_generate(e);
    ts_engine_clip_pad(e, 13);
    CHECK(PADCLIP(e, 13) && e->p.clip == 13, "an empty pad (13) keeps it where you tapped");
    ts_engine_clip_pad(e, 3);
    CHECK(e->p.clip == 3 && PADCLIP(e, 3) && !PADKEPT(e, 3), "a pad with a clip plays it");
    ts_engine_clip_pad(e, 99);
    ts_engine_clip_pad(e, 0);
    ts_engine_clip_pad(NULL, 3);
    CHECK(e->p.clip == 3, "bad pads are ignored");
    ts_engine_t *empty = ts_engine_create(44100.0f);
    CHECK(ts_engine_keep(empty, 0) == -1 && ts_engine_keep(NULL, 0) == -1, "nothing to keep: refused");
    ts_engine_destroy(empty);
    ts_engine_generate(e);
    CHECK(ts_engine_keep(e, 3) == -1 && ts_engine_keep(e, 99) == -1 && ts_engine_keep(e, 16) == -1, "a taken pad, a bad pad, the Generate pad: refused");
    int filled = e->nclips;
    ts_engine_generate(e);
    int first = ts_engine_keep(e, 0);
    CHECK(first > 0 && ts_engine_keep(e, 0) == first && ts_engine_keep(e, 12) == first && e->nclips == filled + 1 && e->p.clip == first,
          "the same clip is never kept twice: asking again goes to the pad it is on (%d)", first);
    ts_engine_clear_clip(e, first);                                                 /* (put it back as it was) */

    /* auto uses the clips you kept too */
    p = e->p;
    p.clip = TS_CLIP_AUTO; p.sample = 0;
    ts_engine_set_params(e, &p);
    int seen9 = 0, seen13 = 0;
    for (long long bar = 0; bar < 300; bar++) { int cc = ts_clip_auto(e, bar); seen9 += cc == 9; seen13 += cc == 13; }
    CHECK(seen9 > 0 && seen13 > 0, "Auto plays the clips you kept as well as the others");

    /* deleting: an empty pad has nothing to delete (any clip can go: see test_delete_any) */
    CHECK(ts_engine_clear_clip(e, 16) == -1 && ts_engine_clear_clip(NULL, 9) == -1 && ts_engine_clear_clip(e, 99) == -1, "an empty pad has nothing to delete");
    e->p.clip = 9;
    CHECK(ts_engine_clear_clip(e, 0) == 0 && PADCLIP(e, 9) == NULL && !PADKEPT(e, 9) && e->nclips == 9 && e->p.clip == TS_CLIP_AUTO,
          "deleting the selected clip you made empties its pad and goes back to Auto");
    CHECK(ts_engine_clear_clip(e, 13) == 0 && e->nclips == TS_BUILTIN_CLIPS, "...or one named by its pad");
    p = e->p; p.clip = 9; ts_engine_set_params(e, &p);
    ts_engine_render(e, blk, 128);
    CHECK(e->lane_len == TS_LANE_BEATS, "a deleted clip that was selected plays nothing");
    ts_engine_destroy(e);
    (void)cf;
}

/* ---- 10i. a bank of clip pads for every sample ------------------------------------------------------------- */

static void test_unload_files(void)
{
    const char *dir = TDIR "/samples_u";
    MKDIR(dir);
    char path[600];
    int16_t pcm[44100];
    make_sample(pcm, 44100, SMP_DC, 0.4f);
    size_t sz = make_wav(g_wav, pcm, 44100, 44100, 1, 16, 1);
    snprintf(path, sizeof path, "%s/unloaded.txt", dir); remove(path);
    snprintf(path, sizeof path, "%s/mine.wav", dir); remove(path);
    snprintf(path, sizeof path, "%s/user2.wav", dir); remove(path);
    put_file(dir, "mine.wav", g_wav, sz);                                       /* a file somebody copied in */
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_save_dir(e, dir);
    int mask = ts_engine_load_dir(e, dir);
    CHECK((mask & 4) && !strcmp(e->slot[2].file, "mine.wav"), "a copied-in file fills a User slot");
    ts_engine_select_sample(e, 2);
    ts_engine_clear_slot(e, 2);
    ts_engine_service(e);
    static float blk[128];
    ts_engine_render(e, blk, 128);
    snprintf(path, sizeof path, "%s/mine.wav", dir);
    FILE *f = fopen(path, "rb");
    CHECK(!e->slot[2].loaded && f != NULL, "unloading it empties the slot and leaves the file alone");
    if (f) fclose(f);
    ts_engine_destroy(e);
    e = ts_engine_create(44100.0f);
    ts_engine_set_save_dir(e, dir);
    ts_engine_load_dir(e, dir);
    CHECK(!e->slot[2].loaded, "...and the next start does not load it again");
    ts_engine_destroy(e);
    /* what was made here goes with its slot */
    put_file(dir, "user2.wav", g_wav, sz);
    e = ts_engine_create(44100.0f);
    ts_engine_set_save_dir(e, dir);
    mask = ts_engine_load_dir(e, dir);
    CHECK((mask & 8) && !strcmp(e->slot[3].file, "user2.wav"), "a recording (userN.wav) loads into its slot");
    ts_engine_clear_slot(e, 3);
    ts_engine_service(e);
    snprintf(path, sizeof path, "%s/user2.wav", dir);
    f = fopen(path, "rb");
    CHECK(f == NULL, "unloading it deletes its copy");
    if (f) fclose(f);
    ts_engine_destroy(e);
    snprintf(path, sizeof path, "%s/mine.wav", dir); remove(path);
    snprintf(path, sizeof path, "%s/unloaded.txt", dir); remove(path);
}

static void test_pad_runs_clip(void)
{
    printf("a held pad with the transport stopped\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.pads = 0; p.normalise = 0; p.drag = 0.0f; p.note_gate = 1; p.clip = 1;
    ts_engine_set_params(e, &p);
    ts_engine_note(e, 100, 100, 1);
    static float blk[128];
    long open = 0, shut = 0;
    for (int b = 0; b < 700; b++) {                                              /* a bar and a bit at 120 bpm */
        ts_engine_render(e, blk, 128);
        if (b > 20) { if (fabsf(blk[127]) > 0.1f) open++; else shut++; }
    }
    CHECK(open > 100 && shut > 100, "holding a pad with the transport stopped plays the clip with its own cuts (%ld open, %ld shut)", open, shut);
    ts_engine_note(e, 100, 0, 0);
    ts_engine_destroy(e);
}

/* Cue pads: with no clip playing, the bottom row drops the needle at the sample's attacks and plays on at its own speed */
static void test_cues(void)
{
    printf("cue pads\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    static int16_t pcm[44100];
    for (int i = 0; i < 44100; i++) {                                            /* three bursts, 0.05, 0.40 and 0.70 s in, with near silence between */
        double t = i / 44100.0;
        double env = (t >= 0.05 && t < 0.25) || (t >= 0.40 && t < 0.60) || (t >= 0.70 && t < 0.90) ? 0.8 : 0.01;
        pcm[i] = (int16_t)(env * sin(6.2831853 * 440.0 * t) * 32767.0);
    }
    size_t sz = make_wav(g_wav, pcm, 44100, 44100, 1, 16, 1);
    CHECK(ts_engine_load_wav_mem(e, 0, "bursts.wav", g_wav, sz) == TS_WAV_OK, "(a sample with three attacks)");
    ts_params_t p;
    ts_params_default(&p);
    p.clip = 0; p.normalise = 0; p.drag = 0.0f; p.note_gate = 1; p.swing = 0.0f;
    ts_engine_set_params(e, &p);
    static char js[8192];
    char z[64];
    double v = 0;
    ts_engine_view(e, js, sizeof js);
    int cu[8] = { 0 }, asc = json_str(js, "cu", z, sizeof z) && strlen(z) == 16, near40 = 0, near70 = 0;
    for (int i = 0; i < 8 && asc; i++) {
        sscanf(z + 2 * i, "%2x", &cu[i]);
        asc &= i == 0 || cu[i] >= cu[i - 1];
        near40 |= abs(cu[i] - (int)(0.40 * 127)) <= 4;
        near70 |= abs(cu[i] - (int)(0.70 * 127)) <= 4;
    }
    CHECK(asc && abs(cu[0] - (int)(0.05 * 127)) <= 4 && near40 && near70, "the view lists eight cues in order: the start of the sound, then its attacks (%s)", z);
    CHECK(json_num(js, "cs", &v) && v == 1 && json_num(js, "cq", &v) && v == -1, "...and none is playing");

    static float blk[128];
    ts_engine_note(e, TS_PAD_FIRST + 3, 100, 1);
    for (int b = 0; b < 10; b++) ts_engine_render(e, blk, 128);
    double want = cu[3] / 127.0 + 1280.0 / 44099.0;
    CHECK(e->cue_on && e->cue_pad == 3 && fabs(e->cue_pos - want) < 0.012, "a pad drops the needle on its cue and plays on (at %.3f, wanted %.3f)", e->cue_pos, want);
    CHECK(e->gate > 0.99f && fabsf(e->vel_out - 1.0f) < 0.1f, "...with the sound open, at the sample's own speed (%.2f)", (double)e->vel_out);
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "cq", &v) && v == 3, "...and the view says which");
    ts_engine_note(e, TS_PAD_FIRST + 3, 0, 0);
    for (int b = 0; b < 3; b++) ts_engine_render(e, blk, 128);
    CHECK(!e->cue_on && e->gate < 0.01f, "let go: it stops, the sound shut");

    ts_engine_note(e, TS_PAD_FIRST + 0, 100, 1);
    ts_engine_render(e, blk, 128);
    double first = e->cue_pos;
    ts_engine_note(e, TS_PAD_FIRST + 0, 0, 0);
    ts_engine_note(e, TS_PAD_FIRST + 7, 100, 1);
    ts_engine_render(e, blk, 128);
    double last = e->cue_pos;
    ts_engine_note(e, TS_PAD_FIRST + 7, 0, 0);
    CHECK(first < 0.15 && last > first + 0.3, "the first pad is the start of the sound and the last is further on (%.2f, %.2f)", first, last);

    ts_engine_note(e, TS_PAD_FIRST + 5, 100, 1);                                  /* a turn of the knob takes over; the cue carries on from where the hand left it */
    ts_engine_render(e, blk, 128);
    p.turntable = 0.8f;
    ts_engine_set_params(e, &p);
    for (int b = 0; b < 10; b++) ts_engine_render(e, blk, 128);
    CHECK(fabs(e->cue_pos - 0.8) < 0.01, "a turn of the knob moves the playhead, not the cue (%.3f)", e->cue_pos);
    for (int b = 0; b < 250; b++) ts_engine_render(e, blk, 128);                  /* 0.6 s of hand hold in all, and a little more */
    CHECK(e->cue_pos > 0.85 && e->cue_pos < 1.0 + 1e-9, "...and when the hand lets go it plays on from there (%.3f)", e->cue_pos);
    ts_engine_note(e, TS_PAD_FIRST + 5, 0, 0);
    ts_engine_render(e, blk, 128);

    p.clip = 1; p.turntable = 0.0f;                                                /* with a clip playing the pads cut it, as they always did */
    ts_engine_set_params(e, &p);
    ts_engine_note(e, TS_PAD_FIRST + 2, 100, 1);
    for (int b = 0; b < 4; b++) ts_engine_render(e, blk, 128);
    CHECK(!e->cue_on, "with a clip playing the pads are not cues");
    ts_engine_note(e, TS_PAD_FIRST + 2, 0, 0);
    p.clip = 0; p.cues = 0;
    ts_engine_set_params(e, &p);
    ts_engine_note(e, TS_PAD_FIRST + 2, 100, 1);
    for (int b = 0; b < 4; b++) ts_engine_render(e, blk, 128);
    CHECK(!e->cue_on && e->gate > 0.99f, "with Cue Pads off they open the sound where the playhead is, as they did");
    ts_engine_note(e, TS_PAD_FIRST + 2, 0, 0);
    p.cues = 1;
    ts_engine_set_params(e, &p);

    ts_engine_pad_raw(e, 1);                                                      /* the Pads screen reports the pads itself */
    ts_engine_pad_cuts(e, 1u << 6);
    ts_engine_render(e, blk, 128);
    CHECK(e->cue_on && e->cue_pad == 6, "from the Pads screen too");
    ts_engine_pad_cuts(e, 0);
    ts_engine_render(e, blk, 128);
    CHECK(!e->cue_on, "...and it stops when they are let go");
    ts_engine_pad_raw(e, 0);
    ts_engine_destroy(e);
}

static void test_pad_mutes_clip(void)
{
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.pads = 0; p.normalise = 0; p.drag = 0.0f; p.note_gate = 1; p.clip = 2;
    ts_engine_set_params(e, &p);
    static float blk[128];
    double beat = 8.0;
    long open_free = 0, open_held = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) ts_engine_note(e, 100, 100, 1);
        for (int b = 0; b < 700; b++) {
            ts_engine_sync_beat(e, beat);
            beat += 128.0 * 120.0 / (60.0 * 44100.0);
            ts_engine_render(e, blk, 128);
            if (b > 20) { long *n = pass ? &open_held : &open_free; *n += fabsf(blk[127]) > 0.1f; }
        }
    }
    CHECK(open_free > 300 && open_held == 0, "with the transport running a held pad cuts the clip out (%ld open free, %ld held)", open_free, open_held);
    ts_engine_note(e, 100, 0, 0);
    long back = 0;
    for (int b = 0; b < 700; b++) { ts_engine_sync_beat(e, beat); beat += 128.0 * 120.0 / (60.0 * 44100.0); ts_engine_render(e, blk, 128); back += b > 20 && fabsf(blk[127]) > 0.1f; }
    CHECK(back > 300, "...and let go, the clip carries on");
    ts_engine_destroy(e);
}

static void test_attacks(void)
{
    printf("clips that find the attacks\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    static int16_t pcm[44100];
    for (int i = 0; i < 44100; i++) {                                           /* silence, a burst, silence, a louder burst, a tail */
        double t = i / 44100.0;
        double amp = (t >= 0.10 && t < 0.30) ? 0.5 : ((t >= 0.55 && t < 0.80) ? 0.9 : 0.0);
        pcm[i] = (int16_t)(amp * 20000.0 * sin(6.2831853 * 330.0 * t));
    }
    size_t sz = make_wav(g_wav, pcm, 44100, 44100, 1, 16, 1);
    CHECK(ts_engine_load_wav_mem(e, 3, "bursts.wav", g_wav, sz) == 0 && e->slot[3].loaded, "a test sample with two bursts");
    float on[8];
    int k = slot_onsets(&e->slot[3], on, 8);
    CHECK(k == 2 && fabsf(on[0] - 0.09f) < 0.04f && fabsf(on[1] - 0.54f) < 0.05f, "its attacks are found: %d (%.2f %.2f)", k, k > 0 ? on[0] : -1.0f, k > 1 ? on[1] : -1.0f);
    static ts_clip_t c;
    int tagged = 1, at_attack = 1, both = 0;
    for (uint32_t seed = 1; seed <= 60; seed++) {
        ts_clip_generate_for(&c, GS_ANY, seed * 7919u, 4, &e->slot[3], 0.75f, 120.0f, 0.0f, 0.0f);
        tagged &= c.tag == 3;
        int hit0 = 0, hit1 = 0;
        for (int bar = 0; bar < 4; bar += 2) {                                 /* (a phrase starts on bars 1 and 3) */
            float v = 1.0f;                                                     /* the lowest point round the bar line: the record turns a little ahead of it */
            for (int d = -TS_CLIP_RES / 8; d <= TS_CLIP_RES / 8; d++) { int ix = ((bar * 16 * (TS_CLIP_RES / 4) + d) % c.n + c.n) % c.n; v = fminf(v, (float)c.pos[ix] / 65535.0f); }
            int ok = 0;
            for (int j = 0; j < k; j++) if (v >= on[j] - 0.13f && v <= on[j] + 0.15f) { ok = 1; hit0 += j == 0; hit1 += j == 1; }        /* (the path wanders a little round its start) */
            at_attack &= ok;
        }
        both += hit0 > 0 && hit1 > 0;
    }
    CHECK(tagged && at_attack, "every phrase starts on one of the attacks, in the sample's own positions");
    CHECK(both > 5, "and a four-bar clip visits both of them now and then (%d of 60)", both);
    ts_clip_generate_for(&c, GS_BABY, 5, 2, NULL, 0.0f, 120.0f, 0.0f, 0.0f);
    CHECK(c.tag == 0, "with no sample it is the plain generator (not native)");
    ts_engine_destroy(e);
}

/* the module as it ships: no hand-written clips on any pad */
/* Swing: as a clip plays, each eighth is split long-short; the clip itself does not change */
/* Tempo: up to 120 bpm Generate does not look at the song's tempo when it lays out the phrase (a version that moved to triplets and
 * thirty-seconds on slow songs sounded worse and was taken out): the same seed makes the same clip. Above 120 the strokes and the cuts are
 * as long (and as rare) in seconds as at 120, so a fast song does not get more sounds a second than a hand can make. */
static uint32_t clip_fnv(const ts_clip_t *c)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < c->n; i++) { h = (h ^ (c->pos[i] & 255)) * 16777619u; h = (h ^ (c->pos[i] >> 8)) * 16777619u; h = (h ^ c->gate[i]) * 16777619u; }
    return h ^ (uint32_t)c->n ^ ((uint32_t)c->tag << 20);
}

static void test_tempo(void)
{
    printf("tempo\n");
    /* clips made before the tempo was looked at, for a few seeds and styles: the tempo does not change them up to 120 bpm */
    static const uint32_t before[15] = { 1953970742u, 557623256u, 1711664955u, 4219920907u, 4086795089u, 4086795089u, 1724656640u, 2335758686u,
                                          2335758686u, 3515860986u, 236595325u, 236595325u, 4199836569u, 3654799649u, 3654799649u };
    ts_engine_t *e = ts_engine_create(44100.0f);
    int have = ts_engine_load_dir(e, "samples") == 3;
    const int cfg[5][3] = { { GS_ANY, 4, 1 }, { GS_ANY, 2, 0 }, { GS_BABY, 2, 1 }, { GS_FLARE, 4, 0 }, { GS_CUTS, 1, 0 } };
    static ts_clip_t c;
    int same = 1, n_checked = 0;
    for (int k = 0; k < 5; k++)
        for (int sl = -1; sl < 2; sl++) {
            if (sl >= 0 && !have) continue;
            uint32_t seed = 7919u * (uint32_t)(k + 3);
            for (int bi = 0; bi < 8; bi++) {
                const float bpms[8] = { 120.0f, 119.0f, 99.0f, 90.0f, 75.0f, 66.0f, 55.0f, 40.0f };
                float bpm = bpms[bi];
                ts_clip_generate_for(&c, cfg[k][0], seed, cfg[k][1], sl >= 0 ? &e->slot[sl] : NULL, cfg[k][2] ? 0.75f : 0.0f, bpm, 0.0f, 0.0f);
                same &= clip_fnv(&c) == before[k * 3 + (sl + 1)];
                n_checked++;
            }
        }
    CHECK(same && n_checked >= 15, "at 40 to 120 bpm the clips are exactly what they were, the tempo changes nothing (%d checked)", n_checked);
    /* above 120 bpm: the sounds a second stay where they are at 120 (a hand tops out near 6), and the clips are not what the grid alone makes */
    double rate[4] = { 0, 0, 0, 0 }, beats[4] = { 0, 0, 0, 0 };
    const float fast[4] = { 120.0f, 140.0f, 160.0f, 200.0f };
    int differs = 0;
    for (int bi = 0; bi < 4; bi++)
        for (uint32_t seed = 1; seed <= 40; seed++) {
            ts_clip_generate_for(&c, GS_ANY, seed * 7919u, 4, NULL, 0.0f, fast[bi], 0.0f, 0.0f);
            beats[bi] += c.len;
            for (int i = 0; i < c.n; i++) rate[bi] += c.gate[i] && !c.gate[(i + c.n - 1) % c.n];
            if (bi == 2) { static ts_clip_t at120; ts_clip_generate_for(&at120, GS_ANY, seed * 7919u, 4, NULL, 0.0f, 120.0f, 0.0f, 0.0f); differs += clip_fnv(&c) != clip_fnv(&at120); }
        }
    double per_s[4];
    for (int bi = 0; bi < 4; bi++) per_s[bi] = rate[bi] / beats[bi] * fast[bi] / 60.0;
    CHECK(per_s[1] > 0.85 * per_s[0] && per_s[1] < 1.15 * per_s[0] && per_s[2] > 0.85 * per_s[0] && per_s[2] < 1.15 * per_s[0] && per_s[3] > 0.8 * per_s[0] && per_s[3] < 1.2 * per_s[0],
          "the sounds a second hold at 120 on fast songs, they do not climb with the tempo (120 bpm: %.1f, 140: %.1f, 160: %.1f, 200: %.1f)", per_s[0], per_s[1], per_s[2], per_s[3]);
    CHECK(differs > 35, "...because a clip made at 160 is another clip than the same seed at 120 (%d of 40)", differs);
    ts_engine_destroy(e);
}

/* Finish: the clip ends with the whole sample played through at its own speed, then a quick shut pull back */
static void test_finish(void)
{
    printf("finish\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 3, "one.wav", SMP_SINE, 0.7f, 44100);                         /* one second: at 120 bpm 0.125 sample lengths to a sixteenth */
    static ts_clip_t c;
    int ends = 0, speed_ok = 1, shut = 1, none_at_0 = 0, n = 40;
    for (uint32_t seed = 1; seed <= (uint32_t)n; seed++) {
        for (int pass = 0; pass < 2; pass++) {
            ts_clip_generate_for(&c, GS_ANY, seed * 7919u, 2, &e->slot[3], 0.0f, 120.0f, pass ? 1.0f : 0.0f, 0.0f);
            const int q = TS_CLIP_RES / 4, last_bar = 16 * q;
            int a = last_bar + 10 * q, b = last_bar + 13 * q, op = 0;         /* three sixteenths of the play-out, clear of the turns at its ends */
            for (int i = a; i < b; i++) op += c.gate[i] != 0;
            double rise = ((double)c.pos[b] - (double)c.pos[a]) / 65535.0, per16 = rise / ((double)(b - a) / q);
            int finishes = op > (b - a) * 9 / 10 && per16 > 0.08 && per16 < 0.18;
            if (pass) {
                ends += finishes;
                speed_ok &= !finishes || (per16 > 0.10 && per16 < 0.15);
                int s0 = last_bar + 15 * q + q / 4;
                int closed = 1;
                for (int i = s0; i < s0 + q / 2 && i < c.n; i++) closed &= c.gate[i] == 0;
                shut &= closed;
            } else none_at_0 += finishes;
        }
    }
    CHECK(ends >= n - 2, "Finish at 100%%: every clip ends with the sample played through, fader open, rising (%d of %d)", ends, n);
    CHECK(speed_ok, "...at the sample's own speed (0.125 sample lengths a sixteenth at 120 bpm for a one second sample)");
    CHECK(shut, "...and the pull back after it has the fader shut");
    CHECK(none_at_0 < n / 4, "Finish at 0 leaves clips as they were (%d of %d end that way anyway)", none_at_0, n);
    /* a long sample (2.2 s, like a recording) is played right to its end: at its own speed where three quarters of a bar allows (75 bpm:
     * 10 sixteenths), a little faster where it does not (120 bpm: 16 sixteenths of sound in 12) */
    load_sample(e, 4, "long.wav", SMP_SINE, 0.7f, 97020);
    for (int pass = 0; pass < 2; pass++) {
        float bpm = pass ? 120.0f : 75.0f;
        int reach = 0, open_ok = 1, cnt = 0;
        for (uint32_t seed = 1; seed <= 30; seed++) {
            ts_clip_generate_for(&c, GS_ANY, seed * 7919u, 2, &e->slot[4], 0.0f, bpm, 1.0f, 0.0f);
            const int q = TS_CLIP_RES / 4, lb = 16 * q;
            int i14 = lb + 14 * q, op = 0;
            for (int i = lb + 6 * q; i < i14 - q / 2; i++) op += c.gate[i] != 0;
            cnt++;
            reach += (float)c.pos[i14 - q / 4] / 65535.0f > 0.88f;
            open_ok &= op > (i14 - q / 2 - (lb + 6 * q)) * 8 / 10;
        }
        CHECK(reach >= cnt - 1 && open_ok, "a 2.2 s sample at %.0f bpm: Finish plays it right to its end, fader open (%d of %d)", (double)bpm, reach, cnt);
    }
    ts_engine_destroy(e);
}

/* Echo: a stab four times over, each at a lower level of the fader (a gate of 1, 2, 3, 4 steps down through 1.0, 0.7, 0.5, 0.35) */
static void test_echo(void)
{
    printf("echo\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    static ts_clip_t c;
    int echo_clips = 0, levels_ok = 1, plain_ok = 1, n = 40;
    for (uint32_t seed = 1; seed <= (uint32_t)n; seed++) {
        ts_clip_generate_for(&c, GS_ANY, seed * 7919u, 4, NULL, 0.0f, 120.0f, 0.0f, 0.0f);
        for (int i = 0; i < c.n; i++) plain_ok &= c.gate[i] <= 1;               /* no echo: the fader is open or shut, as it was */
        ts_clip_generate_for(&c, GS_ANY, seed * 7919u, 4, NULL, 0.0f, 120.0f, 0.0f, 1.0f);
        /* the notes in order: a level for each opening; an echo is four in a row at levels 1, 2, 3, 4 */
        int seq[256], ns = 0, prev = 0;
        for (int i = 0; i < c.n; i++) {
            if (c.gate[i] && !prev && ns < 256) seq[ns++] = c.gate[i];
            else if (c.gate[i] && prev && c.gate[i] != prev && ns > 0 && ns < 256) seq[ns - 1] = c.gate[i] > seq[ns - 1] ? c.gate[i] : seq[ns - 1];
            prev = c.gate[i];
        }
        int found = 0;
        for (int k = 0; k + 3 < ns; k++) found |= seq[k] == 1 && seq[k + 1] == 2 && seq[k + 2] == 3 && seq[k + 3] == 4;
        echo_clips += found;
        for (int i = 0; i < c.n; i++) levels_ok &= c.gate[i] <= 4;
    }
    CHECK(plain_ok, "with Echo at 0 the fader is only open or shut (no clip changes)");
    CHECK(echo_clips >= 30 && levels_ok, "with Echo at 100%% clips hold a stab repeated at falling levels, 1, 2, 3, 4 (%d of %d)", echo_clips, n);
    /* playback: a lower level is a quieter sound */
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    memset(&e->clip_data[0], 0, sizeof e->clip_data[0]);
    ts_clip_t *k = &e->clip_data[0];
    k->len = 4; k->n = 4 * TS_CLIP_RES; k->tag = 3;
    for (int i = 0; i < k->n; i++) { k->pos[i] = 30000; k->gate[i] = i < TS_CLIP_RES ? 1 : (i < 2 * TS_CLIP_RES ? 3 : (i < 3 * TS_CLIP_RES ? 4 : 0)); }
    memset(e->bank, 0, sizeof e->bank);
    e->bank[0][0] = 1;
    recount_clips(e);
    CHECK(ts_clip_level(e, 1, 0.5) == 1.0f && fabsf(ts_clip_level(e, 1, 1.5) - 0.5f) < 1e-6f && fabsf(ts_clip_level(e, 1, 2.5) - 0.35f) < 1e-6f && ts_clip_level(e, 1, 3.5) == 0.0f &&
          ts_clip_cut(e, 1, 1.5) == 3, "the clip's level at a moment: 1.0, 0.5, 0.35, shut");
    ts_params_t p = e->p;
    p.clip = 1; p.drag = 0.0f; p.note_gate = 1; p.normalise = 0; p.pads = 0; p.turntable = 0.5f;
    ts_engine_set_params(e, &p);
    static float blk[128];
    double beat = 4.0;
    float amp[4] = { 0, 0, 0, 0 };
    for (int b = 0; b < 1200; b++) {
        ts_engine_sync_beat(e, beat);
        ts_engine_render(e, blk, 128);
        double pos = fmod(e->mclock, 4.0);
        int beat_i = (int)pos;
        if (b > 10 && fabs(pos - beat_i - 0.5) < 0.05) amp[beat_i] = fmaxf(amp[beat_i], fabsf(blk[127]));
        beat += 128.0 * 120.0 / (60.0 * 44100.0);
    }
    CHECK(amp[0] > 0.1f && fabsf(amp[1] / amp[0] - 0.5f) < 0.06f && fabsf(amp[2] / amp[0] - 0.35f) < 0.06f && amp[3] < 0.01f,
          "and it plays that much quieter (%.2f, %.2f, %.2f, %.2f)", amp[0], amp[1], amp[2], amp[3]);
    ts_engine_destroy(e);
}

static void test_swing(void)
{
    printf("swing\n");
    CHECK(ts_swing_time(1.25, 0.0f) == 1.25 && ts_swing_time(7.0, 1.0f) == 7.0 && fabs(ts_swing_time(7.5, 1.0f) - 7.5) < 1e-9, "no swing: as it was; the beats and the eighths stay where they are");
    CHECK(fabs(ts_swing_time(2.0 + 1.0 / 3.0, 1.0f) - 2.25) < 1e-9 && fabs(ts_swing_time(2.0 + 0.5 * 0.5833333333, 0.5f) - 2.25) < 1e-6,
          "full swing: the second sixteenth comes two thirds of the way through the eighth (a triplet); half swing, 0.583 of it");
    int mono = 1;
    for (int i = 1; i < 400; i++) mono &= ts_swing_time(i * 0.01, 0.7f) > ts_swing_time((i - 1) * 0.01, 0.7f);
    CHECK(mono, "time only moves forward, whatever the swing");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p = e->p;
    p.clip = 2; p.drag = 0.0f; p.note_gate = 1; p.swing = 1.0f; p.pads = 0;
    ts_engine_set_params(e, &p);
    static float blk[128];
    double beat = 4.0;
    int follows = 1;
    for (int b = 0; b < 200; b++) {
        ts_engine_sync_beat(e, beat);
        ts_engine_render(e, blk, 128);
        double want = (double)e->slot[0].lo + (double)(e->slot[0].hi - e->slot[0].lo) * (double)ts_clip_pos(e, 2, ts_swing_time(e->mclock, 1.0f));
        follows &= fabs(e->target - want) < 1e-6;
        beat += 128.0 * 120.0 / (60.0 * 44100.0);
    }
    CHECK(follows, "a clip plays at the swung time");
    char js[8192];
    ts_engine_view(e, js, sizeof js);
    CHECK(strstr(js, "\"sw\":1.00") != NULL, "the view says the swing");
    p.swing = 9.0f; ts_engine_set_params(e, &p);
    CHECK(e->p.swing == 1.0f, "...clamped");
    ts_engine_destroy(e);
}

/* Humanise: a clip plays a little looser (its timing, its cuts, their level, the size of its strokes); what is stored never changes.
 * The same clip, the same pass and the same setting always sound the same, and each pass round the clip is different. */
static void run_humanised(float h, int blocks, float *tgt, float *gate, int *blocks_per_pass)
{
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "dc.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p = e->p;
    p.clip = 2; p.drag = 0.0f; p.note_gate = 1; p.swing = 0.0f; p.humanise = h; p.pads = 0;
    ts_engine_set_params(e, &p);
    const ts_clip_t *k = bank_clip(e, 0, 2);
    *blocks_per_pass = k ? (int)lround((double)k->len * 60.0 / 120.0 * 44100.0 / 128.0) : 0;
    static float blk[128];
    double beat = 4.0;
    for (int b = 0; b < blocks; b++) {
        ts_engine_sync_beat(e, beat);
        ts_engine_render(e, blk, 128);
        tgt[b] = (float)e->target;
        gate[b] = e->gate;
        beat += 128.0 * 120.0 / (60.0 * 44100.0);
    }
    ts_engine_destroy(e);
}

static void test_humanise(void)
{
    printf("humanise\n");
    enum { N = 9000 };
    static float t0[N], g0[N], t0b[N], g0b[N], t1[N], g1[N], t1b[N], g1b[N];
    int pass = 0, pass_b = 0;
    run_humanised(0.0f, N, t0, g0, &pass);
    run_humanised(0.0f, N, t0b, g0b, &pass_b);
    run_humanised(1.0f, N, t1, g1, &pass_b);
    CHECK(pass > 100 && 2 * pass < N, "(a clip %d blocks long)", pass);
    CHECK(memcmp(t0, t0b, sizeof t0) == 0 && memcmp(g0, g0b, sizeof g0) == 0, "at 0 a clip plays exactly as written, every time");
    run_humanised(1.0f, N, t1b, g1b, &pass_b);
    CHECK(memcmp(t1, t1b, sizeof t1) == 0 && memcmp(g1, g1b, sizeof g1) == 0, "at 100%% it is the same every time too (the looseness is a function of the clip and the pass)");
    double dt = 0.0, open0 = 0.0, open1 = 0.0, maxd = 0.0;
    int gate_moved = 0;
    for (int b = 0; b < N; b++) {
        double d = fabs((double)t1[b] - (double)t0[b]);
        dt += d;
        maxd = d > maxd ? d : maxd;
        open0 += g0[b] > 0.5f;
        open1 += g1[b] > 0.5f;
        gate_moved += fabsf(g1[b] - g0[b]) > 0.01f;
    }
    dt /= N;
    CHECK(dt > 0.003 && dt < 0.12, "at 100%% the playhead is somewhere else, but not far (on average %.4f of the sample, at most %.3f)", dt, maxd);
    CHECK(gate_moved > 20 && fabs(open1 - open0) / N < 0.06, "...the cuts move (%d blocks differ) without the sound opening more or less (%.1f%% against %.1f%% of the time)", gate_moved, 100.0 * open1 / N, 100.0 * open0 / N);
    double again0 = 0.0, again1 = 0.0;
    for (int b = 0; b < pass; b++) { again0 += fabs((double)t0[b] - (double)t0[b + pass]); again1 += fabs((double)t1[b] - (double)t1[b + pass]); }
    again0 /= pass; again1 /= pass;
    CHECK(again0 < 0.003 && again1 > 2.0 * again0 + 0.002, "each pass round the clip is different when it is humanised, and the same when it is not (%.4f against %.4f)", again0, again1);
    double lo = 1.0, soft = 0.0;
    for (int b = 1; b + 1 < N; b++)                                               /* (steady levels: not the 2 ms ramps of a cut opening or closing) */
        if (g1[b] > 0.05f && fabsf(g1[b] - g1[b - 1]) < 0.005f && fabsf(g1[b] - g1[b + 1]) < 0.005f) { lo = g1[b] < lo ? g1[b] : lo; soft += g1[b] < 0.95f; }
    CHECK(soft > 0 && lo > 0.6, "the cuts are a little softer, never much (quietest open level %.2f)", lo);
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_params_t p = e->p;
    p.humanise = 9.0f; ts_engine_set_params(e, &p);
    CHECK(e->p.humanise == 1.0f && e->p.humanise <= 1.0f, "Humanise is clamped to 0..1");
    ts_params_t d;
    ts_params_default(&d);
    CHECK(d.humanise == 0.0f, "it starts at 0: clips play as written");
    ts_engine_destroy(e);
}

static void test_empty_pads(void)
{
    printf("as shipped: empty pads\n");
    ts_builtins_on_pads = 0;
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.7f, 44100);
    int any = 0;
    for (int s = 0; s < TS_SLOTS; s++) for (int pad = 1; pad <= TS_NUM_CLIPS; pad++) any |= e->bank[s][pad - 1] != 0;
    CHECK(!any && e->nclips == 0, "every sample starts with empty pads");
    CHECK(ts_clip_auto(e, 3) == 0, "...so Auto has nothing to play");
    ts_engine_generate(e);
    CHECK(ts_engine_keep(e, 0) == 1 && PADKEPT(e, 1) && e->nclips == 1, "the first clip you keep goes on pad 1");
    ts_engine_destroy(e);
    ts_builtins_on_pads = 1;
}

static void test_delete_any(void)
{
    printf("deleting any clip\n");
    const char *lib = TDIR "/rec/recipes_any.txt";
    remove(lib);
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, lib);
    CHECK(PADCLIP(e, 3) && PADCLIP(e, 4), "Ahhh starts with its built-in clips");
    CHECK(ts_engine_clear_clip(e, 3) == 0 && PADCLIP(e, 3) == NULL && PADCLIP(e, 2) && PADCLIP(e, 4) && e->nclips == TS_BUILTIN_CLIPS - 1, "a built-in clip in between can be deleted, and the others stay where they were");
    ts_engine_select_sample(e, 1);
    CHECK(PADCLIP(e, 3) != NULL, "...for that sample only: the others still have it");
    ts_engine_select_sample(e, 0);
    e->p.clip = 5;
    CHECK(ts_engine_clear_clip(e, 0) == 0 && e->p.clip == TS_CLIP_AUTO && PADCLIP(e, 5) == NULL, "the selected one: back to Auto");
    ts_engine_service(e);
    ts_engine_destroy(e);
    e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, lib);
    ts_engine_load_recipes(e);
    CHECK(PADCLIP(e, 3) == NULL && PADCLIP(e, 5) == NULL && PADCLIP(e, 4) && PADCLIP(e, 1) && e->nclips == TS_BUILTIN_CLIPS - 2, "a new instance has them gone too");
    ts_engine_generate(e);
    CHECK(ts_engine_keep(e, 0) == 3 && PADCLIP(e, 3) && PADKEPT(e, 3) && !e->removed[0][2], "the next clip you keep takes the first free pad");
    int seen = 0;
    for (long long bar = 0; bar < 200; bar++) { int c = ts_clip_auto(e, bar); seen += c == 3; seen -= 1000 * (c == 5); }
    CHECK(seen > 0, "Auto plays the clips that are left, gaps and all");
    ts_engine_destroy(e);
    remove(lib);
}

static void test_unload_drops_clips(void)
{
    printf("unloading a sample\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.7f, 44100);
    load_sample(e, 2, "mine.wav", SMP_SINE, 0.7f, 44100);
    ts_engine_select_sample(e, 2);
    ts_engine_generate(e);
    int kept = ts_engine_keep(e, 0);
    ts_engine_select_sample(e, 0);
    ts_engine_generate(e);
    int other = ts_engine_keep(e, 0);
    CHECK(kept > 0 && other > 0, "a clip kept on a User sample and one on Ahhh");
    ts_engine_select_sample(e, 2);
    ts_engine_clear_slot(e, 2);
    ts_engine_service(e);
    static float blk[128];
    ts_engine_render(e, blk, 128);
    CHECK(!e->slot[2].loaded && e->bank[2][kept - 1] == 0 && e->nclips == 0, "unloading a User sample takes the clips kept for it with it (%d left)", e->nclips);
    ts_engine_select_sample(e, 0);
    CHECK(e->bank[0][other - 1] != 0 && e->kept[e->bank[0][other - 1] - 1].used, "...and nothing kept for another sample");
    ts_engine_clear_slot(e, 0);
    ts_engine_clear_slot(e, 1);
    ts_engine_service(e);
    ts_engine_render(e, blk, 128);
    CHECK(e->slot[0].loaded, "Ahhh and Fresssh cannot be unloaded");
    ts_engine_destroy(e);
}

static void test_banks(void)
{
    printf("a bank of clips for every sample\n");
    const char *lib = TDIR "/rec/banks.txt";
    remove(lib);
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, lib);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.pads = 0; p.note_gate = 1; p.normalise = 0; p.drag = 0.0f;
    ts_engine_set_params(e, &p);
    CHECK(e->bank_last[0] == TS_CLIP_AUTO && e->bank_last[5] == TS_CLIP_AUTO, "every sample starts on Auto");

    /* each sample remembers the clip it last had */
    p = e->p; p.sample = 0; p.clip = 3; ts_engine_set_params(e, &p);
    p = e->p; p.sample = 1; ts_engine_set_params(e, &p);
    CHECK(e->p.sample == 1 && e->p.clip == TS_CLIP_AUTO, "a sample not used yet starts on Auto");
    p = e->p; p.clip = 5; ts_engine_set_params(e, &p);
    p = e->p; p.sample = 0; ts_engine_set_params(e, &p);
    CHECK(e->p.clip == 3, "going back to the first sample brings back its clip (3)");
    p = e->p; p.sample = 1; ts_engine_set_params(e, &p);
    CHECK(e->p.clip == 5, "...and the second its own (5)");
    p = e->p; p.sample = 2; p.clip = 7; ts_engine_set_params(e, &p);
    CHECK(e->p.sample == 2 && e->p.clip == 7, "a clip chosen along with the sample is used");
    p = e->p; p.clip = 0; ts_engine_set_params(e, &p);
    ts_engine_select_sample(e, 0);
    CHECK(e->p.sample == 0 && e->p.clip == 3, "selecting a sample any other way (a pad, Record) does the same");
    ts_engine_select_sample(e, 2);
    CHECK(e->p.clip == 0, "...and Off is remembered too");
    ts_engine_select_sample(e, 2);
    ts_engine_select_sample(e, 99);
    ts_engine_select_sample(e, -1);
    ts_engine_select_sample(NULL, 1);
    CHECK(e->p.sample == 2 && e->p.clip == 0, "the same sample again, or a bad one, changes nothing");

    /* a clip you keep belongs to the sample you were on */
    p = e->p; p.clip = TS_CLIP_AUTO; ts_engine_set_params(e, &p);
    ts_engine_generate(e);
    CHECK(ts_engine_keep(e, 0) == 1 && PADCLIP(e, 1) && PADKEPT(e, 1) && e->nclips == 1, "kept on a user sample, which starts with empty pads: its pad 1");
    ts_engine_select_sample(e, 0);
    CHECK(!PADKEPT(e, 1) && e->nclips == 8, "the first sample's pads do not have it");
    ts_engine_select_sample(e, 2);
    CHECK(PADCLIP(e, 1) && PADKEPT(e, 1) && e->nclips == 1, "the user sample still does");
    char js[8192], cf[64];
    ts_engine_view(e, js, sizeof js);
    json_str(js, "cf", cf, sizeof cf);
    CHECK(!strncmp(cf, "1000000000000000", 16), "the view shows the selected sample's pads (%s)", cf);
    json_str(js, "kp", cf, sizeof cf);
    CHECK(!strncmp(cf, "1000000000000000", 16), "...and which of them you made (%s)", cf);
    ts_engine_select_sample(e, 0);
    ts_engine_view(e, js, sizeof js);
    json_str(js, "cf", cf, sizeof cf);
    CHECK(!strncmp(cf, "1111111100000000", 16), "...and the first sample's own");

    /* auto stays on the selected sample's pads */
    int out_of_bank = 0;
    for (long long bar = 0; bar < 300; bar++) out_of_bank += ts_clip_auto(e, bar) > 8;
    CHECK(out_of_bank == 0, "auto stays inside the selected sample's pads");

    /* the library keeps which sample a clip was kept for */
    ts_engine_select_sample(e, 2);
    ts_engine_service(e);
    FILE *f = fopen(lib, "rb");
    char text[128] = { 0 };
    if (f) { size_t n = fread(text, 1, sizeof text - 1, f); text[n] = 0; fclose(f); }
    int sm = -1, pd = 0, st = 0, br = 0;
    unsigned long sd = 0;
    CHECK(sscanf(text, "%d %d %d %d %lu", &sm, &pd, &st, &br, &sd) == 5 && sm == 2 && pd == 1, "the library says which sample it was kept for (%s)", text);
    ts_engine_destroy(e);
    e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, lib);
    CHECK(ts_engine_load_recipes(e) == 1 && bank_clip(e, 2, 1) && bank_clip(e, 0, 9) == NULL && bank_clip(e, 1, 9) == NULL, "a new instance puts it back on that sample's pad");
    ts_engine_destroy(e);
    remove(lib);

    /* an empty pad plays nothing: no scratching, whatever the playhead */
    e = ts_engine_create(44100.0f);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.5f, 44100);
    p = e->p; p.pads = 0; p.note_gate = 1; p.normalise = 0; p.drag = 0.0f; p.sample = 0; p.clip = 12;
    ts_engine_set_params(e, &p);
    double spb = 120.0 / (60.0 * 44100.0);
    float blk[128];
    float peak = 0.0f;
    for (int b = 0; b < 2000; b++) {
        ts_engine_sync_beat(e, 128.0 * spb * b);
        ts_engine_render(e, blk, 128);
        for (int i = 0; i < 128; i++) peak = fmaxf(peak, fabsf(blk[i]));
    }
    CHECK(peak == 0.0f && fabs(e->pos - e->man) < 1e-9, "an empty pad is selected: nothing plays, and the playhead stays where the Turntable has it");
    p.clip = 3;
    ts_engine_set_params(e, &p);
    for (int b = 0; b < 2000; b++) {
        ts_engine_sync_beat(e, 128.0 * spb * b);
        ts_engine_render(e, blk, 128);
        for (int i = 0; i < 128; i++) peak = fmaxf(peak, fabsf(blk[i]));
    }
    CHECK(peak > 0.05f, "(and a pad with a clip does)");
    ts_engine_destroy(e);
}

/* ---- 10j. loading a file the user picked, and the zoomed view ------------------------------------------------- */

static void test_load_file_and_zoom(void)
{
    printf("a picked file, and the zoomed view\n");
    const char *dir = TDIR "/rec";
    remove(TDIR "/rec/user2.wav");
    remove(TDIR "/rec/user1.wav");
    static int16_t pcm[8000];
    for (int i = 0; i < 8000; i++) pcm[i] = (int16_t)(12000.0f * sinf(0.03f * (float)i));
    size_t sz = make_wav(g_wav, pcm, 8000, 44100, 1, 16, 1);
    FILE *f = fopen(TDIR "/rec/pick_me.wav", "wb");
    fwrite(g_wav, 1, sz, f);
    fclose(f);
    f = fopen(TDIR "/rec/notawav.wav", "wb");
    fputs("this is not a wav file, just text that is long enough to pass the size check for sure, so the decoder must say no", f);
    fclose(f);

    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_save_dir(e, dir);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p); p.swing = 0.0f;
    p.pads = 0; p.note_gate = 0; p.normalise = 0; p.drag = 0.0f; p.turntable = 0.4f; p.clip = 0; p.sample = 3;
    ts_engine_set_params(e, &p);
    int slot = ts_engine_load_file(e, TDIR "/rec/pick_me.wav");
    CHECK(slot == 3 && e->pend_ready == 1 && e->p.sample == 3, "a picked file goes into the selected blank User slot");
    float blk[128];
    ts_engine_render(e, blk, 128);
    CHECK(e->slot[3].loaded && e->slot[3].len == 8000 && !strcmp(e->slot[3].name, "PICK_ME") && e->slot[3].pk[5] > 0, "...handed over to the audio thread like a recording (%d frames, %s)", e->slot[3].len, e->slot[3].name);
    FILE *k = fopen(TDIR "/rec/user2.wav", "rb");
    CHECK(k != NULL, "...and kept as user2.wav so it is there next time");
    if (k) fclose(k);
    ts_engine_t *g2 = ts_engine_create(44100.0f);
    ts_engine_load_dir(g2, dir);
    CHECK(g2->slot[3].loaded && g2->slot[3].len == 8000, "(which loads back into User 2)");
    ts_engine_destroy(g2);

    ts_engine_select_sample(e, 0);
    CHECK(ts_engine_load_file(e, TDIR "/rec/pick_me.wav") == 2 && e->p.sample == 2, "with Ahhh selected it goes into the first empty User slot, and selects it");
    ts_engine_render(e, blk, 128);
    CHECK(e->slot[2].loaded, "(taken)");
    ts_engine_select_sample(e, 3);
    CHECK(ts_engine_load_file(e, TDIR "/rec/no_such_file.wav") < 0 && ts_engine_load_file(e, TDIR "/rec/notawav.wav") < 0 && ts_engine_load_file(e, "") < 0 &&
          ts_engine_load_file(e, NULL) < 0 && ts_engine_load_file(NULL, TDIR "/rec/pick_me.wav") < 0, "a missing file, a file that is not a wav, an empty path: refused");
    CHECK(e->slot[3].loaded && e->slot[3].len == 8000 && e->pend_ready == 0, "(the sample already there stays)");
    ts_engine_load_file(e, TDIR "/rec/pick_me.wav");
    CHECK(ts_engine_load_file(e, TDIR "/rec/pick_me.wav") == -1, "a second file before the audio thread has taken the first is refused");
    ts_engine_render(e, blk, 128);
    ts_engine_service(e);
    ts_engine_destroy(e);
    remove(TDIR "/rec/user1.wav");
    remove(TDIR "/rec/user2.wav");
    remove(TDIR "/rec/pick_me.wav");
    remove(TDIR "/rec/notawav.wav");

    /* the zoomed view: the next few beats, round the clock */
    e = ts_engine_create(44100.0f);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.5f, 44100);
    ts_engine_set_recipes_path(e, "");
    p = e->p; p.swing = 0.0f;
    p.pads = 0; p.clip = 3; p.sample = 0;
    ts_engine_set_params(e, &p);
    char js[8192], z[600];
    double v = 0;
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "zm", &v) && v == 0 && !strstr(js, "\"zp\""), "zoom off: the view has no window");
    ts_engine_set_zoom(e, 2);
    double spb = 120.0 / (60.0 * 44100.0);
    for (int b = 0; b < 700; b++) { ts_engine_sync_beat(e, 6.0 + 128.0 * spb * b); ts_engine_render(e, blk, 128); }
    ts_engine_view(e, js, sizeof js);
    double zs = 0, zw = 0;
    CHECK(json_num(js, "zm", &v) && v == 2 && json_num(js, "zs", &zs) && json_num(js, "zw", &zw) && zw == 2.0 && fabs(zs - (e->mclock - 0.5)) < 2e-3,
          "zoom 2: a window of 2 beats, a quarter of it behind now (it starts at %.3f; now is %.3f)", zs, e->mclock);
    CHECK(json_str(js, "zp", z, sizeof z) && strlen(z) == 256, "...with the playhead's path, two hex digits a column");
    int path_ok = 1;
    for (int c = 0; c < 128; c++) {
        int hv = 0;
        sscanf(z + 2 * c, "%2x", &hv);
        double beat = zs + (c + 0.5) * 2.0 / 128.0;
        path_ok &= abs(hv - (int)(ts_clip_pos(e, 3, beat) * 255.0f + 0.5f)) <= 1;
    }
    CHECK(path_ok, "...it is the clip's own path at those beats");
    char zg[200];
    json_str(js, "zg", zg, sizeof zg);
    int gate_ok = strlen(zg) == 128;
    for (int c = 0; c < 128 && gate_ok; c++) gate_ok &= (zg[c] == '1') == (ts_clip_cut(e, 3, zs + (c + 0.5) * 2.0 / 128.0) != 0);
    CHECK(gate_ok, "...and its cuts");
    ts_engine_set_zoom(e, 3);
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "zm", &v) && v == 3 && json_num(js, "zw", &zw) && zw == 1.0, "the closest zoom is 1 beat");
    ts_engine_set_zoom(e, 1);
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "zw", &zw) && zw == 4.0, "and the widest 4 beats");
    ts_engine_set_zoom(e, 99);
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "zm", &v) && v == 3, "a bad zoom is clamped");
    ts_engine_set_zoom(e, -4);
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "zm", &v) && v == 0 && !strstr(js, "\"zp\""), "...either way");

    /* in Auto the window shows the next bar's clip once it reaches the bar line */
    p = e->p; p.clip = TS_CLIP_AUTO; ts_engine_set_params(e, &p);
    ts_engine_set_zoom(e, 1);
    for (int b = 0; b < 4; b++) { ts_engine_sync_beat(e, 7.3 + 128.0 * spb * b); ts_engine_render(e, blk, 128); }        /* just before the end of bar 2 (beat 8) */
    ts_engine_view(e, js, sizeof js);
    json_num(js, "zs", &zs);
    json_str(js, "zp", z, sizeof z);
    int c_next = ts_clip_auto(e, 2), c_now = ts_clip_auto(e, 1), auto_ok = 1, differs = c_next != c_now;
    for (int c = 0; c < 128; c++) {
        int hv = 0;
        sscanf(z + 2 * c, "%2x", &hv);
        double beat = zs + (c + 0.5) * 4.0 / 128.0;
        int clip = ts_clip_auto(e, (long long)floor(beat / 4.0));
        auto_ok &= abs(hv - (int)(ts_clip_pos(e, clip, beat) * 255.0f + 0.5f)) <= 1;
    }
    CHECK(auto_ok && differs, "in Auto, the window carries on into the next bar's clip (clip %d, then %d)", c_now, c_next);

    /* with no clip there is nothing to show: a flat, silent window */
    p = e->p; p.clip = 0; ts_engine_set_params(e, &p);
    ts_engine_view(e, js, sizeof js);
    json_str(js, "zg", zg, sizeof zg);
    CHECK(strspn(zg, "0") == 128, "no clip: no cuts in the window");
    ts_engine_destroy(e);
}



/* ---- 10g. recording from the audio input into a User slot --------------------------------------------------- */

static void test_recording(void)
{
    printf("recording\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_save_dir(e, TDIR "/rec");
    remove(TDIR "/rec/user1.wav");
    remove(TDIR "/rec/user2.wav");
    load_sample(e, 0, "ahhh.wav", SMP_DC, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.pads = 0; p.clip = 0; p.note_gate = 0; p.normalise = 0; p.drag = 0.0f; p.turntable = 0.5f; p.sample = 3;
    ts_engine_set_params(e, &p);
    static float buf[4096];
    render_n(e, buf, 2048);
    char js[4096];
    ts_engine_view(e, js, sizeof js);
    CHECK(peak_of(buf, 2048) == 0.0f && strstr(js, "\"n\":\"EMPTY\""), "an empty User slot is silent and says EMPTY");

    CHECK(ts_engine_rec_start(e) == 3 && e->p.sample == 3 && e->rec_state == 1, "Record takes the selected User slot");
    float in[128];
    for (int b = 0; b < 40; b++) {
        for (int i = 0; i < 128; i++) in[i] = 0.5f * sinf(0.05f * (float)(b * 128 + i));
        ts_engine_feed_input(e, in, 128);
    }
    double v = 0;
    ts_engine_view(e, js, sizeof js);
    CHECK(json_num(js, "rc", &v) && v == 1 && json_num(js, "rt", &v) && v == 1 && json_num(js, "rl", &v) && v > 20, "the view says it is recording, for how long, and the input level (%g)", v);
    ts_engine_rec_stop(e);
    CHECK(e->rec_state == 2, "stopped");
    ts_engine_service(e);
    CHECK(e->pend_ready == 1 && e->rec_state == 0 && !e->slot[3].loaded, "the service call builds the sample and hands it over (the audio thread has not taken it yet)");
    render_n(e, buf, 128);
    CHECK(e->slot[3].loaded && e->slot[3].len == 5120 && e->pend_ready == 0 && strcmp(e->slot[3].name, "USER 2") == 0,
          "...the audio thread swaps it in (%d frames, %s)", e->slot[3].len, e->slot[3].name);
    {
        float g1 = e->slot[3].data[300] / (0.5f * sinf(0.05f * 300.0f)), g2 = e->slot[3].data[333] / (0.5f * sinf(0.05f * 333.0f));      /* (past the 3 ms fade in) */
        double sq = 0.0;
        for (int i = 0; i < e->slot[3].len; i++) sq += (double)e->slot[3].data[i] * e->slot[3].data[i];
        double rms = sqrt(sq / e->slot[3].len);
        CHECK(fabsf(g1 - g2) < 1e-3f && g1 > 0.0f && fabs(rms - 0.2) < 0.02 && e->slot[3].pk[10] > 0 && e->slot[3].norm > 1.5f,
              "it is what was fed in, brought to the loudness of Ahhh and Fresssh (%.1f dBFS on average), with its overview and its gain", 20.0 * log10(rms));
    }
    render_n(e, buf, 4096);
    CHECK(rms_of(buf, 4096) > 0.01, "and it plays");
    ts_engine_view(e, js, sizeof js);
    CHECK(strstr(js, "\"n\":\"USER 2\"") && strstr(js, "\"ld\":\"10010000\"") && json_num(js, "rc", &v) && v == 0, "the screen shows its name, and that it is no longer recording");

    /* it was kept as a file, and a fresh start loads it back into the same slot whatever else is there */
    ts_engine_t *f = ts_engine_create(44100.0f);
    ts_engine_load_dir(f, TDIR "/rec");
    CHECK(f->slot[3].loaded && !f->slot[2].loaded && f->slot[3].len == 5120 && !strcmp(f->slot[3].name, "USER 2") && fabsf(f->slot[3].data[100] - e->slot[3].data[100]) < 1e-4f,
          "it was kept as user2.wav, and loads back into User 2");
    ts_engine_destroy(f);

    /* too short: thrown away, the old one stays */
    ts_engine_rec_start(e);
    for (int i = 0; i < 128; i++) in[i] = 0.9f;
    ts_engine_feed_input(e, in, 128);
    ts_engine_rec_stop(e);
    ts_engine_service(e);
    render_n(e, buf, 128);
    CHECK(e->pend_ready == 0 && e->slot[3].len == 5120 && e->slot[3].data[100] < 0.6f, "a recording under 46 ms is thrown away and the sample already there stays");

    /* it stops by itself after four seconds, and replaces the old one, which the service call frees */
    ts_engine_rec_start(e);
    for (int b = 0; b < 1500; b++) ts_engine_feed_input(e, in, 128);
    CHECK(e->rec_state == 2 && e->rec_len == e->rec_cap && e->rec_cap == 176400, "it stops by itself after four seconds (%d frames)", e->rec_len);
    ts_engine_service(e);
    render_n(e, buf, 128);
    int parked = 0;
    for (int i = 0; i < TS_SLOTS + 2; i++) parked += e->garbage[i] != NULL;
    CHECK(e->slot[3].len == 176400 - 2646 - 4410 && parked == 1, "recording over a sample replaces it (less the first 60 ms and the last 100 ms: the pad's press and let-go); the old data waits for the service call, not freed on the audio thread (%d)", e->slot[3].len);
    ts_engine_service(e);
    parked = 0;
    for (int i = 0; i < TS_SLOTS + 2; i++) parked += e->garbage[i] != NULL;
    CHECK(parked == 0, "...which frees it");

    /* a held recording is cropped to its sound: the silence round it goes, and so do the taps of the pad at its very ends */
    {
        ts_engine_rec_start(e);
        static float blk2[128];
        int total = 0;
        for (int b = 0; b < 345; b++) {                                         /* 1 s: 0.3 s quiet, 0.4 s of voice, 0.3 s quiet, a tap at each end */
            for (int i = 0; i < 128; i++, total++) {
                double t = total / 44100.0;
                float sv = (t >= 0.3 && t < 0.7) ? 0.6f * sinf(0.07f * (float)total) : 0.0005f * sinf(0.3f * (float)total);
                if (t < 0.01 || t > 0.985) sv = 0.8f;                             /* the press and the let-go */
                blk2[i] = sv;
            }
            ts_engine_feed_input(e, blk2, 128);
        }
        ts_engine_rec_stop(e);
        ts_engine_service(e);
        render_n(e, buf, 128);
        int len = e->slot[3].len;
        CHECK(len > (int)(0.4 * 44100) && len < (int)(0.44 * 44100) && fabsf(e->slot[3].data[0]) < 0.05f && fabsf(e->slot[3].data[(int)(0.006 * 44100)]) > 0.0f,
              "it keeps the voice and a hair round it (%.3f s), not the silence or the taps", len / 44100.0);
        ts_engine_rec_start(e);                                                   /* only silence (and the taps): nothing is kept */
        for (int b = 0; b < 100; b++) { for (int i = 0; i < 128; i++) blk2[i] = (b == 0 || b == 99) ? 0.8f : 0.0f; ts_engine_feed_input(e, blk2, 128); }
        ts_engine_rec_stop(e);
        ts_engine_service(e);
        render_n(e, buf, 128);
        CHECK(e->slot[3].len == len, "a recording of nothing but the taps is thrown away; the sample already there stays");
    }

    /* clicks a little way in from either end (past what is skipped), with silence between them and the voice, are dropped; the ends fade */
    {
        ts_engine_rec_start(e);
        static float blk4[128];
        int total = 0;
        for (int b = 0; b < 414; b++) {                                         /* 1.2 s: a click at 0.15 s, the voice 0.4-0.8 s, a click at 1.0 s */
            for (int i = 0; i < 128; i++, total++) {
                double t = total / 44100.0;
                float cv = (t >= 0.4 && t < 0.8) ? 0.5f * sinf(0.07f * (float)total) : 0.0003f * sinf(0.3f * (float)total);
                if ((t >= 0.150 && t < 0.153) || (t >= 1.000 && t < 1.003)) cv = 0.7f;
                blk4[i] = cv;
            }
            ts_engine_feed_input(e, blk4, 128);
        }
        ts_engine_rec_stop(e);
        ts_engine_service(e);
        render_n(e, buf, 128);
        int len = e->slot[3].len;
        const float *d = e->slot[3].data;
        CHECK(len > (int)(0.40 * 44100) && len < (int)(0.44 * 44100), "the clicks either side of the voice are dropped (%.3f s kept)", len / 44100.0);
        CHECK(d && fabsf(d[0]) < 1e-6f && fabsf(d[len - 1]) < 1e-6f, "and the ends fade in and out, so the cut itself does not click");
    }

    /* a quiet voice with sharp peaks comes out as loud as Ahhh and Fresssh, its peaks rounded off, never past full scale */
    {
        ts_engine_rec_start(e);
        static float blk3[128];
        int total = 0;
        for (int b = 0; b < 300; b++) {
            for (int i = 0; i < 128; i++, total++) blk3[i] = 0.03f * sinf(0.09f * (float)total) + ((total % 997) < 6 ? 0.35f : 0.0f);
            ts_engine_feed_input(e, blk3, 128);
        }
        ts_engine_rec_stop(e);
        ts_engine_service(e);
        render_n(e, buf, 128);
        double sq = 0.0;
        float pk = 0.0f;
        for (int i = 0; i < e->slot[3].len; i++) { sq += (double)e->slot[3].data[i] * e->slot[3].data[i]; pk = fmaxf(pk, fabsf(e->slot[3].data[i])); }
        double rms = sqrt(sq / (e->slot[3].len > 0 ? e->slot[3].len : 1));
        CHECK(e->slot[3].len > 0 && rms > 0.13 && rms < 0.23 && pk <= 0.951f, "a quiet, peaky recording is kept as loud as Ahhh (%.1f dBFS on average, peak %.2f)", 20.0 * log10(rms), (double)pk);
    }

    /* from Ahhh or Fresssh it goes to the first empty User slot; empty or full, nothing breaks */
    p.sample = 0;
    ts_engine_set_params(e, &p);
    CHECK(ts_engine_rec_start(e) == 2 && e->p.sample == 2, "selected on Ahhh: the first empty User slot, and it is selected");
    ts_engine_rec_stop(e);
    ts_engine_service(e);
    CHECK(e->rec_state == 0 && !e->slot[2].loaded, "(stopped at once: nothing to keep)");

    /* clearing empties a User slot, and its file; never Ahhh or Fresssh */
    ts_engine_clear_slot(e, 0);
    ts_engine_clear_slot(e, 1);
    ts_engine_service(e);
    render_n(e, buf, 128);
    CHECK(e->slot[0].loaded, "Ahhh and Fresssh cannot be cleared");
    ts_engine_clear_slot(e, 3);
    ts_engine_service(e);
    render_n(e, buf, 128);
    FILE *fp = fopen(TDIR "/rec/user2.wav", "rb");
    CHECK(!e->slot[3].loaded && e->slot[3].len == 0 && e->slot[3].data == NULL && fp == NULL, "Clear empties the slot and deletes its file");
    if (fp) fclose(fp);
    ts_engine_service(e);

    /* bad calls */
    ts_engine_feed_input(NULL, in, 128);
    ts_engine_feed_input(e, NULL, 128);
    ts_engine_feed_input(e, in, 0);
    ts_engine_rec_stop(NULL);
    ts_engine_service(NULL);
    ts_engine_clear_slot(NULL, 3);
    ts_engine_clear_slot(e, 99);
    ts_engine_service(e);
    CHECK(ts_engine_rec_start(NULL) == -1, "bad calls are harmless");
    ts_engine_destroy(e);
}


/* ---- 11. rendering is safe ------------------------------------------------------------------------ */

static void test_render_safety(void)
{
    printf("render safety\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "a.wav", SMP_SINE, 0.9f, 44100);
    load_sample(e, 1, "b.wav", SMP_RAMP, 1.0f, 3000);
    load_sample(e, 2, "tiny.wav", SMP_DC, 0.5f, 100);
    static float buf[4096];
    size_t a0 = g_allocs, f0 = g_frees;
    uint32_t rs = 12345u;
    int bad = 0, oob = 0;
    for (int it = 0; it < 4000; it++) {
        ts_params_t p = e->p;
        int r = (int)(rng_next(&rs) % 100u);
        if (r < 30) p.turntable = rng_f(&rs);
        else if (r < 40) p.drag = rng_f(&rs);
        else if (r < 45) p.gain_db = rng_f(&rs) * 50.0f - 30.0f;
        else if (r < 52) p.sample = (int)(rng_next(&rs) % 8u) - 1;
        else if (r < 62) p.lock = (int)(rng_next(&rs) & 1u);
        else if (r < 70) { p.clip = (int)(rng_next(&rs) % 21u) - 1; p.sample = (int)(rng_next(&rs) % 8u) - 1; }
        else if (r < 80) { p.clip_depth = rng_f(&rs) * 4.0f - 1.0f; p.clip_offset = rng_f(&rs) * 2.0f - 1.0f; }
        else if (r < 84) p.note_gate = (int)(rng_next(&rs) & 1u);
        else if (r < 88) p.normalise = (int)(rng_next(&rs) & 1u);
        else if (r < 90) p.vel_to_vol = (int)(rng_next(&rs) & 1u);
        else if (r < 92) p.pads = (int)(rng_next(&rs) & 1u);
        else if (r < 96) ts_engine_note(e, (int)(rng_next(&rs) % 110u), (int)(rng_next(&rs) % 128u), (int)(rng_next(&rs) & 1u));
        else if (r < 98) ts_engine_sync_beat(e, (rng_next(&rs) & 1u) ? (double)(rng_next(&rs) % 1000u) * 0.37 : -1.0);
        else ts_engine_set_tempo(e, rng_f(&rs) * 400.0f - 50.0f);
        ts_engine_set_params(e, &p);
        int frames = 1 + (int)(rng_next(&rs) % 700u);
        render_n(e, buf, frames);
        for (int i = 0; i < frames; i++) bad += !isfinite(buf[i]) || fabsf(buf[i]) > 5.0f;
        oob += !(e->pos >= 0.0 && e->pos <= 1.0) || !isfinite(e->vel_out) || e->gate < 0.0f || e->gate > 1.0f;
    }
    CHECK(bad == 0 && oob == 0, "4000 random settings and notes: %d bad samples, %d bad states", bad, oob);
    CHECK(g_allocs == a0 && g_frees == f0, "rendering, setting and notes never allocate");
    ts_engine_render(e, buf, 0);
    ts_engine_render(NULL, buf, 10);
    ts_engine_render(e, NULL, 10);
    ts_engine_destroy(e);
}

/* ---- 12. the plugin ----------------------------------------------------------------------------------- */

static float t_bpm_v = 120.0f;
static float t_bpm(void) { return t_bpm_v; }
static double g_beat = -1.0;
static double t_beat(void) { return g_beat; }
static int t_clock(void) { return g_beat >= 0.0 ? TS_CLOCK_RUNNING : 1; }

static uint8_t t_mm[4096];                                  /* the host's shared memory: the audio input at offset 512 */
static int t_have_input = 1;

static const ts_plugin_api_v2_t *plugin_api(void)
{
    static ts_host_api_t host;
    memset(&host, 0, sizeof host);
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.mapped_memory = t_have_input ? t_mm : NULL;
    host.audio_in_offset = 512;
    host.get_bpm = t_bpm;
    host.get_clock_status = t_clock;
    host.get_beat_position = t_beat;
    return move_plugin_init_v2(&host);
}

static void test_plugin(void)
{
    printf("plugin\n");
    const ts_plugin_api_v2_t *api = plugin_api();
    CHECK(api && api->api_version == 2 && api->create_instance && api->render_block, "v2 API");
    void *h = api->create_instance(g_root, "{}");
    CHECK(h != NULL, "create");
    ts_inst_t *in = (ts_inst_t *)h;
    char out[512];
    CHECK(api->get_param(h, "name", out, sizeof out) > 0 && !strcmp(out, "TinyScratch"), "name");
    CHECK(in->eng->slot[0].loaded && in->eng->slot[1].loaded && !strcmp(in->eng->slot[0].name, "AHHH") && !strcmp(in->eng->slot[1].name, "FRESSSH") &&
          in->eng->slot[0].len > 30000 && in->eng->slot[0].len < 46000 && in->eng->slot[1].len > 25000 && in->eng->slot[1].len < 36000,
          "the shipped Ahhh and Fresssh (stereo files, mixed to mono) load from samples/: %d and %d frames", in->eng->slot[0].len, in->eng->slot[1].len);

    /* every parameter: label, index, value, and what get_param reports */
    int fine = 1;
    for (int i = 0; i < P_COUNT; i++) {
        const pdef_t *d = &k_params[i];
        if (d->kind == K_STRING || d->kind == K_FILEPATH) continue;              /* the view, and the file picker: tested on their own */
        if (is_transient(i)) continue;                                           /* the Pads screen's, and one-shots: tested on their own */
        if (d->kind == K_CANVAS) { fine &= api->get_param(h, d->key, out, sizeof out) == 0; continue; }
        if (d->kind == K_ENUM)
            for (int o = 0; o < d->nopts; o++) {
                api->set_param(h, d->key, d->opts[o]);
                int ok1 = api->get_param(h, d->key, out, sizeof out) > 0 && atoi(out) == o;
                char idx[8];
                snprintf(idx, sizeof idx, "%d", o);
                api->set_param(h, d->key, idx);
                int ok2 = api->get_param(h, d->key, out, sizeof out) > 0 && atoi(out) == o;
                if (!ok1 || !ok2) fprintf(stderr, "  (%s option %d: label %d, index %d)\n", d->key, o, ok1, ok2);
                fine &= ok1 && ok2;
            }
        else {
            char v[32];
            snprintf(v, sizeof v, "%g", (double)d->hi);
            api->set_param(h, d->key, v);
            fine &= api->get_param(h, d->key, out, sizeof out) > 0 && fabs(atof(out) - (double)d->hi) < 1e-3;
            snprintf(v, sizeof v, "%g", (double)d->lo);
            api->set_param(h, d->key, v);
            int ok = api->get_param(h, d->key, out, sizeof out) > 0 && fabs(atof(out) - (double)d->lo) < 1e-3;
            if (!ok) fprintf(stderr, "  (%s low: %s)\n", d->key, out);
            fine &= ok;
        }
    }
    CHECK(fine, "every parameter takes its labels, its indices and its limits, and reports its index");
    api->set_param(h, "drag", "5");
    CHECK(api->get_param(h, "drag", out, sizeof out) > 0 && atof(out) == 1.0, "values are clamped");
    api->set_param(h, "drag", "banana");
    api->set_param(h, "nonsense", "1");
    api->set_param(h, NULL, "1");
    api->set_param(h, "drag", NULL);
    api->set_param(NULL, "drag", "1");
    CHECK(api->get_param(h, "drag", out, sizeof out) > 0 && atof(out) == 1.0 && api->get_param(h, "nonsense", out, sizeof out) == -1 &&
          api->get_param(h, "drag", out, 2) == -1 && api->get_param(h, "drag", NULL, 10) == -1 && api->get_param(NULL, "drag", out, 10) == -1,
          "garbage in changes nothing; bad reads fail cleanly");

    api->destroy_instance(h);
    h = api->create_instance(g_root, "{}");
    in = (ts_inst_t *)h;

    /* MIDI (plain notes: the pad grid has its own tests) */
    api->set_param(h, "pads", "Off");
    api->set_param(h, "note_gate", "On");
    api->set_param(h, "turntable", "0.3");
    int16_t pcm[256];
    for (int i = 0; i < 40; i++) api->render_block(h, pcm, 128);
    int peak = 0;
    for (int i = 0; i < 256; i++) peak = peak > abs(pcm[i]) ? peak : abs(pcm[i]);
    CHECK(peak == 0, "no note, no sound");
    uint8_t on[3] = { 0x90, 60, 100 }, off[3] = { 0x80, 60, 0 }, off0[3] = { 0x90, 60, 0 }, low[3] = { 0x90, 3, 100 }, cc[3] = { 0xB0, 123, 0 };
    api->on_midi(h, on, 3, 0);
    for (int i = 0; i < 10; i++) api->render_block(h, pcm, 128);
    api->set_param(h, "turntable", "0.33");
    for (int i = 0; i < 4; i++) api->render_block(h, pcm, 128);
    peak = 0;
    int same = 1;
    for (int i = 0; i < 128; i++) { peak = peak > abs(pcm[2 * i]) ? peak : abs(pcm[2 * i]); same &= pcm[2 * i] == pcm[2 * i + 1]; }
    CHECK(in->eng->notes_held == 1 && peak > 50 && same, "a note on C3 cuts the sound in (peak %d), the same in both channels", peak);
    api->on_midi(h, off, 3, 0);
    CHECK(in->eng->notes_held == 0, "note off");
    api->on_midi(h, on, 3, 0);
    api->on_midi(h, off0, 3, 0);
    CHECK(in->eng->notes_held == 0, "a note on with velocity 0 is a note off");
    api->on_midi(h, on, 3, 0);
    api->on_midi(h, cc, 3, 0);
    CHECK(in->eng->notes_held == 0, "all notes off");
    api->on_midi(h, low, 3, 0);
    CHECK(in->eng->notes_held == 0, "a low note does nothing");
    uint8_t aft[3] = { 0xD0, 100, 0 }, bend[3] = { 0xE0, 0, 64 }, shorty[2] = { 0x90, 60 };
    api->on_midi(h, aft, 3, 0);
    api->on_midi(h, bend, 3, 0);
    api->on_midi(h, shorty, 2, 0);
    api->on_midi(h, NULL, 3, 0);
    api->on_midi(NULL, on, 3, 0);
    CHECK(in->eng->notes_held == 0, "other messages are ignored");

    /* the pad grid reaches the parameters too */
    api->set_param(h, "pads", "On");
    uint8_t pad_sample[3] = { 0x90, 36 + 8 + 1, 100 }, pad_auto[3] = { 0x90, 68 + 16 + 7, 100 }, pad_clip[3] = { 0x90, 68 + 24 + 6, 100 }, pad_gen[3] = { 0x90, 68 + 16 + 6, 100 };
    api->on_midi(h, pad_sample, 3, 0);
    api->get_param(h, "sample", out, sizeof out);
    CHECK(atoi(out) == 1, "a sample pad picks the sample");
    api->on_midi(h, pad_clip, 3, 0);
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == 7, "a clip pad picks the clip");
    api->on_midi(h, pad_auto, 3, 0);
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == TS_CLIP_AUTO && in->p.clip == TS_CLIP_AUTO, "the Auto pad plays the clips in turn");
    api->on_midi(h, pad_gen, 3, 0);
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == TS_CLIP_TRY && in->eng->try_valid && in->p.clip == TS_CLIP_TRY, "the Generate pad makes a clip to try, and plays it");
    api->set_param(h, "clip", "Off");
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == 0, "Clip off");
    api->set_param(h, "clip", "Clip 4");
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == 4, "a clip by name");

    /* the Pads screen: raw pads. The notes are ignored while it is there; the held cut pads come from it */
    api->set_param(h, "clip", "Off");
    uint8_t hi_note[3] = { 0x90, 100, 100 };                                  /* above the pads: a cut, unless they are being read raw */
    api->set_param(h, "pad_raw", "1");
    api->on_midi(h, hi_note, 3, 0);
    CHECK(in->eng->pad_raw > 0 && in->eng->notes_held == 0, "the plugin: while the Pads screen is there the notes are ignored");
    api->set_param(h, "pad_cuts", "5");
    CHECK(in->eng->pad_held == 2 && in->eng->pad_cuts == 5, "...and pad_cuts is the cut pads held");
    api->get_param(h, "pad_cuts", out, sizeof out);
    CHECK(atoi(out) == 0, "(it is not a setting: it reads back as 0)");
    api->set_param(h, "pad_cuts", "-3");
    api->set_param(h, "pad_cuts", "1e9");
    api->set_param(h, "pad_cuts", "x");
    CHECK(in->eng->pad_held == 0, "garbage is no cut");
    {
        char st2[4096];
        api->get_param(h, "state", st2, sizeof st2);
        CHECK(!strstr(st2, "pad_raw") && !strstr(st2, "pad_cuts"), "neither is saved with the project");
    }
    api->set_param(h, "pad_raw", "0");
    api->on_midi(h, hi_note, 3, 0);
    CHECK(in->eng->notes_held == 1, "the Pads screen gone: the notes count again");
    api->on_midi(h, cc, 3, 0);

    /* the host's tempo and transport reach the engine */
    api->set_param(h, "lock", "On");
    g_beat = 12.0;
    t_bpm_v = 90.0f;
    for (int i = 0; i < 4; i++) { api->render_block(h, pcm, 128); g_beat += 128.0 * 90.0 / (60.0 * 44100.0); }
    CHECK(in->eng->transport == 1 && fabsf(in->eng->bpm - 90.0f) < 1e-3f && fabs(in->eng->clock - g_beat) < 0.01, "tempo and beat position follow the host: the Move's own tempo");
    g_beat = -1.0;
    api->render_block(h, pcm, 128);
    CHECK(in->eng->transport == 0, "a stopped transport is noticed");
    t_bpm_v = 120.0f;

    /* big and odd blocks */
    static int16_t big[2 * 1000];
    api->render_block(h, big, 1000);
    api->render_block(h, big, 1);
    api->render_block(h, big, 0);
    api->render_block(h, NULL, 128);

    /* the manifest's JSON from the same table */
    static char cp[8192], ui[4096];
    int ncp = api->get_param(h, "chain_params", cp, sizeof cp), nui = api->get_param(h, "ui_hierarchy", ui, sizeof ui);
    CHECK(ncp > 500 && nui > 200 && balanced(cp) && balanced(ui), "chain_params %d bytes, ui_hierarchy %d bytes", ncp, nui);
    CHECK(api->get_param(h, "chain_params", cp, 100) == -1 && api->get_param(h, "ui_hierarchy", ui, 100) == -1, "a buffer too small is refused, not truncated");
    char vw[2048];
    CHECK(api->get_param(h, "view", vw, sizeof vw) > 0 && balanced(vw) && strstr(vw, "\"sn\":\"AHHH|FRESSSH|"), "the screen's view is served");
    api->destroy_instance(h);
}

/* ---- 13. saved state ------------------------------------------------------------------------------------ */

static void test_state(void)
{
    printf("saved state\n");
    const ts_plugin_api_v2_t *api = plugin_api();
    void *h1 = api->create_instance(g_root, "{}");
    const char *kv[][2] = {
        { "sample", "Fresssh" }, { "turntable", "0.35" }, { "drag", "0.8" }, { "gain", "-4.5" }, { "note_gate", "Off" }, { "vel_to_vol", "On" },
        { "normalise", "Off" }, { "lock", "Off" }, { "cut_from", "48" }, { "pads", "Off" }, { "clip_depth", "1.4" }, { "clip_offset", "-0.2" },
        { "gen_style", "Crab" }, { "gen_bars", "4 bars" },
    };
    for (size_t k = 0; k < sizeof kv / sizeof kv[0]; k++) api->set_param(h1, kv[k][0], kv[k][1]);
    static char st[4096], st2[4096];
    int n = api->get_param(h1, "state", st, sizeof st);
    CHECK(n > 100 && n < 900 && st[0] == '{' && st[n - 1] == '}' && balanced(st), "state is a compact object (%d bytes)", n);
    CHECK(!strstr(st, "\"view\"") && !strstr(st, "scratcher") && !strstr(st, "lfo") && !strstr(st, "preset") && !strstr(st, "\"pre\""), "buttons and pictures are not state, and there is nothing of an LFO");
    CHECK(api->get_param(h1, "state", st2, 40) == -1, "a buffer that is too small is refused");
    api->destroy_instance(h1);

    void *h2 = api->create_instance(g_root, "{}");
    ts_inst_t *b = (ts_inst_t *)h2;
    CHECK(b->p.drag == 0.5f && b->p.sample == 0 && b->p.lock == 1 && b->p.pads == 1, "a new instance starts on the defaults");
    api->set_param(h2, "state", st);
    const ts_params_t *q = &b->p;
    CHECK(q->sample == 1 && fabsf(q->turntable - 0.35f) < 1e-3f && fabsf(q->drag - 0.8f) < 1e-3f && fabsf(q->gain_db + 4.5f) < 1e-3f && q->note_gate == 0 &&
          q->vel_to_vol == 1 && q->normalise == 0 && q->lock == 0 && q->cut_from == 48 && q->pads == 0 &&
          fabsf(q->clip_depth - 1.4f) < 1e-3f && fabsf(q->clip_offset + 0.2f) < 1e-3f && q->gen_style == 9 && q->gen_bars == 2, "every dial comes back");
    CHECK(memcmp(&b->eng->p, &b->p, sizeof b->p) == 0, "...in the engine too");
    CHECK(fabs(b->eng->man - 0.35) < 1e-6, "the playhead is where the knob was");
    int n2 = api->get_param(h2, "state", st2, sizeof st2);
    CHECK(n2 == n && strcmp(st, st2) == 0, "saving what was restored gives the same state");

    /* hostile and broken states change nothing */
    ts_params_t keep = b->p;
    const char *junk[] = { "", "{}", "{", "null", "[1,2]", "{\"v\":\"2\",\"drag\":\"0.1\"}", "{\"v\":\"1\",\"drag\":", "\"v\":\"1\",\"drag\":\"0.1\"" };
    for (size_t k = 0; k < sizeof junk / sizeof junk[0]; k++) api->set_param(h2, "state", junk[k]);
    CHECK(memcmp(&keep, &b->p, sizeof keep) == 0, "garbage state is ignored");
    static char big[20000];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    memcpy(big, "{\"v\":\"1\",\"drag\":\"0.1\",\"x\":\"", 28);
    api->set_param(h2, "state", big);
    CHECK(memcmp(&keep, &b->p, sizeof keep) == 0, "an oversized state is refused");
    api->set_param(h2, "state", "{\"v\":\"1\",\"drag\":\"nan\",\"gain\":\"9999\",\"clip_depth\":\"77\",\"clip_offset\":\"-9\",\"sample\":\"-3\",\"gen_style\":\"99\"}");
    CHECK(b->p.gain_db == 12.0f && b->p.clip_depth == 2.0f && b->p.clip_offset == -0.5f && b->p.gen_style == 11 && b->p.sample == 0 && isfinite(b->p.drag) && b->p.drag >= 0.0f && b->p.drag <= 1.0f,
          "out-of-range values are clamped (gain %.1f depth %.1f style %d)", (double)b->p.gain_db, (double)b->p.clip_depth, b->p.gen_style);
    api->set_param(h2, "state", NULL);
    api->set_param(NULL, "state", "{}");
    int16_t pcm[256];
    for (int i = 0; i < 40; i++) api->render_block(h2, pcm, 128);
    api->destroy_instance(h2);
}

/* ---- 14. the host looks while the audio runs --------------------------------------------------------------- */

static void *poller(void *arg)
{
    ts_inst_t *in = (ts_inst_t *)arg;
    static char buf[4096];
    int bad = 0;
    for (int i = 0; i < 400; i++) {
        int n = ts_engine_view(in->eng, buf, sizeof buf);
        bad += n <= 0 || !balanced(buf);
        build_state(in, buf, sizeof buf);
        bad += !balanced(buf);
    }
    return (void *)(intptr_t)bad;
}

static void test_concurrent(void)
{
    printf("polling while rendering\n");
    const ts_plugin_api_v2_t *api = plugin_api();
    void *h = api->create_instance(g_root, "{}");
    ts_inst_t *in = (ts_inst_t *)h;
    pthread_t th;
    pthread_create(&th, NULL, poller, in);
    int16_t pcm[256];
    uint8_t on[3] = { 0x90, 60, 100 }, off[3] = { 0x80, 60, 0 };
    for (int i = 0; i < 3000; i++) {
        if (i % 50 == 0) api->on_midi(h, on, 3, 0);
        if (i % 50 == 25) api->on_midi(h, off, 3, 0);
        if (i % 200 == 0) api->set_param(h, "clip", (i / 200) & 1 ? "Auto" : "Clip 3");
        api->render_block(h, pcm, 128);
    }
    void *r = NULL;
    pthread_join(th, &r);
    CHECK((int)(intptr_t)r == 0, "every view and state read was whole while notes and parameters changed");
    api->destroy_instance(h);
}
static void *rec_thread(void *arg)
{
    ts_engine_t *e = (ts_engine_t *)arg;
    for (int i = 0; i < 300; i++) {
        ts_engine_rec_start(e);
        for (volatile int k = 0; k < 3000; k++) { }
        ts_engine_rec_stop(e);
        ts_engine_service(e);
        if (i % 5 == 0) ts_engine_clear_slot(e, 2 + (i / 5) % 4);
        ts_engine_service(e);
    }
    return NULL;
}

static void test_record_threads(void)
{
    printf("recording while rendering\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.5f, 44100);
    ts_params_t p;
    ts_params_default(&p);
    p.pads = 0; p.drag = 0.0f; p.note_gate = 0; p.clip = 0;
    ts_engine_set_params(e, &p);
    pthread_t th;
    pthread_create(&th, NULL, rec_thread, e);
    float in[128], out[128];
    int bad = 0;
    for (int b = 0; b < 6000; b++) {
        for (int i = 0; i < 128; i++) in[i] = 0.3f * sinf(0.07f * (float)(b * 128 + i));
        ts_engine_feed_input(e, in, 128);
        ts_engine_render(e, out, 128);
        for (int i = 0; i < 128; i++) bad += !isfinite(out[i]) || fabsf(out[i]) > 4.0f;
    }
    pthread_join(th, NULL);
    ts_engine_service(e);
    ts_engine_render(e, out, 128);
    ts_engine_service(e);
    int sane = 1;
    for (int i = 0; i < TS_SLOTS; i++) sane &= e->slot[i].loaded ? (e->slot[i].data != NULL && e->slot[i].len > 0) : (e->slot[i].data == NULL);
    CHECK(bad == 0 && sane, "recording and clearing from one thread while the audio thread renders: every output finite, every slot whole (%d bad)", bad);
    ts_engine_destroy(e);
}

static void test_plugin_record(void)
{
    printf("plugin recording\n");
    char path[600];
    snprintf(path, sizeof path, "%s/samples/user2.wav", g_root);
    remove(path);
    const ts_plugin_api_v2_t *api = plugin_api();
    void *h = api->create_instance(g_root, "{}");
    ts_inst_t *in = (ts_inst_t *)h;
    int16_t *src = (int16_t *)(t_mm + 512);
    for (int i = 0; i < 128; i++) { src[2 * i] = 1000; src[2 * i + 1] = 3000; }
    char out[8192];
    api->set_param(h, "sample", "3");
    api->set_param(h, "record", "Record");
    api->get_param(h, "record", out, sizeof out);
    CHECK(atoi(out) == 1 && in->eng->rec_state == 1, "Record starts recording");
    int16_t pcm[256];
    for (int i = 0; i < 20; i++) api->render_block(h, pcm, 128);
    CHECK(in->eng->rec_len == 2560 && fabsf(in->eng->rec_buf[0] - 2000.0f / 32768.0f) < 1e-6f, "the Move's audio input is fed in a block at a time, mixed to mono (%d frames)", in->eng->rec_len);
    api->get_param(h, "state", out, sizeof out);
    CHECK(!strstr(out, "\"record\"") && !strstr(out, "\"clear\""), "Record and Clear are not saved with the project");
    api->set_param(h, "record", "Idle");
    api->get_param(h, "record", out, sizeof out);
    CHECK(atoi(out) == 0, "turned off, it stops");
    api->render_block(h, pcm, 128);
    CHECK(in->eng->slot[3].loaded && in->eng->slot[3].len == 2560, "the recording is User 2's sample");
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL, "...kept in the module's samples folder as user2.wav");
    if (fp) fclose(fp);
    api->set_param(h, "clear", "Clear");
    api->render_block(h, pcm, 128);
    fp = fopen(path, "rb");
    CHECK(!in->eng->slot[3].loaded && fp == NULL, "Clear empties it and deletes the file");
    if (fp) fclose(fp);

    /* a host that gives no audio input: it records nothing, and nothing breaks */
    t_have_input = 0;
    const ts_plugin_api_v2_t *api2 = plugin_api();
    void *h2 = api2->create_instance(g_root, "{}");
    ts_inst_t *in2 = (ts_inst_t *)h2;
    api2->set_param(h2, "sample", "4");
    api2->set_param(h2, "record", "Record");
    for (int i = 0; i < 20; i++) api2->render_block(h2, pcm, 128);
    CHECK(in2->eng->rec_len == 0, "no audio input from the host: nothing is recorded");
    api2->set_param(h2, "record", "Idle");
    api2->destroy_instance(h2);
    t_have_input = 1;
    api->destroy_instance(h);
    remove(path);
}
static void test_recipes_library(void)
{
    printf("the library of kept clips\n");
    const char *path = TDIR "/rec/recipes.txt";
    remove(path);
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, path);
    ts_params_t p;
    ts_params_default(&p);
    p.gen_style = GS_CHIRP; p.gen_bars = 0;
    ts_engine_set_params(e, &p);
    ts_engine_generate(e);
    ts_engine_keep(e, 0);
    p.gen_style = GS_ANY; p.gen_bars = 2;
    ts_engine_set_params(e, &p);
    ts_engine_generate(e);
    ts_engine_clip_pad(e, 12);
    CHECK(e->recipes_dirty, "keeping marks the library as changed");
    FILE *f = fopen(path, "rb");
    CHECK(f == NULL, "(nothing is written on the audio or MIDI thread)");
    if (f) fclose(f);
    ts_engine_service(e);
    f = fopen(path, "rb");
    char text[256] = { 0 };
    if (f) { size_t n = fread(text, 1, sizeof text - 1, f); text[n] = 0; fclose(f); }
    int sa = -1, a = 0, b = 0, c = 0, sa2 = -1, a2 = 0, b2 = 0, c2 = 0;
    unsigned long s1 = 0, s2 = 0;
    int fw1 = -1, fw2 = -1, bp1 = -1, bp2 = -1, fn1 = -1, fn2 = -1, ec1 = -1, ec2 = -1;
    int got = sscanf(text, "%d %d %d %d %lu %d %d %d %d\n%d %d %d %d %lu %d %d %d %d", &sa, &a, &b, &c, &s1, &fw1, &bp1, &fn1, &ec1, &sa2, &a2, &b2, &c2, &s2, &fw2, &bp2, &fn2, &ec2);
    CHECK(f && got == 18 && sa == 0 && a == 9 && b == GS_CHIRP && c == 1 && sa2 == 0 && a2 == 12 && c2 == 4 && fw1 == 0 && fw2 == 0 && bp1 == 120 && bp2 == 120 && fn1 == 10 && fn2 == 10 && ec1 == 10 && ec2 == 10 && !e->recipes_dirty,
          "the service call writes it: one line per clip (sample, pad, style, bars, seed, forwards, tempo, finish, echo) (%s)", text);
    static ts_clip_t keep9, keep12;
    memcpy(&keep9, PADCLIP(e, 9), sizeof keep9);
    memcpy(&keep12, PADCLIP(e, 12), sizeof keep12);
    ts_engine_destroy(e);

    /* the next instance, on any track, starts with them, the same clips */
    e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, path);
    CHECK(ts_engine_load_recipes(e) == 2 && e->nclips == TS_BUILTIN_CLIPS + 2 && PADCLIP(e, 9) && PADCLIP(e, 12) && PADKEPT(e, 9) && PADKEPT(e, 12),
          "a new instance loads the kept clips on the same pads");
    CHECK(memcmp(PADCLIP(e, 9)->pos, keep9.pos, (size_t)keep9.n * 2) == 0 && memcmp(PADCLIP(e, 9)->gate, keep9.gate, (size_t)keep9.n) == 0 &&
          memcmp(PADCLIP(e, 12)->pos, keep12.pos, (size_t)keep12.n * 2) == 0 && memcmp(PADCLIP(e, 12)->gate, keep12.gate, (size_t)keep12.n) == 0, "...exactly the clips that were kept");
    CHECK(!e->recipes_dirty, "(loading does not rewrite the file)");
    ts_engine_destroy(e);

    /* a kept clip's own data is saved next to the recipes (kept.bin), and wins over making it again: a clip you chose stays as it is,
     * whatever the generator does later. Proved by changing the saved data: the loaded clip has the change. */
    {
        const char *kpath = TDIR "/rec/kept.bin";
        FILE *kf = fopen(kpath, "rb");
        static uint8_t kb[1 << 16];
        size_t kn = kf ? fread(kb, 1, sizeof kb, kf) : 0;
        if (kf) fclose(kf);
        CHECK(kn > 7 && !memcmp(kb, "TSKP", 4) && kb[4] == 1 && (kb[5] | (kb[6] << 8)) == 2, "the clips' data is written beside the recipes (%zu bytes, 2 clips)", kn);
        size_t g0 = 7 + 12 + (size_t)(kb[7 + 10] | (kb[7 + 11] << 8)) * 2;      /* the first clip's first gate byte */
        uint8_t was = kb[g0];
        kb[g0] = was ? 0 : 1;
        kb[7 + 12] ^= 0x55;                                                       /* and its first position */
        kf = fopen(kpath, "wb");
        fwrite(kb, 1, kn, kf);
        fclose(kf);
        e = ts_engine_create(44100.0f);
        ts_engine_set_recipes_path(e, path);
        CHECK(ts_engine_load_recipes(e) == 2 && PADCLIP(e, 9) && PADCLIP(e, 9)->gate[0] == (was ? 0 : 1) && PADCLIP(e, 9)->pos[0] != keep9.pos[0] &&
              memcmp(PADCLIP(e, 12)->pos, keep12.pos, (size_t)keep12.n * 2) == 0 && !e->recipes_dirty,
              "a new instance takes the clips from their saved data, not from the generator");
        ts_engine_destroy(e);
        kb[g0] = was; kb[7 + 12] ^= 0x55;                                         /* (as it was) */
        kf = fopen(kpath, "wb");
        fwrite(kb, 1, kn, kf);
        fclose(kf);
        remove(kpath);                                                            /* no data: made again from the recipes, and the data is written next time */
        e = ts_engine_create(44100.0f);
        ts_engine_set_recipes_path(e, path);
        CHECK(ts_engine_load_recipes(e) == 2 && memcmp(PADCLIP(e, 9)->pos, keep9.pos, (size_t)keep9.n * 2) == 0 && e->recipes_dirty,
              "without the data the recipes make them again, and the data is due to be written");
        ts_engine_service(e);
        kf = fopen(kpath, "rb");
        CHECK(kf != NULL, "...and it is");
        if (kf) fclose(kf);
        kf = fopen(kpath, "wb");                                                   /* a broken file is ignored, not trusted */
        fputs("TSKP", kf); fputc(1, kf); fputc(9, kf); fputc(0, kf); fputs("rubbish", kf);
        fclose(kf);
        ts_engine_destroy(e);
        e = ts_engine_create(44100.0f);
        ts_engine_set_recipes_path(e, path);
        CHECK(ts_engine_load_recipes(e) == 2 && memcmp(PADCLIP(e, 9)->pos, keep9.pos, (size_t)keep9.n * 2) == 0, "a broken data file is ignored: the recipes make the clips");
    }

    /* deleting rewrites it */
    ts_engine_clear_clip(e, 9);
    ts_engine_service(e);
    ts_engine_destroy(e);
    e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, path);
    CHECK(ts_engine_load_recipes(e) == 1 && PADCLIP(e, 9) == NULL && PADCLIP(e, 12), "a deleted clip is gone from the library");
    ts_engine_destroy(e);

    /* a kept clip takes its pad back from a built-in one (which come and go with clips.bin); bad lines are skipped */
    FILE *w = fopen(path, "wb");
    fputs("3 1 2 777\nnot a line\n9 99 2 5\n10 1 3 5\n12 6 4 42\n", w);
    fclose(w);
    e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, path);
    int n = ts_engine_load_recipes(e);
    CHECK(n == 2 && PADCLIP(e, 3) && PADKEPT(e, 3) && PADCLIP(e, 12) && PADKEPT(e, 12) && PADCLIP(e, 4) && !PADKEPT(e, 4) && PADCLIP(e, 9) == NULL,
          "an older library (no sample column) loads onto the first sample; a kept clip takes its pad (3) from the built-in one there; a bad style, a bad length and junk are skipped (%d loaded)", n);
    ts_engine_destroy(e);
    e = ts_engine_create(44100.0f);
    ts_engine_set_recipes_path(e, TDIR "/rec/no_such_file.txt");
    CHECK(ts_engine_load_recipes(e) == 0 && ts_engine_load_recipes(NULL) == 0 && e->nclips == TS_BUILTIN_CLIPS, "no library: nothing, no trouble");
    ts_engine_set_recipes_path(e, "");
    ts_engine_generate(e);
    ts_engine_keep(e, 0);
    ts_engine_service(e);
    CHECK(e->clip_data[8].n > 0, "with nowhere to keep a library, clips still work for the session");
    ts_engine_destroy(e);
    remove(path);
}

/* the library every new install starts with (clips/recipes.txt and clips/kept.bin): three clips each on Ahhh and Fresssh, as kept, deletable */
static void test_shipped_clips(void)
{
    printf("shipped clips\n");
    ts_engine_t *e = ts_engine_create(44100.0f);
    ts_engine_load_dir(e, "samples");
    ts_engine_set_recipes_path(e, "clips/recipes.txt");
    int n = ts_engine_load_recipes(e);
    int all = 1, data = 1;
    for (int sm = 0; sm < 2; sm++)
        for (int pad = 1; pad <= 3; pad++) {
            const ts_clip_t *c = bank_clip(e, sm, pad);
            all &= c != NULL && e->kept[e->bank[sm][pad - 1] - 1].used;
            data &= c != NULL && c->n > 0;
        }
    CHECK(n == 6 && all && data, "a new install has three kept clips on pads 1-3 of Ahhh and of Fresssh (%d)", n);
    CHECK(!e->recipes_dirty, "...all from their saved data (kept.bin), none made again");
    ts_engine_set_recipes_path(e, TDIR "/rec/shipped_check.txt");               /* (so that deleting writes nowhere near the shipped files) */
    ts_params_t p = e->p;
    p.sample = 1;
    ts_engine_set_params(e, &p);
    CHECK(ts_engine_clear_clip(e, 2) == 0 && !bank_clip(e, 1, 2) && bank_clip(e, 1, 1) && bank_clip(e, 1, 3) && bank_clip(e, 0, 2), "any of them can be deleted, from its own pad only");
    ts_engine_destroy(e);
}

static void test_plugin_generate(void)
{
    printf("plugin generate\n");
    char path[600];
    snprintf(path, sizeof path, "%s/recipes.txt", g_root);
    remove(path);
    const ts_plugin_api_v2_t *api = plugin_api();
    void *h = api->create_instance(g_root, "{}");
    ts_inst_t *in = (ts_inst_t *)h;
    char out[8192];
    api->set_param(h, "gen_style", "Stab");
    api->set_param(h, "gen_bars", "4 bars");
    api->get_param(h, "gen_style", out, sizeof out);
    CHECK(atoi(out) == GS_STAB && in->p.gen_style == GS_STAB && in->p.gen_bars == 2, "Style and Length are settings (read back as their index)");
    api->set_param(h, "generate", "Generate");
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == TS_CLIP_TRY && in->eng->try_valid && in->eng->try_recipe.style == GS_STAB && in->eng->try_recipe.bars == 4, "Generate makes a clip in that style and length, and plays it");
    api->get_param(h, "generate", out, sizeof out);
    CHECK(atoi(out) == 0, "(a one-shot: it reads back idle)");
    api->set_param(h, "clip_pad", "9");
    api->get_param(h, "clip", out, sizeof out);
    CHECK(atoi(out) == 9 && PADKEPT(in->eng, 9), "tapping the first empty pad keeps it there");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "...and the library is written in the module's folder (recipes.txt)");
    if (f) fclose(f);
    api->set_param(h, "generate", "Generate");
    api->set_param(h, "clip_pad", "14");
    CHECK(PADCLIP(in->eng, 14) && in->p.clip == 14, "clip_pad on an empty pad keeps the clip just generated there");
    api->set_param(h, "clip_pad", "2");
    CHECK(in->p.clip == 2, "...and on a pad with a clip plays it");
    api->get_param(h, "state", out, sizeof out);
    CHECK(!strstr(out, "\"generate\"") && !strstr(out, "\"keep\"") && !strstr(out, "clip_pad") && !strstr(out, "clip_delete") && !strstr(out, "clear_clip") && strstr(out, "\"gen_style\":\"5\"") && strstr(out, "\"gen_bars\":\"2\""),
          "the one-shots are not saved with the project; Style and Length are");

    /* a second instance, as on another track: it starts with the kept clips */
    void *h2 = api->create_instance(g_root, "{}");
    ts_inst_t *in2 = (ts_inst_t *)h2;
    CHECK(PADCLIP(in2->eng, 9) && PADCLIP(in2->eng, 14) && in2->eng->nclips == in->eng->nclips &&
          memcmp(PADCLIP(in2->eng, 9)->pos, PADCLIP(in->eng, 9)->pos, (size_t)PADCLIP(in->eng, 9)->n * 2) == 0, "another instance (another track) starts with the same kept clips");
    api->destroy_instance(h2);

    api->set_param(h, "clip_delete", "14");
    CHECK(PADCLIP(in->eng, 14) == NULL && PADCLIP(in->eng, 9), "clip_delete removes a clip you made");
    api->set_param(h, "clip_delete", "15");
    CHECK(PADCLIP(in->eng, 15) == NULL && PADCLIP(in->eng, 2), "...and an empty pad has nothing to delete");
    api->set_param(h, "clip", "Clip 9");
    api->set_param(h, "clip_delete", "9");
    api->get_param(h, "clip", out, sizeof out);
    CHECK(PADCLIP(in->eng, 9) == NULL && atoi(out) == TS_CLIP_AUTO, "deleting the selected clip you made goes back to Auto");
    static char hier[8192];
    CHECK(ts_plugin_json_ui_hierarchy(hier, sizeof hier) > 0 && !strstr(hier, "\"keep\"") && !strstr(hier, "\"clear_clip\"") && strstr(hier, "\"gen_style\"") && strstr(hier, "\"gen_bars\""),
          "no Keep or Clear Clip on the dial pages (that is all on the pads); Style and Length are settings there");
    api->destroy_instance(h);
    remove(path);
}
static void test_plugin_pickers(void)
{
    printf("plugin: Load Sample, Zoom, the Pads Link page\n");
    const ts_plugin_api_v2_t *api = plugin_api();
    char path[600];
    snprintf(path, sizeof path, "%s/samples/user2.wav", g_root);
    remove(path);
    void *h = api->create_instance(g_root, "{}");
    ts_inst_t *in = (ts_inst_t *)h;
    char out[8192];
    static char cp[16384];
    CHECK(ts_plugin_json_chain_params(cp, sizeof cp) > 0 && strstr(cp, "\"key\":\"sample_file\",\"name\":\"Load Sample\",\"type\":\"filepath\"") &&
          strstr(cp, "\"filter\":\".wav\"") && strstr(cp, "\"root\":\"/data/UserData/UserLibrary/Samples\"") && strstr(cp, "\"start_path\":\"/data/UserData/UserLibrary/Samples\""),
          "Load Sample is a file-browser parameter over .wav files, starting in the Move's own sample library");
    static char hier[8192];
    ts_plugin_json_ui_hierarchy(hier, sizeof hier);
    CHECK(strstr(hier, "\"sample_file\"") != NULL, "...and it is in the first menu");
    static int16_t pcm[6000];
    for (int i = 0; i < 6000; i++) pcm[i] = (int16_t)(9000.0f * sinf(0.05f * (float)i));
    size_t sz = make_wav(g_wav, pcm, 6000, 44100, 1, 16, 1);
    char file[600];
    snprintf(file, sizeof file, "%s/picked_test.wav", TDIR);
    FILE *f = fopen(file, "wb");
    fwrite(g_wav, 1, sz, f);
    fclose(f);
    api->set_param(h, "sample", "4");
    api->set_param(h, "sample_file", file);
    int16_t o[256];
    api->render_block(h, o, 128);
    CHECK(in->eng->slot[4].loaded && in->eng->slot[4].len == 6000 && !strcmp(in->eng->slot[4].name, "PICKED_TEST") && in->p.sample == 4,
          "choosing a file loads it into the selected blank User slot (%d frames, %s)", in->eng->slot[4].len, in->eng->slot[4].name);
    api->get_param(h, "sample_file", out, sizeof out);
    CHECK(out[0] == 0, "(it reads back empty: it is a choice, not a setting)");
    api->get_param(h, "state", out, sizeof out);
    CHECK(!strstr(out, "sample_file"), "...and is not saved with the project");
    api->set_param(h, "sample_file", "/no/such/file.wav");
    api->set_param(h, "sample_file", "");
    CHECK(in->eng->slot[4].loaded, "a bad path changes nothing");
    snprintf(path, sizeof path, "%s/samples/user3.wav", g_root);
    remove(path);
    remove(file);

    api->set_param(h, "clip_depth", "1.5");
    api->set_param(h, "clip_offset", "-0.25");
    api->render_block(h, o, 128);
    CHECK(fabsf(in->eng->p.clip_depth - 1.5f) < 1e-4f && fabsf(in->eng->p.clip_offset + 0.25f) < 1e-4f, "Clip Depth and Clip Offset reach the engine");
    api->set_param(h, "clip_depth", "9");
    api->set_param(h, "clip_offset", "-9");
    api->render_block(h, o, 128);
    CHECK(in->eng->p.clip_depth <= 2.0f && in->eng->p.clip_offset >= -0.5f, "...clamped");
    api->get_param(h, "state", out, sizeof out);
    CHECK(strstr(out, "clip_depth") && strstr(out, "clip_offset"), "...and saved with the project");
    api->set_param(h, "clip_depth", "1");
    api->set_param(h, "clip_offset", "0");

    api->set_param(h, "zoom", "2");
    CHECK(in->eng->zoom == 2, "Zoom sets the zoomed view");
    api->set_param(h, "zoom", "9");
    CHECK(in->eng->zoom == 3, "...clamped");
    api->set_param(h, "zoom", "0");
    CHECK(in->eng->zoom == 0, "...and off");
    api->get_param(h, "state", out, sizeof out);
    CHECK(!strstr(out, "\"zoom\""), "it is not saved with the project");
    /* the first menu (Main) is the generator: the Pads screen (the first dial), Load Sample (the second) and what Generate makes. The sample,
     * the clip and the rest of what Generate does are on the pads, so they are on no dial; playback is on the pages after it */
    const char *root_end = strstr(hier, "\"knobs\"");                           /* the root level's params come before its knobs */
    int at_root = root_end != NULL;
    const char *names[] = { "\"pads_screen\"", "\"sample_file\"", "\"gen_style\"", "\"gen_bars\"", "\"gen_fwd\"", "\"gen_fin\"", "\"gen_echo\"" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *at = strstr(hier, names[i]);
        at_root &= at != NULL && at < root_end;
    }
    const char *sw_at = strstr(hier, "\"swing\"");
    CHECK(at_root && sw_at && sw_at > root_end, "the first menu has the Pads screen, Load Sample and the generator's dials, and no playback setting");
    const char *knobs_at = root_end ? strchr(root_end, '[') : NULL;
    const char *knobs_end = knobs_at ? strchr(knobs_at, ']') : NULL;
    int dials = 0, bad_dial = knobs_at == NULL || knobs_end == NULL;
    for (const char *q = knobs_at; q && knobs_end && q < knobs_end; q++) dials += *q == '"' ? 1 : 0;
    dials /= 2;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !bad_dial; i++) {
        const char *at = strstr(root_end, names[i]);
        bad_dial |= at == NULL || at >= knobs_end;                                         /* (all seven are dials, in this order) */
    }
    bad_dial |= !(strstr(knobs_at ? knobs_at : "", "\"pads_screen\"") < strstr(knobs_at ? knobs_at : "", "\"sample_file\"") &&
                  strstr(knobs_at ? knobs_at : "", "\"sample_file\"") < strstr(knobs_at ? knobs_at : "", "\"gen_style\""));
    CHECK(dials == 7 && !bad_dial && !strstr(knobs_at ? knobs_at : "", "\"sample\"") && !strstr(hier, "\"lfo\":") && !strstr(hier, "\"sync\":"),
          "the root has seven dials (the opener, Load Sample as the second, Style, Length, Forwards, Finish and Echo), none of them the sample or clip (%d dials)", dials);
    const char *l_pb = strstr(hier, "{\"level\":\"playback\""), *l_pp = strstr(hier, "{\"level\":\"pad_play\""), *l_wv = strstr(hier, "{\"level\":\"wave\"");
    CHECK(l_pb && l_pp && l_wv && l_pb < l_pp && l_pp < l_wv, "the pages after Main are Playback, Pad Play and, last of all, Waveform");
    const char *d_pb = strstr(hier, "\"playback\":{"), *d_pp = strstr(hier, "\"pad_play\":{"), *d_wv = strstr(hier, "\"wave\":{");
    CHECK(d_pb && d_pp && d_wv && strstr(d_pb, "\"swing\"") && strstr(d_pb, "\"swing\"") < d_pp && strstr(d_pb, "\"humanise\"") && strstr(d_pb, "\"humanise\"") < d_pp &&
          strstr(d_pb, "\"drag\"") && strstr(d_pb, "\"drag\"") < d_pp && strstr(d_pb, "\"clip_depth\"") && strstr(d_pb, "\"clip_depth\"") < d_pp &&
          strstr(d_pp, "\"note_gate\"") && strstr(d_pp, "\"note_gate\"") < d_wv && strstr(d_pp, "\"cues\"") && strstr(d_pp, "\"cues\"") < d_wv && strstr(d_wv, "\"waveform\""),
          "Playback has Swing, Humanise, Clip Depth and Drag; Pad Play has Note Gate and Cue Pads; the waveform page is on its own, last");
    api->destroy_instance(h);
}

/* ---- 15. the manifest --------------------------------------------------------------------------------------- */

static void write_fixtures(void)
{
    /* views for the screen test (tests/screen_test.mjs): what the engine really sends */
    FILE *f = fopen(TDIR "/screen_fixtures.json", "wb");
    if (!f) return;
    fputs("[", f);
    char js[2048];
    for (int k = 0; k < 5; k++) {
        ts_engine_t *e = ts_engine_create(44100.0f);
        load_sample(e, 0, "ahhh.wav", SMP_SINE, 0.7f, 44100);
        load_sample(e, 1, "fresssh.wav", SMP_RAMP, 0.5f, 44100);
        if (k >= 2) load_sample(e, 2, "Eagle Landed.wav", SMP_DC, 0.4f, 44100);
        ts_params_t p;
        ts_params_default(&p);
        p.pads = 0;
        p.sample = k == 3 ? 2 : (k == 1 ? 1 : 0);
        p.note_gate = 1; p.drag = 0.4f;
        if (k >= 1) { p.lock = 0; p.clip = k == 1 ? 3 : (k == 2 ? TS_CLIP_AUTO : 5); }
        if (k == 0) { p.clip = 0; p.turntable = 0.6f; }
        ts_engine_set_params(e, &p);
        float blk[128];
        for (int b = 0; b < 700 + 200 * k; b++) {
            if (b % 180 == 0) ts_engine_note(e, 60, 100, 1);
            if (b % 180 == 70) ts_engine_note(e, 60, 0, 0);
            ts_engine_render(e, blk, 128);
        }
        ts_engine_view(e, js, sizeof js);
        fprintf(f, "%s%s", k ? "," : "", js);
        ts_engine_destroy(e);
    }
    fputs("]", f);
    fclose(f);
}

static void test_manifest(void)
{
    printf("manifest\n");
    char path[512];
    snprintf(path, sizeof path, "%s/module.json", g_root);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "cannot open %s", path);
    if (!f) return;
    static char m[16384];
    size_t n = fread(m, 1, sizeof m - 1, f);
    fclose(f);
    m[n] = 0;
    CHECK(n > 500 && n < 8192, "module.json is %zu bytes (limit 8192)", n);
    CHECK(balanced(m), "unbalanced JSON");
    CHECK(strstr(m, "\"api_version\":2") && strstr(m, "\"component_type\":\"sound_generator\"") && strstr(m, "\"audio_in\":false") && strstr(m, "\"midi_in\":true"), "header fields");
    static char gen[8192];
    CHECK(ts_plugin_json_chain_params(gen, sizeof gen) > 0 && strstr(m, gen), "chain_params out of sync with code (scripts/gen_manifest.sh)");
    CHECK(ts_plugin_json_ui_hierarchy(gen, sizeof gen) > 0 && strstr(m, gen), "ui_hierarchy out of sync with code");

    /* the table and the engine agree on every default, and every parameter is on a page with at most eight knobs */
    ts_params_t d;
    ts_params_default(&d);
    int agree = 1, on_page = 1;
    for (int i = 0; i < P_COUNT; i++) {
        const pdef_t *p = &k_params[i];
        if (p->kind == K_CANVAS || p->kind == K_STRING || p->kind == K_FILEPATH) continue;
        if (is_transient(i)) continue;                                           /* written by the Pads screen, or one-shots: on no page */
        if (fabsf(pget(&d, i) - p->def) > 1e-6f) { agree = 0; fprintf(stderr, "  default of %s: table %g, engine %g\n", p->key, (double)p->def, (double)pget(&d, i)); }
        if (p->kind == K_ENUM) agree &= p->nopts >= 2 && p->def >= 0 && p->def < p->nopts;
        else agree &= p->lo < p->hi && p->def >= p->lo && p->def <= p->hi;
        int found = 0;
        for (int k = 0; k < N_(kn_root); k++) found |= kn_root[k] == i;
        static const char *const off_dial[] = { "sample", "clip", "cut_from", "pads", "lock", "normalise", "turntable" };   /* set from the pads, or kept at the default */
        for (size_t k = 0; k < sizeof off_dial / sizeof off_dial[0]; k++) found |= strcmp(p->key, off_dial[k]) == 0;
        for (int g = 0; g < N_(k_pages); g++) for (int k = 0; k < k_pages[g].nparams; k++) found |= k_pages[g].params[k] == i;
        on_page &= found;
    }
    CHECK(agree, "defaults and ranges in module.json match the engine");
    CHECK(on_page && N_(kn_root) <= 8, "every parameter has a knob");
    for (int g = 0; g < N_(k_pages); g++) CHECK(k_pages[g].nknobs <= 8 && k_pages[g].nknobs <= k_pages[g].nparams, "page %s has %d knobs", k_pages[g].id, k_pages[g].nknobs);
    int short_names = 1;
    for (int i = 0; i < P_COUNT; i++) {
        short_names &= strlen(k_params[i].key) < 32 && strlen(k_params[i].name) < 64;
        for (int o = 0; o < k_params[i].nopts; o++) short_names &= strlen(k_params[i].opts[o]) < 32;
    }
    CHECK(short_names, "keys and options fit Schwung's fixed-size fields");
    printf("  module.json: %zu bytes\n", n);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);          /* a crash must not swallow the progress lines */
    if (argc > 1) g_root = argv[1];
    test_wav();
    test_defaults();
    test_samples();
    test_playhead();
    test_cuts();
    test_pads();
    test_gain();
    test_view();
    test_clips();
    test_raw_pads();
    test_clip_files();
    test_generator();
    test_banks();
    test_unload_drops_clips();
    test_attacks();
    test_delete_any();
    test_empty_pads();
    test_swing();
    test_humanise();
    test_finish();
    test_echo();
    test_tempo();
    test_pad_mutes_clip();
    test_cues();
    test_shipped_clips();
    test_pad_runs_clip();
    test_unload_files();
    test_load_file_and_zoom();
    test_recipes_library();
    test_dedupe();
    test_recording();
    test_render_safety();
    test_plugin();
    test_state();
    test_concurrent();
    test_record_threads();
    test_plugin_record();
    test_plugin_generate();
    test_plugin_pickers();
    write_fixtures();
    test_manifest();

    printf("\n%d checks, %d failed (allocs=%zu frees=%zu)\n", g_checks, (int)g_fail, (size_t)g_allocs, (size_t)g_frees);
    int leak = g_allocs != g_frees;
    if (leak) fprintf(stderr, "  LEAK: %zu allocations, %zu frees\n", (size_t)g_allocs, (size_t)g_frees);
    return g_fail || leak ? 1 : 0;
}
