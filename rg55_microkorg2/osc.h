// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/**
 * RGSC55 - microKORG2 oscillator
 *
 * libEmuSC SC-55 player. One EmuSC::Synth, one part per host voice.
 *
 * Up to 8 parts, on MIDI channels 0..7. A fresh `trigger` bit starts a voice
 * from `pitch[]`; the note callbacks carry key and velocity.
 *
 * Output is the stereo pair at `bufferOffset`: `out[offset + i * outputStride]`
 * is left, `+ 1` right.
 *
 * A `pitch[]` jump of k_step or more between blocks is a new key. Smaller
 * moves are the wheel and are sent as a pitch bend.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "unit.h"
#include "runtime.h"
#include "unit_osc.h"
#include <control_rom.h>
#include <synth.h>
#include <wave_rom.h>
#include "rg55_log.h"
#include "unit_note_queue.h"

static const char* const k_sc55_cpu_rom =
    RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/sc55_rom1.bin";
static const char* const k_sc55_control_rom =
    RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/sc55_rom2.bin";
static const char* const k_sc55_wave_roms[] = {
    RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/sc55_waverom1.bin",
    RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/sc55_waverom2.bin",
    RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/sc55_waverom3.bin",
};

enum {
    PARAM_PROGRAM,
};

/*
 * libEmuSC keeps the SC-55's own internal levels, which leave a single loud
 * patch at about -13 dBFS and an ordinary one 10 dB below that. This brings
 * the loudest patch at full velocity up to about -1.5 dBFS, so the part sits
 * in the mix like the other units. The keyboard has a level of its own, so
 * there is no level knob here.
 *
 * One note may use that headroom; a chord sums and would cross full scale.
 * The limiter below takes the sum down to the threshold instead, so a chord
 * sits where one note does and the host never has to clip it.
 */
static constexpr float k_output_gain = 3.2f;
static constexpr float k_limit_level = 0.5f;
static constexpr float k_limit_release = 0.00042f;  /* ~50 ms at 48 kHz */

struct OutputLimiter {
    float gain = 1.f;

    /*
     * Attack is the peak's own sample, so nothing over the threshold is ever
     * handed out; release is slow enough that a chord's tail does not pump.
     */
    void pair(float& l, float& r) {
        const float al = l < 0.f ? -l : l;
        const float ar = r < 0.f ? -r : r;
        const float peak = al > ar ? al : ar;
        if (peak > k_limit_level) {
            gain = k_limit_level / peak;
        } else if (gain < 1.f) {
            gain += k_limit_release * (1.f - gain);
            if (gain > 1.f) gain = 1.f;
        }
        l *= gain;
        r *= gain;
    }

    void reset() { gain = 1.f; }
};

static constexpr uint8_t k_default_velocity = 100;

/* A pitch that moves this much between two blocks is a new key, not a bend:
 * the wheel moves by hundredths of a semitone in a block, an arpeggiator
 * step by at least one. A step also has to land on a key: a wheel moving
 * quickly is still a bend, and never a restarted note. */
static constexpr float k_step = 0.5f;
static constexpr float k_land = 0.05f;

/* The part's bend range. The host's wheel can reach further than the SC-55's
 * default two semitones, so the part is opened up to the range it accepts. */
static constexpr float k_bend_range = 24.f;
static constexpr float k_bend_epsilon = 0.01f;

/* A released note's tail falls below this, and the engine is not advanced
 * again until a key is played: an idle unit does not render a silent part. */
static constexpr float k_quiet_level = 1e-4f;
static constexpr unsigned k_quiet_blocks = 16;

/* The last note event a key carried in a block: a press and release in that
 * order is a tap, a release and press is a retrigger. */
enum : uint8_t { kLastNone = 0, kLastOn = 1, kLastOff = 2 };

/*
 * The ROM objects, shared by every unit instance in the process. The host
 * builds the unit twice for a load and constructing these reads and decodes
 * several megabytes from the card, so the second instance waits for the
 * first one's copy instead of doing it again. Namespace scope with internal
 * linkage: the unit is one translation unit, and the SDK's compiler does not
 * take C++17 inline variables.
 */
namespace rg55_roms {

struct Cache {
    EmuSC::ControlRom* control;
    EmuSC::WaveRom* wave;
    unsigned users;
};

static Cache cache = {nullptr, nullptr, 0};
static std::mutex lock;

}  // namespace rg55_roms

class Osc {
public:
    Osc()
        : control_rom_(nullptr)
        , wave_rom_(nullptr)
        , synth_(nullptr)
        , sample_rate_(48000)
        , channels_(2)
        , output_channels_(8)
        , init_failed_(false)
        , flush_to_zero_(false)
        , panic_requested_(false)
        , program_dirty_(false)
        , program_(0)
        , last_trigger_(0)
        , last_velocity_(k_default_velocity)
        , bend_{}
        , quiet_(false)
        , quiet_blocks_(0)
        , render_blocks_(0)
        , note_events_(0)
        , max_block_ms_(0.0)
        , late_blocks_(0)
        , max_peak_(0.f)
        , logged_active_(0)
        , late_logs_(0)
        , previous_active_(0)
        , last_entry_(0.0)
        , max_gap_ms_(0.0)
        , long_gaps_(0)
        , lane_probe_logs_(0)
        , bad_lane_logs_(0)
        , seen_layout_count_(0)
    {
        std::memset(&runtime_desc_, 0, sizeof(runtime_desc_));
        clearLanes();
    }

    ~Osc() { Teardown(); }

    int8_t Init(const unit_runtime_desc_t* desc) {
        if (!desc) return k_unit_err_undef;
        rg55::log("init: target=0x%04x api=0x%08x sr=%lu frames=%u in=%u out=%u",
                  static_cast<unsigned>(desc->target), desc->api,
                  static_cast<unsigned long>(desc->samplerate),
                  desc->frames_per_buffer, desc->input_channels,
                  desc->output_channels);

        if (desc->samplerate != 48000) return k_unit_err_samplerate;
        if (desc->input_channels != 0 || desc->output_channels != 8)
            return k_unit_err_geometry;

        runtime_desc_ = *desc;
        sample_rate_ = desc->samplerate;
        channels_ = 2;
        output_channels_ = desc->output_channels;

        /*
         * The host builds the unit twice for a load. The ROM objects are
         * immutable once built and take several seconds of the device's time,
         * so both instances share one copy and the second load only builds
         * its own engine.
         */
        if (!loadRoms()) return k_unit_err_memory;
        if (!buildSynth()) {
            releaseRoms();
            return k_unit_err_memory;
        }
        return k_unit_err_none;
    }

