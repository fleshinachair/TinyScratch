# TinyScratch

A turntable-scratch instrument for the Ableton Move (a Schwung sound generator).

A playhead moves forwards and backwards over a short sample, and a MIDI note "cuts" the sound in, like a
crossfader. Faster movement of the playhead is a higher pitch, slower is lower. That is the whole idea; combine
a movement with a pattern of cuts, ideally at different lengths (16th notes over a 1/3 bar movement), and you have a
baby, a flare, a chirp, an orbit or a transformer. TinyScratch plays those for you (the clips), or you play them.

**Heavily inspired by the Live Scratcher module by Andrew Norris.** TinyScratch is written from scratch: it contains none of that
module's code and none of its clips. The vocabulary of the clip generator comes from scratch notation (the Turntablist
Transcription Methodology, TTM).

    scripts/test.sh                   engine + plugin tests (UBSan, leak and overrun checks) and the screen test
    scripts/build.sh                  aarch64 dsp.so + build/tinyscratch-<version>.tar.gz
    scripts/install_move.sh           build, copy to the Move over SSH, verify, restart
    scripts/fetch_audition_samples.sh public-domain samples of your own to try as a User sample (samples_audition/, gitignored)
    tools/import_clips.py             reads clips (playhead automation + notes) from an Ableton Live Pack you own into local/clips.bin

## What it does

| | |
|---|---|
| **Sample** | six slots: **Ahhh**, **Fresssh**, **User 1-4**, chosen with the second row of pads. Best with samples of one to two seconds, noisy and aggressive, with a little silence at each end |
| **Clip** | what plays the scratch for you: **Clip 1-14** (the top two rows of pads), **Auto** (a random other clip every bar), **Try** (the clip you just generated) or **Off**. **Auto by default** |
| **Drag** (More page) | how quickly the playhead follows. Low is responsive and "digital", high is heavy and "analogue"; the middle is the sweet spot |
| **Gain** (More page) | -24 to +12 dB |
| **Clip Depth** / **Clip Offset** (More page) | how far the clip moves the playhead (0-200%) and where along the sample it moves it (-0.5 to +0.5): turn a clip into a smaller or a shifted version of itself |
| **Style** / **Length** | what the Generate pad makes: one of the styles (or Any) and 1, 2 or 4 bars |
| **Pads** (knob 1) | opens the full-screen version, where the pads are coloured and the dials are Style, Length, Forwards and Swing (below; Back leaves) |

