#!/usr/bin/env node
// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
//
// page-e2e.mjs: YouGame's real page drives two headless Orcas through the queue (ORCA.md
// "Matchmaking"). Unlike queue-e2e.mjs, which plays the page's part itself, this opens the live
// listing in two Playwright browser contexts signed in as two players. Each page gets a stand-in
// for the desktop app's window.yougameDesktop that follows the app's rules for what passes between
// page and Orca. This script only presses the pad and reads the band under the game.
//
//   node Tools/orca/page-e2e.mjs <dolphin-emu-nogui> <disc> --playwright <playwright/index.mjs>
//       --slug <listing> --cookie-a <file> --cookie-b <file> [--queue ranked|casual]
//       [--site <url>] [--pplus <launcher.dol>] [--alarm <s>] [--channel chrome] [--keep]
//
// The cookie files hold a session's Cookie header (mode 600, never printed). Orca runs nice'd,
// muted, with Null video and a perl alarm. On a shared machine, don't run it while someone is
// playing or the load is high.
import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";

const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : fallback;
};
const [BIN, DISC] = argv;
const PLAYWRIGHT = opt("--playwright");
const SLUG = opt("--slug");
if (!BIN || !DISC || !PLAYWRIGHT || !SLUG) {
  console.error(
    "usage: page-e2e.mjs <nogui> <disc> --playwright <path> --slug <listing> --cookie-a <f> --cookie-b <f> ...",
  );
  process.exit(2);
}
const { chromium } = await import(PLAYWRIGHT);
const PPLUS = opt("--pplus");
const QUEUE = opt("--queue", "ranked");
const SITE = (opt("--site", "https://yougame.co") || "").replace(/\/$/, "");
const KEEP = argv.includes("--keep");
// match (default): the whole flow. cancel: one player cancels from the band, then backs out of
// the character select while searching. noshow: the opponent's app closes as soon as they are
// paired, and the host's page gives up after 30 s.
const SCENARIO = opt("--scenario", "match");
const ALARM = Number(opt("--alarm", "1200"));
const APP_VERSION = opt("--app-version", "0.12.18");
const WORK = fs.mkdtempSync(path.join(os.tmpdir(), "orca-page-e2e-"));
const T0 = Date.now();

