#ifndef NEXER_FUNCTION_LIST_H_
#define NEXER_FUNCTION_LIST_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nexer {

// A type-safe, re-entrant observer list. Subscribers call add() to register a
// callback and receive a remove_fn handle they can invoke to unsubscribe. The
// owner calls invoke() to fan out an event to every active subscriber.
//
// Re-entrancy is the main design constraint: a callback triggered by invoke()
// may add new subscribers or remove existing ones (including itself). To handle
// this safely, removals mark entries inactive rather than erasing them
// immediately, and a deferred compact() sweep reclaims the slots once the
// outermost invoke() returns.
//
// Lifetime is tied to an external owner (via a weak_ptr). When the owner is
// destroyed, all outstanding remove_fn handles become no-ops, preventing
// use-after-free on the entries_ vector.
template <typename T, typename... U> class FunctionList {
    struct Entry {
        std::function<T(U...)> fn;
        std::uint64_t id;
        bool active;
    };

    std::vector<Entry> entries_;
    std::weak_ptr<void> owner_;
    std::uint64_t next_id_ = 1;
    unsigned int invoke_depth_ =
        0; // > 0 while inside invoke(), defers compaction
    bool needs_compact_ = false;

    // Removes inactive entries from the vector. Deferred until no invoke() is
    // on the call stack so that iterating callers never see invalidated
    // indices.
    void compact() {
        if (!needs_compact_ || invoke_depth_ > 0) {
            return;
        }

        std::size_t write = 0;
        for (std::size_t read = 0; read < entries_.size(); ++read) {
            if (!entries_[read].active)
                continue;
            if (write != read) {
                entries_[write] = std::move(entries_[read]);
            }
            ++write;
        }
        entries_.resize(write);
        needs_compact_ = false;
    }

  public:
    typedef std::function<void()> remove_fn;

    // Bind this list to an owning object. The weak_ptr is checked on every
    // add() and captured by every remove_fn handle to guard against dangling
    // calls.
    void set_owner(std::shared_ptr<void> owner) { owner_ = std::move(owner); }

    // Register a callback. Returns a remove_fn handle that, when called,
    // unsubscribes the callback. Safe to call from inside invoke().
    remove_fn add(std::function<T(U...)> fn) {
        auto owner = owner_;
        if (owner.expired()) {
            throw std::logic_error(
                "FunctionList::Add called without a live owner");
        }

        auto id = next_id_++;
        entries_.push_back(Entry{std::move(fn), id, true});

        return [this, owner, id] {
            if (owner.expired())
                return;
            for (auto &entry : entries_) {
                if (entry.id == id) {
                    if (!entry.active)
                        return;
                    entry.active = false;
                    needs_compact_ = true;
                    break;
                }
            }
            compact();
        };
    }

    // Fan out an event to all active subscribers. Snapshots the current size
    // so that callbacks added during iteration are not visited in this round.
    void invoke(U... args) {
        ++invoke_depth_;
        auto size = entries_.size();
        for (std::size_t i = 0; i < size; ++i) {
            if (!entries_[i].active)
                continue;
            auto fn = entries_[i].fn;
            fn(args...);
        }
        --invoke_depth_;
        compact();
    }

    // Remove all subscribers. If called from inside invoke(), entries are
    // marked inactive and compacted once the outermost invoke() finishes.
    void clear() {
        if (invoke_depth_ > 0) {
            for (auto &entry : entries_) {
                entry.active = false;
            }
            needs_compact_ = true;
            return;
        }

        entries_.clear();
        needs_compact_ = false;
    }
};

} // namespace nexer

#endif // NEXER_FUNCTION_LIST_H_
