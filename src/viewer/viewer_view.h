// What the viewer draws, as a function of its state, and what the user did while it was drawn.
//
// Dear ImGui is an immediate-mode UI: there is no tree of widget objects to build and update. Every
// paint, the code calls ImGui::Button(...) and the like afresh, and a call returns true when the
// user has just clicked it. So drawing and input are one pass. draw_viewer draws everything from
// the state it is given and returns what the user did; the caller applies that. Nothing in here
// changes state, which is what lets tests draw it with any state they like and check the result.
#pragma once

#include "viewer/context.h"
#include "viewer/viewer_state.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fillscope {

// What the user did during one paint. Usually nothing; at most one click's worth.
struct ViewerActions {
    std::optional<Frame> point_at;        // point at this frame
    std::optional<std::uint32_t> follow;  // follow this id
    bool clear_point = false;
    bool clear_follow = false;

    [[nodiscard]] bool any() const noexcept { return point_at || follow || clear_point || clear_follow; }
};

// Applies the user's actions to the selection.
void apply(const ViewerActions& actions, Selection& selection);

// The names of the viewer's window and of items inside it, which the UI tests use to find them.
inline constexpr const char* kViewerWindow = "viewer";
inline constexpr const char* kPricePlot = "price";
inline constexpr const char* kFrameTable = "frames";
inline constexpr const char* kClearPointButton = "x##clear_point";
inline constexpr const char* kClearFollowButton = "x##clear_follow";

// A frame's row is clicked to point at it, and its id is clicked to follow the id. Everything after
// "##" in a Dear ImGui label is part of the item's ID but not displayed, which keeps each row's
// items distinct while showing just the number.
[[nodiscard]] std::string point_label(const Frame& frame);   // "<index>##point"
[[nodiscard]] std::string follow_label(const Frame& frame);  // "<id>##follow<index>"

// Draws the viewer's one full-screen window. Call between ImGui::NewFrame() and ImGui::Render().
// The context bar is drawn from `published`, the context the file holds, and shows `publish_error`
// if the last write failed: the bar must never claim what the assistant hasn't been given.
[[nodiscard]] ViewerActions draw_viewer(const ViewerState& state, const Selection& selection,
                                        const std::optional<Context>& published,
                                        const std::optional<std::string>& publish_error);

}  // namespace fillscope
