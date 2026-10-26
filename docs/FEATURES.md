# Feature backlog

**Purpose:** the features envisioned for Fillscope and not yet built, each with what it is and what
it rests on. **Status:** live plan; an item is removed when it is built (git keeps it), and the file
is deleted when empty. **Read when:** choosing what to build next, or asking what Fillscope will do.

Answers (15) are being built now, as [PLAN.md](PLAN.md)'s steps 11 to 14; the other items aren't
in order yet. The [charter](CHARTER.md)'s
rules bind every one of them. Known defects and test gaps are in [TECH-DEBT.md](TECH-DEBT.md).

Built so far: the back end, built and tested by Fillscope at a pinned commit; the live view of the
exchange's books; deterministic replay; and the matching engine stepped in a debugger while the
book view follows ([ARCHITECTURE.md](ARCHITECTURE.md)).

## What Fillscope shows
The features that distinguish it.
1. **Fill explanations grounded in exact state.** Click an order and ask "why didn't this fill?".
   The answer comes from the simulator's exact state at that moment: the book on arrival, the queue
   ahead, the trades and cancels that followed, the latency applied. Rests on fill simulation (7).
2. **Per-order counterfactuals, made exact by deterministic replay.** "What if this order had
   arrived 50 µs earlier?" is answered by replaying from a snapshot with one thing changed, not by
   estimating. Rests on deterministic replay (built) and recovery from a snapshot (6).
3. **How much of the profit is an assumption.** The same strategy with and without its queue and
   latency models, and how far the results move. Rests on fill simulation (7).
4. **An animated replay of the book.**

## What the back end gains
The stages of the roadmap, each a change to the fork.

5. **Replay of Nasdaq TotalView-ITCH 5.0 data into the exchange.** Its parser is to be the first
   worked example of checking AI output (charter §3, decision 6). Proposed, not yet decided:
   hand-written decoders for the messages the book needs, and the rest generated. AI builds a
   model of the specification, checked mechanically and against an independent model; a
   deterministic generator writes the decoders from it; they must agree with the hand-written ones
   on every message of a real trading day. The data is downloaded locally, never committed.
6. **Backtesting on simulated time, with recovery from a snapshot.** Latency in a backtest is
   simulated, not the network's ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)).
7. **Fill simulation:** queue position, latency, fees.
8. **Validation:** how far the results can be trusted, and a written report.
9. **Throughput:** measured messages per second.

## Tools around the app
10. **A command-line tool for counterfactual replays,** which an assistant working beside the app
    runs, since no AI runs in it (charter §3, decision 2). It is tested below the UI, with the
    core unit tests and replay fidelity checks.
11. **A small synthetic market data generator for demos,** since market data is never committed
    (charter §3, decision 5).
12. **Demo recordings:** each feature shown by a scripted demo that is part of the test suite
    (charter §2, priority 3), recorded with the Dear ImGui Test Engine, which already drives the
    viewer's UI tests (charter §3, decision 4). Answers' demo and the live book's are built and
    record themselves ([ARCHITECTURE.md](ARCHITECTURE.md), §10, *The demos*); what remains is a
    demo for each other feature: a replay, and the debugger lockstep.

