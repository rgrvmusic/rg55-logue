#pragma once
/**
 * RGSC55 - drumlogue synth unit
 *
 * A monophonic libEmuSC SC-55 player: part 0, in mono mode. A new note stops
 * the one sounding, and what is stopped is faded out over one block first,
 * because cutting it at whatever amplitude it has is a step in the waveform (a
 * tenth of full scale at 25 ms retriggers). A gate holds its note until the
 * gate ends.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "unit.h"
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
    PARAM_LEVEL,
    PARAM_NOTE,
};

/*
 * libEmuSC keeps the SC-55's own internal levels, which leave a single loud
 * patch at about -13 dBFS and an ordinary one 10 dB below that. This brings
 * the loudest patch at full velocity up to about -1.5 dBFS, so the part sits
 * in the mix like the other units.
 */
static constexpr float k_output_gain = 4.0f;

class Synth {
public:
    Synth()
        : control_rom_(nullptr)
        , wave_rom_(nullptr)
        , synth_(nullptr)
        , program_(0)
        , level_(0.8f)
        , note_(60)
        , active_note_(-1)
        , sample_rate_(48000)
        , channels_(2)
        , init_failed_(false)
        , flush_to_zero_(false)
        , panic_requested_(false)
        , program_dirty_(false)
        , fade_left_(0)
        , fade_length_(1)
        , held_count_(0)
        , gate_note_(60)
        , desired_note_(-1)
        , desired_velocity_(0)
        , render_blocks_(0)
        , note_events_(0)
        , max_block_ms_(0.0)
        , late_blocks_(0)
        , max_peak_(0.f)
    {}

    ~Synth() { Teardown(); }

    int8_t Init(const unit_runtime_desc_t* desc) {
        rg55::log("init: target=0x%04x api=0x%08x sr=%lu frames=%u in=%u out=%u",
                  static_cast<unsigned>(desc->target), desc->api,
                  static_cast<unsigned long>(desc->samplerate),
                  static_cast<unsigned>(desc->frames_per_buffer),
                  static_cast<unsigned>(desc->input_channels),
                  static_cast<unsigned>(desc->output_channels));
        logRoms();

        if (desc->samplerate != 48000) return k_unit_err_samplerate;
        if (desc->output_channels != 2) return k_unit_err_geometry;

        sample_rate_ = desc->samplerate;
        channels_ = desc->output_channels;

        /*
         * Build the engine here, not in the first render and not in the
         * display's name lookup. Reading 3.4 MB of ROM and constructing
         * libEmuSC takes about 18 MB and, on the drumlogue's Cortex-A7, long
         * enough that doing it inside an audio callback stalls the daemon.
         * unit_init is the one place where a unit is allowed to take its time.
         * A failure is latched: the unit still registers and stays silent.
         */
        ensureSynth();
        return k_unit_err_none;
    }

    void Teardown() {
        rg55::log("teardown");
        logSummary("teardown");
        delete synth_;
        synth_ = nullptr;
        delete wave_rom_;
        wave_rom_ = nullptr;
        delete control_rom_;
        control_rom_ = nullptr;
        active_note_ = -1;
    }

    void Reset() {
        rg55::log("reset");
        /* panic() frees notes, so it belongs on the render thread too. */
        panic_requested_ = true;
        fade_left_ = 0;
        held_count_ = 0;
        desired_note_ = -1;
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
         * so from the code that renders. A throw that leaves unit_render
         * unwinds through the daemon's C code, which ends the process. Catch it
         * here and the block is silence instead.
         */
        try {
            renderBlock(out, frames);
        } catch (const std::string& error) {
            rg55::log("render: %s", error.c_str());
            __builtin_memset(out, 0, frames * 2 * sizeof(float));
        } catch (const std::exception& ex) {
            rg55::log("render: %s", ex.what());
            __builtin_memset(out, 0, frames * 2 * sizeof(float));
        }
    }

