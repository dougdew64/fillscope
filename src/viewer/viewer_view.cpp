#include "viewer/viewer_view.h"

#include <imgui.h>
#include <imgui_internal.h>  // IMGUI_TEST_ENGINE_ITEM_INFO, ImGuiItemStatusFlags

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fillscope {
namespace {

constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
constexpr ImVec4 kTrueColor{0.45f, 0.85f, 0.45f, 1.0f};  // a claim the replay bears out
constexpr ImVec4 kFollowedRowColor{0.45f, 0.38f, 0.10f, 0.65f};
constexpr ImVec4 kBidColor{0.45f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kAskColor{0.95f, 0.50f, 0.45f, 1.0f};
constexpr ImVec4 kContextBarColor{0.16f, 0.22f, 0.30f, 1.0f};  // a muted slate blue behind the context bar
constexpr ImVec4 kSlicerColor{1.0f, 0.62f, 0.12f, 1.0f};        // the slicer's trades, as the truth says
constexpr ImVec4 kTradeColor{0.62f, 0.62f, 0.62f, 0.85f};       // everyone else's

// Draws text that is already formatted. TextUnformatted takes the characters as they are, with no
// printf-style % processing, so a '%' in the text can't be misread as a format directive.
void text(std::string_view s) { ImGui::TextUnformatted(s.data(), s.data() + s.size()); }

// Text wrapped at the edge of the pane it is in.
void wrapped(std::string_view s) {
    ImGui::PushTextWrapPos(0.0f);
    text(s);
    ImGui::PopTextWrapPos();
}

// Text wrapped short of the pane's edge by the room `button`, a small button drawn after it on its
// last line, takes: wrapped at the edge, a line that just fit would put the button past it.
void wrapped_before(std::string_view s, const char* button) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float room = style.ItemSpacing.x + ImGui::CalcTextSize(button, nullptr, true).x + style.FramePadding.x * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - room);
    text(s);
    ImGui::PopTextWrapPos();
}

// A tooltip for the item just drawn, while it is hovered. `describe` builds the text, and is called
// only while hovered, so no tooltip text is built for the items nobody is pointing at.
// std::invocable<F> is a C++20 concept: it accepts anything callable with no arguments.
template <std::invocable F>
void tooltip(F&& describe) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        const std::string what = describe();
        ImGui::SetTooltip("%s", what.c_str());
    }
}

// Tells the Dear ImGui Test Engine, when one is built in, whether the item just drawn is marked
// (pointed at, followed, the ticker shown), as Dear ImGui does for a checkbox's tick. Dear ImGui
// doesn't report a Selectable's selected state or a row's colour, so without this the UI tests could
// check only the rule (level_marked and the rest), not the book as drawn. In the viewer itself the
// macro is empty.
void report_marked([[maybe_unused]] const Label& label, [[maybe_unused]] bool marked) {
    ImGuiContext& g = *ImGui::GetCurrentContext();  // the macro refers to the context as `g`
    IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetItemID(), label.c_str(),
                                ImGui::GetItemStatusFlags() | ImGuiItemStatusFlags_Checkable |
                                    (marked ? ImGuiItemStatusFlags_Checked : 0));
}

// What a click does, for a tooltip: sent at once, unless writes are failing.
std::string_view sends(const std::optional<std::string>& publish_error) {
    return publish_error ? "but publishing is failing (see the context bar), so the assistant won't receive it"
                         : "sends it to the assistant now";
}

// The context bar's lines: what the assistant has been given, drawn from the last published context
// and never from the live selection, so the bar and the file can't disagree.
// A heading on its own line, and its parts indented beneath it, so the background reads as part
// of the context rather than as the heading's continuation.
void draw_context_bar_lines(const ViewerView& view, ViewerActions& actions) {
    text(kContextHeading);
    ImGui::Indent();
    // The pointing line's room, kept while nothing is pointed at, so that pointing at something, or
    // stopping, doesn't move everything below the bar: a line of text, as tall as the line with its
    // bullet and its small button (a small button has no padding above and below its text).
    const auto room_for_a_point = [] { ImGui::Dummy(ImVec2{0.0f, ImGui::GetTextLineHeight()}); };
    if (!view.published_bar) {
        text(view.publish_error ? "nothing published" : "nothing published yet");
        room_for_a_point();
    } else {
        const ContextBarText& bar = *view.published_bar;
        ImGui::Bullet();  // one per item of context, so each reads as a separate one
        wrapped(bar.background);
        if (bar.point) {
            ImGui::Bullet();
            wrapped_before(*bar.point, kClearPointButton);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearPointButton)) {
                actions.clear_point = true;
            }
            tooltip([&] { return std::format("Stop pointing at it: {}", sends(view.publish_error)); });
        } else {
            room_for_a_point();
        }
        if (bar.follow) {
            ImGui::Bullet();
            wrapped_before(*bar.follow, kClearFollowButton);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearFollowButton)) {
                actions.clear_follow = true;
            }
            tooltip([&] { return std::format("Stop following it: {}", sends(view.publish_error)); });
        }
    }
    if (view.publish_error) {
        // Push and pop must pair; the style stack is checked at the end of the frame.
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        wrapped(std::format("NOT published: {}", *view.publish_error));
        ImGui::PopStyleColor();
    }
    ImGui::Unindent();
}

