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
		self.tm_move = None
		self.tm_starhash = None
		self.tm_orientation = None
		self.bf_starhash = ''
		self.it_callers = None

	def __del__(self):
		self.cancel('tm_move')
		self.cancel('tm_starhash')
		self.cancel('tm_orientation')

	def cancel(self, tm):
		'''Cancel the timer called tm. Sets the attribute tm to None'''
		t = getattr(self, tm)
		setattr(self, tm, None)
		if t:
			t.cancel()

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
			caller.user_data.tm_orientation = \
				call_later(4.0, self.orientation_timer, caller)

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
		caller.user_data.tm_orientation = None
		caller.enqueue(self.orientation)

	def move_invalid(self, caller):
		caller.enqueue(Play(P_Normal, 'there_is_nothing.wav', prefix='lars'))

	def move_timer(self, caller):
		caller.user_data.tm_move = None
		self.generic_invalid(caller)

	def starhash_invalid(self, caller):
		self.generic_invalid(caller)

	def starhash_timer(self, caller):
		caller.user_data.tm_starhash = None
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
		data.cancel('tm_orientation')
		if data.tm_move:
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
				data.cancel('tm_move')
				self.move_invalid(caller)
			if dir:
				data.cancel('tm_move')
				trans = getattr(self, dir, None)
				if trans:
					self.move(caller, trans)
				else:
					self.move_invalid(caller)

			return True
		elif data.tm_starhash:
			if dtmf == '#':
				data.cancel('tm_starhash')
				self.starhash(caller, data.bf_starhash)
			elif dtmf == '*':
				data.cancel('tm_starhash')
				self.starhash_invalid(caller)
			else:
				data.cancel('tm_starhash')
				# inter digit timer for direct access
				data.tm_starhash = call_later(3.0, self.starhash_timer)
				data.bf_starhash = data.bf_starhash + dtmf

			return True

		if dtmf == '5':
			data.tm_move = call_later(2.0, self.move_timer, caller)
			return True
		elif dtmf == '6':
			self.announce_others(caller)
			return True
		elif dtmf == '*':
			data.tm_starhash = call_later(2.0, self.starhash_timer,
										 caller)
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
		super(Room, self).__init__()
		self.talk_id = 0
		self.orientation = Play(P_Discard, 'helpmove_s16.wav', prefix=self.prefix)

	def gen_talk_id(self):
		self.talk_id = self.talk_id + 1
		return '%s_%d.wav' % (self.__class__.__name__, self.talk_id)

	def enter(self, caller):
		super(Room, self).enter(caller)
		if hasattr(self, 'background'):
			caller.enqueue(self.background)

	def leave(self, caller):
		super(Room, self).leave(caller)
		if hasattr(self, 'background'):
			caller.discard(P_Background, P_Normal)

	def starhash(self, caller, key):
		if not super(Room, self).starhash(caller, key):
			logging.debug('%s mail to: %s', caller, key)
			# caller.startDialog(mail.MailDialog(key))

	def event_dtmf_begin(self, caller, dtmf):
		if super(Room, self).event_dtmf_begin(caller, dtmf):
			return True

		data = caller.user_data
		if data.tid_talk:
			caller.stop(data.tid_talk)
		elif dtmf == '4':
			talk = self.gen_talk_id()
			data.tid_talk = caller.enqueue(
				RecordBeep(P_Normal, talk, 10.0, prefix='talk'),
				tid_data = talk)

	def MLCA(self, caller, event, user_data):
		data = caller.user_data
		tid = event['tid']
		status = event['status']
		if data.tid_talk == tid:
			data.tid_talk = None
			logging.info('talk %s (%d): status %s',  user_data, tid, status)
			for c in self.callers:
				if c != caller:
					c.enqueue(Play(P_Discard, user_data,
							  prefix='talk'))

class Transition(object):
	def __init__(self, m_trans, m_in, m_out):
		self.m_trans = m_trans
		self.m_in = m_in
		self.m_out = m_out

class Door(Transition):

	def __init__(self, open='location/tuer_s16.wav',
			close='location/tuer_s16.wav'):

		super(Door, self).__init__(
			Play(P_Transition, open, close),
			Play(P_Transition, open),
			Play(P_Transition, close))

class Stairs(Transition):
	def __init__(self):
		super(Stairs, self).__init__(
			Play(P_Transition, 'location/treppe_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'))

class Teleporter(Transition):
	def __init__(self, dest):
		super(Teleporter, self).__init__(
			Play(P_Transition, 'location/beamer1_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'),
			Play(P_Transition, 'location/tuer_s16.wav'))
		self.dest = dest

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