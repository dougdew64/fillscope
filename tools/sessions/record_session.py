# Records a trading session on Fillscope's exchange (backend/): the book's market maker, liquidity
# taker and random client as the background, and planted slicers (trading/strategy/slicer.h there),
# each sending one large order in pieces its own way; then replays the recording, writing its market
# data, what each request did and who sent what.
#
#   python3 -I tools/sessions/record_session.py OUT_DIR [--exchange EXCHANGE_MAIN] [--trading TRADING_MAIN]
#       [--replay EXCHANGE_REPLAY] [--order-port PORT] [--incremental GROUP:PORT] [--snapshot GROUP:PORT]
#       [--at-least CLIENT:REQUESTS]... [--max-seconds SECONDS] [--live]
#
# OUT_DIR gets the exchange's recording (requests.rec), the replay's market data (market.dat), steps
# (steps.bin) and truth (truth.bin), session.json, which says who ran with what settings and each
# slicer's plan, and every program's log. A session is market data, which is never committed
# (docs/CHARTER.md, decision 5), so OUT_DIR may be in this repository only where git ignores it
# (data/, or build/). The programs are the debug build's unless given; the endpoints are the book's
# unless given. It records until the slicer has sent its every piece, and cancelled it (it cancels
# each piece once accepted), and each --at-least client has sent that many requests, then stops
# every program and replays; it fails, saying why, if that takes more than --max-seconds (120 by
# default) or a program exits.
#
# With --live, it starts the same market and leaves it running for a viewer to watch live, until it
# is stopped (or a program exits: the random client sends its last order after about 7 minutes, and
# the book's clients stop a minute after the market goes quiet); it replays nothing, and OUT_DIR
# gets only the recording and the logs. Every program dies with it, however it is stopped.
import argparse
import ctypes
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
PROGRAMS = REPOSITORY / "build" / "debug" / "backend" / "Chapter12"

# A strategy's settings for one ticker, as trading_main takes them: its clip, its threshold, and its
# risk limits (the largest order, the largest position, the loss at which it stops). The book's own
# (backend/Chapter12/scripts/run_clients.sh): its client 1, a market maker, and its client 3, a
# liquidity taker, on all eight tickers.
MAKER_SETTINGS = [(100, 0.6, 150, 300, -100), (60, 0.6, 150, 300, -100), (150, 0.5, 250, 600, -100),
                  (200, 0.4, 500, 3000, -100), (1000, 0.9, 5000, 4000, -100), (300, 0.8, 1500, 3000, -100),
                  (50, 0.7, 150, 300, -100), (100, 0.3, 250, 300, -100)]
TAKER_SETTINGS = [(300, 0.8, 350, 300, -300), (60, 0.7, 350, 300, -300), (350, 0.5, 250, 600, -300),
                  (200, 0.6, 500, 3000, -300), (3000, 0.5, 5000, 4000, -300), (300, 0.7, 3500, 3000, -300),
                  (50, 0.3, 350, 300, -300), (300, 0.8, 350, 300, -300)]

# The slicer, client 6, working three orders, one of each kind, each on a ticker of its own: equal
# pieces at regular gaps; the gaps jittered; the sizes spread. One client rather than three, as one
# firm's execution algorithm works several orders: each client is a trade engine with a book for
# every ticker, about 1.5 GB, and a session of six clients took 11.7 GB, measured, against 8.5 GB
# for four, which leaves CI's 16 GB room for the tests running beside it.
# Each piece is half the market maker's clip on its ticker: the maker quotes only while both sides
# have orders, and pieces of its whole clip took its whole quote, leaving the slicer's tickers
# one-sided after 29 to 59% of their updates, against 1 to 10% elsewhere; at half, 12 to 34% (one
# session each, measured). A piece that finds the other side empty fills nothing, and is cancelled.
# Each gap is 12 of the ticker's updates, about a second and a half (each ticker has about 8 a
# second), so each order is sent in about 30 s, in 20 pieces.
SLICER = 6
SLICES = [
    {"kind": "equal pieces, regular gaps",
     "ticker": 1, "side": "BUY", "total": 600, "piece": 30, "gap": 12, "gap_jitter": 0, "size_spread": 0},
    {"kind": "jittered gaps",
     "ticker": 2, "side": "SELL", "total": 1500, "piece": 75, "gap": 12, "gap_jitter": 6, "size_spread": 0},
    {"kind": "spread sizes",
     "ticker": 3, "side": "BUY", "total": 2000, "piece": 100, "gap": 12, "gap_jitter": 0, "size_spread": 50},
]

RECORDING_HEADER = 8  # "LLREQ001"
RECORDED = struct.Struct("<BIIQbqI")  # an MEClientRequest, packed: type, client, ticker, order id, side, price, qty
WARM_UP = "1"  # seconds, rather than the book's 10
# A snapshot every second, rather than the book's every minute: a client whose market data starts
# late, or loses a datagram, waits for the next snapshot to recover, deciding nothing meanwhile, and
# the book's client stops after a minute with no market data.
SNAPSHOT_SECONDS = "1"
RECOVERED = "Packet drops on"  # what the client's market data consumer logs as it starts to recover
PR_SET_PDEATHSIG = 1


