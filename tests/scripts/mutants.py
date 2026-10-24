# Checks that the tests catch bugs: each mutant breaks the code in one deliberate way, and the
# tests named for it must then fail, though they pass on the real code (checked first). It works in
# a scratch copy of the working tree with a build of its own (scratch_tree.py), so the files being
# edited and the build in use are never touched, whatever stops it.
#
#   python3 -I tests/scripts/mutants.py [NAME]     every mutant, or those whose names contain NAME
#
# A mutant whose text no longer matches the source is an error, not a pass: the check must not go
# silently stale when the code changes.
import importlib.util
import pathlib
import re
import shutil
import signal
import subprocess
import sys

# Loading the scripts beside this one by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parents[2]

VIEW = "src/viewer/viewer_view.cpp"
PARSE = "src/common/parse.h"
FD = "src/common/fd.h"
REPLAY_DRIVER = "tests/backend/replay_lldb_driver.py"
CONTEXT = "src/viewer/context.cpp"
PUBLISHER = "src/viewer/context_publisher.cpp"
VIEWER_STATE = "src/viewer/viewer_state.cpp"
VIEWER_STATE_HEADER = "src/viewer/viewer_state.h"
DOC_LINKS = "tests/docs/doc_links_test.cpp"
DEBUG_STATE = "tools/vscode-debug-state/src/debug_state.js"
EXTENSION = "tools/vscode-debug-state/src/extension.js"
ATTRIBUTION = "tools/attribution/attribution.py"
BOOK = "backend/Chapter12/exchange/matcher/me_order_book.cpp"
BOOK_HEADER = "backend/Chapter12/exchange/matcher/me_order_book.h"
MD_PUBLISHER = "backend/Chapter12/exchange/market_data/market_data_publisher.cpp"
SNAPSHOT = "backend/Chapter12/exchange/market_data/snapshot_synthesizer.cpp"
PRICE_LEVELS = "backend/Chapter12/exchange/matcher/price_level_table.h"
UPDATE = "src/feed/market_update.cpp"
BOOK_FEED = "src/feed/book.cpp"
BOOK_FEED_HEADER = "src/feed/book.h"
BUILDER = "src/feed/book_builder.cpp"
FEED_SOCKET = "src/feed/feed_socket.cpp"
VIEWER_OPTIONS = "src/viewer/viewer_options.cpp"
REPLAY = "backend/Chapter12/exchange/matcher/replay.cpp"
RECORDING = "backend/Chapter12/exchange/matcher/request_recording.cpp"
MATCHING_ENGINE = "backend/Chapter12/exchange/matcher/matching_engine.h"
REPLAY_MAIN = "backend/Chapter12/exchange/exchange_replay.cpp"
REQUEST_ANCHOR = "backend/Chapter12/exchange/matcher/lockstep_anchor.cpp"

