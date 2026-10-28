# TinyScratch release notes

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