const log = (text) =>
  console.log(`[${((Date.now() - T0) / 1000).toFixed(1).padStart(6)}] ${text}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = null;
function fail(why) {
  if (!failed) {
    failed = why;
    log(`FAIL: ${why}`);
  }
  throw new Error(why);
}

// The desktop app's rules (desktop/src/orca-policy.ts): what a page line may be, and which need a
// feature Orca turned on.
const ORCA_COMMAND =
  /^(rect \d{1,5} \d{1,5} \d{1,5} \d{1,5}|hide|show|focus|blur|pause|resume|reset|prepare-join|leave|join [a-z0-9]{6,12}|host [a-z0-9]{6,12}|queue-cancel|ping (on|off)|delay (auto|[1-6])|perf (off|fps|detailed)|caps( [a-z][a-z0-9-]{0,23}){0,16}|volume (0(\.\d{1,3})?|1(\.0{1,3})?|\.\d{1,3}))$/;
const APP_FEATURES = [
  "join",
  "leave",
  "stats",
  "host",
  "pause",
  "resolution",
  "delay",
  "perf",
];
const CAP_RE = /^[a-z][a-z0-9-]{0,23}$/;
function featureAllows(line, agreed) {
  if (line === "leave") return agreed.has("leave");
  if (line.startsWith("join ")) return agreed.has("join");
  if (line.startsWith("host ") || line === "queue-cancel")
    return agreed.has("host");
  if (line === "pause" || line === "resume") return agreed.has("pause");
  if (line.startsWith("delay ")) return agreed.has("delay");
  if (line.startsWith("perf ")) return agreed.has("perf");
  return true;
}
// The embedded window's own lines: a headless Orca has no window to move.
const WINDOW_LINE = /^(rect |hide$|show$|focus$|blur$|volume )/;

// GameCube adapter wire mask (desktop/src/controllers.ts).
const BTN = {
  A: 1,
  B: 2,
  X: 4,
  Y: 8,
  LEFT: 16,
  RIGHT: 32,
  DOWN: 64,
  UP: 128,
  START: 256,
  Z: 512,
};

// The listing's accepted disc, as the app's disc store would report it.
const DISC_INFO = {
  key: "wii/super-smash-bros-brawl-usa",
  sha1: "59432f150bf6b871ab378bb5b28e92005e0862f6",
  name: "Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs",
  size: 8511160320,
  storedAt: Date.now(),
  grants: [SLUG],
};

// The preload's window.yougameDesktop, its calls forwarded to this script (__ygCall) and its events
// fed back (__ygEmit for onProgress, __ygSdk for onSdkRequest).
function initScript({ disc, features }) {
  const progress = new Set();
  let sdk = null;
  const call = (name, arg) => window.__ygCall(name, arg);
  window.__ygEmit = (p) => {
    for (const cb of [...progress])
      try {
        cb(p);
      } catch (e) {
        console.error("onProgress", e);
      }
  };
  window.__ygSdk = (req) => {
    if (!sdk) return false;
    sdk(req);
    return true;
  };
  window.yougameDesktop = {
    orca: {
      available: true,
      features,
      play: (launch) => call("play", launch),
      send: (slug, session, line) => call("send", { slug, session, line }),
      stop: (slug, session) => call("stop", { slug, session }),
      cancel: () => Promise.resolve(),
      pad: () => {},
      displayHz: () => Promise.resolve(60),
      onDisplayHz: () => () => {},
    },
    discs: {
      list: () => Promise.resolve([disc]),
      pick: () => Promise.resolve({ error: "not in this test" }),
      grant: () => Promise.resolve(),
      revoke: () => Promise.resolve(),
      remove: () => Promise.resolve(),
    },
    info: () =>
      Promise.resolve({
        version: "__VERSION__",
        platform: "macos",
        arch: "arm64",
      }),
    installed: () => Promise.resolve([]),
    play: () => Promise.reject(new Error("no native builds in this test")),
    cancel: () => Promise.resolve(),
    remove: () => Promise.resolve(),
    keep: () => Promise.resolve(),
    reveal: () => Promise.resolve(),
    onProgress: (cb) => {
      progress.add(cb);
      return () => progress.delete(cb);
    },
    onSdkRequest: (cb) => {
      sdk = cb;
      return () => {
        if (sdk === cb) sdk = null;
      };
    },
    sdkReply: (id, answer) => void call("sdkReply", { id, answer }),
    sdkPush: (event) => void call("sdkPush", event),
    authLinks: true,
  };
}

class Side {
  constructor(name, cookieFile) {
    this.name = name;
    this.cookie = fs.readFileSync(cookieFile, "utf8").trim();
    this.token = crypto.randomBytes(16).toString("hex");
    this.lines = [];
    this.waiters = [];
    this.sent = [];
    this.scene = "";
    this.sceneWaiters = [];
    this.pad = { buttons: 0, axes: [128, 128, 128, 128] };
    this.results = [];
    this.bands = [];
    this.events = new Set();
    this.pending = new Map();
    this.offered = [];
    this.agreed = new Set();
    this.emitChain = Promise.resolve();
    this.proc = null;
  }

  async startBridge() {
    this.server = http.createServer((req, res) => this.serve(req, res));
    await new Promise((r) => this.server.listen(0, "127.0.0.1", r));
    this.port = this.server.address().port;
  }

  serve(req, res) {
    if (req.headers.authorization !== `Bearer ${this.token}`) {
      res.writeHead(401).end();
      return;
    }
    if (req.method === "GET" && req.url === "/events") {
      res.writeHead(200, { "content-type": "text/event-stream" });
      res.write(": connected\n\n");
      this.events.add(res);
      const keep = setInterval(() => res.write(": ping\n\n"), 15000);
      req.on("close", () => {
        clearInterval(keep);
        this.events.delete(res);
      });
      return;
    }
    if (req.method === "GET" && req.url === "/controllers/events") {
      res.writeHead(200, { "content-type": "text/event-stream" });
      const session = crypto.randomBytes(4).toString("hex");
      let seq = 0;
      const tick = setInterval(() => {
        const now = Date.now();
        const ports = [0, 1, 2, 3].map((i) => ({
          adapterId: `${session}-gc`,
          port: i,
          seat: i,
          connected: i === 0,
          type: i === 0 ? "wired" : null,
          buttons: i === 0 ? this.pad.buttons : 0,
          axes: i === 0 ? this.pad.axes : [128, 128, 128, 128],
          triggers: [0, 0],
          origin: [128, 128, 128, 128, 0, 0],
          calibrationRevision: 0,
        }));
        const snapshot = {
          schema: 1,
          session,
          sequence: ++seq,
          sentAt: now,
          receivedAt: now,
          source: "native",
          state: "connected",
          message: "",
          owned: true,
          suspended: false,
          ports,
          pads: [],
        };
        res.write(
          `data: ${JSON.stringify({ type: "controllers", snapshot })}\n\n`,
        );
      }, 8);
      req.on("close", () => clearInterval(tick));
      return;
    }
    if (req.method === "POST" && req.url === "/sdk") {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", async () => {
        let msg;
        try {
          msg = JSON.parse(body);
        } catch {
          res.writeHead(400).end();
          return;
        }
        const answer = await this.ask({ ...msg, slug: SLUG });
        res
          .writeHead(answer.ok ? 200 : 400, {
            "content-type": "application/json",
          })
          .end(JSON.stringify(answer));
      });
      return;
    }
    res.writeHead(404).end();
  }

  // Orca's SDK request, answered by the page (desktop main.ts: yg:sdk-request, yg:sdk-reply).
  async ask(msg) {
    const id = crypto.randomUUID();
    const answer = new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        resolve({ ok: false, error: "the YouGame window did not answer" });
      }, 30000);
      this.pending.set(id, (a) => {
        clearTimeout(timer);
        resolve(a);
      });
    });
    const took = await this.page
      .evaluate((req) => window.__ygSdk(req), { id, msg })
      .catch(() => false);
    if (!took) {
      this.pending.get(id)?.({
        ok: false,
        error: "no YouGame page is open to answer",
      });
    }
    const a = await answer;
    if (msg.type === "mpTicket")
      log(
        `${this.name}: mpTicket ${msg.mode} room ${msg.room ?? "-"} -> ${a.ok ? `room ${a.result?.room} (${a.result?.queue})` : `error ${a.error}`}`,
      );
    return a.ok === true
      ? { ok: true, result: a.result }
      : { ok: false, error: String(a.error ?? "Failed") };
  }

  async open(browser) {
    const ua = await (async () => {
      const p = await browser.newPage();
      const u = await p.evaluate(() => navigator.userAgent);
      await p.close();
      return u.replace("HeadlessChrome", "Chrome");
    })();
    this.ctx = await browser.newContext({
      userAgent: `${ua} YouGameDesktop/${APP_VERSION} (macos; arm64)`,
      viewport: { width: 1280, height: 860 },
    });
    const cookies = this.cookie.split(/;\s*/).map((kv) => {
      const i = kv.indexOf("=");
      return {
        name: kv.slice(0, i),
        value: kv.slice(i + 1),
        url: SITE,
        secure: true,
        sameSite: "Lax",
      };
    });
    await this.ctx.addCookies(cookies);
    const script = `(${initScript.toString().replace("__VERSION__", APP_VERSION)})(${JSON.stringify(
      {
        disc: DISC_INFO,
        features: APP_FEATURES,
      },
    )});`;
    await this.ctx.addInitScript({ content: script });
    await this.ctx.exposeBinding("__ygCall", (_source, name, arg) =>
      this.call(name, arg),
    );
    this.page = await this.ctx.newPage();
    this.page.on("pageerror", (e) =>
      log(`${this.name} page error: ${e.message}`),
    );
    this.page.on("console", (m) => {
      if (m.type() === "error" && !/^Failed to load resource/.test(m.text()))
        log(`${this.name} console: ${m.text().slice(0, 200)}`);
    });
    // Failed requests by path (never the query: it can carry a ticket).
    this.page.on("response", (r) => {
      if (r.status() >= 400) {
        const u = new URL(r.url());
        log(
          `${this.name} http ${r.status()} ${r.request().method()} ${u.host}${u.pathname}`,
        );
      }
    });
    this.page.on("dialog", async (d) => {
      log(`${this.name} dialog: ${d.message()}`);
      this.dialogs = (this.dialogs || []).concat(d.message());
      await d.accept();
    });
    await this.page.goto(`${SITE}/g/${SLUG}`, {
      waitUntil: "domcontentloaded",
    });
    // The band under the game, as the player reads it.
    this.bandTimer = setInterval(async () => {
      const text = await this.page
        .evaluate(
          () =>
            document.querySelector("[data-orca-embed] .orca-status")
              ?.textContent ?? null,
        )
        .catch(() => undefined);
      if (text === undefined) return;
      if (text !== this.band) {
        this.band = text;
        this.bands.push(text);
        log(
          `${this.name} band: ${text === null ? "(none)" : JSON.stringify(text)}`,
        );
      }
    }, 250);
  }

  async call(name, arg) {
    if (name === "play") return this.play(arg);
    if (name === "send") return this.send(arg.line);
    if (name === "stop") return this.stop();
    if (name === "sdkReply") {
      const done = this.pending.get(String(arg.id));
      if (done) {
        this.pending.delete(String(arg.id));
        done(arg.answer || { ok: false, error: "Failed" });
      }
      return;
    }
    if (name === "sdkPush") {
      const line = `data: ${JSON.stringify(arg)}\n\n`;
      for (const c of this.events) c.write(line);
      return;
    }
  }

  emit(p) {
    this.emitChain = this.emitChain
      .then(() =>
        this.page.evaluate((x) => window.__ygEmit(x), {
          buildId: `orca:${SLUG}`,
          ...p,
        }),
      )
      .catch(() => {});
  }

  // The app's play (desktop/src/orca.ts): Orca on the disc with the session's environment.
  play(launch) {
    if (this.proc) throw new Error("Orca is already running for this listing");
    log(`${this.name}: the page pressed Play (room ${launch.room ?? "-"})`);
    const user = path.join(WORK, `user-${this.name}`);
    fs.mkdirSync(user, { recursive: true });
    const env = {
      HOME: process.env.HOME,
      PATH: process.env.PATH,
      ORCA_SESSION: "1",
      ORCA_SHADER_WAIT_S: "300",
      ORCA_TEST_COMMANDS: "1",
      YOUGAME_DESKTOP: APP_VERSION,
      YOUGAME_BRIDGE: `http://127.0.0.1:${this.port}`,
      YOUGAME_TOKEN: this.token,
      YOUGAME_GAME: SLUG,
      ORCA_SITE: SITE,
      YG_SCENES: "1",
    };
    if (PPLUS) env.ORCA_PROFILE = "PPLUS32";
    if (launch.room) env.ORCA_ROOM = launch.room;
    const args = [
      "-n",
      "10",
      "perl",
      "-e",
      `alarm ${ALARM}; exec @ARGV`,
      BIN,
      "-p",
      "headless",
      "-u",
      user,
      "-v",
      "Null",
      "-C",
      "Dolphin.DSP.Backend=No Audio Output",
      "-C",
      "Dolphin.Input.BackgroundInput=True",
      "-C",
      "Logger.Logs.ROLLBACK=True",
      "-C",
      "Logger.Logs.NETPLAY=True",
      "-C",
      "Logger.Options.Verbosity=3",
      "-C",
      "Logger.Options.WriteToConsole=True",
      ...(PPLUS
        ? ["-C", `Dolphin.Core.DefaultISO=${DISC}`, "-e", PPLUS]
        : ["-e", DISC]),
    ];
    const logFile = fs.createWriteStream(path.join(WORK, `${this.name}.log`));
    this.proc = spawn("nice", args, { env, stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stdin.on("error", () => {});
    let out = "";
    this.proc.stdout.on("data", (d) => {
      out += d;
      let i;
      while ((i = out.indexOf("\n")) >= 0) {
        const line = out.slice(0, i).trim();
        out = out.slice(i + 1);
        this.onLine(line);
      }
    });
    let err = "";
    this.proc.stderr.on("data", (d) => {
      logFile.write(d);
      err += d;
      let i;
      while ((i = err.indexOf("\n")) >= 0) {
        const line = err.slice(0, i);
        err = err.slice(i + 1);
        const s = /Scene frame \d+: \S* -> (\S+)/.exec(line);
        if (s) this.onScene(s[1]);
        if (
          /Matchmaking:|Orca room: (report|room match|game|the set|casual|beginning|a matchmade|\{)/.test(
            line,
          )
        )
          log(
            `${this.name}~ ${line.replace(/^.*?(Matchmaking|Orca room)/, "$1")}`,
          );
      }
    });
    return new Promise((resolve) => {
      this.proc.on("exit", (code) => {
        this.exited = code ?? -1;
        this.lines.push(null);
        for (const w of [...this.waiters]) w();
        this.emit({ phase: "done", exitCode: code });
        resolve({ exitCode: code });
      });
    });
  }

  // The app's stdout handling (orca.ts, parseOrcaLine): `orca …` lines go to the page unchanged.
  onLine(line) {
    if (!line) return;
    if (!line.startsWith("orca stats ")) {
      if (Buffer.byteLength(line) > 300)
        fail(`${this.name}: a line over 300 bytes: ${line}`);
      log(`${this.name}> ${line}`);
      if (line.startsWith("orca result "))
        this.results.push(JSON.parse(line.slice(12)));
      this.lines.push(line);
      for (const w of [...this.waiters]) w();
    }
    if (/^orca caps( |$)/.test(line)) {
      this.offered = [
        ...new Set(
          line
            .slice(9)
            .trim()
            .split(/\s+/)
            .filter((c) => CAP_RE.test(c)),
        ),
      ].slice(0, 16);
      this.emit({
        phase: "running",
        message: `orca caps ${this.offered.join(" ")}`.trim(),
      });
      // A headless Orca draws nothing, so it never says `ready`: the app's "running" once it is up.
      this.emit({ phase: "running" });
      return;
    }
    if (line.startsWith("orca stats ")) {
      if (this.agreed.has("stats"))
        this.emit({ phase: "running", message: line });
      return;
    }
    if (line.startsWith("orca "))
      return this.emit({ phase: "running", message: line.slice(0, 300) });
    if (line.startsWith("error "))
      return this.emit({ phase: "error", message: line.slice(6, 306) });
    if (/^(state|unsupported|key) /.test(line))
      return this.emit({ phase: "running", message: line.slice(0, 120) });
  }

  // The app's send (orca.ts): ORCA_COMMAND, the agreed features, then Orca's stdin.
  send(line) {
    if (!this.proc || this.exited !== undefined) return;
    if (!ORCA_COMMAND.test(line)) {
      log(`${this.name}: the app dropped ${JSON.stringify(line.slice(0, 80))}`);
      return;
    }
    if (line.startsWith("caps")) {
      const mine = new Set(
        line
          .split(" ")
          .slice(1)
          .filter((c) => CAP_RE.test(c)),
      );
      this.agreed = new Set(this.offered.filter((c) => mine.has(c)));
    }
    if (!featureAllows(line, this.agreed)) {
      log(`${this.name}: the app held back ${line} (not agreed)`);
      return;
    }
    if (WINDOW_LINE.test(line)) return;
    log(`${this.name}< ${line}`);
    this.sent.push(line);
    for (const w of [...this.waiters]) w();
    this.proc.stdin.write(`${line}\n`);
  }

  stop() {
    if (!this.proc || this.exited !== undefined) return;
    log(`${this.name}: the page stopped Orca`);
    const proc = this.proc;
    const done = new Promise((r) => proc.once("exit", r));
    const left = this.agreed.has("leave");
    if (left) proc.stdin.write("leave\n");
    setTimeout(() => proc.stdin.write("quit\n"), left ? 3000 : 0);
    setTimeout(() => proc.kill("SIGKILL"), 8000).unref();
    return done.then(() => {});
  }

  onScene(scene) {
    this.scene = scene;
    for (const w of [...this.sceneWaiters]) w();
  }

  // The first matching stdout line from index `from` on (with `sent`, lines the page sent Orca).
  expect(re, ms, from = 0, sent = false) {
    return new Promise((resolve, reject) => {
      const end = setTimeout(() => {
        cleanup();
        reject(
          new Error(
            `${this.name}: no ${sent ? "sent " : ""}${re} in ${ms / 1000} s`,
          ),
        );
      }, ms);
      const check = () => {
        const list = sent ? this.sent : this.lines;
        for (let i = from; i < list.length; ++i) {
          const l = list[i];
          if (l === null) {
            cleanup();
            reject(
              new Error(
                `${this.name} exited (${this.exited}) waiting for ${re}`,
              ),
            );
            return;
          }
          const m = re.exec(l);
          if (m) {
            cleanup();
            resolve({ line: l, index: i, m });
            return;
          }
        }
      };
      const cleanup = () => {
        clearTimeout(end);
        this.waiters = this.waiters.filter((w) => w !== check);
      };
      this.waiters.push(check);
      check();
    });
  }

  async expectBand(re, ms, from = 0) {
    const end = Date.now() + ms;
    for (;;) {
      const hit = this.bands.slice(from).find((b) => b !== null && re.test(b));
      if (hit) return hit;
      if (Date.now() > end)
        fail(
          `${this.name}: the band never read ${re} (bands: ${JSON.stringify(this.bands.slice(-6))})`,
        );
      await sleep(250);
    }
  }

  waitScene(name, ms) {
    return new Promise((resolve, reject) => {
      const end = setTimeout(() => {
        cleanup();
        reject(
          new Error(`${this.name}: never reached ${name} (at ${this.scene})`),
        );
      }, ms);
      const check = () => {
        if (this.scene === name) {
          cleanup();
          resolve();
        }
      };
      const cleanup = () => {
        clearTimeout(end);
        this.sceneWaiters = this.sceneWaiters.filter((w) => w !== check);
      };
      this.sceneWaiters.push(check);
      check();
    });
  }

  async press(button, ms = 120) {
    this.pad.buttons |= BTN[button];
    await sleep(ms);
    this.pad.buttons &= ~BTN[button];
    await sleep(120);
  }

  async hold(axes, ms) {
    this.pad.axes = axes;
    await sleep(ms);
    this.pad.axes = [128, 128, 128, 128];
    await sleep(120);
  }

  async close() {
    clearInterval(this.bandTimer);
    if (this.proc && this.exited === undefined) {
      try {
        this.proc.stdin.write("quit\n");
      } catch {}
      setTimeout(() => this.proc.kill("SIGKILL"), 8000).unref();
    }
    await this.ctx?.close().catch(() => {});
    this.server?.close();
  }
}

// The Online menu's path to With Anyone > Casual or Ranked (queue-e2e.mjs menuToQueue).
async function menuToQueue(side, queue) {
  if (PPLUS) {
    await side.waitScene("scSelctCharacter", 180000);
    await sleep(3000);
    await side.press("B", 900);
    await side.waitScene("muMenuMain", 30000);
    await sleep(2500);
    await side.press("B");
    await sleep(1500);
  } else {
    const mash = setInterval(() => {
      if (["", "scStrap", "scBoot", "scTitle"].includes(side.scene))
        side.press("A");
    }, 700);
    await side.waitScene("muMenuMain", 180000);
    clearInterval(mash);
    await sleep(2500);
  }
  for (const b of [
    "DOWN",
    "A",
    "RIGHT",
    "A",
    ...(queue === "ranked" ? ["DOWN"] : []),
    "A",
  ]) {
    await side.press(b);
    await sleep(700);
  }
}

// One game, as queue-e2e.mjs playGame: both pick, the host picks the stage, port 2 walks off until
// the fight ends, and both press on through the results.
async function playGame(host, joiner, first) {
  await host.waitScene("scSelctCharacter", 120000);
  await sleep(2500);
  if (first) {
    await Promise.all([host.press("A"), joiner.press("A")]);
    await sleep(600);
    await Promise.all([
      host.hold([128, 255, 128, 128], 450),
      joiner.hold([128, 255, 128, 128], 450),
    ]);
    await sleep(300);
    await Promise.all([host.press("A"), joiner.press("A")]);
    await sleep(1500);
  }
  for (let tries = 0; host.scene === "scSelctCharacter"; ++tries) {
    if (tries >= 25) fail("the character select never started the match");
    await host.press("START");
    await sleep(1200);
  }
  await host.waitScene("scSelStage", 30000);
  await sleep(1500);
  await host.hold([128, 255, 128, 128], PPLUS ? 70 : 700);
  await host.press("A");
  for (let i = 0; i < 6 && host.scene === "scSelStage"; ++i) {
    await sleep(1500);
    await host.press("A");
  }
  await host.waitScene("scMelee", 30000);
  await sleep(6000);
  joiner.pad.axes = [0, 128, 128, 128];
  await host
    .waitScene("scVsResult", 240000)
    .finally(() => (joiner.pad.axes = [128, 128, 128, 128]));
  await sleep(4000);
  const presser = setInterval(() => {
    if (host.scene !== "scVsResult") return;
    host.press(Date.now() % 2 ? "A" : "START");
    joiner.press("A");
  }, 900);
  try {
    await Promise.race([
      host.waitScene("scSelctCharacter", 60000),
      host.expect(/^orca state friend-left match-over$/, 60000),
    ]);
  } finally {
    clearInterval(presser);
  }
}

// Both pick in the menu (`first` waits longest, so it hosts); the page queues each and sends Orca
// `host` or `join`; they meet in the matched room.
async function meet(first, second, queue, pick) {
  const sentFrom = [first.sent.length, second.sent.length];
  const searching =
    queue === "ranked"
      ? /^Searching for a ranked opponent \(beta\)/
      : /^Searching for an opponent/;
  const bandFrom = [first.bands.length, second.bands.length];
  await pick(first);
  await first.expect(new RegExp(`^orca menu online ${queue}$`), 60000);
  await waitFor(
    () => first.bands.slice(bandFrom[0]).some((b) => b && searching.test(b)),
    30000,
    `${first.name}: no searching band`,
  );
  await sleep(6000);
  await pick(second);
  const cmd = /^(host|join) ([a-z0-9]+)$/;
  const [c1, c2] = await Promise.all([
    first.expect(cmd, 120000, sentFrom[0], true),
    second.expect(cmd, 120000, sentFrom[1], true),
  ]);
  if (c1.m[2] !== c2.m[2])
    fail(`matched into different rooms: ${c1.line} / ${c2.line}`);
  if (c1.m[1] === c2.m[1]) fail(`both pages sent ${c1.m[1]}`);
  const host = c1.m[1] === "host" ? first : second;
  const joiner = host === first ? second : first;
  log(`host ${host.name}, joiner ${joiner.name}, room ${c1.m[2]}`);
  await host.expect(/^orca state friend-joined$/, 150000);
  return { host, joiner, room: c1.m[2] };
}

async function waitFor(cond, ms, why) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) fail(why);
    await sleep(250);
  }
}

