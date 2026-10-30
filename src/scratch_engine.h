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
#define TS_SLOTS         8          /* Ahhh, Fresssh, User 1..6: the eight pads of the sample row */
#define TS_MAX_SECONDS   10         /* longest sample; scratching works best with 1 to 2 s */
#define TS_REC_SECONDS   10         /* the longest recording from the audio input, as long as a loaded sample (it is cropped to its sound when it is kept) */
#define TS_REC_MIN       2048       /* a recording shorter than this (about 46 ms) is thrown away */
#define TS_REC_FLOOR     0.003f     /* ...and one never louder than this (about -50 dBFS): nothing was there to keep */
#define TS_NAME_LEN      64
#define TS_VIEW_COLS     128        /* waveform columns on the screen */
#define TS_LANE          128        /* the clip view: one bar of playhead and cuts, a column each */
#define TS_LANE_BEATS    4
#define TS_RCOLS         512        /* the Remote UI's waveform: columns of the high-resolution overview */
#define TS_RLANE         512        /* the Remote UI's clip view with no clip: what the playhead did, a column each */
#define TS_RCLIP_RES     64         /* the Remote UI's clip plan: points to the beat (half the clip's own resolution) */
#define TS_SCRATCH_PTS   64         /* the Remote UI's hand on the record: points held (a second at 60 a second) */
#define TS_SCRATCH_DELAY_MS 40.0    /* ...and how late they are played: the room the network's jitter gets */
#define TS_REACH_S       1.2f       /* a sample longer than this is a long one: its strokes are as long in seconds as on one this long, and start on its words (Ahhh and Fresssh are shorter: nothing changes for them) */
#define TS_CUT_NOTE      60         /* C3: this note and above cut the sound in */
#define TS_PAD_FIRST     36         /* the Move's 32 pads send notes 36..67 (drum layout) or 68..99 (pad ids) */
#define TS_PAD_LAST      99

/* Clips: a clip is a few bars of playhead movement (its Turntable automation) and the notes that cut the sound in. They
 * are loaded from local/clips.bin (tools/import_clips.py makes it from a Live Pack you own); without it there are eight
 * built-in one-bar clips. 0 is off; TS_CLIP_AUTO plays a clip right through, from its own start, and then a random OTHER
 * one (like a clip Follow Action set to "Other"): a one-bar clip is a bar, a four-bar clip four. A clip
 * can be made for a sample (Ahhh or Fresssh): auto picks among those for the sample in use (all of them for a user sample). While a clip runs it moves the playhead and cuts;
 * the Turntable knob still takes over for a moment, and held notes add cuts. */
#define TS_NUM_CLIPS      16                /* clip pads: the built-in or imported clips, then the ones you keep */
#define TS_PAGES          4                 /* pages of clip pads for each sample: the Pads screen shows one, 14 pads, and < and > turn the page */
#define TS_BANK           (TS_PAGES * TS_NUM_CLIPS)   /* a sample's bank, all its pages: pad p of page g is at index g * 16 + p - 1 (page 0 is where the pads always were) */
#define TS_CLIP_AUTO      17                /* the clip parameter: 0 off, 1..16 a clip, 17 auto, 18 the clip just generated */
#define TS_CLIP_TRY       18
#define TS_CLIP_PADS      14                /* clip pads 1-14: the two places after them on the pads are Generate and Auto */
#define TS_POOL           256               /* every clip there is: the built-in or imported ones, and the ones you kept */
#define TS_GEN_STYLES     15                /* any, baby, cuts, military, scribble, stab, chirp, transform, flare, crab, orbit, drag, tear, stutter, and (a trial) Improv as 1.1.0 */
#define TS_GS_IMPV_110    14
#define TS_CLIP_RES       128               /* points to the beat in a clip's data */
#define TS_CLIP_MAX_BEATS 16

typedef struct {
    int      len;                           /* beats: 4 to 16 */
    int      tag;                           /* 1: made for the Ahhh sample, 2: for Fresssh, 3: generated for the sample it is on (positions are its own), 0: for anything */
    int      n;                             /* len * TS_CLIP_RES */
    uint16_t pos[TS_CLIP_MAX_BEATS * TS_CLIP_RES];      /* the playhead, 0..65535 */
    uint8_t  gate[TS_CLIP_MAX_BEATS * TS_CLIP_RES];     /* 1: a note is on, the sound is cut in */
    uint8_t  extras;                                    /* what the generator really wrote in, as TS_EX_*: only when TS_EX_KNOWN is set (a built-in clip, or old saved data, is 0: not known) */
} ts_clip_t;

