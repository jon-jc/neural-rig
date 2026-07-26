#pragma once

#include <atomic>
#include <memory>

#include "NamModel.h"

namespace nr::dsp
{

/**
    Holds the capture the audio thread is currently playing, and swaps in a new
    one without blocking or allocating.

    Loading a .nam reads a file, allocates a network and runs inference to
    prewarm it. None of that can happen on the audio thread. Equally, the old
    capture cannot be destroyed there — freeing a WaveNet's weights is an
    unbounded operation, and doing it mid-block is how a plugin earns a
    reputation for crackling when you change presets.

    So ownership moves in a cycle, and each pointer is only ever touched by one
    thread at a time:

        message thread   loads and prepares a NamModel
                         publishes it into `staged`
        audio thread     claims `staged`, makes it active, and hands the
                         displaced capture back through `retired`
        message thread   collects `retired` and destroys it

    The audio thread only ever exchanges pointers. It never allocates, never
    frees, and never waits.
*/
class ModelSlot
{
public:
    ModelSlot() = default;
    ~ModelSlot();

    ModelSlot(const ModelSlot&) = delete;
    ModelSlot& operator=(const ModelSlot&) = delete;

    /** Publishes a prepared capture for the audio thread to pick up. Message
        thread only.

        Passing nullptr is equivalent to requestClear(). Any capture displaced
        by an earlier call is collected here, so retirements never back up. */
    void stage(std::unique_ptr<NamModel> model);

    /** Asks the audio thread to stop playing whatever is loaded. Message
        thread only.

        This needs its own flag rather than staging a null pointer: an empty
        `staged` slot already means "nothing new to pick up", so there would be
        no way to tell "no change" apart from "go silent". */
    void requestClear();

    /** Destroys anything the audio thread has handed back. Call periodically
        from the message thread — a timer is ideal. Safe to call at any time. */
    void collectRetired();

    /** True if a staged capture is still waiting to be picked up. */
    bool hasPendingStage() const noexcept { return staged.load(std::memory_order_acquire) != nullptr; }

    /** Claims any staged capture and returns the one now in play, or nullptr
        if the slot is empty. Real-time safe; audio thread only. */
    NamModel* acquire() noexcept;

    /** The capture currently in play without checking for a staged swap.
        Audio thread only. */
    NamModel* current() const noexcept { return active; }

private:
    // Published by the message thread, claimed by the audio thread.
    std::atomic<NamModel*> staged { nullptr };
    // Set by the message thread, consumed by the audio thread.
    std::atomic<bool> clearRequested { false };
    // Handed back by the audio thread, destroyed by the message thread.
    std::atomic<NamModel*> retired { nullptr };
    // Audio thread only; no synchronisation needed.
    NamModel* active = nullptr;
};

} // namespace nr::dsp
