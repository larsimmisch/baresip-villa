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
	def __init__(self, transport, data):
		self.transport = transport
		self.call_id = data['id']
		self.data = data
		self.token_count = 0

	@staticmethod
	def accept_incoming(details):
		return True

	def send_command(self, command, *args):
		self.token_count += 1
		token = f'{self.call_id}-{self.token_count}'
		self.transport.send_command(command, self.call_id, *args, token=token)
		return token

	def event_dtmf_begin(self, data):
		dtmf = data['key']
		logging.info(f'{self.call_id} received DTMF {dtmf}')
		if dtmf == '1':
			self.send_command('enqueue', pr_normal, mode_discard, "17",
							{ 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/beep_s16.wav'},
							{ 'type': 'record', 'filename': 'record.wav', 'max_silence': 1000, 'dtmf_stop': True })
		elif dtmf == '2':
			self.send_command('enqueue', pr_normal, mode_discard, "17",
							{ 'type': 'play', 'filename': 'record.wav' })
		elif dtmf == '#':
			self.send_command('enqueue', pr_normal, mode_discard,
							{ 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/help_s16.wav', 'max_silence': 2000 })


	def enqueue(self, molecule):
		args = molecule.as_args()
		self.send_command('enqueue', *args)

	def call_accepted(self):
		self.send_command('enqueue', pr_background, mode_loop | mode_mute,
		 				{ 'type': 'play', 'filename': '/usr/local/share/baresip/villa/Villa/pipiszimmer/party_s16.wav'})

class VillaProtocol(asyncio.Protocol):
	def __init__(self, on_con_lost, caller_class=Caller):
		self.on_con_lost = on_con_lost
		self.caller_class = caller_class
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
		self.send_command('listen', '<sip:villa@immisch-macbook-pro.local>')

	def event_call_incoming(self, data):
		call_id = data['id']
		self.call_data[call_id] = data
		if self.caller_class.accept_incoming(data):
			self.send_command('answer', call_id, token=call_id)
		else:
			self.send_command('hangup', call_id, token=call_id)

	def event_call_closed(self, data):
		call_id = data['id']
		self.callers[call_id].event_hangup(data)
		del self.callers[call_id]

	def response_received(self, command, token, result):
		if command == 'answer':
			if result == 0:
				call_data = self.call_data[token]
				caller = self.caller_class(self, call_data)
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

class CallerIterator(object):
	"""A specialised iterator for caller lists.
	It is cyclic and supports deletion of items in the sequence while the
	sequence is iterated over."""

	def __init__(self, items):
		self.items = items
		self.pos = None
		self.invalid = False

	def next(self):
		if not len(self.items):
			return None

		if self.pos is None:
			self.pos = 0
		elif self.invalid:
			self.invalid = False
			self.pos = self.pos % (len(self.items) - 1)
		else:
			self.pos = (self.pos + 1) % len(self.items)

		return self.items[self.pos]

	def prev(self):
		if not len(self.items):
			return None

		if self.pos is None:
			self.pos = len(self.items) - 1
		else:
			self.pos = (self.pos - 1) % len(self.items)

		return self.items[self.pos]

	def invalidate(self, item):
		"""Indicate that an item in the sequence we iterate over will be
		deleted soon. This method gives the iterator the chance to adjust
		internal position counters.

		Note that the item must still be in the sequence when this method
		is called - the item should be deleted afterwards."""
		i = self.items.index(item)

		if i < self.pos:
			self.pos = self.pos - 1
		elif i == self.pos:
			self.invalid = True


async def run(server, port):
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
	logging.basicConfig(format='%(asctime)s %(levelname)s %(message)s', level=logging.INFO)
	asyncio.run(run('127.0.0.1', 1235))