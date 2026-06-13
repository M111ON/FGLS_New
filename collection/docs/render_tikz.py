import matplotlib.pyplot as plt
import numpy as np

fig, ax = plt.subplots(figsize=(10, 8))
ax.set_aspect('equal')

pts = {
    'A': (0, 3),
    'B': (0, 0),
    "A'": (-1.7633557568774194, 2.4270509831248424),
    "B'": (-1.7633557568774194, 0.5729490168751576),
    'C': (-1.0898137920080413, 1.5),
    "D'": (0.6735419648693779, 0.9270509831248424),
    "B'_1": (0.6735419648693779, 2.0729490168751576),
    "B'_2": (2.598076211353316, 1.5),
    "D'_1": (1.5, 2.598076211353316),
    'G': (1.299038105676658, 0.75),
    'H': (0, 1.5),
    'I': (0.8660254037844386, 1.5),
}

ax.fill([pts['A'][0], pts["B'_2"][0], pts['B'][0]],
        [pts['A'][1], pts["B'_2"][1], pts['B'][1]],
        alpha=0.1, color='#993300')

ax.plot([pts['A'][0], pts['B'][0]], [pts['A'][1], pts['B'][1]], 'k-', linewidth=4.4)
ax.plot([pts['A'][0], pts["D'"][0]], [pts['A'][1], pts["D'"][1]], 'k-', linewidth=2)
ax.plot([pts["B'_1"][0], pts['B'][0]], [pts["B'_1"][1], pts['B'][1]], 'k-', linewidth=1.6)
ax.plot([pts['A'][0], pts["B'_2"][0]], [pts['A'][1], pts["B'_2"][1]], '--', color='#993300', linewidth=2)
ax.plot([pts["B'_2"][0], pts['B'][0]], [pts["B'_2"][1], pts['B'][1]], '--', color='#993300', linewidth=2)
ax.plot([pts["B'_1"][0], pts['C'][0]], [pts["B'_1"][1], pts['C'][1]], '--', color='#666666', linewidth=1.6)
ax.plot([pts["D'"][0], pts['C'][0]], [pts["D'"][1], pts['C'][1]], '--', color='#666666', linewidth=1.6)

x_vals = np.linspace(-4.46, 0, 100)
y1 = (-5.2900672706322585 - 2.4270509831248424 * x_vals) / -1.7633557568774194
ax.plot(x_vals, y1, 'k-', linewidth=2)
y2 = (0 - 2.4270509831248424 * x_vals) / -1.7633557568774194
ax.plot(x_vals, y2, 'k-', linewidth=2)

def draw_arc(center, start_deg, end_deg, r, color, label=None):
    theta = np.linspace(np.radians(start_deg), np.radians(end_deg), 100)
    x = center[0] + r * np.cos(theta)
    y = center[1] + r * np.sin(theta)
    ax.plot(x, y, color=color, linewidth=2)
    mid = np.radians((start_deg + end_deg)/2)
    mx = center[0] + (r+0.15) * np.cos(mid)
    my = center[1] + (r+0.15) * np.sin(mid)
    if label:
        ax.text(mx, my, label, fontsize=9, color=color, ha='center', va='center')

r = 0.25
draw_arc((0,3), -126, -90, r, 'green', r'$\alpha = 36^\circ$')
draw_arc((0,0), 90, 126, r, 'green', r'$\beta = 36^\circ$')
draw_arc((-1.0898137920080413, 1.5), -18, 54, r, 'green', r'$\gamma = 72^\circ$')
draw_arc((-1.0898137920080413, 1.5), -54, 18, r, 'green', r'$\delta = 72^\circ$')
draw_arc((0,3), -90, -30, r, 'green', r'$\epsilon = 60^\circ$')
draw_arc((0,0), 60, 90, r, 'green', r'$\zeta = 30^\circ$')

colors = {'A': 'blue', "A'": 'blue', 'B': 'blue', "B'": 'blue', "D'": 'blue',
          "B'_1": 'blue', "B'_2": 'blue', "D'_1": 'blue', 'G': 'blue', 'H': 'blue', 'I': 'blue'}
for name, (x, y) in pts.items():
    if name == 'C':
        continue
    ax.plot(x, y, 'o', color=colors.get(name, 'blue'), markersize=4)
    ax.text(x+0.05, y+0.08, f'${name}$', fontsize=10, color=colors.get(name, 'blue'))

ax.plot(pts['C'][0], pts['C'][1], 'o', color='#6e6e6e', markersize=3)
ax.text(pts['C'][0]+0.05, pts['C'][1]+0.08, '$C$', fontsize=10, color='#6e6e6e')

labels = {'g': (-0.92, 1.94), 'h': (-0.92, 1.21), 'k': (-0.22, 1.95),
          'l': (-0.16, 1.38), 'm': (0.27, 2.01), 'n': (0.27, 1.14),
          'i': (-0.08, 1.57)}
for name, (x, y) in labels.items():
    ax.text(x, y, f'${name}$', fontsize=10)
ax.text(1.27, 2.24, '$b$', fontsize=10, color='#993300')
ax.text(1.27, 0.90, '$d$', fontsize=10, color='#993300')

ax.set_xlim(-4.46, 4.90)
ax.set_ylim(-2.24, 4.47)
ax.grid(True, alpha=0.3)
ax.set_xlabel('x')
ax.set_ylabel('y')
plt.tight_layout()
plt.savefig('I:\\FGLS_new\\collection\\docs\\tikz_render.svg', format='svg')
print("Saved tikz_render.svg")
