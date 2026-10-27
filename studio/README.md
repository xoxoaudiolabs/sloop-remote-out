# SLOOP Remote Out: the Studio side

[SLOOP](https://github.com/isod89/sloop-fm1), the groovebox firmware for the M-VAVE FM-1, running natively on the
FM-1 Companion Studio (a Raspberry Pi 3 B+ with a 3.5" 480×320 touchscreen), played from a real FM-1.

It's SLOOP's own code: its sound engines, sequencer and screens, compiled for the Pi through the harness SLOOP's
tests use (`tests/ui_pages_test.c`). Nothing is emulated.

**Status:** works with the FM-1 Companion's Studio v1.4 or later; the shared clock with X0X (below) needs the
Companion from its main branch after Studio v1.5. See *Tested* below for what has been tried on the real Studio.

## How it works

- **Screen:** SLOOP's 240×240 picture at 4/3 size (320×320) in the middle of the Studio's screen. The Companion
  draws the margins: its **< HOME** corner (top left) goes back to the Companion with SLOOP still running behind it
  (playing, if it was), and **PLAY SLOOP** (MORE, it says ON or OFF) shows it again as it was. **HOLD OFF** (bottom
  left, held a second) saves its project and stops it. The right margin has SLOOP's level (+ / −), also on the Companion's MIXER.
- **Sound:** 44.1 kHz stereo into PipeWire, so it plays wherever the Studio plays (the Bluetooth speaker or the jack).
- **Control:** a real FM-1 running this repo's SLOOP (SLOOP with a REMOTE OUT: [the repo's README](../README.md)),
  with GLO › MIDI 3 › REMOTE set to **OUT 16**. Its 14 buttons (CC 102–115), 7 knobs (CC 116–122, relative) and 27 keys
  (notes 36–62) go out on channel 16; the Companion passes FM-1 A's MIDI to this program through a FIFO.
- **Saved:** everything SLOOP keeps in an FM-1's flash, kept the way it keeps it: SLOOP's own saving code
  (`storage.c`, `project.c`, `upreset.c`) on `~/.local/share/sloop-remote-out/flash.bin`, a 1 MiB image of the FM-1's
  flash with the same layout. Its settings (SYNC and CLK OUT among them), the working project, the four project
  slots, the song's sections, user presets, the FM6 patches and the four sample slots. Changed parts are written to
  the file about a second after SLOOP writes them, and all of it when it stops. The first time, the working project
  and the SYNC / CLK OUT that earlier versions kept (`project.bin`, `settings.bin`) are taken over.
- **Backups:** SLOOP's editor protocol (`editor.c`, what SLOOP's web editor and the FM-1 Companion's library speak)
  answers through the MIDI FIFO, replies going to a second one (`--sysex-out`). The Companion's backups, restores,
  projects and samples for an FM-1 work on it unchanged (Settings › Sloop › For: SLOOP Studio), with the same files.
  `sloop_harness.c` builds it: the harness SLOOP's tests use, with SLOOP's real saving in place of its stand-ins.
