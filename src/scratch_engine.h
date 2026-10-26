#ifndef SCRATCH_ENGINE_H
#define SCRATCH_ENGINE_H

/* TinyScratch: a turntable-scratch instrument for the Move.
 *
 * A playhead ("Turntable", 0..1) moves over a short sample; faster movement is higher pitch. It is moved by
 * hand (the Turntable knob), or by a clip (the hand takes over for a moment when you touch the knob). Drag sets how quickly the playhead follows. MIDI notes
 * at C3 and above "cut" the sound in, like a crossfader.
 *
 * Plain C, no Schwung dependency, mono float out. render() never allocates, locks or touches files. */

#include <stddef.h>
#include <stdint.h>

#define TS_SAMPLE_RATE   44100.0f
#define TS_SLOTS         6          /* Ahhh, Fresssh, User 1..4 */
#define TS_MAX_SECONDS   10         /* longest sample; scratching works best with 1 to 2 s */
#define TS_REC_SECONDS   4          /* the longest recording from the audio input (it is cropped to its sound when it is kept) */
#define TS_REC_MIN       2048       /* a recording shorter than this (about 46 ms) is thrown away */
#define TS_NAME_LEN      64
#define TS_VIEW_COLS     128        /* waveform columns on the screen */
#define TS_LANE          128        /* the clip view: one bar of playhead and cuts, a column each */
#define TS_LANE_BEATS    4
#define TS_CUT_NOTE      60         /* C3: this note and above cut the sound in */
#define TS_PAD_FIRST     36         /* the Move's 32 pads send notes 36..67 (drum layout) or 68..99 (pad ids) */
#define TS_PAD_LAST      99

/* Clips: a clip is a few bars of playhead movement (its Turntable automation) and the notes that cut the sound in. They
 * are loaded from local/clips.bin (tools/import_clips.py makes it from a Live Pack you own); without it there are eight
 * built-in one-bar clips. 0 is off; TS_CLIP_AUTO works like a clip Follow Action set to "Other" with Legato launch: at
 * the end of every bar, a random OTHER clip, played from the same position of its loop as the song has reached. A clip
 * can be made for a sample (Ahhh or Fresssh): auto picks among those for the sample in use (all of them for a user sample). While a clip runs it moves the playhead and cuts;
 * the Turntable knob still takes over for a moment, and held notes add cuts. */
#define TS_NUM_CLIPS      16                /* clip pads: the built-in or imported clips, then the ones you keep */
#define TS_CLIP_AUTO      17                /* the clip parameter: 0 off, 1..16 a clip, 17 auto, 18 the clip just generated */
#define TS_CLIP_TRY       18
#define TS_CLIP_PADS      14                /* clip pads 1-14: the two places after them on the pads are Generate and Auto */
#define TS_POOL           64                /* every clip there is: the built-in or imported ones, and the ones you kept */
#define TS_GEN_STYLES     12                /* any, baby, cuts, military, scribble, stab, chirp, transform, flare, crab, orbit, drag */
#define TS_CLIP_RES       128               /* points to the beat in a clip's data */
#define TS_CLIP_MAX_BEATS 16

typedef struct {
    int      len;                           /* beats: 4 to 16 */
    int      tag;                           /* 1: made for the Ahhh sample, 2: for Fresssh, 3: generated for the sample it is on (positions are its own), 0: for anything */
    int      n;                             /* len * TS_CLIP_RES */
    uint16_t pos[TS_CLIP_MAX_BEATS * TS_CLIP_RES];      /* the playhead, 0..65535 */
    uint8_t  gate[TS_CLIP_MAX_BEATS * TS_CLIP_RES];     /* 1: a note is on, the sound is cut in */
} ts_clip_t;