// The context bar, on a background of its own so it stands apart from the market below it. The
// background's height is known only once the lines are drawn, so they are drawn first, onto a
// draw-list channel above the one the background then goes on. (A child window would colour it too,
// but would change the IDs of the buttons inside it, by which the UI tests find them.)
void draw_context_bar(const ViewerView& view, ViewerActions& actions) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    draw_context_bar_lines(view, actions);
    ImGui::EndGroup();
    draw->ChannelsSetCurrent(0);
    const ImVec2 window = ImGui::GetWindowPos();
    const float bottom = ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y / 2;
    draw->AddRectFilled(window, {window.x + ImGui::GetWindowWidth(), bottom}, ImGui::GetColorU32(kContextBarColor));
    draw->ChannelsMerge();
}

// The market data's state and the viewer's own liveness.
void draw_status(const ViewerView& view) {
    const ViewerState& state = view.state;
    // Live, the feed's books, whatever the screen shows (a time-shifted moment is said below them);
    // in a replay or an answer, the books shown.
    const BookBuilder& books = state.replay || state.answer ? state.books : state.live;
    const bool answering = state.answer.has_value();
    if (state.answer && !state.replay) {
        wrapped("market data: none until an answer is shown");
    } else if (state.replay) {
        // A replay is read from a file: not the exchange, and with no snapshots to recover from.
        if (books.status() == BookBuilder::Status::recovering) {
            wrapped(std::format("market data: a replay, stuck after update {}: {}; a replay has no snapshots to recover from",
                             books.last_seq(), books.recovering_because().value_or("an update can't be applied")));
        } else {
            // In an answer, the replay is the answer's own, so only where it is matters.
            wrapped(answering ? std::format("the books as of update {}", books.last_seq())
                              : std::format("market data: a replay, the books are as of update {}", books.last_seq()));
        }
    } else {
        switch (books.status()) {
        case BookBuilder::Status::waiting:
            wrapped("market data: waiting for the exchange");
            break;
        case BookBuilder::Status::live:
            wrapped(std::format("market data: live, the books are as of update {}", books.last_seq()));
            break;
        case BookBuilder::Status::recovering:
            wrapped(std::format("market data: recovering, the books are as of update {}: {}; waiting for a snapshot",
                             books.last_seq(), books.recovering_because().value_or("updates were missed")));
            break;
        }
    }
    // In an answer, the line above says all a viewer needs; the rest are the feed's and the viewer's
    // health, for watching the exchange.
    if (!answering) {
        wrapped(std::format("updates received: {}  recoveries: {}", books.updates_received(), books.recoveries().size()));
    }
    if (state.feed_error) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        wrapped(std::format("last market data error: {}", *state.feed_error));
        ImGui::PopStyleColor();
    }
    if (answering) {
        return;
    }
    if (view.timing.seconds_since_update) {
        wrapped(std::format("seconds since last update: {:.1f}", *view.timing.seconds_since_update));
    }
    wrapped(std::format("viewer paints: {} (keeps rising while the exchange is stopped)", view.timing.paints));
}

// The space between paragraphs of an answer: half a line, so it grows with the text.
void paragraph_break() {
    ImGui::Dummy(ImVec2{0.0f, ImGui::GetTextLineHeight() * 0.5f});
}

// A divider between the parts of a pane, with a paragraph's space on either side.
void divider() {
    paragraph_break();
    ImGui::Separator();
    paragraph_break();
}

// Live, what the screen shows, for the line above the time-shifting controls: the newest update, or
// one behind it, paused or playing. Short, so it doesn't wrap at the windows it is shown in.
std::string time_shift_line(const ViewerView& view) {
    const TimeShiftBuffer& kept = *view.kept;
    const std::optional<TimeShift>& shift = view.state.time_shift;
    if (!shift) {
        return kept.newest() ? std::format("showing: live, the newest update; {} kept",
                                           kept.kept() == 1 ? std::string{"1 update"} : std::format("{} updates", kept.kept()))
                             : std::string{"showing: live; nothing kept yet"};
    }
    const std::uint64_t behind = shift->newest - shift->shown;
    std::string where = std::format("showing: update {}, {}, {}", shift->shown, shift->playing ? "playing" : "paused",
                                    behind == 1 ? std::string{"1 update"} : std::format("{} updates", behind));
    if (shift->behind_seconds) {
        where += std::format(" ({:.1f} s)", *shift->behind_seconds);
    }
    return where + " behind live";
}

// The height the time-shifting footer takes (draw_time_shift, after a divider): so the market above it
// can end where it begins, and the controls stay at the pane's bottom whatever the lines above say.
float time_shift_height(const ViewerView& view) {
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float half_line = ImGui::GetTextLineHeight() * 0.5f;
    const float divider_height = 2.0f * (half_line + spacing) + 1.0f + spacing;
    const float line = ImGui::CalcTextSize(time_shift_line(view).c_str(), nullptr, false, ImGui::GetContentRegionAvail().x).y + spacing;
    return divider_height + line + half_line + spacing + ImGui::GetFrameHeight() + spacing;
}

