from enum import Enum
from location import Room
from protocol import call_later

class Direction(Enum):
	NONE = 0
	UP = 1
	DOWN = 2

class Elevator(Room):

	def __init__(self):
		self.floor = 0
		self.direction = Direction.NONE
		self.stops = []
		self.tm_moving = None
		self.tm_stopping = None

	def next_direction(self):
		if self.direction == Direction.NONE and len(self.stops):
			return Direction.UP if self.stops[0] > self.floor else Direction.DOWN

	def start(self):
		if self.direction == Direction.NONE and len(self.stops):
			self.direction = Direction.UP if self.stops[0] > self.floor else Direction.DOWN
			self.timer = call_later(abs(self.floor - self.stops[0]), self.arrived, self)

	def call(self, floor):
		added = False
		for i, st in enumerate(self.stops):
			if floor == st:
				added = True
				break

			# we might be stopped
			direction = self.direction if self.direction != Direction.NONE else self.next_direction()

			if direction == Direction.UP and floor > self.floor and floor < st \
				or direction == Direction.DOWN and floor < self.floor and floor > st:
					self.stops.insert(i, floor)
					added = True
					break

		if not added:
			self.stops.append(floor)

		self.start()

	def arrived(self):
		self.direction = Direction.NONE
