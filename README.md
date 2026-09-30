# Fader FX

Performance effects for MPC OS / Akai Force (VST2 insert), Octatrack-scene style: one **FADER**
morphs every enabled effect from untouched (left) to its setting (right); pull it back and you are
at the origin again. Built on [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).

| Effect | What the fader does |
|---|---|
| Filter | bipolar DJ filter: LP/HP knob < 0 low-pass, > 0 high-pass, sweeps with the fader; Resonance |
| Repeat | beat roll of the last slice, 1/4 getting shorter as you push, down to *Repeat Max* |
| Crush | bit depth and sample rate down |
| Gater | tempo-synced chop (*Gate Rate*), depth |
| Riser | band-passed noise sweeping up, louder the further you push |
| Drop | tape-stop style slow-down, darker as it slows |
| Delay | throw: the send follows the fader, the echoes keep going after it's back (*Delay Time*, *Feedback*) |
| Reverb | send follows the fader, tail keeps ringing; *Freeze* holds it while the fader is fully right |

Tempo follows the MPC. Put it on the master or a track as an insert; assign the FADER to a Q-Link
(it is Q-Link 1 on the Perform page) or use the touchscreen fader.

Build: Actions -> "VST release (draft)". Offline test: `vst/test.sh`. License: MIT.
The skin font (Titillium Web, `vst/fonts/`) is under the SIL Open Font License.
