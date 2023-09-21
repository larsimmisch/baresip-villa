#!/usr/bin/env python3

import asyncio

class VillaProtocol(asyncio.Protocol):
    def __init__(self, on_con_lost):
        self.on_con_lost = on_con_lost
        self.current_message = ''

    def connection_made(self, transport):
        print('connected')

    def data_received(self, data):
        data = self.current_message + data.decode()
        lines = data.split('\r\n')
        for l in lines[:-1]:
            print(f'Message received: {l}')

        self.current_message += lines[-1]

    def connection_lost(self, exc):
        print('The server closed the connection')
        self.on_con_lost.set_result(True)


async def main():
    # Get a reference to the event loop as we plan to use
    # low-level APIs.
    loop = asyncio.get_running_loop()

    on_con_lost = loop.create_future()

    transport, protocol = await loop.create_connection(
        lambda: VillaProtocol(on_con_lost),
        '127.0.0.1', 1235)

    # Wait until the protocol signals that the connection
    # is lost and close the transport.
    try:
        await on_con_lost
    finally:
        transport.close()


asyncio.run(main())