#define TS_EX_ECHO  1                       /* an echo scratch is in the clip */
#define TS_EX_FWD   2                       /* a forward is (or the run-out to the end of the sample) */
#define TS_EX_FIN   4                       /* the clip ends with the whole sample played through */
#define TS_EX_KNOWN 0x80
#define TS_EX_TMPL_SHIFT 3                  /* bits 3..6: the groove template a clip was made with, plus one (0: no groove) */

/* The taste log (off by default; the Taste Log setting): one line for each clip you keep, pass over or delete, in taste_log.csv next to the
 * library, with a few numbers measured from the clip. It stays on the Move: nothing reads it or sends it anywhere. */
#define TS_TASTE_NF 14                      /* the numbers measured from a clip */
#define TS_TASTE_Q  16
#define TS_GENERATOR_VERSION 1              /* in the log's header: bump it when the generator's output changes */

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
    int   taste_log;            /* 1: keep a log of the clips you keep, pass over and delete (taste_log.csv, local to this Move) */
    int   cues;                 /* 1: with no clip playing, the bottom row of pads are cues: each drops the needle at an attack of the sample and plays it on */
    int   clip;                 /* 0 off, 1..16 a pad of the page clip_page, 17 auto, 18 the clip just generated ("try") */
    int   clip_page;            /* 0..3: the page of the pad in clip (the page of the clip that is playing: not the page the pads show) */
    int   gen_style;            /* what Generate makes: 0 any, or one of the styles above */
    int   gen_bars;             /* 0, 1, 2: a clip of 1, 2 or 4 bars */
    float gen_fwd;              /* 0..1: how many of Any's bars have a forward (the record let go through the word); 0 = none */
    float swing;                /* 0..1: the second sixteenth of each pair played late, as clips play (0.67 = triplets, 1 = a hard dotted swing); 0 = straight */
    float humanise;             /* 0..1: how loose a clip plays: its timing drifts, each cut lands a little early or late and a little softer, the strokes a little shorter or longer; 0 = exactly as written */
    float bite;                 /* 0..1: soft saturation of the sound as it plays, harder on a fast stroke and for a moment as the fader opens; 0 = none (clean) */
    float gen_fin;              /* 0..1: how often a clip ends with the whole sample playing through at its own speed, then a pull back; 0 = never */
    float gen_echo;             /* 0..1: how often a bar of Any holds an echo scratch (a stab repeated four times, each at a lower level); 0 = never */
    float gen_groove;           /* 0..1: how far Improv keeps to a groove: its strokes land where a drum pattern leaves room for a scratch, there are rests, a fill ends each phrase and the bars breathe; 0 = as it always was */
    float clip_depth;           /* 0..2: how far a clip moves the playhead (1 = as written; the movement is scaled, then offset) */
    float clip_offset;          /* -0.5..0.5: shifts the whole clip along the sample */
    float vinyl;                /* 0..1: how much it sounds like a record under a needle: quieter as it slows (a cartridge's output follows the needle's speed), and the groove's own noise and crackle moving with it; 0 = as it always was */
} ts_params_t;

typedef struct {
    float *data;
    int    len;
    int    loaded;
    float  norm;                /* gain that brings its peak to -1 dBFS */
    char   name[TS_NAME_LEN];   /* what the screen calls it */
    char   file[TS_NAME_LEN];   /* the file it came from, if it came from one: a file is loaded once, however many folders are searched */
    uint8_t pk[TS_VIEW_COLS], rms[TS_VIEW_COLS];   /* overview per column: peak and body, 0..255 */
    uint8_t pk_hi[TS_RCOLS], rms_hi[TS_RCOLS];     /* the same, four times finer, for the Remote UI */
    float lo, hi;               /* where the sound is, 0..1: the clips scratch between these, not through the silence round it */
} ts_slot_t;

/* A generated clip is kept as its recipe: the same recipe makes the same clip. */
typedef struct { int used, style, bars; uint32_t seed; int fwd; int bpm; int fin; int echo; int groove; } ts_recipe_t;     /* fwd: the Forwards setting it was made with, 0..100; bpm: the song's tempo */

