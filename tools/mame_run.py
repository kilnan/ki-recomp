"""Run MAME as a child that never outlives its caller.

MAME is started in its own process group with a wall-clock timeout. It is
killed on timeout, on any exception (including KeyboardInterrupt) and on
SIGTERM/SIGHUP to the Python process, so interrupted tools leave no orphaned
emulators behind.
"""
import os
import signal
import subprocess


def _raise_exit(signum, _frame):
    raise SystemExit(128 + signum)


for _sig in (signal.SIGTERM, signal.SIGHUP):
    signal.signal(_sig, _raise_exit)


def run(cmd, cwd, timeout):
    """Run cmd; return (returncode, stdout+stderr). returncode is None on timeout."""
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=timeout)
        return proc.returncode, out
    except subprocess.TimeoutExpired:
        _kill(proc)
        out, _ = proc.communicate()
        return None, (out or "") + f"\n[killed after {timeout}s wall-clock timeout]\n"
    except BaseException:
        _kill(proc)
        raise


def _kill(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