// Live, the time-shifting controls (feed/time_shift.h), at the bottom of the market pane, under the
// line saying what the screen shows. Each control asks; the loop moves the books shown
// (ViewerApp::go_to and the rest). At the bottom, as a video player's are, so they stay put however
// the lines above them change as the market moves: they once moved as the mouse reached for them.
void draw_time_shift(const ViewerView& view, ViewerActions& actions) {
    const TimeShiftBuffer& kept = *view.kept;
    const std::optional<TimeShift>& shift = view.state.time_shift;
    wrapped(time_shift_line(view));
    if (!shift) {
        const bool can_pause = kept.newest() && kept.newest() == view.state.live.last_seq();
        paragraph_break();  // the controls apart from the line above them
        if (ImGui::SmallButton(kShiftPause) && can_pause) {
            actions.pause = true;
        }
        tooltip([can_pause] {
            return can_pause ? std::string{"Pause: hold the books as they are now, while the updates arriving are kept"}
                             : std::string{"Pause, once market data has arrived"};
        });
        return;
    }
    tooltip([] {
        return std::string{"The seconds are between the two updates' arrivals here, at the viewer: the market data carries "
                           "no times of its own"};
    });
    paragraph_break();  // the controls apart from the line above them
    const std::uint64_t oldest = kept.runs().front().first;
    if (ImGui::SmallButton(kShiftOldest)) {
        actions.go_to = oldest;
    }
    tooltip([] { return std::string{"To the oldest update kept"}; });
    ImGui::SameLine();
    const auto previous = kept.previous(shift->shown);
    if (ImGui::SmallButton(kShiftBack) && previous) {
        actions.go_to = *previous;
    }
    tooltip([] { return std::string{"Back one update (across a gap, to the last before it)"}; });
    ImGui::SameLine();
    const auto next = kept.next(shift->shown);
    if (ImGui::SmallButton(kShiftForward) && next) {
        actions.go_to = *next;
    }
    tooltip([] { return std::string{"Forward one update (across a gap, to the first after it)"}; });
    ImGui::SameLine();
    if (shift->playing) {
        if (ImGui::SmallButton(kShiftPause)) {
            actions.pause = true;
        }
        tooltip([] { return std::string{"Pause: stay at this update"}; });
    } else {
        if (ImGui::SmallButton(kShiftPlay)) {
            actions.play = true;
        }
        tooltip([] { return std::string{"Play from this update, at the pace the updates arrived, behind live"}; });
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(kShiftLive)) {
        actions.go_live = true;
    }
    tooltip([] { return std::string{"Back to live: the newest update, and every one after"}; });
    ImGui::SameLine();
    std::uint64_t position = shift->shown;
    const std::uint64_t newest = shift->newest;
    if (ImGui::SliderScalar(kShiftSlider, ImGuiDataType_U64, &position, &oldest, &newest) && position != shift->shown) {
        actions.go_to = position;
    }
}

