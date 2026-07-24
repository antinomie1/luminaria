// luminaria/core/signal.hpp — type-safe observer signal with RAII connections.
//
// This is the memory-safety centerpiece. wlroots embeds a `wl_listener` in each
// observer and relies on manual `wl_list_remove`; forgetting it is the classic
// compositor use-after-free. Here a `Signal<Event>::Connection` removes itself
// on destruction, and is safe in BOTH lifetime orders:
//   - Connection destroyed first -> the slot is unregistered.
//   - Signal destroyed first     -> the Connection becomes an inert no-op.
// The friendly API is: `auto c = sig.connect([](Event& e){ ... });` and you keep
// `c` alive exactly as long as you want the callback.
//
// Pure C++: it does NOT touch libwayland's wl_signal, so callbacks are typed
// (no void* trampolines). Interop wrappers for raw C wl_signal live elsewhere.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <utility>

namespace luminaria {

template <class Event>
class Signal {
    struct Entry {
        std::uint64_t id;
        std::function<void(Event&)> slot;
        bool alive;
    };
    struct Impl {
        std::list<Entry> entries; // node addresses are stable across mutation
        std::uint64_t next_id = 1;
        int emitting = 0;
        void sweep() {
            if (emitting == 0) {
                entries.remove_if([](const Entry& e) { return !e.alive; });
            }
        }
    };
    std::shared_ptr<Impl> impl_ = std::make_shared<Impl>();

public:
    /// Movable RAII handle keeping one slot connected. Non-copyable.
    class Connection {
        std::weak_ptr<Impl> impl_;
        std::uint64_t id_ = 0;
        friend class Signal;
        Connection(std::weak_ptr<Impl> impl, std::uint64_t id)
            : impl_(std::move(impl)), id_(id) {}

    public:
        Connection() = default;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&& o) noexcept { *this = std::move(o); }
        Connection& operator=(Connection&& o) noexcept {
            if (this != &o) {
                disconnect();
                impl_ = std::move(o.impl_);
                id_ = o.id_;
                o.impl_.reset();
                o.id_ = 0;
            }
            return *this;
        }
        ~Connection() { disconnect(); }

        /// Unregister the slot now. Idempotent; safe if the Signal is already gone.
        void disconnect() noexcept {
            if (auto s = impl_.lock()) {
                if (id_ != 0) {
                    for (auto& e : s->entries) {
                        if (e.id == id_) {
                            e.alive = false;
                            break;
                        }
                    }
                    s->sweep(); // deferred if an emit is in progress
                }
            }
            impl_.reset();
            id_ = 0;
        }

        /// True while the slot is still registered and the Signal still exists.
        [[nodiscard]] bool connected() const noexcept {
            if (auto s = impl_.lock(); s && id_ != 0) {
                for (const auto& e : s->entries) {
                    if (e.id == id_) {
                        return e.alive;
                    }
                }
            }
            return false;
        }
    };

    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) noexcept = default;
    Signal& operator=(Signal&&) noexcept = default;

    /// Register a slot. Keep the returned Connection alive to stay subscribed.
    [[nodiscard]] Connection connect(std::function<void(Event&)> slot) {
        const std::uint64_t id = impl_->next_id++;
        impl_->entries.push_back(Entry{id, std::move(slot), true});
        return Connection{impl_, id};
    }

    /// Invoke every live slot with `event`.
    /// Re-entrancy is defined: slots connected DURING this emit are not called
    /// until the next emit, and disconnecting any slot (including self) never
    /// destroys a std::function that is currently executing.
    void emit(Event& event) {
        auto s = impl_; // keep Impl alive even if the Signal is moved-from mid-emit
        const std::uint64_t boundary = s->next_id; // slots added during emit have id >= boundary
        ++s->emitting;
        for (auto& e : s->entries) {
            if (e.alive && e.id < boundary) {
                e.slot(event);
            }
        }
        --s->emitting;
        s->sweep();
    }

    /// Number of currently-live slots. Mainly for tests/assertions.
    [[nodiscard]] std::size_t slot_count() const noexcept {
        std::size_t n = 0;
        for (const auto& e : impl_->entries) {
            if (e.alive) {
                ++n;
            }
        }
        return n;
    }
};

} // namespace luminaria
