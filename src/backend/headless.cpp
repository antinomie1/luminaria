#include "luminaria/backend/headless.hpp"

#include <utility>

namespace luminaria {

HeadlessOutput::HeadlessOutput(EventLoop loop, int width, int height, unsigned interval_ms)
    : Output(width, height), interval_ms_(interval_ms) {
    frame_timer_ = loop.add_timer([this] {
        emit_software_frame(interval_ms_ * 1000000u);
        frame_timer_.arm(interval_ms_); // repeat
    });
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

void HeadlessOutput::start_pump() {
    frame_timer_.arm(interval_ms_);
}

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
