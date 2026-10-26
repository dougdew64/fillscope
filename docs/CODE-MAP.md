# The code map

**Purpose:** every part of the code, where the architecture documents explain it, and whether the
owner has asked about it. **Status:** reference; a test (`code_map_check`) keeps its file lists
true to the code, and its last column is a record. **Read when:** choosing what to learn or to
explain next; after adding a file, to put it in a part.

The owner learns the system by asking about it, and each answer is written into
[ARCHITECTURE.md](ARCHITECTURE.md) or a document it links to (see its opening). This map compares
those documents with the code, so the gaps show:
- **Explained in** names the section that explains the part, or says **in part** when a section
  touches it without explaining how it works, or **not yet**.
- **Asked about** records the owner's questions about the part, with their dates. A part that is
  explained but never asked about was explained before the owner came to it; the owner has yet to
  learn it.

Every code file is in exactly one part: everything in the repository except its documents and its
license, and, of the back end, `backend/Chapter12/`, the version of the book's system that is
built and run (the fork keeps the book's earlier chapters, and its own tools, unchanged or
untested by Fillscope). A new file goes into a part in the same commit, or the test fails. Section
numbers are ARCHITECTURE.md's unless another document is named.

## Fillscope
| Part | Files | Explained in | Asked about |
|---|---|---|---|
| Market data: the updates' bytes | `src/feed/market_update.*` | §3, *What arrives* | |
| An order book | `src/feed/book.*` | §3, *A book*, *What the viewer's table is called* | 2026-10-26: whether the viewer's table is rightly called "the book" |
| Keeping the books in step, and recovery | `src/feed/book_builder.*` | §3, *Keeping the books in step* | 2026-10-24: why the exchange sends snapshots as well as changes |
| Receiving a stream | `src/feed/feed_socket.*` | §3, *Receiving* | |
| A replay's market data file | `src/feed/market_data_file.*` | §3, *Replaying*; §4, *A replay* | |
| Keeping what arrives, to show the books after any update | `src/feed/time_shift.*` | §3, *Keeping what arrives* | |
| The viewer program: start, options, stop signals, its window | `src/viewer/main.cpp`, `src/viewer/viewer_options.*`, `src/viewer/stop_signals.*`, `src/viewer/window_size.h` | §4, *The loop* (in part); the options in [RUNNING.md](RUNNING.md) | 2026-10-24: what the viewer is, beside the exchange and the client; how to stop it |
| The viewer's loop and state | `src/viewer/viewer_app.*`, `src/viewer/viewer_state.h` | §4, *The loop*, *The pieces* | |
| Drawing, and clicks | `src/viewer/viewer_view.*` | §4, *What it draws*; §8 | 2026-10-24: what the ticker buttons choose; 2026-10-26: what the paint count's line is for |
| The context: what is published | `src/viewer/context.*` | §4, *What it draws*, *The pieces*, *An order pointed at* | 2026-10-24: what "showing ticker 0" in the bar means; 2026-10-25: whether "exchange live at update N" is part of the context |
| Publishing the context | `src/viewer/context_publisher.*` | §4, *Publishing* | 2026-10-25: where the context bar is defined |
| The viewer's log, and a crash recorded in it | `src/viewer/viewer_log.*`, `src/viewer/crash_log.*`, `src/viewer/log_events.*` | §4, *The viewer's log* | |
| Answers: a scenario played through the real matcher, its claims checked, and the viewer playing it | `src/answer/`, `src/viewer/answer_state.*` | §10 | 2026-10-25: whether an answer goes through the running exchange and the network to the viewer |
| The course: lessons, each an answer's scenario, stored and tested | `course/` | §10, *The course* | |
| Shared helpers: a descriptor's owner, number parsing, a file replaced atomically | `src/common/` | §2 (in part); §4, *Publishing* | |
| The debug-state extension | `tools/vscode-debug-state/` | §5; [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md) | |
| The attribution report | `tools/attribution/` | [AI-USE.md](AI-USE.md) (in part) | |
| The code map's check | `tools/code_map/` | this document's opening | |
| The build, and the editor's configurations | `CMakeLists.txt`, `.vscode/`, `.gitignore`, `.gitmodules` | not yet; how to use them is in [RUNNING.md](RUNNING.md) | |
| CI | `.github/` | §9 (in part) | |
| The tests, by area | `tests/answer/`, `tests/feed/`, `tests/viewer/`, `tests/common/`, `tests/docs/`, `tests/support/`, `tests/vscode-debug-state/`, `tests/attribution/`, `tests/code_map/`, `tests/backend/backend_system_test.py`, `tests/*/CMakeLists.txt` | §9; §10, *The demos* (`tests/viewer/viewer_demo.cpp`) | 2026-10-26: how the demos are made: a program of their own, driven through the Test Engine, recorded by a flag |
| The debugger tests | `tests/backend/replay_debugger_test.cpp`, `tests/backend/replay_lldb_driver.py` | §6, *What keeps both working*; [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md) | |
| The test machinery: registration, selection, the checks of the tests | `tests/CMakeLists.txt`, `tests/scripts/*.py`, `tests/scripts/run-tests.sh` | §9, *Three times the tests run*, *Whether the tests can be trusted*, *Why a mutation check, and what it costs*; [RUNNING.md](RUNNING.md) | 2026-10-24: why there is a mutation check, why it costs so much here, and why a very large codebase couldn't run one this way; how a mutant begins life, finds its place, and is deleted; where the test programs are, what CTest is beside GoogleTest, what `RUN_ALL_TESTS` returns, and what `last-pass.json` records; 2026-10-25: whether the selection check decides which programs run |

