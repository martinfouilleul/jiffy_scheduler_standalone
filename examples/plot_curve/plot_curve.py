#!/usr/bin/env python3

import sys
import re
import os
import numpy as np
import csv
from matplotlib import pyplot as plt

#TODO parse arguments etc


if os.system('./build.sh') != 0:
	exit(1);

os.system('./bin/print_curve > curve.tmp')

# read the input file

time_from_pos_lines = []
pos_from_time_lines = []
contents_lines = time_from_pos_lines

csvFile = open('curve.tmp')
for line in csvFile:
	line = line.rstrip()
	if line == "time from pos":
		contents_lines = time_from_pos_lines
	elif line == "pos from time":
		contents_lines = pos_from_time_lines
	else:
		contents_lines.append(line)

time_from_pos_reader = csv.reader(time_from_pos_lines, delimiter = ';')
time_from_pos_contents = list(time_from_pos_reader)
time_from_pos_columns = list(map(list, zip(*time_from_pos_contents)))

pos_from_time_reader = csv.reader(pos_from_time_lines, delimiter = ';')
pos_from_time_contents = list(pos_from_time_reader)
pos_from_time_columns = list(map(list, zip(*pos_from_time_contents)))


os.remove('curve.tmp')

# make arrays
# time from pos
tfp_pos = np.asarray(time_from_pos_columns[0], dtype=np.float)
tfp_time = np.asarray(time_from_pos_columns[1], dtype=np.float)

tfp_pulses_pos = tfp_pos[ np.abs(tfp_pos - np.floor(tfp_pos)) < (tfp_pos[1]-tfp_pos[0])]
tfp_pulses_time = tfp_time[ np.abs(tfp_pos - np.floor(tfp_pos)) < (tfp_pos[1]-tfp_pos[0]) ]

tfp_inferred_tempo = (tfp_pos[1]-tfp_pos[0])/(tfp_time[1:] - tfp_time[:len(tfp_time)-1])

# pos from time
pft_pos = np.asarray(pos_from_time_columns[0], dtype=np.float)
pft_time = np.asarray(pos_from_time_columns[1], dtype=np.float)

pft_pulses_pos = pft_pos[ np.abs(pft_pos - np.floor(pft_pos)) < 0.01]
pft_pulses_time = pft_time[ np.abs(pft_pos - np.floor(pft_pos)) < 0.01 ]

pft_inferred_tempo = (pft_pos[1:] - pft_pos[:len(pft_pos)-1])/(pft_time[1]-pft_time[0])


#tfp_inferred_tempo_var = tfp_inferred_tempo[1:] - tfp_inferred_tempo[:len(tfp_inferred_tempo)-1]
#tfp_inferred_tempo_spikes = (tfp_pos[:len(tfp_pos)-2])[ tfp_inferred_tempo_var < -0.02 ]
#print(tfp_inferred_tempo_spikes);

# plot various views

height = np.max(tfp_pos)
stem_h = 0.2*height

fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=[12, 8])

ax1.stem(tfp_pulses_time, stem_h* np.ones(len(tfp_pulses_time)), 'C1-', 'C1o', basefmt='C1-')
ax1.plot(tfp_time, tfp_pos, color = 'blue')
ax1.set_xlabel('time')
ax1.set_ylabel('position', color = 'blue')
ax1.set_title('time computed from position updates')

ax1_2 = ax1.twinx()
ax1_2.plot(tfp_time[:len(tfp_time)-1], tfp_inferred_tempo, color = 'red')
ax1_2.set_ylabel('tempo', color = 'red')
#ax1_2.set_ylim([1.8, 8.2])

ax2.plot(tfp_pos[:len(tfp_pos)-1], tfp_inferred_tempo, color = 'red')
ax2.set_xlabel('position')
ax2.set_title('tempo inferred from position updates')
#ax2.set_ylim([1.8, 8.2])

ax3.stem(pft_pulses_time, stem_h* np.ones(len(pft_pulses_time)), 'C1-', 'C1o', basefmt='C1-')
ax3.plot(pft_time, pft_pos, color = 'blue')
ax3.set_xlabel('time')
ax3.set_ylabel('position', color = 'blue')
ax3.set_title('position computed from time updates')

ax3_2 = ax3.twinx()
ax3_2.plot(pft_time[:len(pft_time)-1], pft_inferred_tempo, color = 'red')
ax3_2.set_ylabel('tempo', color = 'red')
#ax3_2.set_ylim([1.8, 8.2])

ax4.plot(pft_pos[:len(pft_pos)-1], pft_inferred_tempo, color = 'red')
ax4.set_xlabel('position')
ax4.set_title('tempo inferred from time updates')
#ax4.set_ylim([1.8, 8.2])

plt.tight_layout()
plt.savefig('curve.eps')
plt.show()
