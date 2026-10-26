# The back end (backend/, the fork of the trading system in Ghosh's book) as Fillscope builds it: the
# exchange and a client run together, orders reach the exchange, and market data leaves it. Checked
# from outside, as the viewer will see it: this test joins the exchange's incremental market data
# feed and decodes what arrives.
#
#   python3 -I tests/backend/backend_system_test.py EXCHANGE_MAIN TRADING_MAIN
#
# The exchange takes orders over TCP and sends its market data by UDP multicast, on the loopback
# interface. By default on the book's ports (12345, and 233.252.14.3:20001 for the incremental
# updates); this test gives it and its client ports of the test's own, by the rule in RUNNING.md
# (*Ports of their own*), so nothing else on the machine using the book's ports (a replay stepped in
# VS Code, another run of the tests) reaches it, which once let it pass on a replay's market data.
import contextlib
import fcntl
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest



def claim_slot():
    """The slot this process holds, 0 to 999, by RUNNING.md's rule: the first of the files
    fillscope-test-ports/slot-N in the temporary directory it can lock, held until it exits."""
    directory = os.path.join(tempfile.gettempdir(), "fillscope-test-ports")
    os.makedirs(directory, exist_ok=True)
    for slot in range(1000):
        fd = os.open(os.path.join(directory, f"slot-{slot}"), os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o666)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return slot  # the descriptor stays open, and the lock held, until the process exits
        except BlockingIOError:
            os.close(fd)
    raise RuntimeError(f"every slot in {directory} is taken")


SLOT = claim_slot()
ORDER_PORT = 22000 + 10 * SLOT
INCREMENTAL_GROUP, INCREMENTAL_PORT = "233.252.14.3", 22000 + 10 * SLOT + 1
SNAPSHOT_GROUP, SNAPSHOT_PORT = "233.252.14.1", 22000 + 10 * SLOT + 2
# For the exchange and the client both.
ENDPOINTS = ["--order-port", str(ORDER_PORT), "--incremental", f"{INCREMENTAL_GROUP}:{INCREMENTAL_PORT}",
             "--snapshot", f"{SNAPSHOT_GROUP}:{SNAPSHOT_PORT}"]
# The book's own incremental stream, where a replay stepped in VS Code publishes.
BOOK_INCREMENTAL = ("233.252.14.3", 20001)
# MDPMarketUpdate, packed (market_update.h): seq_num_ (size_t), then MEMarketUpdate: type_ (uint8),
# order_id_ (uint64), ticker_id_ (uint32), side_ (int8), price_ (int64), qty_ (uint32),
# priority_ (uint64). Little-endian, no padding.
UPDATE = struct.Struct("<QBQIbqIQ")
ADD, CANCEL, TRADE = 2, 4, 5
IMPOSTOR_PRICE = 987654321  # BookStreamSender's; the client's prices are a few hundred
MAX_TICKERS = 8  # ME_MAX_TICKERS, types.h
MAX_ORDER_IDS = 1024 * 1024  # ME_MAX_ORDER_IDS, types.h
# On the order port (client_request.h, client_response.h), packed, little-endian: a request is its
# sequence number (size_t), then MEClientRequest: type_ (uint8), client_id_ (uint32), ticker_id_
# (uint32), order_id_ (uint64), side_ (int8), price_ (int64), qty_ (uint32); a response is its
# sequence number, then MEClientResponse: type_, client_id_, ticker_id_, client_order_id_ (uint64),
# market_order_id_ (uint64), side_, price_, exec_qty_ (uint32), leaves_qty_ (uint32). A recording is
# its 8-byte header, then each MEClientRequest.
REQUEST = struct.Struct("<QBIIQbqI")
RESPONSE = struct.Struct("<QBIIQQbqII")
RECORDED = struct.Struct("<BIIQbqI")
NEW, BUY, ACCEPTED, REJECTED = 1, 1, 1, 5
# The exchange opens its port in about a second; the client, told to warm up for WARM_UP second
# rather than the book's 10, sends its first order about a second after that (orders then follow
# every 20 ms). The limits allow for the sanitizer build, which is slower, and a loaded machine;
# FIRST_UPDATE_LIMIT is still short of the book's 10 s, so a warm-up the client ignored fails it.
EXCHANGE_LIMIT = 30.0
WARM_UP = "1"
FIRST_UPDATE_LIMIT = 9.0
FIRST_UPDATES_LIMIT = 60.0
UPDATES_WANTED = 200

