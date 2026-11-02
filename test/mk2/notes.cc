// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Host check: does the unit leave a note sounding after the keys are released?
 * Drives note events in the orders a keyboard produces when keys are held and
 * dragged over, and measures the output after everything is up.
 */

#include "osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const size_t kFrames = 64;
static const float kPoison = 0.f;

static unit_runtime_osc_context_t g_ctxt;
static float g_block[64 * 4 * 2];
static int g_failures = 0;

static void init() {
    g_ctxt.outputStride = 4;
}

static float renderBlock(Osc& unit, int blocks = 1) {
    float peak = 0.f;
    for (int b = 0; b < blocks; ++b) {
        std::memset(g_block, 0, sizeof(g_block));
        unit.Render(g_block, kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            const float l = std::fabs(g_block[i * 4]);
            if (l > peak) peak = l;
        }
    }
    return peak;
}

static void check(bool ok, const char* what, float value) {
    std::printf("  %s %-58s (peak %.4f)\n", ok ? "ok  " : "FAIL", what, value);
    if (!ok) ++g_failures;
}

static void settle(Osc& unit) {
    /* A second of blocks, so the release tail is gone whatever the patch. */
    renderBlock(unit, 750);
}

int main() {
    unit_runtime_osc_context_t* ctxt = &g_ctxt;
    init();

    /* A: hold three keys, then release them one by one. */
    {
        Osc unit;
        unit_runtime_desc_t d;
        std::memset(&d, 0, sizeof(d));
        d.samplerate = 48000;
        d.frames_per_buffer = 64;
        d.output_channels = 8;
        g_ctxt.outputStride = 4;
        d.hooks.runtime_context = ctxt;
        if (unit.Init(&d) != k_unit_err_none) { std::printf("init failed\n"); return 1; }

        unit.NoteOn(60, 100); renderBlock(unit, 4);
        unit.NoteOn(64, 100); renderBlock(unit, 4);
        unit.NoteOn(67, 100); renderBlock(unit, 4);
        const float held = renderBlock(unit, 4);
        check(held > 0.01f, "three keys held: something is sounding", held);
        unit.NoteOff(60); renderBlock(unit, 4);
        unit.NoteOff(64); renderBlock(unit, 4);
        unit.NoteOff(67);
        settle(unit);
        const float after = renderBlock(unit, 8);
        check(after < 1e-4f, "all three released: silence", after);
        unit.Teardown();
    }

    /* B: drag over three keys, the way a hand slides across the keyboard:
     * each new key down before the one before it comes up. */
    {
        Osc unit;
        unit_runtime_desc_t d;
        std::memset(&d, 0, sizeof(d));
        d.samplerate = 48000;
        d.frames_per_buffer = 64;
        d.output_channels = 8;
        d.hooks.runtime_context = ctxt;
        if (unit.Init(&d) != k_unit_err_none) { std::printf("init failed (ROMs?)\n"); return 1; }

        unit.NoteOn(60, 100); renderBlock(unit, 2);
        unit.NoteOn(64, 100); unit.NoteOff(60); renderBlock(unit, 2);
        unit.NoteOn(67, 100); unit.NoteOff(64); renderBlock(unit, 2);
        unit.NoteOff(67);
        settle(unit);
        const float after = renderBlock(unit, 8);
        check(after < 1e-4f, "dragged across three keys: silence after release", after);
        unit.Teardown();
    }

    /* C: a fast drag where a key is down and up again inside one block. */
    {
        Osc unit;
        unit_runtime_desc_t d;
        std::memset(&d, 0, sizeof(d));
        d.samplerate = 48000;
        d.frames_per_buffer = 64;
        d.output_channels = 8;
        d.hooks.runtime_context = ctxt;
        if (unit.Init(&d) != k_unit_err_none) { std::printf("init failed (ROMs?)\n"); return 1; }

        unit.NoteOn(60, 100); renderBlock(unit, 4);
        unit.NoteOn(64, 100);
        unit.NoteOff(64);
        renderBlock(unit, 4);
        unit.NoteOff(60);
        settle(unit);
        const float after = renderBlock(unit, 8);
        check(after < 1e-4f, "a key tapped and released inside one block: silence", after);
        unit.Teardown();
    }

    /* E: unison. The host sends one note-on per voice for a key, and the
     * matching note-offs; the key is down once. */
    {
        Osc unit;
        unit_runtime_desc_t d;
        std::memset(&d, 0, sizeof(d));
        d.samplerate = 48000;
        d.frames_per_buffer = 64;
        d.output_channels = 8;
        d.hooks.runtime_context = ctxt;
        if (unit.Init(&d) != k_unit_err_none) { std::printf("init failed (ROMs?)\n"); return 1; }

        for (int i = 0; i < 3; ++i) unit.NoteOn(60, 100);
        renderBlock(unit, 4);
        for (int i = 0; i < 3; ++i) unit.NoteOn(64, 100);
        renderBlock(unit, 4);
        for (int i = 0; i < 3; ++i) unit.NoteOff(60);
        renderBlock(unit, 4);
        for (int i = 0; i < 3; ++i) unit.NoteOff(64);
        settle(unit);
        const float after = renderBlock(unit, 8);
        check(after < 1e-4f, "unison: three note-ons and three note-offs per key", after);
        unit.Teardown();
    }

    /* D: the same, for the note that is sounding when a newer one is tapped. */
    {
        Osc unit;
        unit_runtime_desc_t d;
        std::memset(&d, 0, sizeof(d));
        d.samplerate = 48000;
        d.frames_per_buffer = 64;
        d.output_channels = 8;
        d.hooks.runtime_context = ctxt;
        if (unit.Init(&d) != k_unit_err_none) { std::printf("init failed (ROMs?)\n"); return 1; }

        unit.NoteOn(60, 100); renderBlock(unit, 4);
        unit.NoteOn(67, 100); unit.NoteOff(60); renderBlock(unit, 4);
        unit.NoteOff(67); renderBlock(unit, 4);
        unit.NoteOff(64);
        settle(unit);
        const float after = renderBlock(unit, 8);
        check(after < 1e-4f, "release of a key that was never started: silence", after);
        unit.Teardown();
    }

    std::printf("%s\n", g_failures ? "FAILURES" : "all sequences end silent");
    return g_failures ? 1 : 0;
}