// A replay's position and controls. Each control asks for the books after one update; the loop
// rebuilds them (ViewerApp::replay_to).
void draw_replay(const ViewerView& view, ViewerActions& actions) {
    const Replay& replay = *view.state.replay;
    const std::uint64_t at = view.state.books.last_seq();
    const std::uint64_t last = replay.updates.size();
    wrapped(std::format("replay of {} (recorded in {}): after update {} of {}{}", replay.file.string(), replay.recording, at, last,
                        replay.play.playing ? std::format(", playing at {} updates a second", replay.play.rate) : std::string{}));
    if (ImGui::SmallButton(kReplayStart)) {
        actions.replay_to = 0;
    }
    tooltip([] { return std::string{"To the start: the books before the first update"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kReplayBack) && at > 0) {
        actions.replay_to = at - 1;
    }
    tooltip([] { return std::string{"Back one update"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kReplayForward) && at < last) {
        actions.replay_to = at + 1;
    }
    tooltip([] { return std::string{"Forward one update"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kReplayEnd)) {
        actions.replay_to = last;
    }
    tooltip([] { return std::string{"To the end: the books after the last update"}; });
    ImGui::SameLine();
    std::uint64_t position = at;
    const std::uint64_t first = 0;
    if (ImGui::SliderScalar(kReplaySlider, ImGuiDataType_U64, &position, &first, &last) && position != at) {
        actions.replay_to = position;
    }

    // Playing, and the jumps: each jump's target is found when it is clicked, not every paint.
    if (replay.play.playing) {
        if (ImGui::SmallButton(kReplayPause)) {
            actions.replay_pause = true;
        }
        tooltip([] { return std::string{"Pause: stay at the update shown"}; });
    } else {
        if (ImGui::SmallButton(kReplayPlay)) {
            actions.replay_play = true;
        }
        tooltip([&] {
            return at >= last ? std::string{"Play from the start"} : std::string{"Play on from the update shown, by itself"};
        });
    }
    for (const double rate : kReplayRates) {
        ImGui::SameLine();
        // "###": the ID is what follows alone, since a "/" in it would read as a path to the tests.
        const Label label{"{}/s###replay_rate{}", rate, rate};
        const bool chosen = replay.play.rate == rate;
        if (ImGui::Selectable(label.c_str(), chosen, ImGuiSelectableFlags_None, ImVec2{ImGui::CalcTextSize("1000/s").x, 0.0f})) {
            actions.replay_rate = rate;
        }
        report_marked(label, chosen);
        tooltip([&] { return std::format("Play at {} updates a second", rate); });
    }
    const std::uint32_t ticker = view.state.shown_ticker;
    // A space before each group: the trades, and the slicer's.
    const auto jump = [&](const char* label, Way way, const SessionTruth* slicer, std::string_view what) {
        ImGui::SameLine(0.0f, way == Way::back ? ImGui::GetFontSize() : -1.0f);
        if (ImGui::SmallButton(label)) {
            if (const auto target = nearest_trade(replay.updates, ticker, at, way, slicer)) {
                actions.replay_to = *target;
            }
        }
        tooltip([&] {
            return std::format("To the {} {} on ticker {}{}", way == Way::forward ? "next" : "previous", what, ticker,
                               slicer ? ", as the recording's truth says" : "");
        });
    };
    jump(kReplayPrevTrade, Way::back, nullptr, "trade");
    jump(kReplayNextTrade, Way::forward, nullptr, "trade");
    if (replay.truth) {
        jump(kReplayPrevSlicer, Way::back, &*replay.truth, "trade of the slicer's");
        jump(kReplayNextSlicer, Way::forward, &*replay.truth, "trade of the slicer's");
    }
}

// An answer's controls and where the user is in it. Each control asks for a frame; the loop shows it
// (ViewerApp::answer_to), moving the books with it.
void draw_answer_controls(const AnswerShown& shown, ViewerActions& actions) {
    const LoadedAnswer& answer = *shown.answer;
    const std::size_t last = answer.frames.size() - 1;
    if (ImGui::SmallButton(kAnswerStart)) {
        actions.answer_to = 0;
    }
    tooltip([] { return std::string{"To the start: the introduction, the books empty"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kAnswerBack) && shown.frame > 0) {
        actions.answer_to = shown.frame - 1;
    }
    tooltip([] { return std::string{"Back"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kAnswerForward) && shown.frame < last) {
        actions.answer_to = shown.frame + 1;
    }
    tooltip([] { return std::string{"Forward: a step's question, then the step played"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kAnswerEnd)) {
        actions.answer_to = last;
    }
    tooltip([] { return std::string{"To the end: the last step played"}; });
    // Where the user is, on a line of its own below the buttons, wrapped to the pane. A step that
    // asks first takes two frames, its question and then the step played; each says which, so the
    // two aren't taken for two steps.
    const AnswerFrame& frame = answer.frames[shown.frame];
    const std::size_t steps = answer.scenario.steps.size();
    if (frame.step == 0) {
        const std::size_t asking = answer.frames.size() - 1 - steps;
        wrapped(asking == 0 ? std::format("introduction; {} steps", steps)
                         : std::format("introduction; {} steps, {} asking a question first, so {} presses to the end", steps,
                                       asking, last));
    } else if (frame.asking) {
        wrapped(std::format("step {} of {}: the question", frame.step, steps));
    } else if (!answer.scenario.steps[frame.step - 1].ask.empty()) {
        wrapped(std::format("step {} of {}: what happened", frame.step, steps));
    } else {
        wrapped(std::format("step {} of {}", frame.step, steps));
    }
}

// What one step did: the market data it made, which every trader sees; what each client was told,
// which only that client sees; the book after it; and each claim, marked as this viewer's own check
// of it found it (answer_state.h), not as the answer file says.
void draw_step_played(const LoadedAnswer& answer, std::size_t step, const Book& book) {
    const CheckedStep& checked = answer.checked[step - 1];
    const bool cancel_step = std::holds_alternative<Cancel>(answer.scenario.steps[step - 1].request.action);
    wrapped(answer.scenario.steps[step - 1].say);
    paragraph_break();
    if (checked.update_count == 0) {
        text("Market data: none. It changes no book.");
    } else {
        wrapped("Market data (public: every trader sees this):");
        ImGui::Indent();
        for (std::uint64_t seq = checked.first_update; seq < checked.first_update + checked.update_count; ++seq) {
            // The replay's updates are numbered 1, 2, 3, ..., so update n is updates[n - 1].
            wrapped(describe_update(answer.market_data.updates[seq - 1], answer, cancel_step));
        }
        ImGui::Unindent();
    }
    if (!checked.responses.empty()) {
        paragraph_break();
        wrapped("The exchange tells (privately: each client only its own):");
        ImGui::Indent();
        for (const ClientResponse& response : checked.responses) {
            wrapped(std::format("{}: {}", role_of(answer.scenario, response.client), describe(response)));
        }
        ImGui::Unindent();
    }
    paragraph_break();
    text("The book after this step:");
    ImGui::Indent();
    wrapped(describe_top(book, answer));
    ImGui::Unindent();
    if (!checked.claims.empty()) {
        paragraph_break();
        text("Claims, checked against the replay:");
        ImGui::Indent();
        for (const ClaimResult& claim : checked.claims) {
            ImGui::PushStyleColor(ImGuiCol_Text, claim.holds ? kTrueColor : kErrorColor);
            text(claim.holds ? "true " : "FALSE");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            wrapped(claim.holds ? claim.says : std::format("{} In fact, {}.", claim.says, claim.actually));
        }
        ImGui::Unindent();
    }
}

// `s`, cut to fit `width` with "..." at its end if it is wider: a combo's preview is otherwise cut
// off mid-letter at its arrow.
std::string fit(std::string s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) {
        return s;
    }
    while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) {
        s.pop_back();
    }
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s + "...";
}

// With a course: what the pane shows, the user's latest question or one of its lessons; nothing, until
// one is chosen.
void draw_course_choice(const CourseChoice& course, ViewerActions& actions) {
    const auto label = [&course](std::size_t choice) {
        return choice == 0 ? std::string{"Your latest question"}
                           : std::format("{}. {}", choice, course.course.lessons[choice - 1].title);
    };
    ImGui::SetNextItemWidth(-1.0f);  // the pane's width
    {
        // BeginCombo reports no label to the Test Engine, which finds items by their labels, so it
        // is reported here, under the ID BeginCombo gives it. (The macro refers to the context as g;
        // it is empty in the viewer itself.)
        [[maybe_unused]] ImGuiContext& g = *ImGui::GetCurrentContext();
        IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetID(kLessonChoice), kLessonChoice, ImGuiItemStatusFlags_Openable);
    }
    const std::string shown = fit(course.chosen ? label(*course.chosen) : std::string{"Choose a lesson, or your latest question"},
                                  ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::BeginCombo(kLessonChoice, shown.c_str())) {
        for (std::size_t choice = 0; choice <= course.course.lessons.size(); ++choice) {
            // A std::string, not a Label: a lesson's title may pass a Label's 64 characters, and the
            // cut would take the ##choice that keeps the entries apart.
            if (ImGui::Selectable(std::format("{}##choice{}", label(choice), choice).c_str(), choice == course.chosen) &&
                choice != course.chosen) {
                actions.choose = choice;
            }
        }
        ImGui::EndCombo();
    }
    tooltip([&course] { return std::format("{}: its lessons, in order, or the answer to your latest question", course.course.title); });
    if (course.chosen > 0 && !course.course.lessons[*course.chosen - 1].read.empty()) {
        wrapped(std::format("Read: {}", course.course.lessons[*course.chosen - 1].read));
    }
    divider();
}

// The answer's pane: what it shows, its question, where the user is in it, and that frame's text.
void draw_answer(const ViewerView& view, ViewerActions& actions) {
    const AnswerShown& shown = *view.state.answer;
    if (view.state.course) {
        draw_course_choice(*view.state.course, actions);
        if (!view.state.course->chosen) {
            return;
        }
    }
    if (!shown.answer) {
        if (shown.problem) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            wrapped(std::format("Can't show the answer: {}", *shown.problem));
            ImGui::PopStyleColor();
        } else {
            wrapped(std::format("Waiting for an answer at {}.", shown.file.string()));
        }
        return;
    }
    const LoadedAnswer& answer = *shown.answer;
    // A lesson's title, in the drop-down above, says what its question would; an answer of the
    // user's own is named only by its question.
    if (!view.state.course || view.state.course->chosen == 0) {
        wrapped(answer.scenario.question);
        divider();
    }
    draw_answer_controls(shown, actions);
    divider();
    const AnswerFrame& frame = answer.frames[shown.frame];
    if (frame.step == 0) {
        if (!answer.scenario.intro.empty()) {
            wrapped(answer.scenario.intro);
        }
        paragraph_break();
        text("Press > to begin.");
        return;
    }
    const ScenarioStep& step = answer.scenario.steps[frame.step - 1];
    wrapped(std::format("Step {}. {}", frame.step, describe(step.request, answer.scenario)));
    paragraph_break();
    if (frame.asking) {
        text("The book before this step:");
        ImGui::Indent();
        wrapped(describe_top(view.state.books.book(answer.scenario.ticker), answer));
        ImGui::Unindent();
        paragraph_break();
        wrapped(step.ask);
        paragraph_break();
        text("Press > to see what happens.");
        return;
    }
    draw_step_played(answer, frame.step, view.state.books.book(answer.scenario.ticker));
}

// One button per ticker, its order count in its tooltip; the shown one is marked.
void draw_tickers(const ViewerView& view, ViewerActions& actions) {
    text("ticker");
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        ImGui::SameLine();
        const bool shown = ticker == view.state.shown_ticker;
        const Label label = ticker_label(ticker);
        if (ImGui::Selectable(label.c_str(), shown, ImGuiSelectableFlags_None, ImVec2{ImGui::GetFontSize() * 1.5f, 0.0f})) {
            actions.show_ticker = ticker;
        }
        report_marked(label, shown);
        tooltip([&] {
            return std::format("Show ticker {}'s book ({}): {}", ticker,
                               view.state.books.book(ticker).order_count() == 1
                                   ? std::string{"1 order"}
                                   : std::format("{} orders", view.state.books.book(ticker).order_count()),
                               sends(view.publish_error));
        });
    }
}

// One row of the book: an order, and whether it is the first of its level.
struct BookRow {
    const Level* level;
    std::size_t position;  // in its level's queue, from 0
};

// The shown ticker's book, one row per resting order, asks above bids, each side's best price
// nearest the middle; within a price, orders in the order they will fill. Clicking a row points at
// its order; clicking a price points at its level; clicking an order's id follows it. What is pointed
// at and followed is highlighted as published, like the bar.
void draw_book(const ViewerView& view, ViewerActions& actions) {
    const std::uint32_t ticker = view.state.shown_ticker;
    const Book& book = view.state.books.book(ticker);
    if (book.order_count() == 0) {
        text(std::format("ticker {}: no orders", ticker));
        return;
    }
    // Built every paint: a book holds at most a few hundred levels (the exchange's limit), so this
    // costs little beside drawing them.
    std::vector<Level> asks = book.levels(Side::sell);
    std::vector<Level> bids = book.levels(Side::buy);
    std::vector<BookRow> rows;
    for (auto level = asks.rbegin(); level != asks.rend(); ++level) {  // worst ask at the top
        for (std::size_t i = 0; i < level->orders.size(); ++i) {
            rows.push_back({&*level, i});
        }
    }
    for (const Level& level : bids) {  // best bid at the top of the bids
        for (std::size_t i = 0; i < level.orders.size(); ++i) {
            rows.push_back({&level, i});
        }
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    // Half the remaining height; the followed order's history takes the rest.
    const float height = view.published && view.published->follow ? ImGui::GetContentRegionAvail().y * 0.6f
                                                                  : ImGui::GetContentRegionAvail().y;
    // In an answer, a column says whose each order is, which the answer's replay knows (and the
    // market data doesn't); in a recorded session's replay, the session's truth does.
    const LoadedAnswer* answer = view.state.answer && view.state.answer->answer ? &*view.state.answer->answer : nullptr;
    const SessionTruth* truth = view.state.replay && view.state.replay->truth ? &*view.state.replay->truth : nullptr;
    const bool whose = answer != nullptr || truth != nullptr;
    if (!ImGui::BeginTable(kBookTable, whose ? 7 : 6, flags, ImVec2{0.0f, height})) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);  // the header row stays visible while scrolling
    ImGui::TableSetupColumn("side");
    ImGui::TableSetupColumn("price");
    ImGui::TableSetupColumn("level qty");
    ImGui::TableSetupColumn("queue");
    ImGui::TableSetupColumn("order");
    ImGui::TableSetupColumn("qty");
    if (whose) {
        ImGui::TableSetupColumn("whose");
    }
    ImGui::TableHeadersRow();

    for (const BookRow& row : rows) {
        const Level& level = *row.level;
        const Order& order = level.orders[row.position];
        const OrderRef order_ref{ticker, order.order_id};
        const LevelRef level_ref{ticker, level.side, level.price};
        ImGui::TableNextRow();
        const bool followed = order_followed(order_ref, view.published);
        if (followed) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(kFollowedRowColor));
        }

        ImGui::TableNextColumn();
        // SpanAllColumns makes the whole row the click target; AllowOverlap lets the price's and the
        // id's buttons, drawn on top of it, take their own clicks.
        ImGui::PushStyleColor(ImGuiCol_Text, level.side == Side::buy ? kBidColor : kAskColor);
        const Label row_label = order_label(order);
        const bool pointed = order_pointed(order_ref, view.published);
        if (ImGui::Selectable(row_label.c_str(), pointed, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
            actions.point_at = order_ref;
        }
        ImGui::PopStyleColor();
        report_marked(row_label, pointed);
        tooltip([&] { return std::format("Point at order {}: {}", order.order_id, sends(view.publish_error)); });

        ImGui::TableNextColumn();
        if (row.position == 0) {  // the level's price and total, on its first row
            const Label price_label = level_label(level_ref);
            if (ImGui::SmallButton(price_label.c_str())) {
                actions.point_at = level_ref;
            }
            report_marked(price_label, level_marked(level_ref, view.published));
            tooltip([&] {
                return std::format("Point at the {} level at {}: {}", to_string(level.side), level.price, sends(view.publish_error));
            });
            ImGui::TableNextColumn();
            text(Label{"{}", level.qty}.c_str());
        } else {
            ImGui::TableNextColumn();
        }

        ImGui::TableNextColumn();
        text(Label{"#{}", row.position + 1}.c_str());
        ImGui::TableNextColumn();
        const Label id_label = follow_label(order);
        if (ImGui::SmallButton(id_label.c_str())) {
            actions.follow = order_ref;
        }
        report_marked(id_label, followed);
        tooltip([&] {
            return std::format("Follow order {}: every update to it, and {}", order.order_id, sends(view.publish_error));
        });
        ImGui::TableNextColumn();
        text(Label{"{}", order.qty}.c_str());
        if (answer) {
            ImGui::TableNextColumn();
            const auto owner = answer->owners.find(order.order_id);
            text(owner != answer->owners.end() ? owner->second : std::string{"unknown"});
        } else if (truth) {
            ImGui::TableNextColumn();
            const auto client = truth->truth.client_of(ticker, order.order_id);
            text(client ? client_name(*truth, *client) : std::string{"unknown"});
        }
    }
    ImGui::EndTable();
}