EXCHANGE, TRADING = (os.path.abspath(path) for path in sys.argv[1:3])  # they run in another directory


def order_port_open():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        return probe.connect_ex(("127.0.0.1", ORDER_PORT)) == 0


def incremental_feed(interface="127.0.0.1"):
    """A socket that receives the incremental feed, joined as the book's own consumer joins it
    (socket_utils.h): bound to the port on any address, a member of the group on the loopback
    interface. Joined on 0.0.0.0, the default interface (where the book's exchange sent it), it
    receives only what arrives there: IP_MULTICAST_ALL off keeps it from receiving the group from
    another interface because another socket joined it there."""
    feed = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        feed.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        feed.setsockopt(socket.IPPROTO_IP, getattr(socket, "IP_MULTICAST_ALL", 49), 0)
        feed.bind(("", INCREMENTAL_PORT))
        membership = struct.pack("4s4s", socket.inet_aton(INCREMENTAL_GROUP), socket.inet_aton(interface))
        feed.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    except OSError:
        feed.close()
        raise
    return feed


def default_interface_feed():
    """The feed as joined on the default interface; on a machine with none (no network), nothing to
    receive, which a `with` closes as it would a socket."""
    try:
        return incremental_feed("0.0.0.0")
    except OSError:
        return contextlib.nullcontext()


class BookStreamSender:
    """Sends updates on the book's own incremental stream, on the loopback interface, every 5 ms, as
    a replay stepped in VS Code would, numbered from 1 like a real feed; each has the price
    IMPOSTOR_PRICE, which no order of the client's has. A `with` starts and stops it."""

    def __init__(self):
        self.stop = threading.Event()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.socket.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
        self.thread = threading.Thread(target=self.send)

    def send(self):
        seq = 0
        while not self.stop.wait(0.005):
            seq += 1
            self.socket.sendto(UPDATE.pack(seq, ADD, seq, 0, 1, IMPOSTOR_PRICE, 1, seq), BOOK_INCREMENTAL)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join()
        self.socket.close()


def decode(datagram):
    """The updates in one datagram: the publisher may send several in one."""
    if len(datagram) % UPDATE.size != 0:
        raise AssertionError(f"a datagram of {len(datagram)} bytes isn't a whole number of {UPDATE.size}-byte updates")
    return [UPDATE.unpack_from(datagram, offset) for offset in range(0, len(datagram), UPDATE.size)]


class Process:
    """A back-end program in its own process group, in the test's directory (it writes its logs
    there). Killed at the end: the test is not about how they shut down, which takes a minute."""

    def __init__(self, args, cwd):
        self.process = subprocess.Popen(args, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                                        start_new_session=True)

    def running(self):
        return self.process.poll() is None

    def kill(self):
        if self.running():
            os.killpg(self.process.pid, signal.SIGKILL)
        self.process.wait()


class SlotTest(unittest.TestCase):
    def test_this_process_holds_its_slot(self):
        """No other process can claim this one's slot while it runs: a second lock on the slot's file,
        from another open of it, is refused."""
        path = os.path.join(tempfile.gettempdir(), "fillscope-test-ports", f"slot-{SLOT}")
        fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        try:
            with self.assertRaises(BlockingIOError, msg=f"slot {SLOT} could be claimed again"):
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            os.close(fd)