/* an event for the log, queued where it happens (maybe the audio thread) and written by ts_engine_service */
typedef struct {
    volatile int full;
    char  ev;                   /* K kept, P passed over (Generate pressed again without keeping it), D deleted */
    int   style, bars, fwd, fin, echo, bpm, sample, extras, played_ds, groove, tmpl;
    uint32_t seed;
    float f[TS_TASTE_NF];
} taste_ev_t;

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
    unsigned    note_cues;      /* the bottom-row pads held as notes: bit n is the pad in column n */
    unsigned    cue_prev;       /* the cue pads held a frame ago */
    int         cue_on;         /* 1: a cue pad is playing the sound on from its cue */
    int         cue_pad;        /* which one (0..7) */
    float       bite_atk;       /* 1 as the fader opens, falling away in a few milliseconds: Bite bites harder at the attack */
    float       bite_lp;        /* Bite's brightness: the low part of the sound (a one-pole low-pass), so the highs can be lifted before the clipping */
    double      cue_pos;        /* where it has got to, 0..1 */
    int         last_note;      /* the last note that arrived, 0 if none (the screen shows it, to see what the pads send) */
    int         running;        /* 1: the transport runs (Lock on), or Lock is off: clips play */
    uint32_t    rng;

    int         hand_hold;      /* samples left in which the knob, not the clip, moves the playhead */
    int         scratch_hold;   /* samples left in which a hand on the Remote UI's record also holds the fader open */
    /* where the hand was, and when (the page's clock, ms): replayed TS_SCRATCH_DELAY_MS late, a straight line between points, so the
     * network's uneven delivery is not heard as a stutter */
    double      sc_t[TS_SCRATCH_PTS];
    float       sc_pos[TS_SCRATCH_PTS];
    int         sc_n, sc_head;  /* points held, and where the oldest is */
    double      sc_off;         /* engine ms minus page ms, at the quickest delivery seen in this gesture */
    float       vy_g, vy_lp, vy_hp, vy_click;   /* Vinyl: the cartridge's level, the groove noise's filters, a crackle dying away */
    uint32_t    vy_rng;
    const void *bref_ck;        /* a long sample's Clip Depth: the clip whose bars' starting points are in bref, and the block they were read in */
    uint64_t    bref_at;
    float       bref[TS_CLIP_MAX_BEATS / 4];

    /* the clip view: where the playhead was and whether the sound was cut in, over one bar of the clock */
    uint8_t     lane_pos[TS_LANE], lane_gate[TS_LANE];
    int         lane_col;
    int         lane_len;       /* the beats the clip view spans: the clip's length, or a bar */
    uint8_t     hand_pos[TS_RLANE], hand_gate[TS_RLANE];   /* the same record for the Remote UI, finer: 0..255 a column */
    int         hand_col;
    uint64_t    frames;         /* samples rendered since the engine was made: the Remote UI's clock (device time) */

    ts_clip_t   clip_data[TS_POOL + 2];        /* every clip; the last two are the "try" clip, built in the one not playing, then switched to */
    int         bank[TS_SLOTS][TS_BANK];       /* each sample has its own clip pads, in pages of 16: a clip's place in clip_data, plus one; 0 is an empty pad */
    int         bank_last[TS_SLOTS];           /* the clip each sample last had selected (a pad, 0 off, auto, try)... */
    int         bank_last_page[TS_SLOTS];      /* ...and the page it was on */
    int         view_page;                     /* the page of pads the Pads screen shows (what a tap on a pad means), not the page of the clip playing */
    uint8_t     removed[TS_SLOTS][TS_BANK];   /* 1: a built-in or imported clip you took off this pad (remembered in the library) */
    int         nclips;         /* how many pads of the selected sample's bank have a clip */
    int         try_idx, try_valid;
    double      try_t0;         /* when the clip just generated began (free_clock) */
    int         try_kept, try_sample;
    taste_ev_t  taste_q[TS_TASTE_Q];
    unsigned    taste_w, taste_r;
    ts_recipe_t try_recipe, kept[TS_POOL];
    unsigned    gen_count, clips_ver;
    volatile int recipes_dirty;
    unsigned    view_seq;       /* counts audio blocks: it is in the view, so the view differs on every poll (and a screen is told) */
    int         zoom;           /* the zoomed clip view on the Pads screen: 0 off, 1..3 = a window of 4, 2 or 1 beats */
    char        recipes_path[480];
    double      auto_start, auto_end;   /* auto's choice is kept for the beats of its turn: from where the clip starts to where it ends */
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

/* TS_TASTE_NF numbers measured from a clip's data (how busy, how big its strokes, how much is cut in, ...): what the taste log records */
void ts_clip_features(const ts_clip_t *c, float *f);

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
void ts_engine_set_page(ts_engine_t *e, int page);   /* the page of clip pads the Pads screen shows, 0..TS_PAGES-1 */
void ts_engine_set_zoom(ts_engine_t *e, int zoom);      /* the zoomed clip view: 0 off, 1..3 a window of 4, 2 or 1 beats */
void ts_engine_pad_cuts(ts_engine_t *e, unsigned mask);
/* A hand on the record in the Remote UI: the playhead goes to pos (0..1) as with the Turntable, and the fader is held open while the
 * hand is on, as a DJ's is while scratching (a Turntable write alone leaves the fader where it was). Not a parameter: never saved. */
void ts_engine_scratch(ts_engine_t *e, float pos);
/* The same, with when the hand was there (t_ms, the page's own clock). The points are replayed TS_SCRATCH_DELAY_MS behind the
 * quickest delivery seen, joined by straight lines, so the record moves as the hand did however unevenly the writes arrive. */