    void renderBlock(float* out, size_t frames) {
        if (!flush_to_zero_) {
            enableFlushToZero();
            flush_to_zero_ = true;
        }

        drainNoteQueue();

        if (!ensureSynth()) {
            __builtin_memset(out, 0, frames * 2 * sizeof(float));
            return;
        }

        if (panic_requested_) {
            panic_requested_ = false;
            synth_->panic();
            active_note_ = -1;
            fade_left_ = 0;
        }
        if (program_dirty_) {
            program_dirty_ = false;
            applyProgram();
        }
        /* With nothing sounding there is nothing to fade: the note the keys
         * ask for starts in this block. With something sounding, it is faded
         * over this block first and the switch happens below. */
        if (desired_note_ != active_note_) {
            if (active_note_ < 0 || frames == 0) {
                switchNote();
            } else if (fade_left_ == 0) {
                fade_left_ = fade_length_ = static_cast<int>(frames);
            }
        }

        const float gain = level_ * k_output_gain;
#ifdef RG55_DEBUG
        const double started = rg55::seconds();
#endif
        for (size_t i = 0; i < frames; ++i) {
            float envelope = 1.f;
            if (fade_left_ > 0) {
                envelope = static_cast<float>(fade_left_ - 1) /
                           static_cast<float>(fade_length_);
                --fade_left_;
            }
            float left = 0.f;
            float right = 0.f;
            synth_->get_next_frame(left, right);
            out[i * 2] = left * gain * envelope;
            out[i * 2 + 1] = right * gain * envelope;
#ifdef RG55_DEBUG
            const float magnitude = out[i * 2] < 0 ? -out[i * 2] : out[i * 2];
            if (magnitude > max_peak_) max_peak_ = magnitude;
#endif
        }

        /* The faded-out note is only now dropped, so the engine's cut falls on
         * silence. */
        if (fade_left_ == 0 && desired_note_ != active_note_)
            switchNote();

#ifdef RG55_DEBUG
        /* A block that takes longer than it lasts is a block the daemon had to
         * wait for, which is heard as a crackle. Nothing is written from here:
         * the counters are read out when the unit is torn down, so measuring
         * does not put a flash write in the middle of the audio. */
        const double ms = (rg55::seconds() - started) * 1000.0;
        const double deadline = frames / static_cast<double>(sample_rate_) * 1000.0;
        if (ms > max_block_ms_) max_block_ms_ = ms;
        if (ms > deadline) ++late_blocks_;
#endif
        /* Taken apart at compile time when this is on, and the block count is
         * what the event log stamps itself with. */
        ++render_blocks_;
    }

    void setParameter(uint8_t index, int32_t value) {
        switch (index) {
        case PARAM_PROGRAM:
            /* Applied by the render thread. set_part_instrument() rewrites the
             * part's settings and the display's thread is not the one walking
             * them, so doing it here races the engine mid-block. */
            program_ = clampMidi(value);
            program_dirty_ = true;
            break;
        case PARAM_LEVEL:
            level_ = value < 0 ? 0.f : value > 100 ? 1.f : value / 100.f;
            break;
        case PARAM_NOTE:
            note_ = clampMidi(value);
            break;
        default:
            break;
        }
    }

    int32_t getParameterValue(uint8_t index) const {
        switch (index) {
        case PARAM_PROGRAM: return program_;
        case PARAM_LEVEL:   return static_cast<int32_t>(level_ * 100.f);
        case PARAM_NOTE:    return note_;
        default:            return 0;
        }
    }

    /*
     * What the PROGRAM row prints instead of its number. The instrument names
     * are the control ROM's own, resolved the way Part::set_program() resolves
     * them, so the name shown is the one that sounds. This control ROM has no
     * usable variation banks: only 8, 126 and 127 carry entries, and the rest
     * walk down to bank 0, which is why there is no BANK row.
     */
    const char* getParameterStrValue(uint8_t index, int32_t value) {
        if (index != PARAM_PROGRAM) return nullptr;
        /*
         * With no engine there are no names to show, and a number would read
         * as a name that failed to arrive. "---" says the row has nothing.
         * This never builds the engine: unit_init already did, and a display
         * refresh must not turn into a multi-second ROM read.
         */
        if (!synth_) return "---";
        return programName(clampMidi(value));
    }
    const uint8_t* getParameterBmpValue(uint8_t, int32_t) const { return nullptr; }

