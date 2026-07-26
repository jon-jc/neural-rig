#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "ModelSlot.h"

namespace nr::dsp
{

/** Per-slot controls, read fresh each block from the parameter tree. */
struct NodeSettings
{
    bool bypassed = false;
    float gainDb = 0.0f;
    float mixPercent = 100.0f;
};

/**
    An ordered chain of NAM captures.

    Stacking captures is the point of the plugin: an overdrive capture into an
    amp capture behaves far more like the real pairing than either alone,
    because the second network sees the first one's actual output rather than a
    clean guitar signal.

    Two things make this harder than running one capture after another.

    **Latency must not move.** Each capture contributes its own resampling
    latency, so the chain's total is the sum. If bypassing a node simply
    skipped it, the total would change and the host would have to re-align
    mid-session — audible as the whole track jumping. A bypassed node
    therefore still passes its audio through a delay of exactly its own
    latency, so the reported figure is stable whatever the user toggles.

    **A per-node mix needs its dry path delayed.** Blending a capture's output
    against an undelayed copy of its input combs the signal, because the wet
    side is late by the resampler's latency. Each node delays its dry copy to
    match before mixing.

    Every slot hot-swaps independently through its own ModelSlot, so loading
    into slot 3 never disturbs what slots 1 and 2 are playing.
*/
class RigChain
{
public:
    /** Four captures is already a heavy load — each one is a full network
        evaluation per sample — and it comfortably covers pedal into amp into
        a second stage. */
    static constexpr int numSlots = 4;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Runs the whole chain over a mono block in place. Real-time safe. */
    void process(float* samples, int numSamples, const std::array<NodeSettings, numSlots>& settings);

    /** Sum of every loaded slot's latency, in samples. Independent of bypass
        state by design. Audio thread or message thread. */
    int latencySamples() const noexcept;

    // --- Message thread only ------------------------------------------------

    /** Publishes a prepared capture into a slot. */
    void stage(int slotIndex, std::unique_ptr<NamModel> model);

    /** Empties a slot. */
    void clear(int slotIndex);

    /** Asks the audio thread to exchange two slots' captures, which is how the
        UI reorders the rig. Order matters enormously: a drive capture before
        an amp capture is a different instrument from the reverse.

        Deferred rather than immediate because the playing captures belong to
        the audio thread; the exchange happens at the top of the next block.
        Total latency is unaffected, since a sum does not care about order. */
    void requestSwap(int firstSlot, int secondSlot);

    /** Destroys captures the audio thread has handed back. Call on a timer. */
    void collectRetired();

    /** The capture in a slot, or nullptr. For the UI to show names. */
    const NamModel* modelAt(int slotIndex) const noexcept;

    /** The earliest loaded capture, or nullptr.

        Input calibration keys off this one: it is what actually receives the
        player's signal, so it is the only capture whose trained input level
        says anything about how hard to drive the rig. */
    const NamModel* firstLoaded() const noexcept;

    /** The last loaded capture, or nullptr.

        Output levelling keys off this one, since whatever it does to the
        signal is what reaches the DAW. Normalising against an earlier stage
        would be corrected away by everything after it. */
    const NamModel* lastLoaded() const noexcept;

private:
    struct Node
    {
        ModelSlot slot;
        // Delays the dry copy for the mix control, and carries the audio when
        // the node is bypassed, so latency stays put either way.
        std::vector<float> delayBuffer;
        int writeIndex = 0;
        int currentDelay = 0;

        void prepareDelay(int maxDelaySamples);
        void resetDelay() noexcept;
        /** Pushes `input` through the delay, writing the delayed signal to
            `output`. Both may alias. */
        void runDelay(const float* input, float* output, int numSamples, int delaySamples) noexcept;
    };

    static bool isValidSlot(int slotIndex) noexcept { return slotIndex >= 0 && slotIndex < numSlots; }

    void applyPendingSwap() noexcept;

    std::array<Node, numSlots> nodes;
    std::vector<float> dryScratch;

    /** Encodes a pending reorder as (first + 1) << 8 | (second + 1); zero
        means nothing pending. Packing both indices into one atomic keeps the
        request indivisible without a lock. */
    std::atomic<std::uint32_t> pendingSwap { 0 };

    double currentSampleRate = 0.0;
    int currentBlockSize = 0;
};

} // namespace nr::dsp
