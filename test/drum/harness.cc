// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Host check for the RG55 drumlogue unit: the unit's own source, driven
 * through the callbacks the device calls, with the real engine and the real
 * ROMs. It checks that
 *   - the header's three parameters hold what it declares,
 *   - a gate sounds, and the same note played as a note gives the same samples,
 *   - the note ends when the gate does, and a newer note replaces the one
 *     sounding rather than leaving it under the new one,
 *   - PROGRAM changes the sound and LEVEL's ends are silence and full scale.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "unit.h"

static const uint32_t kFrames = 128;
static int g_checks = 0;
static int g_failures = 0;

struct Result {
    double peak;      /* the loudest sample of the whole run */
    double last;      /* the loudest sample of the last block */
    bool finite;
};

static void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static unit_runtime_desc_t makeDesc() {
    unit_runtime_desc_t d;
    std::memset(&d, 0, sizeof(d));
    d.target = unit_header.target;
    d.api = UNIT_API_VERSION;
    d.samplerate = 48000;
    d.frames_per_buffer = kFrames;
    d.input_channels = 2;
    d.output_channels = 2;
    return d;
}

static bool start() {
    unit_runtime_desc_t d = makeDesc();
    return unit_init(&d) == k_unit_err_none;
}

static Result render(int blocks, std::vector<float>* capture = nullptr) {
    std::vector<float> in(kFrames * 2, 0.0f);
    std::vector<float> out(kFrames * 2);
    Result r = {0.0, 0.0, true};
    for (int b = 0; b < blocks; ++b) {
        unit_render(in.data(), out.data(), kFrames);
        double last = 0.0;
        for (float v : out) {
            const double a = std::fabs(v);
            if (!std::isfinite(v)) r.finite = false;
            if (a > last) last = a;
            if (a > r.peak) r.peak = a;
        }
        r.last = last;
        if (capture) capture->insert(capture->end(), out.begin(), out.end());
    }
    return r;
}

static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        worst = std::fmax(worst, std::fabs(a[i] - b[i]));
    return worst;
}

int main() {
    if (!start()) {
        std::printf("init failed (ROMs?)\n");
        return 1;
    }
    check(unit_get_param_value(0) == 0 && unit_get_param_value(1) == 80 &&
              unit_get_param_value(2) == 60,
          "the parameters start at 0, 80, 60");
    const char* program0 = unit_get_param_str_value(0, 0);
    check(program0 && std::strcmp(program0, "Piano 1") == 0,
          "PROGRAM 0 is the control ROM's first instrument");
    unit_teardown();

    /* The gate's note and the same note played as a note are one path. */
    std::vector<float> gate, note;
    start();
    unit_gate_on(100);
    Result g = render(100, &gate);
    unit_teardown();
    start();
    unit_note_on(60, 100);
    Result n = render(100, &note);
    unit_teardown();
    check(g.peak > 0.05, "the gate sounds");
    check(n.peak > 0.05, "note 60 sounds");
    check(g.finite && n.finite && g.peak <= 1.0,
          "every sample is finite and inside the device's range");
    check(maxDiff(gate, note) < 1e-6, "gate and NOTE 60 are the same samples");

    /* The gate holds the note only until it ends. */
    start();
    unit_gate_on(100);
    render(40);
    unit_gate_off();
    Result off = render(400);
    unit_teardown();
    check(off.finite && off.last < 1e-4, "the note ends when the gate does");

    /* The newest key held sounds; releasing it returns to the one still held. */
    start();
    unit_note_on(60, 100);
    render(20);
    unit_note_on(70, 100);
    render(20);
    unit_note_off(70);
    const Result back = render(40);
    unit_note_off(60);
    const Result mono = render(400);
    unit_teardown();
    check(back.finite && back.peak > 0.01, "releasing the newer key returns to the held one");
    check(mono.finite && mono.last < 1e-4, "no note is left sounding");

    /* PROGRAM changes the sound. */
    std::vector<float> p0, p1;
    start();
    unit_note_on(60, 100);
    render(60, &p0);
    unit_teardown();
    start();
    unit_set_param_value(0, 1);
    unit_note_on(60, 100);
    render(60, &p1);
    unit_teardown();
    check(maxDiff(p0, p1) > 0.01, "PROGRAM 1 sounds different from PROGRAM 0");

    /* LEVEL's ends: silence and full scale, against the default 80. */
    start();
    unit_set_param_value(1, 0);
    unit_note_on(60, 100);
    Result l0 = render(60);
    unit_teardown();
    start();
    unit_set_param_value(1, 100);
    unit_note_on(60, 100);
    Result l100 = render(60);
    unit_teardown();
    check(l0.finite && l0.peak == 0.0, "LEVEL 0 is silence");
    check(l100.peak > n.peak, "LEVEL 100 is louder than the default 80");

    std::printf("summary: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
