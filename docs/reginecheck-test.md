# About the ReGINE test


Build the unit here first, so `bin/` holds the file being tested:

```sh
./build-drumlogue.sh
```

Then, on the board (or under `qemu-arm -L <armhf sysroot>`), with `<dlhost>` being ReGINE's
`dlhost`:

```sh
# 1. the panel, as the device reads it: every row, as the unit declares it
<dlhost> --unit bin/RG55.drmlgunit --info

# 2. the sound: a note, two seconds, a WAV, peak and rms
<dlhost> --unit bin/RG55.drmlgunit --note 60 --seconds 2 --out out/rg55.wav
```

`--info` loads the unit, initialises it, prints its header and stops - no note, no render, no
output. What it prints is the `name` the device will show, the ids, the module, how many presets,
then every row with the `min`, `max`, `init` and `center` the unit declared, and the type name the
device will use to draw it. That is the panel, read out of the shipped file rather than out of
`header.c`: the file the device loads, answering for itself.

The `unit_init` reads the five ROM files from the userfs path it was built with,
`/var/lib/drumlogued/userfs/Programs` - the board's own userfs on a board, and inside the sysroot
under `qemu-arm`. They have to be there before anything can render; the check names the ones that
are missing and exits rather than reporting silence as a result.

Every run that renders prints a read-backs list as well, after initialising and before the
first block: for each row whose text the unit itself supplies, the value the unit holds and the
text it supplies for it. That is the control ROM's own instrument name for `PROGRAM`, and the
unit's own formatting for the rest, read through the device's own call - and it is after any
`--param`/`--raw` has been applied, so a read-back is also how a moved row is confirmed rather than
assumed.

A render's report ends in `rendered` (frames, blocks, rate, channels), `peak` (per channel) and
`rms`. For a synth that was sent a note, `peak` at 0.0 is silence and a failure, not a quiet
sound; `peak` reading `nan` means the unit's arithmetic went non-finite and says how many samples
did. A peak above 1.0 is the unit's own output past the ±1.0 the device takes, which the host check
treats as a failure on the host and which is worth the same suspicion here. The exit codes say all
of it without reading the report:

| Exit | What it means |
| --- | --- |
| 0 | loaded, initialised, rendered, finite |
| 1 | usage error |
| 2 | the unit could not be loaded (not a unit, wrong architecture, refused by the licence) |
| 3 | `unit_init` refused the descriptor (the `k_unit_err_*` name is printed) |
| 4 | the WAV could not be written |
| 5 | the output contained NaN or infinity |


### The whole sweep: `tools/reginecheck.sh`

The two commands above are how a single question is asked. Everything they show for this unit,
checked against its own `header.c` and `config.mk`, is [`tools/reginecheck.sh`](../tools/reginecheck.sh):

```sh
tools/reginecheck.sh                      # with dlhost found on PATH
tools/reginecheck.sh --seconds 3          # longer renders
```

`DLHOST=` names the binary outright when it is not on `PATH`. It needs no other argument. `SYSROOT=`
(or `QEMU=`/`SYSROOT=` together) is how an armhf `dlhost` is run here under `qemu-arm`; both are
dropped when the binary runs here anyway, which is what a 32-bit ARM board does.

What it checks, and where each one is in the report:

| Check | What it reads |
| --- | --- |
| `loads` | `--info` exits 0. Exit 2 is a file this runtime will not load |
| `name`, `ids`, `module`, `presets`, `rows` | the header, against the unit's own `header.c` and `config.mk` |
| `panel` | every row's label, type and the numbers it is declared with, against `header.c` |
| `render` | a note at the unit's declared `NOTE` (60): not silent, finite, inside ±1.0 |
| `wav` | the file it wrote is the length its own report says |
| `gate` | `--gate` renders the same note as `--note`, to the sample |
| `program` | `PROGRAM` 0 and 1 render differently |
| `level` | `LEVEL` 0 is silence, and 100 is louder than the default 80 |

Peak and rms are too blunt for the pairs - two different sounds can agree on both - so the two
renders are compared **as files**, byte for byte. A row that is on the panel and wired to nothing
comes back byte-identical, and that is a failure.

A Developer Version build of ReGINE loads only its licensee's units and refuses the rest at load,
before `unit_init`. That refusal is reported as a skip with the line `dlhost` printed, because it is
the licence working and not the unit failing, and nothing below it can be seen through a host that
will not load the file. The unit declares `dev_id 0x72677276`, which is Regroove | RGRV's own.

Every render is left in `out/dlhost/`, with the report `dlhost` printed for it: that is the
evidence, and `out/dlhost/RG55.wav` is the one to listen to.


### What to look for in these three rows

`--info` says how each row is **declared**, the read-backs of a render say what the unit
**answers** for it, and the pair is where the unit's own row rule shows. Every row is declared from
zero up, because a row whose text the unit formats is held by the runtime in a byte, so a declared
negative is one the device cannot keep.

| Row | Type | Declared | Answers |
| --- | --- | --- | --- |
| `PROGRAM` | strings | `0..127`, init 0 | the control ROM's instrument name (`Piano 1` at 0) |
| `LEVEL` | percent | `0..100`, init 80 | `0%` at the bottom, `100%` at the top |
| `NOTE` | midi_note | `0..127`, init 60 | the note a gate plays |

`PROGRAM` is the reason the strings type is used: what the display should show is the instrument's
name, not its number, and it comes from the ROM rather than from a table in the unit. No row answers
`(null)`.

`--note 60` is middle C and plays the default `Piano 1`. On the sequencer's gate the same note
comes from `NOTE`, which is why the `gate` check compares the two files rather than their peaks.


### Moving a row

Two grammars:

```sh
<dlhost> --unit bin/RG55.drmlgunit --note 60 --seconds 2 --param "LEVEL=1023"
<dlhost> --unit bin/RG55.drmlgunit --note 60 --seconds 2 --raw 1=0
```

- `--param <id>=<v>` takes a knob position, `0..1023` (the device's own resolution), scaled into
  the row's declared range.
- `--raw <id>=<v>` writes the declared value through unscaled: `--raw 1=0` is `LEVEL` at the bottom,
  `--raw 1=100` at the top, and `--raw 0=1` is the second instrument.

`<id>` is an index or a name from the unit's own header. Here the three labels are unique, so either
works; with a panel that carries one label twice, prefer the index.

A row that is on the panel but wired to nothing renders identically at both ends, which is what the
`program` and `level` pairs catch and a single render cannot:

```sh
# the level has to be audible: silence at 0, and louder at 100 than at the default 80
<dlhost> --unit bin/RG55.drmlgunit --note 60 --seconds 2 --raw 1=0
<dlhost> --unit bin/RG55.drmlgunit --note 60 --seconds 2 --raw 1=100
```

Comparing the two WAVs byte for byte is the check; peak and rms are too blunt, since two different
sounds can agree on both. A pair that comes back identical is a row to verify.

