// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Renders blocks on the real engine and the real ROMs and checks that
 *   - the unit's stereo pair at bufferOffset agrees with one reference engine
 *     driven by the same MIDI, for a chord of eight keys,
 *   - nothing outside that pair is written,
 *   - a host pitch jump starts the new key and stops the old one,
 *   - a host pitch that moves gradually bends the held note instead, past the
 *     range where the old design restarted it,
 *   - every voice stolen four times leaves nothing of the key before it,
 *   - a key pressed again retriggers it, though only one key is down,
 *   - after the keys are released and the tails have fallen, the pair is
 *     cleared rather than left stale.
 */

#include "osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const size_t   kFrames   = 64;
static const uint32_t kStride   = 4;
static const size_t   kBlockLen = kFrames * kStride * 2;  /* two voice groups */
static const float    kPoison   = -1234.5f;
static const float    kGain     = 3.2f;                    /* the unit's fixed output gain */

static unit_runtime_osc_context_t g_ctxt;
/* Mirrors the unit's gain and limiter for whichever reference engine is current. */
static OutputLimiter g_lim;
static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static unit_runtime_desc_t makeDesc() {
    unit_runtime_desc_t d;
    std::memset(&d, 0, sizeof(d));
    d.samplerate = 48000;
    d.frames_per_buffer = (uint32_t)kFrames;
    d.input_channels = 0;
    d.output_channels = 8;
    std::memset(&g_ctxt, 0, sizeof(g_ctxt));
    g_ctxt.outputStride = (uint16_t)kStride;
    g_ctxt.bufferOffset = 0;
    g_ctxt.voiceOffset = 0;
    g_ctxt.voiceLimit = 8;
    d.hooks.runtime_context = &g_ctxt;
    return d;
}

/* The host tells the unit a key went down and, in the same block, that this
 * voice started playing it. */
static void press(Osc& unit, int voice, uint8_t key) {
    g_ctxt.pitch[voice] = (float)key;
    g_ctxt.trigger = (uint8_t)(g_ctxt.trigger | (1u << voice));
    unit.NoteOn(key, 100);
}

static void release(Osc& unit, int voice, uint8_t key) {
    unit.NoteOff(key);
    /* The host frees the voice's trigger along with the key. */
    g_ctxt.trigger = (uint8_t)(g_ctxt.trigger & ~(1u << voice));
}

/* The same part settings the unit gives its own engine: one part per host
 * voice, each listening on that voice's channel. */
static EmuSC::Synth* makeRef(EmuSC::ControlRom& crom, EmuSC::WaveRom& wrom) {
    EmuSC::Synth* s = new EmuSC::Synth(crom, wrom);
    s->set_audio_format(48000, 2);
    for (int v = 0; v < kMk2MaxVoices; ++v) {
        s->set_param(EmuSC::PatchParam::PolyMode, static_cast<uint8_t>(1),
                     static_cast<int8_t>(v));
        s->set_param(EmuSC::PatchParam::PB_PitchControl,
                     static_cast<uint8_t>(0x40 + 24), static_cast<int8_t>(v));
        s->set_part_instrument((uint8_t)v, 0, 0);
    }
    /* The unit caps the engine at the voices the host hands it. */
    s->set_note_limit(kMk2MaxVoices);
    return s;
}

static void refOn(EmuSC::Synth* ref, int voice, uint8_t key) {
    ref->midi_input((uint8_t)(0x90 | voice), key, 100);
}

static void refOff(EmuSC::Synth* ref, int voice, uint8_t key) {
    ref->midi_input((uint8_t)(0x80 | voice), key, 0);
}

static void refBend(EmuSC::Synth* ref, int voice, float semitones) {
    const int value = (int)(8192.0f + semitones / 24.0f * 8192.0f);
    const int clamped = value < 0 ? 0 : value > 16383 ? 16383 : value;
    ref->midi_input((uint8_t)(0xe0 | voice), (uint8_t)(clamped & 0x7f),
                    (uint8_t)((clamped >> 7) & 0x7f));
}

static void poison(float* block) {
    for (size_t i = 0; i < kBlockLen; ++i) block[i] = kPoison;
}

/* The pair at the unit's buffer offset: indices 0 and 1 of each frame. */
static int laneIndex(int channel) { return channel; }

struct Run {
    static void pull(EmuSC::Synth* ref, float expected[kFrames][2]) {
        for (size_t i = 0; i < kFrames; ++i) {
            ref->get_next_frame(expected[i][0], expected[i][1]);
            expected[i][0] *= kGain;
            expected[i][1] *= kGain;
            g_lim.pair(expected[i][0], expected[i][1]);
        }
    }

