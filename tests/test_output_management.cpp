// wlr-output-management: wlr-randr / kanshi list the heads and ask for a new
// arrangement; the compositor decides and the client is told the verdict.
//
// What this pins down:
//  - every head is described from Output + OutputLayout: the mode list (a head
//    with none still advertises the mode it runs at, so current_mode has an
//    object to name), the current mode, position, transform, scale, and
//    "enabled" meaning "in the layout";
//  - test and apply both reach the compositor as one request carrying every
//    head, and the client hears the handler's verdict;
//  - after an apply, update() sends only what changed — a new current_mode
//    pointing into the same mode objects — under a new serial;
//  - a configuration built on an old serial is cancelled without reaching the
//    compositor at all, and so is one whose head was unplugged under it;
//  - an output destroyed before or while a client is bound is finished (and
//    never read again — run under ASan to see the read it used to be);
//  - a configuration that leaves a head out is a protocol error.
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "wlr-output-management-unstable-v1-client-protocol.h"

import luminaria.desktop;
import std;

namespace lm = luminaria;

namespace {

// --------------------------------------------------------------------- server

/// A monitor with a fixed mode list, switchable like a KMS connector.
class FakeOutput final : public lm::Output {
public:
    FakeOutput(int width, int height, std::vector<lm::OutputMode> modes)
        : Output(width, height), modes_(std::move(modes)) {}

    [[nodiscard]] std::vector<lm::OutputMode> modes() const override { return modes_; }
    [[nodiscard]] lm::OutputMode current_mode() const override {
        for (const lm::OutputMode& m : modes_) {
            if (m.width == width_ && m.height == height_) {
                return m;
            }
        }
        return Output::current_mode();
    }
    lm::Status set_mode(int width, int height, int refresh_mhz) override {
        for (const lm::OutputMode& m : modes_) {
            if (m.width == width && m.height == height &&
                (refresh_mhz == 0 || refresh_mhz == m.refresh_mhz)) {
                width_ = width;
                height_ = height;
                lm::OutputModeChange change{*this, m};
                mode_changed.emit(change);
                return lm::ok();
            }
        }
        return Output::set_mode(width, height, refresh_mhz);
    }
    lm::Status commit(lm::Color) override { return lm::ok(); }

    // A FreeSync-capable monitor when `vrr_capable_` is set.
    [[nodiscard]] bool vrr_capable() const noexcept override { return vrr_capable_; }
    [[nodiscard]] bool vrr_enabled() const noexcept override { return vrr_on_; }
    lm::Status set_vrr(bool enabled) override {
        if (enabled && !vrr_capable_) {
            return Output::set_vrr(enabled);
        }
        vrr_on_ = enabled;
        return lm::ok();
    }
    bool vrr_capable_ = false;

private:
    bool vrr_on_ = false;
    std::vector<lm::OutputMode> modes_;
};

struct ServerSeen {
    int tests = 0;
    int applies = 0;
};

// --------------------------------------------------------------------- client

struct Mode {
    int width = 0;
    int height = 0;
    int refresh = 0;
    bool preferred = false;
    bool finished = false;
};

struct Head {
    zwlr_output_head_v1* proxy = nullptr;
    std::string name;
    std::string description;
    std::vector<zwlr_output_mode_v1*> modes;
    zwlr_output_mode_v1* current = nullptr;
    bool enabled = false;
    int x = -1;
    int y = -1;
    int transform = -1;
    double scale = 0;
    int adaptive_sync = -1;
    bool finished = false;
};

enum class Answer { none, succeeded, failed, cancelled };

struct ClientState {
    zwlr_output_manager_v1* manager = nullptr;
    std::deque<Head> heads;  // deque: listeners hold pointers into it
    std::deque<Mode> modes;
    std::uint32_t serial = 0;
    int dones = 0;
};

Head* head_of(void* data) { return static_cast<Head*>(data); }
Mode* mode_of(zwlr_output_mode_v1* m) { return static_cast<Mode*>(zwlr_output_mode_v1_get_user_data(m)); }

void mode_size(void* data, zwlr_output_mode_v1*, int32_t w, int32_t h) {
    static_cast<Mode*>(data)->width = w;
    static_cast<Mode*>(data)->height = h;
}
void mode_refresh(void* data, zwlr_output_mode_v1*, int32_t r) { static_cast<Mode*>(data)->refresh = r; }
void mode_preferred(void* data, zwlr_output_mode_v1*) { static_cast<Mode*>(data)->preferred = true; }
void mode_finished(void* data, zwlr_output_mode_v1*) { static_cast<Mode*>(data)->finished = true; }
const zwlr_output_mode_v1_listener kMode{mode_size, mode_refresh, mode_preferred, mode_finished};

ClientState* g_client = nullptr;

void head_name(void* data, zwlr_output_head_v1*, const char* name) { head_of(data)->name = name; }
void head_description(void* data, zwlr_output_head_v1*, const char* d) {
    head_of(data)->description = d;
}
void head_physical_size(void*, zwlr_output_head_v1*, int32_t, int32_t) {}
void head_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    g_client->modes.emplace_back();
    zwlr_output_mode_v1_add_listener(mode, &kMode, &g_client->modes.back());
    head_of(data)->modes.push_back(mode);
}
void head_enabled(void* data, zwlr_output_head_v1*, int32_t enabled) {
    head_of(data)->enabled = enabled != 0;
}
void head_current_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    head_of(data)->current = mode;
}
void head_position(void* data, zwlr_output_head_v1*, int32_t x, int32_t y) {
    head_of(data)->x = x;
    head_of(data)->y = y;
}
void head_transform(void* data, zwlr_output_head_v1*, int32_t t) { head_of(data)->transform = t; }
void head_scale(void* data, zwlr_output_head_v1*, wl_fixed_t s) {
    head_of(data)->scale = wl_fixed_to_double(s);
}
void head_finished(void* data, zwlr_output_head_v1*) { head_of(data)->finished = true; }
void head_make(void*, zwlr_output_head_v1*, const char*) {}
void head_model(void*, zwlr_output_head_v1*, const char*) {}
void head_serial(void*, zwlr_output_head_v1*, const char*) {}
void head_adaptive_sync(void* data, zwlr_output_head_v1*, uint32_t state) {
    head_of(data)->adaptive_sync = static_cast<int>(state);
}
const zwlr_output_head_v1_listener kHead{
    head_name,     head_description, head_physical_size, head_mode,  head_enabled,
    head_current_mode, head_position, head_transform,    head_scale, head_finished,
    head_make,     head_model,       head_serial,        head_adaptive_sync};

