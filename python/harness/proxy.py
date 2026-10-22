"""TCP proxies for directed peer links, controllable while the cluster runs.

Every peer link i -> j goes through its own proxy. Node i dials the proxy instead of node j,
and replies from j to i travel over j's own link j -> i. Blocking one proxy therefore cuts one
direction only; blocking both proxies of a pair partitions the two nodes. No root privileges or
firewall rules are needed.
"""

from __future__ import annotations

import asyncio
import contextlib
import threading
from dataclasses import dataclass, field


@dataclass
class LinkState:
    blocked: bool = False
    delay_s: float = 0.0
    connections: set[asyncio.StreamWriter] = field(default_factory=set)


class ProxyHub:
    """Owns one event loop thread and any number of directed link proxies."""

    def __init__(self) -> None:
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thread.start()
        self._links: dict[tuple[int, int], LinkState] = {}
        self._servers: list[asyncio.base_events.Server] = []

    def _call(self, coroutine):
        return asyncio.run_coroutine_threadsafe(coroutine, self._loop).result(timeout=10)

    def add_link(self, link: tuple[int, int], listen_port: int, target_port: int) -> None:
        state = LinkState()
        self._links[link] = state

        async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            if state.blocked:
                writer.close()
                return
            try:
                upstream_reader, upstream_writer = await asyncio.open_connection(
                    "127.0.0.1", target_port
                )
            except OSError:
                writer.close()
                return
            state.connections.update({writer, upstream_writer})
            await asyncio.gather(
                self._pump(state, reader, upstream_writer),
                self._pump(state, upstream_reader, writer),
                return_exceptions=True,
            )
            for stream in (writer, upstream_writer):
                state.connections.discard(stream)
                stream.close()

        async def start() -> None:
            server = await asyncio.start_server(handle, "127.0.0.1", listen_port)
            self._servers.append(server)

        self._call(start())

    @staticmethod
    async def _pump(
        state: LinkState, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        while True:
            data = await reader.read(65536)
            if not data or state.blocked:
                break
            if state.delay_s > 0:
                # Order is preserved: one pump forwards one stream sequentially.
                await asyncio.sleep(state.delay_s)
            if state.blocked:
                break
            writer.write(data)
            await writer.drain()
        writer.close()

    def set_blocked(self, link: tuple[int, int], blocked: bool) -> None:
        state = self._links[link]

        async def apply() -> None:
            state.blocked = blocked
            if blocked:
                # Existing streams are cut, as a partition would cut them.
                for stream in list(state.connections):
                    stream.close()

        self._call(apply())

    def set_delay(self, link: tuple[int, int], delay_s: float) -> None:
        self._links[link].delay_s = delay_s

    def heal(self) -> None:
        for link in self._links:
            self.set_blocked(link, False)
            self.set_delay(link, 0.0)

    def links(self) -> list[tuple[int, int]]:
        return list(self._links)

    def close(self) -> None:
        async def stop() -> None:
            for server in self._servers:
                server.close()
            for state in self._links.values():
                for stream in list(state.connections):
                    stream.close()

        with contextlib.suppress(Exception):
            self._call(stop())
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(timeout=5)
