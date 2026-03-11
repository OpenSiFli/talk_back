import os
from building import *
import rtconfig

src = []
inc = []

cwd = GetCurrentDir()
inc    += [cwd]
inc    += [os.path.join(cwd, 'talk_network')]
inc    += [os.path.join(cwd, 'ble_talk')]
src  += Glob('ble_talk/*.c')
src  += Glob('talk_network/*.c')

group = DefineGroup('talk_back', src, depend = ['USING_TALK_BACK'], CPPPATH = inc)

Return('group')
