/* audio_convert: any FLAC / WAV / AIFF / MP3 -> TinyScratch-ready WAV (mono, 16-bit, 44.1 kHz).
 *
 *   audio_convert IN OUT [--start SECONDS] [--len SECONDS] [--trim] [--peak DB]
 *
 *   --trim     cut the silence off both ends (anything under -50 dB of the peak), keeping 5 ms of lead-in
 *   --peak DB  normalise to DB dBFS instead of -1
 *
 * Decodes with mackron's public-domain dr_flac / dr_wav / dr_mp3 (fetched into
 * build/third_party by scripts/fetch_audition_samples.sh, never committed),
 * mixes to mono, cuts the excerpt, removes DC and rumble, resamples with a
 * windowed-sinc low-pass (old transfers are often 96 kHz; naive interpolation
 * would fold their hiss back into the audible band), normalises to -1 dBFS and
 * fades the ends so a cut excerpt cannot click.
 *
 * Development tool only: not part of the module. */
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_SR 44100.0

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++)
        if ((s[a - b + i] | 32) != (suf[i] | 32)) return 0;
    return 1;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned long v) { put16(f, v & 0xFFFF); put16(f, (v >> 16) & 0xFFFF); }

static int write_wav(const char *path, const float *x, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 2);
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, (unsigned long)OUT_SR); put32(f, (unsigned long)OUT_SR * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, n * 2);
    for (size_t i = 0; i < n; i++) {
        double v = x[i] * 32767.0;
        v = v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v);
        put16(f, (unsigned)(uint16_t)(int16_t)lrint(v));
    }
    return fclose(f) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IN OUT [--start S] [--len S] [--trim] [--peak DB]\n", argv[0]);
        return 2;
    }
    double start = 0.0, len = 0.0, peak_db = -1.0;
    int trim = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--trim")) trim = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--start")) start = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--len")) len = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--peak")) peak_db = atof(argv[++i]);
    }
    if (peak_db > 0.0) peak_db = 0.0;

    unsigned int ch = 0, sr = 0;
    drwav_uint64 frames = 0;
    float *pcm = NULL;
    if (ends_with(argv[1], ".flac")) {
        drflac_uint64 fr = 0;
        pcm = drflac_open_file_and_read_pcm_frames_f32(argv[1], &ch, &sr, &fr, NULL);
        frames = fr;
    } else if (ends_with(argv[1], ".mp3")) {
        drmp3_config cfg;
        drmp3_uint64 fr = 0;
        pcm = drmp3_open_file_and_read_pcm_frames_f32(argv[1], &cfg, &fr, NULL);
        ch = cfg.channels;
        sr = cfg.sampleRate;
        frames = fr;
    } else {
        pcm = drwav_open_file_and_read_pcm_frames_f32(argv[1], &ch, &sr, &frames, NULL);   /* WAV and AIFF */
    }
    if (!pcm || !ch || !sr || !frames) {
        fprintf(stderr, "audio_convert: cannot decode %s\n", argv[1]);
        return 1;
    }

    /* mono */
    size_t n = (size_t)frames;
    float *mono = (float *)malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        double s = 0.0;
        for (unsigned c = 0; c < ch; c++) s += pcm[i * ch + c];
        mono[i] = (float)(s / ch);
    }
    free(pcm);

    /* excerpt */
    size_t a = (size_t)(start * sr);
    if (a >= n) a = 0;
    size_t b = len > 0.0 ? a + (size_t)(len * sr) : n;
    if (b > n) b = n;
    float *x = mono + a;
    size_t m = b - a;

    if (trim && m > 0) {
        float pk = 0.0f;
        for (size_t i = 0; i < m; i++) pk = fmaxf(pk, fabsf(x[i]));
        float th = pk * 0.00316f;                    /* -50 dB */
        size_t lo = 0, hi = m;
        while (lo < m && fabsf(x[lo]) < th) lo++;
        while (hi > lo && fabsf(x[hi - 1]) < th) hi--;
        size_t lead = (size_t)(0.005 * sr);
        lo = lo > lead ? lo - lead : 0;
        if (hi > lo + 64) { x += lo; m = hi - lo; }
    }

    /* DC / rumble: one-pole high-pass at 25 Hz */
    double hp = exp(-2.0 * M_PI * 25.0 / sr), px = 0.0, py = 0.0;
    for (size_t i = 0; i < m; i++) {
        double y = hp * (py + x[i] - px);
        px = x[i];
        py = y;
        x[i] = (float)y;
    }

    /* windowed-sinc resample to 44.1 kHz */
    double ratio = (double)sr / OUT_SR;
    double fc = 0.5 * (ratio > 1.0 ? 1.0 / ratio : 1.0) * 0.92;   /* cycles per source sample */
    int half = (int)ceil(16.0 / (2.0 * fc));
    size_t out_n = (size_t)floor((double)m / ratio);
    float *out = (float *)calloc(out_n ? out_n : 1, sizeof(float));
    for (size_t i = 0; i < out_n; i++) {
        double t = (double)i * ratio;
        long k0 = (long)floor(t);
        double acc = 0.0, wsum = 0.0;
        for (long k = k0 - half + 1; k <= k0 + half; k++) {
            if (k < 0 || (size_t)k >= m) continue;
            double d = t - (double)k;
            double arg = 2.0 * fc * d;
            double sinc = fabs(arg) < 1e-9 ? 1.0 : sin(M_PI * arg) / (M_PI * arg);
            double u = d / half;
            if (fabs(u) >= 1.0) continue;
            double w = 0.42 + 0.5 * cos(M_PI * u) + 0.08 * cos(2.0 * M_PI * u);   /* Blackman */
            double h = 2.0 * fc * sinc * w;
            acc += x[k] * h;
            wsum += h;
        }
        out[i] = (float)(wsum != 0.0 ? acc / wsum : 0.0);
    }

    /* fade the ends so a cut excerpt cannot click (a trimmed one-shot keeps its hard attack: 0.5 ms in),
     * then normalise what is left to the requested peak (-1 dBFS) */
    size_t fi = (size_t)((trim ? 0.0005 : 0.005) * OUT_SR), fo = (size_t)((trim ? 0.008 : 0.020) * OUT_SR);
    for (size_t i = 0; i < out_n; i++) {
        double e = 1.0;
        if (i < fi) e = (double)i / fi;
        if (out_n - 1 - i < fo) e = fmin(e, (double)(out_n - 1 - i) / fo);
        out[i] = (float)(out[i] * e);
    }
    double peak = 0.0;
    for (size_t i = 0; i < out_n; i++) peak = fmax(peak, fabs(out[i]));
    double g = peak > 1e-9 ? pow(10.0, peak_db / 20.0) / peak : 1.0;
    for (size_t i = 0; i < out_n; i++) out[i] = (float)(out[i] * g);

    int r = write_wav(argv[2], out, out_n);
    printf("%s: %u Hz x%u, %.1f s -> %.1f s at 44.1 kHz (peak %.3f, gain %+.1f dB)\n", argv[2], sr, ch,
           (double)n / sr, (double)out_n / OUT_SR, peak, 20.0 * log10(g));
    free(mono);
    free(out);
    return r ? 1 : 0;
}
