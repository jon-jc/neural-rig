#include "ModelSlot.h"

namespace nr::dsp
{

ModelSlot::~ModelSlot()
{
    // By destruction time the audio thread is stopped, so everything still
    // held anywhere in the cycle is ours to delete.
    delete staged.exchange(nullptr, std::memory_order_acq_rel);
    delete retired.exchange(nullptr, std::memory_order_acq_rel);
    delete active;
    active = nullptr;
}

void ModelSlot::stage(std::unique_ptr<NamModel> model)
{
    // Clear anything the audio thread handed back first, so a burst of model
    // changes cannot leave retirements queueing up behind us.
    collectRetired();

    auto* incoming = model.release();

    if (incoming == nullptr)
    {
        requestClear();
        return;
    }

    // A real capture supersedes any pending clear.
    clearRequested.store(false, std::memory_order_release);

    auto* displaced = staged.exchange(incoming, std::memory_order_acq_rel);

    // A capture that was staged but never claimed — the user picked another
    // before the audio thread reached this one. It never became active, so
    // destroying it here is safe.
    delete displaced;
}

void ModelSlot::requestClear()
{
    // Drop anything still waiting to be picked up; it never became active, so
    // this thread can destroy it.
    delete staged.exchange(nullptr, std::memory_order_acq_rel);
    clearRequested.store(true, std::memory_order_release);
}

void ModelSlot::collectRetired()
{
    delete retired.exchange(nullptr, std::memory_order_acq_rel);
}

NamModel* ModelSlot::acquire() noexcept
{
    // Take a swap only if there is somewhere to put the capture it displaces.
    // This thread is the only one that fills `retired` and the message thread
    // only ever clears it, so observing it empty here stays true through the
    // store below — the handoff cannot race.
    if (active != nullptr && retired.load(std::memory_order_acquire) != nullptr)
        return active; // last retirement not collected yet; retry next block

    if (clearRequested.exchange(false, std::memory_order_acq_rel))
    {
        auto* displaced = active;
        active = nullptr;

        if (displaced != nullptr)
            retired.store(displaced, std::memory_order_release);

        return nullptr;
    }

    if (auto* incoming = staged.exchange(nullptr, std::memory_order_acq_rel))
    {
        auto* displaced = active;
        active = incoming;

        if (displaced != nullptr)
            retired.store(displaced, std::memory_order_release);
    }

    return active;
}

} // namespace nr::dsp