class ExchangeCommandLineTest(unittest.TestCase):
    def test_a_bad_command_line_is_refused_and_leaves_the_recording_as_it_was(self):
        """Refused before anything starts, so these need no exchange to be stopped. Each interval was
        once accepted: inf and 1e10 overflowed the nanoseconds, 1e-12 made them 0."""
        with tempfile.TemporaryDirectory() as directory:
            recording = os.path.join(directory, "live.rec")
            with open(recording, "w") as file:
                file.write("an earlier run")
            for args in (["--snapshot-interval-seconds", "inf"], ["--snapshot-interval-seconds", "1e10"],
                         ["--snapshot-interval-seconds", "1e-12"], ["--snapshot-interval-seconds", "nan"],
                         ["--snapshot-interval-seconds", "x"], ["--record", recording, "--bogus", "x"],
                         ["--record", recording, "--snapshot-interval-seconds"],
                         ["--record", recording, "--order-port", "0"], ["--record", recording, "--order-port", "65536"],
                         ["--record", recording, "--incremental", "10.0.0.1:22001"],
                         ["--record", recording, "--snapshot", "233.252.14.1"], ["--record", recording, "--snapshot"]):
                result = subprocess.run([EXCHANGE, *args], cwd=directory, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2, args)
                self.assertIn("usage: exchange_main", result.stderr, args)
            with open(recording) as file:
                self.assertEqual(file.read(), "an earlier run", "a refused command line left the recording alone")


class ClientCommandLineTest(unittest.TestCase):
    def test_a_bad_warm_up_is_refused(self):
        """Refused before anything starts, so these need no client to be stopped."""
        with tempfile.TemporaryDirectory() as directory:
            for value in (["-1"], ["3601"], ["inf"], ["nan"], ["x"], ["1s"], []):
                args = [TRADING, "5", "RANDOM", "--warm-up-seconds", *value]
                result = subprocess.run(args, cwd=directory, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1, value)
                self.assertIn("USAGE trading_main", result.stderr, value)
                self.assertIn("[--warm-up-seconds SECONDS]", result.stderr, value)


def start_exchange(test, args, directory):
    """The exchange, started in `directory`, once its order port is open."""
    test.assertFalse(order_port_open(), f"something is already listening on this test's order port, {ORDER_PORT}")
    exchange = Process([EXCHANGE, *args, *ENDPOINTS], directory)
    deadline = time.monotonic() + EXCHANGE_LIMIT
    while not order_port_open():
        if not exchange.running() or time.monotonic() > deadline:
            exchange.kill()
            test.fail(f"the exchange didn't open port {ORDER_PORT} (exit code {exchange.process.returncode})")
        time.sleep(0.1)
    return exchange


def receive_response(connection):
    """The next response on the order port, as (seq, type, client, ticker, client_order_id, ...)."""
    data = b""
    while len(data) < RESPONSE.size:
        chunk = connection.recv(RESPONSE.size - len(data))
        if not chunk:
            raise AssertionError("the exchange closed the connection")
        data += chunk
    return RESPONSE.unpack(data)


class ExchangeRefusesWhatItCantProcessTest(unittest.TestCase):
    def test_an_order_past_the_order_ids_is_rejected_unrecorded_and_the_exchange_runs_on(self):
        """An order id past the matcher's table once ended the exchange (an exception in a noexcept
        function); the order server now rejects it before the matcher sees it."""
        with tempfile.TemporaryDirectory() as directory:
            exchange = start_exchange(self, ["--record", "live.rec"], directory)
            try:
                with socket.create_connection(("127.0.0.1", ORDER_PORT), timeout=10) as connection:
                    connection.sendall(REQUEST.pack(1, NEW, 9, 0, MAX_ORDER_IDS, BUY, 100, 10))
                    seq, kind, client, ticker, order, *_ = receive_response(connection)
                    self.assertEqual((seq, kind, client, ticker, order), (1, REJECTED, 9, 0, MAX_ORDER_IDS))
                    connection.sendall(REQUEST.pack(2, NEW, 9, 0, 1, BUY, 100, 10))
                    seq, kind, client, ticker, order, *_ = receive_response(connection)
                    self.assertEqual((seq, kind, client, ticker, order), (2, ACCEPTED, 9, 0, 1))
                self.assertTrue(exchange.running(), f"the exchange exited with code {exchange.process.returncode}")
            finally:
                exchange.kill()
            # Each request is written as the matcher processes it, so the recording is whole though the
            # exchange was killed; the rejected order isn't in it.
            with open(os.path.join(directory, "live.rec"), "rb") as file:
                recording = file.read()
        self.assertEqual(recording[:8], b"LLREQ001")
        requests = [RECORDED.unpack_from(recording, offset) for offset in range(8, len(recording), RECORDED.size)]
        self.assertEqual(requests, [(NEW, 9, 0, 1, BUY, 100, 10)])