## Inferring traders from anonymous orders
13. **Which orders belong to one trader, inferred from the market data alone.** The market data
    names no client: an order carries an id the exchange assigned, which lasts its whole life, and a
    trade carries no order id, only the side of the incoming order that caused it. The exchange's
    recording of the requests does name each order's client, so every guess can be scored against
    what really happened.
    - **Decided (the owner, 2026-10-25).** The first version detects slicing alone; the other
      patterns are later items. The detector lives in Fillscope and reads only the market data,
      update by update, live or replayed, so it has no way to see a client. The truth comes from a
      small change to the fork: replaying a recording also writes each order id with its client,
      and each trade with its aggressor's client, which the matcher's responses to the clients
      carry. The score is defined before the detector is written: for each trade, flagged as part
      of a slice or not, against whether a target client sent it, which gives hits, misses and
      false alarms exactly. The display's horizontal axis is the update number, since the data has
      no time.
    - **The target (the owner, 2026-10-25): any client that trades repeatedly at one side and
      size,** which is what counting side and quantity finds. That is the planted slicer, and also
      every liquidity-taking client of the book's: it always trades one fixed size (its "clip"),
      at the best price, so it repeats one side and size too. Which clients are targets is known
      from how each was started, and the truth marks their trades. Telling a deliberate slicer from
      a taker's routine clip (the taker trades when the book's state triggers it, a slicer on a
      schedule) is a later step, with its own scene.
    - **Who writes what (the owner, 2026-10-25).** The owner writes the detector and designs its
      interface (what goes in, what comes out), and may write some of its tests. Claude writes the
      fork's ground-truth output, the planted strategy, the scorer (the detector's check, so not
      written by the detector's author), and the tests, the owner's code included. Claude builds
      the display first, checking it in screenshots; the owner then judges it, and either it stays
      Claude's or the owner takes it over to polish. Whether the owner also writes the strategy's
      evasion step (the second scene) is decided once the display works.
    - **The patterns.** A large order sliced into equal pieces: repeated trades of one side and
      quantity. Cancel and replace: a cancel, and an order added close by in price soon after,
      likely the same trader adjusting; a client can't modify an order, so this is what moving one
      looks like. A market maker: a buy and a sell added together, the same size, and canceled and
      re-added together, likely one trader quoting both sides. "Soon" and "together" are counted
      in updates, not time: neither the market data nor the recording carries a time, so a replay
      gives the same guesses every run.
    - **The planted trader.** A new strategy in the fork's trading client that slices a large order,
      run among the book's market-making, liquidity-taking and random clients, whose orders are the
      background it must be found in. Only the random client has run under Fillscope so far (as
      client 5); the market-making and liquidity-taking ones are run and tested first, like the
      rest of the fork.
    - **The score.** What the detector caught, what it missed, and what it flagged wrongly, against
      the recording, so only a replay is scored; live, the viewer shows the detector's flags and
      says there is no truth to score them against. A guess is shown as a guess, never as a fact about who placed an order (charter
      §2, priority 1).
    - **The display,** a panel of the viewer, beside the book: the detector runs in the viewer as
      each update arrives. Each order a dot, update number across and price up, gray at first; in
      the first version, the trades flagged as a slice take a color, and with the later patterns
      the orders the detector groups take one color per inferred trader, joined by lines. Then the
      truth outlined, with the hits, misses and false alarms, and the score.
    - **The demo,** in three scenes: the slicer uses equal sizes and is caught; it randomizes its
      sizes and is lost; the detector improves and finds it again.
    - **Rests on** the animated replay (4), a new strategy in the fork, the fork's ground-truth
      output, and the book's market-making and liquidity-taking clients, run and tested.
14. **A GPU sweep of the detector's settings, with Vulkan.** Scoring the slicing detector (13) at
    thousands of settings (window size, minimum count) over a whole recording, each setting
    independent of the others, which suits a GPU's compute shaders; the result, hits against false
    alarms as the threshold moves, shows how good the detector can be. Every score the GPU gives
    must equal the CPU scorer's exactly, a check that doesn't depend on the GPU code. The detector
    itself stays on the CPU: it does a little sequential work per trade, which a GPU would slow
    down. Mesa's lavapipe runs Vulkan on the CPU, so the tests can run without a GPU, in CI too.
    Rests on 13.