async function pressPlay(side) {
  const play = side.page.getByRole("button", { name: /^Play$/ }).first();
  await play.waitFor({ timeout: 60000 });
  await play.click();
}

const sides = [];

async function caps(s) {
  await s.expect(/^orca caps .*\bhost\b/, 120000);
  const line = (await s.expect(/^caps /, 30000, 0, true)).line;
  log(`${s.name}: the page answered ${line}`);
  for (const c of ["host", "join", ...(QUEUE === "ranked" ? ["results"] : [])])
    if (!line.split(" ").includes(c)) fail(`${s.name}: the page's caps lack ${c}`);
}

// One player cancels from the band, then picks again and backs out of the character select while
// searching (`orca menu cancel`).
async function cancelScenario(browser, a) {
  const searching = QUEUE === "ranked" ? /^Searching for a ranked opponent \(beta\)/ : /^Searching for an opponent/;
  await a.startBridge();
  await a.open(browser);
  await pressPlay(a);
  await caps(a);
  await menuToQueue(a, QUEUE);
  await a.expect(new RegExp(`^orca menu online ${QUEUE}$`), 60000);
  await a.expectBand(searching, 30000);
  await waitFor(() => /searching/.test(a.band || ""), 30000, "no searching count");
  let sent = a.sent.length;
  await a.page.getByRole("button", { name: "Cancel" }).click();
  await a.expect(/^queue-cancel$/, 10000, sent, true);
  await waitFor(() => a.band === null || !searching.test(a.band), 10000, "the band still says searching");
  log("PASS: the band's Cancel ended the search and told Orca queue-cancel");
  await a.waitScene("scSelctCharacter", 30000);
  await sleep(2000);
  let from = a.lines.length;
  await a.press("B", 1500);
  await a.waitScene("muMenuMain", 30000);
  await sleep(4000);
  if (a.lines.slice(from).some((l) => l === "orca menu cancel")) fail("orca menu cancel after the page's Cancel");
  // The same pick again, then backing out while searching.
  from = a.lines.length;
  const bands = a.bands.length;
  await a.press("A");
  await a.expect(new RegExp(`^orca menu online ${QUEUE}$`), 60000, from);
  await a.expectBand(searching, 30000, bands);
  await waitFor(() => /searching/.test(a.band || ""), 30000, "no searching count the second time");
  await a.waitScene("scSelctCharacter", 30000);
  await sleep(3000);
  from = a.lines.length;
  sent = a.sent.length;
  await a.press("B", 1500);
  await a.expect(/^orca menu cancel$/, 30000, from);
  await a.expect(/^queue-cancel$/, 10000, sent, true);
  await waitFor(() => a.band === null || !searching.test(a.band), 10000, "the band still says searching after the back-out");
  log("PASS: backing out while searching: orca menu cancel, the page ended the search (queue-cancel)");
}