// Tells the UI tests, as report_marked does, what the trade's dot just drawn shows.
void report_trade([[maybe_unused]] const TradeDot& dot) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetItemID(), trade_report(dot).c_str(), ImGui::GetItemStatusFlags());
}

// The trades on the shown ticker up to the update shown, each a dot: its update across, its price up,
// its area as its quantity, on an axis of kTradesWindow updates from update 1 that scrolls once the
// update shown passes its end. In a recorded session's replay, the
// slicer's are coloured, from the truth: filled where its piece came in and took, a ring where its
// resting order was met. Hovering a dot says who traded; clicking it goes to its update.
void draw_trades(const ViewerView& view, ViewerActions& actions) {
    const std::uint32_t ticker = view.state.shown_ticker;
    if (!view.state.replay) {
        wrapped("Trades are drawn in a replay. Live, there is no truth to say who traded: the market data never names a "
                "client.");
        return;
    }
    const Replay& replay = *view.state.replay;
    const SessionTruth* truth = replay.truth ? &*replay.truth : nullptr;
    const std::uint64_t to = view.state.books.last_seq();
    // The axis: kTradesWindow updates, or the whole replay if it is shorter, so a short one's trades
    // aren't crowded together. It starts at update 1 and stays put while the update shown is within
    // it, the dots filling in from the left; after that it scrolls, the update shown at its right.
    const std::uint64_t window = std::min<std::uint64_t>(kTradesWindow, std::max<std::uint64_t>(replay.updates.size(), 2));
    const std::uint64_t from = to > window ? to - window + 1 : 1;  // the axis's first update
    const std::uint64_t axis_end = from + window - 1;
    static const SessionTruth kNoTruth{.directory = {}, .truth = Truth{"", {}, {}}, .roles = {}, .slicers = {}, .slices = {}};
    const std::vector<TradeDot> dots = trade_dots(replay.updates, truth ? *truth : kNoTruth, ticker, from, to);
    wrapped(to == 0 ? std::format("Trades on ticker {}: none yet, before the first update.", ticker)
                    : std::format("Trades on ticker {}, updates {} to {}: {}.", ticker, from, to, dots.size()));
    if (truth) {
        ImGui::PushStyleColor(ImGuiCol_Text, kSlicerColor);
        wrapped("Orange: the slicer's, filled where its piece took, a ring where its resting order was met.");
        ImGui::PopStyleColor();
        wrapped("Grey: everyone else's. Who traded is the recording's truth, which the market data doesn't say.");
        const auto slice = truth->slices.find(ticker);
        wrapped(slice != truth->slices.end() ? std::format("Here the slicer {}.", slice->second)
                                             : std::string{"The slicer has no order on this ticker."});
    } else {
        wrapped("No truth for this replay, so no colours: the market data doesn't say who traded (viewer --session DIR "
                "shows a recorded session's).");
    }
    paragraph_break();

    // The canvas: the price at its left, the updates along its bottom.
    const float font = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y - font * 1.5f, font * 6.0f)};
    const float left = origin.x + font * 3.0f;
    const float right = origin.x + size.x - font * 0.8f;
    const float top = origin.y + font * 0.8f;
    const float bottom = origin.y + size.y - font * 1.6f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect({left, top}, {right, bottom}, ImGui::GetColorU32(ImGuiCol_Border));
    std::int64_t low = 0;
    std::int64_t high = 0;
    std::uint32_t most = 1;
    for (const TradeDot& dot : dots) {
        low = &dot == &dots.front() ? dot.price : std::min(low, dot.price);
        high = &dot == &dots.front() ? dot.price : std::max(high, dot.price);
        most = std::max(most, dot.qty);
    }
    low -= 1;
    high += 1;
    const auto x_of = [&](std::uint64_t update) {
        const double span = static_cast<double>(window - 1);
        return left + static_cast<float>(static_cast<double>(update - from) / span) * (right - left);
    };
    const auto y_of = [&](std::int64_t price) {
        return bottom - static_cast<float>(static_cast<double>(price - low) / static_cast<double>(high - low)) * (bottom - top);
    };
    const ImU32 label_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    if (!dots.empty()) {
        draw->AddText({origin.x, top - font * 0.5f}, label_color, Label{"{}", high}.c_str());
        draw->AddText({origin.x, bottom - font * 0.5f}, label_color, Label{"{}", low}.c_str());
    }

    // The axis's ends under its ends, and a line at the update shown (at the right edge once the axis
    // scrolls).
    draw->AddText({left, bottom + font * 0.2f}, label_color, Label{"{}", from}.c_str());
    const Label end_label{"{}", axis_end};
    draw->AddText({right - ImGui::CalcTextSize(end_label.c_str()).x, bottom + font * 0.2f}, label_color, end_label.c_str());
    if (to > 0) {
        draw->AddLine({x_of(to), top}, {x_of(to), bottom}, ImGui::GetColorU32(ImGuiCol_TextDisabled), font * 0.08f);
    }
    const ImU32 slicer = ImGui::GetColorU32(kSlicerColor);
    const ImU32 other = ImGui::GetColorU32(kTradeColor);
    // Everyone else's first, so the slicer's are drawn over them.
    for (const bool slicers : {false, true}) {
        for (const TradeDot& dot : dots) {
            if ((dot.slicer != TradeDot::Slicer::none) != slicers) {
                continue;
            }
            const float radius = font * (0.15f + 0.35f * std::sqrt(static_cast<float>(dot.qty) / static_cast<float>(most)));
            const ImVec2 centre{x_of(dot.update), y_of(dot.price)};
            if (dot.slicer == TradeDot::Slicer::met) {
                draw->AddCircle(centre, radius, slicer, 0, font * 0.12f);
            } else {
                draw->AddCircleFilled(centre, radius, dot.slicer == TradeDot::Slicer::took ? slicer : other);
            }
            ImGui::SetCursorScreenPos({centre.x - radius, centre.y - radius});
            if (ImGui::InvisibleButton(Label{"##trade{}", dot.update}.c_str(), {radius * 2.0f, radius * 2.0f})) {
                actions.replay_to = dot.update;
            }
            report_trade(dot);
            tooltip([&] {
                std::string said = std::format("Update {}: a {} of {} at {}", dot.update, dot.side == Side::buy ? "buy" : "sell",
                                               dot.qty, dot.price);
                if (truth) {
                    if (const TradeTruth* trade = truth->truth.trade_at(dot.update)) {
                        said += std::format("; the incoming order client {}'s, the resting one client {}'s",
                                            client_name(*truth, trade->incoming_client), client_name(*truth, trade->resting_client));
                    }
                }
                return said + ". Click to go to it.";
            });
        }
    }
    // The canvas's room, taken by an item, as Dear ImGui needs after the cursor has been moved.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(size);
}

