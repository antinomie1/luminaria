// luminaria/backend/headless.hpp — virtual backend with no real hardware.
//
// Outputs exist purely in memory; frames are pumped by a software timer. This is
// the CI/test backend — no GPU, no display server required.
#pragma once

#include <memory>
#include <span>
#include <vector>

#include "luminaria/backend.hpp"
#include "luminaria/core/event_loop.hpp"
#include "luminaria/output.hpp"
#include "luminaria/util/pixel.hpp"

namespace luminaria {

class HeadlessOutput final : public Output {
    EventSource frame_timer_;
    unsigned interval_ms_;
    std::vector<Pixel> last_frame_;

public:
    HeadlessOutput(EventLoop loop, int width, int height, unsigned interval_ms);

    Status commit(Color color) override;
    Status commit_frame(std::span<const Pixel> rgba, int width, int height) override;

    /// The most recently presented frame (for tests/inspection).
    [[nodiscard]] const std::vector<Pixel>& last_frame() const noexcept { return last_frame_; }

    /// Begin the software frame pump.
    void start_pump();
};

class HeadlessBackend final : public Backend {
    EventLoop loop_;
    unsigned interval_ms_;
    std::vector<std::unique_ptr<HeadlessOutput>> outputs_;
    bool started_ = false;

public:
    explicit HeadlessBackend(EventLoop loop, unsigned frame_interval_ms = 16);

    /// Add a virtual output. Call before start().
    Output& add_output(int width, int height);

    Status start() override;
};

} // namespace luminaria
