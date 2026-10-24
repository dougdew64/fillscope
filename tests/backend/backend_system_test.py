# The back end (backend/, the fork of the trading system in Ghosh's book) as Fillscope builds it: the
# exchange and a client run together, orders reach the exchange, and market data leaves it. Checked
# from outside, as the viewer will see it: this test joins the exchange's incremental market data
# feed and decodes what arrives.
#
#   python3 -I tests/backend/backend_system_test.py EXCHANGE_MAIN TRADING_MAIN
#
# The exchange's addresses are fixed in its code (exchange_main.cpp): orders over TCP on port 12345,
# incremental updates on UDP multicast 233.252.14.3:20001, sent on the interface the system routes
# the group to and looped back to this machine. So only one exchange can run on the machine at a
# time, and the test says so if another is running.
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ORDER_PORT = 12345
INCREMENTAL_GROUP, INCREMENTAL_PORT = "233.252.14.3", 20001
# MDPMarketUpdate, packed (market_update.h): seq_num_ (size_t), then MEMarketUpdate: type_ (uint8),
# order_id_ (uint64), ticker_id_ (uint32), side_ (int8), price_ (int64), qty_ (uint32),
# priority_ (uint64). Little-endian, no padding.
UPDATE = struct.Struct("<QBQIbqIQ")
ADD, CANCEL, TRADE = 2, 4, 5
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
# The exchange starts its parts a couple of seconds apart, so it takes about 10 s to open its port;
# the client takes about 15 s more to start and send its first order (orders then follow every
# 20 ms). The limits allow for the sanitizer build, which is slower.
EXCHANGE_LIMIT = 30.0
FIRST_UPDATES_LIMIT = 60.0
UPDATES_WANTED = 200

EXCHANGE, TRADING = (os.path.abspath(path) for path in sys.argv[1:3])  # they run in another directory


def order_port_open():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        return probe.connect_ex(("127.0.0.1", ORDER_PORT)) == 0


def incremental_feed():
    """A socket that receives the incremental feed, joined as the book's own consumer joins it
    (socket_utils.h): bound to the port on any address, a member of the group on any interface."""
    feed = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    feed.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    feed.bind(("", INCREMENTAL_PORT))
    membership = struct.pack("4s4s", socket.inet_aton(INCREMENTAL_GROUP), socket.inet_aton("0.0.0.0"))
    feed.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    return feed


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
                         ["--record", recording, "--snapshot-interval-seconds"]):
                result = subprocess.run([EXCHANGE, *args], cwd=directory, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2, args)
                self.assertIn("usage: exchange_main", result.stderr, args)
            with open(recording) as file:
                self.assertEqual(file.read(), "an earlier run", "a refused command line left the recording alone")


def start_exchange(test, args, directory):
    """The exchange, started in `directory`, once its order port is open."""
    test.assertFalse(order_port_open(), f"something is already listening on port {ORDER_PORT}: is an exchange running?")
    exchange = Process([EXCHANGE, *args], directory)
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
        self.assertFalse(order_port_open(), f"something is already listening on port {ORDER_PORT}: is an exchange running?")
        with tempfile.TemporaryDirectory() as directory, incremental_feed() as feed:
            exchange = Process([EXCHANGE], directory)
            client = None
            try:
                deadline = time.monotonic() + EXCHANGE_LIMIT
                while not order_port_open():
                    self.assertTrue(exchange.running(), f"the exchange exited with code {exchange.process.returncode}")
                    self.assertLess(time.monotonic(), deadline, f"the exchange didn't open port {ORDER_PORT}")
                    time.sleep(0.1)
                client = Process([TRADING, "5", "RANDOM"], directory)

                updates = []
                feed.settimeout(1.0)
                deadline = time.monotonic() + FIRST_UPDATES_LIMIT
                while len(updates) < UPDATES_WANTED:
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

        # The feed's sequence numbers count from 1 with no gap: the test saw every update, in order.
        self.assertEqual([u[0] for u in updates], list(range(1, len(updates) + 1)))
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
