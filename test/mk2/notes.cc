// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Host check: does the unit leave a note sounding after the keys are released,
 * and does it keep sounding the notes it was given? Drives the voices the way
 * the microKORG2's host does: a key's note-on next to the trigger bit of the
 * voice it landed on, and that voice's key-up when the key is let go.
 */

#include "osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const size_t kFrames = 64;
static const int kVoices = 4;   /* the layout this unit is played in */

static unit_runtime_osc_context_t g_ctxt;
static float g_block[kFrames * 4 * 2];
static int g_failures = 0;

static void resetContext() {
    std::memset(&g_ctxt, 0, sizeof(g_ctxt));
    g_ctxt.outputStride = 4;
    g_ctxt.bufferOffset = 0;
    g_ctxt.voiceOffset = 0;
    g_ctxt.voiceLimit = (uint8_t)kVoices;
}

static bool initUnit(Osc& unit) {
    resetContext();
    unit_runtime_desc_t d;
    std::memset(&d, 0, sizeof(d));
    d.samplerate = 48000;
    d.frames_per_buffer = (uint32_t)kFrames;
    d.output_channels = 8;
    d.hooks.runtime_context = &g_ctxt;
    return unit.Init(&d) == k_unit_err_none;
}

static void press(Osc& unit, int voice, uint8_t key, uint8_t velocity = 100) {
    g_ctxt.pitch[voice] = (float)key;
    g_ctxt.trigger = (uint8_t)(g_ctxt.trigger | (1u << voice));
    unit.NoteOn(key, velocity);
}

static void release(Osc& unit, int voice, uint8_t key) {
    unit.NoteOff(key);
    g_ctxt.trigger = (uint8_t)(g_ctxt.trigger & ~(1u << voice));
}

static float renderBlock(Osc& unit, int blocks = 1) {
    float peak = 0.f;
    for (int b = 0; b < blocks; ++b) {
        std::memset(g_block, 0, sizeof(g_block));
        unit.Render(g_block, kFrames);
        for (size_t i = 0; i < kFrames * 4; ++i) {
            const float magnitude = std::fabs(g_block[i]);
            if (magnitude > peak) peak = magnitude;
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
    /* A: hold a chord, then release its keys one by one. */
    {
        Osc unit;
        if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
        press(unit, 0, 60); press(unit, 1, 64); press(unit, 2, 67);
        const float held = renderBlock(unit, 4);
        check(held > 0.01f, "a three-note chord sounds", held);
        release(unit, 0, 60); renderBlock(unit, 4);
        release(unit, 1, 64); renderBlock(unit, 4);
        release(unit, 2, 67);
        settle(unit);
        check(renderBlock(unit, 8) < 1e-4f, "all three released: silence", 0.f);
        unit.Teardown();
    }

    /* B: drag over three keys, the way a hand slides across the keyboard:
     * each new key down before the one before it comes up. */
    {
        Osc unit;
        if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
        press(unit, 0, 60); renderBlock(unit, 2);
        press(unit, 1, 64); release(unit, 0, 60); renderBlock(unit, 2);
        press(unit, 2, 67); release(unit, 1, 64); renderBlock(unit, 2);
        release(unit, 2, 67);
        settle(unit);
        check(renderBlock(unit, 8) < 1e-4f,
              "dragged across three keys: silence after release", 0.f);
        unit.Teardown();
    }

    /* C: a key tapped inside one block. The host may still set that voice's
     * trigger: nothing may start from it, because a note released before its
     * first render would never be heard to stop. */
    {
        Osc unit;
        if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
        press(unit, 0, 60); renderBlock(unit, 4);
        g_ctxt.pitch[1] = 64.f;
        g_ctxt.trigger = (uint8_t)(g_ctxt.trigger | (1u << 1));
        unit.NoteOn(64, 100);
        unit.NoteOff(64);
        renderBlock(unit, 4);
        g_ctxt.trigger = (uint8_t)(g_ctxt.trigger & ~(1u << 1));
        release(unit, 0, 60);
        settle(unit);
        check(renderBlock(unit, 8) < 1e-4f,
              "a key tapped and released inside one block: silence", 0.f);
        unit.Teardown();
    }

    /* D: a key-up with no note-on before it starts nothing. */
    {
        Osc unit;
        if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
        unit.NoteOff(64);
        press(unit, 0, 60); renderBlock(unit, 4);
        release(unit, 0, 60);
        settle(unit);
        check(renderBlock(unit, 8) < 1e-4f,
              "release of a key that was never started: silence", 0.f);
        unit.Teardown();
    }

    /* E: unison. Three note-ons for one key, each on its own voice, are three
     * notes in the engine; the old design kept one note per key. */
    {
        float one = 0.f;
        {
            Osc unit;
            if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
            press(unit, 0, 60);
            one = renderBlock(unit, 4);
            unit.Teardown();
        }
        Osc unit;
        if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
        for (int v = 0; v < 3; ++v) press(unit, v, 60);
        const float held = renderBlock(unit, 4);
        check(held > one * 2.0f, "unison: three voices are three notes", held);
        for (int v = 0; v < 3; ++v) release(unit, v, 60);
        settle(unit);
        check(renderBlock(unit, 8) < 1e-4f,
              "unison: three note-offs release all three voices", 0.f);
        unit.Teardown();
    }

    /* F: the playing this unit is used for. Four-note chords, each released
     * before the next is played on the same four voices, a few times over.
     * Nothing may be left behind for the chord after it to trip over. */
    {
        Osc unit;
        if (!initUnit(unit)) { std::printf("init failed\n"); return 1; }
        static const uint8_t kChords[][4] = {
            { 60, 64, 67, 72 },
            { 61, 65, 68, 73 },
            { 62, 66, 69, 74 },
            { 59, 63, 66, 71 },
        };
        bool all_sound = true;
        for (int c = 0; c < 4; ++c) {
            for (int v = 0; v < kVoices; ++v) press(unit, v, kChords[c][v]);
            if (renderBlock(unit, 8) <= 0.01f) all_sound = false;
            for (int v = 0; v < kVoices; ++v) release(unit, v, kChords[c][v]);
            renderBlock(unit, 8);
        }
        check(all_sound, "four chords in a row each sound in full", 0.f);
        settle(unit);
        check(renderBlock(unit, 8) < 1e-4f,
              "after the fourth chord is released: silence", 0.f);
        unit.Teardown();
    }

    std::printf("%s\n", g_failures ? "FAILURES" : "all sequences end silent");
    return g_failures ? 1 : 0;
}