    void Teardown() {
        if (torn_down_) return;
        torn_down_ = true;
        rg55::log("teardown");
        logSummary("teardown");
        delete synth_;
        synth_ = nullptr;
        releaseRoms();
        clearLanes();
        last_trigger_ = 0;
    }

    void Reset() {
        rg55::log("reset");
        /* A panic frees notes, so it belongs on the render thread too. */
        panic_requested_ = true;
        /* Queued events belong to the pattern that was just stopped. */
        events_.clear();
    }

    void Resume() { rg55::log("resume"); }
    void Suspend() {
        rg55::log("suspend");
        logSummary("suspend");
    }

    fast_inline void Render(float* out, size_t frames) {
        /*
         * libEmuSC reports an unknown sample or bank by throwing, and it does
         * so from the code that renders. Catch it here and the block is
         * silence instead of a unit that stops the host.
         */
        try {
            renderBlock(out, frames);
        } catch (const std::string& error) {
            rg55::log("render: %s", error.c_str());
            silence(out, frames);
        } catch (const std::exception& ex) {
            rg55::log("render: %s", ex.what());
            silence(out, frames);
        }
    }

    void renderBlock(float* out, size_t frames) {
        [[maybe_unused]] double gap_ms = -1.0;
#ifdef RG55_DEBUG
        /* The host calls this once per block while it is running. A gap much
         * longer than a block is the host itself missing a deadline, not the
         * unit, and nothing the unit measures inside a block can see it. A gap
         * that has shrunk to the work this unit did is the host running flat
         * out: it called back the moment the previous block was returned. */
        const double block_started = rg55::seconds();
        if (last_entry_ > 0.0) {
            const double gap = block_started - last_entry_;
            if (gap > 0.0 && gap < 1.0) {
                gap_ms = gap * 1000.0;
                if (gap_ms > max_gap_ms_) max_gap_ms_ = gap_ms;
                if (gap_ms > frames / static_cast<double>(sample_rate_) * 1500.0)
                    ++long_gaps_;
            }
        }
        last_entry_ = block_started;
#endif
        if (!flush_to_zero_) {
            enableFlushToZero();
            flush_to_zero_ = true;
        }

        const unit_runtime_osc_context_t* ctxt = oscContext();
        const uint32_t stride = outputStride(ctxt);
        const unsigned limit = voiceLimit(ctxt);
        logLayout(ctxt, limit, stride);

        if (panic_requested_) {
            panic_requested_ = false;
            panic();
        }
        if (program_dirty_) {
            program_dirty_ = false;
            applyProgram();
        }

        if (!synth_) {
            silence(out, frames);
            return;
        }

        /* The host gives this unit a fixed number of voices. The engine's own
         * limit is the SC-55's 24, and a released tail keeps its voice until
         * it has fallen, so a piano could leave the part rendering 24 notes
         * for 8 keys. Cap it at the voices the host actually has. */
        synth_->set_note_limit(static_cast<int>(limit));

        /* The key callbacks for this block. */
        uint8_t on_key[64];
        uint8_t on_velocity[64];
        uint8_t off_key[64];
        unsigned on_count = 0;
        unsigned off_count = 0;
        bool panic_event = false;
        std::memset(key_last_, kLastNone, sizeof(key_last_));
        std::memset(released_, 0, sizeof(released_));
        std::memset(attacked_, 0, sizeof(attacked_));
        drainNoteQueue(on_key, on_velocity, on_count, off_key, off_count,
                       panic_event);
        if (panic_event) panic();

        /* A key that was pressed and released inside this block starts
         * nothing: the release would land before the note's first render, and
         * libEmuSC's first update would overwrite it. Remembered so the fresh
         * trigger bit that came with it does not start it from pitch. The
         * order matters: a release followed by a press is a retrigger, and is
         * not a tap. */
        uint8_t tap_key[64];
        unsigned tap_count = 0;
        unsigned kept = 0;
        for (unsigned k = 0; k < on_count; ++k) {
            if (key_last_[on_key[k]] == kLastOff && !laneHolds(on_key[k]) &&
                !hostHolds(ctxt, limit, on_key[k])) {
                if (tap_count < 64) tap_key[tap_count++] = on_key[k];
#ifdef RG55_DEBUG
                rg55::log("host: tap %u dropped", on_key[k]);
#endif
                continue;
            }
            on_key[kept] = on_key[k];
            on_velocity[kept] = on_velocity[k];
            ++kept;
        }
        on_count = kept;

        /* Releases first: a key pressed again in the same block is a new note,
         * not a continuation of the one being let go. */
        for (unsigned j = 0; j < off_count; ++j) releaseKey(off_key[j]);

        const uint8_t fresh =
            ctxt ? static_cast<uint8_t>(ctxt->trigger & ~last_trigger_) : 0;
#ifdef RG55_DEBUG
        logHostState(ctxt, limit);
#endif
        if (ctxt) last_trigger_ = ctxt->trigger;

        /* The trigger bit is not a gate: it can fall while the key is still
         * held, so only the key's note-off releases a lane. */

        bool on_used[64] = {};
        if (fresh) {
            for (unsigned i = 0; i < limit; ++i) {
                const int voice = voiceIndex(ctxt, i);
                if (voice < 0 || voice >= kMk2MaxVoices) continue;
                if (!(fresh & static_cast<uint8_t>(1u << voice))) continue;

                const uint8_t pitch_note = noteFromPitch(ctxt->pitch[i]);

                /* Prefer the callback whose key is the note this voice holds,
                 * so the engine gets the keyboard's own key and velocity. */
                int match = -1;
                for (unsigned k = 0; k < on_count; ++k) {
                    if (!on_used[k] && on_key[k] == pitch_note) {
                        match = static_cast<int>(k);
                        break;
                    }
                }
                if (match >= 0) {
                    on_used[match] = true;
                    startLane(static_cast<unsigned>(voice), on_key[match],
                              on_velocity[match], ctxt->pitch[i]);
                    continue;
                }

                bool tapped = false;
                for (unsigned t = 0; t < tap_count; ++t) {
                    if (tap_key[t] == pitch_note) {
                        tapped = true;
                        break;
                    }
                }
                if (tapped) continue;

                /* No callback for this key: take the first unused one, or the
                 * context's pitch when no callback arrived with the trigger. */
                uint8_t key = pitch_note;
                uint8_t velocity = last_velocity_;
                for (unsigned k = 0; k < on_count; ++k) {
                    if (!on_used[k]) {
                        key = on_key[k];
                        velocity = on_velocity[k];
                        on_used[k] = true;
                        break;
                    }
                }
                startLane(static_cast<unsigned>(voice), key, velocity,
                          ctxt->pitch[i]);
            }
        }

        /*
         * A key the host pressed again while the part already sounds it. Its
         * voice may not have had a fresh trigger bit (a latched arpeggiator
         * keeps the bit up), and its pitch has not moved (that is the whole
         * point of a one-key arpeggio), so the callback is the only sign of a
         * new attack. Retriggering sends the note-off and note-on, which is
         * what a note pressed again means; it never adds a second copy.
         */
        if (ctxt) {
            for (unsigned k = 0; k < on_count; ++k) {
                if (on_used[k]) continue;
                const uint8_t key = on_key[k];
                if (!laneHolds(key) || attacked_[key]) continue;
                for (unsigned v = 0; v < kMk2MaxVoices; ++v) {
                    if (!lane_on_[v] || lane_key_[v] != key) continue;
                    retrigger(v, key, on_velocity[k]);
                }
                attacked_[key] = true;
                on_used[k] = true;
            }
        }

        /*
         * A key the host put on a voice that was already triggered. Its
         * trigger bit was high in the last block too, so it is not a fresh
         * edge; the key callback and the voice's new pitch are what say which
         * voice it landed on. A voice released in this same block is free
         * again, which is how a note repeated on one voice arrives, so its
         * new pitch is compared with the key itself.
         */
        if (ctxt) {
            for (unsigned k = 0; k < on_count; ++k) {
                if (on_used[k]) continue;
                const uint8_t key = on_key[k];
                int best = -1;
                float best_pitch = 0.f;
                float best_score = k_step;
                for (unsigned i = 0; i < limit; ++i) {
                    const int voice = voiceIndex(ctxt, i);
                    if (voice < 0 || voice >= kMk2MaxVoices) continue;
                    /* A lane still sounding this key is not a second voice
                     * for it: one engine note per key. */
                    if (lane_on_[voice] && lane_key_[voice] == key) continue;

                    const float now = ctxt->pitch[i];
                    /* A sounding voice's expected pitch keeps its bend; a
                     * released one starts from the key. */
                    const float expected =
                        lane_on_[voice]
                            ? static_cast<float>(key) + lane_pitch_[voice] -
                                  static_cast<float>(lane_key_[voice])
                            : static_cast<float>(key);
                    float score = now - expected;
                    if (score < 0) score = -score;
                    if (score < best_score) {
                        best_score = score;
                        best = voice;
                        best_pitch = now;
                    }
                }
                if (best >= 0)
                    startLane(static_cast<unsigned>(best), key, on_velocity[k],
                              best_pitch);
            }
        }

        /* A voice that is still triggered follows its pitch, sounding or not:
         * a latched arpeggiator keeps the bit up after the keys are released
         * and only moves the pitch. */
        if (ctxt) {
            for (unsigned i = 0; i < limit; ++i) {
                const int voice = voiceIndex(ctxt, i);
                if (voice < 0 || voice >= kMk2MaxVoices) continue;
                const uint8_t bit = static_cast<uint8_t>(1u << voice);
                if (fresh & bit) continue;
                /* A lane that never started has no key to follow: a trigger
                 * bit left up for a key released inside its block must not
                 * start it from pitch later. */
                if (!lane_on_[voice] &&
                    (lane_key_[voice] == 0xff || !(ctxt->trigger & bit)))
                    continue;
                followPitch(static_cast<unsigned>(voice), ctxt->pitch[i]);
            }
        }

        /*
         * Make the part agree with the lanes, whatever order the events came
         * in. A key the lanes want is started, one they no longer want is
         * stopped; nothing else is touched, so no key can be left sounding or
         * sent twice.
         */
        reconcileNotes();

        /* Nothing is sounding and the last tail has fallen: the engine is not
         * advanced, and the block is written as silence. */
        if (quiet_ && !anyLaneOn()) {
            limiter_.reset();
            silence(out, frames);
            return;
        }

#ifdef RG55_DEBUG
        const double started = rg55::seconds();
#endif
        const uint32_t offset = outputOffset(ctxt);
        const size_t size = frames * static_cast<size_t>(output_channels_);
        const size_t last = frames ? offset + (frames - 1) * stride + 1 : offset;
        if (frames == 0 || stride < 2 || last >= size) {
            silence(out, frames);
        } else {
            probeUnusedLanes(out, frames, offset, stride, size, limit);
            float* const first = out + offset;
            float peak = 0.f;
            for (size_t f = 0; f < frames; ++f) {
                float left = 0.f;
                float right = 0.f;
                synth_->get_next_frame(left, right);
                left *= k_output_gain;
                right *= k_output_gain;
                limiter_.pair(left, right);
                first[f * stride] = left;
                first[f * stride + 1] = right;
                const float l = left < 0 ? -left : left;
                const float r = right < 0 ? -right : right;
                if (l > peak) peak = l;
                if (r > peak) peak = r;
            }
#ifdef RG55_DEBUG
            if (peak > max_peak_) max_peak_ = peak;
#endif
            if (!anyLaneOn() && peak < k_quiet_level) {
                if (++quiet_blocks_ >= k_quiet_blocks && !quiet_) {
                    quiet_ = true;
                    /* The engine is not advanced from here, so a release that
                     * has not finished would never finish and would come back
                     * with the next note. The tails are already below the
                     * threshold: drop them. */
                    synth_->panic();
                    /* The phrase is over: write the numbers it gathered. */
                    logVoiceStats("idle");
                }
            } else {
                quiet_blocks_ = 0;
                quiet_ = false;
            }
        }

#ifdef RG55_DEBUG
        /* A block that takes longer than it lasts is a block the host had to
         * wait for. The render loop alone is not the block: the note work
         * before it is timed separately, because a chord arrives as several
         * note-ons in one block. */
        const double finished = rg55::seconds();
        const double ms = (finished - started) * 1000.0;
        const double pre_ms = (started - block_started) * 1000.0;
        const double total_ms = (finished - block_started) * 1000.0;
        const double deadline = frames / static_cast<double>(sample_rate_) * 1000.0;
        if (total_ms > max_block_ms_) max_block_ms_ = total_ms;
        if (total_ms > deadline) ++late_blocks_;

        unsigned active = 0;
        for (unsigned i = 0; i < limit; ++i) {
            const int voice = voiceIndex(ctxt, i);
            if (voice >= 0 && voice < static_cast<int>(kMk2MaxVoices) &&
                lane_on_[voice])
                ++active;
        }
        ++blocks_by_voices_[active];
        /* The gap belongs to the block that ran before this one. */
        if (gap_ms >= 0.0 && previous_active_ <= kMk2MaxVoices) {
            double& lo = min_gap_by_voices_[previous_active_];
            if (lo == 0.0 || gap_ms < lo) lo = gap_ms;
            if (gap_ms > max_gap_by_voices_[previous_active_])
                max_gap_by_voices_[previous_active_] = gap_ms;
        }
        previous_active_ = active;
        if (ms > max_render_by_voices_[active])
            max_render_by_voices_[active] = ms;
        if (pre_ms > max_pre_by_voices_[active])
            max_pre_by_voices_[active] = pre_ms;
        if (total_ms > max_total_by_voices_[active])
            max_total_by_voices_[active] = total_ms;
        if (total_ms > deadline) ++late_by_voices_[active];
        /* The block where a new voice count first appears is the block that
         * is suspected of killing the host's audio, so print it as it happens
         * rather than waiting for a flush that may never come. */
        if (active > logged_active_) {
            logged_active_ = active;
            rg55::log("voice: %u sounding, render %.2fms, notes %.2fms, "
                      "total %.2fms, gap %.2fms",
                      active, ms, pre_ms, total_ms, gap_ms);
        }
        if (total_ms > deadline && late_logs_ < 8) {
            ++late_logs_;
            rg55::log("late: %u sounding, render %.2fms, notes %.2fms, "
                      "total %.2fms, deadline %.2fms, gap %.2fms",
                      active, ms, pre_ms, total_ms, deadline, gap_ms);
        }
        /* Nothing is written to the card from here. The counters are dumped
         * at teardown, off the audio thread: a flash write can stall this
         * block, and a log line every time the last tail falls is exactly
         * repeated play. */
#endif
        ++render_blocks_;
    }

