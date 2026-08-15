// luminaria/backend/headless.cppm — virtual backend with no real hardware.
//
// Outputs exist purely in memory; frames are pumped by a software timer. This is
// the CI/test backend — no GPU, no display server required.

module;

#include <memory>
#include <span>
#include <vector>

#include <utility>

export module luminaria:headless;

import :backend;
import :color;
import :event_loop;
import :expected;
import :output;
import :pixel;

export namespace luminaria {

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

    /// Ask for the first frame. Every later one is asked for the same way every
    /// other backend's is — `Output::schedule_frame()`. The timer only paces
    /// requests; it does not manufacture them, so an idle headless output ticks
    /// exactly zero times a second.
    void start_pump();

protected:
    void arm_frame() override;
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

// --------------------------------------------------------------- implementation
namespace luminaria {

HeadlessOutput::HeadlessOutput(EventLoop loop, int width, int height, unsigned interval_ms)
    : Output(width, height), interval_ms_(interval_ms) {
    // One-shot: emit_software_frame() clears the request, and a handler that
    // wants another frame re-arms the timer through schedule_frame().
    frame_timer_ = loop.add_timer([this] { emit_software_frame(interval_ms_ * 1000000u); });
}

Status HeadlessOutput::commit(Color color) {
    last_committed_ = color;
    return ok();
}

Status HeadlessOutput::commit_frame(std::span<const Pixel> rgba, int width, int height) {
    if (width != width_ || height != height_ ||
        rgba.size() != static_cast<size_t>(width) * height) {
        return fail("headless: frame size mismatch");
    }
    last_frame_.assign(rgba.begin(), rgba.end());
    return ok();
}

void HeadlessOutput::arm_frame() { frame_timer_.arm(interval_ms_ == 0 ? 1 : interval_ms_); }

void HeadlessOutput::start_pump() { schedule_frame(); }

HeadlessBackend::HeadlessBackend(EventLoop loop, unsigned frame_interval_ms)
    : loop_(loop), interval_ms_(frame_interval_ms) {}

Output& HeadlessBackend::add_output(int width, int height) {
    outputs_.push_back(std::make_unique<HeadlessOutput>(loop_, width, height, interval_ms_));
    return *outputs_.back();
}

Status HeadlessBackend::start() {
    started_ = true;
    for (auto& out : outputs_) {
        NewOutput event{*out};
        new_output.emit(event);
    }
    for (auto& out : outputs_) {
        out->start_pump();
    }
    return ok();
}

} // namespace luminaria
