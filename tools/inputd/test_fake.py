#!/usr/bin/env python3
"""Isolated socket integration test. Never launches Steam or games."""
import argparse
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

EVENT = struct.Struct('<qqHHi')
RUMBLE = struct.Struct('<4sHHIIII')


def frame(client):
    events = []
    while True:
        record = bytearray()
        while len(record) < EVENT.size:
            chunk = client.recv(EVENT.size - len(record))
            assert chunk, 'unexpected EOF'
            record.extend(chunk)
        sec, usec, kind, code, value = EVENT.unpack(record)
        assert sec > 0 and 0 <= usec < 1000000, 'invalid timestamp'
        if kind == 0:
            assert code == value == 0, 'invalid SYN_REPORT'
            return events
        events.append((kind, code, value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', default='/tmp/lxrt-input-codex')
    parser.add_argument('--binary', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    binary = args.binary or root / os.environ.get('STEAMARM_BUILD', 'build') / 'steamarm-inputd'
    directory = Path(args.directory)
    assert str(directory) != '/tmp/lxrt-input', 'shared test directory forbidden'
    assert not (directory / 'inputd.pid').exists(), 'test directory already in use'
    with tempfile.TemporaryDirectory(prefix='inputd-test-') as temp:
        temp = Path(temp)
        script = temp / 'events.txt'
        script.write_text('200 a 1\n350 a 0\n500 leftx 32767\n650 zl 32767\n800 dpadUp 1\n')
        with (temp / 'stderr').open('w+') as log:
            command = [str(binary), '--fake', 'xbox360', '--script', str(script),
                       '--dir', str(directory), '--config', str(temp / 'missing.json'), '--foreground']
            process = subprocess.Popen(command, stderr=log)
            clients = []
            try:
                deadline = time.monotonic() + 5
                while not (directory / 'event0').exists():
                    if process.poll() is not None:
                        log.seek(0)
                        raise AssertionError('daemon exited at startup: ' + log.read().strip())
                    assert time.monotonic() < deadline, 'socket startup timed out'
                    time.sleep(.01)
                meta = (directory / 'meta/event0').read_text()
                assert 'name Microsoft X-Box 360 pad\n' in meta
                assert 'id 0003 045e 028e 0114\n' in meta
                assert 'phys usb-steamarm-0/input0\n' in meta
                assert 'key 304 305 307 308 310 311 314 315 316 317 318\n' in meta
                assert 'abs 0 -32768 32767 16 128 0\n' in meta
                assert 'abs 2 0 255 0 0 0\n' in meta
                assert 'ff 80 81 88 89 90 96\neffects 16\n' in meta
                print('PASS metadata and publication')
                for _ in range(2):
                    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    client.settimeout(3)
                    client.connect(str(directory / 'event0'))
                    clients.append(client)
                    snapshot = frame(client)
                    assert set(snapshot) == {(3, code, 0) for code in (0, 1, 2, 3, 4, 5, 16, 17)}, snapshot
                print('PASS initial snapshots (two clients)')
                duplicate = subprocess.run(command, capture_output=True, text=True, timeout=3)
                assert duplicate.returncode == 0 and 'already running' in duplicate.stderr
                print('PASS single instance')
                expected = [[(1, 304, 1)], [(1, 304, 0)], [(3, 0, 32767)], [(3, 2, 255)], [(3, 17, -1)]]
                for client in clients:
                    actual = [frame(client) for _ in expected]
                    assert actual == expected, actual
                print('PASS scripted changed-only frames and SYN_REPORT')
                late = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                late.settimeout(3)
                late.connect(str(directory / 'event0'))
                clients.append(late)
                snapshot = frame(late)
                assert (3, 0, 32767) in snapshot and (3, 2, 255) in snapshot and (3, 17, -1) in snapshot
                print('PASS current-state snapshot on late accept')
                record = RUMBLE.pack(b'RMBL', 12345, 23456, 65535, 0, 0, 0)
                clients[0].sendall(record[:7])
                time.sleep(.02)
                clients[0].sendall(record[7:] + RUMBLE.pack(b'RMBL', 0, 0, 0, 0, 0, 0))
                deadline = time.monotonic() + 3
                while True:
                    log.seek(0)
                    output = log.read()
                    if 'rumble strong=12345 weak=23456 ms=65535' in output and 'rumble strong=0 weak=0 ms=0' in output:
                        break
                    assert time.monotonic() < deadline, output
                    time.sleep(.01)
                print('PASS fragmented/coalesced RMBL records (65535 and stop)')
                clients[0].close()
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=5) == 0
                assert not list(directory.glob('event*'))
                assert not (directory / 'inputd.pid').exists()
                assert not (directory / 'meta').exists() or not list((directory / 'meta').iterdir())
                print('PASS SIGTERM cleanup')
            finally:
                for client in clients:
                    client.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    main()