    /*
     * Take the events the callbacks left, oldest first, and apply them where
     * nothing else is touching the engine.
     */
    inline void drainNoteQueue() {
        rfx::UnitNoteMessage msg;
        while (events_.pop(msg)) {
          switch (msg.type) {
          case rfx::kUnitNoteOn:     processNoteOn(msg.note, msg.velocity); break;
          case rfx::kUnitNoteOff:    processNoteOff(msg.note); break;
          case rfx::kUnitGateOn:     processGate(msg.velocity); break;
          case rfx::kUnitGateOff:    processGateOff(); break;
          case rfx::kUnitAllNoteOff: processAllNoteOff(); break;
          }
        }
    }

    /*
     * The runtime calls these from its MIDI handling and its step sequencer,
     * never from the thread that renders, so each one only records its event.
     * Applying it here would touch the engine while Render() is walking it,
     * which is heard as a note that does not sound.  Render() takes the events
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

    /*
     * The keys decide which note the part should be playing; this only
     * records the key. The note that sounds is the newest key still held,
     * which is what the SC-55 does in mono mode.
     *
     * It has to be the keys and not the events, because an event and a key
     * are not the same thing: a host can send a burst of note-ons for one key
     * and the matching burst of note-offs, and overlapping notes put a
     * note-on and an older key's note-off in the same block. Working from the
     * last event alone, the note-off of a key that never got started is
     * dropped and the note-on that follows it is started after the key is up,
     * which is a note that sounds until the next one is played.
     */
    void processNoteOn(uint8_t note, uint8_t velocity) {
        if (!ensureSynth()) return;
        holdNote(note, velocity);
    }

    void processNoteOff(uint8_t note) {
        if (!synth_) return;
        releaseNote(note);
    }

    void processGate(uint8_t velocity) {
        if (!ensureSynth()) return;
        gate_note_ = note_;
        holdNote(gate_note_, velocity);
    }
    void processGateOff() {
        if (synth_) releaseNote(gate_note_);
    }
    void processAllNoteOff() { Reset(); }