void ts_engine_scratch_at(ts_engine_t *e, float pos, double t_ms);

/* Clips. Position 0..1 and whether the sound is cut in, at a beat of the song (a clip loops over its length). */
float ts_clip_pos(const ts_engine_t *e, int clip, double beat);
/* Swing: where in a clip to read at a moment of the song. Each eighth note is split long-short instead of half and half: the first
 * sixteenth takes 0.5 + swing/4 of it (two thirds at 67%, a triplet feel; three quarters at 100%, a hard dotted swing). The playhead path and the
 * fader move together; no clip changes. */
double ts_swing_time(double beat, float swing);
int   ts_clip_cut(const ts_engine_t *e, int clip, double beat);
/* The fader's level, 0..1: a clip's gate is 0 shut, 1 fully open, and 2 to 7 the lower levels an echo scratch steps down through. */
float ts_clip_level(const ts_engine_t *e, int clip, double beat);
int   ts_clip_auto(const ts_engine_t *e, long long bar);      /* which clip (1..) auto plays at the start of this bar of the song (a clip's turn lasts as many bars as it is long) */
int   ts_engine_load_clips(ts_engine_t *e, const char *path);  /* clips.bin: the clip count, or TS_WAV_ERR_*; file I/O */

/* The generator: a clip made from a style and a seed, from the vocabulary of scratching (babies, cuts, military, scribbles,
 * stabs, chirps, transformers, flares, crabs, orbits, drags): one, two or four bars of playhead movement and the cuts that go
 * with it, on the sixteenth-note grid, with a human feel. The same style, seed and length always give the same clip. */
void ts_clip_generate(ts_clip_t *out, int style, uint32_t seed, int bars);
/* The same, for a sample: each bar's strokes start where the sample has an attack (the start of its sound, the start of a word), the way a
 * DJ places the record, and the positions are the sample's own (the clip is "native": tag 3). With no sample it is the plain one above. */
void ts_clip_generate_ex(ts_clip_t *out, int style, uint32_t seed, int bars, const ts_slot_t *slot, float fwd, float bpm, float fin, float echo, float groove);     /* groove: 0..1, how far Improv keeps to a groove */
void ts_clip_generate_for(ts_clip_t *out, int style, uint32_t seed, int bars, const ts_slot_t *slot, float fwd, float bpm, float fin, float echo);     /* fwd: 0..1, forwards in Any */
/* (bpm is the song's tempo. Finish uses it to play the sound through at its own speed. Any is on sixteenths at every tempo, but above
 * 120 bpm its strokes and cuts are as long in seconds as at 120, so a fast song is not busier than a hand can be; up to 120 it does not matter.) */
int  ts_engine_generate(ts_engine_t *e);               /* a new "try" clip from the Generate settings; plays it; returns its style */
int  ts_engine_keep(ts_engine_t *e, int pad);          /* the try clip on a clip pad of the selected sample (0: the first free one); the pad, or -1 */
void ts_engine_select_sample(ts_engine_t *e, int sample);   /* another sample: its own clip pads, and the clip it last had selected */
void ts_engine_clip_pad(ts_engine_t *e, int n);        /* a clip pad: play it; empty while the try clip is playing: keep it there; else nothing plays */
int  ts_engine_clear_clip(ts_engine_t *e, int slot);   /* delete a clip (0: the selected one); -1 for an empty pad. The other pads stay where they are */
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
/* The Remote UI (web_ui.html) reads two more, through the host's extras pump (twice a second, and on every edit):
 *   rview   small and live: where the playhead is and is heading, the clock (beats, tempo, device time), the fader, and the
 *           status the screens show (sample, clip, pages, cues, recording). The browser runs the clip from it between reads.
 *   rclip   what changes rarely: the clip playing at TS_RCLIP_RES points to the beat (path and fader levels), the path this pass
 *           with Humanise on, the sample's overview at TS_RCOLS columns, and with no clip the record of what the playhead did. It also
 *           turns over every two seconds (sq), so the host, which sends only what changed, sends it again: a lost copy is replaced.
 * Both are flat JSON objects; neither is a parameter (a declared parameter would be read only every few seconds). */
int  ts_engine_rview(const ts_engine_t *e, char *buf, int cap);
int  ts_engine_rclip(const ts_engine_t *e, char *buf, int cap);
/*   rpads   the shapes of the clips on the pads of the page the Pads screen shows, and of the try (Generate's): a picture on each
 *           pad. Named by pk, which rview carries too, so the page knows when its pictures are another page's or out of date. */
int  ts_engine_rpads(const ts_engine_t *e, char *buf, int cap);

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
