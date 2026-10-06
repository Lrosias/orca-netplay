// Two DolphinNoGUI on this machine play the scripted Brawl match against each other through a
// yougame.co room (a dev game, no sign-in), with the env-gated measurements in Rollback/Diag.h.
// Both share this machine's CPU and memory bandwidth, so absolute numbers are pessimistic.
//
//   DISC=<RSBE01 rev 2 image> [DOLPHIN=<DolphinNoGUI>] node Tools/orca/selfmatch.mjs <label> [frames] [KEY=VAL ...]
//
// Defaults: 3000 frames, muted, Null video, YG_FRAMETIME=300 and YG_SNAPTIME=1 on both sides.
// The fight starts at frame 5995: pass 9000 and YG_FRAMETIME=6000 to time it.
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");
const [label = "self", frames = "3000", ...pairs] = process.argv.slice(2);
const dolphin =
  process.env.DOLPHIN ||
  [
    path.join(repo, "build", "release", "x64", "Binaries", "DolphinNoGUI.exe"),
    path.join(repo, "build", "Binaries", "DolphinNoGUI"),
  ].find((p) => fs.existsSync(p));
const disc = process.env.DISC;
if (!dolphin || !disc) {
  console.error("Set DISC to the Brawl image (and DOLPHIN if the build is not in the default place).");
  process.exit(2);
}
// Room codes are 6 to 12 lowercase letters and digits.
const room = `self${Date.now() % 10000000}`;

function start(name, delayMs) {
  const user = path.join(os.tmpdir(), `orca-self-${name}`);
  fs.rmSync(user, { recursive: true, force: true });
  fs.mkdirSync(user, { recursive: true });
  const env = { ...process.env };
  for (const k of Object.keys(env)) if (k.startsWith("ORCA_") || k.startsWith("YG_")) delete env[k];
  Object.assign(env, {
    ORCA_SESSION: "1",
    ORCA_TEST_DEV_GAME: "orca-selftest",
    ORCA_ROOM: room,
    ORCA_NAME: name,
    YG_INPUT: path.join(here, "inputs", "bf-mario-link-results.txt"),
    YG_EXIT_AFTER: frames,
    YG_FRAMETIME: "300",
    YG_SNAPTIME: "1",
  });
  for (const p of pairs) {
    const i = p.indexOf("=");
    env[p.slice(0, i)] = p.slice(i + 1);
  }
  const args = [
    "-p", "headless", "-u", user, "-v", "Null", "-C", "Dolphin.DSP.Backend=No Audio Output",
    "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True", "-C", "Logger.Options.Verbosity=2",
    "-e", disc,
  ];
  return new Promise((resolve) =>
    setTimeout(() => {
      const started = Date.now();
      const child = spawn(dolphin, args, { env, stdio: ["ignore", "pipe", "pipe"] });
      let out = "";
      child.stdout.on("data", (d) => (out += d));
      child.stderr.on("data", (d) => (out += d));
      child.on("exit", (code) => {
        fs.writeFileSync(path.join(os.tmpdir(), `orca-self-${label}-${name}.log`), out);
        fs.rmSync(user, { recursive: true, force: true });
        resolve({ name, code, seconds: (Date.now() - started) / 1000, out });
      });
    }, delayMs)
  );
}

const results = await Promise.all([start("A", 0), start("B", 3000)]);
for (const r of results) {
  console.log(`[${label} ${r.name}] exit ${r.code}, ${r.seconds.toFixed(1)} s`);
  for (const line of r.out.split(/\r?\n/)) {
    if (/Diag |Online match at exit|match started|orca error|Desync|E\[/.test(line))
      console.log("  " + line.replace(/^\d+:\d+:\d+ \S+ /, ""));
  }
}
