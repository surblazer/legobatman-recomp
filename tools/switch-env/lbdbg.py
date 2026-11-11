#!/usr/bin/env python3
"""Client for the LEGO Batman Switch debug server (legobatman/switch/host/debug_server.cpp, port 6543).

    lbdbg.py ping | info
    lbdbg.py log [--seconds N] [--save FILE]       the log, live (Ctrl-C stops)
    lbdbg.py shot [OUT.png]                        the next presented frame
    lbdbg.py press A [B ...] [--ms 120]            buttons together (Xbox names: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT)
    lbdbg.py hold A | release [A]                  held until released
    lbdbg.py stick L|R X Y                         -1..1, y up; "stick L 0 0" lets go
    lbdbg.py trigger L|R V                         0..1
    lbdbg.py draws | skip PS_HASH | unskip [PS_HASH]   next frame's draw list; skip a pixel shader's draws
    lbdbg.py mem ADDR_HEX BYTES OUT                guest physical memory to a file
    lbdbg.py get REMOTE [LOCAL] | put LOCAL REMOTE | ls [PATH] | rm PATH
Paths are relative to sdmc:/switch/legobatman/ unless they start with '/'.
The console's address: env LB_HOST, or the first line of tools/switch-env/switch_host.txt.
"""
import os
import socket
import sys
import time
from pathlib import Path

PORT = 6543


def host():
    h = os.environ.get('LB_HOST')
    if h:
        return h
    f = Path(__file__).with_name('switch_host.txt')
    if f.exists():
        return f.read_text().split()[0]
    sys.exit('set LB_HOST or write the console IP into ' + str(f))


def connect():
    s = socket.create_connection((host(), PORT), timeout=10)
    return s


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(min(1 << 20, n - len(buf)))
        if not chunk:
            raise ConnectionError('connection closed')
        buf += chunk
    return bytes(buf)


def recv_line(s):
    buf = bytearray()
    while True:
        c = s.recv(1)
        if not c:
            raise ConnectionError('connection closed')
        if c == b'\n':
            return buf.decode()
        buf += c


def command(line, payload=None, timeout=30):
    s = connect()
    s.settimeout(timeout)
    s.sendall((line + '\n').encode())
    if payload:
        s.sendall(payload)
    head = recv_line(s).split()
    body = recv_exact(s, int(head[1]))
    s.close()
    return head[0] == 'ok', body


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd in ('ping', 'info'):
        ok, body = command(cmd)
        print(body.decode(errors='replace'))
    elif cmd == 'log':
        seconds = float(a[a.index('--seconds') + 1]) if '--seconds' in a else None
        out = open(a[a.index('--save') + 1], 'ab') if '--save' in a else None
        s = connect()
        s.sendall(b'log\n')
        recv_line(s)
        s.settimeout(1.0)
        t0 = time.time()
        try:
            while seconds is None or time.time() - t0 < seconds:
                try:
                    data = s.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
                if out:
                    out.write(data)
        except KeyboardInterrupt:
            pass
    elif cmd == 'shot':
        out = a[1] if len(a) > 1 else 'shot.png'
        ok, body = command('shot', timeout=15)
        if not ok:
            sys.exit(body.decode())
        nl = body.index(b'\n')
        _, w, h = body[:nl].split()
        from PIL import Image
        Image.frombytes('RGBA', (int(w), int(h)), body[nl + 1:]).convert('RGB').save(out)
        print(out, w, h)
    elif cmd in ('press', 'hold', 'release'):
        ms = a[a.index('--ms') + 1] if '--ms' in a else None
        names = [x for x in a[1:] if x != '--ms' and x != ms]
        line = cmd + (' ' + '+'.join(names) if names else '') + (' ' + ms if ms and cmd == 'press' else '')
        ok, body = command(line)
        if not ok:
            sys.exit(body.decode())
    elif cmd in ('stick', 'trigger'):
        ok, body = command(' '.join(a))
        if not ok:
            sys.exit(body.decode())
    elif cmd == 'get':
        ok, body = command('get "%s"' % a[1], timeout=120)
        if not ok:
            sys.exit(body.decode())
        Path(a[2] if len(a) > 2 else Path(a[1]).name).write_bytes(body)
        print(len(body), 'bytes')
    elif cmd == 'put':
        data = Path(a[1]).read_bytes()
        ok, body = command('put "%s" %d' % (a[2], len(data)), payload=data, timeout=300)
        print(body.decode())
        if not ok:
            sys.exit(1)
    elif cmd == 'mem':
        ok, body = command('mem %s %s' % (a[1], a[2]), timeout=60)
        if not ok:
            sys.exit(body.decode(errors='replace'))
        Path(a[3]).write_bytes(body)
        print(len(body), 'bytes')
    elif cmd in ('skip', 'unskip', 'draws', 'flag'):
        ok, body = command(' '.join(a), timeout=15)
        sys.stdout.write(body.decode(errors='replace'))
        if not ok:
            sys.exit(1)
    elif cmd in ('ls', 'rm'):
        ok, body = command('%s "%s"' % (cmd, a[1] if len(a) > 1 else '.'))
        print(body.decode(errors='replace'))
        if not ok:
            sys.exit(1)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
