RG-55 for logue
===============


A Sound Canvas SC-55 player based on libEmuSC for the KORG drumlogue and
microKORG2.


## Constraints and notes


Chorus and reverb are disabled at compile time (`EMUSC_NO_REVERB`,
`EMUSC_NO_CHORUS`), otherwise this unit is not usable.

The drumlogue unit uses libEmuSC part 0 and runs it in mono mode: `GateOn()`
sends to it, a new note stops the one sounding, and a gate holds its note until
the gate ends. The note being stopped is faded out over one 64-frame block
first, because switching it off otherwise distorts: at 25 ms retriggers the cut
reaches a tenth of full scale.

The microKORG2 unit builds one `EmuSC::Synth` with one part per host voice, on
MIDI channels 0..7, and writes one stereo pair at `bufferOffset`. A fresh
`trigger` bit starts that voice's part from `pitch[]`; the note callbacks carry
key and velocity, and a note-off stops that voice's part. A voice handed a new
key stops its old note first. The engine's note limit follows the host's voice
count, so released tails are taken back instead of accumulating. The `trigger`
bit is not a gate and can fall while a key is held, so only a note-off releases
a voice. A key pressed and released between two blocks starts nothing.


## How to build

libEmuSC is the `libemusc` submodule, at commit
`f503c9be3368370920055472599af9f2df78f623`. The build scripts apply the unit's
patch for you. It adds the `EMUSC_MAX_PARTS`, `EMUSC_NO_REVERB` and
`EMUSC_NO_CHORUS` guards and the sub-block refill, and fixes what GCC 6 and the
drumlogue's loader reject. The defines come from each unit's `config.mk`:
`EMUSC_MAX_PARTS=1` for the drumlogue, `8` for the microKORG2.

On the development machine, with a logue SDK checkout at
`~/Projects/logue/logue-sdk`:

```sh
git submodule update --init libemusc
./build-drumlogue.sh
./build-microkorg2.sh
```

This will result in the two units: `bin/RG55.drmlgunit` and `bin/RG55.mk2unit`.


## How to use


Place `RG55.drmlgunit` in `/Units/Synths` of the userfs mount. Place
`RG55.mk2unit` in a `SlotN` folder under `Units/Oscs` of the userfs mount.

Before loading the unit, copy the complete user-provided SC-55L set to
`/Programs`:

```
./sc55_rom1.bin      (32 KB CPU ROM)
./sc55_rom2.bin      (256 KB control/program ROM)
./sc55_waverom1.bin  (1 MB PCM ROM)
./sc55_waverom2.bin  (1 MB PCM ROM)
./sc55_waverom3.bin  (1 MB PCM ROM)
```

> [!NOTE]
> All three PCM ROMs are required. The ROMs are neither included nor distributed
> by this project.


## Disclaimer

This project is pulled out of my experiments for RFX (Regroove Effects), hence
you will see this namespace used. It has been refactored in to a separate project
and licensed as MIT.

libEmuSC is licensed as LGPL-2.1+ and is built into the unit; the rest of the code
is MIT licensed.
