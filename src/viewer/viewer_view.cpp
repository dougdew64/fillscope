#include "viewer/viewer_view.h"

#include <imgui.h>

#include <cfloat>

namespace fillscope {

void draw_viewer(const ViewerState& state) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(kViewerWindow, nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::Text("source: %s", state.source_connected ? "connected" : "waiting");
    ImGui::Text("frames received: %zu", state.frames.size());
    if (!state.frames.empty()) {
        const Frame& last = state.frames.back();
        ImGui::Text("last frame: #%llu  price %lld  qty %u", static_cast<unsigned long long>(last.index),
                    static_cast<long long>(last.price), last.qty);
    }
    ImGui::Text("seconds since last frame: %.1f", state.seconds_since_frame);
    ImGui::Text("viewer paints: %llu (keeps rising while the source is stopped)",
                static_cast<unsigned long long>(state.paints));
    if (!state.prices.empty()) {
        ImGui::PlotLines(kPricePlot, state.prices.data(), static_cast<int>(state.prices.size()), 0, nullptr,
                         FLT_MAX, FLT_MAX, ImVec2(-1, -1));
    }
    ImGui::End();
}

}  // namespace fillscope
