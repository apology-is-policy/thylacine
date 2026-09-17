#!/usr/bin/env python3
"""Bounded loopback fixture for netperf's three-port NIC workload.

The sink verifies every byte independently of the guest's send-completion
report. Echo ports exercise immediate and delayed RX wakeups. No guestfwd is
needed: the guest dials the host gateway after the listener is ready.
"""
import argparse
import signal
import socket
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--log', required=True)
args = parser.parse_args()
log = open(args.log, 'w', buffering=1)
lock = threading.Lock()

def report(text):
    with lock:
        print(text, file=log, flush=True)

def bind(port):
    sock = socket.socket()
    try:
        sock.bind(('127.0.0.1', port))
        sock.listen(64)
        return sock
    except Exception:
        sock.close()
        raise

# Retain all three listeners before publishing the base; no check-then-bind.
listeners = []
for _ in range(100):
    try:
        listeners = [bind(0)]
        port = listeners[0].getsockname()[1]
        if port > 65533:
            raise OSError('base too high')
        listeners.append(bind(port + 1))
        listeners.append(bind(port + 2))
        break
    except OSError:
        for sock in listeners:
            sock.close()
else:
    raise SystemExit('cannot reserve three listener ports')

def client(sock, kind):
    total = 0
    try:
        with sock:
            sock.settimeout(180)
            while True:
                data = sock.recv(65536)
                if not data:
                    break
                if kind == 2:
                    if data != b'\xa5' * len(data):
                        raise ValueError('corrupted sink payload')
                    total += len(data)
                    if total > 8 * 1024 * 1024:
                        raise ValueError('excess sink payload')
                else:
                    if data != b'\x5a' * len(data):
                        raise ValueError('corrupted echo payload')
                    if kind == 1:
                        time.sleep(0.005)
                    sock.sendall(data)
        if kind == 2:
            report(f'sink EOF verified {total} bytes')
    except Exception as error:
        report(f'ERROR {kind}: {error}')

def serve(sock, kind):
    while True:
        connection, _ = sock.accept()
        threading.Thread(target=client, args=(connection, kind), daemon=True).start()

for kind, sock in enumerate(listeners):
    threading.Thread(target=serve, args=(sock, kind), daemon=True).start()
report(f'listening {port}')
signal.signal(signal.SIGTERM, lambda *_: exit(0))
# A failed guest/test cannot leave the fixture behind indefinitely.
time.sleep(240)
report('ERROR fixture deadline expired')
