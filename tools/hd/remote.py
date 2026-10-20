#!/usr/bin/env python3
"""Size-checked transfers to and from a GPU instance over ssh (no scp).

  remote.py SSH_CMD put LOCAL_TAR REMOTE_DIR NAME   extract the tar in REMOTE_DIR, then touch NAME/READY
  remote.py SSH_CMD get REMOTE_DIR PATH LOCAL_TAR   tar REMOTE_DIR/PATH into LOCAL_TAR
SSH_CMD runs a command on the instance (e.g. a wrapper that finds its IP and port).
Each transfer is checked against the size the other side sees, and retried
(connections to busy instances stall or drop). On macOS create tars with
COPYFILE_DISABLE=1: '._*' files break the upscaler.
"""
import os
import subprocess
import sys
import time


def run(ssh: str, cmd: str, timeout: int, stdin=None, stdout=subprocess.PIPE) -> subprocess.CompletedProcess:
    return subprocess.run([ssh, cmd], stdin=stdin if stdin is not None else subprocess.DEVNULL, stdout=stdout,
                          stderr=subprocess.DEVNULL, timeout=timeout)


def numbers(out: bytes) -> list:
    return [int(x) for x in out.split() if x.isdigit()]


def main() -> int:
    ssh, op = sys.argv[1], sys.argv[2]
    for attempt in range(5):
        try:
            if op == "put":
                local, rdir, name = sys.argv[3:6]
                size = os.path.getsize(local)
                with open(local, "rb") as f:
                    r = run(ssh, f"mkdir -p {rdir} && cd {rdir} && cat > up_{name}.tar && wc -c < up_{name}.tar && "
                                 f"tar xf up_{name}.tar && rm up_{name}.tar && find . -name '._*' -delete && "
                                 f"touch {name}/READY", 600, stdin=f)
                got = numbers(r.stdout)
                if got and got[-1] == size:
                    print(name, "uploaded", size)
                    return 0
            elif op == "get":
                rdir, path, local = sys.argv[3:6]
                size = numbers(run(ssh, f"cd {rdir} && tar cf - {path} | wc -c", 300).stdout)[-1]
                with open(local, "wb") as f:
                    run(ssh, f"cd {rdir} && tar cf - {path}", 3600, stdout=f)
                if os.path.getsize(local) == size:
                    print(path, "downloaded", size)
                    return 0
        except (subprocess.TimeoutExpired, IndexError):
            pass
        print("retry", attempt + 1, file=sys.stderr)
        time.sleep(5)
    return 1


if __name__ == "__main__":
    sys.exit(main())
