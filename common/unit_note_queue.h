#pragma once
/**
 * Note events, handed from the thread that receives them to the thread that
 * renders.
 *
 *   rfx::UnitNoteQueue<64> queue_;
 *
 *   // in the note callbacks
 *   queue_.push(rfx::kUnitNoteOn, note, velocity);
 *
 *   // at the top of Render()
 *   rfx::UnitNoteMessage msg;
 *   while (queue_.pop(msg)) { ... }
 *
 * Single producer, single consumer: the runtime delivers its MIDI and its
 * sequencer events on one thread.  Nothing here allocates, locks, or throws, so
 * it is safe to use from a callback.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rfx {

enum UnitNoteEventType : uint8_t {
  kUnitNoteOn = 0,
  kUnitNoteOff,
  kUnitGateOn,
  kUnitGateOff,
  kUnitAllNoteOff
};

struct UnitNoteMessage {
  UnitNoteEventType type;
  uint8_t           note;
  uint8_t           velocity;
};

/*
 * A ring with one slot left free, which is what tells a full queue apart from
 * an empty one.  When it is full the event is dropped: an event that cannot fit
 * has arrived faster than the block that is being rendered, and the ones
 * already waiting are the older notes.
 */
template <unsigned Size>
class UnitNoteQueue {
  static_assert(Size >= 2 && Size <= 128,
                "one slot is kept free, and the index is a uint8_t");

 public:
  UnitNoteQueue() : write_(0), read_(0) {}

  void push(UnitNoteEventType type, uint8_t note = 0, uint8_t velocity = 0) {
    const uint8_t w    = write_.load(std::memory_order_relaxed);
    const uint8_t next = (uint8_t)((w + 1) % Size);

    if (next == read_.load(std::memory_order_acquire))
      return;

    messages_[w].type     = type;
    messages_[w].note     = note;
    messages_[w].velocity = velocity;

    /* Written before the index that publishes it. */
    write_.store(next, std::memory_order_release);
  }

  /* The next event, oldest first; false when there is none. */
  bool pop(UnitNoteMessage & out) {
    const uint8_t r = read_.load(std::memory_order_relaxed);

    if (r == write_.load(std::memory_order_acquire))
      return false;

    out = messages_[r];

    read_.store((uint8_t)((r + 1) % Size), std::memory_order_release);
    return true;
  }

  /*
   * Drop what is waiting, without touching what has already been applied.
   * This is what a reset uses: an event queued a moment before the unit was
   * reset belongs to the pattern that was just stopped.
   */
  void clear() {
    read_.store(write_.load(std::memory_order_acquire),
                std::memory_order_release);
  }

  bool empty() const {
    return read_.load(std::memory_order_relaxed) ==
           write_.load(std::memory_order_acquire);
  }

 private:
  UnitNoteMessage      messages_[Size];
  std::atomic<uint8_t> write_;
  std::atomic<uint8_t> read_;
};

}  // namespace rfx