    void setParameter(uint8_t index, int32_t value) {
        switch (index) {
        case PARAM_PROGRAM:
            /* Applied by the render thread. set_part_instrument() rewrites the
             * part's settings and the runtime's parameter thread is not the
             * one walking them, so doing it here races the engine mid-block. */
            program_ = clampMidi(value);
            program_dirty_ = true;
            break;
        default:
            break;
        }
    }

    int32_t getParameterValue(uint8_t index) const {
        return index == PARAM_PROGRAM ? program_.load() : 0;
    }

    /*
     * What the PROGRAM row prints instead of its number. The instrument names
     * are the control ROM's own, resolved the way Part::set_program() resolves
     * them, so the name shown is the one that sounds. This control ROM has no
     * usable variation banks, which is why there is no BANK row.
     */
    const char* getParameterStrValue(uint8_t index, int32_t value) {
        if (index != PARAM_PROGRAM) return nullptr;
        /*
         * With no ROM there are no names to show, and a number would read as a
         * name that failed to arrive. "---" says the row has nothing. This
         * never builds the ROM: unit_init already did, and a display refresh
         * must not turn into a multi-second ROM read.
         */
        if (!control_rom_) return "---";
        return programName(clampMidi(value));
    }
    const uint8_t* getParameterBmpValue(uint8_t, int32_t) const { return nullptr; }