// The opponent's app closes as they are paired: after 30 s the host's page sends `leave`, says
// they didn't connect, and offers Find another match.
async function noShowScenario(browser, a, b) {
  for (const s of [a, b]) {
    await s.startBridge();
    await s.open(browser);
  }
  await pressPlay(a);
  await sleep(4000);
  await pressPlay(b);
  for (const s of [a, b]) await caps(s);
  await menuToQueue(a, QUEUE);
  await a.expect(new RegExp(`^orca menu online ${QUEUE}$`), 60000);
  await sleep(6000);
  await menuToQueue(b, QUEUE);
  const cmd = /^(host|join) ([a-z0-9]+)$/;
  // Whoever is told to host first: the other's Orca dies before its join can go.
  const first = await Promise.race([a.expect(cmd, 120000, 0, true).then((c) => [a, c]), b.expect(cmd, 120000, 0, true).then((c) => [b, c])]);
  const [host, c] = first;
  if (c.m[1] !== "host") fail(`${host.name} was told ${c.line} first`);
  const gone = host === a ? b : a;
  gone.proc.kill("SIGKILL");
  log(`${gone.name}'s Orca closed as they were paired; ${host.name} hosts ${c.m[2]}`);
  const sent = host.sent.length;
  const t0 = Date.now();
  await host.expect(/^leave$/, 60000, sent, true);
  const waited = (Date.now() - t0) / 1000;
  if (waited < 25) fail(`the host's page left after ${waited} s`);
  await host.expect(/^orca state left$/, 30000);
  await host.expectBand(/^Your opponent didn't connect/, 15000);
  await waitFor(() => /Find another match/.test(host.band || ""), 10000, "no Find another match after the no-show");
  log(`PASS: no-show: the host's page left ${c.m[2]} after ${waited.toFixed(0)} s and offered Find another match`);
}

async function main() {
  // --channel chrome: the installed Google Chrome (headless, a fresh profile), when Playwright's own
  // browser for this version is not installed.
  const channel = opt("--channel");
  const browser = await chromium.launch({
    headless: true,
    ...(channel ? { channel } : {}),
  });
  sides.browser = browser;
  const a = new Side("ada", opt("--cookie-a"));
  const b = new Side("bo", opt("--cookie-b"));
  sides.push(a, b);
  log(`work ${WORK}; listing ${SLUG} on ${SITE}; ${QUEUE}; ${SCENARIO}`);
  if (SCENARIO === "cancel") return cancelScenario(browser, a);
  if (SCENARIO === "noshow") return noShowScenario(browser, a, b);
  for (const s of [a, b]) {
    await s.startBridge();
    await s.open(browser);
  }
  await pressPlay(a);
  await sleep(4000);
  await pressPlay(b);
  for (const s of [a, b]) {
    await s.expect(/^orca caps .*\bhost\b/, 120000);
    const caps = await s.expect(/^caps /, 30000, 0, true);
    log(`${s.name}: the page answered ${caps.line}`);
    for (const c of [
      "host",
      "join",
      ...(QUEUE === "ranked" ? ["results"] : []),
    ])
      if (!caps.line.split(" ").includes(c))
        fail(`${s.name}: the page's caps lack ${c}`);
  }
  const { host, joiner, room } = await meet(a, b, QUEUE, (s) =>
    menuToQueue(s, QUEUE),
  );
  if (QUEUE === "ranked")
    await host.expectBand(/^Ranked beta · best of 3 · 0–0/, 30000);
  else await host.expectBand(/^Casual · vs /, 30000);
  await joiner.expectBand(
    QUEUE === "ranked" ? /^Ranked beta · best of 3 · 0–0/ : /^Casual · vs /,
    60000,
  );

  if (QUEUE === "ranked") {
    for (let g = 0; g < 3 && !host.results.some((r) => r.k === "set"); ++g) {
      await playGame(host, joiner, g === 0);
      await host.expect(/^orca result \{"q":"ranked","k":"game"/, 60000);
    }
    await host.expect(/^orca result \{"q":"ranked","k":"set"/, 60000);
    await joiner.expect(/^orca result \{"q":"ranked","k":"set"/, 60000);
    const hs = host.results.find((r) => r.k === "set");
    const won = hs.out === "won" ? host : joiner;
    const lost = won === host ? joiner : host;
    const wonBand = await won.expectBand(
      /^You won the set 2–[01] · Rating [\d,]+ → [\d,]+/,
      30000,
    );
    const lostBand = await lost.expectBand(
      /^You lost the set [01]–2 · Rating [\d,]+ → [\d,]+/,
      30000,
    );
    log(
      `PASS: set bands: ${won.name} ${JSON.stringify(wonBand)}; ${lost.name} ${JSON.stringify(lostBand)}`,
    );
    for (const s of [host, joiner])
      await s.expect(/^orca state (friend|host)-left match-over$/, 30000);
    await sleep(3000);
    for (const s of [host, joiner])
      if (!/Find another match/.test(s.band || ""))
        fail(
          `${s.name}: no Find another match after the set (band ${JSON.stringify(s.band)})`,
        );
    // Find another match, straight from the band: both search again and meet again.
    const sentFrom = [a.sent.length, b.sent.length];
    const linesFrom = [a.lines.length, b.lines.length];
    const bandsFrom = [a.bands.length, b.bands.length];
    await a.page.getByRole("button", { name: "Find another match" }).click();
    await a.expectBand(
      /^Searching for a ranked opponent \(beta\)/,
      30000,
      bandsFrom[0],
    );
    await sleep(5000);
    await b.page.getByRole("button", { name: "Find another match" }).click();
    const cmd = /^(host|join) ([a-z0-9]+)$/;
    const [c1, c2] = await Promise.all([
      a.expect(cmd, 120000, sentFrom[0], true),
      b.expect(cmd, 120000, sentFrom[1], true),
    ]);
    if (c1.m[2] !== c2.m[2] || c1.m[2] === room)
      fail(`the second match: ${c1.line} / ${c2.line}`);
    const host2 = c1.m[1] === "host" ? a : b;
    const joiner2 = host2 === a ? b : a;
    const from2 = linesFrom[host2 === a ? 0 : 1];
    const bands2 = bandsFrom[host2 === a ? 0 : 1];
    await host2.expect(/^orca state friend-joined$/, 150000, from2);
    await host2.expectBand(/^Ranked beta · best of 3 · 0–0/, 60000, bands2);
    log(
      `PASS: Find another match paired them again in ${c1.m[2]} (host ${host2.name})`,
    );
    // The joiner's player presses Stop mid-set: the page asks first, then Orca leaves; the host
    // waits in the room for the forfeit and its page says so.
    await joiner2.page.getByRole("button", { name: "Stop game" }).click();
    await waitFor(
      () =>
        (joiner2.dialogs || []).some((d) =>
          /Leaving now loses the set/.test(d),
        ),
      10000,
      "no leave warning",
    );
    await host2.expectBand(/Your opponent left/, 60000, bands2);
    const fset = await host2.expect(
      /^orca result (\{"q":"ranked","k":"set".*)$/,
      60000,
      from2,
    );
    const fr = JSON.parse(fset.m[1]);
    if (fr.out !== "won" || fr.by !== "forfeit")
      fail(`the forfeit set: ${fset.m[1]}`);
    await host2.expectBand(
      /^Your opponent left: you win the set/,
      30000,
      bands2,
    );
    log(
      "PASS: Stop mid-set warned, and the host's page said it won the set by forfeit",
    );
  } else {
    for (let g = 0; g < 2; ++g) {
      const from = host.results.length;
      const bf = host.bands.length;
      await playGame(host, joiner, g === 0);
      await host.expect(/^orca result \{"q":"casual","k":"game"/, 60000);
      await waitFor(
        () => host.results.length > from,
        30000,
        "no casual result",
      );
      const r = host.results[host.results.length - 1];
      await host.expectBand(
        r.out === "won" ? /^You won/ : /^You lost/,
        30000,
        bf,
      );
    }
    // The joiner's player presses Stop: the host's page says the opponent left and offers another match.
    await joiner.page.getByRole("button", { name: "Stop game" }).click();
    await host.expect(/^orca state friend-left /, 60000);
    await host.expectBand(/opponent left/i, 30000);
    await waitFor(
      () => /Find another match/.test(host.band || ""),
      30000,
      `${host.name}: no Find another match`,
    );
    log(
      "PASS: casual games read on the band; the opponent's Stop ended the pairing with Find another match",
    );
  }
  const errors = [a, b].flatMap((s) =>
    s.lines
      .filter((l) => l && l.startsWith("orca error "))
      .map((l) => `${s.name}: ${l}`),
  );
  if (errors.length) fail(errors.join("; "));
  log("PASS: the page's whole flow");
}

let code = 0;
try {
  await main();
} catch (e) {
  if (!failed) log(`FAIL: ${e.message}`);
  code = 1;
  for (const s of sides)
    await s.page
      ?.screenshot({ path: path.join(WORK, `${s.name}-fail.png`) })
      .catch(() => {});
} finally {
  for (const s of sides) await s.close();
  await sides.browser?.close().catch(() => {});
}
await sleep(9000);
for (const s of sides) log(`${s.name} bands: ${JSON.stringify(s.bands)}`);
if (!KEEP && code === 0) fs.rmSync(WORK, { recursive: true, force: true });
else log(`logs in ${WORK}`);
process.exit(code);
