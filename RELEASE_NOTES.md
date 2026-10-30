# TinyScratch release notes

## v1.5.0

- **Three ways to Improv**, at the top of the Style list: **Flow** (the default) moves like a real DJ's hand; **Snap** is Improv exactly
  as 1.1.0 made it, punchier; **Freestyle** is Flow with longer, looser sounds, as real DJs play on their own. Old projects' Improv opens as Flow.
- **Stutter**, a new style: a word's first sound pushed again and again in time, then let go ("f-f-f-fight"). Best on speech.
- **Long samples**: each bar starts on one of the sample's words or hits, with strokes as long as on a short sample. Record up to 10 s.
- **Pick spots from a song** (Remote UI): drop a song, mark up to six spots, preview them, then send them to User slots.
- **Vinyl** (Playback, dial 8; off by default): slow moves are quieter, with the groove's faint hiss and crackle.
- **Fix:** the Move's screen named styles after Tear as Tear.

Kept clips and the shipped clips and samples sound exactly as before.

## v1.1.0

- **Remote UI.** TinyScratch now has a page in Schwung's Remote UI (`http://move.local:7700/remote-ui`): an overhead turntable whose record
  turns backward and forward with the scratching, its white label hand-written with the sample's name and the set's; high-resolution
  versions of the Move's graphs (the waveform, the clip's lanes and the zoom); the pads and Pad Play; and the Pre-gen and Playback dials,
  grouped and ordered as on the Move. The page runs the clip itself between the host's updates, so the record and graphs move in time.
- **Scratch by hand.** Drag the record to scratch it; the fader opens while you hold it. The pads are on the keyboard too (Q to K), and each
  pad shows a picture of its clip.
- **Drop a sound on the deck** (or LOAD SOUND): it is converted to a WAV and saved to a User slot.
- **Stroke names.** The clip graph can name each stroke (baby, chirp, transformer...); STROKES turns them off.
- **Wide and PERFORM layouts.** In a pop-out window on a wide screen the page fills it: pads, deck and graphs side by side. PERFORM shows
  just the deck, the pads and the zoom.
- **Fix:** a tap on an empty User pad on the Pads screen recorded a tenth of a second and kept it, so deleted User slots filled up again.
  Recording now needs a hold, and a recording of silence is not kept.

## v1.0.2

- **On-device help.** TinyScratch now ships a `help.json`, so its help is in Schwung's Help viewer (Global Settings, System, Help) and one jog from
  its controls (the Module Help row): getting started, the pads, every dial, the styles, Groove, clip pages, samples and the Taste Log.

## v1.0.1

- A clip made with Groove now says **which groove** it kept to on the bottom line, after its percentage: **GRV60 FUNK** (BOOM, FLOOR, BRKS, HALF,
  FUNK, DEMBO or OPEN). It is shown for clips made with Groove in 1.0.0 and later; clips from before Groove, or whose saved data an older
  generator made, show no name.

## v1.0.0

- **Groove.** A new dial that makes **Improv** keep to a drum groove, the way a scratch DJ plays over a beat. It is dial 8 on the Main page,
  and dial 5 on the Pads screen (in the place of Swing, which is on the Playback page), so you can tweak it while you generate. At 0%,
  the default, Improv is exactly what it was. Turned up, each clip keeps to one of seven drum patterns (Boom-bap, Four on the floor, Breaks,
  Half-time, Funk, Dembow, Open): strokes land on the snare or a step ahead of a hit, the fader leaves the kicks clear and rests in the gaps,
  the bars breathe, build or drop, and each phrase ends in a fill. **Groove leaves room for a beat**: on its own a clip can sound thin,
  over a song it sits in the gaps. It does nothing to the named styles.
- A clip made with Groove above 0 shows **GRV60** (its percentage) on the bottom line when it plays.
- **Taste Log.** It now also records each clip's Groove and which groove it kept to (the last two columns). It stays on your Move and is
  never sent anywhere. Libraries and logs from earlier versions still read as they did (the library line gains a tenth number, the Groove).

