#!/usr/bin/env python3

import json
import asyncio
import logging

class VillaProtocol(asyncio.Protocol):
    def __init__(self, on_con_lost):
        self.on_con_lost = on_con_lost
        self.current_message = ''

    def send(self, command):
        cmd = (json.dumps(command) + '\r\n').encode()
        logging.info(f'command: {cmd}')
        self.transport.write(cmd)

    def connection_made(self, transport):
        logging.info('connected')
        self.transport = transport
        self.send({ 'type': 'listen', 'command' : True,
                   'params': ['<sip:villa@immisch-macbook-pro.local>'] })

    def call_incoming(self, data):
        call_id = data['id']
        self.send({ 'type': 'answer', 'command': True, 'params': [call_id] })

    def response_received(self, command, token, status):
        if command == 'answer':
            pass

    def data_received(self, data):
        data = self.current_message + data.decode()
        lines = data.split('\r\n')
        for l in lines[:-1]:
            logging.info(f'received: {l}')

            jd = json.loads(l)
            if jd.get('event', None):
                # look up the method named in type (lower case)
                t = jd['type']
                if t:
                    fn = getattr(self, t.lower(), None)
                    if fn:
                        # and call it if available
                        fn(jd)

        self.current_message += lines[-1]

    def connection_lost(self, exc):
        logging.info('The server closed the connection')
        self.on_con_lost.set_result(True)


async def main():
    # Get a reference to the event loop as we plan to use
    # low-level APIs.
    loop = asyncio.get_running_loop()

    # Wait until the protocol signals that the connection
    # is lost and close the transport.
    timeout = 0.125
    while True:
        transport = None
        try:
            on_con_lost = loop.create_future()

            transport, protocol = await loop.create_connection(
                lambda: VillaProtocol(on_con_lost),
                '127.0.0.1', 1235)

            timeout = 0.125
            await on_con_lost

        except ConnectionRefusedError as e:
            if transport:
                transport.close()
            logging.info(f'connection refused, retrying after {timeout}')
            await asyncio.sleep(timeout)
            if timeout < 2.0:
                timeout = timeout * 2


logging.basicConfig(format='%(asctime)s %(levelname)s %(message)s', level=logging.INFO)
asyncio.run(main())