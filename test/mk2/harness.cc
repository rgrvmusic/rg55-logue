// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Renders two blocks with the real engine and the real ROMs: the first into
 * the first voice group (bufferOffset 0), the second into the second group
 * (bufferOffset frames*4), and checks that
 *   - every lane of the pair is the reference engine's sample times the unit's
 *     gain,
 *   - every other lane in the whole block is still the poison value, and
 *   - the group the unit was not asked for is untouched by the call.
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

static void poison(float* block, size_t n) {
    for (size_t i = 0; i < n; ++i) block[i] = kPoison;
}

int main() {
    Osc unit;
    unit_runtime_desc_t desc = makeDesc();
    if (unit.Init(&desc) != k_unit_err_none) {
        std::printf("init failed (ROMs?)\n");
        return 1;
    }

    /* The same engine, driven by hand, with nothing between it and the line. */
    EmuSC::ControlRom crom(k_sc55_control_rom, k_sc55_cpu_rom);
    EmuSC::WaveRom wrom(std::vector<std::string>(k_sc55_wave_roms,
                                                 k_sc55_wave_roms + 3), crom);
    EmuSC::Synth ref(crom, wrom);
    ref.set_audio_format(48000, 2);
    ref.set_param(EmuSC::PatchParam::PolyMode, static_cast<uint8_t>(0),
                  static_cast<int8_t>(0));
    ref.set_part_instrument(0, 0, 0);

    static float block[kBlockLen];
    static float expected[kFrames * 2];

    /* A few blocks under the key, so the note is well past its attack. */
    unit.NoteOn(60, 100);
    ref.midi_input(0x90, 60, 100);
    for (int b = 0; b < 8; ++b) {
        g_ctxt.bufferOffset = 0;
        poison(block, kBlockLen);
        unit.Render(block, kFrames);
        for (size_t i = 0; i < kFrames; ++i)
            ref.get_next_frame(expected[i * 2], expected[i * 2 + 1]);
    }

    /* Block 9: the first voice group, checked lane by lane. */
    g_ctxt.bufferOffset = 0;
    poison(block, kBlockLen);
    unit.Render(block, kFrames);
    for (size_t i = 0; i < kFrames; ++i)
        ref.get_next_frame(expected[i * 2], expected[i * 2 + 1]);

    double worst = 0.0;
    bool lanes_ok = true, none_else = true, second_group_clean = true;
    for (size_t i = 0; i < kFrames; ++i) {
        const size_t at = i * kStride;
        const double dl = std::fabs(block[at] - expected[i * 2] * kGain);
        const double dr = std::fabs(block[at + 1] - expected[i * 2 + 1] * kGain);
        if (dl > worst) worst = dl;
        if (dr > worst) worst = dr;
        if (block[at + 2] != kPoison || block[at + 3] != kPoison) none_else = false;
    }
    for (size_t i = kFrames * kStride; i < kBlockLen; ++i)
        if (block[i] != kPoison) second_group_clean = false;

    std::printf("group 0 (bufferOffset 0):\n");
    check(worst < 1e-6, "lane 0 and lane 1 carry the engine's samples");
    check(none_else, "lanes 2 and 3, the other voices of the group, are untouched");
    check(second_group_clean, "the second voice group is untouched");

    /* Block 10: the second voice group, at bufferOffset frames * stride. */
    const uint32_t second = (uint32_t)(kFrames * kStride);
    poison(block, kBlockLen);
    g_ctxt.bufferOffset = (uint16_t)second;
    unit.Render(block, kFrames);
    for (size_t i = 0; i < kFrames; ++i)
        ref.get_next_frame(expected[i * 2], expected[i * 2 + 1]);

    worst = 0.0;
    bool first_clean = true;
    for (size_t i = 0; i < kFrames; ++i) {
        const size_t at = second + i * kStride;
        const double dl = std::fabs(block[at] - expected[i * 2] * kGain);
        const double dr = std::fabs(block[at + 1] - expected[i * 2 + 1] * kGain);
        if (dl > worst) worst = dl;
        if (dr > worst) worst = dr;
    }
    for (size_t i = 0; i < second; ++i)
        if (block[i] != kPoison) first_clean = false;

    std::printf("group 1 (bufferOffset %u):\n", second);
    check(worst < 1e-6, "lane 0 and lane 1 carry the engine's samples");
    check(first_clean, "the first voice group is untouched");

    /* The engine is a stateful stream: two groups are two halves of one block,
     * so the samples have to continue where the previous call left off. */
    std::printf("summary: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    unit.Teardown();
    return g_failures == 0 ? 0 : 1;
}