**The dials.** The module's first menu has the Pads screen (knob 1, which opens it), then four dials: **Style**, **Length**,
**Forwards** and **Swing**, and **Load Sample** (the Move's file browser). The same four dials work on the Pads screen (dials 2 to 5). **Forwards**
(0 to 100%, 0 to begin with) is how often a clip made by Any lets the record go through most of the sample, so the word is heard (and,
at a quarter of that, how often a clip ends by running on to the sample's end); at 0 there are none. It only changes what Generate makes
next: a clip you kept stays as it is. **Swing** (0 to 100%, 0 to begin with) is how every clip *plays*: each pair of sixteenths is played
long-short instead of even (at 100% a triplet feel: the first takes two thirds of the eighth), the playhead and the fader together, like
the "Swing" versions of the example clips. It changes no clip; at 0 everything plays exactly as it is. It is saved with the project.
Touch one and its name and current value show on the bottom line (white on black) for as long as your finger is on it, so you
can check it is the right one before you turn it; turn it and the new value shows for a moment; Style and Length are always shown there (for instance
`CRAB 2B`). Choosing the sample and the clip, and everything about Generate, is on the pads, so none of that is on a dial.
The **More** page has the rest: **Drag**, **Gain**, **Note Gate** (on: the red pads and notes work as the fader, which
cuts a playing clip out and, with no clip, opens the sound while held; off: they do nothing and you always hear the playhead), **Vel to Vol**, and **Clip Depth** and **Clip Offset** (kept off
the Pads screen so they cannot be moved by accident: they change how every clip plays, not the clips themselves, and are saved with the
project; 100% and 0 are the clips as generated). Recording and unloading a sample are on the pads.
Lock (the clips follow the transport), Normalise, Cut From and Pad Grid keep their defaults and are saved with the project.

### Clips: the scratching

A clip is a few bars (2 to 4) of **playhead movement** (smooth curves) and **the notes that cut the sound in** (a pattern with
a human feel, open about half the time). **Clip** picks one, or **Auto**, which works like an Ableton clip Follow Action
set to "Other" with Legato launch: at the end of each bar it goes to **another random clip**, played from the same place in
its loop as the song has reached. **Every sample has its own bank of 16 clip pads**: the Ahhh clips are on Ahhh's pads, the
Fresssh ones on Fresssh's, and a user sample starts with none (they are empty until you generate and keep some). Choosing
a sample switches the pads, and **brings back the clip that sample last had selected** (Off and Auto included). Auto
picks among the clips on the selected sample's pads. With two to choose from it is the other one every bar; otherwise a
shuffle, so none comes twice running and all are heard before any comes round again. It depends only on the bar number,
so it stays with the song when the transport jumps. **An empty pad plays nothing**: select one and the scratching stops.

**Where the clips come from.** Every sample starts with empty pads: the clips are the ones you generate and keep (below). (The eight
hand-written one-bar clips of earlier versions are no longer on the pads; they remain in the code for the tests.) A fresh install starts with
a handful of kept clips (`clips/recipes.txt`).
Longer, hand-made clips can be imported from an Ableton Live Pack you own: `python tools/import_clips.py "your pack.alp"`
reads every MIDI clip that has playhead (Turntable) automation and notes into `local/clips.bin` (each goes on the pads of the
sample it was made for, by its name). That file is someone else's work, so it stays out of the repository (`local/` is
gitignored); `scripts/install_move.sh` sends it to the Move when it is there. (At the moment no imported clips are installed: the installer sends `local/clips.bin` only if it is there.) Imported clips go on the pads of the sample they were made for
(Ahhh, Fresssh), replace the built-in eight, and are **read-only**: they can be played but not deleted.

### Generating clips: a recipe generator

**How the generator was tuned.** Clips from a real scratch DJ's playing and a set of hand-made example clips were measured, and the
generator was tuned to the numbers (the example clips are not in this repository and nothing of them is copied; only these measures were
used). The sources are Kjetil Falkenberg Hansen and Roberto Bresin, [Analysis of a genuine scratch performance](https://www.speech.kth.se/~hansen/files/thesis/HansenBresin03_GW.pdf)
(2004), and Hansen's doctoral thesis [The acoustics and performance of DJ scratching](https://www.speech.kth.se/~hansen/files/thesis/KjetilFalkenbergHansen_2010_phdthesis.pdf) (2010).

| what was measured | a real DJ / the example clips | **Any** now |
|---|---|---|
| how a clip is built | improvised phrases, not techniques glued together: no pulse on the beat (the clips hardly match themselves a beat later), a two-bar phrase that comes back varied | a two-bar phrase of strokes of every length, then the same phrase with some strokes changed |
| stroke lengths | a sixteenth 22%, two 24%, three 21%, four 14% | 28%, 28%, 25%, 18% |
| when the fader opens | on the sixteenth grid (57% in its first quarter, 19% just before), for about half a sixteenth | 81% on the grid, 19% just before |
| the record's turns hidden by the fader | 70-90% / 99% | 100%: the record turns a little ahead of the grid and the fader cuts in on the beat, with the record already moving fast (the bite) |
| time the fader is open | about half / 46% | 45% |
| sounds a beat | 5.7 a second (a stream of sixteenths at 90 bpm) / 3.1 a beat | 3.2 a beat |
| longest opening | under half a second / about a beat | under a beat |
| strokes that are the same length as the next, that start and end on the same spot | 30%, 25% / 57%, 47% | 47%, 43% |
| shape of a stroke | speed highest at the start, falling to about a third by the end | a "flick": position ln(1+6t)/ln 7 |
| stroke size | 0.47 of the sample, 3.2 sample-lengths of travel a bar | 0.47 and 3.3 |
| where the strokes turn | where the sample is quiet and about to get louder | each phrase starts just inside an attack of the sample |
| forwards (the record let go through most of the sample, so the word is heard) | on the fourth beat of most bars / on beat 3 (for a beat) or beat 4 (to the bar line), 0.9 a bar on Fresssh, 0.25 on Ahhh | on beat 3 for a beat or on beat 4 to the bar line (never on an odd sixteenth), as many bars as the Forwards dial says (three in four at 75%), at about the sample's own speed (at 120 bpm): 0.84 a bar on Fresssh, the fader held open from the beat, then a quick silent pull |

The named styles (Baby, Cuts, ...) keep their own shapes and use the same fader on the grid and the same turn ahead of the beat. The Live
Scratcher manual's description of a scratch, a rhythm of cuts (sixteenths) over a movement on another interval, is the same idea.

Strokes bite in and settle; Baby can also be built from shark-tooth strokes (a curved climb and a short shut drop, or the reverse), strokes
run from two sixteenths up to half a bar, about one clip in five ends by running on to the end of the sample (a stroke on to the sample's
tail, then a silent jump back), and small cuts are flicked all over the bar on a Euclidean rhythm (fewer to the bar in a 1 bar clip, which loops
often, than in a 4 bar one). What was *not* matched: the example clips keep the fader open less of the time (46%) than the generator does (61%
overall), and the real DJ's strokes are not constant in speed (the platter turns in an arc), which the generator does not model.

**It looks at the sample.** A DJ puts the record where the sound bites: in the real performances and the example clips the strokes turn where
the sample is quiet and about to get louder (their turning points sit at about 0.4 of the loudest level, on a rising slope, against 0.65 on
average). So **Generate reads the sample on the pads**: it finds the attacks (the start of the sound, and any strong rise out of a real dip,
such as a new word or the next hit), starts every bar's strokes just inside one of them, and uses the sample's own positions (the clip is
tagged for that sample, and plays at its positions instead of being stretched over where the sound is). A clip of four bars can visit more
than one attack. Ahhh and Fresssh each have one clear attack (their start; the "ssh" of Fresssh grows gradually, so it is not found), so
their clips begin there; a sample with several words or hits gets clips that begin on them. A kept clip is regenerated from its recipe against the
same sample, so unload a sample (which also removes the clips kept for it) before putting a different one in its slot.

**Mostly sound, not silence.** A scratch is almost never quiet, so each clip is measured as it is made: the share of it where the fader is
open *and* the playhead is moving (a stopped playhead is silence whatever the fader does). A seed that comes out too quiet for its style is
thrown again (the same seed always gives the same clip), strokes fill the whole bar, and there is never a rest of a beat or more. The
quietest styles are the chopped ones (Stab, Transform) by nature.

**Generate** makes a new clip, plays it at once (as **Try**) and lets you keep it if you like it. The clips are made from the
vocabulary of scratching, in 1, 2 or 4 bars on the sixteenth-note grid, with a human feel, and they are for any sample (they are
spread over where the sample has its sound). **Style**: **Any** (a phrase that strings styles together: each bar follows
from the last the way a scratcher would, by a small Markov chain), or one of **Baby** (rubbing back and forth), **Cuts** (the
fader shut on the pull-back), **Military** (cuts and rubs), **Scribble**, **Stab** (short fast jabs, placed on a Euclidean
rhythm), **Chirp** (a click at every turn), **Transform** (a slow stroke chopped on a Euclidean rhythm), **Flare** (clicks
in the middle of each stroke), **Crab** (four quick clicks to a sixteenth), **Orbit** or **Drag**. The vocabulary is from the TTM
(Turntablist Transcription Methodology); the Euclidean rhythm and the Markov chain are standard ideas from algorithmic
composition, written here from scratch.

All of it is on the pads, apart from the two settings (**Style**, **Length** and **Forwards**, dials 2 to 4 of the Pads screen).

- **Trigger it** with the **Generate** pad (second row, right end) on the Pads screen. Press it again for another one.
- **Keep it** by tapping an **empty clip pad** while the generated clip is the one playing. It goes on that pad of the
  selected sample, and is a normal clip from then on (**Auto** includes it). Tap an empty pad at any other time and it is
  just selected: nothing plays.
- **Delete it**: hold the clip pad for a second and a half (a bar fills along the screen and the pad goes red; let go before
  the end and nothing happens). **Any clip can be deleted**, built-in ones too; it is remembered (in `recipes.txt`) and only for that
  sample. The other pads stay where they are (pads do not shuffle up, so what you have learned stays put) and the next clip you keep
  takes the first free pad. (The Move's Delete button is read by the host itself and is never passed to a screen, so it cannot be used.)
- **A kept clip is kept exactly**: its own playhead path and fader are saved beside the recipes (`kept.bin`), so it sounds the same
  whatever the generator becomes later; only a clip with no saved data is made again from its recipe. A kept clip keeps its pad: a built-in
  or imported clip never pushes it off.
- **Kept clips are a library**: a kept clip is stored as a tiny recipe (sample, pad, style, seed, length) in `recipes.txt` in the
  module's folder, so every new instance of the module, on any track, starts with the same clips on the same pads, and
  `scripts/install_move.sh` leaves the file alone. (An instance that is already running picks up changes the next time it loads.)

### Playing the clips

- **Press play on the Move and the clip plays. Stop the transport and it stops.** Nothing plays by itself while the
  transport is stopped; the screen says PRESS PLAY.
- **Hold a cut pad (or any note from C3 up) with the transport stopped**: the clip plays from its start for as long as
  you hold it, with its own cuts, and stops when you let go.
- With the transport **running**, the red pads are the crossfader on top of the clip: **hold one and the sound cuts out** (a 2 ms fade)
  wherever the clip had it open; let go and the clip's own cuts carry on. (Needs Note Gate on, which it is by default.)
- **Clip off**: only notes cut (a held note or pad opens the sound), and the playhead stays where it is.
- **Lock** (on by default) is what ties the clip to the transport; with it off the clip would run by itself, whatever the
  transport does.
- **To stop it**: stop the transport, tap the **Auto** pad again (it turns Auto off), or select an empty clip pad.

### Notes

**Any note at C3 (60) and above cuts the sound in while it is held**, and notes below do nothing.
Play them from the Move's sequencer: a clip of notes is the pattern, and a clip is the movement.

**Pad Grid** (on by default) is for playing on the 32 pads, whose notes would otherwise mostly land below C3. Notes
36 to 99 are read as a grid whatever their numbers (both the drum layout, 36-67, and the pad ids, 68-99), counting from
the bottom-left, eight to a row, so a clip recorded from the pads plays back the same:

| row | pads |
|---|---|
| top two | **clips 1 to 14** of the selected sample: 1-8 on the top row, 9-14 below it (tap an empty one to keep the clip you just generated there), then **Generate** and **Auto** (a random clip every bar; tap it again to turn it off) as the last two pads |
| second | **Ahhh, Fresssh, User 1, 2, 3, 4** (hold an empty one to record into it; hold a User sample to unload it), then two pads not used |
| bottom | 8 cut pads: with a clip running, hold one to cut the sound out; stopped, hold one to play the clip |

Notes outside 36-99 keep the rule above. Turn Pad Grid off for plain notes.

**The Move turns a pad press into a note by its own layout, octave and transpose**, so the notes a module hears move about
with the octave buttons. The grid above therefore only lines up in one octave. **The full-screen Pads screen fixes that**: it is
told which pad was pressed (the raw pad, 68-99, whatever the octave), so while it is open **the pads are always this grid**:
it sets the clip and the sample itself and tells the DSP which cut pads are held, and the DSP ignores the notes the pads
send. It lets go by itself a second and a half after the screen stops saying it is there, so closing it (or it going away)
always gives the notes back. While it is open, notes from the Move's sequencer are ignored too; close it to let a recorded
clip of notes cut. The Pads screen's status line shows the last raw pad (P), RAW or NOTE (how the DSP is reading the pads) and
whether the pad lights are being written (LED). **Turn the jog wheel right for a zoomed view** of the clip: the path and the notes
for the next 4, 2 or 1 beats, scrolling past a fixed "now" a quarter of the way in, with a line for every beat (solid on each
bar) and, in Auto, the next bar's clip arriving at the bar line. Turn it left to zoom back out.

### The screens

Two pages, and the full-screen Pads screen. The first is the picture: the **name of the sample in use** in big letters (with a block and dots under the
vinyl for the six slots); the waveform (the body of the sound solid, the peaks drawn round it) with the playhead through
it, and a tick under it where the clip is sending it; the **vinyl** (the white line starts at 12 o'clock and goes once round
as the playhead crosses the sample); and the **clip view** along the bottom: the whole clip (2 to 4 bars) in a column each,
with the playhead's path as a line, the **cuts as blocks**, a tick on every beat (a taller one on each bar) and a cursor
for now. With a clip it is the clip's plan; with none, what the playhead just did. Beside it, the clip, the last note that
arrived (N) and PRESS PLAY when a clip is waiting for the transport.

The second page, **Waveform**, is the sample on its own, full size: a column to every pixel, the playhead through it, a
bar along the top that lights while the sound is cut in, a ruler in tenths of the sample, a mark for where the clip is
sending the playhead, and the playhead's speed (1.0X is the sample's own pitch).

Open **Pads** (knob 1 of the first menu) for the first picture full screen with the pads coloured: cuts red (white while held), samples
yellow (white for the one in use, dim for an empty slot), clips violet (white for the one picked; in Auto the one playing
now is green; an empty pad is dark, or dim while there is a generated clip to keep), Auto green, Generate orange (white while
the generated clip is the one playing). A clip you made, held, goes red until it is deleted.
**The Move's pad lights are the Move's**: when this screen is closed, Back is pressed, or it stops being drawn (another view is
in front, or the Move takes the display back for another slot), every pad is repainted once with the colour the Move itself had
(read from the shim's snapshot of the Move's own writes, as TinySID does), so nothing stays over another view. While the screen is up
it also puts back, at once, any pad the Move lights itself (a pressed pad flashes, and so does its twin an octave away).

## Samples

`samples/ahhh.wav` and `samples/fresssh.wav` ship with the module: the two classic scratch sounds that turntablists use as
their basic tools, which come from the end of "Change the Beat" (Fab 5 Freddy, 1982) and are described as public domain
(see samples/CREDITS.txt).

**User 1-4** start empty (an empty slot is silent and the screen says EMPTY). Fill them by:

- **Recording** from the Move's audio input: **hold an empty User slot's pad** and it records for as long as you hold it (up to four
  seconds); let go and it is kept. (To record again into a slot that has a sample, unload it first.) The title becomes REC with the
  seconds so far and a bar for the input level.
  A recording is **cropped to its sound** before it is kept: the silence before and after it goes (a few milliseconds are left before
  the first sound so its attack is whole), the first 40 ms and the last 60 ms are not listened to at all (the press and the let-go of the
  pad, which the Move's microphone, right beside the pads, hears), and one with no sound in it is thrown away. It is also made **as loud as Ahhh and
  Fresssh**: its average level (where there is sound) is brought to about -14 dBFS, with at most 24 dB of gain, and the peaks that would
  go over are rounded off by a soft limiter, so a voice from the microphone is not quieter than the built-in samples. It is that slot's sample,
  and is kept as `user1.wav` .. `user4.wav` in the module's `samples/` folder, so it is there next time (and `scripts/install_move.sh`
  leaves those files alone). Which input it hears (the mic or the line in) is whatever the Move
  itself is set to listen to. **To unload a User sample**, hold its pad for a second and a half (a bar fills along the screen and the pad goes red): the slot
  is emptied and the clips you kept for it go too. A recording, or a file you picked (the copy this module made as `userN.wav`), is deleted
  with it; a file you copied into the module's `samples/` folder yourself is never deleted, just listed in `unloaded.txt` so it is not
  loaded again. Ahhh and Fresssh cannot be unloaded.
- **Load Sample** (in the first menu): opens the Move's file browser, showing `.wav` files, starting in the Move's own
  sample library (`UserLibrary/Samples`). The chosen file goes into the selected blank User slot (from
  Ahhh or Fresssh, the first empty one) and is kept as `userN.wav`, like a recording. A module cannot open the browser by
  itself when you select a blank slot, so it is one click in the menu.
- **Copying files** into the module's `samples/` folder: any `.wav` (16-bit, mono or stereo, any rate, up to ten seconds)
  that is not one of the names above fills the User slots that are left, alphabetically. `scripts/install_move.sh` also
  pushes anything in `samples_audition/` (gitignored: a place for samples of your own to try).

`tools/audio_convert.c` (`scripts/build_converter.sh`) converts FLAC / WAV / AIFF / MP3 to that format and can cut a
one-second excerpt.

## How it fits Schwung

- The DSP serves the screens one JSON string, `view`; `scratcher.js` and `waveform.js` are canvas scripts that only draw it.
- `module.json` is generated from the plugin's parameter table (`scripts/gen_manifest.sh`) so the two cannot drift.
- `get_param("state")` / `set_param("state")` keep every setting with the project.
- The host's tempo and beat position drive the clips. The DSP sends no MIDI. It reads files only when it
  starts (samples, clips, the library of kept clips) and writes one only when you record or clear a sample or keep or delete a clip, never on the audio thread: a recording is
  captured on the audio thread and turned into a sample, and saved, by the thread that sets parameters.

## Not yet verified on a Move

The screens, the Move's note numbers for the pads (36-67 and 68-99 are both handled), how the host delivers the
transport, and recording from the audio input have been tested headlessly against mocks and the engine's own output, and
the engine has run on the Move's CPU. Some of this has been played on a Move (the clips, the pads, the screens), but
recording, the clip generator (how it sounds), the hold-to-delete gesture, the dials on the Pads screen, the file browser from a blank
slot, the recipe library, and the release of the pad lights have not yet.