    /*
     * Take the events the callbacks left. Note-ons are kept in arrival order
     * so they can be paired with the voices the context says started; a
     * note-off names the key, and the voices that took that key are released.
     * The gates carry no key on a pitched unit, so they only carry velocity.
     */
    inline void drainNoteQueue(uint8_t* on_key, uint8_t* on_velocity,
                               unsigned& on_count, uint8_t* off_key,
                               unsigned& off_count, bool& panic_requested) {
        rfx::UnitNoteMessage msg;
        while (events_.pop(msg)) {
            switch (msg.type) {
            case rfx::kUnitNoteOn:
                if (msg.velocity == 0) {
                    if (off_count < 64) off_key[off_count++] = msg.note;
                    key_last_[msg.note] = kLastOff;
                } else {
                    if (on_count < 64) {
                        on_key[on_count] = msg.note;
                        on_velocity[on_count] = msg.velocity;
                        ++on_count;
                    }
                    key_last_[msg.note] = kLastOn;
                    last_velocity_ = msg.velocity;
                }
                break;
            case rfx::kUnitNoteOff:
                if (off_count < 64) off_key[off_count++] = msg.note;
                key_last_[msg.note] = kLastOff;
                break;
            case rfx::kUnitGateOn:
                last_velocity_ = msg.velocity;
                break;
            case rfx::kUnitGateOff:
                break;
            case rfx::kUnitAllNoteOff:
                panic_requested = true;
                break;
            }
        }
    }

    /*
     * The runtime calls these from its MIDI handling and its step sequencer,
     * never from the thread that renders, so each one only records its event.
     * Applying it here would touch the engine while Render() is walking it,
     * which is heard as a note that does not sound. Render() takes the events
     * out at the top of a block, in order.
     */
    inline void NoteOn(uint8_t note, uint8_t velocity) {
        ++note_events_;
        events_.push(rfx::kUnitNoteOn, note, velocity);
    }
    inline void NoteOff(uint8_t note) {
        ++note_events_;
        events_.push(rfx::kUnitNoteOff, note, 0);
    }
    inline void GateOn(uint8_t velocity) {
        ++note_events_;
        events_.push(rfx::kUnitGateOn, 0, velocity);
    }
    inline void GateOff() {
        ++note_events_;
        events_.push(rfx::kUnitGateOff, 0, 0);
    }
    inline void AllNoteOff() {
        ++note_events_;
        events_.push(rfx::kUnitAllNoteOff, 0, 0);
    }

    void PitchBend(uint16_t) {
        /* Deprecated, and redundant: the host bakes bend into pitch. */
    }

    void ChannelPressure(uint8_t) {}
    void Aftertouch(uint8_t, uint8_t) {}
    void LoadPreset(uint8_t) {}
    uint8_t getPresetIndex() const { return 0; }
    static const char* getPresetName(uint8_t) { return nullptr; }

private:
    static uint8_t clampMidi(int32_t value) {
        return static_cast<uint8_t>(value < 0 ? 0 : value > 127 ? 127 : value);
    }

