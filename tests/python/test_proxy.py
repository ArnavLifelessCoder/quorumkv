import os
import pathlib
import socket
import sys
import threading
import time

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "python"))
from harness.cluster import free_ports
from harness.proxy import ProxyHub


@pytest.fixture
def echo_port():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    port = listener.getsockname()[1]

    def serve():
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            threading.Thread(target=_echo, args=(connection,), daemon=True).start()

    threading.Thread(target=serve, daemon=True).start()
    yield port
    listener.close()


def _echo(connection: socket.socket) -> None:
    with connection:
        while data := connection.recv(4096):
            connection.sendall(data)


def roundtrip(port: int, payload: bytes, timeout: float = 2.0) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as stream:
        stream.sendall(payload)
        received = b""
        while len(received) < len(payload):
            chunk = stream.recv(4096)
            if not chunk:
                break
            received += chunk
        return received


@pytest.fixture
def hub():
    proxies = ProxyHub()
    yield proxies
    proxies.close()


def test_link_forwards_blocks_and_heals(hub, echo_port):
    (forward,) = free_ports(1)
    # The echo reply travels back over the same proxied TCP stream in this test.
    hub.add_link((1, 2), forward, echo_port)
    assert roundtrip(forward, b"hello") == b"hello"
    hub.set_blocked((1, 2), True)
    with pytest.raises(OSError):
        if roundtrip(forward, b"x", timeout=0.5) != b"x":
            raise ConnectionResetError
    hub.heal()
    assert roundtrip(forward, b"again") == b"again"


def test_blocking_cuts_an_established_stream(hub, echo_port):
    (port,) = free_ports(1)
    hub.add_link((1, 2), port, echo_port)
    with socket.create_connection(("127.0.0.1", port), timeout=1) as stream:
        stream.sendall(b"a")
        assert stream.recv(1) == b"a"
        hub.set_blocked((1, 2), True)
        time.sleep(0.1)
        with pytest.raises(OSError):
            stream.sendall(b"b")
            if not stream.recv(1):
                raise ConnectionResetError


def test_delay_slows_delivery(hub, echo_port):
    (port,) = free_ports(1)
    hub.add_link((1, 2), port, echo_port)
    hub.set_delay((1, 2), 0.2)
    started = time.monotonic()
    assert roundtrip(port, b"slow") == b"slow"
    # Both directions of the stream pass through the delayed link.
    assert time.monotonic() - started >= 0.35


def _binaries():
    root = os.environ.get("QUORUMKV_BIN")
    if not root:
        return None
    suffix = ".exe" if sys.platform == "win32" else ""
    server = pathlib.Path(root) / f"quorumkv_server{suffix}"
    client = pathlib.Path(root) / f"quorumkv_client{suffix}"
    return (server, client) if server.exists() and client.exists() else None


@pytest.mark.skipif(_binaries() is None, reason="set QUORUMKV_BIN to a build with the binaries")
def test_short_nemesis_run_is_linearizable(tmp_path):
    import argparse

    from harness.run import run

    server, client = _binaries()
    summary = run(
        argparse.Namespace(
            server=str(server),
            client=str(client),
            seed=3,
            duration=15.0,
            recovery=5.0,
            nodes=3,
            clients=3,
            keys=3,
            unsafe_reads=False,
            out=str(tmp_path / "run"),
        )
    )
    assert summary["operations"] > 100
    assert summary["linearizable"] is True
