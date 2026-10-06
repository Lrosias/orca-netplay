#!/usr/bin/env python3
# xplat.py: one side of a cross-machine Orca match over prod YouGame rooms, as the desktop app
# runs it: real tickets (a signed-in account's cookie through fake_bridge.py), YouGame's keyframe
# store, scripted inputs, headless and muted. Run the host on one machine and the joiner on the
# other with the same --room; each prints a JSON summary and exits 0 only if it saw no error and
# no desync, and (joiner) plugged in and matched checksums.
#
#   host:  xplat.py --role host --room xp1234ab --bin <nogui> --disc <rsbe01 rev2> --cookie <file>
#   join:  xplat.py --role join --room xp1234ab --bin <nogui> --disc <...> --cookie <file> --delay 60
#   P+:    add --pplus "<dir>/Project+ Netplay Launcher.dol" (and --slug project-rollback) on both.
#
# --cookie is a file holding a session cookie header (sandbox-ada on one side, sandbox-bo on the
# other; both are testers on open-brawl and project-rollback): web/scripts/online-tests/lib.mjs
# roster("ada").cookie. It is never printed. --frames is how long the host runs (frames at 60 Hz);
# the joiner starts --delay seconds after launch and leaves --leave-at frames in (0: stays until
# the host ends). Inputs default to Tools/orca/inputs/bf-mario-link-results.txt (Brawl) or
# pplus-1v1.txt (P+): each side plays its own port's rules.
import argparse, json, os, re, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
p = argparse.ArgumentParser()
p.add_argument("--role", choices=["host", "join"], required=True)
p.add_argument("--room", required=True)
p.add_argument("--bin", required=True)
p.add_argument("--disc", required=True)
p.add_argument("--cookie", required=True)
p.add_argument("--pplus")
p.add_argument("--slug")
p.add_argument("--frames", type=int, default=36000)
p.add_argument("--delay", type=float, default=0)
p.add_argument("--leave-at", type=int, default=0)
p.add_argument("--input")
p.add_argument("--name")
p.add_argument("--keep", action="store_true")
a = p.parse_args()
if not re.fullmatch(r"[a-z0-9]{6,12}", a.room):
    sys.exit("room: 6-12 of a-z0-9")
slug = a.slug or ("project-rollback" if a.pplus else "open-brawl")
inp = a.input or os.path.join(HERE, "inputs", "pplus-1v1.txt" if a.pplus else "bf-mario-link-results.txt")
work = tempfile.mkdtemp(prefix=f"orca-xplat-{a.role}-")
T0 = time.time()


def log(s):
    print(f"[{time.time() - T0:7.1f}] {s}", flush=True)


if a.delay:
    log(f"waiting {a.delay:.0f} s")
    time.sleep(a.delay)

with open(a.cookie) as f:
    cookie = f.read().strip()
benv = dict(os.environ, FAKE_ROOM=a.room, FAKE_LOG=os.path.join(work, "bridge.log"), FAKE_SLUG=slug,
            FAKE_TICKET_SLUG=slug, FAKE_NO_LEAVE="1", FAKE_COOKIE=cookie)
# An idle controller stream: the scripted inputs drive this player.
idle = os.path.join(work, "idle-pad.jsonl")
with open(idle, "w") as f:
    f.write('{"t": 0, "buttons": 0}\n')
benv["FAKE_PAD_SCRIPT"] = idle
bridge = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_bridge.py")], env=benv,
                          stdout=subprocess.PIPE, stderr=open(os.path.join(work, "bridge.err"), "w"),
                          text=True)
port = bridge.stdout.readline().strip()
if not port.isdigit():
    sys.exit(f"fake_bridge didn't start (see {work}/bridge.err)")

env = {k: v for k, v in os.environ.items() if k in ("HOME", "PATH", "SYSTEMROOT", "TEMP", "TMP",
                                                    "USERPROFILE", "APPDATA", "LOCALAPPDATA")}
# Test overrides pass through (ORCA_TEST_NO_AFP=1 on both machines plays an M1-class Mac against a
# PC; any test override marks both keys as test sessions, so they still meet), and so do the direct
# link's switches (ORCA_DIRECT=0, ORCA_DIRECT_FORCE=turn, ...; ORCA.md "Direct links").
env.update({k: v for k, v in os.environ.items()
            if k.startswith("ORCA_TEST_") or k.startswith("ORCA_DIRECT")})
env.update(ORCA_SESSION="1", ORCA_ROOM=a.room, ORCA_NAME=a.name or a.role.capitalize(),
           YOUGAME_BRIDGE=f"http://127.0.0.1:{port}", YOUGAME_TOKEN="t0ken", YOUGAME_GAME=slug,
           ORCA_TEST_COMMANDS="1", YG_SCENES="1", YG_INPUT=inp)
if a.role == "join":
    env["ORCA_JOIN"] = "1"
    if a.leave_at:
        env["ORCA_TEST_LEAVE_AT"] = str(a.leave_at)
    # Ends a little before the host does (frames are the host's once the keyframe is in), so the
    # check measures the match, not the goodbye.
    env["YG_EXIT_AFTER"] = str(max(a.frames - 600, 1))
else:
    env["YG_EXIT_AFTER"] = str(a.frames)
