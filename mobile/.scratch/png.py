import zlib, struct, sys

def png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    i = 8; idat = b''; w = h = ct = 0
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]; typ = d[i+4:i+8]; data = d[i+8:i+8+ln]
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', data[:10])
        elif typ == b'IDAT':
            idat += data
        elif typ == b'IEND':
            break
        i += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    stride = w * ch
    out = bytearray(h * stride); prev = bytearray(stride); p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for x in range(ch, stride): line[x] = (line[x] + line[x-ch]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0
                b = prev[x]; c = prev[x-ch] if x >= ch else 0
                pa = abs(b-c); pb = abs(a-c); pc = abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y*stride:(y+1)*stride] = line; prev = line
    return w, h, ch, bytes(out)

def px(img, x, y):
    w, h, ch, p = img
    o = (y*w + x)*ch
    return p[o], p[o+1], p[o+2]

if __name__ == '__main__':
    img = png(sys.argv[1]); w, h, ch, p = img
    mode = sys.argv[2] if len(sys.argv) > 2 else 'lum'
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 56
    rows = int(cols*h/w*0.5)
    print('size', w, h, 'ch', ch)
    ramp = " .:-=+*#%@"
    for r in range(rows):
        out = []
        for c in range(cols):
            R, G, B = px(img, int((c+0.5)*w/cols), int((r+0.5)*h/rows))
            if mode == 'lum':
                l = (0.299*R + 0.587*G + 0.114*B)/255
                out.append(ramp[min(9, int(l*10))])
            elif mode == 'hex':
                out.append("%02x%02x%02x " % (R, G, B))
            else:
                # classify: sky/water/grass/sand/other
                if B > R+25 and B > 70: out.append('W')
                elif G > R+18 and G > B+18: out.append('g')
                elif R > 150 and G > 130 and B < 150: out.append('s')
                elif R > G and G > B and R > 180: out.append('S')
                elif abs(R-G) < 18 and abs(G-B) < 18 and R > 180: out.append('.')
                elif abs(R-G) < 22 and abs(G-B) < 22 and R < 80: out.append('#')
                elif R > 120 and G < 110 and B < 110: out.append('r')
                else: out.append('?')
        print(''.join(out))
