RG-55 for logue
===============


A monophonic Sound Canvas SC-55 player based on libEmuSC for the KORG drumlogue and
microKORG2.


## Constraints and notes


Chorus and reverb have been turned off (disabled at compile time), otherwise
this unit not be usuable. It always uses libEmuSC part 0, in mono mode: drumlogue
`GateOn()` sends to that part and a new note stops the one sounding, with no release
tail under it. The microKORG2 unit sends the keyboard's notes to the same part and
behaves the same.

The note being stopped is faded out over one 64-frame block first, because
switching it off, otherwise it will distort: at 25 ms retriggers the cut reaches a
tenth of full scale. A gate holds the note until the gate ends.


## How to build

libEmuSC is the `libemusc` submodule, at commit
`f503c9be3368370920055472599af9f2df78f623`. The build scripts apply the unit's
patch for you; it enables the one-part build and the
`EMUSC_NO_REVERB`/`EMUSC_NO_CHORUS` switches, and fixes what GCC 6 and the
drumlogue's loader reject.

On the development machine, with a logue SDK checkout at
`~/Projects/logue/logue-sdk`:

```sh
git submodule update --init libemusc
./build-drumlogue.sh
./build-microkorg2.sh
```

This will result in the two units: `bin/RG55.drmlgunit` and `bin/RG55.mk2unit` .


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

This project is pulled out of my experiments for RFX (Regreoove Effects), hence
you will see this namespace used. It has been refactored in to a separate project
and licensed as MIT.

libEmuSC is licensed as LGPL-2.1+ and is built into the unit; the rest of the code
is MIT licensed.
