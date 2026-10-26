"""Check the launcher's decisions without a game or a port 443.

    python test_launch.py

The launcher is the one piece that runs before anything else, so it has to get the
boring cases right: is that port ours, is it a leftover copy, is it somebody else's
program - and does the report say enough when the game dies. Everything here works on
a fake network, so it runs anywhere and never touches the real server.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import launch  # noqa: E402

SERVER_MTIME = os.path.getmtime(os.path.join(launch.HERE, "server.py"))


class Fake:
    """Stands in for the network and the process table."""

    def __init__(self, info=None, open_ports=(), pid=None, line="", start_result=None):
        self.info = info
        self.open_ports = set(open_ports)
        self.pid = pid
        self.line = line
        self.start_result = start_result
        self.killed = []
        self.started = 0

    def install(self):
        self._saved = (launch.probe, launch.port_open, launch.listener_pid,
                       launch.command_line, launch.kill, launch.start_server, launch.say)
        launch.probe = lambda host, port, timeout=1.5: self.info if host == "127.0.0.1" else None
        launch.port_open = lambda host, port, timeout=1.0: port in self.open_ports
        launch.listener_pid = lambda port: self.pid
        launch.command_line = lambda pid: self.line
        launch.kill = lambda pid: self.killed.append(pid)
        launch.start_server = self._start
        launch.say = lambda *a, **k: None
        return self

    def _start(self, port, safe):
        self.started += 1
        if self.start_result is None:
            return None, {"pid": 999, "port": port, "code": {"server": SERVER_MTIME}}
        return None, self.start_result

    def remove(self):
        (launch.probe, launch.port_open, launch.listener_pid, launch.command_line,
         launch.kill, launch.start_server, launch.say) = self._saved


def main():
    failures = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-34s %s" % (name, detail or ""))
        except AssertionError as e:
            failures.append(name)
            print("FAIL  %-34s %s" % (name, e))
        except Exception as e:                       # noqa: BLE001
            failures.append(name)
            print("FAIL  %-34s %r" % (name, e))

    def own_server():
        fake = Fake(info={"pid": 1, "code": {"server": SERVER_MTIME}}).install()
        try:
            state, found = launch.server_state(443)
            assert state == "ours", state
            info, note = launch.ensure_server(443, False)
            assert info["pid"] == 1 and "already running" in note
            assert not fake.killed and fake.started == 0
        finally:
            fake.remove()
        return "reused, nothing restarted"

    def leftover_server():
        # An older copy answers /__local/info with the neutral unknown-route reply.
        fake = Fake(info={"mReturnCode": 0, "mMessage": ""}, open_ports=(443,),
                    pid=42, line='"C:\\Python311\\python.exe" server.py').install()
        try:
            state, _ = launch.server_state(443)
            assert state == "stale", state
            info, note = launch.ensure_server(443, False)
            assert fake.killed == [42], fake.killed
            assert fake.started == 1 and info["pid"] == 999
        finally:
            fake.remove()
        return "old build killed and replaced"

    def foreign_program():
        fake = Fake(info=None, open_ports=(443,), pid=7,
                    line='"C:\\Program Files\\IIS\\w3wp.exe"').install()
        try:
            state, _ = launch.server_state(443)
            assert state == "other", state
            try:
                launch.ensure_server(443, False)
            except SystemExit as e:
                assert "already in use" in str(e) and "w3wp" in str(e)
            else:
                raise AssertionError("did not refuse a foreign listener")
            assert not fake.killed and fake.started == 0
        finally:
            fake.remove()
        return "named the program, changed nothing"

    def leftover_but_not_ours():
        fake = Fake(info={"mReturnCode": 0}, open_ports=(443,), pid=9,
                    line='"C:\\Python311\\python.exe" other_server.py').install()
        try:
            try:
                launch.ensure_server(443, False)
            except SystemExit as e:
                assert "already in use" in str(e)
            else:
                raise AssertionError("killed a python that is not ours")
            assert not fake.killed
        finally:
            fake.remove()
        return "refused to kill a stranger"

    def cold_start():
        fake = Fake(info=None, open_ports=()).install()
        try:
            assert launch.server_state(443)[0] == "free"
            info, note = launch.ensure_server(443, False)
            assert fake.started == 1 and info["pid"] == 999
        finally:
            fake.remove()
        return "started on a free port"

    def server_that_dies():
        fake = Fake(info=None, open_ports=()).install()
        launch.start_server = lambda port, safe: (None, None)
        launch.console_tail = lambda lines=25: "Traceback: address already in use"
        try:
            try:
                launch.ensure_server(443, False)
            except SystemExit as e:
                assert "did not start" in str(e) and "address already in use" in str(e)
            else:
                raise AssertionError("did not report a server that never listened")
        finally:
            launch.console_tail = launch.console_tail
            fake.remove()
        return "dead server reported with its output"

    def report_says_what_happened():
        report = launch.build_report("/games/triad", 443, None, "not running",
                                     exit_code=0xC0000005, alive=2.5)
        for expected in ("3221225477", "Triad Wars launch report",
                         "What to look at first",
                         "without ever contacting the server"):
            assert expected in report, expected
        assert "None" not in report, "a None leaked into the report"
        return "%d lines, no None" % len(report.splitlines())

    def useroptions_repair():
        import shutil
        import tempfile

        import useroptions
        workdir = tempfile.mkdtemp(prefix="tw-uo-")
        path = os.path.join(workdir, "UserOptions.dat")
        saved = (useroptions.PATH, useroptions.ORIG)
        useroptions.PATH, useroptions.ORIG = path, path + ".orig"
        try:
            good = b'<UserOptions><Param key="Environment">prod.services.triadwars.com</Param></UserOptions>'
            with open(path, "wb") as f:
                f.write(useroptions.xor(good))
            shutil.copy2(path, path + ".orig")
            with open(path, "wb") as f:
                f.write(bytes([0xC9, 0xC8, 0xC9]))       # XORs to invalid UTF-8
            before, note = launch.point_client_at_localhost(workdir)
            assert "repaired" in (note or ""), (before, note)
            assert 'key="Environment">localhost<' in useroptions.read()
            before, note = launch.point_client_at_localhost(workdir)
            assert before == "localhost" and note is None, (before, note)
        finally:
            useroptions.PATH, useroptions.ORIG = saved
        return "corrupt file repaired from .orig"

    def probes_are_not_client_traffic():
        import tempfile
        workdir = tempfile.mkdtemp(prefix="tw-launch-")
        log = os.path.join(workdir, "server.log")
        with open(log, "w", encoding="utf-8") as f:
            f.write("22:00:00.000 GET /__local/info\n"
                    "22:00:00.500 GET /game/api/v1/storeaccess?launcher=1\n"
                    "22:00:01.000 GET /game/api/v1/storeaccess\n"
                    "22:00:02.000 POST /game/api/v1/errands/end/0\n")
        launch.LOG_DIR = workdir
        lines, total = launch.request_lines({"log": os.path.basename(log)})
        assert total == 2, (total, lines)
        assert all("__local" not in ln for ln in lines)
        return "2 real request(s)"

    check("port is our own server", own_server)
    check("port has an older build", leftover_server)
    check("port has a stranger", foreign_program)
    check("python, but not ours", leftover_but_not_ours)
    check("nothing on the port", cold_start)
    check("server dies on start", server_that_dies)
    check("report contents", report_says_what_happened)
    check("UserOptions.dat repair", useroptions_repair)
    check("request counting", probes_are_not_client_traffic)
    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
