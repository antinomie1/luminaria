// libseat needs a real logind session or a running seatd, which CI and a
// desktop terminal usually aren't. So this skips (77) rather than failing when
// there is no seat to join — what it actually guards is that create() reports
// that cleanly instead of crashing, and that a seat we DO get is usable.
#include <cassert>
#include <cstdio>

#include "luminaria/core/display.hpp"
#include "luminaria/session.hpp"

int main() {
    auto display = luminaria::Display::create();
    assert(display.has_value());

    auto session = luminaria::Session::create(display->event_loop());
    if (!session) {
        std::fprintf(stderr, "skip: %s\n", session.error().message.c_str());
        return 77;
    }

    // A seat that opened is active by construction — create() waits for the
    // first enable_seat before returning.
    assert(session->active());

    // Opening a device we are not allowed to touch must fail rather than
    // returning a bogus fd.
    int id = -1;
    auto bogus = session->open_device("/dev/dri/definitely-not-a-device", id);
    assert(!bogus.has_value());
    assert(id == -1);

    // The activity signal is the contract the DRM backend relies on; make sure
    // subscribing and dropping the subscription is safe with no events at all.
    bool seen = false;
    {
        auto conn = session->activity().connect([&](luminaria::SessionActive&) { seen = true; });
    }
    assert(!seen);

    if (auto opened = session->open_device("/dev/dri/card0", id)) {
        assert(*opened >= 0 && id >= 0);
        session->close_device(id);
    }
    return 0;
}