    static uint8_t noteFromPitch(float pitch) {
        if (!(pitch > 0.f)) return 60;
        const int note = static_cast<int>(pitch + 0.5f);
        return static_cast<uint8_t>(note < 0 ? 0 : note > 127 ? 127 : note);
    }

    /*
     * Each host voice is a part on its own channel, so two voices can sound
     * the same key: that is what unison is. These keep every part in step
     * with the lane it belongs to, one note at a time.
     */
    void noteOn(unsigned lane, uint8_t key, uint8_t velocity) {
        if (note_key_[lane] == key) return;
        if (note_key_[lane] != 0xff) noteOff(lane);
        synth_->midi_input(static_cast<uint8_t>(0x90 | lane), key, velocity);
        note_key_[lane] = key;
#ifdef RG55_DEBUG
        rg55::log("part %u: on %u v %u", lane, key, velocity);
#endif
    }

    void noteOff(unsigned lane) {
        if (note_key_[lane] == 0xff) return;
        synth_->midi_input(static_cast<uint8_t>(0x80 | lane), note_key_[lane], 0);
        note_key_[lane] = 0xff;
#ifdef RG55_DEBUG
        rg55::log("part %u: off", lane);
#endif
    }

    void retrigger(unsigned lane, uint8_t key, uint8_t velocity) {
        noteOff(lane);
        noteOn(lane, key, velocity);
    }

    /* A host voice takes a key: whatever it held is over. */
    void startLane(unsigned lane, uint8_t key, uint8_t velocity, float pitch) {
        const bool was_on = lane_on_[lane];
        const uint8_t old = lane_key_[lane];
        lane_key_[lane] = key;
        lane_pitch_[lane] = pitch;
        lane_on_[lane] = true;
        last_velocity_ = velocity;

        /* The same lane pressed again, or a key released and pressed within
         * this block, is a new attack on a note the part may still hold. */
        if (!attacked_[key] && ((was_on && old == key) || released_[key])) {
            retrigger(lane, key, velocity);
            attacked_[key] = true;
#ifdef RG55_DEBUG
            rg55::log("lane %u: retrigger %u (was %u on %d)", lane, key, old,
                      was_on ? 1 : 0);
#endif
        } else if (note_key_[lane] != key) {
            noteOn(lane, key, velocity);
            attacked_[key] = true;
        }
        bendTo(lane, pitch - static_cast<float>(key));
    }

    /* Whether a host voice is still playing this key. */
    bool laneHolds(uint8_t key) const {
        for (unsigned v = 0; v < kMk2MaxVoices; ++v) {
            if (lane_on_[v] && lane_key_[v] == key) return true;
        }
        return false;
    }

    /*
     * Whether a host voice is still playing the key, on the lane that played
     * it last. A block that carries a note-off after the note-on is a tap
     * only when no such voice exists; when one does, the off belongs to the
     * copy the new press replaces, and the press is a new attack.
     */
    bool hostHolds(const unit_runtime_osc_context_t* ctxt,
                   unsigned limit, uint8_t key) const {
        if (!ctxt) return false;
        for (unsigned i = 0; i < limit; ++i) {
            const int voice = voiceIndex(ctxt, i);
            if (voice < 0 || voice >= kMk2MaxVoices) continue;
            if (lane_key_[voice] != key) continue;
            if (!(ctxt->trigger & static_cast<uint8_t>(1u << voice))) continue;
            if (noteFromPitch(ctxt->pitch[i]) == key) return true;
        }
        return false;
    }

    /*
     * Every host voice holding the key lets it go. The key it played is kept
     * on the lane: a latched arpeggiator leaves the voice triggered and only
     * moves the pitch, so the next step has to find the lane it belongs to.
     */
    void releaseKey(uint8_t key) {
        bool any = false;
        for (unsigned v = 0; v < kMk2MaxVoices; ++v) {
            if (!lane_on_[v] || lane_key_[v] != key) continue;
            lane_on_[v] = false;
            /* Every voice holding the key lets its own part go, so a unison
             * key stops all of its copies. */
            noteOff(v);
            any = true;
        }
        if (any) released_[key] = true;
    }

    /*
     * A voice's pitch, as the host moves it. A jump to another key is the
     * arpeggiator stepping (or a transposed voice), and the note is restarted
     * there; anything smaller, or a jump that does not land on a key, is the
     * wheel, which is sent to the part as a bend so the note keeps its
     * envelope. A lane that has been released still follows its pitch: that
     * is how a latched arpeggiator sounds the next step after the keys are up.
     */
    void followPitch(unsigned lane, float pitch) {
        const float last = lane_pitch_[lane];
        lane_pitch_[lane] = pitch;
        const float delta = pitch - last;
        if (delta == 0.f) return;

        const bool on = lane_on_[lane];
        const uint8_t old = lane_key_[lane];
        const uint8_t note = noteFromPitch(pitch);
        const float landing = pitch - static_cast<float>(note);
        const bool jump = delta <= -k_step || delta >= k_step;
        const bool keyed = jump && note != old && landing > -k_land &&
                           landing < k_land;

        if (!keyed) {
            if (on && old != 0xff) bendTo(lane, pitch - static_cast<float>(old));
            return;
        }

        lane_key_[lane] = note;
        lane_on_[lane] = true;
        if (released_[note]) {
            retrigger(lane, note, last_velocity_);
        } else if (note_key_[lane] != note) {
            noteOn(lane, note, last_velocity_);
        }
        bendTo(lane, pitch - static_cast<float>(note));
    }

    /*
     * Every part agrees with its lane after the block, whatever order the
     * events came in: a lane's key sounds, and a lane that let go stops.
     */
    void reconcileNotes() {
        for (unsigned v = 0; v < kMk2MaxVoices; ++v) {
            if (lane_on_[v] && lane_key_[v] != 0xff) {
                if (note_key_[v] != lane_key_[v])
                    noteOn(v, lane_key_[v], last_velocity_);
            } else if (note_key_[v] != 0xff) {
                noteOff(v);
            }
        }
    }