- **Clock, with another app on the Studio** (such as
  [X0X for the FM-1 Companion Studio](https://github.com/xoxoaudiolabs/fm1-x0x-companion-studio)): the Companion
  runs both at once and connects each one's MIDI clock to the other, as two FM-1s cabled both ways. On the Studio,
  SLOOP's CLK OUT starts **ON**, so SLOOP leads: its PLAY starts both from the top, its tempo (GLO + SELECT) is
  both apps' tempo, its STOP stops both. To have the other app lead, set SYNC to **USB**. An app follows the
  other's clock only while the other plays, so one playing on its own isn't pulled off its beat when the other
  opens.

## Build (on the Studio)

It's built from this repo's SLOOP, one folder up: the same sources the FM-1 runs, so both SLOOPs match.

```sh
git clone https://github.com/xoxoaudiolabs/sloop-remote-out
cd sloop-remote-out/studio
./build.sh                          # needs gcc and python3 with Pillow; about 2 minutes on a Pi 3 B+
mkdir -p ~/.local/share/sloop-remote-out && cp sloop-remote-out ~/.local/share/sloop-remote-out/
```

The [FM-1 Companion](https://github.com/xoxoaudiolabs/FM-1-Companion) (Studio v1.4 or later) shows a
**PLAY SLOOP** tile in MORE once `~/.local/share/sloop-remote-out/sloop-remote-out` exists. On the FM-1: GLO › MIDI 3 ›
REMOTE to **OUT 16**.

## Tested

**Hardware used:** the FM-1 Companion Studio (a Raspberry Pi 3 B+ on Raspberry Pi OS Lite, Trixie, with the Hosyond
3.5" touchscreen), FM-1 A running SLOOP Remote Out (SLOOP 2.8.1 with REMOTE OUT, REMOTE on OUT 16), a JBL Go 3 over
Bluetooth and headphones on the Studio's 3.5 mm jack.

First on the real Studio (2026-10-25) with the panel messages sent by a script:

- SLOOP's TRACKS screen drawn on the Studio's screen.
- PLAY over MIDI starts the sequencer.
- Its sound reaches PipeWire as a node named "SLOOP", using about 9% of one core.
- SLOOP's engine benchmark (`tests/hostsim.c`): every engine with 8 notes in 3.5–8% of one core.

Then from a real FM-1 A running sloop-remote with REMOTE = OUT 16 (2026-10-25): its keys, buttons (GLO, HOME)
and knobs (SELECT, KNOB 1–3) arrive on channel 16, and it drives SLOOP on the Studio (opened from the Companion's
PLAY SLOOP tile; FX pages reached from FM-1 A's panel).

Saving: on a PC (WSL), a change saved, and after a restart loaded back identical (the file not rewritten). On the
Studio (2026-10-25): < HOME hides it and PLAY SLOOP brings it back as it was, and the working project is written to
`project.bin` after a change.

Margins on the Studio (2026-10-25): < HOME, HOLD OFF and the + / − (its level saved and set on its PipeWire
stream). Turned on again from PLAY SLOOP after a Companion restart, it came back with its saved project (a changed
track 1 sound kept).

Clock with X0X (2026-10-25):
- By hand, from FM-1 A through the Companion's PLAY SLOOP and PLAY X0X tiles: both running at once, SLOOP leading
  (the default), playing together in time by ear. A stutter in X0X when SLOOP opened was found this way and fixed
  (X0X caught SLOOP's clock mid-bar); by hand afterwards, it was gone.
- By a script on the Studio, each app's sound recorded: X0X following SLOOP at 120 and 130 BPM, through STOP and
  PLAY, with no drift (each bar the same length in both); SLOOP following X0X the same way, with SYNC on USB. Two
  X0Xs, one following the other, in one recording: the follower 9–13 ms after the leader on average, about 8–9 ms
  of jitter. X0X kept a steady beat while SLOOP was opened and closed next to it.

Saving and backups (2026-10-26), by script on the Studio in test sessions, not yet by hand:
- SLOOP's editor protocol through the FIFOs: INFO (SLOOP 2.8.1, protocol 12), a whole backup with the Companion's
  `sloop.py`.
- A real FM-1 A backup restored with the Companion's own restore job; a backup right after it matched, all 16
  parts (projects, settings, drum kits, MIDI map, theme, the four sample slots).
- A WAV loaded into USR2 with the Companion's load-sample job, seen in the next backup.
- A project loaded into slot B, still there after SLOOP was stopped and started again (from `flash.bin`).
- The working project of an earlier version (`project.bin`) taken over with the same screen, then loaded from
  `flash.bin` alone.

**Not tested yet:**
- Any of the saving and backups by hand: SLOOP's own SAVE and LOAD of its project slots on its screen, the
  Companion's rows on the Studio's screen.
- A sample loaded into a slot heard playing.
- A backup of the Studio's SLOOP restored onto a real FM-1.
- SLOOP stopped in the middle of a backup or restore.
- The MIXER tab's SLOOP slider; the project loading back after a reboot of the Studio.
- SLOOP following X0X (SYNC on USB) by hand: only by the script.
- Turning off the leading app (HOLD OFF) while both play: the other should carry on at its own tempo, after a
  stop of about half a second.

## License

GPL-3.0-only ([LICENSE](../LICENSE)), as SLOOP: this program is built from SLOOP's sources, by isod89
([isod89/sloop-fm1](https://github.com/isod89/sloop-fm1)), after Felucca by Leo Kuroshita / Hügelton Instruments.

**What's ours in this fork:** `studio/` (this program), and SLOOP's REMOTE OUT (see [the repo's README](../README.md)).
Everything else is SLOOP's, unchanged.

**Built and run with** (on the Studio, from Raspberry Pi OS's packages; not copied into this repository, each
under its own licence):

| Software | What for | Licence |
|---|---|---|
| GCC | building it | GPL-3.0-or-later (what it builds isn't bound by it: the GCC Runtime Library Exception) |
| the GNU C library (glibc), libm | linked into it | LGPL-2.1-or-later |
| Python 3, with Pillow | SLOOP's build scripts that make its generated headers (fonts, icons, tables, samples) | PSF License; the PIL Software License (MIT-CMU) |
| PipeWire (`pw-cat`) | its sound | MIT |
| the Linux kernel (the framebuffer) | its screen | GPL-2.0 |

The licences are each project's own, as Debian lists them in `/usr/share/doc/<package>/copyright`: that file is
the one to go by.