// The followed order's history, as published.
void draw_history(const Context::Follow& follow) {
    wrapped(std::format("order {} of ticker {}: {} event{}", follow.order.ref.order_id, follow.order.ref.ticker,
                     follow.event_count, follow.event_count == 1 ? "" : "s"));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(kHistoryTable, 6, flags, ImVec2{0.0f, ImGui::GetContentRegionAvail().y})) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("update");
    ImGui::TableSetupColumn("event");
    ImGui::TableSetupColumn("side");
    ImGui::TableSetupColumn("price");
    ImGui::TableSetupColumn("qty");
    ImGui::TableSetupColumn("seen");
    ImGui::TableHeadersRow();
    for (const OrderEvent& event : follow.events) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        text(Label{"{}", event.seq}.c_str());
        ImGui::TableNextColumn();
        text(to_string(event.type));
        ImGui::TableNextColumn();
        text(to_string(event.side));
        ImGui::TableNextColumn();
        text(Label{"{}", event.price}.c_str());
        ImGui::TableNextColumn();
        text(Label{"{}", event.qty}.c_str());
        ImGui::TableNextColumn();
        text(event.from_snapshot ? "in a snapshot" : "as it happened");
    }
    ImGui::EndTable();
}

}  // namespace

void apply(const ViewerActions& actions, Selection& selection, ViewerState& state) {
    if (actions.show_ticker && *actions.show_ticker < BookBuilder::kTickers) {
        state.shown_ticker = *actions.show_ticker;
    }
    if (actions.point_at) {
        selection.point_at(*actions.point_at);
    }
    if (actions.follow) {
        selection.follow(*actions.follow);
    }
    if (actions.clear_point) {
        selection.clear_point();
    }
    if (actions.clear_follow) {
        selection.clear_follow();
    }
}

