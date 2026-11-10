#!/usr/bin/env python3
"""Unpacks a RECORD 5 SEC file (sdmc:/switch/cvhd/rec/rec_<swap>.hrec, switch/host/frame_recorder.cpp).

    python tools/dkrec.py <rec_N.hrec> [<out dir>] [--ffmpeg <ffmpeg.exe>]

Writes <out>/f0000.png ... (lossless frames), <out>/diff/d0001.png ... (white = pixels that changed from the frame
before), <out>/changes.txt (changed pixels per frame and the 16x9 tiles that changed), <out>/sheet.jpg (every 10th
frame, quarter size) and, with --ffmpeg, <out>/rec.mp4 at the recorded frame times. Needs lz4, numpy, Pillow.
"""
import struct
import subprocess
import sys
from pathlib import Path

import lz4.block
import numpy as np
from PIL import Image


def frames(path):
    data = Path(path).read_bytes()
    pos, prev = 0, None
    while pos + 32 <= len(data):
        magic, index, swap, us, w, h, size, _ = struct.unpack_from('<8I', data, pos)
        assert magic == 0x43455248, 'bad header at %d' % pos
        pos += 32
        delta = np.frombuffer(lz4.block.decompress(data[pos:pos + size], uncompressed_size=w * h * 4), np.uint8)
        pos += size
        cur = delta if prev is None else delta ^ prev
        prev = cur
        yield index, swap, us, cur.reshape(h, w, 4)


def main():
    args = sys.argv[1:]
    ff = None
    if '--ffmpeg' in args:
        i = args.index('--ffmpeg')
        ff = args[i + 1]
        del args[i:i + 2]
    src = Path(args[0])
    out = Path(args[1]) if len(args) > 1 else src.with_suffix('')
    (out / 'diff').mkdir(parents=True, exist_ok=True)
    prev, thumbs, times, lines = None, [], [], []
    for index, swap, us, img in frames(src):
        Image.fromarray(img[:, :, :3]).save(out / ('f%04d.png' % index))
        times.append(us)
        if prev is not None:
            changed = np.any(img[:, :, :3] != prev[:, :, :3], axis=2)
            Image.fromarray((changed * 255).astype(np.uint8)).save(out / 'diff' / ('d%04d.png' % index))
            h, w = changed.shape
            tiles = [(tx, ty) for ty in range(9) for tx in range(16)
                     if changed[ty * h // 9:(ty + 1) * h // 9, tx * w // 16:(tx + 1) * w // 16].any()]
            lines.append('frame %d swap %d t %.1f ms: %d pixels changed, tiles %s' % (
                index, swap, us / 1000, int(changed.sum()), ' '.join('%d,%d' % t for t in tiles) or '-'))
        if index % 10 == 0:
            thumbs.append(Image.fromarray(img[:, :, :3]).resize((320, 180)))
        prev = img
    (out / 'changes.txt').write_text('\n'.join(lines) + '\n')
    if thumbs:
        cols = 5
        sheet = Image.new('RGB', (320 * cols, 180 * ((len(thumbs) + cols - 1) // cols)))
        for i, t in enumerate(thumbs):
            sheet.paste(t, (320 * (i % cols), 180 * (i // cols)))
        sheet.save(out / 'sheet.jpg', quality=90)
    if ff and len(times) > 1:
        fps = (len(times) - 1) / ((times[-1] - times[0]) / 1e6)
        subprocess.run([ff, '-y', '-framerate', '%.3f' % fps, '-i', str(out / 'f%04d.png'), '-c:v', 'libx264', '-crf', '12',
                        '-pix_fmt', 'yuv420p', str(out / 'rec.mp4')], check=True)
    print('%d frames -> %s' % (len(times), out))


if __name__ == '__main__':
    main()