typedef struct {
    int   sample;               /* slot 0..5 */
    float turntable;            /* playhead position 0..1 across the sample (manual) */
    float drag;                 /* 0..1: how slowly the playhead follows; 0.5 is the sweet spot */
    float gain_db;              /* -24..+12 */
    int   note_gate;            /* 1: the sample is heard only while a cutting note is held */
    int   vel_to_vol;           /* 1: note velocity sets the volume */
    int   normalise;            /* 1: every sample plays at the same peak level */
    int   lock;                 /* 1: the clips follow the transport while it runs (and free-run while it is stopped) */
    int   cut_from;             /* lowest cutting note; C3 = 60 */
    int   pads;                 /* 1: the 32 pads are a fixed grid (below) instead of plain notes */
    int   clip;                 /* 0 off, 1..16 a clip, 17 auto, 18 the clip just generated ("try") */
    int   gen_style;            /* what Generate makes: 0 any, or one of the styles above */
    int   gen_bars;             /* 0, 1, 2: a clip of 1, 2 or 4 bars */
    float gen_fwd;              /* 0..1: how many of Any's bars have a forward (the record let go through the word); 0 = none */
    float swing;                /* 0..1: the second sixteenth of each pair played late, as clips play (1 = triplets); 0 = straight */
    int   speed;                /* 0 half, 1 as written, 2 double: how fast clips play against the song (double for a slow song) */
    float clip_depth;           /* 0..2: how far a clip moves the playhead (1 = as written; the movement is scaled, then offset) */
    float clip_offset;          /* -0.5..0.5: shifts the whole clip along the sample */
} ts_params_t;

typedef struct {
    float *data;
    int    len;
    int    loaded;
    float  norm;                /* gain that brings its peak to -1 dBFS */
    char   name[TS_NAME_LEN];   /* what the screen calls it */
    char   file[TS_NAME_LEN];   /* the file it came from, if it came from one: a file is loaded once, however many folders are searched */
    uint8_t pk[TS_VIEW_COLS], rms[TS_VIEW_COLS];   /* overview per column: peak and body, 0..255 */
    float lo, hi;               /* where the sound is, 0..1: the clips scratch between these, not through the silence round it */
} ts_slot_t;

/* A generated clip is kept as its recipe: the same recipe makes the same clip. */
typedef struct { int used, style, bars; uint32_t seed; int fwd; } ts_recipe_t;     /* fwd: the Forwards setting it was made with, 0..100 */

typedef struct ts_engine {
    float       sr;
    ts_params_t p;
    ts_slot_t   slot[TS_SLOTS];

    double      man;            /* the manual playhead, 0..1 (the Turntable knob) */
    double      pos;            /* where the playhead actually is: man or the clip, through Drag */
    double      target;         /* where it is heading */
    float       vel_out;        /* samples per sample at the playhead (pitch) */

    int         notes_held, last_vel;
    float       gate;           /* 0..1, ramped */

    float       bpm;
    double      clock;          /* beats on the transport, or this module's own clock without one */
    int         transport;      /* 1 while the host transport runs */
    double      free_clock;     /* beats since start, never stopped (clips with Lock off) */
    double      mclock;         /* the musical clock the clips, the cuts and the clip view share (beats) */
    double      held_clock;     /* without a transport (Lock on) the clock runs only while a cut is held, from its start */
    int         clip_now;       /* the clip in use: 1..8 (in auto, the one this pass) */
    int         pad_raw;        /* >0: samples left of raw pad mode: the Pads screen reports the pads, the notes are ignored */
    unsigned    pad_cuts;       /* the cut pads held, from the Pads screen: bit n is pad n of the bottom two rows */
    int         pad_held;       /* how many */
    int         last_note;      /* the last note that arrived, 0 if none (the screen shows it, to see what the pads send) */
    int         running;        /* 1: the transport runs (Lock on), or Lock is off: clips play */
    uint32_t    rng;

    int         hand_hold;      /* samples left in which the knob, not the clip, moves the playhead */

    /* the clip view: where the playhead was and whether the sound was cut in, over one bar of the clock */
    uint8_t     lane_pos[TS_LANE], lane_gate[TS_LANE];
    int         lane_col;
    int         lane_len;       /* the beats the clip view spans: the clip's length, or a bar */

    uint8_t     ro[TS_POOL];                   /* 1: imported from clips.bin: read-only, it cannot be deleted */
    ts_clip_t   clip_data[TS_POOL + 2];        /* every clip; the last two are the "try" clip, built in the one not playing, then switched to */
    int         bank[TS_SLOTS][TS_NUM_CLIPS];  /* each sample has its own clip pads: a clip's place in clip_data, plus one; 0 is an empty pad */
    int         bank_last[TS_SLOTS];           /* the clip each sample last had selected (a pad, 0 off, auto, try) */
    uint8_t     removed[TS_SLOTS][TS_NUM_CLIPS];   /* 1: a built-in or imported clip you took off this pad (remembered in the library) */
    int         nclips;         /* how many pads of the selected sample's bank have a clip */
    int         try_idx, try_valid;
    ts_recipe_t try_recipe, kept[TS_POOL];
    unsigned    gen_count, clips_ver;
    volatile int recipes_dirty;
    unsigned    view_seq;       /* counts audio blocks: it is in the view, so the view differs on every poll (and a screen is told) */
    int         zoom;           /* the zoomed clip view on the Pads screen: 0 off, 1..3 = a window of 4, 2 or 1 beats */
    char        recipes_path[480];
    long long   auto_bar;       /* auto's choice is kept for the bar it was made in */
    int         auto_sample, auto_nc, auto_clip;

    /* Recording from the audio input into a User slot. The render thread only ever copies input into rec_buf and swaps in
     * a finished slot; everything that allocates, frees, analyses or writes a file happens in ts_engine_service, on the
     * thread that sets parameters. */
    float      *rec_buf;
    int         rec_cap, rec_len, rec_slot;
    volatile int rec_state;     /* 0 idle, 1 recording, 2 stopped: waiting for ts_engine_service */
    float       in_peak;        /* the input level, 0..1 */
    ts_slot_t   pend;           /* a finished slot, built by the service call and swapped in by the render thread */
    int         pend_slot;
    volatile int pend_ready;
    float      *garbage[TS_SLOTS + 2];   /* sample data replaced by the render thread, for the service call to free */
    volatile int clear_req;     /* slot + 1: a User slot to empty */
    char        save_dir[480];  /* where recordings are kept (userN.wav), "" for nowhere */
} ts_engine_t;

