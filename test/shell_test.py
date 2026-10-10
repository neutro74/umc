#!/usr/bin/env python3
"""Drives `umc shell` in a real pseudo-terminal against a running game.

usage: shell_test.py <umc binary> <game pid>
"""
import os
import pty
import select
import sys
import time

umc, game = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.execv(umc, [umc, "-p", game, "shell"])

output = b""


def read(timeout=0.4):
    global output
    end = time.time() + timeout
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            output += data


def wait_for(text, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        read(0.2)
        if text.encode() in output:
            return True
    return False


failures = 0


def check(what, ok):
    global failures
    print(("  pass  " if ok else "  FAIL  ") + what)
    failures += 0 if ok else 1


check("shell shows a prompt for the game", wait_for("umc[1.99.9 client]>"))
os.write(fd, b"status\r")
check("a command runs and prints", wait_for("minecraft 1.99.9"))
os.write(fd, b"sta\t")
read(0.5)
check("tab completes commands", b"status" in output)
os.write(fd, b"\x15script list\r")
check("script listing works", wait_for("no scripts loaded"))
os.write(fd, b"trace net.minecraft.client.Minecraft computeScore\r")
check("trace starts", wait_for("tracing"))
check("live events appear above the prompt", wait_for("trace net.minecraft.client.Minecraft.computeScore", 8))
os.write(fd, b"untrace all\r")
wait_for("stopped")
os.write(fd, b"nosuchcommand\r")
check("unknown commands are reported", wait_for("unknown command"))
os.write(fd, b"\x1b[A")                  # up arrow recalls history
read(0.4)
os.write(fd, b"\x15quit\r")
read(1.0)
try:
    _, status = os.waitpid(pid, 0)
    check("quit exits cleanly", os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)
except ChildProcessError:
    check("quit exits cleanly", True)

sys.exit(1 if failures else 0)
