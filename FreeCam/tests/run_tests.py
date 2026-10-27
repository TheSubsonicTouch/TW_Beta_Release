"""Build and run the free camera's unit tests.

    python FreeCam/tests/run_tests.py

The camera maths, the ini parser and the toggle/movement state machine are plain C with
no Windows calls, so they are tested here rather than in game. Any C99 compiler works;
this looks for cc, gcc or clang, and says so if none is installed.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SRC = os.path.join(ROOT, "src")


def compiler():
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return None


def main():
    cc = compiler()
    if not cc:
        print("no C compiler found (cc / gcc / clang); cannot build the tests")
        return 2
    workdir = tempfile.mkdtemp(prefix="freecam-tests-")
    exe = os.path.join(workdir, "test_freecam.exe" if os.name == "nt" else "test_freecam")
    cmd = [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-o", exe,
           os.path.join(SRC, "freecam.c"), os.path.join(SRC, "test_freecam.c"), "-lm"]
    print(" ".join(cmd))
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout)
        print(build.stderr)
        return 1
    if build.stderr.strip():
        print(build.stderr.strip())
    run = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
