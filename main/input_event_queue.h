#pragma once

#include <cstddef>
#include <utility>

namespace input_event_queue {

// A tiny allocation-free queue for physical input edges. Ordinary presses and
// repeats may be dropped under pathological load, but a release always evicts a
// droppable down event instead. This prevents a full queue from leaving Codex
// with a logically held Agent Key, action key or encoder button.
template <typename T, size_t Capacity> class Queue {
  public:
    static_assert(Capacity > 0, "input queue capacity must be non-zero");

    template <typename IsRelease, typename IsDroppable>
    bool push(const T& item, bool repeat, IsRelease&& is_release, IsDroppable&& is_droppable)
    {
        if (count_ == Capacity) {
            if (repeat || !is_release(item))
                return false;
            size_t victim = Capacity;
            for (size_t i = 0; i < count_; ++i) {
                if (is_droppable(items_[i])) {
                    victim = i;
                    break;
                }
            }
            // A release is the state-restoring edge. If every queued item is
            // important, sacrifice the oldest one rather than losing release.
            if (victim == Capacity)
                victim = 0;
            erase(victim);
        }
        items_[count_++] = item;
        return true;
    }

    bool pop(T& out)
    {
        if (count_ == 0)
            return false;
        out = items_[0];
        erase(0);
        return true;
    }

    void clear()
    {
        count_ = 0;
    }
    size_t size() const
    {
        return count_;
    }
    constexpr size_t capacity() const
    {
        return Capacity;
    }

  private:
    void erase(size_t index)
    {
        for (size_t i = index + 1; i < count_; ++i)
            items_[i - 1] = std::move(items_[i]);
        --count_;
    }

    T items_[Capacity] = {};
    size_t count_ = 0;
};

} // namespace input_event_queue
