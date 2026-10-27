// Self-checks for Signal<Event> — the memory-safety centerpiece.
// No framework: assert-based, returns non-zero on failure.
#include <cassert>

import luminaria;
import std;

using luminaria::Signal;

namespace {

struct Ev {
    int value;
};

void basic_emit_and_count() {
    Signal<Ev> sig;
    int seen = 0;
    auto c = sig.connect([&](Ev& e) { seen += e.value; });
    assert(sig.slot_count() == 1);
    Ev e{3};
    sig.emit(e);
    sig.emit(e);
    assert(seen == 6);
    assert(c.connected());
}

void raii_disconnect_on_scope_exit() {
    Signal<Ev> sig;
    int seen = 0;
    {
        auto c = sig.connect([&](Ev&) { ++seen; });
        Ev e{0};
        sig.emit(e);
    } // c destroyed here -> auto-unregister
    assert(sig.slot_count() == 0);
    Ev e{0};
    sig.emit(e);
    assert(seen == 1); // not called after disconnect
}

void self_disconnect_during_emit_is_safe() {
    Signal<Ev> sig;
    int calls = 0;
    // The connection is captured by the slot; the slot disconnects itself.
    // Deferred sweep must NOT destroy the executing std::function.
    auto conn = std::make_shared<std::optional<Signal<Ev>::Connection>>();
    *conn = sig.connect([&, conn](Ev&) {
        ++calls;
        (*conn)->disconnect();
    });
    Ev e{0};
    sig.emit(e);
    sig.emit(e);
    assert(calls == 1); // fired once, then gone
    assert(sig.slot_count() == 0);
}

void connect_during_emit_defers_to_next_round() {
    Signal<Ev> sig;
    int a = 0, b = 0;
    Signal<Ev>::Connection late; // kept alive past the emit
    auto c = sig.connect([&](Ev&) {
        ++a;
        if (!late.connected()) {
            late = sig.connect([&](Ev&) { ++b; });
        }
    });
    Ev e{0};
    sig.emit(e); // a fires, b connected but NOT called this round
    assert(a == 1 && b == 0);
    sig.emit(e); // now both fire
    assert(a == 2 && b == 1);
}

void signal_destroyed_before_connection_is_inert() {
    Signal<Ev>::Connection c;
    {
        Signal<Ev> sig;
        c = sig.connect([](Ev&) {});
        assert(c.connected());
    } // sig destroyed while c still alive
    assert(!c.connected());
    c.disconnect(); // must be a safe no-op, not a crash
}

void move_transfers_ownership() {
    Signal<Ev> sig;
    int seen = 0;
    auto c1 = sig.connect([&](Ev&) { ++seen; });
    auto c2 = std::move(c1);
    assert(!c1.connected());
    assert(c2.connected());
    assert(sig.slot_count() == 1);
    Ev e{0};
    sig.emit(e);
    assert(seen == 1);
}

void zero_argument_slot_ignores_the_event() {
    Signal<Ev> sig;
    int seen = 0;
    auto c = sig.connect([&] { ++seen; });
    Ev e{7};
    sig.emit(e);
    sig.emit(e);
    assert(seen == 2);
    c = {};
    sig.emit(e);
    assert(seen == 2);
}

void connections_drop_with_the_bag_and_forget_dead_signals() {
    int seen = 0;
    Signal<Ev> long_lived;
    {
        luminaria::Connections bag;
        bag += long_lived.connect([&] { ++seen; });
        // A signal per "window": each dies at once, leaving an inert connection.
        for (int i = 0; i < 1000; ++i) {
            Signal<Ev> transient;
            bag += transient.connect([] {});
        }
        assert(bag.size() < 100); // pruned while growing, not 1001 corpses
        Ev e{0};
        long_lived.emit(e);
        assert(seen == 1);
    } // bag destroyed -> everything it held disconnected
    assert(long_lived.slot_count() == 0);
}

} // namespace

int main() {
    zero_argument_slot_ignores_the_event();
    connections_drop_with_the_bag_and_forget_dead_signals();
    basic_emit_and_count();
    raii_disconnect_on_scope_exit();
    self_disconnect_during_emit_is_safe();
    connect_during_emit_defers_to_next_round();
    signal_destroyed_before_connection_is_inert();
    move_transfers_ownership();
    return 0;
}