bool level_marked(const LevelRef& level, const std::optional<Context>& published) {
    if (!published || !published->point) {
        return false;
    }
    const auto* state = std::get_if<LevelState>(&published->point->target);
    return state && state->ref == level;
}

bool order_pointed(const OrderRef& order, const std::optional<Context>& published) {
    if (!published || !published->point) {
        return false;
    }
    const auto* state = std::get_if<OrderState>(&published->point->target);
    return state && state->ref == order;
}

bool order_followed(const OrderRef& order, const std::optional<Context>& published) {
    return published && published->follow && published->follow->order.ref == order;
}

Label ticker_label(std::uint32_t ticker) { return Label{"{}##ticker", ticker}; }

Label level_label(const LevelRef& level) { return Label{"{}##level{}", level.price, to_string(level.side)}; }

Label order_label(const Order& order) { return Label{"{}##order{}", to_string(order.side), order.order_id}; }

Label follow_label(const Order& order) { return Label{"{}##follow", order.order_id}; }

Label trade_report(const TradeDot& dot) {
    return Label{"{}: {} at {}, {}", dot.update, dot.qty, dot.price,
                 dot.slicer == TradeDot::Slicer::took ? "took" : dot.slicer == TradeDot::Slicer::met ? "met" : "none"};
}

void apply_viewer_look() {
    ImGui::GetIO().Fonts->AddFontDefaultVector();
    ImGui::GetStyle().FontScaleMain = 2.0f;
}