## Teaching the domain
15. **Answers: a scenario the real matcher plays, explained step by step.** The owner asks, in the
    editor's chat, about a scenario (a maker quoting, a taker crossing, a slicer); the assistant
    answers with a scenario of client requests and an explanation of each; a tool plays the requests
    through Ghosh's matching engine, and the viewer shows the result step by step, the explanation in a pane beside the
    book. At any step the owner can point at an order and ask more; the context file already says
    which order, and as of which update, and will say which step of which answer too. Modelled on HRW's Answers (the assistant writes a file the app
    picks up and shows), and on HRW's lesson that explanation nothing checks rots.
    **In progress:** [PLAN.md](PLAN.md), steps 11 to 14 (the owner, 2026-10-25).
    - **No AI in the app** (charter §3, decision 2). The context file goes from the viewer to the
      assistant; an answer goes the other way, as a file the assistant writes,
      `.fillscope/answer.json` (ignored by git, replaced for each question), which the viewer picks
      up while it runs and shows at once. An answer is about the moment: it is never committed.
    - **The rows are the matcher's, not the assistant's.** An answer gives requests (which client
      sends what, in what order), never books or trades. A command-line tool turns them into a
      recording, in the format `exchange_main --record` writes, and replays it with
      `exchange_replay`, so every book, trade and response shown is the real engine's output. A
      wrong explanation is then contradicted by the rows rather than illustrated by them: the check
      that doesn't depend on the AI (charter §3, decision 6).
    - **Claims are checked.** Where a step's text states a fact (the best ask is 101, for 5; this
      order rests with 3; client 3 was told it filled 5 at 101), the claim is written in a form the
      tool checks against the replay's real state, and the viewer marks it true or false. The tool
      refuses an answer with a false claim, so the assistant corrects it before it is shown; the
      viewer shows the result as well, so a claim is never taken on trust.
    - **A step is one request:** the order sent, and the trades, book changes and responses to the
      clients that it causes. The market data file doesn't say which request caused which updates
      ([market_data_file.h](../src/feed/market_data_file.h)), so the fork's replay writes, for each
      request, the range of updates it caused and the responses it sent to clients. The same
      output gives trader inference its truth (13).
    - **What the scenarios must respect, in Ghosh's exchange:** every order is a limit order (a
      price and a quantity), and whatever doesn't trade rests in the book; there are no market
      orders and no immediate-or-cancel, so a taker is a limit order priced to cross. A client can
      only add and cancel; the only modify in the market data is a resting order partly filled.
      Clients are numbers, and a role ("the maker") is the answer's name for one; a scenario needs
      no trading-client program, since the requests are written directly.
    - **Who writes what:** Claude, all of it ([PLAN.md](PLAN.md), steps 11 to 14).
    - **Quizzes, as answers** (the owner, 2026-10-25). A step may carry an `ask`: a question the
      pane shows before the step is played ("the taker sends buy 8 at 102: what trades, and what's
      left?"), its explanation revealed when the owner steps on. The right answer is the replay's,
      so it is exact and checked; part of step 13. Predicting a client's next request is a
      different question, a trader's choice rather than the exchange's: it has one right answer
      only for a client following stated rules, and its check is reasoning about a strategy, not the
      matcher.
    - **Next, after step 14: who made money, and how much.** A fifth kind of claim, a client's
      profit and loss: its cash and position from the fills the matcher reported (the buyer pays
      price times quantity and gains the shares, the seller the reverse), and what an open position
      is valued at, stated (the mid-price, or the last trade), since a profit not yet taken depends
      on it. Ghosh's exchange charges no fees, which the answer says; fees are part of fill
      simulation (7).
    - **The phase after: strategies in the loop** (the owner, 2026-10-25). Some clients of a
      scenario are Ghosh's own strategies, with their settings ("client 1 is a market maker,
      threshold 0.6, clip 10"), and only the others' requests are scripted. One deterministic loop
      runs the matcher and the strategies: each request's market updates go to every strategy, and
      what a strategy sends goes back to the matcher, in a fixed order. Its output is an ordinary
      recording, so the steps, the replay and the viewer serve it unchanged. Then "what will the
      maker send now?" is answered by the maker's own code; why, by its inputs published at each
      step (its fair price, the trade ratio, the threshold), which claims can state and the tool
      check; and "what if its threshold were lower?" by the same outside world played again. It
      can be exact because the strategies' decisions read no clock: the market maker quotes at the
      best prices, stepping a quote back a tick when its fair price (the best bid and ask weighted
      by the opposite side's quantity) is within its threshold, and the liquidity taker sends its
      clip when a trade's size against what was left passes its threshold (the back end's
      `trading/strategy/market_maker.h`, `liquidity_taker.h`, `feature_engine.h`); the trade engine
      reads the time only to notice a quiet market. Rests on the book's market-making and
      liquidity-taking clients run and tested, and on running its trading engine without its
      threads and sockets, which is most of what backtesting on simulated time (6) needs too.