void mgr_head(void* data, zwlr_output_manager_v1*, zwlr_output_head_v1* head) {
    auto* st = static_cast<ClientState*>(data);
    st->heads.emplace_back();
    st->heads.back().proxy = head;
    zwlr_output_head_v1_add_listener(head, &kHead, &st->heads.back());
}
void mgr_done(void* data, zwlr_output_manager_v1*, uint32_t serial) {
    auto* st = static_cast<ClientState*>(data);
    st->serial = serial;
    ++st->dones;
}
void mgr_finished(void*, zwlr_output_manager_v1*) {}
const zwlr_output_manager_v1_listener kManager{mgr_head, mgr_done, mgr_finished};

void cfg_succeeded(void* data, zwlr_output_configuration_v1*) {
    *static_cast<Answer*>(data) = Answer::succeeded;
}
void cfg_failed(void* data, zwlr_output_configuration_v1*) {
    *static_cast<Answer*>(data) = Answer::failed;
}
void cfg_cancelled(void* data, zwlr_output_configuration_v1*) {
    *static_cast<Answer*>(data) = Answer::cancelled;
}
const zwlr_output_configuration_v1_listener kConfig{cfg_succeeded, cfg_failed, cfg_cancelled};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t) {
    auto* st = static_cast<ClientState*>(data);
    if (std::strcmp(iface, "zwlr_output_manager_v1") == 0) {
        st->manager = static_cast<zwlr_output_manager_v1*>(
            wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface, 4));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

/// The mode of `head` that is `width`x`height`.
zwlr_output_mode_v1* find_mode(const Head& head, int width, int height) {
    for (zwlr_output_mode_v1* m : head.modes) {
        if (!mode_of(m)->finished && mode_of(m)->width == width && mode_of(m)->height == height) {
            return m;
        }
    }
    return nullptr;
}

/// Send a configuration and wait for the verdict.
Answer submit(wl_display* display, zwlr_output_configuration_v1* config, bool test) {
    Answer answer = Answer::none;
    zwlr_output_configuration_v1_add_listener(config, &kConfig, &answer);
    if (test) {
        zwlr_output_configuration_v1_test(config);
    } else {
        zwlr_output_configuration_v1_apply(config);
    }
    while (answer == Answer::none && wl_display_roundtrip(display) >= 0) {
    }
    zwlr_output_configuration_v1_destroy(config);
    return answer;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    ClientState st;
    g_client = &st;
    wl_registry* reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &kRegistry, &st);
    wl_display_roundtrip(display);
    assert(st.manager != nullptr);
    zwlr_output_manager_v1_add_listener(st.manager, &kManager, &st);
    wl_display_roundtrip(display);

    // The initial description. The monitor unplugged before we bound is gone.
    assert(st.dones == 1);
    assert(st.heads.size() == 2);
    Head& dp = st.heads[0];
    Head& virt = st.heads[1];
    assert(dp.name == "DP-1" && dp.description == "Fake monitor");
    assert(dp.modes.size() == 2);
    assert(mode_of(dp.modes[0])->width == 1920 && mode_of(dp.modes[0])->refresh == 60000);
    assert(mode_of(dp.modes[0])->preferred && !mode_of(dp.modes[1])->preferred);
    assert(dp.current == dp.modes[0]);
    assert(dp.enabled && dp.x == 0 && dp.y == 0 && dp.transform == 0 && dp.scale == 1.0);
    assert(dp.adaptive_sync == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
    // No mode list, no refresh: the size it runs at is still a mode, preferred.
    assert(virt.name == "VIRT-1" && virt.description == "VIRT-1");
    assert(virt.modes.size() == 1 && virt.current == virt.modes[0]);
    assert(mode_of(virt.modes[0])->width == 800 && mode_of(virt.modes[0])->refresh == 0);
    assert(mode_of(virt.modes[0])->preferred);
    assert(virt.enabled && virt.x == 1920 && virt.y == 0);

    zwlr_output_mode_v1* small = find_mode(dp, 1280, 720);
    assert(small != nullptr);
    const std::uint32_t first_serial = st.serial;

    // test: validated, nothing changes, no new serial.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        auto* h = zwlr_output_configuration_v1_enable_head(config, dp.proxy);
        zwlr_output_configuration_head_v1_set_mode(h, small);
        zwlr_output_configuration_head_v1_set_position(h, 0, 0);
        auto* v = zwlr_output_configuration_v1_enable_head(config, virt.proxy);
        zwlr_output_configuration_head_v1_set_scale(v, wl_fixed_from_int(2));
        assert(submit(display, config, true) == Answer::succeeded);
        zwlr_output_configuration_head_v1_destroy(h);
        zwlr_output_configuration_head_v1_destroy(v);
        wl_display_roundtrip(display);
        assert(st.serial == first_serial && dp.current == dp.modes[0]);
    }

    // apply: the compositor switches mode and rescales the second head; the
    // client hears exactly that, under a new serial.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        auto* h = zwlr_output_configuration_v1_enable_head(config, dp.proxy);
        zwlr_output_configuration_head_v1_set_mode(h, small);
        auto* v = zwlr_output_configuration_v1_enable_head(config, virt.proxy);
        zwlr_output_configuration_head_v1_set_scale(v, wl_fixed_from_int(2));
        zwlr_output_configuration_head_v1_set_transform(v, WL_OUTPUT_TRANSFORM_90);
        assert(submit(display, config, false) == Answer::succeeded);
        zwlr_output_configuration_head_v1_destroy(h);
        zwlr_output_configuration_head_v1_destroy(v);
        wl_display_roundtrip(display);
        assert(st.serial != first_serial);
        assert(dp.modes.size() == 2 && dp.current == small); // same objects, new current
        // DP-1 shrank, so the head right of it moved left.
        assert(virt.scale == 2.0 && virt.transform == WL_OUTPUT_TRANSFORM_90);
        assert(virt.x == 1280);
    }

    // Variable refresh: the capable monitor takes it and the client is told;
    // asking the other one for it is refused, and nothing is left half done.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        auto* h = zwlr_output_configuration_v1_enable_head(config, dp.proxy);
        zwlr_output_configuration_head_v1_set_adaptive_sync(
            h, ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED);
        zwlr_output_configuration_head_v1_destroy(
            zwlr_output_configuration_v1_enable_head(config, virt.proxy));
        assert(submit(display, config, false) == Answer::succeeded);
        zwlr_output_configuration_head_v1_destroy(h);
        wl_display_roundtrip(display);
        assert(dp.adaptive_sync == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED);
        assert(virt.adaptive_sync == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);

        config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        zwlr_output_configuration_head_v1_destroy(
            zwlr_output_configuration_v1_enable_head(config, dp.proxy));
        auto* v = zwlr_output_configuration_v1_enable_head(config, virt.proxy);
        zwlr_output_configuration_head_v1_set_adaptive_sync(
            v, ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED);
        assert(submit(display, config, false) == Answer::failed);
        zwlr_output_configuration_head_v1_destroy(v);
        wl_display_roundtrip(display);
        assert(virt.adaptive_sync == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
    }

    // The compositor's verdict, not ours: it refuses to switch anything off.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        zwlr_output_configuration_head_v1_destroy(
            zwlr_output_configuration_v1_enable_head(config, dp.proxy));
        zwlr_output_configuration_v1_disable_head(config, virt.proxy);
        assert(submit(display, config, false) == Answer::failed);
        assert(virt.enabled);
    }

    // Built against an old picture of the outputs: cancelled, never emitted.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, first_serial);
        zwlr_output_configuration_head_v1_destroy(
            zwlr_output_configuration_v1_enable_head(config, dp.proxy));
        zwlr_output_configuration_head_v1_destroy(
            zwlr_output_configuration_v1_enable_head(config, virt.proxy));
        assert(submit(display, config, false) == Answer::cancelled);
    }

    // A monitor unplugged under a pending test: the compositor answers
    // cancelled, and the head and its mode are finished.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        zwlr_output_configuration_head_v1_destroy(
            zwlr_output_configuration_v1_enable_head(config, dp.proxy));
        auto* v = zwlr_output_configuration_v1_enable_head(config, virt.proxy);
        zwlr_output_configuration_head_v1_set_transform(v, WL_OUTPUT_TRANSFORM_FLIPPED);
        assert(submit(display, config, true) == Answer::cancelled);
        zwlr_output_configuration_head_v1_destroy(v);
        wl_display_roundtrip(display);
        assert(virt.finished && mode_of(virt.modes[0])->finished);
        assert(!dp.finished);
    }

    // Leaving a head out of a configuration is a protocol error.
    {
        auto* config = zwlr_output_manager_v1_create_configuration(st.manager, st.serial);
        zwlr_output_configuration_v1_apply(config);
        assert(wl_display_roundtrip(display) < 0);
        assert(wl_display_get_error(display) == EPROTO);
        const wl_interface* iface = nullptr;
        std::uint32_t id = 0;
        assert(wl_display_get_protocol_error(display, &iface, &id) ==
               ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD);
        assert(iface == &zwlr_output_configuration_v1_interface);
    }
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    lm::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    auto* ctx =
        reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) - offsetof(DestroyCtx, listener));
    ctx->display->terminate();
}

} // namespace