## The back end (`backend/Chapter12/`)
| Part | Files | Explained in | Asked about |
|---|---|---|---|
| The exchange program | `exchange/exchange_main.cpp` | §1, *Three programs*, *Why the first orders take about twelve seconds* (in part) | 2026-10-24: what the exchange is; why the first orders take so long to appear; how to stop it |
| The matching engine: its thread and queues | `exchange/matcher/matching_engine.*` | §3, *What a replay is* (in part) | |
| The exchange's order books, and matching | `exchange/matcher/me_order*`, `exchange/matcher/price_level_table.h`, `exchange/matcher/unordered_map_me_order_book.*` | not yet; the fork's fixes to its two tables in the back end's README ([backend/README.md](../backend/README.md)) | 2026-10-24: why a table left uninitialized went unnoticed until the sanitizer build (commit `f9633eb`) |
| The order server: orders over TCP, in arrival order | `exchange/order_server/` | not yet | |
| The market data publisher and the snapshot synthesizer | `exchange/market_data/` | §3, *Two streams* (in part) | 2026-10-24: whether the exchange sends whole books or only changes |
| Recording and replaying requests | `exchange/matcher/request_recording.*`, `exchange/matcher/replay.*`, `exchange/exchange_replay.cpp` | §3, *What a replay is*, *Replaying* | |
| The lockstep anchor | `exchange/matcher/lockstep_anchor.*` | §6 | |
| The trading client program | `trading/trading_main.cpp` | §1, *Three programs*, *Why the first orders take about twelve seconds* (in part) | 2026-10-24: what the client is, and why it is a program apart from the viewer; why it waits before trading; how to stop it |
| The client's market data consumer | `trading/market_data/` | not yet | |
| The client's order gateway | `trading/order_gw/` | not yet | |
| The client's trade engine, and its strategies | `trading/strategy/trade_engine.*`, `trading/strategy/feature_engine.h`, `trading/strategy/liquidity_taker.*`, `trading/strategy/market_maker.*` | not yet | |
| The client's copy of the books | `trading/strategy/market_order*` | not yet | |
| The client's orders, risk and position | `trading/strategy/om_order.h`, `trading/strategy/order_manager.*`, `trading/strategy/risk_manager.*`, `trading/strategy/position_keeper.h` | not yet | |
| The lock-free queue between threads | `common/lf_queue*` | §1, *How the exchange's threads hand each other work* | 2026-10-24: how it works, what happens when one is full, and what to do about it |
| Memory pools | `common/mem_pool*`, `common/opt_mem_pool.h` | §1, *Why the first orders take about twelve seconds* (in part) | 2026-10-24: why starting takes seconds |
| Logging | `common/logging*`, `common/opt_logging.h` | not yet | |
| Sockets: TCP and multicast, and where the programs meet | `common/socket_*`, `common/mcast_socket.*`, `common/tcp_*`, `common/endpoints.h` | §1, *Why the viewer listens to multicast* (in part) | |
| Threads, time, types, macros, measurement | `common/thread_*`, `common/time_utils.h`, `common/types.h`, `common/macros.h`, `common/perf_utils.h` | §3, *A ticker* (in part); §1, *Why the first orders take about twelve seconds* (`ASSERT`, `createAndStartThread`) | 2026-10-24: what a ticker is; whether `ASSERT` is a macro, and what building its message costs |
| Benchmarks and performance analysis | `benchmarks/`, `notebooks/`, `perf_analysis.html`, `scripts/run_benchmarks.sh` | not yet | |
| The build, and the book's run scripts | `CMakeLists.txt`, `common/CMakeLists.txt`, `exchange/CMakeLists.txt`, `trading/CMakeLists.txt`, `scripts/build.sh`, `scripts/no_clean_build.sh`, `scripts/run_clients.sh`, `scripts/run_exchange_and_clients.sh` | not yet | |
| The back end's tests | `tests/` | the back end's README ([backend/README.md](../backend/README.md)), in part | |