ts_engine_t *ts_engine_create(float sample_rate);
void ts_engine_destroy(ts_engine_t *e);

void ts_params_default(ts_params_t *p);
void ts_params_sanitize(ts_params_t *p);
void ts_engine_set_params(ts_engine_t *e, const ts_params_t *p);

void ts_engine_set_tempo(ts_engine_t *e, float bpm);
/* The transport's position in beats, or < 0 when it is not running. */
void ts_engine_sync_beat(ts_engine_t *e, double beat);

/* MIDI notes: C3 and above cut the sound in while held; lower notes do nothing.
 *
 * With Pads on, notes 36..99 are the Move's 32 pads, read as a grid whatever the note numbers are (pad = (note-36) & 31,
 * counting from the bottom-left, eight to a row, so a clip recorded from the pads plays back the same):
 *   bottom row       8 cut pads          second row   Ahhh, Fresssh, User 1-4, (nothing), (Record: the Pads screen's)
 *   top two rows     clips 1..14 of the selected sample, 1-8 on the top row (an empty one keeps the clip just generated), then
 *                    Generate and Auto as the last two pads of the second of those rows
 * Notes outside 36..99 still follow the rule above. */
int ts_pad_index(int note);                              /* 0..31, or -1 if the note is not a pad */
void ts_engine_note(ts_engine_t *e, int note, int velocity, int on);
void ts_engine_all_notes_off(ts_engine_t *e);

/* Raw pads. The Move turns a pad press into a note by its own layout, octave and transpose, so the notes a module hears
 * move about. The full-screen Pads screen is told the real pad, so it reports them: ts_engine_pad_raw(1) says "I am
 * here" (call it again at least every second; it lapses by itself 1.5 s after the last call, so a screen that goes away
 * cannot leave the notes ignored) and while it holds, notes are ignored; ts_engine_pad_cuts is the cut pads held. */
void ts_engine_pad_raw(ts_engine_t *e, int on);
void ts_engine_set_zoom(ts_engine_t *e, int zoom);      /* the zoomed clip view: 0 off, 1..3 a window of 4, 2 or 1 beats */
void ts_engine_pad_cuts(ts_engine_t *e, unsigned mask);

/* Clips. Position 0..1 and whether the sound is cut in, at a beat of the song (a clip loops over its length). */
float ts_clip_pos(const ts_engine_t *e, int clip, double beat);
/* Swing: where in a clip to read at a moment of the song. Each eighth note is split long-short instead of half and half: the first
 * sixteenth takes 0.5 + swing/6 of it (two thirds at 1, a triplet feel). The playhead path and the fader move together; no clip changes. */
double ts_swing_time(double beat, float swing);
int   ts_clip_cut(const ts_engine_t *e, int clip, double beat);
int   ts_clip_auto(const ts_engine_t *e, long long bar);      /* which clip (1..) auto plays in this bar of the song */
int   ts_engine_load_clips(ts_engine_t *e, const char *path);  /* clips.bin: the clip count, or TS_WAV_ERR_*; file I/O */

/* The generator: a clip made from a style and a seed, from the vocabulary of scratching (babies, cuts, military, scribbles,
 * stabs, chirps, transformers, flares, crabs, orbits, drags): one, two or four bars of playhead movement and the cuts that go
 * with it, on the sixteenth-note grid, with a human feel. The same style, seed and length always give the same clip. */
