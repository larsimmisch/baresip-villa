#!/usr/bin/env python3

import json
import asyncio
import logging

mode_discard = 1
mode_pause = 2
mode_mute = 4
mode_restart = 8
m_dont_interrupt = 16
mode_loop = 32

prio_background = 2
prio_announcement = 4

class Caller(object):
	def __init__(self, transport, data):
		self.transport = transport
		self.call_id = data['id']
		self.data = data
		self.token_count = 0

	def send_command(self, command, *args):
		self.token_count += 1
		token = f'{self.call_id}-{self.token_count}'
		self.transport.send_command(command, self.call_id, *args, token=token)
		return token

	def call_accepted(self):
		self.send_command('enqueue', prio_background, mode_loop,
						  { 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/diele/dieleatm_s16.wav'})

class VillaProtocol(asyncio.Protocol):
	def __init__(self, on_con_lost):
		self.on_con_lost = on_con_lost
		self.current_message = ''
		self.callers = {}

	def send(self, command):
		cmd = (json.dumps(command) + '\r\n').encode()
		logging.info(f'command: {cmd}')
		self.transport.write(cmd)

	def send_command(self, command, *args, **kwargs):
		self.send({ 'type': command, 'command' : True,
				   'token': kwargs.get('token', None), 'params': args })

	def connection_made(self, transport):
		logging.info('connected')
		self.transport = transport
		self.send_command('listen', '<sip:villa@immisch-macbook-pro.local>')

	def call_incoming(self, data):
		call_id = data['id']
		self.callers[call_id] = Caller(self, data)
		self.send_command('answer', call_id, token=call_id)

	def response_received(self, command, token, status):
		if command == 'answer':
			caller = self.callers[token]
			caller.call_accepted()

	def data_received(self, data):
		data = self.current_message + data.decode()
		lines = data.split('\r\n')
		for l in lines[:-1]:
			logging.info(f'received: {l}')
			if l:
				jd = json.loads(l)
				if jd.get('event', None):
					# look up the method named in type (lower case)
					t = jd['type']
					if t:
						fn = getattr(self, t.lower(), None)
						if fn:
							# and call it if available
							fn(jd)
				elif jd.get('response', None):
					t = jd['type']
					if t:
						self.response_received(t, jd.get('token', None), jd.get('status', 0))

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