## v0.9.5

- **Pages of clips.** Each sample now has **four pages of 14 clip pads**. Press **>** and **<** (the Move's right and left arrow buttons)
  to turn the page; only the 14 clip pads change, everything else stays as it is. A clip keeps playing on its own page while you look at
  another (its page mark, top right, blinks), a tap plays the clip of the page you are on, tapping an empty pad keeps the generated clip
  on the page you are viewing, and **Auto** plays the clips of every page. Each page has its own pad colour, and a clip on another page
  than the first is labelled with it (CLIP 2.3). The clips you have already kept stay on the first page exactly as they were.
- The Taste Log (on by default, local to your Move, never sent anywhere) carries on as in 0.9.3.

## v0.9.3

- **Taste Log is now on by default.** It is the groundwork for a future feature (a generator that learns what you like), and it needs
  some history to learn from, so it starts logging from the first clip you keep, pass over or delete. Nothing uses it yet. **It never
  leaves your Move: TinyScratch does not send it or ask for it.** It holds no audio, sample name or clip data, it is kept to about 800 kB,
  and you can switch it off (Pad Play page) or delete `taste_log.csv` at any time.

## v0.9.2

- **Taste Log** (Pad Play page), groundwork for a future feature: a generator that learns what you like. **It is switched off by default
  and nothing uses it yet**; this release only adds the log. Turned on, TinyScratch appends a line to `taste_log.csv` in the module's
  folder each time you keep a clip, pass one over (press Generate again) or delete one you kept: its style, length and settings, the seed,
  how long it played, and 14 numbers measured from the clip. No audio, sample name or clip data. It stays on your Move: nothing reads it
  or sends it anywhere, and it is kept to about 800 kB.

## v0.9.1

- **Bug fix.** Tapping a clip pad on the Pads screen could also reach the Move itself, and if the Move was sitting in its Set overview
  or Session view it opened another Set or launched a clip. The Pads screen now keeps its pads to itself while it is up.
- **Pads screen.** The clip playing now shows its style next to its number (CLIP 3 BABY), and, bottom right, which of Echo, Forwards
  and Finish are really in it (nothing if none). Humanise now starts at 35% and Bite at 10%.
- **Licence.** TinyScratch's code is now released under the MIT licence (`LICENSE`, also in the package).

## v0.9.0

First public release. TinyScratch is a generative auto-scratcher for the Ableton Move (a Schwung sound generator): press a pad and it
writes a new, human-sounding scratch for your sample, plays it in time with your song, and lets you keep the ones you like.

- **The generator.** **Improv** is an improvised two-bar phrase tuned to measurements of real scratching; twelve named styles follow
  the Turntablist Transcription Methodology (Baby, Cuts, Military, Scribble, Stab, Chirp, Transform, Flare, Crab, Orbit, Drag, Tear).
  Echo, Forwards and Finish add echo scratches, word-length pushes and a play-out to the end of the sample. Above 120 bpm strokes keep
  the length in seconds they have at 120, so a fast song is not busier than a hand can be.
- **Pads screen.** Generate, keep (tap an empty clip pad), delete (hold a clip pad), eight samples (hold an empty User pad to
  record, up to four seconds), and eight red pads that cut the clip, or, with no clip playing, drop the needle at the sample's attacks.
  All eight dials are in use: Style, Length, Drag, Humanise, Swing, Echo, Forwards, Finish.
- **Auto** plays each clip right through, from its start, then another.
- **Playback.** Swing, Humanise (with the played path drawn against the written one), Bite (soft saturation with a brightness
  boost), Clip Depth and Offset, Drag and Gain. The fader is a fixed steep 1 ms and playback is anti-aliased.
- **Three clips each come with Ahhh and Fresssh**, kept exactly. Any clip can be deleted. A saved clip never changes.

Install `TinyScratch.tar.gz` with Schwung's module manager (Install Custom Module, From GitHub URL, `fleshinachair/TinyScratch`).
The look and workflow are inspired by Andrew Norris's Live Scratcher; none of its code or clips is used.