    void bendTo(unsigned lane, float semitones) {
        if (semitones > k_bend_range) semitones = k_bend_range;
        if (semitones < -k_bend_range) semitones = -k_bend_range;
        if (semitones > bend_[lane] - k_bend_epsilon &&
            semitones < bend_[lane] + k_bend_epsilon)
            return;
        bend_[lane] = semitones;
        int value = static_cast<int>(8192.0f +
                                     semitones / k_bend_range * 8192.0f);
        if (value < 0) value = 0;
        if (value > 16383) value = 16383;
        synth_->midi_input(static_cast<uint8_t>(0xe0 | lane),
                           static_cast<uint8_t>(value & 0x7f),
                           static_cast<uint8_t>((value >> 7) & 0x7f));
#ifdef RG55_DEBUG
        rg55::log("part %u: bend %.3f (%d)", lane, semitones, value);
#endif
    }

    void panic() {
        if (synth_) synth_->panic();
        clearLanes();
        last_trigger_ = 0;
    }

    void clearLanes() {
        std::memset(lane_key_, 0xff, sizeof(lane_key_));
        std::memset(lane_pitch_, 0, sizeof(lane_pitch_));
        std::memset(lane_on_, 0, sizeof(lane_on_));
        std::memset(note_key_, 0xff, sizeof(note_key_));
        std::memset(released_, 0, sizeof(released_));
        std::memset(attacked_, 0, sizeof(attacked_));
        std::memset(key_last_, kLastNone, sizeof(key_last_));
        std::memset(bend_, 0, sizeof(bend_));
        limiter_.reset();
        quiet_ = false;
        quiet_blocks_ = 0;
    }

    bool anyLaneOn() const {
        for (unsigned v = 0; v < kMk2MaxVoices; ++v)
            if (lane_on_[v]) return true;
        return false;
    }

    const unit_runtime_osc_context_t* oscContext() const {
        return static_cast<const unit_runtime_osc_context_t*>(
            runtime_desc_.hooks.runtime_context);
    }

    /*
     * Where this unit's lane pair starts. A host that hands no context gives
     * the block as plain interleaved stereo from its start.
     */
    static uint32_t outputOffset(const unit_runtime_osc_context_t* ctxt) {
        return ctxt ? ctxt->bufferOffset : 0;
    }

    /*
     * How many streams are interlaced there. A host that hands no context
     * gives the block as plain interleaved stereo from its start, which is
     * offset 0 and stride 2.
     */
    static uint32_t outputStride(const unit_runtime_osc_context_t* ctxt) {
        return (ctxt && ctxt->outputStride) ? ctxt->outputStride : 2;
    }

    static unsigned voiceLimit(const unit_runtime_osc_context_t* ctxt) {
        if (!ctxt) return 1;
        const unsigned limit = static_cast<unsigned>(ctxt->voiceLimit);
        const unsigned most = static_cast<unsigned>(kMk2MaxVoices);
        return limit < most ? limit : most;
    }

    /* The voice in the host's numbering: its trigger bit. */
    static int voiceIndex(const unit_runtime_osc_context_t* ctxt, unsigned i) {
        if (!ctxt) return static_cast<int>(i);
        return static_cast<int>(ctxt->voiceOffset) + static_cast<int>(i);
    }

    void silence(float* out, size_t frames) {
        const unit_runtime_osc_context_t* ctxt = oscContext();
        const uint32_t stride = outputStride(ctxt);
        const size_t size = frames * static_cast<size_t>(output_channels_);
        float* const first = out + outputOffset(ctxt);
        for (size_t f = 0; f < frames; ++f) {
            const size_t at = f * stride;
            if (at >= size) break;
            first[at] = 0.f;
            if (stride >= 2 && at + 1 < size) first[at + 1] = 0.f;
        }
    }

#ifdef RG55_DEBUG
    /*
     * This unit puts one finished stereo pair in the first voice's lanes and
     * says nothing to the rest. The SDK oscillators write every lane in the
     * context, so it matters whether the host clears the buffer or leaves the
     * last block's contents there. Read the lanes before they are overwritten.
     */
    void probeUnusedLanes(const float* out, size_t frames, uint32_t offset,
                          uint32_t stride, size_t size, unsigned limit) {
        if (lane_probe_logs_ >= 8) return;
        float worst = 0.f;
        size_t at = 0;
        for (size_t f = 0; f < frames; ++f) {
            const size_t base = offset + f * stride;
            for (uint32_t lane = 2; lane < stride; ++lane) {
                if (base + lane >= size) break;
                const float v = out[base + lane];
                const float a = v < 0 ? -v : v;
                if (a > worst) { worst = a; at = base + lane; }
            }
        }
        if (limit > 4) {
            const size_t second = offset + frames * stride;
            for (size_t i = second; i < size; ++i) {
                const float v = out[i];
                const float a = v < 0 ? -v : v;
                if (a > worst) { worst = a; at = i; }
            }
        }
        if (worst > 1e-6f) {
            ++lane_probe_logs_;
            rg55::log("lanes: %u voices, stride %u, unused lane max %.6f at %u",
                      limit, stride, worst, static_cast<unsigned>(at));
        }
    }
#else
    void probeUnusedLanes(const float*, size_t, uint32_t, uint32_t, size_t,
                          unsigned) {}
#endif

    /*
     * VERY IMPORTANT
     * ARM computes denormal floats by default and each one costs far more than
     * a normal operation. FPSCR.FZ applies to the calling thread, which is why
     * this runs from Render and not from init.
     */
    static void enableFlushToZero() {
#if defined(__arm__)
        uint32_t fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#ifdef RG55_DEBUG
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        rg55::log("fpscr: fz %u, dn %u", (fpscr >> 24) & 1u, (fpscr >> 25) & 1u);
#endif
#endif
    }