WindowSize window_size(int area_width, int area_height) {
    constexpr WindowSize kRoomy{.width = 1800, .height = 1400};
    if (area_width <= 0 || area_height <= 0) {
        return kRoomy;
    }
    return {.width = std::min(kRoomy.width, area_width), .height = std::min(kRoomy.height, area_height)};
}

ViewerActions draw_viewer(const ViewerView& view) {
    ViewerActions actions;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(kViewerWindow, nullptr, ImGuiWindowFlags_NoDecoration);

    draw_context_bar(view, actions);
    ImGui::Separator();
    // An answer's pane on the left, the market on the right. In an answer the books move only with
    // its frames, so the replay's own controls aren't drawn: the pane and the books can't disagree.
    const bool answering = view.state.answer.has_value();
    if (answering) {
        ImGui::BeginChild(kAnswerPane, {ImGui::GetContentRegionAvail().x * 0.45f, 0.0f}, ImGuiChildFlags_Borders);
        draw_answer(view, actions);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild(kMarketPane);
    }
    paragraph_break();
    draw_status(view);
    if (view.state.replay && !answering) {
        draw_replay(view, actions);
    }
    // Live, the market in a region of its own, ending where the time-shifting footer begins, so the
    // footer's controls stay at the pane's bottom.
    if (view.kept) {
        const float footer = time_shift_height(view);
        ImGui::BeginChild(kLiveMarket, {0.0f, std::max(ImGui::GetContentRegionAvail().y - footer, ImGui::GetFrameHeight() * 4.0f)});
    }
    divider();
    draw_tickers(view, actions);
    paragraph_break();
    // The book, and beside it the trades (not in an answer, whose pane is beside the market).
    if (!answering) {
        const float region = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("book_and_history", {ImGui::GetContentRegionAvail().x * 0.58f, region});
    }
    draw_book(view, actions);
    if (view.published && view.published->follow) {
        divider();
        draw_history(*view.published->follow);
    }
    if (!answering) {
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild(kTradesPanel, {0.0f, ImGui::GetContentRegionAvail().y}, ImGuiChildFlags_Borders);
        draw_trades(view, actions);
        ImGui::EndChild();
    }
    if (view.kept) {
        ImGui::EndChild();
        divider();
        draw_time_shift(view, actions);
    }
    if (answering) {
        ImGui::EndChild();
    }

    ImGui::End();
    return actions;
}

}  // namespace fillscope
