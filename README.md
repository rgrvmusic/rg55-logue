# RGSC55 drumlogue unit


RGSC55 is a monophonic Sound Canvas SC-55 synth based on libEmuSC.



# Constraints and notes


Chorus and reverb have been turned off (disabled at compile time), otherwise
this unit not be usuable. It always uses libEmuSC part 0, in mono mode: drumlogue
`GateOn()` sends to that part and a new note stops the one sounding, with no release
tail under it.

The note being stopped is faded out over one 64-frame block first, because
switching it off, otherwise it will distort: at 25 ms retriggers the cut reaches a
tenth of full scale. A gate holds the note until the gate ends.


## How to build

On the development machine:

```
mkdir -p ~/Projects/various/
cd ~/Projects/various
git clone https://github.com/skjelten/emusc.git
```

This must be the libEmuSC repository at commit
`f503c9be3368370920055472599af9f2df78f623`.

```sh
git checkout f503c9
```

Apply the unit's patch: it enables the one-part build and the
`EMUSC_NO_REVERB`/`EMUSC_NO_CHORUS` switches, and fixes what GCC 6 and the
drumlogue's loader reject:

```sh
cd ~/Projects/various/emusc
patch -p1 < libemusc-drumlogue.patch
```


# How to use


Place `RG55.drmlgunit` in `/Units/Synths` of the userfs mount.

On the drumlogue, copy the complete user-provided SC-55L set to
`/Programs` before loading the unit:

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


# Disclaimer

This project is pulled out of my experiments for RFX (Regreoove Effects), hence
you will see this namespace used. It has been refactored in to a separate project
and licensed as MIT.

