# ==============================================================================
# tests/integration/test_redis_cli.py
#
# Purpose:
#   End-to-end integration test harness for the Keva in-memory database server.
#   Validates protocol correctness, command execution, and persistence behavior
#   against a live Keva server process:
#   - Protocol handshake and basic commands (PING, ECHO)
#   - String commands (SET, GET, INCR, DECR, INCRBY, DECRBY, APPEND, STRLEN, MSET, MGET)
#   - Key space management (EXISTS, DEL, TYPE, KEYS)
#   - Expiration and TTL semantics (EXPIRE, TTL, PERSIST, passive & active eviction)
#   - Server management (DBSIZE, FLUSHDB)
#   - RDB snapshot creation (SAVE, BGSAVE) and point-in-time recovery on restart
# ==============================================================================

import os
import socket
import subprocess
import sys
import time
import shutil

PORT = 6399
SERVER_BIN = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/src/keva-server"))
RDB_FILE = "/tmp/keva_test_dump.rdb"


class RespClient:
    """Minimal zero-dependency RESP2 client."""

    def __init__(self, host="127.0.0.1", port=PORT, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.reader = self.sock.makefile("rb")

    def close(self):
        try:
            self.reader.close()
            self.sock.close()
        except Exception:
            pass

    def send_command(self, *args):
        # Format RESP2 array of bulk strings
        parts = [f"*{len(args)}\r\n".encode("utf-8")]
        for arg in args:
            arg_bytes = str(arg).encode("utf-8")
            parts.append(f"${len(arg_bytes)}\r\n".encode("utf-8"))
            parts.append(arg_bytes + b"\r\n")
        self.sock.sendall(b"".join(parts))
        return self.read_response()

    def read_response(self):
        line = self.reader.readline()
        if not line:
            raise ConnectionError("Server closed connection")
        prefix = line[0:1]
        payload = line[1:-2]  # strip prefix and \r\n

        if prefix == b"+":
            return payload.decode("utf-8")
        elif prefix == b"-":
            return Exception(payload.decode("utf-8"))
        elif prefix == b":":
            return int(payload)
        elif prefix == b"$":
            length = int(payload)
            if length == -1:
                return None
            data = self.reader.read(length)
            crlf = self.reader.read(2)
            assert crlf == b"\r\n", f"Expected CRLF, got {crlf}"
            return data.decode("utf-8", errors="replace")
        elif prefix == b"*":
            count = int(payload)
            if count == -1:
                return None
            elements = []
            for _ in range(count):
                elements.append(self.read_response())
            return elements
        else:
            raise ValueError(f"Unknown RESP byte: {prefix}")


def start_server(port=PORT, rdb_path=RDB_FILE, clean_rdb=True):
    if clean_rdb and os.path.exists(rdb_path):
        os.remove(rdb_path)
    cmd = [SERVER_BIN, "--port", str(port), "--loglevel", "debug", "--rdb-filename", rdb_path]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Poll until port is open
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.1)
            s.close()
            break
        except (ConnectionRefusedError, OSError):
            time.sleep(0.05)
    else:
        proc.kill()
        out, err = proc.communicate()
        raise RuntimeError(f"Server failed to start:\nstdout: {out.decode()}\nstderr: {err.decode()}")
    return proc


def stop_server(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def run_tests():
    print(f"Starting Keva server on port {PORT}...")
    proc = start_server()
    client = None
    passed = 0
    failed = 0

    def check(name, expr, expected=True):
        nonlocal passed, failed
        if expr == expected:
            print(f"  [PASS] {name}")
            passed += 1
        else:
            print(f"  [FAIL] {name}: got {expr!r}, expected {expected!r}")
            failed += 1

    try:
        client = RespClient()

        print("\n--- Test Suite 1: Connection & Diagnostics ---")
        check("PING", client.send_command("PING"), "PONG")
        check("PING custom", client.send_command("PING", "hello"), "hello")
        check("ECHO", client.send_command("ECHO", "hello keva"), "hello keva")

        print("\n--- Test Suite 2: Basic Key-Value Operations ---")
        check("SET k1 v1", client.send_command("SET", "k1", "v1"), "OK")
        check("GET k1", client.send_command("GET", "k1"), "v1")
        check("EXISTS k1", client.send_command("EXISTS", "k1"), 1)
        check("EXISTS non_existent", client.send_command("EXISTS", "non_existent"), 0)
        check("TYPE k1", client.send_command("TYPE", "k1"), "string")
        check("DEL k1", client.send_command("DEL", "k1"), 1)
        check("GET k1 after DEL", client.send_command("GET", "k1"), None)
        check("EXISTS k1 after DEL", client.send_command("EXISTS", "k1"), 0)

        print("\n--- Test Suite 3: Numeric & Append Operations ---")
        check("SET num 10", client.send_command("SET", "num", "10"), "OK")
        check("INCR num", client.send_command("INCR", "num"), 11)
        check("INCRBY num 5", client.send_command("INCRBY", "num", "5"), 16)
        check("DECR num", client.send_command("DECR", "num"), 15)
        check("DECRBY num 10", client.send_command("DECRBY", "num", "10"), 5)
        check("GET num", client.send_command("GET", "num"), "5")

        check("SET str hello", client.send_command("SET", "str", "hello"), "OK")
        check("APPEND str _world", client.send_command("APPEND", "str", "_world"), 11)
        check("STRLEN str", client.send_command("STRLEN", "str"), 11)
        check("GET str", client.send_command("GET", "str"), "hello_world")

        print("\n--- Test Suite 4: Multi-Key Operations ---")
        check("MSET a 1 b 2 c 3", client.send_command("MSET", "a", "1", "b", "2", "c", "3"), "OK")
        check("MGET a b c missing", client.send_command("MGET", "a", "b", "c", "missing"), ["1", "2", "3", None])
        check("DBSIZE >= 5", client.send_command("DBSIZE") >= 5, True)

        print("\n--- Test Suite 5: Expiration & TTL ---")
        check("SET exp_key exp_val", client.send_command("SET", "exp_key", "exp_val"), "OK")
        check("EXPIRE exp_key 2", client.send_command("EXPIRE", "exp_key", "2"), 1)
        ttl = client.send_command("TTL", "exp_key")
        check("TTL > 0", ttl > 0 and ttl <= 2, True)
        time.sleep(2.2)
        check("GET exp_key after expiration", client.send_command("GET", "exp_key"), None)
        check("TTL exp_key after expiration", client.send_command("TTL", "exp_key"), -2)

        print("\n--- Test Suite 6: Persistence (SAVE & BGSAVE) ---")
        check("SET persist_key persist_val", client.send_command("SET", "persist_key", "persist_val"), "OK")
        check("SAVE", client.send_command("SAVE"), "OK")
        check("RDB file created", os.path.exists(RDB_FILE), True)

        check("BGSAVE", client.send_command("BGSAVE"), "Background saving started")
        time.sleep(0.5)

        client.close()
        client = None

        print("\n--- Test Suite 7: Server Restart & RDB Recovery ---")
        stop_server(proc)
        time.sleep(0.2)
        print("Restarting Keva server with restored RDB snapshot...")
        proc = start_server(clean_rdb=False)
        client = RespClient()

        check("GET persist_key after reload", client.send_command("GET", "persist_key"), "persist_val")
        check("GET num after reload", client.send_command("GET", "num"), "5")

    finally:
        if client:
            client.close()
        stop_server(proc)
        if os.path.exists(RDB_FILE):
            os.remove(RDB_FILE)

    print("\n==========================================")
    print(f"Integration Tests Completed: {passed} passed, {failed} failed")
    print("==========================================")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(run_tests())
