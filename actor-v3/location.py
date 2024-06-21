import logging
import os
from molecule import *
from protocol import call_later
import copy

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

class LocationData(object):
	'''Per caller data for each location.'''
	def __init__(self):
		self.tid_talk = None
		self.bf_starhash = ''
		self.it_callers = None
		self.timers = {}

	def __del__(self):
		for tm in self.timers.values():
			tm.cancel()

	def wrap_callback(self, id: str, callback: callable, *args):
		callback(*args)
		del self.timers[id]

	def call_later(self, id: str, delay: float, callback: callable, *args):
		self.timers[id] = call_later(delay, self.wrap_callback, self, id, callback, *args)

	def cancel(self, timer_id: str):
		'''Cancel the timer called tm. Sets the attribute tm to None'''
		t = self.timers.get(timer_id, None)
		if t:
			t.cancel()
			del self.timers[timer_id]

class Transition(object):
	def __init__(self, m_trans, m_in, m_out):
		self.m_trans = m_trans
		self.m_in = m_in
		self.m_out = m_out

class Door(Transition):

	def __init__(self, f_trans='location/tuer_s16.wav',
			f_in='location/tuer_s16.wav', f_out='location/tuer_s16.wav'):

		super().__init__(
			Play(P_Transition, f_trans),
			Play(P_Transition, f_in),
			Play(P_Transition, f_out))

class Stairs(Transition):
	def __init__(self):
		super().__init__(
			Play(P_Transition, 'location/treppe_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'))

class Teleporter(Transition):
	def __init__(self):
		super().__init__(
			Play(P_Transition, 'location/beamer1_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'))

class Location(object):

	def __init__(self):
		self.callers = []
		if exists(self.prefix, 'intro_s16.wav'):
			self.orientation = Play(P_Normal, 'intro_s16.wav', self.prefix)

	def user_data(self):
		return LocationData()

	def enter(self, caller):
		'''Enter the location'''
		caller.location = self
		caller.user_data = self.user_data()
		if hasattr(self, 'orientation'):
			caller.user_data.call_later('orientation', 4.0, self.orientation_timer, caller)

		logging.debug('%s enter: %s', caller, self.__class__.__name__)

		self.callers.append(caller)

	def leave(self, caller):
		'''Leave the location. Adjust caller list iterators
		in all participants.'''

		for c in self.callers:
			it = c.user_data.it_callers
			if it:
				it.invalidate(caller)

		self.callers.remove(caller)

		logging.debug('%s left: %s', caller, self.__class__.__name__)

		d = caller.user_data
		caller.user_data = None
		del d
		caller.location = None

	def move(self, caller, transition):
		# transition sound
		caller.enqueue(transition.m_trans)

		# play door out sound to other callers
		for c in self.callers:
			if c != caller:
				c.enqueue(transition.m_out)

		self.leave(caller)
		transition.dest.enter(caller)

		# play door in sound to other callers
		for c in transition.dest.callers:
			if c != caller:
				c.enqueue(transition.m_in)

	def generic_invalid(self, caller):
		caller.enqueue(Beep(P_Normal, 1))

	def orientation_timer(self, caller):
		caller.enqueue(self.orientation)

	def move_invalid(self, caller):
		caller.enqueue(Play(P_Normal, 'there_is_nothing.wav', prefix='lars'))

	def move_timer(self, caller):
		self.generic_invalid(caller)

	def starhash_invalid(self, caller):
		self.generic_invalid(caller)

	def starhash_timer(self, caller):
		self.generic_invalid(caller)

	def starhash(self, caller, key):
		try:
			d = caller.world.shortcuts[key]
		except AttributeError:
			logging.info('%s no shortcut for %s', caller, key, exc_info=1)
			return False
		except KeyError:
			logging.info('%s no shortcut for %s', caller, key, exc_info=1)
			return False

		self.move(caller, Teleporter(d))
		return True

	def announce_others(self, caller):
		count = len(self.callers) - 1
		if count == 0:
			caller.enqueue(Play(P_Normal, 'you_are_alone.wav', prefix='lars'))
		elif count == 1:
			caller.enqueue(Play(P_Normal, 'here_is_one_other.wav',
								prefix='lars'))
		elif count in range(2, 9):
			caller.enqueue(Play(P_Normal, 'here_are.wav',
								'%d.wav' % (count), 'people.wav',
								prefix='lars'))
		else:
			caller.enqueue(Play(P_Normal, 'here_are.wav', 'many.wav',
								'people.wav', prefix='lars'))

	def event_dtmf_begin(self, caller, dtmf):
		data = caller.user_data
		data.cancel('orientation')
		if data.timers.get('move', None):
			dir = None
			if dtmf == '1':
				dir = 'northwest'
			elif dtmf == '2':
				dir = 'north'
			elif dtmf == '3':
				dir = 'northeast'
			elif dtmf == '4':
				dir = 'west'
			elif dtmf == '6':
				dir = 'east'
			elif dtmf == '7':
				dir = 'southwest'
			elif dtmf == '8':
				dir = 'south'
			elif dtmf == '9':
				dir = 'southeast'
			else:
				data.cancel('move')
				self.move_invalid(caller)
			if dir:
				data.cancel('move')
				trans = getattr(self, dir, None)
				if trans:
					self.move(caller, trans)
				else:
					self.move_invalid(caller)

			return True
		elif data.timers.get('starhash', None):
			if dtmf == '#':
				data.cancel('starhash')
				self.starhash(caller, data.bf_starhash)
			elif dtmf == '*':
				data.cancel('starhash')
				self.starhash_invalid(caller)
			else:
				# inter digit timer for direct access
				data.call_later('starhash', 3.0, self.starhash_timer, self, caller)
				data.bf_starhash = data.bf_starhash + dtmf

			return True

		if dtmf == '5':
			data.call_later('move', 2.0, self.move_timer, self, caller)
			return True
		elif dtmf == '6':
			self.announce_others(caller)
			return True
		elif dtmf == '*':
			data.call_later('starhash', 2.0, self.starhash_timer, self, caller)
			return True
		elif dtmf == '#':
			if hasattr(self, 'help'):
				caller.enqueue(self.help)
			elif hasattr(self, 'orientation'):
				caller.enqueue(self.orientation)
			return True

		return False