    /*
     * The host can call the unit once per block for all eight voices, or once
     * per four-voice group with a different base offset. This prints each
     * distinct layout once, with the trigger byte, so the log says which one
     * the host is using and whether the trigger bits are re-based per call.
     */
    void logLayout(const unit_runtime_osc_context_t* ctxt, unsigned limit,
                   uint32_t stride) {
#ifdef RG55_DEBUG
        if (!ctxt) return;
        for (unsigned i = 0; i < seen_layout_count_; ++i) {
            Layout& s = seen_layouts_[i];
            if (s.base == ctxt->bufferOffset && s.limit == limit &&
                s.offset == ctxt->voiceOffset && s.stride == stride) {
                if (!s.trigger_seen && ctxt->trigger) {
                    s.trigger_seen = true;
                    rg55::log("ctx trigger: 0x%02x, pitch0 %.2f",
                              static_cast<unsigned>(ctxt->trigger),
                              ctxt->pitch[0]);
                }
                return;
            }
        }
        if (seen_layout_count_ < 8) {
            Layout& s = seen_layouts_[seen_layout_count_++];
            s.base = ctxt->bufferOffset;
            s.limit = static_cast<uint8_t>(limit);
            s.offset = ctxt->voiceOffset;
            s.stride = static_cast<uint16_t>(stride);
            s.trigger_seen = ctxt->trigger != 0;
        }
        rg55::log("ctx: limit %u, offset %u, base %u, stride %lu, trigger 0x%02x, "
                  "pitch0 %.2f",
                  limit, static_cast<unsigned>(ctxt->voiceOffset),
                  static_cast<unsigned>(ctxt->bufferOffset),
                  static_cast<unsigned long>(stride),
                  static_cast<unsigned>(ctxt->trigger), ctxt->pitch[0]);
#else
        (void) ctxt;
        (void) limit;
        (void) stride;
#endif
    }

    /*
     * What the host says about the voices, whenever it changes. The layout log
     * above only prints a layout once; this prints every trigger and pitch
     * change, which is what says whether a lifted key clears its bit, whether
     * the pitch of a freed voice moves, and whether a new chord lands on the
     * voices the old one used.
     */
    void logHostState(const unit_runtime_osc_context_t* ctxt, unsigned limit) {
#ifdef RG55_DEBUG
        if (!ctxt) return;
        bool changed = ctxt->trigger != logged_trigger_ ||
                       static_cast<uint8_t>(limit) != logged_limit_ ||
                       ctxt->voiceOffset != logged_offset_;
        for (unsigned i = 0; i < kMk2MaxVoices && !changed; ++i)
            if (ctxt->pitch[i] != logged_pitch_[i]) changed = true;
        if (!changed) return;
        logged_trigger_ = ctxt->trigger;
        logged_limit_ = static_cast<uint8_t>(limit);
        logged_offset_ = ctxt->voiceOffset;
        for (unsigned i = 0; i < kMk2MaxVoices; ++i)
            logged_pitch_[i] = ctxt->pitch[i];
        rg55::log("host: trigger 0x%02x limit %u offset %u pitch %.2f %.2f %.2f "
                  "%.2f %.2f %.2f %.2f %.2f",
                  static_cast<unsigned>(ctxt->trigger), limit,
                  static_cast<unsigned>(ctxt->voiceOffset),
                  ctxt->pitch[0], ctxt->pitch[1], ctxt->pitch[2],
                  ctxt->pitch[3], ctxt->pitch[4], ctxt->pitch[5],
                  ctxt->pitch[6], ctxt->pitch[7]);
#else
        (void) ctxt;
        (void) limit;
#endif
    }

    void logSummary(const char* when) {
#ifdef RG55_DEBUG
        rg55::log("%s: %lu blocks, %lu events, max %.2fms, late %lu, peak %.4f, "
                  "gap max %.2fms, long %lu",
                  when, static_cast<unsigned long>(render_blocks_),
                  static_cast<unsigned long>(note_events_), max_block_ms_,
                  static_cast<unsigned long>(late_blocks_), max_peak_,
                  max_gap_ms_, static_cast<unsigned long>(long_gaps_));
        logVoiceStats(when);
#else
        (void) when;
#endif
    }

    /* The per voice-count numbers, and a reset so they are not written twice. */
    void logVoiceStats(const char* when) {
#ifdef RG55_DEBUG
        for (unsigned v = 0; v <= kMk2MaxVoices; ++v) {
            if (!blocks_by_voices_[v]) continue;
            rg55::log("%s: %u sounding: %lu blocks, render %.2fms, notes %.2fms, "
                      "total %.2fms, late %lu, gap %.2f..%.2fms",
                      when, v,
                      static_cast<unsigned long>(blocks_by_voices_[v]),
                      max_render_by_voices_[v], max_pre_by_voices_[v],
                      max_total_by_voices_[v],
                      static_cast<unsigned long>(late_by_voices_[v]),
                      min_gap_by_voices_[v], max_gap_by_voices_[v]);
            blocks_by_voices_[v] = 0;
            max_render_by_voices_[v] = 0.0;
            max_pre_by_voices_[v] = 0.0;
            max_total_by_voices_[v] = 0.0;
            late_by_voices_[v] = 0;
            min_gap_by_voices_[v] = 0.0;
            max_gap_by_voices_[v] = 0.0;
        }
#else
        (void) when;
#endif
    }

    /*
     * The name of the instrument that a program resolves to at bank 0, which
     * is the bank the unit always uses.
     */
    const char* programName(uint8_t program) {
        const uint16_t instrument = control_rom_->variation(0)[program];
        if (instrument == 0xffff ||
            instrument >= static_cast<uint16_t>(control_rom_->numInstruments()))
            return "---";

        static char name[20];
        std::snprintf(name, sizeof name, "%s",
                      control_rom_->instrument(instrument).name.c_str());
        return name;
    }

    /*
     * Take a reference on the shared ROMs, building them on the first call.
     */
    bool loadRoms() {
        std::lock_guard<std::mutex> guard(rg55_roms::lock);
        if (!rg55_roms::cache.control) {
            rg55::log("engine: reading ROMs");
            const double started = rg55::seconds();
            try {
                EmuSC::ControlRom* control =
                    new EmuSC::ControlRom(k_sc55_control_rom, k_sc55_cpu_rom);
                EmuSC::WaveRom* wave = nullptr;
                try {
                    wave = new EmuSC::WaveRom(
                        std::vector<std::string>(k_sc55_wave_roms,
                                                 k_sc55_wave_roms + 3),
                        *control);
                } catch (...) {
                    delete control;
                    throw;
                }
                rg55_roms::cache.control = control;
                rg55_roms::cache.wave = wave;
                rg55_roms::cache.users = 0;
                rg55::log("engine: ROMs ready, %s %s, %.3fs",
                          control->model().c_str(), control->version().c_str(),
                          rg55::seconds() - started);
            } catch (const std::string& error) {
                rg55::log("engine: ROMs failed after %.2fs: %s",
                          rg55::seconds() - started, error.c_str());
                return false;
            } catch (const std::exception& ex) {
                rg55::log("engine: ROMs failed after %.2fs: %s",
                          rg55::seconds() - started, ex.what());
                return false;
            }
        }
        control_rom_ = rg55_roms::cache.control;
        wave_rom_ = rg55_roms::cache.wave;
        ++rg55_roms::cache.users;
        return true;
    }

