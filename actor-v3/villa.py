#!/usr/bin/env python3

import os
import logging
import asyncio
from location import Room, Door
from molecule import *
import protocol
import copy
import sqlite3

log = logging.getLogger('world')

# set the filesystem root for all molecules
set_root('/usr/local/share/baresip/villa/Villa')

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

class Diele(Room):
	prefix = 'diele'
	shortcut = '001'
	floor = 0
	background = Play(P_Background, 'dieleatm_s16.wav',
					  prefix=prefix)
class Flur(Room):
	prefix = 'flur'
	shortcut = '002'
	floor = 0
	background = Play(P_Background, 'fluidum_s16.wav',
					  prefix=prefix)

class Salon(Room):
	prefix = 'salon'
	shortcut = '003'
	floor = 0
	background = Play(P_Background, 'smooth1_s16.wav',
					  prefix=prefix)

class Sofa(Room):
	prefix = 'sofa'
	shortcut = '004'
	floor = 0
	background = Play(P_Background, 'welten1x_s16.wav',
					  prefix=prefix)


class Speiseraum(Room):
	prefix = 'flur'
	shortcut = '003'
	floor = 0
	background = Play(P_Background, 'welten6x_s16.wav',
					  prefix=prefix)

class Küche(Room):
	prefix = 'kueche'
	shortcut = '004'
	floor = 0
	background = Play(P_Background, 'suchbiet_s16.wav',
					  prefix=prefix)

class Kühlschrank(Room):
	prefix = 'kuehlschrank'
	shortcut = '005'
	floor = 0
	background = Play(P_Background, 'dgfltatm_s16.wav',
					  prefix=prefix)

class Bibliothek(Room):
	prefix = 'bibliothek'
	shortcut = '006'
	floor = 0
	background = Play(P_Background, 'birdy2_s16.wav',
					  prefix=prefix)

class Terasse(Room):
	prefix = 'terasse'
	shortcut = '007'
	floor = 0

class Garten(Room):
	prefix = 'garten'
	shortcut = '008'
	floor = 0
	background = Play(P_Background, 'wiese_s16.wav',
					  prefix=prefix)

class Damenklo(Room):
	prefix = 'damenklo'
	shortcut = '009'
	floor = 0
	background = Play(P_Background, 'flussmus_s16.wav',
					  prefix=prefix)

class Herrenklo(Room):
	prefix = 'damenklo'
	shortcut = '010'
	floor = 0
	background = Play(P_Background, 'rico_s16.wav',
					  prefix=prefix)

class Regal(Room):
	prefix = 'regal'
	shortcut = '011'
	floor = 0

class Kommode(Room):
	prefix = 'kommode'
	shortcut = '012'
	floor = 0


class Schreibtisch(Room):
	prefix = 'schreibtisch'
	shortcut = '011'
	help = Play(P_Discard, 'help.wav', prefix=prefix)

	def enter(self, caller):
		super(Schreibtisch, self).enter(caller)
		if caller.mailbox:
			caller.mailbox.reset()

		if caller.mailbox:
			count = len(caller.mailbox.messages)
			if count == 0:
				caller.enqueue(Play(P_Normal, 'duhastkeinepost.wav',
									prefix='lars'))
			elif count == 1:
				caller.enqueue(Play(P_Normal, 'duhasteinenachricht.wav',
									prefix='lars'))
			elif count in range(2, 9):
				caller.enqueue(Play(P_Normal, 'duhast.wav',
									'%d.wav' % (count), 'nachrichten.wav',
									prefix='lars'))
			else:
				caller.enqueue(Play(P_Normal, 'duhast.wav',
									'many.wav', 'nachrichten.wav',
									prefix='lars'))

	def browse(self, caller, dtmf):
		'''Browse through the messages'''

		if caller.mailbox is None or not caller.mailbox.messages:
			caller.enqueue(Play(P_Discard,
								'duhastkeinepost.wav', prefix='lars'))
			return True

		if dtmf == '1':
			msg = caller.mailbox.previous()
		elif dtmf == '2':
			msg = caller.mailbox.current()
		elif dtmf == '3':
			msg = caller.mailbox.next()

		if msg:
			log.debug('browsing - message: %s', msg.filename())
			caller.mailbox.play_current(caller)
		else:
			# wraparound, most likely. Beep for now.
			self.generic_invalid(caller)

		return True

	def event_dtmf_begin(self, caller, dtmf):
		if super(Schreibtisch, self).DTMF(caller, dtmf):
			return True

		if dtmf in ['1', '2', '3']:
			return self.browse(caller, dtmf)

		if dtmf == '4':
			# reply
			inmsg = caller.mailbox.current()
			if inmsg is None or inmsg.from_ is None:
				self.generic_invalid(caller)
				return True
			caller.startDialog(MailDialog(inmsg.from_))
		elif dtmf == '9':
			if caller.mailbox.delete_current(caller):
				# Play crumpling paper sound
				caller.enqueue(Play(P_Discard, 'crinkle.wav', prefix='lars'))
			else:
				self.generic_invalid(caller)

class DBData(object):
    def __init__(self, id, pin, cli, email, flags):
        self.id = id
        self.pin = pin
        self.cli = cli
        self.email = email
        self.flags = flags
