from location import Room

class Fahrstuhl(Room):
    prefix = 'fahrstuhl'
    shortcut = '01'
    background = Play(P_Background, 'motor_s16.wav',
                      prefix=prefix)

	def __init__(self):
		self.floor = 0