# (name, file, [(old, new), ...], CTest regex of tests that must fail)
MUTANTS = [
    ("a number is read from only part of the text", PARSE,
     [("if (error != std::errc{} || stopped_at != last) {", "if (error != std::errc{}) {")],
     "ParseNumber.RejectsTextThatIsNotExactlyANumber|ParseViewerArgs.RefusesWhatIsntAnOption"),
    ("a NaN --exit-after is accepted", "src/viewer/viewer_options.cpp",
     [("if (!(*seconds >= 0) || std::isinf(*seconds)) {", "if (*seconds < 0 || std::isinf(*seconds)) {")],
     "ParseViewerArgs.RefusesWhatIsntAnOption"),
    ("an empty --context-file is accepted", "src/viewer/viewer_options.cpp",
     [('                return std::unexpected{"--context-file needs a path"};', '                options.context_file = "";')],
     "ParseViewerArgs.RefusesWhatIsntAnOption"),
    # Each tier's timeout fails a program that grows too slow.
    ("a unit test takes 3 seconds", "tests/common/common_test.cpp",
     [("#include <utility>\n", "#include <thread>\n#include <utility>\n"),
      ("TEST(Fd, StartsEmpty) {\n", "TEST(Fd, StartsEmpty) {\n    std::this_thread::sleep_for(std::chrono::seconds{3});\n")],
     "Fd.StartsEmpty"),
    ("a UI test takes 11 seconds", "tests/viewer/viewer_ui_test.cpp",
     [("int main(int argc, char** argv) {\n", "int main(int argc, char** argv) {\n    std::this_thread::sleep_for(std::chrono::seconds{11});\n"),
      ("#include <deque>\n", "#include <deque>\n#include <thread>\n")],
     "viewer_ui_tests"),
    ("the type check takes 31 seconds", "tests/vscode-debug-state/CMakeLists.txt",
     [("  COMMAND ${FLOCK} --shared ${EXTENSION} ${NODE} ${EXTENSION}/node_modules/typescript/bin/tsc -p ${EXTENSION}",
       "  COMMAND ${CMAKE_COMMAND} -E sleep 31")],
     "vscode_debug_state_typecheck"),
    ("a debugger test takes 31 seconds", REPLAY_DRIVER,
     [("MAX_STOPS = 1000", "import time\ntime.sleep(31)\nMAX_STOPS = 1000")],
     "ReplayLockstep.LldbBreakpointByName"),
    ("a program with no tests passes registration", "tests/scripts/iterate.py",
     [("        if not with_tests.get(program.name):", "        if False:")],
     "scripts_tests"),
    ("a test that copies a program's labels passes registration", "tests/scripts/iterate.py",
     [("        elif program.binary is not None and program.binary not in command:", "        elif False:")],
     "scripts_tests"),
    ("a program that tested nothing is recorded", "tests/scripts/iterate.py",
     [("    return {p.name for p in everything if not by_program.get(p.name) or set(by_program[p.name]) <= skipped}",
       "    return {p.name for p in everything if by_program.get(p.name) and set(by_program[p.name]) <= skipped}")],
     "scripts_tests"),
    ("the sanitizer check ignores UBSan", "tests/scripts/iterate.py",
     [('        for library in ("libasan", "libubsan"):', '        for library in ("libasan",):')],
     "scripts_tests"),
    ("a deleted run-time file crashes the fingerprint", "tests/scripts/iterate.py",
     [("    if not file.exists():\n        return b\"missing\"\n", "")],
     "scripts_tests"),
    ("blame credits a whitespace-only change", ATTRIBUTION,
     [('"blame", "--porcelain", "-w", "-M", "-C",', '"blame", "--porcelain", "-M", "-C",')],
     "attribution_tests"),
    ("blame doesn't follow lines moved between files", ATTRIBUTION,
     [('"blame", "--porcelain", "-w", "-M", "-C",', '"blame", "--porcelain", "-w",')],
     "attribution_tests"),
    ("a co-author whose name starts with Claude counts as Claude", ATTRIBUTION,
     [('    return value == "Claude" or value.startswith("Claude ")', '    return value.startswith("Claude")')],
     "attribution_tests"),
    ("binary files are counted", ATTRIBUTION,
     [('        if added != "-" and path in regular:', '        if path in regular:')],
     "attribution_tests"),
    ("the report counts itself", ATTRIBUTION,
     [("        if path == report_path:\n            continue\n", "")],
     "attribution_tests"),
    ("files nobody wrote are counted", ATTRIBUTION,
     [("        if path in NOT_WRITTEN:", "        if False:")],
     "attribution_tests"),
    ("the check accepts a commit outside HEAD's history", ATTRIBUTION,
     [("    if ancestor.returncode != 0:", "    if False:")],
     "attribution_tests"),
    ("the check doesn't compare the numbers", ATTRIBUTION,
     [("    if report(root, commit, report_path, upstream) != text:", "    if False:")],
     "attribution_tests"),
    ("a submodule's entry is counted as a file", ATTRIBUTION,
     [('            if mode in ("100644", "100755"):', '            if True:')],
     "attribution_tests"),
    # The system tier's timeout must reach its test: at one second, the test (half a minute) fails.
    ("the system tier's timeout is one second", "tests/CMakeLists.txt",
     [("set(FILLSCOPE_TIMEOUT_system 120)", "set(FILLSCOPE_TIMEOUT_system 1)")],
     "backend_system_tests"),
    # The back end's matching and market data, caught by its own tests (one CTest test runs them).
    ("a buy doesn't trade at the ask's price", BOOK,
     [("        if (LIKELY(price < ask_itr->price_)) {", "        if (LIKELY(price <= ask_itr->price_)) {")],
     "lowlatency_tests"),
    ("a fill takes the resting order's whole quantity", BOOK,
     [("    const auto fill_qty = std::min(*leaves_qty, order_qty);", "    const auto fill_qty = order_qty;")],
     "lowlatency_tests"),
    ("a rejected cancel is reported as cancelled", BOOK,
     [("      client_response_ = {ClientResponseType::CANCEL_REJECTED, client_id, ticker_id, order_id, OrderId_INVALID,",
       "      client_response_ = {ClientResponseType::CANCELED, client_id, ticker_id, order_id, OrderId_INVALID,")],
     "lowlatency_tests"),
    ("priorities at a price skip one", BOOK_HEADER,
     [("      return orders_at_price->first_me_order_->prev_order_->priority_ + 1;",
       "      return orders_at_price->first_me_order_->prev_order_->priority_ + 2;")],
     "lowlatency_tests"),
    ("a new order goes to the front of its price's queue", BOOK_HEADER,
     [("        first_order->prev_order_ = order;\n",
       "        first_order->prev_order_ = order;\n        orders_at_price->first_me_order_ = order;\n")],
     "lowlatency_tests"),
    ("a better bid doesn't become the best", BOOK_HEADER,
     [("          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ > best_orders_by_price->price_) ||",
       "          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < best_orders_by_price->price_) ||")],
     "lowlatency_tests"),
    ("a price's level is whatever the probe meets first", PRICE_LEVELS,
     [("        if (level == nullptr || level->price_ == price) {", "        if (true) {")],
     "lowlatency_tests"),
    ("a removal leaves a hole in the probe", PRICE_LEVELS,
     [("        if (distance(home(slots_[slot]->price_), slot) >= distance(hole, slot)) {", "        if (false) {")],
     "lowlatency_tests"),
    ("a removal moves entries that belong after the hole", PRICE_LEVELS,
     [("        if (distance(home(slots_[slot]->price_), slot) >= distance(hole, slot)) {",
       "        if (distance(home(slots_[slot]->price_), slot) < distance(hole, slot)) {")],
     "lowlatency_tests"),
    ("the incremental feed's sequence number doesn't advance", MD_PUBLISHER,
     [("        ++next_inc_seq_num_;\n", "")],
     "lowlatency_tests"),
    ("a snapshot ignores a modify", SNAPSHOT,
     [("        order->qty_ = me_market_update.qty_;\n", "")],
     "lowlatency_tests"),
    ("a snapshot's start doesn't say where to resume", SNAPSHOT,
     [("    const MDPMarketUpdate start_market_update{snapshot_size++, {MarketUpdateType::SNAPSHOT_START, last_inc_seq_num_}};",
       "    const MDPMarketUpdate start_market_update{snapshot_size++, {MarketUpdateType::SNAPSHOT_START, 0}};")],
     "lowlatency_tests"),
    # Fillscope's reading of the market data (src/feed/).
    ("the price is read from the side's byte", UPDATE,
     [("kSideAt = 21, kPriceAt = 22,", "kSideAt = 21, kPriceAt = 21,")],
     "MarketUpdate.DecodesTheBytesTheExchangeSends|MarketUpdate.EncodingThenDecoding"),
    ("an update of an unknown type is decoded", UPDATE,
     [("type > static_cast<std::uint8_t>(UpdateType::snapshot_end)", "type > 200")],
     "MarketUpdate.RejectsBytes"),
    ("a datagram's partial update is decoded", UPDATE,
     [("    if (datagram.size() % kUpdateSize != 0) {", "    if (false) {")],
     "MarketUpdate.RejectsBytes|FeedSocket.SaysWhyADatagram"),
    ("a price's queue is filled latest first", BOOK_FEED_HEADER,
     [("    using Queue = std::map<std::uint64_t, std::uint64_t>;", "    using Queue = std::map<std::uint64_t, std::uint64_t, std::greater<>>;")],
     "Book.KeepsEachPricesOrdersInPriorityOrder"),
    ("the lowest bid comes first", BOOK_FEED_HEADER,
     [("    std::map<std::int64_t, Queue, std::greater<>> bids_;", "    std::map<std::int64_t, Queue> bids_;")],
     "Book.KeepsEachPricesOrdersInPriorityOrder + ui:shows_the_book_asks_above_bids_each_queue_in_fill_order"),
    ("a modify at another price is accepted", BOOK_FEED,
     [("        if (found->second.side != update.side || found->second.price != update.price) {", "        if (false) {")],
     "Book.RejectsAnUpdateItCantFollow"),
    ("a priority already taken is accepted", BOOK_FEED,
     [("        if (const auto holder = update.side == Side::buy ? taken(bids_) : taken(asks_)) {",
       "        if (const auto holder = std::optional<std::uint64_t>{}) {")],
     "Book.RejectsAnUpdateItCantFollow"),
    ("an emptied level stays", BOOK_FEED,
     [("            if (level->second.empty()) {", "            if (false) {")],
     "Book.AModifyChangesTheQuantityAndACancelRemoves"),
    ("a gap goes unnoticed", BUILDER,
     [("    if (update.seq != next_seq_) {", "    if (false) {")],
     "BookBuilder.AGapStartsRecovery|BookBuilder.JoiningAFeedAlreadyRunning"),
    ("a snapshot with a gap is used", BUILDER,
     [("        if (seq != expected++) {", "        if (seq != expected++ && false) {")],
     "BookBuilder.AnIncompleteSnapshotIsNotUsed"),
    ("a gap after the snapshot goes unnoticed", BUILDER,
     [("        if (seq != next) {", "        if (false) {")],
     "BookBuilder.ASnapshotThatEndsBeforeTheQueuedUpdatesBegin"),
    ("a failed rebuild keeps the half-built books", BUILDER,
     [("            books_ = std::move(saved_books);\n", "")],
     "BookBuilder.AnInconsistentSnapshotLeavesTheBooksAsTheyWere"),
    ("a failed rebuild keeps the events it added", BUILDER,
     [("                events.pop_back();\n", "")],
     "BookBuilder.AnInconsistentSnapshotLeavesTheBooksAsTheyWere"),
    ("a rebuild is recorded only once the queued updates apply", BUILDER,
     [("    // The books are rebuilt: whatever happens to the queued updates, this is a recovery.\n"
       "    recoveries_.push_back(Recovery{.after_seq = recovery_after_, .snapshot_seq = snapshot_seq,\n"
       "                                   .reason = recovering_because_.value_or(\"\")});\n", ""),
      ("    status_ = Status::live;\n    recovering_because_.reset();",
       "    recoveries_.push_back(Recovery{.after_seq = recovery_after_, .snapshot_seq = snapshot_seq,\n"
       "                                   .reason = recovering_because_.value_or(\"\")});\n"
       "    status_ = Status::live;\n    recovering_because_.reset();")],
     "BookBuilder.ARebuildIsRecordedEvenWhenAQueuedUpdateAfterItFails"),
    ("two snapshots joined at a loss are used", BUILDER,
     [("    if (queued_snapshot_.rbegin()->second.order_id != snapshot_seq) {", "    if (false) {")],
     "BookBuilder.TwoSnapshotsJoinedAtALossAreNotUsed"),
    ("snapshots are used while live", BUILDER,
     [("    if (status_ != Status::recovering) {\n        return;\n    }\n", "")],
     "BookBuilder.SnapshotsAreIgnoredWhileLive"),
    ("an order found in a snapshot is said to have been seen arriving", BUILDER,
     [("                                                                    .from_snapshot = from_snapshot});",
       "                                                                    .from_snapshot = false});")],
     "BookBuilder.AGapStartsRecovery|MakeContext.AFollowedOrdersGaps"),
    ("a socket receives every group on its port", FEED_SOCKET,
     [("    const sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(stream.port), .sin_addr = group, .sin_zero = {}};",
       "    const sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(stream.port), .sin_addr = {htonl(INADDR_ANY)}, .sin_zero = {}};")],
     "FeedSocket.IgnoresAnotherGroupOnTheSamePort"),
    # The viewer: its context, view, loop and options.
    ("nothing is ahead of an order in its queue", CONTEXT,
     [("                ++state.orders_ahead;\n", "")],
     "MakeContext.ARestingOrderPointedAt|ContextJson.APointedOrderCarries|ContextBar.SaysWhereAPointedOrderIs + ui:clicking_an_order_points_at_it"),
    ("every recovery is a gap in a followed order's history", CONTEXT,
     [("                    if (recovery.snapshot_seq >= history.front().seq) {", "                    if (true) {")],
     "MakeContext.AFollowedOrdersGaps"),
    ("the earliest recoveries are listed", CONTEXT,
     [("    const auto latest = recoveries.last(std::min(recoveries.size(), kMaxListedRecoveries));",
       "    const auto latest = recoveries.first(std::min(recoveries.size(), kMaxListedRecoveries));")],
     "MakeContext.OnlyTheLatestRecoveries"),
    ("every order is marked pointed at", VIEW,
     [("    const auto* state = std::get_if<OrderState>(&published->point->target);\n    return state && state->ref == order;",
       "    const auto* state = std::get_if<OrderState>(&published->point->target);\n    return state != nullptr;")],
     "ui:clicking_an_order_points_at_it"),
    ("the best ask is drawn at the top", VIEW,
     [("    for (auto level = asks.rbegin(); level != asks.rend(); ++level) {", "    for (auto level = asks.begin(); level != asks.end(); ++level) {")],
     "ui:shows_the_book_asks_above_bids_each_queue_in_fill_order"),
    ("clicking an id also points at its order", VIEW,
     [("            actions.follow = order_ref;\n", "            actions.follow = order_ref;\n            actions.point_at = order_ref;\n")],
     "ui:clicking_an_id_follows_the_order"),
    ("choosing a ticker shows nothing new", VIEW,
     [("        state.shown_ticker = *actions.show_ticker;\n", "")],
     "ui:choosing_a_ticker_shows_its_book"),
    ("the snapshot stream is read but not applied", "src/viewer/viewer_app.cpp",
     [("        apply_snapshot(update);\n", "")],
     "ui:reads_both_market_data_streams"),
    ("an error receiving the incremental stream isn't kept", "src/viewer/viewer_app.cpp",
     [("    if (const auto result = incremental.receive(received_, kMaxDatagramsPerPaint); result.error) {\n        state_.feed_error = result.error;\n",
       "    if (const auto result = incremental.receive(received_, kMaxDatagramsPerPaint); result.error) {\n")],
     "ui:reads_both_market_data_streams"),
    ("the seconds since the last update are always zero", "src/viewer/viewer_app.cpp",
     [("        timing.seconds_since_update = std::chrono::duration<double>(now - *last_update_at_).count();",
       "        timing.seconds_since_update = 0.0;")],
     "ui:the_seconds_since_the_last_update_count_from_it"),
    ("a stream that isn't multicast is accepted", VIEWER_OPTIONS,
     [("(ntohl(address.s_addr) >> 28) != 0xE", "false")],
     "ParseViewerArgs.RefusesWhatIsntAnOptionAStream"),
    # Deterministic replay: the back end's recording and replay, and the viewer's replay mode.
    ("the matcher records nothing", MATCHING_ENGINE,
     [("            recorder_->record(*me_client_request);\n", "")],
     "feed_system_tests"),
    ("a replay numbers its updates from 0", REPLAY,
     [("    size_t next_seq = 1;  // as MarketDataPublisher::next_inc_seq_num_ starts", "    size_t next_seq = 0;")],
     "lowlatency_tests|feed_system_tests"),
    ("a recording's partial last request is read", RECORDING,
     [("    if (body % sizeof(MEClientRequest) != 0) {", "    if (false) {")],
     "lowlatency_tests"),
    ("a market data file's numbering isn't checked", "src/feed/market_data_file.cpp",
     [("        if ((*updates)[i].seq != i + 1) {", "        if (false) {")],
     "MarketDataFileTest.SaysWhyAFileCantBeRead"),
    ("going back in a replay leaves the books where they were", "src/viewer/viewer_app.cpp",
     [("    if (seq < state_.books.last_seq()) {\n        state_.books = BookBuilder{};\n    }\n", "")],
     "ui:a_replay_goes_to_any_update"),
    ("a replay address names the replay's end, not the books' update", CONTEXT,
     [("                             .seq = books.last_seq()};", "                             .seq = state.replay->updates.size()};")],
     "MakeContext.InAReplayPointAndFollow|ContextJson.AReplaySaysWhich + ui:a_replay_goes_to_any_update"),
    ("live, the context claims a replay address", CONTEXT,
     [("    if (state.replay) {\n        feed.replay = ReplayAddress{", "    if (true) {\n        feed.replay = ReplayAddress{")],
     "MakeContext.LiveThereIsNoReplayAddress|ContextJson.AReplaySaysWhich"),
    # Lockstep on a replay: the replay sends each request's updates, then calls the anchor.
    ("the replay calls the anchor before it publishes", REPLAY_MAIN,
     [("    if (socket) {\n      // As the exchange's publisher sends them:",
       "    fillscope_request_anchor(request_number, sent + made.size());\n    if (socket) {\n      // As the exchange's publisher sends them:"),
      ("    fillscope_request_anchor(request_number, sent);\n", "")],
     "ReplayLockstep.Lldb"),
    ("the replay's updates are still buffered at the anchor", REPLAY_MAIN,
     [("      socket->sendAndRecv();\n", "")],
     "ReplayLockstep.Lldb"),
    ("the request anchor's self-stop skips the tracer check", REQUEST_ANCHOR,
     [("  } else if (*traced) {\n", "  } else {\n")],
     "lowlatency_tests"),
    ("the request anchor reads TracerPid anywhere in a line", REQUEST_ANCHOR,
     [("      if (line.starts_with(key)) {", "      if (line.find(key) != std::string_view::npos) {"),
      ("        auto value = line.substr(key.size());", "        auto value = line.substr(line.find(key) + key.size());")],
     "lowlatency_tests"),
    ("the request anchor's self-stop is reported past its own line", REQUEST_ANCHOR,
     [('    asm volatile("int3\\n\\tnop" ::"r"(request_number), "r"(updates_published));',
       '    asm volatile("int3" ::"r"(request_number), "r"(updates_published));\n    Exchange::g_anchored = false;')],
     "ReplayLockstep.GdbSelfStop|ReplayLockstep.LldbSelfStop"),
    ("at -O2 the request anchor's arguments can't be read", REQUEST_ANCHOR,
     [('    asm volatile("int3\\n\\tnop" ::"r"(request_number), "r"(updates_published));',
       '    asm volatile("int3\\n\\tnop");\n    (void)updates_published;')],
     "ReplayLockstep.AtO2GdbSelfStop|ReplayLockstep.AtO2LldbSelfStop"),
    ("the request anchor takes any TracerPid for a tracer", REQUEST_ANCHOR,
     [("        return error == std::errc{} && ptr == value.data() + value.size() && pid != 0;",
       "        return error == std::errc{} && ptr == value.data() + value.size();")],
     "lowlatency_tests"),
    ("the request anchor doesn't record the request", REQUEST_ANCHOR,
     [("  Exchange::g_last_request = request_number;", "  Exchange::g_last_request = 0;")],
     "lowlatency_tests"),
    ("--self-stop does nothing", REQUEST_ANCHOR,
     [("  auto setSelfStop(bool on) -> void { g_self_stop = on; }", "  auto setSelfStop(bool) -> void {}")],
     "ReplayLockstep.GdbSelfStop|ReplayLockstep.LldbSelfStop"),
    # The bug found in VS Code with the first anchor: its first statement an std::atomic store,
    # inlined from libstdc++ even at -O0. LLDB's breakpoint by name lands in atomic_base.h; GDB's
    # stays in our file, so only the LLDB test can catch it.
    ("the request anchor's first statement inlined from a header", REQUEST_ANCHOR,
     [("#include <array>\n", "#include <array>\n#include <atomic>\n"),
      ("    uint64_t g_last_request = 0;", "    std::atomic<uint64_t> g_last_request{0};"),
      ("  Exchange::g_last_request = request_number;",
       "  Exchange::g_last_request.store(request_number, std::memory_order_release);")],
     "ReplayLockstep.LldbBreakpointByName"),
    # The driver counts what arrived at each stop; a miscount must fail the lockstep check.
    ("the LLDB driver miscounts the updates", REPLAY_DRIVER,
     [("                self.received += len(self.socket.recv(65536)) // UPDATE_SIZE",
       "                self.received += len(self.socket.recv(65536)) // UPDATE_SIZE + 1")],
     "ReplayLockstep.Lldb"),
    # Fillscope's shared helpers (src/common/).
    ("an Fd doesn't close its descriptor", FD,
     [("            ::close(fd_);\n", "")],
     "Fd.ClosesOnDestruction|Fd.ResetCloses|Fd.AMoveLeaves|Fd.MoveAssignment"),
    ("a moved-from Fd keeps its descriptor", FD,
     [("    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}", "    Fd(Fd&& other) noexcept : fd_{other.fd_} {}")],
     "Fd.AMoveLeavesTheSourceEmpty"),
    ("move assignment leaks the old descriptor", FD,
     [("            reset();\n            fd_ = std::exchange(other.fd_, -1);", "            fd_ = std::exchange(other.fd_, -1);")],
     "Fd.MoveAssignmentClosesTheOldDescriptor"),
    ("self-move-assignment closes the descriptor", FD,
     [("        if (this != &other) {\n            reset();", "        if (true) {\n            reset();")],
     "Fd.SelfMoveAssignment"),
    ("a number too large is called not a number", PARSE,
     [("    if (error == std::errc::result_out_of_range) {", "    if (false) {")],
     "ParseNumber.RejectsAValue"),
    ("a fork's upstream lines are counted as the owner's", ATTRIBUTION,
     [("        counts[0 if sha in upstream else 2 if sha in coauthored else 1] += 1",
       "        counts[2 if sha in coauthored else 1] += 1")],
     "attribution_tests"),
    ("a file changed since upstream is counted as upstream's", ATTRIBUTION,
     [("        if path in changed:\n", "        if False:\n")],
     "attribution_tests"),
    ("a fork's report is checked without its upstream", ATTRIBUTION,
     [("    upstream = upstream.group(1) if upstream else None\n", "    upstream = None\n")],
     "attribution_tests"),
    ("finish publishes nothing", "src/viewer/viewer_app.cpp",
     [("    publisher_.finish(make_context(state_, selection_));\n", "")],
     "ui:finish_publishes_that_the_viewer_has_stopped"),
    ("a publishing failure is never reported", "src/viewer/viewer_app.cpp",
     [("    if (error == reported_error_) {\n", "    if (true) {\n")],
     "ui:publishing_failing_and_recovering_is_reported_once_each"),
    ("a user action waits like a background change", PUBLISHER,
     [("if (trigger == PublishTrigger::background && last_attempt_ && now - *last_attempt_ < min_interval_) {",
       "if (last_attempt_ && now - *last_attempt_ < min_interval_) {")],
     "Publisher.AUserActionIsWrittenAtOnce"),
    ("an unchanged context is rewritten", PUBLISHER,
     [("    if (published_ == context && !error_) {\n", "    if (false) {\n")],
     "Publisher.AnUnchangedContextIsNotRewritten"),
    # The bar draws what was published; claiming a failed write as published would make it lie.
    ("a failed write is claimed as published", PUBLISHER,
     [("        error_ = std::move(written.error());  // the bar shows this\n",
       "        error_ = std::move(written.error());\n        published_ = context;\n")],
     "Publisher.AFailedWriteKeeps + ui:a_failed_write_shows_in_the_bar_and_the_book"),
    ("a failed write leaves the old file", PUBLISHER,
     [("        if (::unlink(file_.c_str()) == 0 || errno", "        if (false || errno")],
     "Publisher.AFailedWriteRemovesTheOldFile + node:a failed write removes the old file, is logged, and the next write works again"),
    ("a failed write is retried only when the context changes", PUBLISHER,
     [("    if (published_ == context && !error_) {", "    if (published_ == context) {")],
     "Publisher.AFailedWriteIsRetriedWhenTheContextReturns"),
    ("a file gone before a failed write is still claimed", PUBLISHER,
     [("        if (::unlink(file_.c_str()) == 0 || errno == ENOENT || errno == EISDIR) {",
       "        if (::unlink(file_.c_str()) == 0 || errno == EISDIR) {")],
     "Publisher.AFailedWriteWithTheFileGoneClaimsNothing"),
    ("a directory in the file's place is claimed to hold the context", PUBLISHER,
     [("        if (::unlink(file_.c_str()) == 0 || errno == ENOENT || errno == EISDIR) {",
       "        if (::unlink(file_.c_str()) == 0 || errno == ENOENT) {")],
     "Publisher.AFailedWriteIsRetriedAndRecovers"),
    ("a replay's status speaks of the exchange", VIEW,
     [("    if (state.replay) {\n        // A replay is read from a file", "    if (false) {\n        // A replay is read from a file")],
     "ui:a_replay_that_cant_be_followed_says_where_it_stopped"),
    ("a stuck replay is fed its updates again", "src/viewer/viewer_app.cpp",
     [("         next <= seq && state_.books.status() != BookBuilder::Status::recovering; ++next) {",
       "         next <= seq; ++next) {")],
     "ui:a_replay_that_cant_be_followed_says_where_it_stopped"),
    ("the bar doesn't say a replay is stuck", CONTEXT,
     [("        if (feed.status == BookBuilder::Status::recovering) {\n            text.background += std::format(\", stuck",
       "        if (false) {\n            text.background += std::format(\", stuck")],
     "ui:a_replay_that_cant_be_followed_says_where_it_stopped"),
    ("an order that left during a recovery claims its last update as how it left", CONTEXT,
     [("        if (!state.resting && state.last_event->type != UpdateType::cancel) {", "        if (false) {")],
     "MakeContext.AnOrderThatLeftDuringARecoverySaysSo"),
    ("a cancelled order is said to have left during a recovery", CONTEXT,
     [("        if (!state.resting && state.last_event->type != UpdateType::cancel) {", "        if (!state.resting) {")],
     "MakeContext.AnOrderThatLeftDuringARecoverySaysSo"),
    ("--replay with a stream is accepted", VIEWER_OPTIONS,
     [("    if (options.replay && stream_given) {", "    if (false) {")],
     "ParseViewerArgs.RefusesWhatIsntAnOption"),
    ("a second signal is caught too", "src/viewer/stop_signals.cpp",
     [("    action.sa_flags = SA_RESETHAND;", "    action.sa_flags = 0;")],
     "StopSignals.ASecondSignalEndsTheProcess"),
    ("a signal ends the viewer at once", "src/viewer/stop_signals.cpp",
     [("    action.sa_handler = request_stop;", "    action.sa_handler = SIG_DFL;")],
     "StopSignals"),
    ("an infinite --exit-after is accepted", "src/viewer/viewer_options.cpp",
     [("if (!(*seconds >= 0) || std::isinf(*seconds)) {", "if (!(*seconds >= 0)) {")],
     "ParseViewerArgs"),
    ("a bare file name is refused", PUBLISHER,
     [("if (const std::filesystem::path directory = file.parent_path(); !directory.empty()) {",
       "if (const std::filesystem::path directory = file.parent_path(); true) {")],
     "Publisher.ABareFileName"),
    ("a failed write leaves its temporary file", PUBLISHER,
     [("        ::unlink(temporary.c_str());\n", "")],
     "Publisher.AFailedWriteIsRetried + node:a failed write throws and leaves no temporary file"),
    ("finish says the viewer is still running", PUBLISHER,
     [("writer_.running = false;", "writer_.running = true;")],
     "Publisher.FinishSays"),
    ("a document links to a file that doesn't exist", "README.md",
     [("(docs/RUNNING.md)", "(docs/RUNNING-missing.md)")],
     "DocLinks.EveryRelativeLinkResolves"),
    ("links inside code blocks are followed", DOC_LINKS,
     [("        if (in_code_block) {\n            continue;\n        }\n", "")],
     "DocLinks.FindsRelativeLinksOnly"),
    ("a stale stop is published", DEBUG_STATE,
     [("            if (isStale()) {\n                return;  // the program moved on", "            if (false) {\n                return;  // the program moved on")],
     "node:a stop whose details arrive after the program resumed is not published, and asks nothing more|of two overlapping stops, only the later is published|a session that ends while a stop is being read stays ended"),
    ("only 'continue' resumes", DEBUG_STATE,
     [("        if (RESUMING_REQUESTS.has(request.command) || MOVING_REQUESTS.has(request.command)",
       "        if (request.command === 'continue' || MOVING_REQUESTS.has(request.command)")],
     "node:every resuming request publishes running once the debugger accepts it, not when it is sent|stepping another thread alone leaves the published stop|a stop whose details arrive after the program resumed is not published, and asks nothing more"),
    ("a refused resume is published as running", DEBUG_STATE,
     [("        if (!response.success) {\n", "        if (false) {\n")],
     "node:a refused resuming request leaves the stop published|a launch that fails leaves the session starting, never running"),
    ("a late resume hides a newer stop", DEBUG_STATE,
     [("        if (record.generation !== pending.generation) {\n", "        if (false) {\n")],
     "node:a resume accepted after a newer stop arrived does not hide that stop"),
    ("an earlier incarnation's calls are obeyed", DEBUG_STATE,
     [("        return incarnation === undefined || this.records.get(id)?.incarnation === incarnation;", "        return true;")],
     "node:calls from a session's earlier incarnation change nothing|after a restart under the same id, the old adapter's late exit and messages change nothing"),
    ("an ended session is changed by later events", DEBUG_STATE,
     [("        if (record === undefined || this.hasEnded(session.id) || !this.isCurrent(session.id, incarnation)) {\n            return;\n        }\n        if (message?.type === 'response') {",
       "        if (record === undefined) {\n            return;\n        }\n        if (message?.type === 'response') {")],
     "node:nothing changes a session after it has ended|calls from a session's earlier incarnation change nothing|after a restart under the same id, the old adapter's late exit and messages change nothing"),
    ("the debugger's frame estimate decides completeness", DEBUG_STATE,
     [("    const complete = got < asked;", "    const complete = false;")],
     "node:a stop publishes the stack, the locals and the stop reason|an empty stack says there were no locals to read|a stack returned whole has an exact count, even when the debugger's total says more"),
    ("another thread continuing clears the stop", DEBUG_STATE,
     [("if (body?.allThreadsContinued === false && this.isAnotherThread(session.id, body.threadId)) {", "if (false) {")],
     "node:another thread continuing alone leaves the published stop; the stopped thread continuing clears it|a stop being read survives another thread resuming alone|a stop that names no thread publishes whose stack it shows, and keeps it against another thread's resume"),
    ("the stop being read isn't counted as the stopped thread", DEBUG_STATE,
     [("        if (record?.describing && record.describing.generation === record.generation) {", "        if (false) {")],
     "node:a stop being read survives another thread resuming alone"),
    ("continue's response is taken for every thread", DEBUG_STATE,
     [("if (pending.command === 'continue' && allContinued === false && this.isAnotherThread(session.id, pending.threadId)) {",
       "if (false) {")],
     "node:a continue the debugger says resumed only another thread leaves the stop"),
    ("a thread-less stop's thread isn't the stopped one", DEBUG_STATE,
     [("                stopped = entry.framesThreadId ?? entry.stop.threadId;", "                stopped = entry.stop.threadId;")],
     "node:a stop that names no thread publishes whose stack it shows, and keeps it against another thread's resume"),
    ("goto is published as running", DEBUG_STATE,
     [("            this.change(session, { state: 'moving' });", "            this.change(session, { state: 'running' });")],
     "node:goto and restartFrame make the stop out of date without claiming the program runs"),
    ("terminated is published as exited", DEBUG_STATE,
     [("                this.change(session, { state: 'terminated' });", "                this.change(session, { state: 'exited', exitCode: null });")],
     "node:\"terminated\" says debugging ended, not that the program exited"),
    ("a session is running before its launch succeeds", DEBUG_STATE,
     [("        this.publish(session, { state: 'starting' });", "        this.publish(session, { state: 'running' });")],
     "node:a session that ends is published as ended, and dropped when a new session starts|a session is starting until its launch is accepted, then running|a launch that fails leaves the session starting, never running"),
    ("an expensive scope named Locals is read", DEBUG_STATE,
     [("    const cheap = scopes.filter((s) => !s.expensive);", "    const cheap = scopes;")],
     "node:without a scope named for locals, the first inexpensive one is read, never an expensive one|the locals are the scope the debugger marks as such, else the one named so, never an expensive one"),
    ("any scope containing 'local' is the locals", DEBUG_STATE,
     [("        ?? cheap.find((s) => /^locals?$/i.test(s.name))", "        ?? cheap.find((s) => /local/i.test(s.name))")],
     "node:the locals are the scope the debugger marks as such, else the one named so, never an expensive one"),
    ("a stop's text is published as its description", DEBUG_STATE,
     [("            description: body?.description ?? null,", "            description: body?.description ?? body?.text ?? null,")],
     "node:a stop's text and description are published as the debugger gave them"),
    ("every structured local is expanded", DEBUG_STATE,
     [("            if (expanded === EXPANDED_LIMIT) {\n", "            if (false) {\n")],
     "node:only the first few structured locals are expanded, and the rest say so"),
    ("a stale stop keeps asking", DEBUG_STATE,
     [("    const ask = async (command, args) => {\n        if (isStale()) {\n", "    const ask = async (command, args) => {\n        if (false) {\n")],
     "node:a stop whose details arrive after the program resumed is not published, and asks nothing more|a stop that goes stale while members are being read asks nothing more|every write by the writer has a higher sequence number, its last one too"),
    ("stop() doesn't take a new sequence number", DEBUG_STATE,
     [("    stop() {\n        this.seq += 1;\n", "    stop() {\n")],
     "node:the output line names the session, the place and the counts|every write by the writer has a higher sequence number, its last one too"),
    ("a failed write leaves its temporary file", DEBUG_STATE,
     [("        io.rmSync(temporary, { force: true });\n", "")],
     "node:a failed write throws and leaves no temporary file"),
    # Caught by a test that watches the calls, so it doesn't depend on a race with a reader.
    ("the file is written in place", DEBUG_STATE,
     [("        io.writeFileSync(temporary, JSON.stringify(state, null, 1) + '\\n');\n        io.renameSync(temporary, file);\n",
       "        io.writeFileSync(file, JSON.stringify(state, null, 1) + '\\n');\n")],
     "node:the file is written to a temporary file in its directory, then renamed over the old one|a reader never sees part of a write|a failed write removes the old file, is logged, and the next write works again"),
    ("a session in any folder is published", EXTENSION,
     [("return isFillscope(session.workspaceFolder) ? session.workspaceFolder : undefined;", "return session.workspaceFolder;")],
     "node:a session outside a Fillscope folder gets no tracker and writes nothing|in a workspace with a Fillscope folder, a session of another folder is still not published"),
    ("a session of another folder goes to the Fillscope folder", EXTENSION,
     [("    if (session.workspaceFolder !== undefined) {\n        return isFillscope(session.workspaceFolder) ? session.workspaceFolder : undefined;\n    }\n", "")],
     "node:in a workspace with a Fillscope folder, a session of another folder is still not published|each Fillscope folder has its own file, including one added after activation"),
    ("shutting down leaves the sessions as they were", EXTENSION,
     [("        publisher.stop();\n", "")],
     "node:when VS Code shuts the extension down, the file says so"),
    ("a normal adapter exit is published as a failure", EXTENSION,
     [("    if (code === 0) {\n        return null;\n    }\n", "")],
     "node:an adapter process that exits says why only when it didn't end normally"),
    ("a session ended by VS Code otherwise stays", EXTENSION,
     [("            entry.publisher.sessionEnded(session.id, null, entry.incarnation);\n", "")],
     "node:a session VS Code ends any other way is ended too"),
    ("a restarted session's old adapter is obeyed", EXTENSION,
     [("                onExit: (code) => publisher.sessionEnded(session.id, adapterExitReason(code), incarnation),",
       "                onExit: (code) => publisher.sessionEnded(session.id, adapterExitReason(code)),")],
     "node:after a restart under the same id, the old adapter's late exit and messages change nothing"),
    ("a failed write leaves the old file", EXTENSION,
     [("                fs.rmSync(file, { force: true });\n", "")],
     "node:a failed write removes the old file, is logged, and the next write works again"),
    ("a symlinked folder gets a publisher of its own", EXTENSION,
     [("        const key = fs.realpathSync(folder.uri.fsPath);", "        const key = folder.uri.fsPath;")],
     "node:two workspace folders that reach one directory share one file"),
    ("unread locals published as none", DEBUG_STATE,
     [("        details.variables = null;\n        details.variablesError = messageOf(error);",
       "        details.variables = [];\n        details.variablesError = messageOf(error);")],
     "node:locals that could not be read are null with a reason, never an empty list|failed scopes leave the locals unread, with the reason"),
    # Only the type check can catch this one: at run time the misspelt property is just undefined.
    ("a misspelt property", DEBUG_STATE,
     [("            text: body?.text ?? null,", "            text: body?.txt ?? null,")],
     "vscode_debug_state_typecheck"),
]


