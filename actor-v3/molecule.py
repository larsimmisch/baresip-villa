#!/usr/bin/env python3

import os

_root = ''

def set_root(root):
	global _root
	_root = root

def get_root():
	global _root
	return _root

def exists(prefix, name):
	global _root
	p = os.path.join(_root, prefix or '', name)
	return os.path.exists(p)
class Atom(object):
	def __init__(self):
		"""notify should be 'none', 'begin', 'end' or 'both'"""
		self.notify = 'none'

class PlayAtom(Atom):
	def __init__(self, filename, prefix = None):
		Atom.__init__(self)
		if prefix:
			self.filename = os.path.join(_root, prefix, filename)
		else:
			self.filename = os.path.join(_root, filename)

	def as_json(self):
		return {
			'type': 'play',
			'filename': self.filename
		}

class RecordAtom(Atom):
	def __init__(self, filename, maxtime, maxsilence=2.0, dtmf_stop=True, prefix = None):
		Atom.__init__(self)
		if prefix:
			self.filename = os.path.join(_root, prefix, filename)
		else:
			self.filename = os.path.join(_root, filename)
		self.maxtime = maxtime
		self.maxsilence = maxsilence
		self.dtmf_stop = dtmf_stop

	def as_json(self):
		return {
			'type': 'record',
			'filename': self.filename,
			'max_silence': self.maxsilence * 1000,
			'max_length': self.maxtime * 1000,
			'dtmf_stop': self.dtmf_stop
		}

class BeepAtom(PlayAtom):
	def __init__(self, count, prefix=None):
		PlayAtom.__init__(self, 'beep_s16.wav')
		self.count = count

class ConferenceAtom(Atom):
	def __init__(self, conference, mode):
		Atom.__init__(self)
		self.conference = conference
		if type(mode) != type('') or not mode in ['listen', 'speak', 'duplex']:
			raise TypeError("mode must be one of 'listen', 'speak' or " \
							"'duplex'")
		self.mode = mode

	def as_json(self):
		return {
			'type': 'conf',
			'handle': self.conference,
			'mode': self.mode
		}

class Molecule(list):
	def __init__(self, policy, *atoms):
		self.policy = policy
		for a in atoms:
			self.append(a)

	def __setitem__(self, key, item):
		if not isinstance(item, Atom):
			raise TypeError('%s must be a subclass of Atom' % repr(item))
		super(Molecule, self).__setitem__(self, key, item)

	def as_args(self):
		"""Generate the molecule as arguments for enqueue."""
		args = [self.policy.priority, self.policy.mode]

		for i in self:
			args.append(i.as_json())

		return args

class Play(Molecule):
	def __init__(self, policy, *args, **kwargs):
		self.policy = policy
		prefix = kwargs.get('prefix', None)
		for a in args:
			self.append(PlayAtom(a, prefix))

class Beep(Molecule):
	def __init__(self, policy, count):
		self.policy = policy
		self.append(BeepAtom(count))

class Record(Molecule):
	def __init__(self, policy, filename, maxtime, maxsilence = 2.0,
				 prefix = None):
		self.policy = policy
		self.append(RecordAtom(filename, maxtime, maxsilence, prefix))

class RecordBeep(Molecule):
	def __init__(self, policy, filename, maxtime, maxsilence = 2.0,
				 prefix = None):
		self.policy = policy
		self.append(BeepAtom(1))
		self.append(RecordAtom(filename, maxtime, maxsilence, prefix))

class Conference(Molecule):
	def __init__(self, policy, conference, mode):
		self.policy = policy
		self.append(ConferenceAtom(conference, mode))

# mode definitions
mode_discard = 1
mode_pause = 2
mode_mute = 4
mode_restart = 8
mode_dont_interrupt = 16
mode_loop = 32
mode_dtmf_stop = 64

# priority definitions
pr_background = 0
pr_normal = 1
pr_mail = 2
pr_transition = 3
pr_urgent = 4

class Policy(object):
	def __init__(self, priority, mode):
		self.priority = priority
		self.mode = mode

# define application specific policies
P_Background = Policy(pr_background, mode_mute|mode_loop)
P_Normal = Policy(pr_normal, mode_mute)
P_Discard = Policy(pr_normal, mode_discard|mode_dtmf_stop)
P_Mail = Policy(pr_mail, mode_discard|mode_dtmf_stop)
P_Transition = Policy(pr_transition, mode_dont_interrupt|mode_dtmf_stop)
P_Urgent = Policy(pr_urgent, mode_dont_interrupt)
