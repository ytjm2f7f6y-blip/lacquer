Lacquer: install
================

0. Quit Logic. If an older "lacquer" disk is still open, eject it
   (Finder sidebar, eject button) so only this one is open.
1. Open Terminal (press Cmd+Space, type Terminal, press Return).
2. Paste this line and press Return:

   bash "$(ls -td /Volumes/lacquer*/ | head -1)install.sh"

3. Open Logic. Put Lacquer last on the Stereo Out:
   Audio FX > Audio Units > Rumpus Room Records > Lacquer


Using it
--------
The first time: play the song through once. Lacquer learns how loud
it is and from then on lands it on TARGET (under LOUDNESS).

The assistant:
  LISTEN, then play the song through once and stop.
  MOVES (screen key) lists its suggestions: click one to take or skip
  it, or press TAKE ALL. AMOUNT scales the moves you have taken.
  BALANCE shows the song against the target; LOUDNESS shows where
  more loudness starts to cost the drums; CHECKS is the preflight.
  TARGET: GENRE (click the style's name on the BALANCE screen to
  change it), REFS (drop audio files on the screen), MINE (SET >
  remember this master's balance).

Sections:
  Press MARK, play the song, and tap 1 to 5 as each part starts.
  Press MARK again to finish. With the song stopped, press a number:
  knobs you turn now change only that section. SONG goes back to the
  whole song. Double-click a knob to make the section follow the song
  again. On the SECTIONS screen, drag a block's left edge to move a
  mark; right-click a block to change or remove it. Click the tape
  over the number keys to name the sections.

The SPACE tray: give the latch at the bottom left a quarter turn.

Knobs: drag up or right, Shift for fine, double-click to reset.
Click a selector switch to step it round (Alt-click steps back).
SET (bottom rail): window size, save a preset, name the sections.
Click the INTEGRATED readout to start its count again.

Point at any control and the strip under the faceplate says what it
does.

Not working yet in this build: CODEC (the switch moves but does
nothing).
