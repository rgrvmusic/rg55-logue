# Running the unit on ARM with ReGINE


The step between the host checks and the device. `tools/hostcheck-drumlogue.sh` builds the unit's
sources natively, on whatever machine you are on, and that is how the panel and the sound are
checked. But it cannot run the shipped file, because a `.drmlgunit` is a 32-bit ARM shared object
and a desktop is not. ReGINE is what runs the file itself, on 32-bit ARM, off the device.

[ReGINE](https://rgrv.nl/regine/) is Regroove | RGRV's own logue runtime (it was called NTHost
before it grew past the NTS-1), and it is not part of this repository. Developers of Logue units can
obtain a license, or a user can purchase a hosting runtime. Its drumlogue host is `dlhost`
This loads the unit, calls `unit_init`, render blocks, write a WAV, report what came out.
`dlsynthd` is the same backend with MIDI in and audio out instead of a WAV, for playing a unit by
hand rather than measuring it.

Nothing here is emulation of the unit: the unit's own ARM code runs on the ARM core, off the
device. What ReGINE stands in for is the machine around it (the host, the clock, the panel's own
writes, the note events), which is exactly the part a unit is not.


## Why it has to be 32-bit ARM

A `.drmlgunit` is an armhf ELF shared object, so the host that loads it has to be an armhf build:
`dlhost` exists in the 32-bit ARM build of ReGINE and in no other.


## The developer's licence, in brief

Developers of Logue units that are registered with a `dev_id` can apply for developers license
which grants them a non-transferable right to use the hosting runtime for development purposes,
such as CI/CD. For more information, please use our [contact form](https://rgrv.nl/contact).

