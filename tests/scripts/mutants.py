# Checks that the tests catch bugs: each mutant breaks the code in one deliberate way, and the
# tests named for it must then fail, though they pass on the real code (checked first). It works in
# a copy of the working tree with a build of its own (scratch_tree.py), kept in build/mutants/tree
# and brought up to date each time, so the files being edited and the build in use are never
# touched, whatever stops it.
#
#   python3 -I tests/scripts/mutants.py              every mutant not caught already, as things are
#   python3 -I tests/scripts/mutants.py --all        every mutant
#   python3 -I tests/scripts/mutants.py NAME         the mutants whose names contain NAME
#   python3 -I tests/scripts/mutants.py --list       which mutants would run, and why, running none
#   python3 -I tests/scripts/mutants.py --record-all record every mutant as caught, as things are
#                                                    (for selection_check.py)
#
# A mutant is skipped when nothing its result depends on has changed since it was last caught:
# its own definition, the file it mutates, the test programs it runs (their fingerprints, as
# iterate.py takes them, from the copy's build) and this check's own code. Each caught mutant's key,
# a hash of those, is recorded in build/mutants/last-caught.json; a mutant not caught loses its
# record. CI keeps no record, so it runs every mutant. FILLSCOPE_MUTANTS_DIR puts the copy and the
# record elsewhere (selection_check.py does, to check the skipping without touching either).
#
# A mutant whose text no longer matches the source is an error, not a pass: the check must not go
# silently stale when the code changes.
import fcntl
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import signal
import sys
import tempfile

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
CODE_MAP = "tools/code_map/code_map.py"
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
CLIENT_REQUEST = "backend/Chapter12/exchange/order_server/client_request.h"
ORDER_SERVER = "backend/Chapter12/exchange/order_server/order_server.h"
ORDER_MANAGER = "backend/Chapter12/trading/strategy/order_manager.h"
MATCHING_ENGINE = "backend/Chapter12/exchange/matcher/matching_engine.h"
REPLAY_MAIN = "backend/Chapter12/exchange/exchange_replay.cpp"
REQUEST_ANCHOR = "backend/Chapter12/exchange/matcher/lockstep_anchor.cpp"
LF_QUEUE = "backend/Chapter12/common/lf_queue.h"
MACROS = "backend/Chapter12/common/macros.h"
MUTANTS_SCRIPT = "tests/scripts/mutants.py"
SCRATCH_TREE_SCRIPT = "tests/scripts/scratch_tree.py"
ITERATE_SCRIPT = "tests/scripts/iterate.py"
SOCKET_UTILS = "backend/Chapter12/common/socket_utils.h"
ENDPOINTS = "backend/Chapter12/common/endpoints.h"
EXCHANGE_MAIN_SOURCE = "backend/Chapter12/exchange/exchange_main.cpp"
TRADING_MAIN_SOURCE = "backend/Chapter12/trading/trading_main.cpp"
TEST_SUPPORT = "tests/support/test_support.h"
FORK_TEST_SUPPORT = "backend/Chapter12/tests/test_support.cpp"
BACKEND_SYSTEM_TEST = "tests/backend/backend_system_test.py"