class Room(Location):
	def __init__(self):
		super().__init__()
		self.talk_id = 0
		self.orientation = Play(P_Discard, 'helpmove_s16.wav', prefix=self.prefix)

	def gen_talk_id(self):
		self.talk_id = self.talk_id + 1
		return '%s_%d.wav' % (self.__class__.__name__, self.talk_id)

	def enter(self, caller):
		super().enter(caller)
		if hasattr(self, 'background'):
			caller.enqueue(self.background)

	def leave(self, caller):
		super().leave(caller)
		if hasattr(self, 'background'):
			caller.discard_range(P_Background, P_Normal)

	def starhash(self, caller, key):
		if not super().starhash(caller, key):
			logging.debug('%s mail to: %s', caller, key)
			# caller.startDialog(mail.MailDialog(key))

	def event_dtmf_begin(self, caller, dtmf):
		if super().event_dtmf_begin(caller, dtmf):
			return True

		data = caller.user_data
		if dtmf == '4':
			if data.tid_talk:
				caller.stop(data.tid_talk)
			else:
				data.tid_talk = self.gen_talk_id()
				caller.enqueue(
					RecordBeep(P_Normal, data.tid_talk, 10.0, prefix=os.path.join(self.prefix or '', 'talk')),
					token=data.tid_talk)

	def event_molecule_done(self, caller, event):
		data = caller.user_data
		token = event['token']
		status = event['status']
		if data.tid_talk == token:
			data.tid_talk = None
			logging.info('talk %s: status %s',  token, status)
			for c in self.callers:
				if c != caller:
					c.enqueue(Play(P_Discard, token,
							  prefix=os.path.join(self.prefix or '', 'talk')))


_mirror = { 'north': 'south',
			'northeast': 'southwest',
			'east': 'west',
			'southeast': 'northwest',
			'south': 'north',
			'southwest': 'northeast',
			'west': 'east',
			'northwest': 'southeast' }

def connect(source, dest, transition, direction, reverse = None):
	k = _mirror.keys()
	if not direction in k:
		raise ValueError('direction must be in %s', k)

	if reverse is None:
		reverse = _mirror[direction]

	transition.source = source
	transition.dest = dest
	setattr(source, direction, transition)
	t = copy.copy(transition)
	t.source = dest
	t.dest = source
	setattr(dest, reverse, t)