class DBEntryDialog:
	max_retries = 3

	def __init__(self, caller, world):
		self.caller = caller
		self.world = world
		self.buffer = ''
		self.retries = 0

		if self.caller.details.calling:
			q = world.db.runQuery('SELECT * FROM user WHERE cli = %s;' %
								  self.caller.details.calling)

			q.addCallback(self.db_cli)
			q.addErrback(self.db_error)

	def db_cli(self, result):
		if result:
			log.debug('%s db_cli result: %s', self.caller, result[0])
			self.caller.db = DBData(*result[0])
			self.startPin()
		else:
			log.debug('%s db_cli empty result', self.caller)
			self.startId()

	def db_id(self, result):
		if result:
			log.debug('%s db_id result: %s', self.caller, result[0])
			self.caller.db = DBData(*result[0])
			self.state = 'pin'
			self.caller.enqueue(Play(P_Normal, 'prima.wav', 'pin.wav',
									 prefix='lars'))
		else:
			log.debug('%s db_id empty result', self.caller)
			self.invalid()

	def db_error(self, result):
		log.debug('%s db error: %s', self.caller, result)
		self.caller.disconnect()

	def startId(self):
		self.state = 'id'
		self.caller.enqueue(Play(P_Normal, 'id.wav', prefix='lars'))

	def startPin(self):
		self.state = 'pin'
		self.caller.enqueue(Play(P_Normal, 'pin.wav', prefix='lars'))

	def invalid(self):
		self.retries = self.retries + 1
		if self.retries > self.max_retries:
			self.state = 'rejected'
			self.caller.enqueue(Play(P_Normal, '4003_suonho_accessdenied_iLLCommunications_suonho.wav'))
		else:
			if self.state == 'id':
				self.caller.enqueue(Play(P_Normal,'id_wrong.wav',
										 prefix='lars'))
			else:
				self.caller.enqueue(Play(P_Normal, 'pin_wrong.wav',
										 prefix='lars'))
			self.startId()

	def hash(self, caller):
		log.debug('%s entry state %s', self.caller, self.state)
		if self.state == 'id':
			if self.buffer:
				q = self.world.db.runQuery(
					'SELECT * FROM user WHERE id = %s;' % self.buffer)

				q.addCallback(self.db_id)
				q.addErrback(self.db_error)
				self.state = 'waiting'
				self.buffer = ''
			else:
				self.invalid()
		else:
			if self.buffer == self.caller.db.pin:
				self.world.entry.enter(caller)
				return True

			self.invalid()

	def event_dtmf_start(self, caller, dtmf):
		if dtmf == '#':
			return self.hash(caller)
		else:
			self.buffer = self.buffer + dtmf

class EntryDialog:
	'''Ask the caller for a name'''

	def __init__(self, caller, world):
		self.caller = caller
		self.world = world
		self.cli = self.caller.details.calling

		self.tid = caller.enqueue(
			Molecule(P_Transition,
					 PlayAtom('wieheisstdu.wav', prefix='lars'),
					 RecordBeep(P_Transition, 'name.wav', 10.0,
								prefix=os.path.join('user', self.cli))))

	def event_molecule_done(self, caller, event, user_data):
		tid = event['tid']
		status = event['status']
		if self.tid_talk == tid:
			self.tid_talk = None
			log.info('talk %s (%d): status %s',  user_data, tid, status)
			for c in self.callers.itervalues():
				if c != caller:
					c.enqueue(Play(P_Discard, user_data,
							  prefix='talk'))

class Villa(object):
	def __init__(self):
		self.callers = {}
		self.shortcuts = {}
		self.db = sqlite3.connect('villa.db')

	def accept_incoming(self, details):
		return True

	def update_shortcuts(self):
		'''Update the shortcut dictionary'''
		# This is a bit crude as it ignores the class dict
		for d, v in self.__dict__.items():
			s = getattr(v, 'shortcut', None)
			if s:
				log.debug('%s has shortcut %s', d, s)
				self.shortcuts[s] = v

	def connection_made(self, transport):
		self.transport = transport

		self.diele = Diele()
		self.küche = Küche()
		self.speiseraum = Speiseraum()
		self.damenklo = Damenklo()
		self.herrenklo = Herrenklo()
		self.flur = Flur()
		self.bibliothek = Bibliothek()
		self.regal = Regal()
		self.salon = Salon()
		self.terasse = Terasse()
		self.sofa = Sofa()
		self.kommode = Kommode()

		# Erdgeschoss
		connect(self.diele, self.flur, Door(), 'north')
		connect(self.diele, self.küche, Door(), 'east')
		connect(self.diele, self.speiseraum, Door(), 'west')
		# connect(self.diele, self.aufzug, Door(), 'northeast')

		connect(self.flur, self.terasse, Door(), 'north')
		connect(self.flur, self.salon, Door(), 'east')
		connect(self.flur, self.damenklo, Door(), 'west')
		connect(self.flur, self.herrenklo, Door(), 'northwest')

		connect(self.salon, self.terasse, Door(), 'north')
		connect(self.salon, self.sofa, Door(), 'east')
		connect(self.salon, self.kommode, Door(), 'northeast')
		connect(self.salon, self.regal, Door(), 'southeast')

		self.update_shortcuts()

		transport.send_command('listen', '<sip:villa@immisch-macbook-pro.local>')

	def stop(self):
		self.callers = {}
		self.shortcuts = {}

	def enter(self, caller):
		log.debug('%s entered', caller)

		self.callers[caller.call_id] = caller

		caller.enqueue(
			Play(P_Transition,
				 '4011_suonho_sweetchoff_iLLCommunications_suonho_s16.wav'))

		# caller.startDialog(EntryDialog(caller, self))

		self.diele.enter(caller)

	def leave(self, caller):
		del self.callers[caller.call_id]

		log.debug('%s left', caller)


if __name__ == '__main__':

	villa = Villa()

	logging.basicConfig(format='%(asctime)s %(levelname)s %(message)s', level=logging.INFO)
	asyncio.run(protocol.run('127.0.0.1', 1235, villa))