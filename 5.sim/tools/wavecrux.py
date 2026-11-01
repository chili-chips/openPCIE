#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Opens a run's waveform in WaveCrux at the RC model checker's first violation
# (make wavecrux SEED=N). WaveCrux must run with its remote control (WCP) on,
# e.g. wavecrux_pro.exe --wcp-port 54321; from WSL it is reached on localhost.
#
#   wavecrux.py FST LOG [SIGNAL ...]
#
# The time is the first "**** CHECK <rule> (<section>) at <t> us" line in LOG;
# with none, the whole run is shown. Under WSL the FST is handed over as its
# Windows path (wslpath -w). Standard library only.

import json
import os
import re
import socket
import subprocess
import sys
import time

PORT = int(os.environ.get('WCX_PORT', '54321'))
HALF_VIEW_PS = 2_000_000    # 2 us either side of the violation
LOAD_WAIT_S = 120           # for WaveCrux to report the file loaded


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: wavecrux.py FST LOG [SIGNAL ...]')
    fst, log, signals = os.path.abspath(sys.argv[1]), sys.argv[2], sys.argv[3:]

    first = None
    with open(log, errors='replace') as f:
        for line in f:
            m = re.search(r'\*\*\*\* CHECK (\S+) \((\S+)\) at ([0-9.]+) us: (.*)', line)
            if m:
                first = m
                break

    try:
        win = subprocess.run(['wslpath', '-w', fst], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        win = fst

    try:
        sock = socket.create_connection(('127.0.0.1', PORT), timeout=10)
    except OSError:
        sys.exit(f'wavecrux.py: no WaveCrux remote control on port {PORT}: start WaveCrux with --wcp-port {PORT}')
    sock.settimeout(None)       # a large FST may take a while to load
    buf = b''

    def recv():
        nonlocal buf
        while b'\0' not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                sys.exit('wavecrux.py: WaveCrux closed the connection')
            buf += chunk
        frame, buf = buf.split(b'\0', 1)
        return json.loads(frame) if frame.strip() else {}

    events = []

    def send(obj):
        sock.sendall(json.dumps(obj).encode() + b'\0')
        while True:
            msg = recv()
            if msg.get('type') == 'event':
                events.append(msg)
            else:
                return msg

    send({'type': 'greeting', 'version': '0', 'commands': []})
    reply = send({'type': 'command', 'command': 'load', 'source': win})
    if reply.get('type') == 'error':
        sys.exit(f'wavecrux.py: load failed: {reply}')

    # Load ends with an event; view and cursor set before it are reset
    deadline = time.time() + LOAD_WAIT_S
    while not any(e.get('event') == 'waveforms_loaded' for e in events) and time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            msg = recv()
            if msg.get('type') == 'event':
                events.append(msg)
        except socket.timeout:
            break
    sock.settimeout(None)

    if signals:
        reply = send({'type': 'command', 'command': 'add_items', 'items': signals})
        if reply.get('type') == 'error':
            print(f'wavecrux.py: add_items failed: {reply}', file=sys.stderr)
    if first:
        # WaveCrux resets the view once more after a load; set it until it holds
        t_ps  = int(float(first.group(3)) * 1_000_000)
        start = max(0, t_ps - HALF_VIEW_PS)
        for _ in range(10):
            send({'type': 'command', 'command': 'set_viewport_range', 'start': start, 'end': t_ps + HALF_VIEW_PS})
            send({'type': 'command', 'command': 'set_cursor', 'timestamp': t_ps})
            time.sleep(0.5)
            state = send({'type': 'command', 'command': 'wavecrux.getState'})
            if abs(float(state.get('pan_offset_ticks') or 0) - start) < HALF_VIEW_PS / 100:
                break
        print(f'WaveCrux: {win}, cursor at {first.group(3)} us: {first.group(1)} ({first.group(2)}) {first.group(4)}')
    else:
        send({'type': 'command', 'command': 'zoom_to_fit', 'viewport_idx': 0})
        print(f'WaveCrux: {win}, no checker violation in {log}: the whole run')
    sock.close()


if __name__ == '__main__':
    main()
