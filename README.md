# Fader FX

Performance effects for MPC OS / Akai Force (VST2 insert), Octatrack-scene style: one **FADER**
morphs every enabled effect from untouched (left) to its setting (right); pull it back and you are
at the origin again. Built on [mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins).

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

## Steuerung per MIDI-CC (seit 1.1.0)

Fader FX hat einen eigenen MIDI-Port **Control In** (in den MIDI-Listen „Fader FX Control In“). Er verbindet
sich selbst mit allen Hardware-MIDI-Eingängen (USB-Controller wie Faderfox EC4, DIN-In), auch mit später
eingesteckten. Eine MIDI-Spur kann ihn als Output-Port wählen, CC Map erreicht ihn mit **LINK A**.
Ausgewertet werden nur CCs auf **CONTROL CH** (Tab SETUP, OFF / 1–16, Voreinstellung **11**; wird mit dem
Projekt gespeichert). Schalter sind ab 64 an; Auswahllisten springen auf die nächste Option.

| CC | Parameter | CC | Parameter | CC | Parameter |
|---|---|---|---|---|---|
| 20 | **Fader** | 29 | Filter an/aus | 40 | Repeat Max |
| 21 | Filter LP/HP | 30 | Repeat an/aus | 41 | Gate Rate |
| 22 | Resonance | 31 | Crush an/aus | 42 | Delay Time |
| 23 | Crush Amt | 32 | Gater an/aus | 43 | Feedback |
| 24 | Gate Depth | 33 | Riser an/aus | 44 | Size |
| 25 | Riser Level | 34 | Drop an/aus | 45 | Freeze |
| 26 | Drop Depth | 35 | Delay an/aus | | |
| 27 | Delay Send | 36 | Reverb an/aus | | |
| 28 | Reverb Send | | | | |

CC 20–35 liegen in der Reihenfolge der Q-Links der Seite PERFORM. Ob der Bildschirm der Force den per CC
gesetzten Wert mitzeichnet, hängt vom Wrapper ab (`mpc_engine_changed`); der Klang folgt in jedem Fall.

Build: Actions -> "VST release (draft)". Offline test: `vst/test.sh`. License: MIT.
The skin font (Titillium Web, `vst/fonts/`) is under the SIL Open Font License.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