def run(args, cwd=None):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=cwd)


def load_scratch_tree():
    """scratch_tree.py, beside this script. Loaded by path: python3 -I puts no script directory on
    the import path."""
    spec = importlib.util.spec_from_file_location("scratch_tree", pathlib.Path(__file__).with_name("scratch_tree.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Runner:
    """Runs the tests a mutant names, in the scratch copy's build. A name is one of:
      a CTest regular expression, as `ctest -R` takes;
      "ui:FILTER", Dear ImGui Test Engine tests, as viewer_ui_tests takes a filter (comma-separated
        parts of test names);
      "node:PATTERN", the extension's Node tests whose names match, as `node --test-name-pattern`
        takes;
    and several, joined by " + ", are all run. Each check returns (passed, ran any)."""

    def __init__(self, copy, build):
        self.copy = copy
        self.build = build
        self.node = shutil.which("node") or "node"
        self.node_tests = sorted(str(p) for p in (copy / "tests" / "vscode-debug-state").glob("*.test.js"))

    def check(self, spec):
        results = [self.check_one(part.strip()) for part in spec.split(" + ")]
        return all(passed for passed, _ in results), all(ran for _, ran in results)

    def check_one(self, spec):
        if spec.startswith("ui:"):
            result = run([str(self.build / "tests" / "viewer" / "viewer_ui_tests"), spec[len("ui:"):]], cwd=self.build)
            ran = re.search(r"passed of ([1-9]\d*) run", result.stdout) is not None
            return result.returncode == 0, ran
        if spec.startswith("node:"):
            result = run([self.node, "--test", f"--test-name-pattern={spec[len('node:'):]}", *self.node_tests])
            tests = re.search(r"^# tests (\d+)", result.stdout, re.MULTILINE)
            failed = re.search(r"^# fail (\d+)", result.stdout, re.MULTILINE)
            ran = tests is not None and int(tests.group(1)) > 0
            return result.returncode == 0 and failed is not None and failed.group(1) == "0", ran
        result = run(["ctest", "--test-dir", str(self.build), "-R", spec])
        return result.returncode == 0, "No tests were found" not in result.stdout


def stop_on_signal(signum, _frame):
    # Raised in the main thread, so the `finally` that removes the copy runs, as on Ctrl-C.
    raise SystemExit(128 + signum)


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    only = sys.argv[1] if len(sys.argv) > 1 else None
    mutants = [m for m in MUTANTS if only is None or only in m[0]]
    scratch_tree = load_scratch_tree()
    copy = scratch_tree.make(ROOT)
    try:
        build = scratch_tree.configure(copy, ROOT)
        if build is None:
            print("PROBLEM: the copy of the working tree couldn't be configured")
            sys.exit(1)
        check(Runner(copy, build), copy, build, mutants)
    finally:
        scratch_tree.remove(copy)


def check(runner, copy, build, mutants):
    if run(["cmake", "--build", str(build)]).returncode != 0:
        print("PROBLEM: the real code does not build")
        sys.exit(1)

    # The baseline: every set of tests a mutant names must pass on the real code. Otherwise a test
    # already failing (or flaky) would count every mutant naming it as caught.
    baseline_failures = []
    for tests in sorted({tests for _, _, _, tests in mutants}):
        passed, ran = runner.check(tests)
        if not (passed and ran):
            baseline_failures.append(f"'{tests}' {'fails' if ran else 'matches no test'} on the real code")
    if baseline_failures:
        for failure in baseline_failures:
            print(f"PROBLEM: {failure}")
        print("no mutants run: the baseline must pass first")
        sys.exit(1)

    caught, problems = 0, []
    for name, file, edits, tests in mutants:
        path = copy / file
        original = path.read_text()
        mutated = original
        try:
            for old, new in edits:
                if mutated.count(old) != 1:
                    raise LookupError(f"'{old}' occurs {mutated.count(old)} times in {file}, not once")
                mutated = mutated.replace(old, new)
            path.write_text(mutated)
            built = run(["cmake", "--build", str(build)])
            if built.returncode != 0:
                problems.append(f"{name}: the mutant does not compile\n{built.stdout[-2000:]}")
                continue
            passed, ran = runner.check(tests)
            if ran and not passed:
                caught += 1
                print(f"caught:  {name}")
            else:
                problems.append(f"{name}: NOT caught by {tests}")
        except LookupError as error:
            problems.append(f"{name}: {error}")
        finally:
            path.write_text(original)
    for problem in problems:
        print(f"PROBLEM: {problem}")
    print(f"{caught} of {len(mutants)} mutants caught")
    sys.exit(0 if caught == len(mutants) and not problems else 1)


main()