    void PitchBend(uint16_t bend) {
        if (synth_)
            synth_->midi_input(0xe0, bend & 0x7f, (bend >> 7) & 0x7f);
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

    /* Hold a key, newest last. A key already down is not held twice. */
    void holdNote(uint8_t note, uint8_t velocity) {
        dropHeld(note);
        if (held_count_ == k_max_held) {
            for (uint8_t i = 0; i + 1 < held_count_; ++i) held_[i] = held_[i + 1];
            --held_count_;
        }
        held_[held_count_].note = note;
        held_[held_count_].velocity = velocity;
        ++held_count_;
        updateDesired();
    }

    void releaseNote(uint8_t note) {
        dropHeld(note);
        updateDesired();
    }

    void dropHeld(uint8_t note) {
        for (uint8_t i = 0; i < held_count_; ++i) {
            if (held_[i].note != note) continue;
            for (uint8_t j = i; j + 1 < held_count_; ++j) held_[j] = held_[j + 1];
            --held_count_;
            return;
        }
    }

    /* The newest key still held, or none. */
    void updateDesired() {
        if (held_count_ == 0) {
            desired_note_ = -1;
            return;
        }
        desired_note_ = held_[held_count_ - 1].note;
        desired_velocity_ = held_[held_count_ - 1].velocity;
    }

    /* Drop what is sounding and start what the keys ask for. Only ever called
     * on silence: either nothing was sounding, or the fade has just ended. */
    void switchNote() {
        if (active_note_ >= 0) {
            synth_->midi_input(0x80, static_cast<uint8_t>(active_note_), 0);
            active_note_ = -1;
        }
        if (desired_note_ >= 0) {
            synth_->midi_input(0x90, static_cast<uint8_t>(desired_note_),
                               desired_velocity_);
            active_note_ = desired_note_;
        }
    }

    /*
     * THIS IS VERY IMPORTANT:
     * ARM computes denormal floats by default and on the Cortex-A7 each one
     * costs far more than a normal operation. The reverb tail decays straight
     * through the denormal range and libEmuSC never flushes it, so a unit
     * sitting in silence is the slowest it gets. FPSCR.FZ applies to the
     * calling thread, which is why this runs from Render and not from init.
     */
    static void enableFlushToZero() {
#if defined(__arm__)
        uint32_t fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#endif
    }

    /*
     * What the render measured, written when the unit is not rendering. A line
     * from inside a render block is a flash write in the middle of the audio,
     * which is a crackle.
     */
    void logSummary(const char* when) {
#ifdef RG55_DEBUG
        rg55::log("%s: %lu blocks, %lu events, max %.2fms, late %lu, peak %.4f",
                  when, static_cast<unsigned long>(render_blocks_),
                  static_cast<unsigned long>(note_events_), max_block_ms_,
                  static_cast<unsigned long>(late_blocks_), max_peak_);
#else
        (void) when;
#endif
    }

    void logRoms() {
        rg55::log("roms: %s %ld bytes", k_sc55_cpu_rom, rg55::fileSize(k_sc55_cpu_rom));
        rg55::log("roms: %s %ld bytes", k_sc55_control_rom,
                  rg55::fileSize(k_sc55_control_rom));
        for (unsigned i = 0; i < 3; ++i)
            rg55::log("roms: %s %ld bytes", k_sc55_wave_roms[i],
                      rg55::fileSize(k_sc55_wave_roms[i]));
    }

    /*
     * The name of the instrument that a program resolves to at bank 0
     * The unit always uses bank 0!
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

    // Build the engine, or report the earlier failure. unit_init calls this;
    // the render and note paths call it too so they are safe if init did not
    // manage it. A failure is logged and latched so the unit stays silent
    // rather than crashing or retrying.
    bool ensureSynth() {
        if (synth_) return true;
        if (init_failed_) return false;

        rg55::log("engine: reading ROMs");
        const double started = rg55::seconds();
        try {
            control_rom_ = new EmuSC::ControlRom(k_sc55_control_rom, k_sc55_cpu_rom);
            wave_rom_ = new EmuSC::WaveRom(
                std::vector<std::string>(k_sc55_wave_roms,
                                         k_sc55_wave_roms + 3),
                *control_rom_);
            synth_ = new EmuSC::Synth(*control_rom_, *wave_rom_);
            synth_->set_audio_format(sample_rate_, channels_);
            /*
             * Mono.
             * A new note stops the one before it instead of layering a release tail
             * under it.
             * The drumlogue plays this unit one note at a time, and a release under
             * every retrigger is both a second voice to render
             */
            synth_->set_param(EmuSC::PatchParam::PolyMode, static_cast<uint8_t>(0),
                              static_cast<int8_t>(0));
            applyProgram();
            rg55::log("engine: ready, %s %s, %.2fs", control_rom_->model().c_str(),
                      control_rom_->version().c_str(), rg55::seconds() - started);
            return true;
        } catch (const std::string& error) {
            rg55::log("engine: failed after %.2fs: %s", rg55::seconds() - started,
                      error.c_str());
            std::fprintf(stderr, "RGSC55: %s\n", error.c_str());
        } catch (const std::exception& ex) {
            rg55::log("engine: failed after %.2fs: %s", rg55::seconds() - started,
                      ex.what());
            std::fprintf(stderr, "RGSC55: %s\n", ex.what());
        }
        Teardown();
        init_failed_ = true;
        return false;
    }

    void applyProgram() {
        if (synth_) synth_->set_part_instrument(0, program_, 0);
    }

    EmuSC::ControlRom* control_rom_;
    EmuSC::WaveRom* wave_rom_;
    EmuSC::Synth* synth_;
    uint8_t program_;
    float level_;
    uint8_t note_;
    int active_note_;
    uint32_t sample_rate_;
    uint8_t channels_;
    bool init_failed_;
    bool flush_to_zero_;
    /* Set by the callbacks, acted on by the render thread. */
    bool panic_requested_;
    bool program_dirty_;
    int fade_left_;
    int fade_length_;
    /* The keys the host says are down, oldest first, and the note they ask
     * for: the newest one. The gate is one of those keys. */
    struct HeldKey {
        uint8_t note;
        uint8_t velocity;
    };
    static constexpr uint8_t k_max_held = 16;
    HeldKey held_[k_max_held];
    uint8_t held_count_;
    uint8_t gate_note_;
    int desired_note_;
    uint8_t desired_velocity_;
    uint32_t render_blocks_;
    uint32_t note_events_;
    double max_block_ms_;
    unsigned long late_blocks_;
    float max_peak_;

    /* Filled by the note callbacks, emptied by Render(). */
    rfx::UnitNoteQueue<64> events_;
};
