"""Start the local server and the game, and write down what happened if it dies.

    python launch.py                 start the server, start the game, watch 25 s
    python launch.py diagnose        report on the setup; start nothing
    python launch.py --safe          everything with the new content switched off
    python launch.py --port 8443     use another port (testing)

`Play-TriadWars.bat` runs this. The point is the report: when the client exits on
launch, the old launcher closed its window and left nothing behind, so every crash
looked the same. This one writes `LocalServer\\last-launch.txt` - the server's pid and
build, whether the client reached it at all, the client's exit code, and the Windows
application-error entry with the faulting module and offset, if there is one.

It also replaces a server left running from an older copy of this zip, and answers
"which build is listening" through `/__local/info` (see server.py).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOG_DIR = os.path.join(HERE, "logs")
CONSOLE = os.path.join(LOG_DIR, "console.log")
REPORT = os.path.join(HERE, "last-launch.txt")
DEFAULT_PORT = 443
WATCH_SECONDS = 25


# -- small helpers -----------------------------------------------------------

def stamp():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def say(line=""):
    print(line, flush=True)


def _ctx():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE      # the server's certificate is self-signed
    return ctx


def _url(host, port, path="/__local/info"):
    if ":" in host:
        return "https://[%s]:%d%s" % (host, port, path)
    return "https://%s:%d%s" % (host, port, path)


def probe(host, port, timeout=1.5):
    """The /__local/info reply, or None when nothing usable answers there."""
    try:
        with urllib.request.urlopen(_url(host, port), context=_ctx(), timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return None


def info(port):
    for host in ("127.0.0.1", "::1"):
        found = probe(host, port)
        if isinstance(found, dict):
            found["_via"] = host
            return found
    return None


def port_open(host, port, timeout=1.0):
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    try:
        s = socket.socket(family, socket.SOCK_STREAM)
    except OSError:                     # no IPv6 on this machine
        return False
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def _powershell(command, timeout=25):
    exe = shutil.which("powershell") or "powershell"
    try:
        out = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-Command", command],
                             capture_output=True, text=True, timeout=timeout)
        return (out.stdout or "").strip()
    except Exception:
        return ""


def listener_pid(port):
    """The pid listening on a TCP port, on Windows; None elsewhere."""
    if os.name != "nt":
        return None
    out = _powershell("(Get-NetTCPConnection -LocalPort %d -State Listen "
                      "-ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess" % port)
    if out.isdigit():
        return int(out)
    try:
        netstat = subprocess.run(["netstat", "-ano"], capture_output=True, text=True,
                                 timeout=20).stdout
    except Exception:
        return None
    for line in netstat.splitlines():
        fields = line.split()
        if len(fields) >= 5 and fields[0].upper() == "TCP" and fields[-2].upper() == "LISTENING":
            if fields[1].rsplit(":", 1)[-1] == str(port):
                return int(fields[-1])
    return None


def command_line(pid):
    if os.name != "nt" or not pid:
        return ""
    return _powershell("(Get-CimInstance Win32_Process -Filter 'ProcessId=%d' "
                       "-ErrorAction SilentlyContinue).CommandLine" % pid)


def kill(pid):
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    else:
        try:
            os.kill(pid, 9)
        except OSError:
            pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# -- setup -------------------------------------------------------------------

def _useroptions(root):
    """useroptions.py works relative to its own folder; point it at the game instead."""
    sys.path.insert(0, HERE)
    import useroptions
    useroptions.ROOT = root
    useroptions.PATH = os.path.join(root, "UserOptions.dat")
    useroptions.ORIG = useroptions.PATH + ".orig"
    return useroptions


def useroptions_environment(root):
    """What UserOptions.dat currently points the client at."""
    path = os.path.join(root, "UserOptions.dat")
    if not os.path.isfile(path):
        return None
    try:
        text = _useroptions(root).read()
    except Exception as e:
        return "unreadable (%s)" % e
    m = re.search(r'<Param key="Environment">(.*?)</Param>', text)
    return m.group(1) if m else "(not set)"


def point_client_at_localhost(root):
    """Rewrite UserOptions.dat if the game put the real service back.

    Returns (what it said before, what was done). A file that cannot be decoded at all
    is first restored from UserOptions.dat.orig, which useroptions.py keeps: the client
    rewrites this file on every exit, and a half-written one looks like a crash on
    launch from the outside.
    """
    current = useroptions_environment(root)
    if current is None:
        return None, "UserOptions.dat is missing"
    repaired = False
    if str(current).startswith("unreadable"):
        backup = os.path.join(root, "UserOptions.dat.orig")
        if os.path.isfile(backup):
            shutil.copy2(backup, os.path.join(root, "UserOptions.dat"))
            current = useroptions_environment(root)
            repaired = True
    if current == "localhost":
        return current, ("repaired from UserOptions.dat.orig" if repaired else None)
    if any(mark in str(current) for mark in (".orig", "unreadable")):
        return current, None                       # nothing we can safely edit
    useroptions = _useroptions(root)
    useroptions.write(useroptions.set_param(useroptions.read(), "Environment", "localhost"))
    if repaired:
        return current, "repaired from UserOptions.dat.orig and pointed at localhost"
    return current, "rewritten"


GAME_NAME = "TriadWars.exe"
CONFIG = os.path.join(HERE, "game-folder.txt")
_SKIP_DIRS = {"$recycle.bin", "system volume information", "windows", "winsxs",
              "node_modules", ".git", "__pycache__", "logs", "appdata"}


def game_exe(root):
    return os.path.join(root, GAME_NAME)


def game_in(folder):
    return bool(folder) and os.path.isfile(os.path.join(folder, GAME_NAME))


def saved_game_folder():
    """The folder a previous run found, if the exe is still there."""
    try:
        with open(CONFIG, encoding="utf-8") as f:
            folder = f.read().strip()
    except OSError:
        return None
    return folder if game_in(folder) else None


def save_game_folder(folder):
    try:
        with open(CONFIG, "w", encoding="utf-8") as f:
            f.write(os.path.abspath(folder))
    except OSError:
        pass


def _walk_for_game(base, depth, budget):
    """Folders under base that hold TriadWars.exe, without walking the whole disk."""
    hits, seen = [], 0
    stack = [(os.path.abspath(base), 0)]
    while stack and seen < budget:
        folder, level = stack.pop()
        seen += 1
        if game_in(folder):
            hits.append(folder)
            continue
        if level >= depth:
            continue
        try:
            entries = sorted(os.scandir(folder), key=lambda e: e.name.lower())
        except OSError:
            continue
        for entry in entries:
            try:
                if entry.is_dir(follow_symlinks=False) and entry.name.lower() not in _SKIP_DIRS:
                    stack.append((entry.path, level + 1))
            except OSError:
                continue
    return hits


def _steam_libraries():
    libs = []
    for base in filter(None, (os.environ.get("PROGRAMFILES(X86)"),
                              os.environ.get("PROGRAMFILES"))):
        vdf = os.path.join(base, "Steam", "steamapps", "libraryfolders.vdf")
        try:
            with open(vdf, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for match in re.finditer(r'"path"\s*"([^"]+)"', text):
            libs.append(os.path.join(match.group(1).replace("\\\\", "\\"),
                                     "steamapps", "common"))
    return libs


def _registry_installs():
    if os.name != "nt":
        return []
    import winreg
    out = []
    places = [(winreg.HKEY_LOCAL_MACHINE,
               r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
              (winreg.HKEY_LOCAL_MACHINE,
               r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
              (winreg.HKEY_CURRENT_USER,
               r"Software\Microsoft\Windows\CurrentVersion\Uninstall")]
    for hive, path in places:
        try:
            parent = winreg.OpenKey(hive, path)
        except OSError:
            continue
        with parent:
            for index in range(winreg.QueryInfoKey(parent)[0]):
                try:
                    with winreg.OpenKey(parent, winreg.EnumKey(parent, index)) as sub:
                        name = winreg.QueryValueEx(sub, "DisplayName")[0]
                        if "triad" not in str(name).lower():
                            continue
                        try:
                            location = winreg.QueryValueEx(sub, "InstallLocation")[0]
                        except OSError:
                            continue
                        out.append(str(location).strip('"'))
                except OSError:
                    continue
    return out


def search_places(root):
    """Folders worth looking in, nearest first."""
    places = [root, os.path.dirname(os.path.abspath(root))]
    home = os.path.expanduser("~")
    places += [os.path.join(home, name) for name in
               ("Desktop", "Downloads", "Documents", "Games", "Triad Wars", "TriadWars")]
    drive = os.environ.get("SYSTEMDRIVE", "C:")
    places += [os.path.join(drive, "Games")]
    places += [os.environ.get("PROGRAMFILES"), os.environ.get("PROGRAMFILES(X86)"),
               os.environ.get("PROGRAMW6432")]
    places += _steam_libraries()
    places += _registry_installs()
    seen, ordered = set(), []
    for place in places:
        if not place:
            continue
        place = os.path.normpath(place)
        key = os.path.normcase(place)
        if key in seen or not os.path.isdir(place):
            continue
        seen.add(key)
        ordered.append(place)
    return ordered


def locate_game(root, budget=3000):
    """(folder with TriadWars.exe or None, how it was found, where we looked)."""
    if game_in(root):
        return root, "here", []
    saved = saved_game_folder()
    if saved:
        return saved, "remembered", []
    looked = search_places(root)
    hits = []
    for index, place in enumerate(looked):
        # The two nearest places get a real look; Program Files only a shallow one.
        depth = 3 if index < 2 else 1
        hits += _walk_for_game(place, depth, budget)
        if len(hits) >= 5:
            break
    if not hits:
        return None, "", looked
    # Shortest path first, then the most recently touched executable.
    def rank(folder):
        try:
            touched = os.path.getmtime(game_exe(folder))
        except OSError:
            touched = 0
        return (folder.count(os.sep), -touched)
    hits.sort(key=rank)
    return hits[0], "found nearby", looked


def patch_state(root):
    """'original', 'patched', 'unknown' or 'missing' for the game executable."""
    exe = game_exe(root)
    if not os.path.isfile(exe):
        return "missing"
    try:
        sys.path.insert(0, os.path.join(HERE, "tools"))
        import patch_exe
        with open(exe, "rb") as f:
            return patch_exe.state(f.read())
    except Exception:
        return "unknown"


# -- server ------------------------------------------------------------------

def server_state(port):
    """('ours' | 'stale' | 'other' | 'free', info)."""
    found = info(port)
    if found is not None:
        try:
            same = abs(float(found.get("code", {}).get("server", 0))
                       - os.path.getmtime(os.path.join(HERE, "server.py"))) < 2.0
        except (TypeError, ValueError):
            same = False
        return ("ours" if same else "stale"), found
    if port_open("127.0.0.1", port) or port_open("::1", port):
        return "other", None
    return "free", None


def start_server(port, safe):
    os.makedirs(LOG_DIR, exist_ok=True)
    env = dict(os.environ)
    if safe:
        env["TW_SAFE_MODE"] = "1"
    with open(CONSOLE, "a", encoding="utf-8", errors="replace") as log:
        log.write("\n===== %s starting server (python %s, safe=%s) =====\n"
                  % (stamp(), sys.version.split()[0], safe))
    log = open(CONSOLE, "a", encoding="utf-8", errors="replace")
    kwargs = {}
    if os.name == "nt":
        # Detached, so the server outlives this window - as it did before.
        kwargs["creationflags"] = 0x00000008 | 0x00000200      # DETACHED | NEW_PROCESS_GROUP
    proc = subprocess.Popen([sys.executable, "server.py", "--port", str(port)],
                            cwd=HERE, stdout=log, stderr=subprocess.STDOUT, env=env, **kwargs)
    deadline = time.time() + 25
    while time.time() < deadline:
        if proc.poll() is not None:
            return proc, None
        found = info(port)
        if found is not None:
            return proc, found
        time.sleep(0.4)
    return proc, info(port)


def ensure_server(port, safe):
    """Make sure the server answering on the port is this build. Returns (info, note)."""
    state, found = server_state(port)
    if state == "ours":
        return found, "server already running (this build)"
    if state in ("stale", "other"):
        pid = listener_pid(port)
        line = command_line(pid) if pid else ""
        # A stale answer can only come from this server's own /__local/info route, so a
        # python running server.py on the port is ours to replace. Anything that does
        # not look like that is left alone and reported.
        ours = bool(pid) and (not line or re.search(r'(^|[\s"\'=])server\.py($|[\s"\'])', line))
        if state == "stale" and ours:
            say("Replacing the server left running from an older copy (pid %d)." % pid)
            kill(pid)
            time.sleep(0.8)
        else:
            what = line or ("pid %d" % pid if pid else "something")
            raise SystemExit(
                "Port %d is already in use by another program (%s).\n"
                "Close it, or tell it to stop listening on %d, and try again."
                % (port, what, port))
    proc, found = start_server(port, safe)
    if found is None:
        tail = console_tail(25)
        raise SystemExit("The local server did not start.\n\n" +
                         (tail or "(the server wrote nothing)") +
                         "\n\nIts output is in LocalServer\\logs\\console.log")
    return found, "server started"


def answers_the_game(port):
    """True when the server answers storeaccess the way the client needs."""
    try:
        # ?launcher=1 keeps this out of the client's request count in the report.
        with urllib.request.urlopen(_url("127.0.0.1", port,
                                         "/game/api/v1/storeaccess?launcher=1"),
                                    context=_ctx(), timeout=5) as r:
            return json.loads(r.read().decode("utf-8", "replace")) == {"mHasAccess": True}
    except Exception:
        return False


def console_tail(lines=25):
    try:
        with open(CONSOLE, encoding="utf-8", errors="replace") as f:
            return "\n".join(f.read().splitlines()[-lines:])
    except OSError:
        return ""


# -- evidence ----------------------------------------------------------------

def request_lines(info_dict, lines=25):
    """The last requests the server logged for this run."""
    name = (info_dict or {}).get("log") or ""
    path = os.path.join(LOG_DIR, name) if name else ""
    if not path or not os.path.isfile(path):
        return [], 0
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read().splitlines()
    requests = [ln for ln in text if any(m in ln for m in (" GET /", " POST /", " PUT /",
                                                          " DELETE /"))
                and "/__local/info" not in ln and "launcher=1" not in ln]
    return requests[-lines:], len(requests)


def windows_errors(since, limit=5):
    """Application-log entries that mention the game, as text."""
    if os.name != "nt":
        return ""
    script = (
        "$s = [datetime]::Now.AddMinutes(-15)\n"
        "Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$s} "
        "-ErrorAction SilentlyContinue |\n"
        "  Where-Object { $_.Message -match 'TriadWars' } | Select-Object -First %d |\n"
        "  ForEach-Object { '--- ' + $_.TimeCreated.ToString('HH:mm:ss') + ' ' + "
        "$_.ProviderName + ' (' + $_.Id + ')'; $_.Message }\n" % limit)
    with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False, encoding="utf-8") as f:
        f.write(script)
        path = f.name
    try:
        exe = shutil.which("powershell") or "powershell"
        out = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                              "-File", path], capture_output=True, text=True, timeout=40)
        return (out.stdout or "").strip()
    except Exception as e:
        return "(could not read the Windows event log: %s)" % e
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def crash_files(root, since):
    found = []
    try:
        names = os.listdir(root)
    except OSError:
        return found
    for name in names:
        path = os.path.join(root, name)
        if not os.path.isfile(path):
            continue
        if not (name.lower().endswith((".dmp", ".mdmp", ".wer"))
                or name.lower().startswith("triadwars.exe.")):
            continue
        try:
            if os.path.getmtime(path) >= since - 60:
                found.append("%s (%d bytes)" % (name, os.path.getsize(path)))
        except OSError:
            pass
    return found


def build_report(root, port, found, note, exit_code=None, alive=None, safe=False,
                 looked=None):
    env = useroptions_environment(root)
    exe = game_exe(root)
    requests, total = request_lines(found)
    lines = []
    add = lines.append

    add("Triad Wars launch report")
    add("=" * 60)
    add("Generated      : " + stamp())
    if exit_code is not None:
        add("The game       : exited after %s s with code %s" % (alive, exit_code))
    else:
        add("The game       : not started by this run" if alive is None
            else "The game       : still running after %s s" % alive)
    add("Server         : %s" % (note or "?"))
    if found:
        add("                 pid %s, listening on port %s, started %s, via %s"
            % (found.get("pid"), found.get("port"), found.get("started"), found.get("_via")))
        code = found.get("code", {})
        try:
            same = abs(float(code.get("server", 0))
                       - os.path.getmtime(os.path.join(HERE, "server.py"))) < 2.0
        except (TypeError, ValueError):
            same = False
        add("                 code: %s (%s this copy)" % (
            datetime.datetime.fromtimestamp(code.get("server") or 0).strftime("%Y-%m-%d %H:%M"),
            "same as" if same else "different from"))
        add("                 features: %s" % json.dumps(found.get("features", {})))
        add("                 log: %s (%d request(s) this run)" % (found.get("log"), total))
    add("Safe mode      : %s" % ("on - the new screens were switched off" if safe else "off"))
    add("")
    def row(label, value):
        add("%-16s %s" % ((label + ":").ljust(16), value))

    add("Setup")
    add("-" * 60)
    row("Game folder", root)
    row("Python", "%s (%s)" % (sys.version.split()[0], sys.executable))
    row("UserOptions.dat", "Environment = %s" % (env if env is not None
                                                 else "(file not found)"))
    if os.path.isfile(exe):
        row("TriadWars.exe", "%d bytes, %s"
            % (os.path.getsize(exe),
               datetime.datetime.fromtimestamp(os.path.getmtime(exe)).strftime("%Y-%m-%d %H:%M")))
        row("  sha256", sha256(exe))
        row("Crash patch", patch_state(root))
    else:
        row("TriadWars.exe", "NOT FOUND")
        if looked:
            row("", "looked in: " + ", ".join(looked[:6])
                + (" ..." if len(looked) > 6 else ""))
    row("Port %d" % port, "%s (IPv4), %s (IPv6)"
        % ("answering" if port_open("127.0.0.1", port) else "closed",
           "answering" if port_open("::1", port) else "closed"))
    dumps = crash_files(root, time.time() - 3600)
    row("Crash files", ", ".join(dumps) if dumps else "none in the last hour")
    row("Answers game", "yes" if answers_the_game(port) else "NO - see the server output")
    add("")
    add("What to look at first")
    add("-" * 60)
    if exit_code is not None and total == 0:
        add("The client exited without ever contacting the server. That rules out every")
        add("reply shape: this is the executable, its files, or the connection. Check the")
        add("entry in the Windows error log below - the faulting module names the culprit.")
        add("If it is the game's own graphics or a DLL, the crash patch and a fresh copy of")
        add("the game folder are the two things to try; if it is python, the server died.")
    elif exit_code is not None:
        add("The client talked to the server and then exited. The last requests below are")
        add("the screen it died on; the Windows entry names the module and offset.")
        add("Run 'Play-TriadWars.bat safe' to repeat the launch with the new screens off -")
        add("if it survives, the reply for one of them is the cause.")
    else:
        add("Nothing died. This report only matters when the game exits during launch.")
    add("")
    if requests:
        add("Last requests from the client")
        add("-" * 60)
        lines.extend("  " + ln for ln in requests)
        add("")
    add("Windows application log (entries mentioning TriadWars)")
    add("-" * 60)
    lines.extend("  " + ln for ln in (windows_errors(time.time()) or "(nothing found)").splitlines())
    add("")
    add("Server output (last 25 lines of logs\\console.log)")
    add("-" * 60)
    lines.extend("  " + ln for ln in (console_tail(25) or "(empty)").splitlines())
    add("")
    add("Send this file as it is; it only contains the paths above and the game's exit")
    add("code.")
    return "\n".join(lines)


def notify(title, text):
    if os.name != "nt":
        return
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, text, title, 0x10)
    except Exception:
        pass


def missing_game_message(root, looked):
    places = "\n".join("  " + p for p in looked[:8]) or "  (nothing to search)"
    return (
        "This folder holds the local server, not the game.\n\n"
        "TriadWars.exe is not in\n  %s\n\n"
        "and I could not find it in the usual places either:\n%s\n\n"
        "This zip is about 2 MB - it is only the server. TriadWars.exe is the game\n"
        "itself (roughly 1.5 GB). Extract this zip *inside the folder that already\n"
        "holds TriadWars.exe*, then run Play-TriadWars.bat again.\n\n"
        "If the game is somewhere unusual, point the launcher at it once:\n"
        "  python \"%s\" --root \"D:\\path\\to\\TriadWars\"\n"
        "or drag that folder onto Play-TriadWars.bat. The launcher remembers it\n"
        "in LocalServer\\game-folder.txt from then on."
        % (root, places, os.path.join(HERE, "launch.py")))


# -- modes -------------------------------------------------------------------

def diagnose(root, port, safe):
    state, found = server_state(port)
    note = {"ours": "running (this build)", "stale": "running (an older build)",
            "other": "another program holds the port", "free": "not running"}[state]
    folder, how, looked = locate_game(root)
    if folder:
        root = folder
    report = build_report(root, port, found, note, safe=safe, looked=looked)
    with open(REPORT, "w", encoding="utf-8") as f:
        f.write(report + "\n")
    say(report)
    say("\nWritten to " + REPORT)
    return 0 if state in ("ours", "free") and os.path.isfile(game_exe(root)) else 1


def play(root, port, safe, watch, game=None):
    say("Triad Wars local server - launcher")
    say("")
    if not game:
        folder, how, looked = locate_game(root)
        if folder is None:
            raise SystemExit(missing_game_message(root, looked))
        if os.path.abspath(folder) != os.path.abspath(root):
            say("The game is not in this folder; using %s (%s)." % (folder, how))
            say("(Extracting the zip there would keep everything together.)")
            say("")
            if how == "found nearby":
                save_game_folder(folder)
        root = folder

    before, note = point_client_at_localhost(root)
    if before is None:
        say("! UserOptions.dat was not found. Start the game once, close it, and run this "
            "again: the game writes that file on its first run.")
    elif note == "rewritten":
        say("UserOptions.dat pointed at %s; set it to localhost again." % before)

    found, note = ensure_server(port, safe)
    say("Server: pid %s, port %s, %s" % (found.get("pid"), found.get("port"), note))
    if not answers_the_game(port):
        raise SystemExit("The server is listening but did not answer the game's own "
                         "storeaccess request.\nIts output is in LocalServer\\logs\\console.log.")
    if safe:
        say("Safe mode: jobs, challenges, exports and production are switched off.")

    exe = game or game_exe(root)
    say("Starting %s ..." % os.path.basename(exe))
    started = time.time()
    proc = subprocess.Popen([exe], cwd=root)
    try:
        code = proc.wait(timeout=watch)
    except subprocess.TimeoutExpired:
        say("The game is up and still running - leaving it to you. (Server keeps running.)")
        return 0

    alive = round(time.time() - started, 1)
    crashed = code != 0 or alive < 10
    report = build_report(root, port, found, note, exit_code=code, alive=alive, safe=safe)
    with open(REPORT, "w", encoding="utf-8") as f:
        f.write(report + "\n")
    head = "\n".join(report.splitlines()[:9])
    say("")
    say("The game closed after %s s (exit code %s).%s"
        % (alive, code, "" if crashed else " If that was you, nothing is wrong."))
    say("")
    say(head)
    say("")
    say("Full report: " + REPORT)
    if crashed:
        notify("Triad Wars closed on launch",
               "The game exited after %s s (code %s).\n\n%s\n\nFull report:\n%s"
               % (alive, code, head, REPORT))
    return 1 if crashed else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Start the server and the game.")
    ap.add_argument("mode", nargs="?", default="play",
                    choices=["play", "diagnose", "server"],
                    help="play = start everything; diagnose = report only; "
                         "server = start the server and exit")
    ap.add_argument("--root", default=ROOT, help="the game folder (default: this zip's root)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--safe", action="store_true",
                    help="start with the new screens switched off")
    ap.add_argument("--watch", type=int, default=WATCH_SECONDS,
                    help="seconds to watch the game before leaving it alone")
    ap.add_argument("--game", default=None, help="executable to start instead of TriadWars.exe")
    argv = list(sys.argv[1:] if argv is None else argv)
    for arg in list(argv):
        if not arg.startswith("-") and os.path.isdir(arg):
            argv.remove(arg)                       # dropping the game folder on the .bat
            argv += ["--root", arg]
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    if game_in(root):
        save_game_folder(root)

    if args.mode == "diagnose":
        return diagnose(root, args.port, args.safe)
    if args.mode == "server":
        found, note = ensure_server(args.port, args.safe)
        say("Server: pid %s, port %s, %s" % (found.get("pid"), found.get("port"), note))
        return 0
    return play(root, args.port, args.safe, args.watch, args.game)


if __name__ == "__main__":
    sys.exit(main())