    /* Render one block and compare the pair with the reference. */
    static void compare(Osc& unit, EmuSC::Synth* ref, float* block,
                        float expected[kFrames][2], const char* what) {
        poison(block);
        unit.Render(block, kFrames);
        pull(ref, expected);

        double worst = 0.0;
        double ref_peak = 0.0;
        double got_peak = 0.0;
        for (size_t i = 0; i < kFrames; ++i) {
            const double want_l = expected[i][0];
            const double want_r = expected[i][1];
            const double got_l = block[i * kStride + laneIndex(0)];
            const double got_r = block[i * kStride + laneIndex(1)];
            const double diff_l = std::fabs(got_l - want_l);
            const double diff_r = std::fabs(got_r - want_r);
            if (diff_l > worst) worst = diff_l;
            if (diff_r > worst) worst = diff_r;
            if (std::fabs(want_l) > ref_peak) ref_peak = std::fabs(want_l);
            if (std::fabs(got_l) > got_peak) got_peak = std::fabs(got_l);
        }

        bool outside_clean = true;
        for (size_t i = 0; i < kBlockLen; ++i) {
            const bool owned = i < kFrames * kStride && (i % kStride) < 2;
            if (!owned && block[i] != kPoison) outside_clean = false;
        }

        std::printf("%s: worst %.3g (unit peak %.3g, ref peak %.3g)\n", what,
                    worst, got_peak, ref_peak);
        check(worst < 1e-6, "the stereo pair matches the reference engine");
        check(outside_clean, "nothing outside the stereo pair is written");
    }
};

