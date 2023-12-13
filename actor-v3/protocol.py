#!/usr/bin/env python3

import os
import json
import asyncio
import logging
from molecule import pr_background, pr_normal, mode_loop, mode_mute, mode_discard

def call_later(delay, callback, *args, context=None):
	loop = asyncio.get_event_loop()
	return loop.call_later(delay, callback, *args, context=context)

class Caller(object):
	def __init__(self, transport, world, data):
		self.transport = transport
		self.world = world
		self.call_id = data['id']
		self.data = data
		self.token_count = 0
		self.dialog = None
		self.location = None

	def send_command(self, command, *args):
		self.token_count += 1
		token = f'{self.call_id}-{self.token_count}'
		self.transport.send_command(command, self.call_id, *args, token=token)
		return token

	def enqueue(self, molecule):
		args = molecule.as_args()
		return self.send_command('enqueue', *args)

	def discard(self, prio_from, prio_to):
		return self.send_command('discard', prio_from, prio_to)

	def call_accepted(self):
		self.world.enter(self)

	def event_dtmf_begin(self, data):
		dtmf = data['key']
		logging.info(f'{self.call_id} received DTMF {dtmf}')

		if self.dialog:
			if self.dialog.event_dtmf_begin(self, dtmf):
				self.dialog = None
		elif self.location:
			self.location.event_dtmf_begin(self, dtmf)

	def call_closed(self, data):
		if self.location:
			self.location.leave()
		self.world.leave(self)

class VillaProtocol(asyncio.Protocol):
	def __init__(self, on_con_lost, world, caller_class=Caller):
		self.on_con_lost = on_con_lost
		self.caller_class = caller_class
		self.world = world
		self.current_message = ''
		self.callers = {}
		self.call_data = {}

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
		self.world.connection_made(self)

	def event_call_incoming(self, data):
		call_id = data['id']
		self.call_data[call_id] = data
		if self.world.accept_incoming(data):
			self.send_command('answer', call_id, token=call_id)
		else:
			self.send_command('hangup', call_id, token=call_id)

	def event_call_closed(self, data):
		call_id = data['id']
		caller = self.callers[call_id]
		caller.event_hangup(data)
		self.world.left(caller)
		del self.callers[call_id]

	def response_received(self, command, token, result):
		if command == 'answer':
			if result == 0:
				call_data = self.call_data[token]
				caller = self.caller_class(self, self.world, call_data)
				del self.call_data[token]
				self.callers[token] = caller
				caller.call_accepted()
			else:
				logging.warning(f'{token} answer failed: {os.strerror(result)}')

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
					if t == 'version':
						logging.info(f'protocol version {jd["protocol_version"]}')
						return
					if t:
						t = t.lower()
						cid = jd.get('id')
						if t == 'call_incoming':
							destination = self
						else:
							destination = self.callers.get(cid, None)
							if destination is None:
								logging.warning(f'no call found for event {t} and id {cid}')
								return

						# look up the event handler and call it
						fn = getattr(destination, 'event_' + t, None)
						if fn:
							fn(jd)
				elif jd.get('response', None):
					t = jd['type']
					if t:
						self.response_received(t, jd.get('token', None), jd.get('result', 0))

		self.current_message += lines[-1]

	def connection_lost(self, exc):
		logging.info('The server closed the connection')
		self.on_con_lost.set_result(True)


async def run(server, port, world, caller_class=Caller):
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
				lambda: VillaProtocol(on_con_lost, world, caller_class),
				server, port)

			timeout = 0.125
			await on_con_lost

		except ConnectionRefusedError as e:
			if transport:
				transport.close()
			logging.info(f'connection refused, retrying after {timeout}s')
			await asyncio.sleep(timeout)
			if timeout < 2.0:
				timeout = timeout * 2


if __name__ == '__main__':

	class TestWorld(object):

		def connection_made(self, transport):
			transport.send_command('listen', '<sip:villa@immisch-macbook-pro.local>')

		def accept_incoming(self, details):
			return True

		def enter(self, caller):
			pass

	class TestCaller(Caller):
		def call_accepted(self):
			self.send_command('enqueue', pr_background, mode_loop | mode_mute,
		 				{ 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/pipiszimmer/party_s16.wav'})

		def event_dtmf_begin(self, data):
			dtmf = data['key']
			logging.info(f'{self.call_id} received DTMF {dtmf}')
			if dtmf == '1':
				self.send_command('enqueue', pr_normal, mode_discard, "17",
								{ 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/beep_s16.wav'},
								{ 'type': 'record', 'filename': 'record.wav', 'max_silence': 2500, 'dtmf_stop': True })
			elif dtmf == '2':
				self.send_command('enqueue', pr_normal, mode_discard,
								{ 'type': 'play', 'filename': 'record.wav' })
			elif dtmf == '#':
				self.send_command('enqueue', pr_normal, mode_discard,
								{ 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/help_s16.wav', 'max_silence': 2000 })

		def event_molecule_done(self, data):
			if data['token'] == '17':
				self.send_command('enqueue', pr_normal, mode_discard,
								{ 'type': 'play', 'filename': 'record.wav' })



	logging.basicConfig(format='%(asctime)s %(levelname)s %(message)s', level=logging.INFO)
	asyncio.run(run('127.0.0.1', 1235, TestWorld(), TestCaller))