# (name, file, [(old, new), ...], the tests that must catch it: a CTest regex, or one of the other
# forms Runner names, such as "lowlatency:FILTER")
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
    ("how CTest runs a program's tests isn't fingerprinted", "tests/scripts/iterate.py",
     [('fingerprint(p, runs.get(p.name, "") + "\\n" + environment_now)', "fingerprint(p, environment_now)")],
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
    ("the code map's check leaves out a submodule's files", CODE_MAP,
     [('            files.extend(f"{name}/{inner}" for inner in listed(path) if (path / inner).is_file())', '            pass')],
     "code_map_tests"),
    ("the code map's check leaves out untracked files", CODE_MAP,
     [('"ls-files", "--cached", "--others", "--exclude-standard", "-z"', '"ls-files", "--cached", "--exclude-standard", "-z"')],
     "code_map_tests"),
    ("the code map counts documents as code", CODE_MAP,
     [('NOT_CODE = ("*.md", "LICENSE")', 'NOT_CODE = ("LICENSE",)')],
     "code_map_tests"),
    ("the code map counts the back end's other chapters as code", CODE_MAP,
     [("    return not path.startswith(BACKEND) or path.startswith(BACKEND_IN_USE)", "    return True")],
     "code_map_tests"),
    ("the code map ignores a heading's directory", CODE_MAP,
     [("        patterns = [base + pattern for pattern", "        patterns = [pattern for pattern")],
     "code_map_tests"),
    ("a directory in the code map matches files beside it", CODE_MAP,
     [("        return path.startswith(pattern)", '        return path.startswith(pattern.rstrip("/"))')],
     "code_map_tests"),
    ("the code map's check passes a pattern that names no file", CODE_MAP,
     [("            if not any(matches(pattern, path) for path in code):", "            if False:")],
     "code_map_tests"),
    ("the code map's check counts documents as files a pattern names", CODE_MAP,
     [("            if not any(matches(pattern, path) for path in code):", "            if not any(matches(pattern, path) for path in files):")],
     "code_map_tests"),
    ("the code map's check passes a file in two parts", CODE_MAP,
     [("        elif len(owners) > 1:", "        elif False:")],
     "code_map_tests"),
    ("the code map's check passes whatever it finds", CODE_MAP,
     [("    if found:\n        print(f\"{MAP}:", "    if False:\n        print(f\"{MAP}:")],
     "code_map_tests"),
    # The system tier's timeout must reach its test: at one second, the test (half a minute) fails.
    ("the system tier's timeout is one second", "tests/CMakeLists.txt",
     [("set(FILLSCOPE_TIMEOUT_system 120)", "set(FILLSCOPE_TIMEOUT_system 1)")],
     "backend_system_tests"),
    # The back end's matching and market data, caught by its own tests (one CTest test runs them).
    ("a buy doesn't trade at the ask's price", BOOK,
     [("        if (LIKELY(price < ask_itr->price_)) {", "        if (LIKELY(price <= ask_itr->price_)) {")],
     ("lowlatency:OrderBookTest.ABidAndAnAskFarApartBothRest:"
      "OrderBookTest.ABuySweepTakesTheAsksBestPriceFirst:OrderBookTest.AFilledOrderCannotBeCancelled:"
      "OrderBookTest.APartialFillLeavesTheRestingOrderWithItsPriority:"
      "OrderBookTest.AnOrderThatCrossesFillsTheEarliestOrderAtThePriceFirst:"
      "Replay.GivesTheMarketUpdatesTheRequestsMakeNumberedFromOne")),
    ("a fill takes the resting order's whole quantity", BOOK,
     [("    const auto fill_qty = std::min(*leaves_qty, order_qty);", "    const auto fill_qty = order_qty;")],
     ("lowlatency:OrderBookTest.APartialFillLeavesTheRestingOrderWithItsPriority:"
      "Replay.GivesTheMarketUpdatesTheRequestsMakeNumberedFromOne")),
    ("a rejected cancel is reported as cancelled", BOOK,
     [("      client_response_ = {ClientResponseType::CANCEL_REJECTED, client_id, ticker_id, order_id, OrderId_INVALID,",
       "      client_response_ = {ClientResponseType::CANCELED, client_id, ticker_id, order_id, OrderId_INVALID,")],
     ("lowlatency:OrderBookTest.ACancelOfAnOrderNeverSentIsRejected:"
      "OrderBookTest.AFilledOrderCannotBeCancelled:OrderBookTest.ASecondCancelOfAnOrderIsRejected")),
    ("priorities at a price skip one", BOOK_HEADER,
     [("      return orders_at_price->first_me_order_->prev_order_->priority_ + 1;",
       "      return orders_at_price->first_me_order_->prev_order_->priority_ + 2;")],
     "lowlatency:OrderBookTest.OrdersAtOnePriceGetIncreasingPriorities"),
    ("a new order goes to the front of its price's queue", BOOK_HEADER,
     [("        first_order->prev_order_ = order;\n",
       "        first_order->prev_order_ = order;\n        orders_at_price->first_me_order_ = order;\n")],
     ("lowlatency:OrderBookTest.AnOrderThatCrossesFillsTheEarliestOrderAtThePriceFirst:"
      "OrderBookTest.OrdersAtOnePriceGetIncreasingPriorities")),
    ("a better bid doesn't become the best", BOOK_HEADER,
     [("          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ > best_orders_by_price->price_) ||",
       "          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < best_orders_by_price->price_) ||")],
     "lowlatency:OrderBookTest.ASellSweepTakesTheBidsBestPriceFirst"),
    ("a price's level is whatever the probe meets first", PRICE_LEVELS,
     [("        if (level == nullptr || level->price_ == price) {", "        if (true) {")],
     "lowlatency:PriceLevelTable.AgreesWithAMapThroughRandomInsertsAndRemovals"),
    ("a removal leaves a hole in the probe", PRICE_LEVELS,
     [("        if (distance(home(slots_[slot]->price_), slot) >= distance(hole, slot)) {", "        if (false) {")],
     "lowlatency:PriceLevelTable.AgreesWithAMapThroughRandomInsertsAndRemovals"),
    ("a removal moves entries that belong after the hole", PRICE_LEVELS,
     [("        if (distance(home(slots_[slot]->price_), slot) >= distance(hole, slot)) {",
       "        if (distance(home(slots_[slot]->price_), slot) < distance(hole, slot)) {")],
     "lowlatency:PriceLevelTable.AgreesWithAMapThroughRandomInsertsAndRemovals"),
    ("the incremental feed's sequence number doesn't advance", MD_PUBLISHER,
     [("        ++next_inc_seq_num_;\n", "")],
     "lowlatency:MarketDataPublisher.NumbersEachUpdateFromOneInOrder"),
    ("a snapshot ignores a modify", SNAPSHOT,
     [("        order->qty_ = me_market_update.qty_;\n", "")],
     "lowlatency:SnapshotSynthesizer.ASnapshotHoldsEveryLiveOrderAsItIsNow"),
    ("a snapshot's start doesn't say where to resume", SNAPSHOT,
     [("    const MDPMarketUpdate start_market_update{snapshot_size++, {MarketUpdateType::SNAPSHOT_START, last_inc_seq_num_}};",
       "    const MDPMarketUpdate start_market_update{snapshot_size++, {MarketUpdateType::SNAPSHOT_START, 0}};")],
     "lowlatency:SnapshotSynthesizer.ASnapshotHoldsEveryLiveOrderAsItIsNow"),
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
    ("a replay numbers its updates from 0", REPLAY,
     [("    size_t next_seq = 1;  // as MarketDataPublisher::next_inc_seq_num_ starts", "    size_t next_seq = 0;")],
     "lowlatency:Replay.GivesTheMarketUpdatesTheRequestsMakeNumberedFromOne"),
    # A step that says the wrong thing about its request would make an explanation of it false.
    ("a step keeps none of its responses", REPLAY,
     [("        step.responses.push_back(*next);\n", "")],
     "lowlatency:Replay.EachStepIsWhatItsRequestDid"),
    ("a step's updates start one late", REPLAY,
     [("      Step step{.first_update = next_seq,", "      Step step{.first_update = next_seq + 1,")],
     "lowlatency:Replay.EachStepIsWhatItsRequestDid"),
    ("the steps file leaves out how many responses a step has", REPLAY,
     [("      write(static_cast<uint32_t>(steps[i].responses.size()));\n", "")],
     "lowlatency:Replay.TheStepsFileIsAHeaderThenEachRequestAndWhatItDid"),
    ("the steps file is written short of a request's step", REPLAY,
     [("    if (requests.size() != steps.size()) {", "    if (false) {")],
     "lowlatency:Replay.TheStepsFileIsAHeaderThenEachRequestAndWhatItDid"),
    ("a recording's partial last request is read", RECORDING,
     [("    if (body % sizeof(MEClientRequest) != 0) {", "    if (false) {")],
     "lowlatency:RequestRecording.SaysWhyARecordingCantBeRead"),
    ("a recording's requests aren't checked", RECORDING,
     [("      if (const auto why = unprocessable(recording.requests[i])) {",
       "      if (const auto why = unprocessable(recording.requests[i]); why && false) {")],
     "lowlatency:RequestRecording.RefusesARequestTheMatcherCantProcess"),
    ("a request's ticker isn't checked", CLIENT_REQUEST,
     [("    if (ticker >= Common::ME_MAX_TICKERS) {", "    if (false) {")],
     ("lowlatency:OrderServer.AClientIdPastTheTablesIsDroppedAndTheServerRunsOn:"
      "OrderServer.ANewOrderWithNoSideIsRejected:OrderServer.ARequestOfUnknownTypeIsRejected:"
      "OrderServer.ATickerPastTheBooksIsRejected:RequestRecording.RefusesARequestTheMatcherCantProcess")),
    ("a request's order id isn't checked", CLIENT_REQUEST,
     [("    if (order >= Common::ME_MAX_ORDER_IDS) {", "    if (false) {")],
     ("lowlatency:OrderServer.ACancelPastTheOrderIdsIsCancelRejected:"
      "OrderServer.AClientIdPastTheTablesIsDroppedAndTheServerRunsOn:"
      "OrderServer.ANewOrderPastTheOrderIdsIsRejected:OrderServer.ANewOrderWithNoSideIsRejected:"
      "OrderServer.ARequestOfUnknownTypeIsRejected:OrderServer.ATickerPastTheBooksIsRejected:"
      "OrderServer.RefusalsTakeTheClientsResponseNumbersInTurn:"
      "RequestRecording.RefusesARequestTheMatcherCantProcess")),
    # The order server refuses what the matcher can't process.
    ("the order server passes on what the matcher can't process", ORDER_SERVER,
     [("            refuse(socket, request->me_client_request_, *why);\n            continue;\n", "")],
     ("lowlatency:OrderServer.ACancelPastTheOrderIdsIsCancelRejected:"
      "OrderServer.AClientIdPastTheTablesIsDroppedAndTheServerRunsOn:"
      "OrderServer.ANewOrderPastTheOrderIdsIsRejected:OrderServer.ANewOrderWithNoSideIsRejected:"
      "OrderServer.ARequestOfUnknownTypeIsRejected:OrderServer.ATickerPastTheBooksIsRejected:"
      "OrderServer.RefusalsTakeTheClientsResponseNumbersInTurn")),
    ("a refused cancel is answered as a rejected order", ORDER_SERVER,
     [("request.type_ == ClientRequestType::CANCEL ? ClientResponseType::CANCEL_REJECTED : ClientResponseType::REJECTED",
       "ClientResponseType::REJECTED")],
     "lowlatency:OrderServer.ACancelPastTheOrderIdsIsCancelRejected"),
    ("a refusal takes no response number", ORDER_SERVER,
     [("      socket->send(&response, sizeof(MEClientResponse));\n      ++next_outgoing_seq_num;\n",
       "      socket->send(&response, sizeof(MEClientResponse));\n")],
     "lowlatency:OrderServer.RefusalsTakeTheClientsResponseNumbersInTurn"),
    ("the trading client ignores a rejection", ORDER_MANAGER,
     [("        case Exchange::ClientResponseType::REJECTED: {\n          order->order_state_ = OMOrderState::DEAD;\n",
       "        case Exchange::ClientResponseType::REJECTED: {\n")],
     "lowlatency:OrderManager.ARejectedOrderIsDead"),
    ("a test claims its slot without locking it", TEST_SUPPORT,
     [("if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0) {", "if (fd >= 0) {")],
     "TestPort.ThisProcessHoldsItsSlot"),
    ("a back-end test claims its slot without locking it", FORK_TEST_SUPPORT,
     [("if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0) {", "if (fd >= 0) {")],
     "lowlatency:TestPort.ThisProcessHoldsItsSlot"),
    ("the system test claims its slot without locking it", BACKEND_SYSTEM_TEST,
     [("            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)\n            return slot",
       "            return slot")],
     "backend_system_tests"),
    ("a stream's group need not be multicast", ENDPOINTS,
     [(" || !IN_MULTICAST(ntohl(address.s_addr))", "")],
     "lowlatency:Endpoints.RefusesAValueThatIsntOne"),
    ("the exchange publishes its updates on the book's port whatever it is given", EXCHANGE_MAIN_SOURCE,
     [("inc_pub_port = endpoints.incremental.port;", "inc_pub_port = 20001;")],
     "backend_system_tests|feed_system_tests"),
    ("the client looks for the exchange on the book's port whatever it is given", TRADING_MAIN_SOURCE,
     [("const int order_gw_port = endpoints.order_port;", "const int order_gw_port = 12345;")],
     "backend_system_tests"),
    ("the exchange's multicast leaves on the default interface", SOCKET_UTILS,
     [("if (socket_cfg.is_udp_ && !socket_cfg.is_listening_) {", "if (false) {")],
     ("lowlatency:MarketDataPublisher.NumbersEachUpdateFromOneInOrder:"
      "McastSocket.ASenderOnAnInterfaceThatDoesntExistStopsTheProgram:"
      "McastSocket.ASenderSendsOnTheInterfaceItIsGiven:"
      "SnapshotSynthesizer.ASnapshotHoldsEveryLiveOrderAsItIsNow:"
      "SnapshotSynthesizer.PublishesASnapshotEveryInterval")),
    ("a book listener joins on the default interface", SOCKET_UTILS,
     [("{inet_addr(iface_ip.c_str())}};", "{htonl(INADDR_ANY)}};")],
     "lowlatency:McastSocket.AListenerJoinsOnTheInterfaceItIsGiven"),
    ("the viewer joins on the default interface", FEED_SOCKET,
     [(".imr_interface = {htonl(INADDR_LOOPBACK)}", ".imr_interface = {htonl(INADDR_ANY)}")],
     "FeedSocket.ReceivesWhatIsSentToItsGroup"),
    # The mutation check's choice of mutants: what a mistake would skip silently.
    ("a mutant's key leaves out its definition", MUTANTS_SCRIPT,
     [("    digest.update(repr(mutant).encode())\n", "")],
     "scripts_tests"),
    ("a mutant's key leaves out the file it mutates", MUTANTS_SCRIPT,
     [("    digest.update(file_digest)\n", "")],
     "scripts_tests"),
    ("a mutant's key leaves out the programs it runs", MUTANTS_SCRIPT,
     [('        digest.update(f"{name}={value}\\n".encode())', "        pass")],
     "scripts_tests"),
    ("the check's own code counts its mutants", MUTANTS_SCRIPT,
     # Split in two, here and below, so that the text occurs once in this file: in the code.
     [("    return source[:start] "  "+ source[end + 3:]", "    return source")],
     "scripts_tests"),
    ("a mutant not caught keeps its record", MUTANTS_SCRIPT,
     [("    if caught:\n        result[name] = key", "    if True:\n        result[name] = key")],
     "scripts_tests"),
    ("mutants may share a name", MUTANTS_SCRIPT,
     [("        (repeated if name in seen "  "else seen).add(name)", "        seen.add(name)")],
     "scripts_tests"),
    ("a UI test's mutant names no program", MUTANTS_SCRIPT,
     [('            found.add("viewer_'  'ui_tests")', "            pass")],
     "scripts_tests"),
    ("the copy keeps a rewritten file's old time", SCRATCH_TREE_SCRIPT,
     [("                shutil.copy(source, target)  # its contents and mode; the time is now",
       "                shutil.copy2(source, target)")],
     "scripts_tests"),
    ("the copy keeps a file gone from the tree", SCRATCH_TREE_SCRIPT,
     [("        (copy / name).unlink()\n", "")],
     "scripts_tests"),
    ("a large test binary is fingerprinted by its size and time", ITERATE_SCRIPT,
     [("file_digest(path, whole=path == program.binary)", "file_digest(path)")],
     "scripts_tests"),
    ("the pass record keeps programs the build no longer lists", ITERATE_SCRIPT,
     [("    if known is not None:\n", "    if False:\n")],
     "scripts_tests"),
    ("an ASSERT builds its message when it holds", MACROS,
     [("    if (UNLIKELY(!(cond))) {                              \\\n      std::cerr << \"ASSERT : \" << (msg) << std::endl;     \\\n",
       "    const std::string message_ = (msg);                   \\\n    if (UNLIKELY(!(cond))) {                              \\\n      std::cerr << \"ASSERT : \" << message_ << std::endl; \\\n")],
     "lowlatency_allocation_tests"),
    ("a full queue is overwritten", LF_QUEUE,
     [("ASSERT(num_elements_ != store_.size(),", "ASSERT(true,")],
     "lowlatency:LFQueue.WritingToAFullQueueStopsTheProgram"),
    ("a queue is full one element early", LF_QUEUE,
     [("ASSERT(num_elements_ != store_.size(),", "ASSERT(num_elements_ + 1 < store_.size(),")],
     "lowlatency:LFQueue.AQueueHoldsAsManyAsItsSizeInOrder"),
    ("a write cut off part-way leaves its part in the recording", RECORDING,
     [("if (written > 0 && ::ftruncate(", "if (written > 0 && false && ::ftruncate(")],
     "lowlatency:RequestRecording.AWriteCutOffPartWayLeavesTheWholeRequestsBefore"),
    ("a cut-off recording is cut back one request too long", RECORDING,
     [("sizeof(kRecordingMagic) + recorded_ * sizeof(request)", "sizeof(kRecordingMagic) + (recorded_ + 1) * sizeof(request)")],
     "lowlatency:RequestRecording.AWriteCutOffPartWayLeavesTheWholeRequestsBefore"),
    ("a failed recording write says nothing of where it stopped", RECORDING,
     [("    ++recorded_;\n", "")],
     ("lowlatency:RequestRecording.AFailedWriteStopsTheRecordingAndSaysWhy:"
      "RequestRecording.AWriteCutOffPartWayLeavesTheWholeRequestsBefore")),
    ("the running matcher records nothing", MATCHING_ENGINE,
     [("            recorder_->record(*me_client_request);\n", "")],
     "lowlatency:RequestRecording.TheRunningMatcherRecordsWhatItProcessesAndDoesWhatItsReplayDoes"),
    ("an infinite snapshot interval is accepted", "backend/Chapter12/exchange/exchange_main.cpp",
     [("!(seconds >= 0.001 && seconds <= 1e6)", "!(seconds > 0)")],
     "backend_system_tests"),
    ("a refused command line truncates the recording", "backend/Chapter12/exchange/exchange_main.cpp",
     [("      record_path = argv[i + 1];", "      record_path = argv[i + 1];\n      delete new Exchange::RequestRecorder(record_path);")],
     "backend_system_tests"),
    ("the replay takes a signed or spaced number", REPLAY_MAIN,
     [('    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {', "    if (text.empty()) {")],
     "ReplayLockstep.ABadCommandLineIsRefused"),
    ("the replay takes a group that isn't an address", REPLAY_MAIN,
     [("        if (!port || *port == 0 || ::inet_pton(AF_INET, value.substr(0, colon).c_str(), &group) != 1) {",
       "        if (!port || *port == 0 || ::inet_pton(AF_INET, value.substr(0, colon).c_str(), &group) < 0) {")],
     "ReplayLockstep.ABadCommandLineIsRefused"),
    ("the tracer check says untraced whatever the status says", REQUEST_ANCHOR,
     [("        return error == std::errc{} && ptr == value.data() + value.size() && pid != 0;",
       "        return error == std::errc{} && ptr == value.data() + value.size() && pid < 0;")],
     ("lowlatency:LockstepAnchor.KnowsWhetherThisProcessIsTraced:"
      "LockstepAnchor.ReadsTracerPidOnlyFromTheStartOfALine")),
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
     "lowlatency:LockstepAnchor.SelfStopWithoutATracerDoesNotStop"),
    ("the request anchor reads TracerPid anywhere in a line", REQUEST_ANCHOR,
     [("      if (line.starts_with(key)) {", "      if (line.find(key) != std::string_view::npos) {"),
      ("        auto value = line.substr(key.size());", "        auto value = line.substr(line.find(key) + key.size());")],
     "lowlatency:LockstepAnchor.ReadsTracerPidOnlyFromTheStartOfALine"),
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
     ("lowlatency:LockstepAnchor.KnowsWhetherThisProcessIsTraced:"
      "LockstepAnchor.ReadsTracerPidOnlyFromTheStartOfALine:"
      "LockstepAnchor.SelfStopWithoutATracerDoesNotStop")),
    ("the request anchor doesn't record the request", REQUEST_ANCHOR,
     [("  Exchange::g_last_request = request_number;", "  Exchange::g_last_request = 0;")],
     "lowlatency:LockstepAnchor.RecordsEachRequest:LockstepAnchor.SelfStopWithoutATracerDoesNotStop"),
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
    ("the viewer's failed write leaves the old file", PUBLISHER,
     [("        if (::unlink(file_.c_str()) == 0 || errno", "        if (false || errno")],
     "Publisher.AFailedWriteRemovesTheOldFile + node:a failed write removes the old file, is logged, and the next write works again"),
    ("a failed write is retried only when the context changes", PUBLISHER,
     [("    if (published_ == context && !error_) {", "    if (published_ == context) {")],
     "Publisher.AFailedWriteThatCantRemoveTheFileKeepsWhatItHolds"),
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
    ("the viewer's failed write leaves its temporary file", PUBLISHER,
     [("        ::unlink(temporary.c_str());\n", "")],
     "Publisher.AFailedWriteIsRetried + node:a failed write throws and leaves no temporary file"),
    ("finish says the viewer is still running", PUBLISHER,
     [("writer_.running = false;", "writer_.running = true;")],
     "Publisher.FinishSays"),
    ("a document links to a file that doesn't exist", "README.md",
     [("(docs/RUNNING.md)", "(docs/RUNNING-missing.md)")],
     "DocLinks.EveryRelativeLinkResolves"),
    ("links inside code blocks are followed", DOC_LINKS,
     [("        if (in_code_block || line.find(\"](\") == std::string::npos) {",
       "        if (line.find(\"](\") == std::string::npos) {")],
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
    ("the extension's failed write leaves its temporary file", DEBUG_STATE,
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
    ("the extension's failed write leaves the old file", EXTENSION,
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


# Mutants only a user who isn't root can catch: their test needs a directory it can't write to,
# which root writes to anyway (CI's container runs as root). Under root they are skipped, and said so.
NEEDS_A_USER_NOT_ROOT = {"a failed write is retried only when the context changes"}


def run(args, cwd=None):
    return SCRATCH_TREE.run(args, cwd=cwd)


def load_scratch_tree():
    """scratch_tree.py, beside this script. Loaded by path: python3 -I puts no script directory on
    the import path."""
    spec = importlib.util.spec_from_file_location("scratch_tree", pathlib.Path(__file__).with_name("scratch_tree.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SCRATCH_TREE = load_scratch_tree()


class Runner:
    """Runs the tests a mutant names, in the scratch copy's build. A name is one of:
      a CTest regular expression, as `ctest -R` takes;
      "ui:FILTER", Dear ImGui Test Engine tests, as viewer_ui_tests takes a filter (comma-separated
        parts of test names);
      "node:PATTERN", the extension's Node tests whose names match, as `node --test-name-pattern`
        takes;
      "lowlatency:FILTER", the back end's own tests (lowlatency_tests, one CTest test for the whole
        program) whose names match, as --gtest_filter takes: the tests that catch the mutant, not
        the whole program, which takes about 20 seconds;
    and several, joined by " + ", are all run. A CTest expression naming several programs runs them
    in parallel. Each check returns (passed, ran any)."""

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
            # All files in one process: isolated, each file counts as a test of its own, so a pattern
            # that matches nothing still reports "# tests 3" (Node 22.23.1) and would count as having run.
            result = run([self.node, "--test", "--experimental-test-isolation=none",
                          f"--test-name-pattern={spec[len('node:'):]}", *self.node_tests])
            tests = re.search(r"^# tests (\d+)", result.stdout, re.MULTILINE)
            failed = re.search(r"^# fail (\d+)", result.stdout, re.MULTILINE)
            ran = tests is not None and int(tests.group(1)) > 0
            return result.returncode == 0 and failed is not None and failed.group(1) == "0", ran
        if spec.startswith("lowlatency:"):
            # The program makes a directory of its own for the logs it writes, and enters it.
            result = run([str(self.build / "backend" / "Chapter12" / "tests" / "lowlatency_tests"),
                          f"--gtest_filter={spec[len('lowlatency:'):]}"])
            return result.returncode == 0, "[ RUN      ]" in result.stdout
        result = run(["ctest", "--test-dir", str(self.build), "-j", str(os.cpu_count() or 1), "-R", spec])
        return result.returncode == 0, "No tests were found" not in result.stdout


def stop_on_signal(signum, _frame):
    # Raised in the main thread, so the `finally` that removes the copy runs, as on Ctrl-C.
    raise SystemExit(128 + signum)


USAGE = "usage: mutants.py [--all | --list | --record-all | NAME]"

MUTATION_DIR = pathlib.Path(os.environ.get("FILLSCOPE_MUTANTS_DIR", ROOT / "build" / "mutants"))
COPY = MUTATION_DIR / "tree"
RECORD = MUTATION_DIR / "last-caught.json"


def duplicate_names(mutants):
    """What is wrong with the mutants' names, or None: each must be unique, since the record of
    caught mutants is kept by name."""
    seen, repeated = set(), set()
    for name, *_ in mutants:
        (repeated if name in seen else seen).add(name)
    return f"mutants share a name: {', '.join(sorted(repeated))}" if repeated else None


def without_mutants(source):
    """mutants.py's source with its list of mutants left out: the check's own code, which every
    mutant's result depends on, apart from the mutants, each of which counts only for itself."""
    start = source.index("\nMUTANTS = [\n")
    end = source.index("\n]\n", start)
    return source[:start] + source[end + 3:]


def machinery_digest(texts):
    """A hash of the check's own code: `texts` are its scripts' sources, mutants.py's without its
    list of mutants."""
    digest = hashlib.sha256()
    for text in texts:
        digest.update(hashlib.sha256(text.encode()).digest())
    return digest.hexdigest()


def programs_named(tests, ctest_programs):
    """The test programs a mutant's tests run: `ctest_programs(regex)` gives the programs of the
    CTest tests a regular expression matches."""
    found = set()
    for part in tests.split(" + "):
        part = part.strip()
        if part.startswith("ui:"):
            found.add("viewer_ui_tests")
        elif part.startswith("node:"):
            found.add("vscode_debug_state_tests")
        elif part.startswith("lowlatency:"):
            found.add("lowlatency_tests")
        else:
            found |= set(ctest_programs(part))
    return found


def mutant_key(machinery, mutant, file_digest, fingerprints):
    """What a mutant's result depends on, as one hash: the check's code, the mutant's definition,
    the contents of the file it mutates, and the fingerprint of each program it runs (by name,
    "none" for a program the build doesn't list)."""
    digest = hashlib.sha256()
    digest.update(machinery.encode())
    digest.update(repr(mutant).encode())
    digest.update(file_digest)
    for name, value in sorted(fingerprints.items()):
        digest.update(f"{name}={value}\n".encode())
    return digest.hexdigest()


def updated(record, name, key, caught):
    """The record after a mutant's run: its key if caught, no entry if not (so it runs next time)."""
    result = {n: k for n, k in record.items() if n != name}
    if caught:
        result[name] = key
    return result


def load_record():
    try:
        return json.loads(RECORD.read_text())
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as error:
        print(f"warning: ignoring the unreadable record {RECORD}: {error}")
        return {}


def write_record(record):
    RECORD.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=RECORD.parent, prefix=RECORD.name, suffix=".tmp", delete=False) as out:
        out.write(json.dumps(record, indent=1, sort_keys=True) + "\n")
    os.replace(out.name, RECORD)


def load_iterate(build):
    """The copy's iterate.py, as a module, taking its fingerprints from the copy's build."""
    previous = os.environ.get("FILLSCOPE_ITERATE_BUILD")
    os.environ["FILLSCOPE_ITERATE_BUILD"] = str(build)
    try:
        spec = importlib.util.spec_from_file_location("copy_iterate", COPY / "tests" / "scripts" / "iterate.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        if previous is None:
            os.environ.pop("FILLSCOPE_ITERATE_BUILD", None)
        else:
            os.environ["FILLSCOPE_ITERATE_BUILD"] = previous
    return module


def keys_of(mutants, build):
    """{mutant name: key}, as the copy and its build are now."""
    iterate = load_iterate(build)
    fingerprints = iterate.fingerprints(iterate.programs())
    scripts = COPY / "tests" / "scripts"
    machinery = machinery_digest([without_mutants((scripts / "mutants.py").read_text()),
                                  (scripts / "scratch_tree.py").read_text(), (scripts / "iterate.py").read_text()])
    by_regex = {}

    def ctest_programs(regex):
        if regex not in by_regex:
            listing = run(["ctest", "--test-dir", str(build), "-N", "-R", regex, "--show-only=json-v1"]).stdout
            tests = json.loads(listing[listing.index("{"):])["tests"]
            by_regex[regex] = {label[len("exe:"):] for test in tests for label in iterate.labels_of(test)
                               if label.startswith("exe:")}
        return by_regex[regex]

    keys = {}
    for mutant in mutants:
        name, file, _, tests = mutant
        path = COPY / file
        contents = hashlib.sha256(path.read_bytes()).digest() if path.exists() else b"missing"
        named = programs_named(tests, ctest_programs)
        keys[name] = mutant_key(machinery, mutant, contents, {p: fingerprints.get(p, "none") for p in named})
    return keys


def lock_copy():
    """Holds an exclusive lock on the copy until this process ends, waiting for any other run of
    the check: two would edit the same copy."""
    MUTATION_DIR.mkdir(parents=True, exist_ok=True)
    lock = open(MUTATION_DIR / ".lock", "w")  # kept open: closing it would release the lock
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print(f"waiting for another mutation check using {COPY}")
        fcntl.flock(lock, fcntl.LOCK_EX)
    return lock


def main(args):
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    if len(args) > 1 or (args and args[0].startswith("-") and args[0] not in ("--all", "--list", "--record-all")):
        print(USAGE, file=sys.stderr)
        return 2
    if problem := duplicate_names(MUTANTS):
        print(f"PROBLEM: {problem}")
        return 1
    option = args[0] if args and args[0].startswith("--") else None
    only = args[0] if args and option is None else None
    mutants = [m for m in MUTANTS if only is None or only in m[0]]
    if os.geteuid() == 0:
        for name in sorted(m[0] for m in mutants if m[0] in NEEDS_A_USER_NOT_ROOT):
            print(f"skipped: {name} (its test can't be arranged as root; run the check as another user)")
        mutants = [m for m in mutants if m[0] not in NEEDS_A_USER_NOT_ROOT]

    lock = lock_copy()  # noqa: F841 (held, not used: released when the process ends)
    SCRATCH_TREE.sync(ROOT, COPY)
    build = SCRATCH_TREE.configure(COPY, ROOT)
    if build is None:
        print("PROBLEM: the copy of the working tree couldn't be configured")
        return 1
    if run(["cmake", "--build", str(build)]).returncode != 0:
        print("PROBLEM: the real code does not build")
        return 1
    keys = keys_of(mutants, build)
    names = {m[0] for m in MUTANTS}
    record = {name: key for name, key in load_record().items() if name in names}

    if option == "--record-all":
        record.update(keys)
        write_record(record)
        print(f"recorded {len(keys)} mutants as caught")
        return 0
    if option == "--all" or only is not None:
        selected = mutants
    else:
        selected = [m for m in mutants if record.get(m[0]) != keys[m[0]]]
    if option == "--list":
        chosen = {m[0] for m in selected}
        for name, *_ in mutants:
            print(("run   " if name in chosen else "skip  ") + name)
        return 0
    return check(Runner(COPY, build), COPY, build, selected, keys, record, len(mutants) - len(selected))


def check(runner, copy, build, mutants, keys, record, skipped):
    # The baseline: every set of tests a mutant to run names must pass on the real code. Otherwise a
    # test already failing (or flaky) would count every mutant naming it as caught.
    baseline_failures = []
    for tests in sorted({tests for _, _, _, tests in mutants}):
        passed, ran = runner.check(tests)
        if not (passed and ran):
            baseline_failures.append(f"'{tests}' {'fails' if ran else 'matches no test'} on the real code")
    if baseline_failures:
        for failure in baseline_failures:
            print(f"PROBLEM: {failure}")
        print("no mutants run: the baseline must pass first")
        return 1

    caught, problems = 0, []
    for name, file, edits, tests in mutants:
        path = copy / file
        original = path.read_text()
        mutated = original
        was_caught = False
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
                was_caught = True
                print(f"caught:  {name}")
            else:
                problems.append(f"{name}: NOT caught by {tests}")
        except LookupError as error:
            problems.append(f"{name}: {error}")
        finally:
            path.write_text(original)
            record = updated(record, name, keys[name], was_caught)
            write_record(record)
    for problem in problems:
        print(f"PROBLEM: {problem}")
    print(f"{caught} of {len(mutants)} mutants caught" +
          (f"; {skipped} unchanged since caught, not run (--all runs every one)" if skipped else ""))
    return 0 if caught == len(mutants) and not problems else 1


if __name__ == "__main__":  # loaded by other scripts too, which run its parts themselves
    sys.exit(main(sys.argv[1:]))
