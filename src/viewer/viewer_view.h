// What the viewer shows, as a function of its state, separate from the window so tests can draw it.
#pragma once

#include "channel/frame.h"

#include <cstdint>
#include <vector>

namespace fillscope {

struct ViewerState {
    std::vector<Frame> frames;
    std::vector<float> prices;  // frames' prices as floats, for ImGui::PlotLines
    bool source_connected = false;
    double seconds_since_frame = 0;
    std::uint64_t paints = 0;

    void append(const Frame& frame) {
        frames.push_back(frame);
        prices.push_back(static_cast<float>(frame.price));
    }
};

// The name of the viewer's window, and of the price plot inside it.
inline constexpr const char* kViewerWindow = "viewer";
inline constexpr const char* kPricePlot = "price";

// Draws the viewer's one full-screen window. Call between ImGui::NewFrame() and ImGui::Render().
void draw_viewer(const ViewerState& state);

}  // namespace fillscope
