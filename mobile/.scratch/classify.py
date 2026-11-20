import sys
sys.path.insert(0, '.')
from png import png, px

img = png(sys.argv[1]); w, h, ch, p = img
x0, y0, x1, y1 = [int(v) for v in sys.argv[2:6]]
step = int(sys.argv[6]) if len(sys.argv) > 6 else 8

# palette: name, r, g, b
pal = [
    ('sky',    0.45, 0.62, 0.85),
    ('sky_lo', 0.70, 0.78, 0.90),
    ('sand',   0.78, 0.70, 0.50),
    ('wet',    0.55, 0.50, 0.36),
    ('grass',  0.30, 0.52, 0.26),
    ('moss',   0.22, 0.40, 0.22),
    ('water',  0.10, 0.25, 0.40),
    ('bark',   0.28, 0.20, 0.14),
    ('rock',   0.42, 0.40, 0.38),
    ('night',  0.02, 0.02, 0.03),
]

sym = {'sky': '.', 'sky_lo': ',', 'sand': 's', 'wet': 'w', 'grass': 'g', 'moss': 'm',
       'water': '~', 'bark': 'T', 'rock': '#', 'night': ' '}

counts = {}
rows = 0
for y in range(y0, y1, step):
    line = ''
    for x in range(x0, x1, step):
        R, G, B = px(img, x, y)
        r, g, b = R/255.0, G/255.0, B/255.0
        best, bd = None, 1e9
        for name, pr, pg, pb in pal:
            d = (r-pr)**2 + (g-pg)**2 + (b-pb)**2
            if d < bd:
                bd, best = d, name
        counts[best] = counts.get(best, 0) + 1
        line += sym[best]
    rows += 1
    print(line)

tot = sum(counts.values())
print(' '.join('%s %.1f%%' % (k, 100.0*v/tot) for k, v in sorted(counts.items(), key=lambda kv: -kv[1])))