class ExchangeAndClientTest(unittest.TestCase):
    def test_a_clients_orders_reach_the_exchange_and_its_market_data_leaves_it(self):
        self.assertFalse(order_port_open(), f"something is already listening on this test's order port, {ORDER_PORT}")
        with tempfile.TemporaryDirectory() as directory, incremental_feed() as feed, \
                default_interface_feed() as elsewhere, BookStreamSender():
            exchange = Process([EXCHANGE, *ENDPOINTS], directory)
            client = None
            try:
                deadline = time.monotonic() + EXCHANGE_LIMIT
                while not order_port_open():
                    self.assertTrue(exchange.running(), f"the exchange exited with code {exchange.process.returncode}")
                    self.assertLess(time.monotonic(), deadline, f"the exchange didn't open port {ORDER_PORT}")
                    time.sleep(0.1)
                client = Process([TRADING, "5", "RANDOM", "--warm-up-seconds", WARM_UP, *ENDPOINTS], directory)

                updates = []
                feed.settimeout(1.0)
                client_started = time.monotonic()
                deadline = client_started + FIRST_UPDATES_LIMIT
                while len(updates) < UPDATES_WANTED:
                    if not updates:
                        self.assertLess(time.monotonic() - client_started, FIRST_UPDATE_LIMIT,
                                        f"no market update {FIRST_UPDATE_LIMIT:.0f} s after the client started, "
                                        f"told to warm up for {WARM_UP}")
                    self.assertTrue(exchange.running(), f"the exchange exited with code {exchange.process.returncode}")
                    self.assertTrue(client.running(), f"the client exited with code {client.process.returncode}")
                    self.assertLess(time.monotonic(), deadline,
                                    f"only {len(updates)} market updates arrived in {FIRST_UPDATES_LIMIT:.0f} s")
                    try:
                        updates += decode(feed.recv(65536))
                    except socket.timeout:
                        pass
            finally:
                if client is not None:
                    client.kill()
                exchange.kill()

            # On loopback only: none of it reached the default interface, which leads off the machine.
            if elsewhere is not None:
                elsewhere.setblocking(False)
                arrived = 0
                try:
                    while elsewhere.recv(65536):
                        arrived += 1
                except BlockingIOError:
                    pass
                self.assertEqual(arrived, 0, "the exchange's market data reached the default interface, off the machine")

        # The feed's sequence numbers count from 1 with no gap: the test saw every update, in order.
        self.assertEqual([u[0] for u in updates], list(range(1, len(updates) + 1)))
        impostors = [u for u in updates if u[5] == IMPOSTOR_PRICE]
        self.assertEqual(impostors, [], "updates sent on the book's own stream reached this test's exchange's")
        kinds = {u[1] for u in updates}
        self.assertIn(ADD, kinds, "no order was added: no order reached the exchange")
        for seq, kind, order_id, ticker, side, price, qty, _ in updates:
            self.assertIn(kind, (ADD, 3, CANCEL, TRADE), f"update {seq}: unexpected type {kind}")
            self.assertLess(ticker, MAX_TICKERS, f"update {seq}: ticker {ticker}")
            if kind == ADD:
                self.assertIn(side, (1, -1), f"update {seq}: side {side}")
                self.assertGreater(qty, 0, f"update {seq}: an order added with quantity 0")


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