    void releaseRoms() {
        if (!control_rom_) return;
        std::lock_guard<std::mutex> guard(rg55_roms::lock);
        control_rom_ = nullptr;
        wave_rom_ = nullptr;
        if (--rg55_roms::cache.users > 0) return;
        delete rg55_roms::cache.wave;
        delete rg55_roms::cache.control;
        rg55_roms::cache.control = nullptr;
        rg55_roms::cache.wave = nullptr;
    }

    /*
     * One engine for the timbre. The part plays in poly mode so the host's
     * voices are its notes, and the bend range is opened to the range the
     * host's wheel can reach.
     */
    bool buildSynth() {
        rg55::log("engine: building");
        const double started = rg55::seconds();
        try {
            synth_ = new EmuSC::Synth(*control_rom_, *wave_rom_);
            synth_->set_audio_format(sample_rate_, channels_);
            const uint8_t program = program_.load(std::memory_order_relaxed);
            /* One part per host voice, each on its own channel: two voices
             * sounding the same key is unison. */
            for (unsigned v = 0; v < kMk2MaxVoices; ++v) {
                synth_->set_param(EmuSC::PatchParam::PolyMode,
                                  static_cast<uint8_t>(1),
                                  static_cast<int8_t>(v));
                synth_->set_param(EmuSC::PatchParam::PB_PitchControl,
                                  static_cast<uint8_t>(0x40 +
                                                       static_cast<int>(k_bend_range)),
                                  static_cast<int8_t>(v));
                synth_->set_part_instrument(static_cast<uint8_t>(v), program, 0);
            }
            rg55::log("engine: ready, %u %s, %.2fs",
                      static_cast<unsigned>(program), programName(program),
                      rg55::seconds() - started);
            return true;
        } catch (const std::string& error) {
            rg55::log("engine: failed after %.2fs: %s", rg55::seconds() - started,
                      error.c_str());
        } catch (const std::exception& ex) {
            rg55::log("engine: failed after %.2fs: %s", rg55::seconds() - started,
                      ex.what());
        }
        delete synth_;
        synth_ = nullptr;
        init_failed_ = true;
        return false;
    }

    /*
     * set_part_instrument() rewrites the part's settings, so it runs on the
     * render thread, where the part is not being walked.
     */
    void applyProgram() {
        const uint8_t program = program_.load(std::memory_order_relaxed);
        rg55::log("program: %u %s", static_cast<unsigned>(program),
                  programName(program));
        if (synth_) {
            for (unsigned v = 0; v < kMk2MaxVoices; ++v)
                synth_->set_part_instrument(static_cast<uint8_t>(v), program, 0);
        }
    }

    EmuSC::ControlRom* control_rom_;
    EmuSC::WaveRom* wave_rom_;
    EmuSC::Synth* synth_;
    uint32_t sample_rate_;
    uint8_t channels_;
    unsigned output_channels_;
    bool init_failed_;
    bool flush_to_zero_;
    bool torn_down_ = false;
    /* Set by the callbacks, acted on by the render thread. */
    bool panic_requested_;
    bool program_dirty_;
    std::atomic<uint8_t> program_;
    /* The trigger byte the last block saw; a new note is a fresh bit. */
    uint8_t last_trigger_;
    /* The last velocity the callbacks carried, for a trigger that arrives
     * without one of its own. */
    uint8_t last_velocity_;
    /* Per host voice: the key it took, the pitch the context gave it, and
     * whether it is still sounding. */
    uint8_t lane_key_[kMk2MaxVoices];
    float lane_pitch_[kMk2MaxVoices];
    bool lane_on_[kMk2MaxVoices];
    /* Per host voice: the key its part is playing (0xff for none). */
    uint8_t note_key_[kMk2MaxVoices];
    /* Per block, from the callbacks: the last event for a key, and which keys
     * were released or already attacked in this block. */
    uint8_t key_last_[128];
    bool released_[128];
    bool attacked_[128];
    /* The bend last sent to each part, in semitones. */
    float bend_[kMk2MaxVoices];
    OutputLimiter limiter_;
    /* No host voice sounding, and the tail has fallen below the quiet level
     * for long enough that the engine can be left alone. */
    bool quiet_;
    unsigned quiet_blocks_;
    uint32_t render_blocks_;
    uint32_t note_events_;
    double max_block_ms_;
    unsigned long late_blocks_;
    float max_peak_;
    /* Per number of voices that were sounding: blocks, worst render loop, worst
     * note work before it, worst whole block, late blocks. */
    uint32_t blocks_by_voices_[kMk2MaxVoices + 1] = {};
    double max_render_by_voices_[kMk2MaxVoices + 1] = {};
    double max_pre_by_voices_[kMk2MaxVoices + 1] = {};
    double max_total_by_voices_[kMk2MaxVoices + 1] = {};
    unsigned long late_by_voices_[kMk2MaxVoices + 1] = {};
    unsigned logged_active_;
    unsigned late_logs_;
    unsigned previous_active_;
    /* Time between two Render calls, and how often that stretched. */
    double last_entry_;
    double max_gap_ms_;
    unsigned long long_gaps_;
    /* How much of the block period was left over, per voice count: if the host
     * is running flat out the next call arrives as soon as this one returns. */
    double min_gap_by_voices_[kMk2MaxVoices + 1] = {};
    double max_gap_by_voices_[kMk2MaxVoices + 1] = {};
    unsigned lane_probe_logs_;
    unsigned bad_lane_logs_;
    struct Layout {
        uint16_t base;
        uint16_t stride;
        uint8_t limit;
        uint8_t offset;
        bool trigger_seen;
    };
    Layout seen_layouts_[8] = {};
    unsigned seen_layout_count_;
    /* What the host's context showed last, so logHostState logs changes only. */
    uint8_t logged_trigger_ = 0xff;
    uint8_t logged_limit_ = 0xff;
    uint8_t logged_offset_ = 0xff;
    float logged_pitch_[kMk2MaxVoices] = {};
    /* Snapshot of the descriptor, which is where the host keeps the context
     * pointer the block's layout is read from. */
    unit_runtime_desc_t runtime_desc_;
    /* Filled by the note callbacks, emptied by Render(). */
    rfx::UnitNoteQueue<64> events_;
};
