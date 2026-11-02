# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

##############################################################################
# Project Configuration
#

PROJECT      := RG55
PROJECT_TYPE := osc

##############################################################################
# Sources
#

EMUSC_SRC   ?= /emusc/libemusc/src
COMMON_PATH ?= /rg55/common

CSRC   = header.c
CXXSRC = unit.cc
CXXSRC += $(EMUSC_SRC)/control_rom.cc
CXXSRC += $(EMUSC_SRC)/envelope.cc
CXXSRC += $(EMUSC_SRC)/note.cc
CXXSRC += $(EMUSC_SRC)/part.cc
CXXSRC += $(EMUSC_SRC)/partial.cc
CXXSRC += $(EMUSC_SRC)/pitch.cc
CXXSRC += $(EMUSC_SRC)/resampler.cc
CXXSRC += $(EMUSC_SRC)/settings.cc
CXXSRC += $(EMUSC_SRC)/svf.cc
CXXSRC += $(EMUSC_SRC)/synth.cc
CXXSRC += $(EMUSC_SRC)/system_effects.cc
CXXSRC += $(EMUSC_SRC)/tva.cc
CXXSRC += $(EMUSC_SRC)/tvf.cc
CXXSRC += $(EMUSC_SRC)/wave_generator.cc
CXXSRC += $(EMUSC_SRC)/wave_oscillator.cc
CXXSRC += $(EMUSC_SRC)/wave_rom.cc
# chorus.cc and reverb.cc are not listed: EMUSC_NO_CHORUS and EMUSC_NO_REVERB
# below take both networks out, and the patched system_effects.cc no longer
# refers to their symbols.

##############################################################################
# Include Paths
#

UINCDIR = $(EMUSC_SRC)
# For rg55_log.h and unit_note_queue.h, which the drumlogue unit uses too.
UINCDIR += $(COMMON_PATH)

##############################################################################
# Libraries
#

ULIBDIR =
ULIBS   = -lm -lc

##############################################################################
# Compiler configuration
#

# libEmuSC needs C++17. The SDK's libstdc++ predates std::clamp, supplied by
# the unit-local compatibility header rather than changing the SDK.
CXXSTD = -std=gnu++17 -include emusc_compat.h

# The SDK defaults to -Os. libEmuSC renders a 256-sample control block in
# sub-blocks of 16, and each 64-frame host block draws two or three of them, so
# what matters is how long a sub-block takes against the 1.33 ms a host block
# lasts. -Os does not get it under.
OPTIM = -O3

##############################################################################
# Macros
#

UDEFS = -DUNIT_TARGET_PLATFORM_MICROKORG2 -DEMUSC_MAX_PARTS=8
UDEFS += -DRFX_USERFS_PROGRAMS_PATH=/var/lib/microkorgd/userfs/Programs
# The SC-55's system effects are eight chorus types and eight reverbs on one
# bus; the microKORG2's own effects are on the other side of this unit, and the
# reverb network is the most expensive thing left in the engine.
UDEFS += -DEMUSC_NO_REVERB
UDEFS += -DEMUSC_NO_CHORUS
# The log is off: the userfs is flash, and writing to it wears it out. Build
# with -DRG55_DEBUG to turn rg55.log back on for a diagnosis.