int main() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto display = lm::Display::create();
    assert(display.has_value());
    lm::OutputLayout layout;
    auto manager = lm::OutputManager::create(*display, layout);
    assert(manager.has_value());

    auto dp = std::make_unique<FakeOutput>(
        1920, 1080,
        std::vector<lm::OutputMode>{{1920, 1080, 60000, true}, {1280, 720, 60000, false}});
    auto virt = std::make_unique<FakeOutput>(800, 600, std::vector<lm::OutputMode>{});
    dp->vrr_capable_ = true; // only the real monitor can do variable refresh
    layout.add_auto(*dp);
    layout.add_auto(*virt);
    manager->add_head(*dp, {.name = "DP-1", .description = "Fake monitor"});
    manager->add_head(*virt, {.name = "VIRT-1"});
    {
        // Plugged in and out before anyone bound: never described.
        FakeOutput gone(640, 480, {});
        manager->add_head(gone, {.name = "GONE-1"});
    }

    ServerSeen seen;
    auto conn = manager->request().connect([&](lm::OutputConfigRequest& r) {
        assert(r.heads.size() == 2);
        assert(r.heads[0].output == dp.get());
        if (r.test) {
            ++seen.tests;
            if (r.heads[1].transform == lm::Transform::flipped) {
                // Unplugged while the client was deciding.
                layout.remove(*virt);
                virt.reset();
                r.verdict = lm::OutputConfigVerdict::cancelled;
                return;
            }
            assert(r.heads[0].mode.has_value() && r.heads[0].mode->width == 1280);
            assert(!r.heads[0].custom_mode && r.heads[0].position.has_value());
            assert(r.heads[1].scale == 2.0 && !r.heads[1].mode.has_value());
            r.verdict = lm::OutputConfigVerdict::succeeded;
            return;
        }
        ++seen.applies;
        for (const lm::OutputHeadConfig& h : r.heads) {
            if (!h.enabled) {
                return; // policy here: every screen stays on; `failed` is the default
            }
        }
        for (const lm::OutputHeadConfig& h : r.heads) {
            if (h.mode && !h.output->set_mode(h.mode->width, h.mode->height, h.mode->refresh_mhz)) {
                return;
            }
            if (h.scale) {
                h.output->set_scale(static_cast<int>(*h.scale));
            }
            if (h.adaptive_sync && !h.output->set_vrr(*h.adaptive_sync)) {
                return; // a monitor that cannot do it: `failed` is the default
            }
            if (h.transform) {
                h.output->set_transform(*h.transform);
            }
        }
        // Re-lay the row out at the new logical sizes.
        layout.remove(*dp);
        layout.remove(*virt);
        layout.add_auto(*dp);
        layout.add_auto(*virt);
        manager->update();
        r.verdict = lm::OutputConfigVerdict::succeeded;
    });

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx dc{{}, &*display};
    dc.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &dc.listener);

    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);
    display->run();
    client_thread.join();

    assert(seen.tests == 2);
    // The mode switch, the two variable-refresh applies (one taken, one refused) and
    // the refused switch-off; the stale one never arrived, the error one never got that far.
    assert(seen.applies == 4);
    assert(virt == nullptr);
    return 0;
}