args = [a.bin, "-p", "headless", "-u", os.path.join(work, "user"), "-v", "Null",
        "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", "Logger.Logs.ROLLBACK=True",
        "-C", "Logger.Logs.NETPLAY=True", "-C", "Logger.Options.Verbosity=2"]
if a.pplus:
    env["ORCA_PROFILE"] = "PPLUS32"
    args += ["-C", f"Dolphin.Core.DefaultISO={a.disc}", "-e", a.pplus]
else:
    args += ["-e", a.disc]

errlog = open(os.path.join(work, "orca.log"), "w")
statlog = open(os.path.join(work, "stats.jsonl"), "w")
# UTF-8 explicitly: Orca prints UTF-8, and Windows' default text encoding is the ANSI code page.
orca = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errlog,
                        text=True, bufsize=1, encoding="utf-8", errors="replace")
states, errors, stats, out_lines = [], [], [], []


def reader():
    for line in orca.stdout:
        line = line.rstrip("\n")
        out_lines.append(line)
        if line.startswith("orca caps"):
            orca.stdin.write("caps join leave stats\n")
            orca.stdin.flush()
        if line.startswith("orca stats "):
            stats.append(json.loads(line[len("orca stats "):]))
            statlog.write(json.dumps({"t": round(time.time(), 3), **stats[-1]}) + "\n")
            statlog.flush()
            continue
        if line.startswith("orca state "):
            st = line[len("orca state "):]
            if not st.startswith("joining ") or st in ("joining 0", "joining 50", "joining 99"):
                log(line)
            states.append(st)
        elif line.startswith("orca error"):
            log(line)
            errors.append(line)
        else:
            log(line)


threading.Thread(target=reader, daemon=True).start()
code = orca.wait()
bridge.terminate()
errlog.close()

# Orca's log: stderr on macOS; on Windows it goes to stdout when that is redirected.
text = open(os.path.join(work, "orca.log"), encoding="utf-8", errors="replace").read()
text += "\n" + "\n".join(out_lines)
ends = re.findall(r"Online match ([^:]*): frame (\d+), (\d+) rollbacks, (\d+) re-run frames, deepest (\d+), "
                  r"(\d+) stalls \((\d+) counted\).*?input delay (\d+).*?, (\d+) checksums matched", text)
matched = max((int(e[8]) for e in ends), default=0)
peers = [s for s in stats if s.get("peers", 0) > 0]
fps = [s["fps"] for s in peers if "fps" in s]
summary = {
    "role": a.role, "room": a.room, "slug": slug, "exit": code, "errors": errors,
    "desync": (any("desync" in e for e in errors) or "Desync" in text
               or any("desync" in st for st in states) or "the session failed" in text),
    "plugged": "plugged into port" in text or any(s.startswith("friend-joining") for s in states),
    "checksums_matched": matched,
    "seconds_with_peer": len(peers),
    "fps_min": min(fps, default=None), "fps_avg": round(sum(fps) / len(fps), 1) if fps else None,
    "rollbacks": sum(s.get("rb", 0) for s in peers), "stalls": sum(s.get("st", 0) for s in peers),
    "stall_ms": sum(s.get("stms", 0) for s in peers),
    # The latest single round trip to the relay, averaged per second (spikes pull it up), and the
    # medians the delay's link part follows: this side's and the friend's, as last reported.
    "ping_avg": round(sum(s.get("ping", 0) for s in peers) / len(peers)) if peers else None,
    "ping_median": peers[-1].get("pmed") if peers else None,
    "friend_ping_median": peers[-1].get("fpmed") if peers else None,
    "delay_max": max((s.get("delay", 0) for s in peers), default=None),
    "hitches": max((s.get("hi", 0) for s in stats), default=0),
    # How the friend's inputs came: seconds on each path ("tx": 0 relay, 1 direct, 2 TURN), and
    # the direct link's median round trip while it carried them.
    "transport_seconds": {name: sum(1 for s in peers if s.get("tx", 0) == k)
                          for k, name in ((0, "relay"), (1, "direct"), (2, "turn"))},
    "transport": max(((sum(1 for s in peers if s.get("tx", 0) == k), name)
                      for k, name in ((0, "relay"), (1, "direct"), (2, "turn"))),
                     default=(0, "relay"))[1] if peers else None,
    "link_rtt_median": (sorted(s["lrtt"] for s in peers if s.get("tx", 0) > 0 and s.get("lrtt", -1) >= 0)
                        or [None])[len([1 for s in peers if s.get("tx", 0) > 0 and s.get("lrtt", -1) >= 0]) // 2],
    "direct_lines": [l[l.find("Orca direct link"):] for l in text.splitlines()
                     if "Orca direct link: seat" in l][:8],
    "peer_hitches": max((s.get("phi", 0) for s in stats), default=0),
    "last_lines": [l for l in text.splitlines() if "Online match" in l][-3:],
    # Every change of this side's input delay, with why (Session.cpp, "Input delay").
    "delay_changes": [l[l.find("input delay"):] for l in text.splitlines()
                      if "Online match: input delay" in l][:20],
    "work": work,
}
ok = (code == 0 and not errors and not summary["desync"] and summary["checksums_matched"] > 0)
summary["ok"] = ok
print("XPLAT " + json.dumps(summary), flush=True)
if ok and not a.keep:
    import shutil
    shutil.rmtree(os.path.join(work, "user"), ignore_errors=True)
sys.exit(0 if ok else 1)
