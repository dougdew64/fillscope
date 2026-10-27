# Feature backlog

**Purpose:** the features envisioned for Fillscope and not yet built, each with what it is and what
it rests on. **Status:** live plan; an item is removed when it is built (git keeps it), and the file
is deleted when empty. **Read when:** choosing what to build next, or asking what Fillscope will do.

**The showcase is item 18:** a pattern detector trained offline and used online, inside a strategy
(the owner, 2026-10-26). Most other features are chosen to serve it, and its demonstration.
Next is slicing, seen before it is detected: the first part of item 13, as [PLAN.md](PLAN.md)'s
steps 21 to 25. Answers (15) are built, and their later parts remain here.
The other items aren't in order yet. The [charter](CHARTER.md)'s rules bind every one of them.
Known defects and test gaps are in [TECH-DEBT.md](TECH-DEBT.md).

Built so far: the back end, built and tested by Fillscope at a pinned commit; the live view of the
exchange's books; deterministic replay; the matching engine stepped in a debugger while the book
view follows; answers, scenarios the real matcher plays, explained step by step; the viewer's
log, which records a crash; and time-shifting the live market data, as a television recorder does:
pause, play behind live, step, and a live moment's address ([ARCHITECTURE.md](ARCHITECTURE.md)).

## The showcase: a detector trained offline, used online in a strategy
18. **A slicing detector trained offline, used online by a market-making strategy, and its worth
    measured exactly by replay** (the owner, 2026-10-26: the showcase; most other features serve
    it). A model learns, offline, to recognize a trader slicing a large order; online, as the
    market data arrives, it scores each trade, and a strategy the owner writes uses the score to
    protect itself; replay then measures, exactly, what the detector changed. The parts below are
    proposed, to be decided when it is planned.
    - **Labelled data, generated rather than found.** Real market data is anonymous, so who was
      slicing is never known; on Fillscope's exchange the recording knows every order's client, so
      every trade comes labelled. Many sessions are recorded: Ghosh's market maker and liquidity
      taker as the background, and planted slicers of varied behaviour (fixed sizes and random
      ones, regular timing and jittered, different paces). Deterministic replay makes each
      reproducible. *For later discussion (2026-10-27):* a script recording many sessions with
      varied slicers (sizes, gaps, paces), each with its truth and a summary, useful for study
      before it is training data.
    - **Features written once, in C++, used both offline and online.** Computed incrementally, an
      update at a time: how many trades of one side and size lately, how regular the gaps between
      them, how a size compares with the usual, whether it took the best price, the recent rate. A
      tool runs them over the recorded sessions and writes the training table; live, the same code
      computes the same features. Training/serving skew, the commonest way such systems fail, is
      ruled out by construction. Each feature uses only what was known at its moment.
    - **Training offline, in Python** (scikit-learn or LightGBM, the industry's norm): gradient-boosted
      trees or logistic regression, explainable, on tabular features. Trained on some sessions and
      tested on held-out ones, slicer behaviours never seen in training among them: the honest
      test of how far it generalizes. Measured by hits against false alarms, never accuracy (most
      trades aren't a slicer's). Its result is a small, versioned file: weights or trees.
    - **Inference online, in C++,** from that file, in microseconds a trade, with no machine-learning
      runtime in the trading path (trees or a linear model are a few dozen lines to evaluate).
      Checked independently of the training: the C++ scores equal Python's on the same table.
    - **The detector is a feature** (13, *Where the detector runs*): fed each market update before
      the strategy is, as the book's `FeatureEngine` is, and read by the strategy, never computed by
      it. So training and the strategy share it: the features computed over recordings for training
      are the ones the strategy reads live.
    - **The strategy, the owner's, a Fillscope program (the owner, 2026-10-27):** a client of the
      exchange that speaks its protocols and links nothing of the back end, as a firm's client
      shares only the exchange's published protocols, never its code. It reads the market data over
      multicast, as Fillscope's feed does already, and sends its orders over TCP: each request an
      8-byte sequence number then the 30-byte request, each response an 8-byte sequence number then
      the 42-byte response (`exchange/order_server/client_request.h` and `client_response.h` in the
      back end). Its design is the book's client's: its books, then its features (the detector among
      them), then the strategy, then the order manager (one order a side, cancelled before it is
      replaced), the risk check and the position; each part written for it, to the same design. The
      book's market maker, taker and random client, and the planted slicer (13), stay in the fork,
      as the rest of the market. The strategy itself is a market maker that reads the score, and when a slicer is
      likely active on one side, widens or leans its quotes away from it, since that flow moves the
      price. Run in the deterministic loop of strategies the backlog describes under answers (15,
      *strategies in the loop*).
    - **What a Fillscope client takes on** (discussed 2026-10-27, nothing decided). Of the book's
      client's layers, the market data and the books are Fillscope's already (`src/feed/`, tested
      against the exchange's own books, recovery included), which is the strongest reason for the
      move: the maker sits on the code the viewer trusts. New: the order gateway, a TCP connection
      with sequence numbers both ways, less forgiving than it looks, since the exchange binds a
      client's id to the connection it first hears it on and drops a request out of sequence
      without a word to the client (`exchange/order_server/order_server.h` in the back end), so a
      gateway's mistake shows as orders that never happen, not as an error; it must count exactly,
      and check the responses' numbers too. Then the features and the detector, one library the
      viewer, the scorer and the maker link; and the strategy, order manager, risk check and
      position, to the book's design.
    - **Lessons from steps 21 to 24 for its design** (2026-10-27):
      - *Don't decide while recovering.* The book's client calls its strategy on each of a
        snapshot's orders as it rebuilds its books, so it decides on books half built, which failed
        Fillscope's session test (PLAN.md, step 24). Fillscope's books say whether they are
        recovering or live, so its strategy can wait.
      - *Have a rule for a one-sided book.* The book's maker quotes only while both sides have
        orders; once one empties, it stops on that ticker, which left the slicer's tickers
        one-sided. Quoting around the last trade, or a reference price, are two such rules.
      - *Moving a quote is slow in the book's order manager:* one order a side, its cancel
        confirmed before the replacement is sent, so a side has no quote meanwhile, most of all
        when a slicer is pushing the price. The exchange has no modify and no immediate-or-cancel:
        moving a quote is a cancel and a new order.
      - *Memory:* each of the book's clients takes about 1.5 GB (every book preallocated for a
        million orders); Fillscope's books grow as needed, so its client can be far smaller, which
        matters for sessions on CI's 16 GB.
    - **Threading, not settled by following the book's design.** The book's client runs three
      busy-spinning threads joined by lock-free queues; the layering agreed doesn't require that. A
      single-threaded loop (wait on the sockets, then market data, features, strategy, orders, in
      turn) is simpler, starves nothing, and is easier to test. Its cost is fairness: the book's
      clients spin and react within microseconds, and a slower maker reaches the queue after them,
      which would bias a comparison with them; for the maker compared with itself, with and without
      the detector, it matters much less.
    - **Who writes what, to be decided when it is planned** (charter, §4): a natural split is the
      strategy and the detector the owner's, and the gateway with its protocol's tests, and the risk
      and position plumbing, Claude's, unless the owner wants some of them. *For later discussion
      (2026-10-27):* Claude's share could be started before the maker is planned, once the split is
      decided.
    - **Its worth, measured exactly.** The same recorded day played twice, with the detector and
      without, compared: profit, fills that went against the strategy, inventory. Deterministic
      replay makes the difference the detector's alone, which most backtests can't claim. Each
      flag can be paused on and stepped through (time-shifting, built), and its moment rebuilt.
      - *The catch, and the question to settle before the maker is designed* (2026-10-27): a
        recording is a fixed list of requests. Replayed with the maker behaving differently, the
        background's requests are replayed as they were: they don't react to it. Only the maker's
        requests change, and the matcher gives exact results for that list: exact, but a
        counterfactual in which the rest of the market is deaf to the maker. The alternative, two
        live sessions with every client reacting, is realistic, but no two live runs are alike, so
        it compares distributions over many sessions, not two exact runs. Exact replay needs the
        maker to run in lockstep with the replayed matcher, an event at a time, easiest if the
        strategy is a pure function from events to requests, with no clock and no thread deciding
        anything; live sessions need nothing special, but many runs. Recommended (Claude): the
        strategy written as a pure function of events either way, which keeps exact replay
        possible, makes it testable as the slicer is, and costs live sessions nothing. Which
        measurement the showcase claims is the owner's to decide.
    - **The demonstration:** a video of the whole loop: the slicer hidden in the background, the
      detector finding it, the strategy stepping aside, and the measured difference.
    - **Limits, said wherever results are shown.** A model trained on planted slicers learns them;
      the held-out behaviours show how far it generalizes, not how it would fare against real ones.
      Ghosh's exchange is simple (no hidden orders, no auctions), so its patterns are cleaner than a
      real market's. On real data (5), with no labels, its flags are guesses, and shown as such.
    - **Rests on** the rule-based detector and its truth and scoring (13), which it is measured
      against (and could be that item's third scene: the detector improved, finding the slicer
      again); the sweep of settings (14), its tuning half; strategies in the loop (15); and, on
      real data, replay of ITCH and fill simulation (5, 7).


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

5. **Replay of Nasdaq TotalView-ITCH 5.0 data on the exchange's market data feed.** Its parser is to be the first
   worked example of checking AI output (charter §3, decision 6). Proposed, not yet decided:
   hand-written decoders for the messages the book needs, and the rest generated. AI builds a
   model of the specification, checked mechanically and against an independent model; a
   deterministic generator writes the decoders from it; they must agree with the hand-written ones
   on every message of a real trading day. The data is downloaded locally, never committed.
   - **Decided (the owner, 2026-10-26): publish it first, then simulate clients' orders against it;
     never re-match it.** First, a program decodes a day's ITCH and publishes it on the exchange's
     market data feed, in the format the viewer reads, as `exchange_replay` publishes a recording:
     Nasdaq's book, order by order, exactly as it happened; no client places orders. Then, with fill
     simulation (7), clients' orders are simulated against the replayed book: an order joins the
     queue at its price behind the real orders there, and fills when real executions at that price
     have used up the queue ahead of it, after a simulated latency; it doesn't change the historical
     market, the method's known limitation, said wherever its results are shown. Not re-matched in
     Ghosh's matcher: many of the orders behind ITCH's executions never appear in it (market,
     immediate-or-cancel, hidden and midpoint orders, the auctions), and the matcher has none of
     them, so its book would drift from Nasdaq's and show what didn't happen.
   - **To carry over:** the exchange's 8 tickers (8 stocks of a day's thousands), its order ids
     (ITCH's are far larger, so renumbered), and ITCH's nanosecond timestamps, which the exchange's
     own feed lacks: time-shifting could then play at the exchange's pace, not the viewer's.
6. **Backtesting on simulated time, with recovery from a snapshot.** Latency in a backtest is
   simulated, not the network's ([DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md)).
7. **Fill simulation:** queue position, latency, fees.
8. **Validation:** how far the results can be trusted, and a written report.
9. **Throughput:** measured messages per second.

## Tools around the app
10. **A command-line tool for counterfactual replays,** which the assistant working beside the app
    runs, since the assistant isn't in it (charter §3, decision 2). It is tested below the UI, with the
    core unit tests and replay fidelity checks.
11. **A small synthetic market data generator for demos,** since market data is never committed
    (charter §3, decision 5).
12. **Demo recordings:** each feature shown by a scripted demo that is part of the test suite
    (charter §2, priority 3), recorded with the Dear ImGui Test Engine, which already drives the
    viewer's UI tests (charter §3, decision 4). Answers' demo, the live book's and time-shifting's
    are built and record themselves ([ARCHITECTURE.md](ARCHITECTURE.md), §10, *The demos*); what
    remains is a demo for each other feature: a replay, and the debugger lockstep.

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
    - **The interface's timing (the owner, 2026-10-27).** Its main user is now the owner's own market
      maker (18), so the interface comes out of the maker's design rather than first; the scorer,
      the detector's tests and the viewer's flags, Claude's, are written against it once the
      detector exists.
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
    - **Where the detector runs (the owner, 2026-10-27): it is a feature, in the sense of the
      book's `FeatureEngine`** (`trading/strategy/feature_engine.h` in the back end). There, on each
      market update, the trade engine updates the features first (the fair price, the size of an
      aggressive trade against the best quantity), then calls the strategy, which only reads them.
      Trading firms' systems separate them the same way: signals computed in one layer, in the
      trading path, an update at a time and in microseconds; the strategies that act on them in
      another. So the detector takes one market update at a time, keeps its own state, uses only
      what is known at that moment, and gives a score; it decides nothing. A market-making strategy
      reads the score to protect itself (18); the scorer runs the detector over a replay and
      compares its flags with the truth; the viewer runs it as each update arrives and shows its
      flags. All three run the same code on the same updates. The same kind of detector also runs,
      in firms and exchanges, outside any strategy: in research, over recordings, as the scorer
      does here; and in surveillance, flagging patterns for review. Here, the viewer is the
      research and monitoring tool, and the strategy side is the detector's home in a real system.
      The detector's code is Fillscope's, and so is the strategy that reads it (18, *The
      strategy*): nothing crosses between the repositories.
    - **The display,** a panel of the viewer, beside the book: the viewer runs the detector as
      each update arrives. Each order a dot, update number across and price up, gray at first; in
      the first version, the trades flagged as a slice take a color, and with the later patterns
      the orders the detector groups take one color per inferred trader, joined by lines. Then the
      truth outlined, with the hits, misses and false alarms, and the score.
    - **Seen first, from the truth (the owner, 2026-10-26).** Before the detector is written, the
      display shows the slicer's trades from the truth alone, and a demo shows it, so the owner
      learns the pattern by watching it ([PLAN.md](PLAN.md), steps 21 to 25).
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
    **Built:** [PLAN.md](PLAN.md), steps 11 to 14 (2026-10-25); what remains is its next parts,
    below: who made money, and strategies in the loop.
    - **The assistant isn't in the app** (charter §3, decision 2). The context file goes from the
      viewer to the assistant; an answer goes the other way, as a file the assistant writes,
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