void ts_clip_generate(ts_clip_t *out, int style, uint32_t seed, int bars);
/* The same, for a sample: each bar's strokes start where the sample has an attack (the start of its sound, the start of a word), the way a
 * DJ places the record, and the positions are the sample's own (the clip is "native": tag 3). With no sample it is the plain one above. */
void ts_clip_generate_for(ts_clip_t *out, int style, uint32_t seed, int bars, const ts_slot_t *slot, float fwd);     /* fwd: 0..1, forwards in Any */
int  ts_engine_generate(ts_engine_t *e);               /* a new "try" clip from the Generate settings; plays it; returns its style */
int  ts_engine_keep(ts_engine_t *e, int pad);          /* the try clip on a clip pad of the selected sample (0: the first free one); the pad, or -1 */
void ts_engine_select_sample(ts_engine_t *e, int sample);   /* another sample: its own clip pads, and the clip it last had selected */
void ts_engine_clip_pad(ts_engine_t *e, int n);        /* a clip pad: play it; empty while the try clip is playing: keep it there; else nothing plays */
int  ts_engine_clear_clip(ts_engine_t *e, int slot);   /* delete a clip (0: the selected one); -1 for an empty pad or an imported (read-only) clip. The other pads stay where they are */
int  ts_engine_restore_kept(ts_engine_t *e, int sample, int pad, int style, uint32_t seed, int bars);   /* (made with Forwards at 75%) */   /* the pad, -1 for an error, -2 if that sample already has it */
/* The kept clips are a library in a file, so the next instance (on any track) has them: one line per clip, "sample pad style bars seed". */
void ts_engine_set_recipes_path(ts_engine_t *e, const char *path);
int  ts_engine_load_recipes(ts_engine_t *e);           /* at start; returns how many */

/* Samples. Slot 0 is ahhh.wav, slot 1 fresssh.wav, slots 2..5 are user1.wav .. user4.wav (what you recorded), and any other
 * .wav files fill the User slots that are left, alphabetically. A file found again by another folder loads once.
 * Loading does file I/O: call from create / load threads only. */
int  ts_engine_load_dir(ts_engine_t *e, const char *dir);

/* Recording. ts_engine_rec_start picks the slot (the selected User slot; for Ahhh or Fresssh the first empty User slot),
 * selects it and returns it. The input is fed in by the plugin, a block at a time, mono. It stops by itself after
 * TS_REC_SECONDS or when told to; ts_engine_service then turns it into the slot's sample (and keeps it as userN.wav in
 * save_dir, which the next start loads), so it must be called from the thread that sets parameters, never from the audio
 * thread. ts_engine_clear_slot empties a User slot (and deletes its file) the same way. */
int  ts_engine_rec_start(ts_engine_t *e);
void ts_engine_rec_stop(ts_engine_t *e);
void ts_engine_feed_input(ts_engine_t *e, const float *mono, int n);
void ts_engine_service(ts_engine_t *e);
void ts_engine_clear_slot(ts_engine_t *e, int slot);
void ts_engine_set_save_dir(ts_engine_t *e, const char *dir);
/* Loading a file the user picked into a User slot (the selected one; from Ahhh or Fresssh, the first empty one): like a
 * recording, it is built off the audio thread, handed over, and kept as userN.wav. Returns the slot, or < 0 (a bad or too
 * big file, or the last hand-over not taken yet). */
int  ts_engine_load_file(ts_engine_t *e, const char *path);
int  ts_engine_load_wav_mem(ts_engine_t *e, int slot, const char *name, const uint8_t *d, size_t n);

void ts_engine_render(ts_engine_t *e, float *out, int frames);

/* What the screen shows, as compact JSON. Returns the length, or -1 if it does not fit (and buf is empty). */
int  ts_engine_view(const ts_engine_t *e, char *buf, int cap);

enum {
    TS_WAV_OK = 0,
    TS_WAV_ERR_FORMAT = -1,
    TS_WAV_ERR_UNSUPPORTED = -2,
    TS_WAV_ERR_SHORT = -3,
    TS_WAV_ERR_IO = -4
};
/* Decodes 16-bit PCM (stereo is mixed down to mono), resampled to dst_sr. Returns frames or TS_WAV_ERR_*. dst == NULL validates only. */
int  ts_wav_decode(const uint8_t *d, size_t n, float *dst, int cap, float dst_sr);

#endif