int main() {
    Osc unit;
    unit_runtime_desc_t desc = makeDesc();
    if (unit.Init(&desc) != k_unit_err_none) {
        std::printf("init failed (ROMs?)\n");
        return 1;
    }

    /* One engine in the unit, one driven by hand, same construction. */
    EmuSC::ControlRom crom(k_sc55_control_rom, k_sc55_cpu_rom);
    EmuSC::WaveRom wrom(
        std::vector<std::string>(k_sc55_wave_roms, k_sc55_wave_roms + 3), crom);
    EmuSC::Synth* ref = makeRef(crom, wrom);
    g_lim.reset();

    static float block[kBlockLen];
    static float expected[kFrames][2];

    /* Eight held keys, one per host voice, from one block. */
    for (int v = 0; v < kMk2MaxVoices; ++v) {
        press(unit, v, (uint8_t)(60 + v));
        refOn(ref, v, (uint8_t)(60 + v));
    }
    for (int b = 0; b < 8; ++b) {
        unit.Render(block, kFrames);
        Run::pull(ref, expected);
    }

    Run::compare(unit, ref, block, expected, "eight voices, one chord");

    /* A host voice given a new key: its old note is gone and the new one
     * sounds in the part. */
    release(unit, 0, 60);
    press(unit, 0, 84);
    refOff(ref, 0, 60);
    refOn(ref, 0, 84);

    Run::compare(unit, ref, block, expected, "a voice given a new key");

    /* The host steps a held voice's pitch without a new trigger, which is how
     * its arpeggiator moves a note. A jump between blocks is a new key. */
    g_ctxt.pitch[0] = 88.0f;
    refOff(ref, 0, 84);
    refOn(ref, 0, 88);

    Run::compare(unit, ref, block, expected, "a pitch stepped by the host");

    /* Gradual movement is the wheel. The note must keep its envelope, and it
     * must not restart when the bend passes the old design's range: walk past
     * two semitones in steps the wheel could make in one block. */
    bool bent_all = true;
    for (int step = 1; step <= 7; ++step) {
        const float semitones = 0.4f * (float)step;
        g_ctxt.pitch[0] = 88.0f + semitones;
        refBend(ref, 0, semitones);

        poison(block);
        unit.Render(block, kFrames);
        Run::pull(ref, expected);

        double worst = 0.0;
        for (size_t i = 0; i < kFrames; ++i) {
            const double want_l = expected[i][0];
            const double want_r = expected[i][1];
            const double diff_l =
                std::fabs(block[i * kStride + laneIndex(0)] - want_l);
            const double diff_r =
                std::fabs(block[i * kStride + laneIndex(1)] - want_r);
            if (diff_l > worst) worst = diff_l;
            if (diff_r > worst) worst = diff_r;
        }
        if (worst >= 1e-6) bent_all = false;
    }
    std::printf("a pitch bent by the host:\n");
    check(bent_all, "a bend past two semitones keeps the held note");

    /* A jump after a bend is still a new key. */
    g_ctxt.pitch[0] = 92.0f;
    refOff(ref, 0, 88);
    refOn(ref, 0, 92);
    refBend(ref, 0, 0.0f);

    Run::compare(unit, ref, block, expected, "a step after a bend");

    /* Every voice taken again, one block after another, the way the host
     * steals them for a new key: the trigger bit stays up, and a note-off for
     * the old key is followed by a note-on for the new one. Nothing may be
     * left sounding from the key before it. */
    /* The keys the lanes hold now: voice 0 stepped to 92 above, the rest are
     * still on the chord's 61..67. */
    uint8_t held[kMk2MaxVoices] = {92, 61, 62, 63, 64, 65, 66, 67};
    for (int round = 0; round < 4; ++round) {
        uint8_t next[kMk2MaxVoices];
        for (int v = 0; v < kMk2MaxVoices; ++v)
            next[v] = (uint8_t)(60 + v + 12 * (round + 1));
        /* The unit sends a block's new notes before the note-offs the lanes
         * no longer want; the reference is given the same order. */
        uint8_t old[kMk2MaxVoices];
        for (int v = 0; v < kMk2MaxVoices; ++v) old[v] = held[v];
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            g_ctxt.pitch[v] = (float)next[v];
            unit.NoteOn(next[v], 100);
            refOn(ref, v, next[v]);
        }
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            unit.NoteOff(old[v]);
            refOff(ref, v, old[v]);
            held[v] = next[v];
        }
        char what[64];
        std::snprintf(what, sizeof(what), "eight voices taken again, round %d",
                      round + 1);
        Run::compare(unit, ref, block, expected, what);
    }

    /* The same stealing, on engines that have only played the eight keys, to
     * tell a steal problem from state left by the tests above. */
    {
        Osc u;
        if (u.Init(&desc) != k_unit_err_none) {
            std::printf("init failed (ROMs?)\n");
            return 1;
        }
        EmuSC::Synth* r = makeRef(crom, wrom);
        g_lim.reset();
        g_ctxt.trigger = 0xff;
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            g_ctxt.pitch[v] = (float)(60 + v);
            u.NoteOn((uint8_t)(60 + v), 100);
            refOn(r, v, (uint8_t)(60 + v));
        }
        for (int b = 0; b < 40; ++b) {
            u.Render(block, kFrames);
            Run::pull(r, expected);
        }
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            g_ctxt.pitch[v] = (float)(72 + v);
            u.NoteOn((uint8_t)(72 + v), 100);
            refOn(r, v, (uint8_t)(72 + v));
        }
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            u.NoteOff((uint8_t)(60 + v));
            refOff(r, v, (uint8_t)(60 + v));
        }
        Run::compare(u, r, block, expected, "a fresh steal of eight voices");
        delete r;
        u.Teardown();
    }

    /* Release everything; the lowest piano note's tail takes seconds to fall
     * through the unit's -80 dB quiet threshold. The pair must then be
     * cleared, not stale. */
    for (int v = 0; v < kMk2MaxVoices; ++v)
        release(unit, v, held[v]);
    for (int b = 0; b < 5000; ++b) {
        poison(block);
        unit.Render(block, kFrames);
    }
    poison(block);
    unit.Render(block, kFrames);

    bool silent = true;
    double pair_peak = 0.0;
    int poison_hits = 0;
    for (size_t i = 0; i < kBlockLen; ++i) {
        /* The unit's pair is in the first voice group, at the frames of that
         * group; everything past it belongs to a unit that is not present. */
        const bool owned = i < kFrames * kStride && (i % kStride) < 2;
        if (owned) {
            if (std::fabs(block[i]) > pair_peak) pair_peak = std::fabs(block[i]);
            if (block[i] != 0.f) silent = false;
        } else if (block[i] != kPoison) {
            ++poison_hits;
            silent = false;
        }
    }
    std::printf("all released and settled: pair peak %.3g, poison hits %d\n",
                pair_peak, poison_hits);
    check(silent, "the stereo pair is cleared and nothing else is touched");

    /* One key, pressed again: a one-key arpeggio. The host can say so with a
     * note-off and note-on in one block, with the trigger bit falling and
     * rising again, or with the note-on alone. Each is a new attack on the
     * same key, not a second copy of it. */
    {
        Osc u;
        if (u.Init(&desc) != k_unit_err_none) {
            std::printf("init failed (ROMs?)\n");
            return 1;
        }
        EmuSC::Synth* r = makeRef(crom, wrom);
        g_lim.reset();

        g_ctxt.pitch[0] = 60.0f;
        g_ctxt.trigger = 0x01;
        u.NoteOn(60, 100);
        refOn(r, 0, 60);
        for (int b = 0; b < 4; ++b) {
            u.Render(block, kFrames);
            Run::pull(r, expected);
        }

        u.NoteOff(60);
        u.NoteOn(60, 100);
        refOff(r, 0, 60);
        refOn(r, 0, 60);
        Run::compare(u, r, block, expected, "one key: release and press again");

        g_ctxt.trigger = 0x00;
        u.Render(block, kFrames);
        Run::pull(r, expected);
        g_ctxt.trigger = 0x01;
        u.NoteOn(60, 100);
        refOff(r, 0, 60);
        refOn(r, 0, 60);
        Run::compare(u, r, block, expected, "one key: trigger down and up again");

        u.NoteOn(60, 100);
        refOff(r, 0, 60);
        refOn(r, 0, 60);
        Run::compare(u, r, block, expected, "one key: note-on alone");

        delete r;
        u.Teardown();
    }

    /* The host can say "this key is played again" with the note-on first and
     * the note-off for the copy it replaces after it, in one block. That is a
     * retrigger, not a key tapped and let go. */
    {
        Osc u;
        if (u.Init(&desc) != k_unit_err_none) {
            std::printf("init failed (ROMs?)\n");
            return 1;
        }
        EmuSC::Synth* r = makeRef(crom, wrom);
        g_lim.reset();

        g_ctxt.pitch[0] = 60.0f;
        g_ctxt.trigger = 0x01;
        u.NoteOn(60, 100);
        refOn(r, 0, 60);
        for (int b = 0; b < 4; ++b) {
            u.Render(block, kFrames);
            Run::pull(r, expected);
        }

        g_ctxt.trigger = 0x00;
        u.NoteOff(60);
        refOff(r, 0, 60);
        u.Render(block, kFrames);
        Run::pull(r, expected);

        g_ctxt.pitch[0] = 60.0f;
        g_ctxt.trigger = 0x01;
        u.NoteOn(60, 100);
        u.NoteOff(60);
        refOff(r, 0, 60);
        refOn(r, 0, 60);
        Run::compare(u, r, block, expected,
                     "one key: note-on before its note-off");
        /* The new attack is heard from the block after the one that carried
         * it, in both engines. */
        Run::compare(u, r, block, expected,
                     "one key: the replay's attack");

        delete r;
        u.Teardown();
    }

    /* Three keys played over and over. Every strike leaves a release tail in
     * the part, and a voice only comes back when the tail has fallen, so the
     * part fills up while the keys are repeated. It must take a voice back
     * rather than ignore the keys that come next. */
    {
        Osc u;
        if (u.Init(&desc) != k_unit_err_none) {
            std::printf("init failed (ROMs?)\n");
            return 1;
        }
        EmuSC::Synth* r = makeRef(crom, wrom);
        g_lim.reset();
        const uint8_t keys[3] = {60, 64, 67};

        g_ctxt.trigger = 0x00;
        for (int round = 0; round < 100; ++round) {
            for (int k = 0; k < 3; ++k) {
                g_ctxt.pitch[k] = (float)keys[k];
                g_ctxt.trigger |= (uint8_t)(1u << k);
                u.NoteOn(keys[k], 100);
                refOn(r, k, keys[k]);
            }
            for (int b = 0; b < 2; ++b) {
                u.Render(block, kFrames);
                Run::pull(r, expected);
            }
            for (int k = 0; k < 3; ++k) {
                g_ctxt.trigger &= (uint8_t)~(1u << k);
                u.NoteOff(keys[k]);
                refOff(r, k, keys[k]);
            }
            for (int b = 0; b < 2; ++b) {
                u.Render(block, kFrames);
                Run::pull(r, expected);
            }
        }

        /* The five keys go to the unit only, so the reference is a control:
         * what it renders is the tails alone. Had the part ignored the keys,
         * as it did when it filled up, the two would stay matched. */
        for (int v = 3; v < 8; ++v) {
            const uint8_t key = (uint8_t)(72 + v);
            g_ctxt.pitch[v] = (float)key;
            g_ctxt.trigger |= (uint8_t)(1u << v);
            u.NoteOn(key, 100);
        }
        poison(block);
        u.Render(block, kFrames);
        Run::pull(r, expected);
        poison(block);
        u.Render(block, kFrames);
        Run::pull(r, expected);

        double heard = 0.0;
        double diff = 0.0;
        for (size_t i = 0; i < kFrames; ++i) {
            const double want_l = expected[i][0];
            const double want_r = expected[i][1];
            const double got_l = block[i * kStride + laneIndex(0)];
            const double got_r = block[i * kStride + laneIndex(1)];
            if (std::fabs(got_l) > heard) heard = std::fabs(got_l);
            if (std::fabs(got_r) > heard) heard = std::fabs(got_r);
            if (std::fabs(got_l - want_l) > diff) diff = std::fabs(got_l - want_l);
            if (std::fabs(got_r - want_r) > diff) diff = std::fabs(got_r - want_r);
        }
        std::printf("five keys after repeated play: heard %.3g, "
                    "tails-only diff %.3g\n", heard, diff);
        check(diff > 0.01, "the keys played after the repeats sound");

        delete r;
        u.Teardown();
    }

    std::printf("summary: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    delete ref;
    unit.Teardown();
    return g_failures == 0 ? 0 : 1;
}