LOG = None  # OUT_DIR/record_session.log, once OUT_DIR is made: what it said, for a caller that discards its output


def say(text, file=sys.stdout):
    print(f"record_session: {text}", file=file)
    if LOG is not None:
        with open(LOG, "a") as log:
            log.write(text + "\n")


def fail(why):
    say(why, file=sys.stderr)
    sys.exit(1)


def die_with_parent():
    """Run in each child before it starts: killed if this script dies, however it dies, so no
    exchange or client is left spinning."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL)


def settings_args(settings):
    return [str(value) for ticker in settings for value in ticker]


def requests_by_client(recording):
    """How many requests each client has in the recording so far."""
    counts = {}
    data = recording.read_bytes() if recording.exists() else b""
    for at in range(RECORDING_HEADER, len(data) - RECORDED.size + 1, RECORDED.size):
        client = RECORDED.unpack_from(data, at)[1]
        counts[client] = counts.get(client, 0) + 1
    return counts


def read_plans(path):
    """A slicer's plan file, a line a piece, its ticker, its quantity and its gap: each ticker's
    pieces, [quantity, gap], in order."""
    plans = {}
    text = path.read_text()
    for line in text[: text.rfind("\n") + 1].splitlines():  # whole lines only: it may be being written
        ticker, qty, gap = (int(field) for field in line.split())
        plans.setdefault(ticker, []).append([qty, gap])
    return plans


def inside_repository_unignored(path):
    try:
        relative = path.resolve().relative_to(REPOSITORY)
    except ValueError:
        return False
    ignored = subprocess.run(["git", "-C", str(REPOSITORY), "check-ignore", "-q", str(relative)])
    return ignored.returncode != 0


def parse_args():
    parser = argparse.ArgumentParser(description="Records a session of planted slicers among the book's clients.")
    parser.add_argument("out_dir", type=pathlib.Path)
    parser.add_argument("--exchange", type=pathlib.Path, default=PROGRAMS / "exchange_main")
    parser.add_argument("--trading", type=pathlib.Path, default=PROGRAMS / "trading_main")
    parser.add_argument("--replay", type=pathlib.Path, default=PROGRAMS / "exchange_replay")
    parser.add_argument("--order-port")
    parser.add_argument("--incremental")
    parser.add_argument("--snapshot")
    parser.add_argument("--at-least", action="append", default=[], metavar="CLIENT:REQUESTS")
    parser.add_argument("--max-seconds", type=float, default=120.0)
    parser.add_argument("--live", action="store_true", help="leave the market running until stopped; replay nothing")
    args = parser.parse_args()
    args.wanted = {}
    for item in args.at_least:
        client, _, count = item.partition(":")
        if not (client.isdigit() and count.isdigit()):
            parser.error(f"--at-least {item}: not CLIENT:REQUESTS")
        args.wanted[int(client)] = int(count)
    return args


def main():
    args = parse_args()
    out = args.out_dir
    if inside_repository_unignored(out):
        fail(f"{out} is in the repository where git doesn't ignore it; a session is market data, never committed")
    out.mkdir(parents=True, exist_ok=True)
    for stale in ("requests.rec", "market.dat", "steps.bin", "truth.bin", "session.json", "record_session.log"):
        (out / stale).unlink(missing_ok=True)
    global LOG
    LOG = out / "record_session.log"
    endpoints = []
    for option, value in (("--order-port", args.order_port), ("--incremental", args.incremental), ("--snapshot", args.snapshot)):
        if value is not None:
            endpoints += [option, value]
    order_port = int(args.order_port) if args.order_port else 12345
    exchange, trading, replay = (str(path.resolve()) for path in (args.exchange, args.trading, args.replay))

    clients = [{"client": 1, "strategy": "MAKER", "args": ["MAKER", *settings_args(MAKER_SETTINGS)], "settings": MAKER_SETTINGS},
               {"client": 3, "strategy": "TAKER", "args": ["TAKER", *settings_args(TAKER_SETTINGS)], "settings": TAKER_SETTINGS},
               {"client": 5, "strategy": "RANDOM", "args": ["RANDOM"]}]
    slicer_args = ["SLICER"]
    for s in SLICES:
        slicer_args += ["--slice", str(s["ticker"]), s["side"], str(s["total"]), str(s["piece"]), str(s["gap"]),
                        "--gap-jitter", str(s["gap_jitter"]), "--size-spread", str(s["size_spread"])]
    slicer_args += ["--slice-plan", f"slicer-{SLICER}.plan"]  # each order seeded with the client's id and its place
    clients.append({"client": SLICER, "strategy": "SLICER", "args": slicer_args,
                    "orders": [{**s, "seed": SLICER + i} for i, s in enumerate(SLICES)]})

    programs = []
    deadline = time.monotonic() + args.max_seconds

    def start(argv, name):
        log = open(out / f"{name}.out", "wb")
        programs.append((name, subprocess.Popen(argv, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                                preexec_fn=die_with_parent)))

    def check_running():
        for name, program in programs:
            if program.poll() is not None:
                fail(f"{name} exited, status {program.returncode} (its output is in {out / (name + '.out')})")

    try:
        start([exchange, "--snapshot-interval-seconds", SNAPSHOT_SECONDS, "--record", "requests.rec", *endpoints], "exchange")
        while True:
            check_running()
            with socket.socket() as probe:
                if probe.connect_ex(("127.0.0.1", order_port)) == 0:
                    break
            if time.monotonic() > deadline:
                fail(f"the exchange didn't open its order port, {order_port}")
            time.sleep(0.1)
        # The market's first update is the random client's first order. The others decide only on
        # market data, so they start first, and it only once each is listening: a client whose market
        # data starts late recovers from a snapshot, which on a busy machine two did in each of three
        # sessions (measured), and its strategy is called on each of the snapshot's orders as its book
        # is rebuilt, deciding on a book half built, and the slicer counts them as updates.
        def start_client(client):
            start([trading, str(client["client"]), *client["args"], "--warm-up-seconds", WARM_UP, *endpoints],
                  f"client-{client['client']}")

        for client in clients:
            if client["strategy"] != "RANDOM":
                start_client(client)
        for client in clients:
            if client["strategy"] != "RANDOM":
                # Its market data consumer's first line, written (and flushed within 10 ms) once its
                # thread runs, after it has joined the stream.
                consumer_log = out / f"trading_market_data_consumer_{client['client']}.log"
                while not consumer_log.exists() or consumer_log.stat().st_size == 0:
                    check_running()
                    if time.monotonic() > deadline:
                        fail(f"client {client['client']} never started listening to the market data")
                    time.sleep(0.05)
        for client in clients:
            if client["strategy"] == "RANDOM":
                start_client(client)
        if args.live:
            say(f"live: the exchange and clients {', '.join(str(c['client']) for c in clients)}, on "
                f"{'the book' + chr(39) + 's endpoints' if not endpoints else 'the endpoints given'}, until stopped")
            # Stopped by a signal, as a debugger stops it: out through the `finally` below.
            signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
            while True:
                check_running()
                time.sleep(0.5)
        # The slicer writes its plans as it starts, before it connects to anything; read again each
        # time, since it may be read while being written, before the slicer could have sent anything.
        plan_file = out / f"slicer-{SLICER}.plan"
        while True:
            plans = read_plans(plan_file) if plan_file.exists() else {}
            pieces = sum(len(plan) for plan in plans.values()) if len(plans) == len(SLICES) else None
            counts = requests_by_client(out / "requests.rec")
            # Two requests a piece: the slicer cancels each piece once it is accepted.
            wanted = {**args.wanted, SLICER: max(args.wanted.get(SLICER, 0), 2 * (pieces or 1))}
            if pieces is not None and all(counts.get(client, 0) >= count for client, count in wanted.items()):
                break
            check_running()
            if time.monotonic() > deadline:
                short = ", ".join(f"client {c} {counts.get(c, 0)} of {n}" for c, n in sorted(wanted.items()))
                fail(f"after {args.max_seconds:.0f} s, not every client had sent enough requests: {short}")
            time.sleep(0.25)
    finally:
        for _, program in programs:
            program.kill()
        for _, program in programs:
            program.wait()
    # Every program stopped: the recording holds every request the matcher took, and the plan file
    # is whole.
    slicer = next(client for client in clients if client["strategy"] == "SLICER")
    plans = read_plans(plan_file)
    for order in slicer["orders"]:
        order["plan"] = plans[order["ticker"]]

    replayed = subprocess.run([replay, "requests.rec", "market.dat", "--steps", "steps.bin", "--truth", "truth.bin"], cwd=out,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if replayed.returncode != 0:
        fail(f"the replay failed: {replayed.stdout.decode(errors='replace').strip()}")
    replay_said = replayed.stdout.decode(errors="replace").strip().splitlines()
    session = {"recording": "requests.rec", "market_data": "market.dat", "steps": "steps.bin", "truth": "truth.bin",
               "clients": []}
    for client in clients:
        entry = {key: client[key] for key in ("client", "strategy", "orders") if key in client}
        # Whether it recovered from a snapshot: a slicer that did counted the snapshot's orders as
        # updates on its ticker, which the market data doesn't have.
        consumer_log = out / f"trading_market_data_consumer_{client['client']}.log"
        entry["recovered"] = consumer_log.exists() and RECOVERED in consumer_log.read_text(errors="replace")
        if "settings" in client:
            entry["tickers"] = [dict(zip(("clip", "threshold", "max_order", "max_position", "max_loss"), ticker))
                                for ticker in client["settings"]]
        session["clients"].append(entry)
    (out / "session.json").write_text(json.dumps(session, indent=2) + "\n")
    counts = requests_by_client(out / "requests.rec")
    say(f"{sum(counts.values())} requests ({', '.join(f'client {c}: {n}' for c, n in sorted(counts.items()))}); "
        f"{replay_said[-1] if replay_said else ''}")


if __name__ == "__main__":
    main()
