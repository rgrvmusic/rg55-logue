##############################################################################
# Project Configuration
#

PROJECT      := RG55
PROJECT_TYPE := synth

##############################################################################
# Sources
#

EMUSC_SRC   ?= /emusc/libemusc/src
SYNTH_PATH  ?= /rfx/synth
COMMON_PATH ?= /rfx/common

CSRC   = header.c
CXXSRC = unit.cc
CXXSRC += $(EMUSC_SRC)/chorus.cc
CXXSRC += $(EMUSC_SRC)/control_rom.cc
CXXSRC += $(EMUSC_SRC)/envelope.cc
CXXSRC += $(EMUSC_SRC)/note.cc
CXXSRC += $(EMUSC_SRC)/part.cc
CXXSRC += $(EMUSC_SRC)/partial.cc
CXXSRC += $(EMUSC_SRC)/pitch.cc
CXXSRC += $(EMUSC_SRC)/resampler.cc
CXXSRC += $(EMUSC_SRC)/reverb.cc
CXXSRC += $(EMUSC_SRC)/settings.cc
CXXSRC += $(EMUSC_SRC)/svf.cc
CXXSRC += $(EMUSC_SRC)/synth.cc
CXXSRC += $(EMUSC_SRC)/system_effects.cc
CXXSRC += $(EMUSC_SRC)/tva.cc
CXXSRC += $(EMUSC_SRC)/tvf.cc
CXXSRC += $(EMUSC_SRC)/wave_generator.cc
CXXSRC += $(EMUSC_SRC)/wave_oscillator.cc
CXXSRC += $(EMUSC_SRC)/wave_rom.cc

##############################################################################
# Include Paths
#

UINCDIR = $(EMUSC_SRC)
# For the shared unit_note_queue.h, which lives in synth/.
UINCDIR += $(SYNTH_PATH)
# For the shared rg55_log.h, which the microKorg2 variant uses too.
UINCDIR += $(COMMON_PATH)

##############################################################################
# Libraries
#
# drumlogued itself links libstdc++.so.6 (GLIBCXX_3.4.22, GCC 6.5) and
# libgcc_s.so.1, and both are in the device rootfs. A unit dlopen()ed by the
# daemon resolves those symbols against the copies already loaded in its
# process, so the toolchain's matching libstdc++ is linked normally.
ULIBDIR =
ULIBS   = -lm -lc -lstdc++

##############################################################################
# Compiler configuration
#

# libEmuSC needs C++17. The SDK's libstdc++ predates std::clamp, supplied by
# the unit-local compatibility header rather than changing the SDK.
CXXSTD = -std=gnu++17 -include emusc_compat.h

# The SDK defaults to -Os. libEmuSC renders its 8 ms of audio in one burst every
# sixth block, so what matters is how long that burst takes against the 1.33 ms
# a 64-frame block lasts, and -Os does not get it under.
OPTIM = -O3

##############################################################################
# Macros
#

UDEFS = -DUNIT_TARGET_PLATFORM_DRUMLOGUE -DEMUSC_MAX_PARTS=1
UDEFS += -DRFX_USERFS_PROGRAMS_PATH=/var/lib/drumlogued/userfs/Programs
# The drumlogue has reverb and delay of its own, and EmuSC's reverb network is
# the most expensive thing in the system effects: the panel's block is 1.33 ms
# and the engine has to render its 8 ms of audio inside one of them.
UDEFS += -DEMUSC_NO_REVERB
# Same for the chorus: the SC-55's system effects are eight chorus types and
# eight reverbs feeding the same bus, and the drumlogue's own chorus is on the
# other side of this unit.
UDEFS += -DEMUSC_NO_CHORUS
# Uncomment to write rg55.log in userfs: init, then a summary of the render
# (slowest block, late blocks, peak) when the unit is suspended or torn down.
# Nothing is logged while audio runs, so a build with this on sounds the same.
UDEFS += -DRG55_DEBUG
