# Orca

Orca is YouGame's GameCube and Wii rollback netcode core: a fork of the
[Dolphin](https://github.com/dolphin-emu/dolphin) emulator. Every Orca source file carries an SPDX
`GPL-2.0-or-later` tag, as Dolphin's own do; in aggregate the repository is GPLv3-compatible (see
[COPYING](COPYING)). Orca is not affiliated with or endorsed by the Dolphin project or Nintendo, and
its builds do not use Dolphin's name or logo.

How Orca's rollback works and how it compares with Slippi and Brawlback: [ORCA_ARCHITECTURE.md](ORCA_ARCHITECTURE.md).

- **Base:** Dolphin release `2609` (`f84df02`). The `upstream` remote is `dolphin-emu/dolphin`;
  Orca is rebased onto later releases deliberately.
- **Design:** an exact whole-machine snapshot at each frame boundary; controller input injected at
  the SI device; re-run frames that parse the GPU command stream but skip host rendering; per-game
  profiles. Super Smash Bros. Brawl (`RSBE01` rev 2, and rev 1 played as rev 2) and Project+ v3.2 (a
  launcher profile on top of Brawl) are the supported games.
- **Online:** through YouGame rooms. Friends drop in to a running game; strangers come from
  YouGame's matchmaking queue (casual or ranked).

No disc images, NAND dumps or save data belong in this repository. Bring your own disc.

This document is the engineering reference: how to run and test Orca, and how each subsystem works
and why. Paths are relative to `Source/Core/Core/` unless they start with `Source/`, `Data/` or
`Tools/`.

## Contents

- [Building](#building)
- [Running a session](#running-a-session)
- [Testing](#testing)
- [Rollback and snapshots](#rollback-and-snapshots)
- [Determinism across hosts](#determinism-across-hosts)
- [Drop-in](#drop-in)
- [Direct links](#direct-links)
- [Input delay](#input-delay)
- [Input latency](#input-latency)
- [Music switch](#music-switch)
- [Match start](#match-start)
- [Code the JIT doesn't see](#code-the-jit-doesnt-see)
- [Game patches](#game-patches)
- [Free space and the match block](#free-space-and-the-match-block)
- [Input gate](#input-gate)
- [Online menu](#online-menu)
- [Matchmaking and results](#matchmaking-and-results)
- [Online rules](#online-rules)
- [No CPUs online](#no-cpus-online)
- [Ranked sets](#ranked-sets)
- [Stage select](#stage-select)
- [Character select](#character-select)
- [On-screen UI](#on-screen-ui)
- [Embedding](#embedding)

## Building

Orca builds like Dolphin (CMake). The targets that matter are `dolphin-emu-nogui` (what the YouGame
desktop app runs) and `tests`. On macOS, `Tools/orca-package-macos.sh` makes **Orca.app**, the
bundle the desktop app starts.

Differences from a stock Dolphin build:

- **libcurl** is the bundled copy on macOS and Linux (with mbedTLS and the system CA file), because
  the netcode needs libcurl's WebSocket support. That client doesn't use the macOS Keychain's extra
  roots and doesn't do TLS 1.3. On Windows the bundled libcurl uses Schannel and the system store,
  with certificate revocation checked best effort.
- **libjuice** 1.7.4 (`Externals/libjuice`, MPL-2.0, used under its section 3.3) provides ICE for
  direct links.
- `Orca/` and `Rollback/` build with `-ffp-contract=off` on clang and gcc
  ([Determinism across hosts](#determinism-across-hosts)).

## Running a session

A session is `ORCA_SESSION=1` plus a disc whose profile is in `Data/Sys/Orca/`. Boot refuses any
other disc, and anything that would change what the game computes (`Orca/Profile.h`). A session also
refuses whatever would change one player's game alone: pausing (except solo, see
[Embedding](#embedding)), savestates, the reset button and the Wii power button.

Online play goes through a private YouGame room of up to four (`Orca/Session/Online.h`). The host
plays at once; a friend who joins plugs into the next port ([Drop-in](#drop-in)).

Two players meet when both use the same room code and the same **compatibility key**: the Orca
build, the game profile and its data files, every forced setting, the disc and the seeded save. The
host CPU is not in the key: an Apple Silicon Mac and an x86-64 PC compute the same game, so Mac and
PC players meet. An x86-64 CPU without FMA3 has a different key. The host (the Orca that started the
game) is seat 0, controller port 1; a friend's seat is its room slot.

### Environment

| Variable | Meaning |
|---|---|
| `YOUGAME_BRIDGE`, `YOUGAME_TOKEN` | Set by the YouGame desktop app: tickets come from its loopback bridge. |
| `ORCA_ROOM` | Room code (6-12 of `a-z0-9`). Default: the app's invite, else a new code. |
| `ORCA_JOIN` | `1`: join the friend's game in that room. |
| `ORCA_NAME` | Name shown in the room. |
| `ORCA_PROFILE` | A launcher profile (below), e.g. `PPLUS32`. |
| `ORCA_DIRECT` | `0`: no direct links, the relay only. Default on. |
| `ORCA_THREAD_QOS` | `0`: no raised thread priority; `1`: also for a scripted harness run. |
| `ORCA_JITWARM` | `0`: no JIT warm-up ([Match start](#match-start)). |
| `ORCA_MUSIC` | `off`: start with the music off ([Music switch](#music-switch)), for harness runs. |
| `ORCA_SHADER_WAIT_S` | Set by the app: how long boot may wait for shaders (60-900 s). |
| `ORCA_CONTROLS` | Set by the app: the player's own controls, 92 hex ([Each player's own controls](#each-players-own-controls)). |

Test-only variables (most are "test overrides": they change the compatibility key, so only two Orcas
with the same value meet):

| Variable | Meaning |
|---|---|
| `ORCA_TEST_DEV_GAME`, `ORCA_SITE` | An anonymous dev ticket from the site (default `https://yougame.co`) instead of the app. No ratings, no account. |
| `ORCA_TEST_KEYFRAME_DIR` | Keyframes go through this folder instead of YouGame's store. |
| `ORCA_TEST_LEAVE_AT` | A joined friend leaves at this frame. |
| `ORCA_TEST_COMMANDS` | `1`: take the embed protocol's stdin commands without embedding, plus test commands: `test-drop-room`, `test-direct off\|on\|in\|out\|rebuild`, `test-shot <name>`. |
| `ORCA_TEST_PRESENT` | Headless only: `WxH` presents into an offscreen image with the OSD and overlay, so screenshots show what a player sees. |
| `ORCA_TEST_PADS`, `ORCA_TEST_SKIP_RENDER` | Harness overrides. |
| `ORCA_TEST_FULL_SNAPSHOTS` | `1`: snapshots copy all of RAM instead of copy-on-write. |
| `ORCA_TEST_NO_AFP` | `1`: an ARM CPU with FEAT_AFP behaves as one without (Apple M4 as M1-M3). |
| `ORCA_TEST_CPUCORE` | `0` (interpreter) or `5` (cached interpreter) as a reference. |
| `ORCA_TEST_NET_DELAY_MS`, `ORCA_TEST_NET_SPIKES`, `ORCA_TEST_NET_UPLINK` | A simulated bad link (`Orca/Profile.h`). |
| `ORCA_DIRECT_FORCE` | `turn`: relayed candidates only; `srflx`: nothing on a host interface. |
| `ORCA_DIRECT_TURN` | `0` never use TURN, `1` from the first link. Default: only after a link found no path. |
| `ORCA_DIRECT_STUN`, `ORCA_DIRECT_ICE_LOG`, `ORCA_DIRECT_LOOPBACK` | Leave STUN out; log every libjuice line (addresses masked); offer loopback candidates for two Orcas on one machine. |
| `ORCA_DIRECT_LOSS`, `ORCA_DIRECT_BLOCK` | Drop this percentage of datagrams; start with every datagram dropped. |
| `ORCA_TEST_GATE` | Input-gate masks ([Input gate](#input-gate)). |
| `ORCA_TEST_QUEUE`, `ORCA_TEST_QUEUE_PICK` | A queue room's header, and a matched joiner's pick ([Character select](#character-select)). |
| `ORCA_TEST_CHAR_ORDER`, `ORCA_TEST_SET_RULES` | Run the ranked character order, or set the stock/time rules, without a real room. |
| `ORCA_UX_TEST_NAMES`, `ORCA_UX_TEST_CONTROLS`, `ORCA_UX_TEST_PLUG_AT` | Names, controls and a plug-in frame for harness ports. |
| `ORCA_TEST_HOLD`, `ORCA_TEST_SHADER_WAIT_MS`, `ORCA_TEST_FXC_FLAGS`, `ORCA_TEST_JITWARM_SLOW_US`, `ORCA_JITWARM_SEED`, `ORCA_JITWARM_LOG`, `ORCA_JITWARM_BUDGET_US` | Knobs for the sections that describe them. |
| `ORCA_UX_PROBE` | Memory watches, dumps and writes for reverse-engineering game state. |
| `ORCA_UX_KIT_DEMO`, `ORCA_UX_SETEND_DEMO`, `ORCA_UX_OVERLAY_DEMO` | Draw overlay elements for design checks. |

### Orca.app and the disc

Orca.app is a session by default (`ORCA_SESSION=0` turns that off). Its user directory is
`~/Library/Application Support/Orca` unless `-u` says otherwise. Without `-e` it asks for the
player's own Brawl disc image, checks the game ID and revision before boot, and remembers the path
in `Config/Orca.ini` `[Disc] Path`. A file given with `-e` is checked before anything else starts:
a missing file prints `orca error disc_missing <path>`, an unreadable one `orca error
disc_unreadable <path>`, and Orca exits 1 within a second. Orca's alert handler is registered before
the boot file is read, so no hidden message box can block a run.

Brawl (USA) (Rev 1) plays as Rev 2. The two dumps differ only in the revision byte of both disc
headers and in one main.dol instruction (`0x8001BC9C`, the version in the disc ID the game builds at
boot); every file, the file table, the partition layout, the IOS and the title version are the same.
Every disc a session boots, and the launcher profile's disc, goes through `Orca::AliasRevision`
(`Orca/Disc.h`): for that exact dump (the content hash in its TMD) reads of those three bytes return
Rev 2's (`VolumeDisc::SetReadPatches`), so the console reads a Rev 2 disc and Rev 1 and Rev 2 players
meet in the same rooms. The TMD and ticket still differ in hashes, signatures and the ticket ID; the
game never reads them: a Rev 1 and a Rev 2 run of the default inputs (Brawl and Project+) give the
same hashlog. Other revisions and modified Rev 1 copies are refused, and so is a launcher's disc that
doesn't read as Rev 2 at boot (`Orca::SessionDiscReady`, e.g. after a read error).

`Orca --verify <disc> [-u <user>]` checks a disc image and exits, without booting, a window, a
session or the app's bridge (`Orca/DiscVerify.h`). It runs Dolphin's integrity check (the Wii hash
tree; no Redump lookup, no whole-disc hashes) and prints `orca verify progress <done> <total>` in
bytes at most once a second and one `orca verify problem <low|medium|high> <text>` per problem, with
Dolphin's severity. A Medium or High problem counts as damage unless it is only in Brawl's
Masterpiece partitions, which Orca and Project+ never read (some WBFS tools wipe them); each one
that counts is followed by `orca verify damage <text>`. Last comes `orca verify ok` (exit 0) or
`orca verify damaged <n>`, n damage lines (exit 2). A file that isn't a disc image prints `orca
verify unreadable <path>` (exit 1). The signature checks use a temporary NAND, never the player's.
A GameCube disc has no hash tree, so only its structure is checked. A good scrubbed Brawl Rev 2
`.wbfs` reports 9 Low problems: no update partition, no stored image size, and errors in unused
blocks (8088 in DATA, 1 in each Masterpiece partition).

Sessions start from an empty NAND (save hash 0); the game makes its save in-session on both
machines. A session with no `Config/GCPadNew.ini` gets `Sys/Orca/Input/GCPadNew.ini`: port 1 on the
keyboard and the first SDL gamepad of any model.

### Launcher profiles (Project+)

A mod that boots through its own loader runs from a launcher profile (`ORCA_PROFILE`;
`Data/Sys/Orca/PPLUS32.ini` is Project+ v3.2). The session boots the loader (`-e <launcher.dol>`)
with the disc as Dolphin's default disc, and refuses unless the loader, the disc's ID and revision
and the SD card image next to the loader match the profile (XXH3-128). The SD image is opened
read-only. The profile also names the save that seeds the session NAND, the game's title, and its
game patches. None of a mod's files are in this repository; the player downloads the official
release.

```bash
ORCA_SESSION=1 ORCA_PROFILE=PPLUS32 dolphin-emu-nogui -p headless -u <user> -v Null \
  -C "Dolphin.Core.DefaultISO=<your Brawl (USA, Rev 2) disc>" \
  -e "<dir>/Project+ Netplay Launcher.dol"
```

Orca.app started with no arguments reads `orca-launch.ini` next to the app if the build ships one:
`[Launch]` with `Profile` and `Executable` (relative to that folder). The parser refuses unknown keys
and sections, and any path that is absolute, has a `.` or `..` component, or resolves outside the
folder (`Orca/Launch.h`). A mod's native build is Orca.app plus the mod's files and that ini.

### The local controller

Under the YouGame app, the app's controller helper reads GameCube adapters (libusb, raw reports) and
gamepads, and Orca takes the newest snapshot of its stream at each frame boundary
(`Orca/UX/ControllerSource.*`). Without the app, it is port 1 as mapped in Dolphin's controller
settings. A session never runs Dolphin's own adapter code.

`orca stats` reports the input age: `pa`, the mean age of the newest adapter report when a frame
reads it (only reads whose pad changed), and `rpf`, adapter reports per frame. Expect a little over
4 ms at the official adapter's 125 Hz and a little over 0.5 ms at 1 kHz. The age is measured on one
clock per span (the helper's, then Orca's); the hop between processes is left out because their wall
clocks drift and step, and its jitter is logged separately. `Tools/orca/controller-latency.mjs`
measures the whole transport (0.1-0.3 ms on loopback); `Tools/orca/fake_bridge.py` with
`FAKE_PAD_HZ` stands in for an adapter.

## Testing

Run tests headless (`-v Null`), muted, and at low priority on a shared machine. Two-instance runs
each start two emulators.

### Unit tests

```bash
ninja tests && ./Binaries/Tests/tests --gtest_filter='Orca*'
```

Live, network and disc tests skip without their switches (`ORCA_TEST_DISC` for a Rev 2 image,
`ORCA_TEST_DISC_REV1` for Rev 1). `ORCA_LIVE_TEST=1` runs `OrcaLive.*`
against real rooms on yougame.co.

### The harness

`Rollback/Harness.h` is a test harness driven by environment variables (it needs `ORCA_SESSION=1`).
The header documents every knob; the main ones:

| Variable | What it does |
|---|---|
| `YG_INPUT=<file>` | Scripted pads (`Tools/orca/inputs/`): hold, mash and fuzz rules, counted from scene entry with `@<scene>`. |
| `YG_EXIT_AFTER=<n>` | Stop after first-pass frame n. |
| `YG_HASHLOG=<file>` | A RAM hash per frame. Two builds that should compute the same game must produce the same log. |
| `YG_SYNCTEST=<k>` | Save every frame, rewind k, re-run, compare RAM and device-state hashes. `0 RAM mismatches` is the bar. `_FROM`, `_DIAG`, `_SECTIONS`, `_CLEARJIT` refine it. |
| `YG_KEYFRAME_TEST` | Capture and load a drop-in keyframe, then check 30 re-run frames. |
| `YG_PADREC=<file>`, `YG_LOOPBACK=<file>` | Record pads and checksums, then replay them through a real session with a late virtual remote (`_LAG`, `_JITTER`, `_SPIKE`, ...). |
| `YG_THROTTLE=1` | Run in real time instead of unthrottled. |
| `YG_SCENES=1`, `YG_RESULTS=1` | Log scene changes and each game's result. |
| `YG_SHOT_EVERY`, `YG_SHOT_AT`, `YG_DUMP_FRAMES_FROM/_TO` | Screenshots and frame dumps. |
| `YG_CLEARJIT_AT`, `YG_JITCODE_LOG`, `YG_JITCODE_CENSUS`, `YG_WATCH` | JIT history diagnostics ([Code the JIT doesn't see](#code-the-jit-doesnt-see)). |

A device-state-only mismatch in a sync test is expected (the emulated icache, GPU dirty flags, host
counters); only RAM mismatches are bugs. Settings go through Dolphin's config system names:
`Graphics.*`, never `GFX.*` (silently ignored). A forced session setting (`Orca/Profile.cpp`) beats
`-C`.

Example: the reference Brawl run.

```bash
ORCA_SESSION=1 YG_INPUT=Tools/orca/inputs/bf-mario-link-results.txt YG_EXIT_AFTER=10800 \
  YG_HASHLOG=brawl.hash dolphin-emu-nogui -p headless -v Null -e <your Brawl (USA, Rev 2) disc>
```

### Two-instance tests

These start two Orca processes and steer them as the desktop app does, over real rooms on yougame.co
with dev tickets (nothing is recorded). Each takes one to three minutes.

| Tool | What it covers |
|---|---|
| `Tools/orca/dropin-commands.py` | Join, leave, rejoin, the host leaving. |
| `Tools/orca/dropin-rooms.py` | A room with no host, a friend first in the host's room, the host's connection dropping; a guest going home, leaving the friends lobby for the queue, a kept pick. |
| `Tools/orca/dropin-delay.py` | The input delay for five minutes after a join, optionally on a simulated bad link. |
| `Tools/orca/dropin-identity.py` | Each player's own name-tag controls in the other's game. |
| `Tools/orca/direct-link.py` | Direct links: off/in/on, rebuilds, TURN, the ticket's switch. |
| `Tools/orca/versus-cpu-dropin.py` | A friend dropping into Group > Brawl's character select with CPUs on it: both machines clear them and seat the friend at the same frame. |
| `Tools/orca/online-menu.py`, `online-menu-dropin.py` | Every path through the Online menu (one Orca, offline, no network access allowed); a friend dropping in on it. |
| `Tools/orca/queue-e2e.mjs` | The matchmaking flow: casual and ranked, rematch, skip, timeout, kill. Plays the page's half of the protocol. |
| `Tools/orca/free-space.py` | That the match block and code caves are never touched by the game. |
| `Tools/orca/jit-history.py` | Code the JIT compiled that RAM no longer holds. |
| `Tools/orca/occluded-test.py` | Frame pacing with the window covered, with a friend, or with the overlay opened. |
| `Tools/orca/xplat.py` | One Orca on each of two machines (`--role join` on the second). The only test that can show a Mac and a PC disagreeing. |

All take `<dolphin-emu-nogui> <disc>`, and `--pplus "<dir>/Project+ Netplay Launcher.dol"` for
Project+.

### Release checklist

Every release candidate passes, for Brawl and for Project+:

1. **Build** `dolphin-emu-nogui` and `tests` in Release: no new warnings.
2. **Unit tests:** `Orca*`, none failing.
3. **Hashlogs** of the default inputs (`bf-mario-link-results.txt` to frame 10800,
   `pplus-1v1.txt` to 9000) equal to the live release's, frame by frame. A release that changes
   emulation on purpose says which frames move and why.
4. **Sync tests** (`YG_SYNCTEST=7`): 0 RAM mismatches over the whole casual flows.
5. **Two-Orca checks** on one machine: `queue-e2e.mjs --queue casual --q2` (and `--scenario
   rematch`) and `online-menu-dropin.py`. Each must pass with every session's checksums matched
   (both Orcas print `K checksums matched` with K > 0), no `Desync at frame`, no `orca error`. A
   sync test runs one game against itself, so only this shows races between two peers' messages.
6. **Mac and PC:** `xplat.py` with one Orca on each. Two Orcas on one machine share a compiler, an
   architecture and a JIT; only this run sees two builds disagree.

Releases that could affect netcode performance also run a two-Orca performance comparison against
the live build.

## Rollback and snapshots

`Rollback/Rollback.*`, `Rollback/OnlineMatch.cpp`, `Orca/Session/Session.*`.

- **The frame hook.** Each profile names an instruction in the game's main loop that ends a frame.
  The hook runs there, at every frame boundary, on first runs and re-runs: it saves or loads
  snapshots, sets the pads for the next frame, and runs Orca's game-memory logic (patches, rules,
  readers). Everything it writes to emulated memory is a pure function of memory, the frame number
  and the session's synced inputs, and is idempotent at its boundary, so a rollback that runs the
  hook again at the same boundary writes nothing new. Anything that depends on one machine (the
  overlay, stdout lines) happens on first runs only.
- **Snapshots** hold device state and MEM1/MEM2, copy-on-write (`Rollback/Cow.h`). The session NAND
  is kept as a journal mark, not bytes. A load restores only the 4 KB pages that changed, and drops
  the JIT blocks in those pages; the rest of the JIT is kept across loads. The EFB is left alone:
  re-run frames draw nothing.
- **Window:** 7 frames of rollback, 2 frames of input delay by default
  ([Input delay](#input-delay)). Peers exchange a checksum every 60 frames; a mismatch is a desync,
  which unplugs the friend rather than letting the games drift.
- **Ports.** Online, SI is forced to four GameCube pads, and a port without a player reports no
  controller (bit `0x80` of byte 0, `UNPLUGGED_PAD`). Pads travel as 8 bytes (`PadCodec.h`).
- **Inputs read where the game reads them.** `Rollback::InputOverride` supplies each poll; the
  [SI relatch](#input-latency) makes the game read the pads the hook just set.

## Determinism across hosts

Mac and PC players share a session, so everything that reaches emulated memory must compute the same
on Apple clang/ARM64 and MSVC/x86-64.

**Orca's own arithmetic is integer.** What Orca's C++ works out from emulated memory on every
machine (input-gate masks, steered sticks, whatever a hook writes) is integer math. Apple clang
contracts `a * b + c` into one fused multiply-add on ARM64 by default, which rounds once where MSVC
rounds twice: a float `sqrt(dx * dx + dy * dy)` in the queue's stick steering gave a Mac and a PC
sticks a unit apart for 12 of 20 million inputs. The steering is now fixed point
(`Queue::ToFixed`, an exact `IntSqrt`), pinned by `OrcaQueue.SteeringIsTheSameOnEveryHost`. `Orca/`
and `Rollback/` also build with `-ffp-contract=off`; MSVC's `/fp:precise` never contracts. Floats
that remain on shared values are only compares, truncations of in-range values, bit copies and
constant writes, none of which round differently.

**The frame hook runs in the host's floating-point mode** (`HostFloatScope`). The CPU thread
otherwise runs in the guest's FP mode; under ARM's FPCR.AH, FNEG and FABS leave a NaN's sign alone,
and clang builds some constants with MOVI + FNEG, which turned a `{0, INT_MAX}` into `{0, -1}`. The
hook switches to the host default on entry and restores the guest's mode from FPSCR on exit.

**Floating point on every host.** Brawl and Project+ run in PowerPC's non-IEEE mode (FPSCR.NI).
Three hosts each differ from PowerPC there:

| | x86-64 (Jit64) | ARM with FEAT_AFP (Apple M4+) | ARM without (Apple M1-M3) |
|---|---|---|---|
| Mode Orca sets | MXCSR FTZ | FPCR FZ, AH | FPCR FZ |
| NaN made from no NaN (inf * 0) | negative | negative | positive (PowerPC: positive) |
| Denormal inputs | kept (as PowerPC) | kept | flushed to zero |
| Denormal outputs | flushed after rounding | flushed after rounding | flushed before rounding (as PowerPC) |

What a session does about it:

- **PowerPC's NaN rules** (accurate NaNs forced): input NaNs picked in PowerPC's order and quieted,
  a new NaN is `0x7FF8000000000000`, a NaN result is never negated. Both JITs' accurate-NaN paths
  now cover every arithmetic op, and fctiw, frsqrte and psq_st of a NaN match PowerPC.
- **Denormal inputs kept** on CPUs that flush them: a single that may be a denormal never goes into
  a single-precision op; the op runs in double precision, as Jit64 computes everything.
- **Paired loads and stores** quiet SNaNs as Jit64 does, and fall back to the interpreter exactly
  where Jit64 does (Project+'s codes use `rA = 0` forms).
- **The interpreter under the host's mode** uses integer sign operations, so fabs, fres and fctiw
  are right under FPCR.AH.
- FEAT_AFP is detected on macOS, and FPCR.AH is set only where the CPU has it.

These fixes also apply outside a session wherever Dolphin's accurate-NaN setting is on. Forcing
accurate NaNs means an input recording from a build without it may not reproduce a run that makes a
NaN.

Remaining differences are detected (the checksum exchange reports a desync), not silent, and none
has been seen: double denormal inputs on M1-M3 (none in about 80,000 scripted frames), tininess
before rounding on M1-M3 (a 2^-25-wide window below FLT_MIN), and fctiw of a value rounding to -0
in an Rc = 1 form. Cost: about 3-5% slower emulation, 10-11% on an M1-M3 path (measured on an M4
with `ORCA_TEST_NO_AFP=1`).

Considered and not done: FZ without AH on every ARM CPU, which would remove the M1-M3 residuals
between Macs at about 5% speed on an M4, but would make Mac-to-PC parity depend on a matching x86
change.

## Drop-in

The host plays at once. An invited friend joins as if they had plugged a controller into the next
port of the host's game, and both machines show it. The friend loads the host's latest state and
syncs from there, so joining never takes longer the longer the host has played.

`Rollback/OnlineMatch.cpp`, `Orca/Session/Session.*`, `Orca/Session/Keyframe.*`.

### How it works

1. **Solo.** The host's port 1 is its local pad with no input delay; the other ports report no
   controller. It logs every frame's pads (32 bytes a frame) and joins its room in the background.
   There is no rollback and no snapshot.
2. **Keyframe.** On an invite (`prepare-join`) or when a friend arrives, whichever comes first:
   - At the next frame boundary the host captures the machine (`SnapshotRing::Capture`, 8-10 ms)
     and takes the session NAND as a copy-on-write clone (APFS; a plain read elsewhere).
   - NAND files that the game's boot writes whatever the player does (`BootNandFrame` in the
     profile; Brawl writes 38.7 MB of packs to `/tmp`) travel as XXH3 hashes only. The joiner runs
     its own boot to that frame while it downloads, then fills those files in from its own NAND,
     refusing the keyframe if one differs. This halved the upload. Saves always travel.
   - A thread packs everything, compresses it (zstd level 3 with long-distance matching), encrypts
     it with AES-256-GCM under a fresh key, and stores it. The game goes on meanwhile. The key
     travels only in the room's `kf` message, so the store holds ciphertext.
   - Never from the host's first 300 frames: Project+'s first frames after its launcher run
     differently on a machine whose JIT holds other blocks. A keyframe under 30 s old is reused;
     each serves one joiner, who deletes it after loading.
3. **Join.** The host starts a rollback session at its next frame J and sends the friend the
   keyframe's id, frame K, size and hash. The friend boots the same disc and profile, runs its boot
   to `BootNandFrame` (`orca state joining <percent>`), downloads and checks the keyframe, replaces
   its session NAND, loads the state (`SnapshotRing::LoadImage`), then runs the host's logged pads
   for [K, J) and live inputs from J, unthrottled and unrendered, until it has caught up.
4. **Plug in.** The host picks a frame S (its frame plus the rollback window, twice the maximum delay
   and the round trip) and announces it in the roster every packet carries. Both machines plug port
   2 in at exactly S. The host never waits on a joiner before S.
5. **Leave.** The host picks an unplug frame L the same way; the friend stops once its inputs before
   L are acknowledged. A friend whose connection drops is unplugged after its last input (rolling
   back frames that guessed otherwise); one silent for 10 s is dropped. Both sides learn why
   (`friend-left left|desync|stalled|network`, `bye desync`, `drop`). With nobody else plugged in,
   the host plays solo again with no input delay.

Seats 3 and 4 work the same way, but only two players have been tested. With a third player, the
packet carrying a catching-up joiner's inputs also limits the inputs the others get, so live players
would stall during a join; catch-up should come from history only.

**Typical numbers** (Brawl): keyframes of 26-30 MB; compress 120-170 ms on a thread; over the
internet a join took 5.7-7.2 s from the friend's first frame to plugged in (upload, download and
about 1 s of catch-up), and about 1.5 s with a local store. Higher zstd levels don't pay: level 19
saves 6 MB over level 3 but takes 10 s; what is left is already-compressed game data.

### Port values: each player's name and controls

A joiner plays on a copy of the host's machine, save included, so its own name tags are not there.
Each port carries its player's values instead (`Orca/UX/NameTags.h`): the YouGame username as the
tag's name, the tag's rumble byte and its whole button layout. The frame hook writes them into the
port's name tag on the character select, and the game copies the layout into its per-port table
(`0x805B7480`) as a match starts, so each port plays with its player's own buttons.

The host decides each port's values and the frame they apply from; every machine holds them before
it runs that frame (`Session::RequireValues`). Values from another machine reach the tag only as the
game's own menus could have set them, at their exact size (64 bytes at most on the wire).

### Each player's own controls

A player's own controls are 46 bytes: rumble, then a tag's layout (`NameTags.h`), written as 92 hex.
They come from `ORCA_CONTROLS` at boot, else the file Orca kept for this game
(`<user>/Config/OrcaControls-<game>.txt`), and from the app's `controls <hex>` mid-run. While the
player is alone, a change they make to the tag they wear (the controls menu, Project+'s name list)
becomes their own controls: Orca prints `orca controls <hex>` and keeps the file. Orca's own writes,
a save that isn't theirs (after a resync) and re-runs never count. Controls loaded from the file
(no `ORCA_CONTROLS`) are printed the same way once at boot, so the page shows what Orca plays.

- **Solo**, the own port's values carry them from the frame they change, so the YouGame tag gets them
  on every character select: Training, the queue's own select, With Friends. A friend who joins
  later gets those entries with the keyframe. A session's host and joiner carry them as before
  (`RefreshOwnValues`, the hello).
- **The name button** takes A on With Friends' selects (the hand's own panel) and on the queue's own
  select while not searching, with the stick centred while A is down; its list takes A and the stick,
  never L (Project+'s hold-L leaves for its controls scene). Start never readies with the list open.
  A queue room's select keeps A in the grid: there each player wears their YouGame tag with their
  own controls.

### Rooms

- Orca asks for 4-seat private rooms with a game-owned lobby. Seats are lobby slots. A hello carries
  the compatibility key and whether the sender is the host. The host must hold slot 0; one that
  finds itself elsewhere opens a fresh room.
- The room's ticket (`mpTicket`, minted by the page) waits up to 30 s for a reply and is asked for
  once more if none came; then the room ends with `network`.
- A joiner that hears no host hello within 8 s of its welcome ends with `peer_left`.
- The server answers a ping every second; 8 s without any message ends the room as a dropped
  connection (close code 4001), so the server holds the seat for 90 s. Not much less than 8 s: a
  keyframe upload can fill a home uplink and delay the pings.
- A host whose room ends without it leaving prints `orca error network <why>` and reopens, backing
  off from 2 s to 30 s, trying the same code first. A socket the server replaced (close code 4000:
  another run on the same account) is never taken back. During the player's pause the host loop
  does the same every 2 s on the CPU thread (`WatchRoomWhilePaused`), so a paused game keeps a room.
- A host that leaves always opens a new room. A player on a port other than 1 after a session (it
  joined someone and went solo) goes home ([Going home](#going-home)).
- A rollback the joiner can't make (an engine fault, not a desync) ends its session as `network`.

### Going home

A player on a port other than 1 after a session (it joined a friend's game, then it left, its host
did, or the session failed) goes home (`OnlineMatch.cpp` `ComeHome`). At its first alone boundary on
the menus (the main menu or a character select; never a fight, a stage select, a results screen, a
scene change or a single-player mode: `OnlineMenu.h` `MenusScene`), with no session, nothing of
drop-in under way and the old room gone, it moves to port 1 and opens a room of its own a second
later, as after a queue room. Solo with no session, that boundary is a first run nothing re-runs,
and only this game changes; the next friend's keyframe carries it.

- Home on the character select a With Anyone pick opened, it announces that pick too ([Casual or
  Ranked with friends in the game](#casual-or-ranked-with-friends-in-the-game)), so its Start
  readies it on the queue's own character select.
- On a character select the move comes a frame after the old host's port unplugged, so port 1's
  panel keeps what the old host put there and the guest's hand now drives it, while its own old
  panel closes and it picks again. Cosmetic: a With Anyone select starts with nobody's token down.
- A player in a fight when its host goes prints `orca state no-room` once, after the old room has
  told the page it is empty (so the page never shows that room again), and goes home once back on
  the menus. The app can still send `join` meanwhile, which wins over going home.

Harness knob: `ORCA_UX_TEST_RESEAT_AT=<frame>`: the player plays port 2 until that frame and port 1
from it, as a guest going home does, for synctests of the move (`Tools/orca/inputs/bf-reseat.txt`).

### Play again after a ranked set

After a set both games are back on their own Ranked character select, each hosting a friends room.
The page's Play again invites the opponent (`prepare-join` to the inviter, `join <code>` to the
invitee). Through 0.3.27 that join failed: the inviter's game still carried the queue select's header,
its keyframe carried it, and the friend refused it (`orca error mismatch`). The same happened to any
invite from a With Anyone character select. Now the newest intent wins
(`OnlineMatch.cpp` `EndQueueForFriend`, `EndQueueForJoin`, `RearmFriendsPick`):

- **The host** (port 1, not joining, its room's welcome said `private`): a friend arriving, or a
  `prepare-join`, while the player is on the queue (its own select, or searching) ends the queue and
  the search and prints `orca menu cancel`. The header goes back to none at the next boundary, a
  keyframe made under the old one is dropped, and the friend's keyframe has none. Never in a matched
  room, nor before a room's welcome.
- **Once the friends are gone** (one came in, all left, and the host is alone again on that pick's
  character select), the pick is armed again if the page can search it (`orca menu online ranked`),
  so Start searches. Never for an invite nobody took yet.
- **The friend** whose keyframe loads into a friends room ends its own queue the same way and drops
  the game it kept for after a queue room, which would otherwise block going home. A join that fails
  before the keyframe loads keeps all of it.

Local: only the host's own solo boundaries and process state change; nothing a session runs on two
machines, so no compatibility change. Tests: `OrcaOnlineMenuLobby.*` (the three play-again tests).

### Keyframe store

`HttpKeyframeStore` in `Orca/Session/Keyframe.cpp`. Tests use `ORCA_TEST_KEYFRAME_DIR`: the store
refuses dev tickets.

- `https://yougame.co/api/orca/keyframes/<room>/<id>`, `id` = `kf-<frame>-<8 hex of XXH3-64>`.
  Auth: `Authorization: Ticket <ticket>` with the room's ticket (accepted up to 6 h past expiry; on
  401 `ticket` or 403 `signed_out` Orca mints a fresh one once).
- **PUT** with `Content-Length` (at most 128 MB), `X-Orca-Frame`, `X-Orca-Hash`. 409 `exists` counts
  as done. 6 PUTs a minute per account.
- **GET** streams with `Content-Length` (for join progress); Orca checks size and hash, then
  decrypts. 404 `gone` means expired.
- **DELETE** by the joiner after loading, best effort; by the host when replacing or leaving.
- Network errors, 429 and 5xx retry 4 times with 0.5/1/2 s backoff; anything else is final. A host
  whose keyframe is refused for good (a refusal with YouGame's code; a bare 4xx from the edge only
  fails that keyframe) makes no more for the friends waiting: it drops them with
  `signed_out` (YouGame's code) or `refused`, prints `orca state friend-left <that>`, and each of
  them ends with `orca error signed_out` or `network`. A joiner whose own download is refused with
  `signed_out` reports that code too.

### Room messages

Relayed `msg` (`Orca/Session/YouGameRoom.cpp`):

- `hello {c, r, h, dl}`: key, heard-you, is-host, offers direct links. Sent until answered.
- `dl`, `dlc`, `dlr`: direct-link signalling, only between two Orcas that both offered links.
- `kf {f, id, n, x, key}`: the keyframe offer, every second until the joiner's first packet.
- `drop {r}`: the host unplugged this joiner (`desync`, `stalled`, `network`).
- `p`: session packets. `q` is the sender's packet count; a stale copy still contributes inputs,
  acknowledgements and checksums, never roster or timing. Drop-in adds `ro` (roster), `ha`
  (history ack), `ht`/`hf`/`hp` (history runs) and `lv` (leaving).
- `bye {r}`: why a player leaves, broadcast before the leave.

## Direct links

`Orca/Session/DirectLink.*`. Every session packet also goes straight to each other Orca in the room
over UDP, beside the relay. The relay costs about 15 ms each way (machine, Cloudflare edge, the
room's Durable Object, edge, machine, over TCP); two machines on one LAN see a 40-50 ms round trip
through it where a direct one takes about 1 ms.

- **ICE** is libjuice: host candidates on every interface (IPv4 and IPv6), the public address from
  STUN, and Cloudflare TURN over UDP from the ticket's `ice` credentials. TURN over TCP or TLS isn't
  used: on a network that blocks UDP the room's relay is the path.
- **Signalling** is room messages. The lower connection id offers; the other answers; candidates
  trickle in batches (at most 10 messages a second).
- **Keys.** Each side sends 32 random bytes in its `dl`. HKDF-SHA256 over both, salted with the room
  code, gives one key per direction. Every datagram is ChaCha20-Poly1305 with a counter nonce and a
  64-counter replay window. Only the two Orcas of a pair can make a datagram the other takes; the
  room sees both halves, so the trust is the relay's, no more.
- **Datagrams** are at most 1200 bytes (IPv6's minimum MTU less headers) and carry the packet's
  JSON without drop-in history (the relay alone carries that, in order). Each echoes the peer's
  newest counter, giving a round trip without a shared clock. Keepalive after 200 ms idle. Marked
  DSCP EF (not on Windows).
- **Sending** happens at once on the emulator thread under the link's own lock: 70-100 us on average
  on an M-series Mac.
- **Both paths, always.** The relay still carries every packet; whichever copy arrives first counts,
  so a link that fails costs only the relay's time.
- **Rebuilds.** libjuice calls a pair failed only after 30 s without consent, so Orca rebuilds a
  link when the agent fails, when no answer comes within 5 s or no path within 10 s, when a link
  that was up hears nothing for 3 s while the relay still brings the peer's packets (a network
  switch, an expired NAT mapping), and once, 10 s in, on a TURN or server-reflexive pair between two
  machines that share both a host subnet and a public address (`OneLan`). A shared /24 alone isn't
  one LAN: two homes on the same private range would otherwise tear down a working link. Failure
  rebuilds back off 2 s to 30 s. Each build is a new generation with fresh keys.
- **Flood limit:** at most 300 packets a second per link after authentication (an honest Orca sends
  one a frame); the rest are dropped (their copy still comes through the relay).
- **Switching off:** YouGame can turn links off for everyone with `"direct": false` in the ticket
  reply; the player can with the embed command `direct off`; `ORCA_DIRECT=0` is a test switch.
- **Logs** never carry addresses, candidates or credentials. `orca stats` gains `tx` (0 relay, 1
  direct, 2 TURN) and `lrtt` (the link's median round trip).
- **Firewall.** The room binds a throwaway UDP socket on every interface as soon as it is in the
  room, so a Windows firewall prompt comes in the lobby rather than mid-game. Windows ties the rule
  to the executable's path, so each new version folder asks again. Orca.app is signed, so the macOS
  firewall lets it be.
- **Privacy.** ICE shows each player's LAN and public addresses to the other players in the room.
  A TURN-only option could hide them later.

**TURN only when needed.** A link starts with host and STUN candidates only. A link that never came
up marks the pair `with_turn`, and the next link to that peer gathers TURN too. A TURN allocation
beside a working direct path made one Wi-Fi card (Realtek RTL8822BE) drop off the air for 100-400 ms
every 5 s, stalling the other player each time (42-57 stalls in 160 s against 4 without TURN). Most
pairs connect host-to-host or through STUN anyway.

**Measured** (two Orcas, one machine, prod rooms): direct links gave 0 rollbacks and 0 stalls over a
two-minute match where the relay alone gave 7 and 65 rollbacks and 4 stalls. With 20% datagram loss
the link still carried the match; forced TURN gave a 23-24 ms round trip.

**Wi-Fi.** A machine whose radio leaves the air loses everything sent to or from it for that long.
Rollback hides up to `input_delay + max_rollback` frames of it (9 frames, about 150 ms, at the
default 2 + 7); past that the
other side stalls. No build fixes a radio's dead time. The fix is the player's: a cable on the
machine whose radio drops out, or Bluetooth off, or the adapter's power saving off, or another
channel. To find whose radio it is, run a 10 Hz router ping on each machine during a match and line
the stall seconds (`stats.jsonl` from `xplat.py`) up against each ping's spikes.

Not yet: playing on through a room disconnect while direct links are up; using the link's round trip
in the delay rule (it uses the relay's); a binary packet codec.

Tests: `OrcaDirectLink*`, `OrcaRoom.TicketSwitchesDirectLinks`, `OrcaSessionDirect*`,
`OrcaLive.DirectLinkBetweenTwoRooms`, `Tools/orca/direct-link.py`.

## Input delay

`Orca/Session/Session.cpp`, "Input delay". Two frames by default, like Slippi. Rollback hides the
rest of the link, jitter and spikes included: a spike past the rollback window costs a stall there,
not a frame of delay on every input all match long. The embed command `delay N` (1-6) fixes it
instead.

The adaptive delay rises only for what rollback can't cover, and only where a little delay cures it:

- **The typical link.** The median of this player's latest 15 round trips to the relay (one a
  second) plus the slowest friend's median, halved for one way. The delay covers what the 7-frame
  window leaves of that, keeping a frame for the boundary and one for jitter: two frames cover a
  round trip of about 240 ms between the players. A 4 ms hysteresis around each frame boundary keeps
  it from flipping.
- **Stalls a little more delay would have spared.** A friend stalls when this player's inputs outran
  its rollback window, so only this player's delay can help. Each side times its own stalls and
  reports how many were over within one, two, three and four frames (`"sp"`). A raise of k frames is
  worth it once it would have spared stalls in 8 seconds per frame of the last 45: a frame on every
  input, against a stall saved every five or six seconds.
  - Lateness a frame or two cures (a slow, bursty uplink; dropouts just past the window every few
    seconds) raises the delay within half a minute or so.
  - Random spikes that outlast the window by anything up to six frames raise nothing: a frame more
    would spare only the shortest. They cost their stalls, as they would in Slippi.
  - Stalls don't count around a hitch on either machine (a frame that took this machine over a
    frame period more than the game itself took) or a frame a game load slowed here, when this
    player's own stall set them off, while a controller is catching up, or in the first 10 s after
    a plug-in.
- Rollbacks and time-sync waits never move the delay.
- **Coming down.** A link-raised delay follows the link down after 3 s. A stall-raised one steps
  down after 5 quiet seconds, then a frame each further quiet second while each packet's reach (how
  far the friend ran past this player's inputs, plus the delay) shows two frames of room. Stalls
  that come back within a minute undo the step and double the next wait (up to 40 s).
- Every change is logged with its reason, e.g. `input delay 2 -> 3 at frame 5400: 1 frame more
  would have spared the others' stalls on this player's inputs in 8 of the last 45 s`.

Why this rule: an earlier rule raised both players on any burst of re-run frames or two stalls in
three seconds, and needed ten quiet seconds to come down, which a match never has. A Wi-Fi link with
spikes every ~11 s sat at delay 4-5 all match with barely fewer rollbacks than at 2. Counting
stalled seconds can't tell a bursty uplink from random spikes (both stall in 5-11 of every 30 s);
how long each stall lasts can. `SessionTest` covers each kind of link, simulated over 16 draws.

`ping` in `orca stats` is this machine's own round trip to the relay, not between the players; one
way between them is about half the sum of the two. `pmed` and `fpmed` are the medians the rule uses.

## Input latency

Between a press and the frame that shows it, and what Orca does about each part. Online adds the
input delay on top (2 frames of 16.68 ms).

| Change | Where | Changes emulation | What it does |
|---|---|---|---|
| Immediate XFB | `Orca/Profile.cpp` | no | Presents each frame at its XFB copy instead of 18 ms later at the VI field that scans it out. |
| VSync off | `Orca/Profile.cpp` | no | Forced: VSync only adds 2-19 ms in a composited window and could let the screen pace the game. |
| Smooth Early Presentation | `Orca/Profile.cpp` | no | Evens presents out for about 2 ms: a 60 Hz screen repeats and drops a third as many frames. |
| No re-present on rollback loads | `Source/Core/VideoCommon/Present.cpp` | no | A rollback no longer flashes the snapshot's stale frame. |
| Slow screens drop frames | `Source/Core/VideoBackends/Metal/MTLGfx.mm` | no | A screen under 59 Hz no longer slows the game to its rate. |
| Thread priority | `Orca/ThreadPriority.cpp` | no | The CPU, room and controller threads run at interactive priority. |
| Brawl's input lag fix | `Data/Sys/Orca/RSBE01.patches` | yes (Brawl) | Brawl copies this frame's pads, not last frame's. |
| SI relatch | `HW/SI/SI.cpp`, `Rollback/Rollback.cpp` | yes | The game reads the pads the frame hook just set, not the previous hook's. |

The two that change emulation change the compatibility key, so builds with and without them never
meet.

**Result** (median of 82 jumps, 120 Hz screen, solo): Brawl's press-to-glass went from 104 ms to
53 ms, Project+'s from 87 ms to 53 ms; the image changes at press+2 frames in both.

### Presentation (host only)

- **Immediate XFB.** Brawl and Project+ copy their XFB about 15.8 ms into a frame and the VI field
  starts 18 ms later. It can't desync: re-run frames skip `ImmediateSwap`, the throttle it bypasses
  only sleeps, and per-frame RAM hashes are identical with it on and off. It also shows the
  corrected frame after a rollback at once.
- **Smooth Early Presentation.** Immediate XFB presents unevenly (4-6% of 60 Hz refreshes repeated a
  frame). Smooth presents at the VI time plus a slowly-following offset: about 2 ms later at the
  present call, but no later at the glass at the median and earlier at p90, because even presents
  catch the compositor's earlier slot. A session never makes a stall's time up (pacing resets when a
  stall ends), so frames after a stall pay the same 2 ms as any other.
- **No re-present on rollback loads.** Rollback snapshots skip `Presenter::DoState`'s redisplay;
  savestates and keyframes still show their frame.
- **Rollback and the vertex buffer.** At the frame boundary the GPU is mid-frame (Brawl and Project+
  are 170-350 draws past their last XFB copy), so the frame shown after a rollback mixes the old
  timeline's draws and the new one's; that stays. But a batch still buffered in the vertex manager
  was flushed after the snapshot's XF and TEV state had been restored, so it was drawn through the
  wrong matrices (one-frame triangle spikes from a character as hits land). Loads now flush it first.
- **Slow screens** (Metal). On a 50 Hz screen `nextDrawable` held the game at 50 fps, dragging the
  friend down too. Drawables now come from a background acquire and a frame without one isn't shown,
  while the screen runs under 59 Hz or `nextDrawable` keeps waiting.
- **Thread priority.** `QOS_CLASS_USER_INTERACTIVE` on macOS; on Windows a raised priority with power
  throttling off. Only in a session. It cut the worst frame under heavy load (98-106 ms against
  121-158 ms) but showed no consistent p99 gain on a fast Mac.

### Audio (host only)

The buffer stays at Dolphin's 80 ms. The game hands the mixer a frame's audio in one burst and a
rollback's re-run holds the next burst back by up to ~15 ms. At 80 ms a minute of match with
rollbacks had 0-1 underflows; at 40 ms 5-32, at 32 ms 174. Sound trails the picture by about
50-65 ms.

### Emulation

- **Brawl's Controller Input Lag Fix** (the community code by Magus, also in Dolphin's
  `GameSettings/RSBE01.ini` netplay set and in Project+'s codeset). Brawl's pad code at `0x8002AD88`
  copies each pad's status from a frame-old array; the fix reads the current one. Orca writes it as
  one guarded group of 45 words. Project+ already runs it.
- **SI relatch.** The game reads its pads from the SI input buffers, which the SI's last poll filled
  just before the frame hook, so every frame played the previous hook's pads: "delay 2" acted like 3.
  At the end of the frame hook, `SerialInterfaceManager::RelatchInputs` writes each channel's input
  buffer from the pads just set, leaving the status register alone. It is deterministic: a pure
  function of the session's pads, applied after the boundary's snapshot was saved or loaded, so every
  load relatches.

Measure with a latency-probe build: a scripted press, frame dumps to find the first changed image,
and timestamps from the sampling boundary to the present call and to CoreAnimation's
`presentedTime`. Presentation must be measured in a `-p macos` window (headless Metal presents
nothing), with a warm shader cache.

## Music switch

`music on|off` on stdin (cap `music`), or `ORCA_MUSIC=off` for a harness run: the player's own
Music switch. Off leaves the game's music out of what this machine plays and changes nothing the
game sees, so two players with different switches stay in sync. It is not in the compatibility key.
Offered for Brawl (any revision) and Project+; without the cap the line answers `unsupported music`.

`Orca/Music.*`, `HW/DSPHLE/UCodes/AXWii.cpp`, `HW/DSP.cpp`:

- **Music is the stream voices.** Brawl and Project+ play their music (BRSTM) on AX voices of type
  stream (`AXPBWii::is_stream` 1): a looping stereo pair over a ring buffer in MEM2, one voice on
  main left, one on main right, no AUX sends. Sound effects are normal voices (type 0), looping ones
  too. Seen on every voice of the default Brawl and Project+ runs (menus, character and stage
  select, a match).
- **RAM gets the same mix.** AX HLE mixes every voice as before and writes the frame (96 samples,
  3 ms) to RAM as before. With the switch off it also keeps what the stream voices added to the
  main bus (a voice's share is additive, `MixAdd`), gives that the compressor's gain too, and mixes
  the frame again without it: same volume ramp, same clamp.
- **Only the speakers get the second mix.** The AI DMA reads the frame from RAM for the host mixer.
  While RAM still holds the bytes the mixer wrote there (Brawl: `0x804E84C0` and `0x804E8640`,
  read by the DMA as `0x004E84C0`), it plays the quiet copy instead; anything else (a rollback
  load, a game write) plays RAM. The last 32 frames are kept.
- **Not RAM.** The game's own volume or options would change memory every peer hashes, and a
  session starts from an empty NAND where a joiner plays on the host's save.

**Checked** (2026-10-06, Mac):

- Hashlogs of the default inputs (Brawl to 10800, Project+ to 9000) with the music on, with it off,
  and of 0.3.27: equal on every frame.
- Sync tests (`YG_SYNCTEST=7`) with it off: 0 RAM mismatches (Brawl 73451 re-run frames, Project+
  60851).
- What Orca played (`Dolphin.DSP.DumpAudio`, the same run on and off, sample-aligned): where only
  music plays (character select after the match) RMS 2000-4000 drops to 0.7, the floor of a silent
  second; in the matches the effects stay (off/on RMS median 0.77 Brawl, 0.84 Project+); with no
  music playing the two dumps are identical. 99.99% of AI blocks went out quiet.
- Two Orcas, one with the music off (`queue-e2e.mjs --q2 --queue casual --music-off ada
  --dump-audio`): Brawl passed with 102 checksums matched on each side, Project+ with 103, no
  desync; the music-off side's dump was silent for 71 of 157 s (Brawl) against 25 for the other.
- Again on the 0.3.29 candidate: hashlogs on = off (Brawl Rev 2 frames 0-10807, Project+ 0-9003),
  Rev 1 off = Rev 2 on (0-10819); a sync test off, 0 RAM mismatches; two Orcas, Project+, one off:
  104 checksums each. A voice probe of the Brawl run with it off: the results screen's music is the
  stream pair too (the announcer and the crowd are normal voices), and no stream voice of the run
  sends to an AUX bus. Cost: instructions retired by the whole run to frame 9000 (a snapshot every
  frame from 5900) are the same on and off, within the 0.1% between runs.

## Match start

The first seconds of a match (the countdown and just after) used to drop frames: two things compiled
on the one thread that runs the game in a session, the GPU pipelines the match draws first and the
match's PowerPC code. Both now compile before the match is on screen.

### Shaders

Sessions force (`Orca/Profile.cpp`, `Source/Core/VideoCommon/ShaderCache.cpp`):

- **Asynchronous ubershaders:** a pipeline never drawn compiles in the background and is drawn with
  an ubershader meanwhile. Dolphin's default compiles it on the emulation thread, which in a session
  is also the GPU thread.
- **Compile before starting:** the ubershaders and every pipeline in `Cache/<game>.uidcache` compile
  on all cores before the first frame, the UID cache first (it is what the match draws and is far
  quicker than the ubershaders).
- **EFB copy pipelines before the first frame:** no Dolphin cache or ubershader covers them, so Orca
  keeps their UIDs in `Cache/<game>.efbcopy.uidcache`.
- **A limit on the wait:** 60 s, or the app's `ORCA_SHADER_WAIT_S`. While it waits Orca prints
  `orca shaders <done> <total>` once a second, so the app can tell a slow boot from a hung one.
- **No ubershader compiled on the emulation thread:** a draw whose ubershader is itself still
  compiling (a boot that hit its limit) is skipped until something is ready, instead of a
  multi-second freeze.
- **FXC output kept across builds** (`Source/Core/VideoBackends/D3DCommon/FxcCache.*`, D3D11 and
  D3D12). Dolphin's compiled shader files are emptied by any other build. `Cache/Shaders/D3D-fxc.cache` maps a SHA-1 of
  exactly what FXC is given (compiler DLL hash, entry point, target, flags, macros, HLSL text) to
  the DXBC it returned. FXC is a pure function of that input, so a binary is only ever reused for
  the compile that made it. Records carry their own checksum; a torn write ends the read and the file
  is rewritten from the records before it. The file is opened shared, so two Orcas on one user folder
  can both use it.

Rendering only: nothing drawn reaches RAM, so none of this can make two players' games differ.

Measured on an M4, Metal: a cold synchronous match start had a worst frame of 1.5 s (Brawl); with
these settings and a warm cache the worst is about 23 ms. On Windows (D3D11) the 63 ubershader
stages take about 30-37 s of FXC on every thread after an update, which is why the boot waits and
why FXC's output is cached.

### The match's code (JIT warm-up)

`Orca/JitWarm.*`. The first match in a process compiles the match's code at first use: in Brawl
~16,500 blocks in the two frames where the stage and the first fighter appear (64-105 ms each on an
M4) and ~4,100 more when the second fighter enters during the countdown.

- **Recording.** In a match's first 600 frames, every block the game compiles is noted (its start,
  physical ranges and a hash of its words). Blocks whose code is rewritten within a second are never
  kept. The list is merged into `Cache/<profile>.jitwarm` (Brawl ~30,000 blocks, 1.2 MB); blocks
  unused for 24 matches age out.
- **Warm-up.** From the next match's first frame, at each boundary, up to 8 ms
  (`ORCA_JITWARM_BUDGET_US`) of compiling listed blocks whose words are in RAM again. Both games
  show a still loading image for a while after the match scene starts (Brawl to frame ~169,
  Project+ to frame 27), which hides the work.
- **Paced by when each block first ran.** Each block is due 3 frames before the recorded match first
  ran it, and a sweep goes past the 8 ms as far as the nearest deadline needs (never more than
  100 ms in one boundary). Without pacing, a PC's Jit64 (about 40 us a block) had compiled only
  ~5,000 blocks by the frame where Project+ runs ~12,600 more, which froze that frame for 374 ms.
- **A shipped list for players without one** (`Data/Sys/Orca/RSBE01.jitwarm`, `PPLUS32.jitwarm`):
  used only when the player's own file is missing, then replaced by it. Re-record both after a change
  to the games' patches, scene detection or how the JIT splits blocks: run the reference inputs
  (`bf-two-new.txt`, `pplus-1v1.txt`) with `ORCA_JITWARM_SEED=0 YG_SCENES=1` on a fresh user folder
  and copy `Cache/*.jitwarm` into `Data/Sys/Orca`. ARM64 and x86-64 record the same lists.
- **JIT tables grow early:** Jit64's backpatch map rehashes inside a compile as it doubles (up to
  26 ms), so `JitBase::ReserveForBlocks` makes room at the match's first frame.
- **Why it can't change the game.** A block compiled early is the block the game's first run would
  compile: the same words at the same addresses (the hash), the same flags, in the guest's FP mode,
  kept only if its instructions span the recorded ranges. Only BAT-mapped code is touched, and the
  JIT never compiles from the hook where it could clear its cache or reuse code memory under the
  running block (`CanCompileFromHook`). Per-frame RAM hashes, sync tests and loopbacks are unchanged
  with it on, on both JITs.

Result: on an M4 every match start is smooth from the first launch after an update (worst frame
22-25 ms in Brawl and about 31 ms in Project+, against 88-102 ms before). `ORCA_JITWARM_LOG=1` logs
the first 600 frames' compiles; `node Tools/orca/match-start.mjs --from-frame 28 <run>` (Project+;
Brawl 170) counts the frame times a player sees after the loading image.

A rollback in Project+ recompiles 40-120 blocks (pages with code also hold data that changes every
frame, and a restore drops every block in a restored page). Invalidating only changed 32-byte lines
would keep most of them (not done).

## Code the JIT doesn't see

Project+'s Gecko code handler applies `NETBOOST.GCT`'s RAM writes with plain stores (no `icbi`), and
the first time it writes them (frame 1) seven words of Brawl's code change, in functions the boot
already ran and the JIT already compiled:

| Words | Code | Effect of a JIT clear |
|---|---|---|
| `80023b88`, `80024028` | Move v-sync call for Brawl/PM | RAM differs the next frame |
| `8001cd24`, `8001cd2c`, `800266b8` | Fixed OSReport Syntax on File Reading | RAM differs at the next file read |
| `8018cfc4`, `8018cfc8` | Extra null pointer catch | none seen |

Nothing invalidates those blocks afterwards, and Dolphin's workaround for such code handlers is
spent during the boot. So a warm JIT runs Brawl's words for the whole process, and the first
whole-cache clear switches to Project+'s: a desync from the next frame. Orca never clears the JIT in
a session itself, but Dolphin does when its code space fills, rarely, and at different times on a Mac
and a PC.

**The fix:** `KeepGameCode = <address> <Brawl's word> <Project+'s word>` lines in `PPLUS32.ini`.
Every instruction fetch at those addresses (both JITs and the interpreters) gets Brawl's word while
RAM holds Project+'s, so any JIT clear compiles what ran before, and RAM, its hashes and the codes'
own reads are unchanged. `JitClearFrame = 2` adds one clear at the end of frame 2, so every machine
compiles from then on whatever a fetch reads, whatever its boot compiled.

Running Project+'s v-sync move for real would cut about 13 ms of latency, but it doubled the 60 Hz
repeats and drops whenever there were rollbacks (a re-run's time lands on that frame's present and
Smooth Early Presentation's offset follows it), so Orca keeps what earlier builds ran.

**Checking a codeset** (a Project+ update, another launcher profile): `Tools/orca/jit-history.py
<nogui> <disc> --pplus <dol>` runs the boot twice (as it comes, and with JIT clears), compares the
hash logs and lists every word a live block runs that a fetch no longer reads. List such words as
`KeepGameCode`.

## Game patches

`Data/Sys/Orca/<profile>.patches` (`Orca/UX/GamePatches.h`): Orca's own changes to the game's code
and data, forced on in every session (the player's own codes are dropped) and hashed into the
compatibility key. They are applied at every frame boundary from the frame hook, first runs and
re-runs, as a pure function of emulated memory. One 32-bit word per line, in hex:

| Kind | Lines | Written |
|---|---|---|
| Guarded group | `<address> <original> <value>`, consecutive words | once, while every word is its original; a line whose original equals its value only guards |
| Data | `<address> * <value>` | every frame |
| Toggled group | guarded lines between `when` lines and `end` | its values while the conditions hold, its originals while they don't |

```
when 806F2380 == 59474D31               # the match block's first word is 'YGM1'
when 806F2384 & FF000000 != 00000000    # and its masked word isn't 0
8119A120 2C030000 2C030000              # cmpwi r3, 0 (guard)
8119A124 4082000C 4800000C              # bne -> b while both hold
end
```

- A `when` reads one aligned word of MEM1 or MEM2 at fixed address; an unmapped address never holds.
- A group whose words are neither all originals nor all values (its module not loaded) is left alone.
- Every write goes through Dolphin's memory-patch path, which drops the JIT's copy.
- Toggled groups are applied last in the frame hook, so a condition set at a boundary is followed
  at that boundary.
- The loader refuses: a `*` line in a block, nesting, an `end` without `when`, a mask of 0, a
  condition that reads a word the file writes, and a toggled group of one word (a lone word matches
  other code by chance too easily: add guard words).
- Never patch a word that game code also writes. Project+'s code handler rewrites its hooks every
  frame, so a toggle there would land and be undone within each frame, and what the JIT runs would
  depend on its history.

Real code goes into a code cave ([Free space](#free-space-and-the-match-block)). Tests:
`OrcaGamePatches*`.

## Free space and the match block

The online rules need room in the game's own memory: a **match block** that every rollback snapshot
and drop-in keyframe carries, and **code caves** for patches. Both come from one place, the same in
Brawl rev 2 and Project+ v3.2: the dead code of Brawl's online sequences in `sora_scene.rel`
(`Orca/UX/FreeSpace.h`).

| What | Where | Size |
|---|---|---|
| Match block (data only) | `0x806F2380`-`0x806F2618`, in sqNetAnyOkiraku::setNext | 664 bytes |
| Code caves | 12 runs from `0x806F2674` to `0x806F82A4` (listed in the header) | 16,908 bytes |

**Why it is free.** sora_scene is loaded once at boot (`0x806BB480`) and never reloaded, and
Project+ ships no copy of its own. The online sequences start only from the main menu's exit codes
24-27 and 29-31, which Orca's patches redirect ([Online menu](#online-menu)). Every reference into
these runs, in every module on the disc and in Project+'s modules and plugins, comes from those
sequences' own tables. Left out: each sequence's create function (it runs at boot), 25 words the
loader relocates against `sora_melee`, and `0x806F3DA4`, which Project+'s codeset nops every frame.

**Proven** by `Tools/orca/free-space.py`, which runs one loop through every scene online play
visits, twice: once watching the block and caves (they keep the game's own bytes, and nothing writes
them), once with them filled with illegal instructions (none ever runs, the fill survives, and every
RAM dump is byte-identical to the first run's outside the block and caves). Sync tests and keyframe
tests pass with the fill.

**Using it.** Before Orca writes it, the block holds code (its first word is `li r17, 0`), so
readers check the magic. Only Orca's frame hook writes the block, never a patch group. A cave is a
guarded patch group whose originals are the dead code there; any MEM1 address is a relative branch
away.

**Layout** (offsets from `0x806F2380`, big-endian; `Orca/UX/MatchBlock.h`):

| Offset | What |
|---|---|
| `+0x00` | Header: `YGM1`, version 1, mode (0 none, 1 casual, 2 ranked), ruleset (1 Brawl Supernova 2025, 2 Project+ 2024), `+0x07` coin, `+0x08` room hash, `+0x0C` queue flags |
| `+0x10`-`+0x8F` | Project+'s stage flow (`MatchBlock::State`) |
| `+0x80`-`+0xBF` | Brawl's stage flow (`BrawlStages`; each game uses its own range) |
| `+0xC0`-`+0xCF` | Ranked ready timer, no-show, plugged ports |
| `+0xD0`-`+0x1A7` | The player's own rules, saved when the header was written (Project+'s stage switch data from `+0x100`); `+0xF4`-`+0xFF` are the stage cursors' shared bytes |
| `+0x1B4`-`+0x1BF` | The two stage cursors |
| `+0x1C0`-`+0x1EF` | The queue's character select |
| `+0x1F0` | Saved rules' flags |
| `+0x200`-`+0x20F` | Character order |
| `+0x210`-`+0x28F` | The ranked set (`SetBlock`, authoritative) |
| `+0x290`-`+0x297` | A friend's move from the menus |

Any new header (another mode, ruleset, coin or room) starts every track's state fresh. Compatibility:
anything that changes what the game computes bumps `UX::kCompatVersion` (currently `ux=21`), which is
in the compatibility key.

## Input gate

The queue rules take input away from a player: the port whose turn it isn't, B on the stage select,
a locked character. The gate does that per port and per frame where the game reads its controllers,
so no game code has to be patched per control (`Rollback/InputGate.*`).

- **A mask** makes buttons read released (L and R also zero their analog value) and sticks centred.
  An unplugged port stays unplugged. A mask can also **press** buttons for the game
  (`Mask::press`), **steer** a stick (`Mask::steer`), or read the stick centred while A is down
  (`a_centres_stick`), so an A acts where the hand stood. `drop_with_lr` releases buttons only
  while L and R are both held (pressed, or a quarter press): Project+'s Code Menu chord.
- **Where.** `Rollback::InputOverride`, called for every SI poll and for the relatch. Everything else
  stays raw: the local pad sampled for the wire, the session's pads, the harness's recording. Both
  machines apply the same masks to the same pads, and a re-run applies them again.
- **When.** One source (`InputGate::SetSource`, `UX.cpp`), a pure function of emulated memory,
  evaluated as the frame hook's last step, after any load at that boundary. So a frame's masks
  always come from the state that frame starts from, and nothing of them needs to be in snapshots.
  The source may not write memory; what it needs from the session (ports, names) the frame hook
  writes into memory earlier at the same boundary.
- **The latch** (`InputGate::SetLatch`): the raw pads of ports 1-2 for the frame about to run,
  written into the match block after the masks, so the rules can see presses the game didn't (Start
  to lock in, B, Z, the stage cursors' sticks). It runs after the boundary's snapshot or keyframe was
  saved or loaded, on every pass, so the next boundary's hook reads the same bytes everywhere.
- Every session masks Z on the results screen (below); everything else comes from a queue header.

Test knob: `ORCA_TEST_GATE=<port>:<buttons>[:always|mem/<n>],...`, e.g.
`ORCA_TEST_GATE=1:A+STICK+CSTICK+X+Y+Z+R:mem/8,2:START`. `mem/n` masks on one frame in n chosen from
a hash of game memory, so masks computed from the wrong frame show up in a sync test. Tests:
`OrcaInputGate*`. Evaluating the masks where the frame callback runs (before a load) failed the sync
test with 385 RAM mismatches; where they are now, 0.

## Online menu

Play Online works in both games, through the game's own menus and never a new screen; no Nintendo
server is ever asked for anything; nothing about connecting looks like Nintendo's.

- **The top page.** Brawl's Wi-Fi button reads PLAY ONLINE (Project+'s already does), with "Play
  different modes online." under it.
- **A opens the game's own ONLINE page** (With Friends / With Anyone) with no connection window: no
  save check, no NAND access, no network task. B goes straight back. Options (WiiConnect24) and
  Spectator never show.
- **With Anyone** shows CASUAL and RANKED. Either goes to the local Versus character select and Orca
  prints `orca menu online casual|ranked` (with the app's `host` cap; `... local` without).
- **With Friends** goes straight to the local Versus character select, where friends drop in, and
  prints `orca menu online friends`. ("Play with friends you invite! (2-4 players)".)
- **B out of that character select** goes back to the page and button the pick was made on, as the
  game does after Group > Brawl. It holds after matches too.

**How** (`Data/Sys/Orca/RSBE01.patches`, `PPLUS32.patches`, `Orca/UX/MenuText.h`, `OnlineMenu.h`):

- **Game patches** (guarded groups; Project+'s menu module sits `0x1480` above Brawl's): the top
  page's press takes the path every other button takes, never creating the connection window; the
  ONLINE page's B does what the game does after its disconnect window says OK; A on With Friends
  leaves the menu with exit code 25; Options and Spectator are never enabled; the main menu's exit
  table sends codes 24-27, 29, 30 (Casual) and 31 (Ranked) to Orca's block, which starts the local
  Versus scene. No online scene, network thread or DWC is ever entered.
- **The back out.** Orca's block keeps the exit code in sqVsMelee's object (`+0x18`, slack in its
  heap block); on the way back a helper turns it into the main menu's argument for that page and
  button. It is game memory, so rollback and keyframes carry it.
- **Text** (`MenuText.h`): the main menu's messages that mention Nintendo WFC, Project+'s
  "(Discontinued)" and the rewritten descriptions are replaced in emulated memory, only while the
  bytes are exactly the game's own, into the same slot.
- **Textures** (`Data/Sys/Orca/Textures/RSBE01`, `Tools/orca/textures/`): Orca's own label art
  (PLAY ONLINE, ONLINE, CASUAL, RANKED), drawn by `make-labels.py` with SIL OFL fonts. A session
  forces texture replacement on and loads Orca's pack with the player's own; Orca's names win, so a
  player's HD pack can never bring back the Nintendo WFC labels. Textures only change what is drawn,
  so the pack is not in the compatibility key.
- **Events** (`OnlineMenu.h`): a reader in the frame hook follows the menu at every first-run
  boundary and reads the exit code. It prints an event only for a pick made while the player's game
  is alone (no friend plugged in or on the way, nothing of drop-in pending); a Casual or Ranked pick
  made with friends goes to the session instead (below). Going solo or loading a keyframe starts it
  over, so nothing stale is printed.
- **No IOS network access:** NET_KD_REQ, IP/Top and SSL refuse in a session, and
  `Tools/orca/online-menu.py` fails on any `/dev/net` open, network request or NAND access to the
  save, WiiConnect24 or network folders after boot.

| stdout | When |
|---|---|
| `orca menu online casual` / `ranked` | With Anyone > Casual or Ranked, with the app's `host`. |
| `orca menu online casual kept` / `ranked kept` | The same with friends in a host's game, kept because the app can't search that queue now: nothing armed, never a search. Printed again unmarked once the friends are gone. |
| `orca menu online casual local` / `ranked local` | The same without `host`: nothing searches. |
| `orca menu online friends` | With Friends. |
| `orca menu cancel` | The player backed out of searching, alone. Once. Also when a friend on the way, or a friends room joined, ends the queue ([Play again](#play-again-after-a-ranked-set)). |

### Casual or Ranked with friends in the game

With Anyone > Casual or Ranked leaves the friends lobby for a 2-player room the matchmaker fills
(`OnlineMatch.cpp` `TakeLobbyPickNow`, `DecideLobbyPick`, `ArmKeptPick`). The reader hands such a
pick (never With Friends) to the session at the boundary that saw it, after the app's commands
(`OnlineMenu.h` `TakeLobbyPick`). Either player's press picks in a shared menu, and every game sees
it at the same frame and goes to the same Versus character select.

- **The host** (port 1, with the app's `host` and the queue's pick cap, `pick-casual` or
  `pick-ranked`), with friends in its game, waiting for a keyframe or arriving, leaves its room as
  the app's Leave does, prints `orca state left lobby`, shows "Left the lobby for Ranked" (or
  Casual), then announces the pick (`orca menu online casual|ranked`). From the next boundary it
  plays alone on port 1, so the queue's own character select gets its header and Start readies. An
  arrival not taken in yet counts as a friend: taken in later, it would refuse the queue's header.
- **Each friend** lets the pick go (the game is its host's), hears `orca state host-left` and goes
  home on that character select ([Going home](#going-home)). There it reads the pick the select was
  opened for (`CssTitle::ReadPick`, 30 or 31: `OnlineMenu.h` `CssPick`) and announces it too,
  whatever its own app can search, so its Start readies it without backing out.
- **A queue the app can't search now** (it answers `pick-casual` / `pick-ranked` in its `caps` only
  for a queue it can search; Ranked needs a signed-in player): leaving would drop the friends for
  nothing. The host keeps them and prints the pick in place, unarmed (`orca menu online ranked
  kept`): no queue select, no search, no queue header. The app answers with its reason and
  `queue-cancel`, which changes nothing. Once the friends are gone and the host's game is alone on
  that select (`DecideKeptPick`), the pick is armed as an alone pick (`orca menu online ranked`);
  it is dropped once the game moves on from that select (`PickStands`) or a queue or search starts
  anyway. The app sends `caps` again when what it can search changes; a later `caps` line replaces
  the whole answer (`Status::SetAppCaps`).
- **Nobody to leave by now** (the app's own leave went first, the last friend has gone, or only a
  keyframe is kept for an invite): the pick is announced in place with no `left lobby`, and a kept
  keyframe is dropped (never while a session still runs). A friend who comes later ends that queue
  first ([Play again](#play-again-after-a-ranked-set)).
- Without the app's `host` nothing would search, so the friends' game stays theirs and nothing is
  printed. A queue room's menus are locked.
- A pick seen at a boundary the commands can't take (mid re-run) waits for the next while it still
  stands (`PickStands`), and is dropped once the game has moved on from it.

Local: the session ends at the pick's boundary, so only the room's leave reaches the friends' games,
and the compatibility key doesn't change. Tests: `OrcaOnlineMenuLobby.*`, `Tools/orca/dropin-rooms.py`
steps 6-10.

### Boot and friends from the menus

Booting either game lands on the main menu. Brawl's patches skip the strap, the save prompt and the
title. Project+'s codeset has "Boot Directly to CSS"; one guarded group over `NETBOOST.GCT`'s words in
memory (`0x80559C20`) makes its default case go to the title and main menu as vanilla Brawl does.
Project+ reaches its main menu at frame 197.

**A friend who drops in while the host is anywhere but Versus** (`OnlineMenu.h` `FriendsMove`): once
the main menu has built its pages and run 30 frames, the frame hook leaves the menu with exit code 25,
as With Friends does, so both games go to the character select at the same boundary. It counts from
the menu's build (`+0xAC8` of muMenuMain), not its start: moving before the build made the menu's
exit call destructors through uninitialised page slots and froze both games. Never in a queue room.
That character select is online, so its CPUs go and a friend whose panel had one takes it at once
([No CPUs online](#no-cpus-online)).

**A join held in a single-player mode:** while the host plays Classic, All-Star, Events, Training,
the Subspace Emissary and the like (`SequenceHoldsDropIn`), a friend who arrives waits: no keyframe
is made. The host prints `orca state friend-holding`, the joiner `orca state friend-waiting` and
shows that the host is in a single-player mode. The join's 90 s limit doesn't run meanwhile.

## Matchmaking and results

With Anyone > Casual or Ranked plays a stranger YouGame's queue found (Elo and distance windows, the
same Orca build and disc), then the existing drop-in into the matched room. Ranked is a best of 3,
rated once per set.

### The flow

1. The pick prints `orca menu online casual|ranked`. The game is on the queue's own character select
   ([Character select](#character-select)); the page searches once the player is ready.
2. The page queues a ticket in the queue's mode with Orca's compatibility key. On a match, slot 0 gets
   `host <code>` and the other `join <code>` 1.5 s later.
3. `host <code>` works only while the game is alone (else `unsupported host`). The game hosts the
   room where it is. Once the room's welcome names its queue, the next safe boundary writes the
   match block's header; the opponent's keyframe waits for it.
4. `join <code>` is the ordinary drop-in: the joiner lands on port 2 of the host's character select.
5. Once the opponent has plugged in, the host begins the room's match (`{"t":"begin"}`); both learn
   the match id from `start`.
6. Each game's result is read from memory once its frames are final and reported by both Orcas, the
   same bytes. Casual: each game is a `finish`, the room stays. Ranked: each game is a `game-report`;
   when one player has 2 agreed wins both send `finish {winner}`; 7 games without that, `finish
   {void}`. The room settles each report by agreement on the outcome; different outcomes void the
   game.
7. After a ranked set's verdict the room stays open for up to five minutes (`set-over`) so the
   players can talk; each Orca leaves when its player moves on ([On-screen UI](#on-screen-ui)).

Leaving a ranked set is a forfeit (the room's 15 s grace). A desync voids the match in progress
before the players part.

| stdin | |
|---|---|
| `host <code>` | Host the matched room. Refused: `unsupported host`. |
| `queue-cancel` | The page's search ended. |

| stdout | |
|---|---|
| `orca caps ... host results locks queue2 pick-casual pick-ranked` | `results` where the result reader is verified; `locks`, `queue2` and the pick caps where the profile has a ruleset (Brawl rev 2 and Project+). The app answers a pick cap only for a queue it can search now. With `music` that is 16 caps, the most an app before the Music switch's release passes (that release takes 24); more needs it as the minimum app (`OrcaOnlineMenuLobby.EveryCapFitsTheAppsCapsLine`). |
| `orca result <json>` | The room's verdict, when it arrives. At most 300 bytes. |
| `orca state friend-left match-over` / `host-left match-over` | A ranked room closed after its set. |

`orca result` (the local player first; `d` the settled detail):

- casual game: `{"q":"casual","k":"game","n":3,"out":"won"|"lost"|"draw"|"void","why"?:...,"d"?:{"st":2,"c":[mine,theirs],"s":[mine,theirs],"to":0|1}}`
- ranked game: `{"q":"ranked","k":"game","n":2,"out":...,"score":[mine,theirs],"d"?:{...}}`
- ranked set: `{"q":"ranked","k":"set","out":"won"|"lost"|"void","by":"games"|"forfeit","score":[...],"r"?:[before,after],"rank"?:"..."}`

Room messages sent (queue rooms only; friends rooms never begin or report): ranked
`{"t":"game-report","matchId":M,"id":"g<n>","winner":"<participant id>","detail":D}` (or `"draw"` or
`"void"`), casual `{"t":"finish","matchId":M,"winner":...,"detail":D}`, a set's `finish`. D is
`{"f","st","c","s","to"}`: the session frame of the first final results-screen reading, the stage,
characters, stocks left and whether time ran out, in port order. Handled: `start`, `game-result`,
`result`, `set-over`, `command-error`, and the socket's 4004 close.

### Reading results

`Orca/UX/Results.*`, read only. At every boundary the frame hook stores a small reading of the frame
just run: the scene (`scMelee` the fight, `scMemoryChange` between, `scVsResult` the results) and, on
the results screen, the result info. OnlineMatch consumes readings in frame order once their frames
are final (`Session::ConfirmedFrame`), so both machines see the same readings. A game is a fight
entered after the opponent's plug frame, ended by the results screen or by any other scene (void
`nocontest`). The verdict needs exactly two humans on ports 1 and 2 (else void `ports`) and a fight
that ended normally.

Where (Brawl rev 2; Project+ runs the same executable): `g_GameGlobal` `0x805A00E0` points to the
mode data (`+0x08`: stage, time limit, per-port init data of `0x5C` bytes), the result info (`+0x18`:
per-port records of `0x2AC` bytes at `+0x24` with character, stocks left, place, KOs, falls; the
decision at `+0x1378`: 9 during a fight, 1 when time runs out, 2 at a deciding KO), and the rules
(`+0x1C`, gmSetRule: mode, minutes, stocks, items).

## Online rules

`Orca/UX/OnlineRules.*`, `MatchBlock.h`. Nothing here acts without a match block header, and only a
queue room's host (or the queue's own character select) writes one. Friends rooms are unchanged.

**The header** is written only at a boundary no other game runs from a state before it: one where
the game is its player's alone, or one where an arriving opponent still waits for a keyframe not yet
made (`HeaderFreeAt`). A keyframe for an arrival is made only once memory carries the header the
room calls for (`Rules::HeaderInPlace`); if that takes over 5 s the host leaves with `orca error
mismatch`. The joiner checks the header after its keyframe loads (`Rules::HeaderFitsRoom`: mode,
ruleset, coin, room hash, queue flags) and leaves on any difference. Casual unlocks when the
opponent unplugs; ranked stays locked while the set is open.

**The locks** (first runs and re-runs, only bytes that differ):

| | Brawl (Supernova 2025) | Project+ (2024) |
|---|---|---|
| Rules | stock, 3 stocks, 8:00, handicap off, team attack on, pause off | the same with 4 stocks |
| Items | none | none |
| Legal stages | Battlefield, Final Destination, Delfino, Yoshi's Island, Lylat, Smashville, Pokemon Stadium; FD off while a human picked Ice Climbers | the 2024 Proposed list (preset 3, `Switch03.rss`): 9 stages, hazards off |
| Input gate | character select: A only in the grid, B only to take the token back up; stage select: every port while the two cursors run (they act instead); fight: Start | the same, plus the D-pad on the character select, and D-pad Down while L and R are held in every scene (the Code Menu's chord) |
| Code Menu | (none) | off, closed, every cheat and gameplay value at its default (below) |
| Fighters | any | Giga Bowser and Wario-Man play as Bowser and Wario |

- **No way back to the menus.** A toggled patch group conditioned on the header flips the character
  select's "back to the menu" exit to "character select again", so BACK reloads the select.
- **Ranked's no-ready timer:** before the set's first fight, once one player has picked and the
  other hasn't, 60 s; then that port is the no-show, whose own Orca leaves (a forfeit).
- **Stalls:** a ranked player whose game has waited 15 s on the opponent's inputs, or whose opponent
  went silent, claims the set (`Online::ReportStall`); the room lets it stand when the other side has
  been silent too.

Test knob: `ORCA_TEST_QUEUE=casual|ranked[:<coin>][:q2|:solo]` writes the header as a queue room's
host would, in a harness run or a dev room. Tests: `OrcaOnlineRules*`.

### Project+'s Code Menu and banned fighters

Project+'s netplay codeset carries its Code Menu (`pf/menu3/dnet.cmnu`, loaded as is to
`0x804E0000`). Any port opens it with L+R+D-pad Down in any scene, mid-fight too, and it pauses both
games while open; from it a player could turn on flight, infinite shield and debug displays, swap a
port's character (Giga Bowser, Wario-Man) and change hitstun, shield and other constants, all in
sync. Its character select also turns a Bowser or Wario pick into Giga Bowser or Wario-Man, both
banned, when the player holds shield (L) as the select ends. Under any locking header (casual,
ranked, the queue's own character select), since `ux=20`:

- **The menu is off and closed.** Code Menu Activation is OFF, its state is closed (an open menu
  closes as its own Start does and the game's pause word comes back), and 73 cheat and gameplay
  values are held at the file's defaults at every boundary. The four Character Select lines follow
  the fighters and are left alone; so are the replay, tag, costume, crowd and HUD colour lines.
  Nothing is written unless the memory has the pinned release's layout (`Rules::CodeMenuPresent`).
- **The chord is dropped:** the gate drops D-pad Down while L and R both read held (button, or
  analog at a quarter press), in every scene (`drop_with_lr`). That also stops the debug codes' own
  L+R+Down toggle. Activation OFF is what keeps the menu shut; the drop is a backup.
- **Banned fighters** become Bowser (`0x0C`) and Wario (`0x17`) on the character select, the stage
  select and between scenes, in the match's init data and in the character select's record
  (gmSelCharData), which the way to the fight copies into the init data again.
- The header saves the player's own activation and its clear gives it back; the values stay at
  Project+'s defaults afterwards. Friends rooms keep the menu as it is.

Tests: `OrcaOnlineRules.*CodeMenu*`, `OrcaOnlineRules.ProjectPlus*`,
`OrcaInputGate.DownDropsOnlyWhileLAndRAreBothHeld`. Two Orcas: `queue-e2e.mjs --pplus <dol> --q2
--queue ranked --pick bowser --giga --chord`. More in ORCA_NOTES.md, "Project+'s Code Menu".

## No CPUs online

No CPU exists in any online mode, and online character selects show only the seats in use
(`ux=19`, `Orca/UX/OnlineSeats.*`). Local Versus (Group > Brawl, offline) keeps its CPUs.

**Where a CPU came from** (found under the harness with `ORCA_UX_PROBE` watches, both games):

- The Versus character select builds its panels from a record the game keeps between visits,
  gmSelCharData (`[0x805A00E0]+0x10`, `0x90180B40` in both games; each port at `+0xB8 + port x
  0x5C`: `+0` the character, `+1` the player's state, 0 a human, 1 a CPU, 3 nobody). While building,
  the select reads each port's state (`0x80685AFC`) into the player area's kind (`+0x1B4`: 0 empty,
  1 human, 2 CPU). It saves its panels back into the record only as it goes on to the stage select
  (`0x806878D4`), never when B leaves to the menu.
- Vanilla Brawl resets the record's players whenever Versus starts from the main menu; Project+
  doesn't. So in Project+ a CPU from a local game that reached the stage select came back on the
  online selects and into the match (a three-way game 1, or a CPU on panel 2 the joiner could never
  take). In Brawl, A on an empty panel's player-type button made a CPU on With Friends' select.
- The stage select's end copies the record into the fight's players (sqVsMelee, `0x806DCF6C`, every
  port whose state isn't 3), overwriting the fight's own data.

**What is online** (`OnlineSeats::Online`, a pure function of memory and the synced ports): the match
block's header locks, or the main menu's pick kept in sqVsMelee (`+0x18`) is With Friends (24-27),
Casual (30) or Ranked (31), or two or more ports are plugged in (a friend dropped in, wherever the
host was).

**What Orca writes there** (the frame hook, first runs and re-runs, only bytes that differ):

- **The record**, only between two scenes (`scMemoryChange`): a CPU's state becomes nobody's (3),
  and so does a human's on a port nobody plugged in. The select then starts with only the players
  who are there. Never on the stage select or in a fight: by then the copy into the fight is made,
  so no fight loses a player to this. Characters stay; only states change.
- **The panels**, on the character select: a CPU's (kind 2) becomes empty (0), which the select
  saves as it leaves, so the CPU plays no fight. A human's panel whose port nobody plugged in (a
  friend who left) is left to the game, which empties it 17 frames after the controller goes.
- **The join** (the input gate, `OnlineSeats::PressesJoin`): a panel a CPU had can't be joined by
  moving the hand into the grid, since its token went with the CPU. The game's own way back is A on
  the panel's player-type button with that port's controller in. So on an online select without a
  header, Orca presses A for a plugged-in port whose own panel is empty, its token neither in the
  hand nor flying, while its hand rests on that panel's player-type button (hand `+0xB0`). A friend
  who drops into a select whose CPU was on their panel takes it at once.
- **The buttons** (the input gate): where no header locks (With Friends, a friend's drop-in, Casual
  or Ranked before the queue's header lands), A never reaches a player-type button (`0x1D`) or
  another panel's name button (`0x1C`), nor any hand below y = -16.4 (the buttons start between
  -17.4 and -18.3 in both games, and a hand moves at most 1.0 a frame). An unreadable hand gets no A
  either. The hand's own name button and its list take A (`ux=21`, [Each player's own
  controls](#each-players-own-controls)). A fresh
  panel needs no A: a hand moved into the grid joins it. The gate reads the plugged ports from the
  friends bytes the frame hook keeps (`MatchBlock::FRIENDS_SEEN`).
- **The seats**, on any character select: an empty panel (kind 0) whose port nobody plugged in is
  hidden while the select is online, except the opponent's seat of a queue select (panel 2: always
  in a queue room, and on the queue's own select while it searches). Each panel is player area i's
  39 models (MuObjects at area `+0xB0..+0x14C`; each one's nw4r `ScnMdl` at `+0xC`, its `ScnObj`
  flags at `+0xCC`, `0xA0000000` in both games). Orca sets `0x60` (draw neither opaque nor
  translucent) on the hidden ones and clears it on the others, only on a model whose flags are
  exactly the game's (with or without those bits) and whose first word is a vtable in the
  executable. The select never writes those flags and rebuilds them with itself, so nothing
  outlives it. Hiding is drawing only: a hidden panel's logic and buttons stay, and the gates keep
  them dead.

So the queue's own select shows the player alone while picking, then the opponent's seat
(SEARCHING) once it searches; a queue room shows both players; With Friends shows the players
plugged in. Forcing the game's own 2-player layout (task `+0x648`, `0x806834D8`) was rejected: it
changes game logic, is fixed when the select starts, and can't serve a 3-4 player friends room.

**Determinism:** every write is a pure, idempotent function of emulated memory and the session's
synced ports, at the frame hook (the record and panels after the online rules, the seats after the
native text); the gate's masks and its join press read memory alone. A joiner loads the host's
keyframe, which carries all of it.

**Test inputs.** The default two-player inputs (`bf-mario-link-results.txt`, `pplus-1v1.txt`) join
with A on the player's own player-type button. In a session two ports are plugged in, so the select
is online and that A is masked: each player joins when the stick takes the hand into the grid.
Two-Orca runs of these inputs therefore play differently from a pre-`ux=19` build's from the first
character select on. Harness-only runs (one port plugged in, a local select) are unchanged, so the
release hashlogs still compare. Harness knobs: a walk that plays ports 2-4 on an online select needs
them plugged in (`ORCA_UX_TEST_NAMES`, and `ORCA_TEST_PADS=4` for ports 3 and 4);
`ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1` gives a port nobody plugged in no controller, as a session does;
`ORCA_UX_TEST_UNPLUG_AT=<frame>:<port 2-4>` unplugs a port from that frame, as a friend's leave
does.

Tests: `OrcaOnlineSeats.*`; `Tools/orca/versus-cpu-dropin.py`; `queue-e2e.mjs --probe
Tools/orca/inputs/pplus-cpu-record-probe.txt` (a queue match with CPUs in both players' records);
inputs `pplus-cpu-online.txt`, `bf-cpu-friends.txt`, `*-cpu-dropin-live.txt`,
`*-friend-leaves.txt`.

## Ranked sets

A ranked set is followed in the game's own memory, so both Orcas, every re-run and every keyframe see
the same score (`Orca/UX/RankedSet.*`, `SetBlock.*`). The room hears each game from there once its
frames are final.

| Offset | |
|---|---|
| `+0x210` | Games recorded, wins per port, done (1 won, 2 void), winner, last game's winner, tiebreak next, ties in a row |
| `+0x218` | The fight being followed: flags (between scenes, sudden death, time ran out), ledge grabs, first frame (the game's id), stocks and percent at time-up |
| `+0x230` | 8 game records of 12 bytes: first frame, winner, how, stage, characters, stocks left, ledge grabs |

**Following a fight** (Brawl): `RankedSet::ReadLiveFight` reads each fighter through `sora_melee`'s
`g_ftEntryManager` (stocks, damage, status; statuses `0x73`-`0x75` are catching or holding the
ledge) and counts ledge grabs; stocks and percent are kept as they were when time ran out. Brawl's
sudden death stays in `scMelee` (fighters vanish and come back with 1 stock each).

**The verdict:**

- Not exactly two humans on ports 1 and 2, or a no contest: no game.
- **Brawl (Supernova 2025):** a KO is the results screen's winner. Out of time: the ledge-grab limit
  first (over 35 loses, Meta Knight over 20; 11 and 6 in a tiebreak game), then more stocks, then
  lower percent. A tie (a simultaneous last stock, or a level time-out) makes the next game a
  tiebreak with 1 stock and 3:00. Five ties in a row void the set.
- **Project+ (2024):** the results screen's winner (its codeset already handles time-outs and
  overtime).

Two wins end the set; 8 games without that void it. Other tracks read the score, the last winner
(the loser picks next), the stages each port won on (for Dave's Stupid Rule) and the tiebreak flag.

Tests: `OrcaSetBlock*`, `OrcaRankedSet*`, `OrcaResults*`. Not checked live: a real ledge-grab count.

## Stage select

### Ranked steps

`Orca/UX/RankedSteps.*` (namespace `Ranked`): pure functions of a ruleset and the set so far, shared
by both games. Stages are the ruleset's legal list, starters first, as bit masks.

- **Game 1:** the coin's port strikes 1 starter, the other strikes 2, then the coin's port picks one
  of the last two. The coin is one bit of an FNV-1a hash of the room code (the server makes the code,
  neither player picks it).
- **Later games:** the last game's winner strikes 2 (Project+) or may ban 1 (Brawl, optional: Y
  skips), then the loser picks, never a stage they already won on (full DSR; dropped if it leaves
  nothing).
- **Brawl only:** the Meta Knight clause (exactly one Meta Knight, character `0x18`: the other player
  picks any legal stage, over strikes, bans and DSR) and Ice Climbers never on Final Destination.
- **Timers:** game 1's steps 30, 30 and 10 s, a later strike, ban or pick 30 s, each with 3 s of
  grace at 0:00. When one runs out, the remaining strikes fall on the first allowed stages, the ban
  is skipped, a pick takes the first allowed stage.

**Brawl** (`Orca/UX/BrawlStages.*`): stages the current step doesn't allow are drawn as plain grey
tiles. The flow's own picks (the last starter, a timer's default) move the game's cursor there (via
the page button for Pokemon Stadium on the Melee page) and press A until the game takes it. A stage
taken against the steps is replaced before the screen copies it out. The stage select task is the
scene's `+0x3AC`: `+0x200` the cursor (`+0x3C`/`+0x40` x and y), `+0x224` the state, `+0x228` the
page, `+0x244` the hovered item, `+0x258` the taken stage (a write before the screen leaves changes
the match's stage).

**Project+** (`Orca/UX/RankedPPlus.*`): outside the stage select, `RSS_EXDATA` (`0x8042C4E8`) holds
the 2024 Proposed preset. On the stage select the hook writes Project+'s own Stage Striking table
(`0x8042C822`) as the block calls for, and sets `PAGE_INDEX` to `0xFF` so Project+ redraws the struck
art; Project+'s own check then refuses a struck stage. In every Project+ session the character
select clears a My Music step (`0x80002810`, Project+'s `MusicSelect.asm`) left from an earlier stage
select: step 1 hid every tile and turned the timer's pick into My Music, a softlock. The log says
`cleared a leftover My Music step` when it does.

### Casual stage pick

Both players at once, 15 s: each player's A on a legal stage under their own cursor names it, B
takes it back. The same stage plays at once; two different ones, a coin takes one the moment the
second player names theirs; a player who named nothing gets a random legal stage. The coin and random
stages come from `CasualSeed(room hash, coin, game number)` through murmur3's finalizer, so every
machine and re-run computes the same. Tests: `OrcaRankedSteps.*Casual*`, `OrcaBrawlCasual.*`,
`OrcaRankedPPlus.Casual*`.

### Two cursors

In queue matches each player has a cursor of their own on the stage select, moved by their own stick
and drawn by Orca's overlay in the player's colour (`Orca/UX/StageCursors.*`). Friends rooms and solo
play keep the game's own stage select.

- **A on a stage proposes it**; B takes the proposal back. When both propose the same legal stage in
  play this game, the match starts there at once, whatever steps are left.
- The steps work on the turn's player's own cursor: X strikes or bans, Y skips Brawl's ban, the
  picker's A picks.
- The game's own cursor is nobody's: the gate masks every port on the stage select, and the flow
  writes the cursor's position (where the cursor that moved last is) and is the only one to press A.
- **Determinism.** The cursors are match block bytes (`+0xF4`-`+0xFF`, `+0x1B4`-`+0x1BF`; positions
  in 1/16 of the game's cursor units). The latch moves them from the raw sticks (dead zone 24 of 128,
  integer math) at every pass over a boundary; the frame hook takes presses as edges of "now" against
  "seen", so a re-run sees none.
- Tile rectangles and the cursor-to-screen map were probed in each game (`StageCursors::Layout`).
  The game clamps its own cursor to its range, so it can't be hidden; the overlay draws the
  follower's crosshair over it.

**Whose turn it is** must be obvious. The acting player's cursor blinks (2.5 beats a second, under
the 3-flashes-a-second limit) with a tag ("YOUR TURN", "X STRIKES A STAGE · 2 LEFT", or "BO'S TURN"
on the other screen) and the turn's timer; a waiting player's cursor goes grey. Under the Meta Knight
clause the stage select's first 3 s say so ("META KNIGHT CLAUSE"). Overlay only.

Not yet: the two cursors over a real two-Orca room and between a Mac and a PC; a keyframe mid-select.

## Character select

### A only in the grid

On a later game's character select the hands come back on the bottom panels, each on its own
player-type button, where A turns the player into a CPU. So while a header locks, the gate passes A
only when the hand is in the character grid or on its own token (`OnlineRules.h`). Without a
header, With Friends and a friend's drop-in keep A off the player-type buttons and other panels'
name buttons ([No CPUs online](#no-cpus-online)).

- Player area i is the select task's (scene `+0x400`) `+0x44 + 4i`. `+0x1A8` is its hand: `+0x80`
  what it points at (2 or 7 the grid, 3 its own token, 6 and 8 the token moving, 4 a leave button,
  1 another button), `+0x90`/`+0x94` its x and y.
- A acts where the stick has just moved the hand, and the gate decides from the state before the
  frame, so it passes A only for y strictly inside the grid less one frame's travel: -3.0 to 15.0 in
  Project+, -4.2 to 15.0 in Brawl (Brawl's bottom row, with RANDOM, reaches -4.07).
- **A on BACK** on the queue's own character select backs out, as held B does there; while the
  player is searching, the first frame of that press un-readies and the next is the game's A.

### B only takes the token back up

Both games tell the two B's apart by the token: a B pressed with the token down puts it back in the
hand at once; B held from there backs out to the menus after 31 frames. So while a header locks the
gate passes B only on the first frame of a press, with that player's token down (`CssBUnpicks`): it
takes the token up, and a held B never backs out.

### Character order

Later games of a ranked set pick in order (`Orca/UX/CharOrder.*`). `CharOrder::kLaterGameOrder` is
one constant:

- **`WinnerFirst`** (the default, as in the standard rulesets): the previous game's winner picks
  first, the loser sees that pick and picks second, 45 s each.
- **`Free`**: the loser is asked first; the winner may lock in at any time.

A picks (puts the token down), B takes it back up, **Start locks the pick in** (read from the raw
latch; the game never sees that Start). Off-turn players' controllers do nothing. When the second
player locks in, the stage select comes at once. The tokens stay down between games, so Start alone
keeps last game's character. A player whose time runs out with the token in hand has it put on the
character under the hand. Game 1 has no order (both pick at once). State: 16 bytes at `+0x200`
(magic `YGCO`, the step, the first picker, the lock-ins, the step's start frame).

Not yet: Supernova 2025's exact order (ban, the loser's stage, then characters).

### The queue's character select

Casual and Ranked land on a character select where the player sees only their own panel, picks and
presses Start; only then does the page search (`Orca/UX/Queue.*`, the session's part in
`Rollback/OnlineMatch.cpp`).

| stdout (with `caps queue2`) | When |
|---|---|
| `orca queue ready <casual\|ranked> <char> <costume>` | A character down and Start pressed: search with this pick. Printed again with a new pick or when back after a room. `<char>` is the select's id (`0x28` none, `0x29` Random). |
| `orca queue unready` | B, the token taken up, or `queue-cancel` while ready. |
| `orca menu cancel` | Backed out to the menus (B or A on BACK while not ready). |
| `orca queue skip` | Casual only: Z held 1.5 s on the room's select. The page leaves and searches again. |
| `orca queue timeout me\|them` | Casual's first game: the 30 s ready timer ran out. |

The page owns the room: Orca never leaves on a skip or a timeout by itself.

- **The own select.** The header carries `FLAG_SOLO`. Ports 2-4 are masked; Start never reaches the
  game (it readies through the latch); while ready, B only un-readies.
- **The pick's identity.** At ready, Orca keeps the pick (character, costume, where the hand placed
  it) and the page's `queue rating`, as a 13-byte queue identity that rides the session's value
  channel, so every machine has the joiner's at its plug frame.
- **The room's select.** Game 1: the host's pick is locked; the joiner's pick is put in from its
  identity by the input gate (its stick held centred for 20 frames, since the game takes a newly
  plugged pad's stick as its origin, then steered to the place, A once the hand has stopped, X until
  the costume matches; about 1.5-2 s, 10 s at most). Then a 30 s ready timer; both ready, Orca
  presses Start. Casual's later games have no timer: players can talk; ranked's go through the
  character order.
- **Random** is a pick like any character (`0x29`); the game resolves it from its own RNG, which is
  emulated state, so both machines resolve the same character.
- **Ranked has no skip** (`Queue::MaySkip`): a ranked match must be played.
- **After the room** (a skip, a timeout, the opponent gone, a set over), the session loads the
  **queue image**, the player's own game as it was when `host` or `join` came (a machine image and
  the NAND, 20-30 ms to capture), as the state of the next frame, and opens a room of its own. The
  player is ready again unless their own timer ran out or a ranked set was played.

Determinism: the region `+0x1C0`-`+0x1EF` (ready flags, timer, game 1's locks, the raw latch's
buttons, the joiner's pick and steering), advanced at every boundary by `Queue::Advance` as a pure,
idempotent function of memory, the frame number and the synced ports. All steering arithmetic is
integer ([Determinism across hosts](#determinism-across-hosts)).

### The results screen's replay

The results screen's Z saves a replay, which a session can't (no save file, no writable SD card). Z
opened a picker that A and Start never leave, so a player mashing A with Z after a game was stuck on
it for good. Every session's input gate masks Z on the results screen (`Rules::SessionMasks`).

## On-screen UI

Everything Orca shows should look like the game drew it. Two ways:

- **Native text** (`Orca/UX/NativeText.*`): where the game has a text box on screen and the words
  are the same for both players, Orca writes them into the game's own message buffer, so they are
  drawn in the game's font and place. A buffer is a window's set-up codes, then UTF-8 words between
  inline codes; Orca replaces only the words (ASCII only: the font lacks the middle dot and en
  dash). Built for Brawl's name plates and rules bar on the Versus character select (task `+0x538`:
  "Game 1: ADA locked in, BO to press Start 0:24"), and Project+'s name plates. Always third person,
  so both machines write the same bytes; this is game memory, so it is in the compatibility key.
- **The kit** (`Orca/UX/Kit.*`, `Widgets.*`): everything else is drawn by the host over the picture,
  local display only, in the game's own fonts (read at run time from the player's disc,
  `system/font/`, never shipped) and the look of that game's menus. Without the disc it falls back to
  ImGui's font.
- **Relabelled textures** (`Orca/UX/Relabel.*`, `CssTitle.*`): the game's big words (READY TO
  FIGHT!, STAGE SELECT, the character select's BRAWL / VERSUS title) are textures. Orca changes a
  value nobody sees (a transparent palette entry's blue, or one texel's high nibble) so the texture
  hashes to another name, and Orca's texture pack has a label for each: PRESS START TO LOCK IN!,
  WINNER PICKS FIRST!, P1 STRIKES 2, CASUAL, RANKED, FRIENDS. Without the pack the game's own words
  show, unchanged to the eye.
- **YouGame's layer** (`Orca/UX/YgCard.*`, `YgOrb*`): chat and room notices in YouGame's own look and
  Roboto (`Data/Sys/Orca/Fonts`, Apache 2.0), deliberately not the game's.

**Set end** (`Orca/UX/SetEnd.*`): VICTORY or DEFEAT with the set's score and the rating counting from
the old number to the new one; casual's per-game YOU WON / YOU LOST; "<name> disconnected · They
forfeit in" with a draining ring that follows the room's 15 s grace (or the 10 s silence limit); "You
left · This set counts as a loss". After a ranked verdict the room stays open (**set over**): both
games keep the session on the results screen with the page's chat; each Orca leaves the room when its
results screen ends, when the player holds Z for 1.5 s, or, with the opponent gone, on Start. A
ranked set's player comes back to their own character select not ready, so the page doesn't search
by itself; a Start still held from the results screen readies nobody until it is let go.

`ORCA_UX_KIT_DEMO=1`, `ORCA_UX_SETEND_DEMO=victory|defeat|countdown|casual|left|stay` and
`ORCA_UX_OVERLAY_DEMO=1` draw these for design checks; with `ORCA_UX_SETEND_DEMO_AT=<frame>` a harness
run shows the same moment at the same frame.

Not yet: Project+'s rules bar (its message has no width code, so a longer line isn't drawn; the
overlay docks its lines on the bar instead), native words on the results screen, a native LOCKED IN.

## Embedding

The YouGame desktop app shows Orca inside its own window, at the page's player box, using the same
line protocol as its libretro host. Code: `Source/Core/DolphinNoGUI/Embed.*`, `Platform.cpp`
(commands), `PlatformWin32.cpp` and `PlatformMacos.mm` (the window). Test hosts:
`Tools/orca/embed-test-host.m` (macOS) and `embed-test-host-win.cpp`.

```
Orca --embed --parent <handle> --rect X Y W H [-u <user>] -e <disc> [-C ...]
```

- `--parent`: on Windows the HWND as a decimal number; on macOS the window's **CGWindowID** (in
  Electron, the middle field of `win.getMediaSourceId()`), since an `NSView*` means nothing to
  another process.
- Orca never shows dialogs while embedded: alerts go to stderr. stdin must stay open; a closed stdin
  means the app is gone, and Orca quits. When the app's window goes away without a `quit`, Orca
  stops too.
- Fd 1 is kept for the protocol; everything else Orca or Dolphin prints goes to stderr.

**Coordinates** are physical pixels (the box's CSS pixels times `devicePixelRatio`). On Windows
`rect` is relative to the parent's client area and Orca's window is a `WS_CHILD`, so it moves and
clips with the parent. On macOS `rect` is relative to the parent's frame (title bar included; the app
adds the title bar's height to Y), and Orca polls the window's bounds at 120 Hz. The app sends `rect`
only when the box moves inside the window.

### stdin

| Command | Effect |
|---|---|
| `rect X Y W H` | Move and resize Orca's window. W or H under 32 hides it until a big enough rect. |
| `view X Y W H` / `view off` | The box the game's picture is laid out in, when it differs from the window: the page cuts the window (`rect`) to make room for its menu panel and keeps `view` the whole box, so the game neither shrinks nor moves. Send `view` before every `rect` once used. Orca's own overlay stays inside the visible part. |
| `dim N` | N% black over the whole frame, eased (the page sends `dim 50` while its menu is open). `focus` clears it. |
| `hide` / `show` | Take the window off screen and back. Hidden means no input. |
| `focus` / `blur` | Give the game the input, or give it back to the page. |
| `pause` / `resume` | Answers `state paused` / `state running`. In a session it pauses only while the player's game is alone, and ends by itself when drop-in needs the game; otherwise `unsupported pause`. |
| `volume V` | 0 to 1, this run only. |
| `perf off\|fps\|detailed` | The overlay's frame meter (frames shown in the last second and the longest; yellow under 59 fps or past 20 ms, red under 50 fps or past two frames). A frame the game itself holds in emulated time (Project+ loading between scenes) counts as the video frames it held, so the meter, `fps` and the `hi` hitch count in `orca stats` show only this machine falling behind; `hil` counts the frames a load alone made slow (`hi` + `hil` is the older builds' `hi`). |
| `music on\|off` | The player's Music switch ([Music switch](#music-switch)), cap `music`. |
| `save N` / `load N` / `reset` | Refused in a session. |
| `prepare-join` | The host invited a friend: capture a keyframe at the next boundary. |
| `join <code>` / `leave` | Into or out of a friend's game. |
| `host <code>` / `queue-cancel` / `queue rating <n\|->` | Matchmaking ([Matchmaking](#matchmaking-and-results)). |
| `direct on\|off` | The player's switch for direct links. |
| `delay auto` / `delay N` | Adaptive input delay, or a fixed 1-6 frames. |
| `chat <name> <text>` | A line from the page's chat, shown while Orca fills the screen. Fields are `encodeURIComponent` of UTF-8 (name at most 24 code points, text 140). Never logged. |
| `controls <hex>` | The player's own controls ([Each player's own controls](#each-players-own-controls)); the tag gets them at the next character select. A bad profile answers `unsupported controls`. |
| `orb <x> <y> <size> <lit> <away> <badge>` / `orb blink` / `orb off` | YouGame's overlay button, drawn where the page's own stands under Orca's window. |
| `notice <kind> <name> <text> [<key> <verb>]` | One of the YouGame overlay's lines (join, leave, invite, ...). Never logged. |
| `quit` | Stop at once and exit. |

Most commands are gated by capabilities (`caps`) that Orca and the app agree on; a later `caps` line
replaces the whole answer. A command without its cap, or a malformed one, answers
`unsupported <command>`. Any other line goes to stderr. Drawing
commands (`view`, `dim`, `perf`, `chat`, `orb`, `notice`) are host-only: never in game memory or
the compatibility key.

**Opening YouGame's overlay** prints `orca orb`: a left press on the button (Windows), Shift+Tab
while Orca has the keyboard, or, on a controller, holding the D-pad's Up and pressing Start
(`Orca/UX/OrbCombo.h`). From that Start until Start and the D-pad are let go the game gets neither,
and the masked pad is what is recorded and sent, so every machine plays it. Opening the overlay
never pauses Orca: the page sends `blur`, the player's pad goes neutral, and the match goes on.

### stdout

| Line | Meaning |
|---|---|
| `ready W H FPS` | The first frame is on screen (Brawl: `640 480 59.9401`). A session may wait in its lobby after this. |
| `state running` / `state paused` | The answer to `pause` and `resume`. |
| `unsupported <command> [N]` | A refused command. Not an error. |
| `key escape` / `key fullscreen` | Windows only, if Orca's window somehow has the keyboard. |
| `orca click` | Windows only: a mouse press on Orca's window, which the page never sees; the app focuses its page and sends `focus`. |
| `error <sentence>` | Something failed. Each `orca error` also gets an `error` twin. |
| `orca state <state>` / `orca error <code> <sentence>` | Session status (`Orca/Status.h`). Forward every line that starts with `orca `. |
| `orca menu ...`, `orca result ...`, `orca queue ...` | See [Online menu](#online-menu) and [Matchmaking](#matchmaking-and-results). |
| `orca controls <hex>` | The player's own controls: changed in the game, or loaded from Orca's file at boot. The page keeps them. |
| `exit <code>` | Always last: 0 normal, 1 a reported error, 2 bad arguments. |

### Focus

Dolphin reads input only while `Host_RendererHasFocus()` holds, and embedded Orca turns off
background input, so that rule always applies.

- **Windows:** Orca never takes the keyboard; the page sees every key and sends the keyboard as a pad
  in the controller stream. A click on the picture lands in Orca's child window, so Orca prints
  `orca click`. Input counts while the last command was `focus`, the view is shown and the app's
  window is in front. A child window in another process shares the app's input queue, so a stall on
  Orca's main thread stalls the app's UI.
- **macOS:** Orca never takes the keyboard or the mouse. Its window can't become key and lets clicks
  through (`ignoresMouseEvents`). Orca reads the keyboard from the HID state and pads through SDL;
  input counts while the last command was `focus`, the view is shown and the app is frontmost. The
  page sends `blur` when it wants keys for itself.

### macOS window behaviour

Orca's view is a borderless window ordered just above the app's window, so the app's menus and other
apps' windows still cover it. The process is an accessory app (no Dock icon), its window follows the
app into full screen and hides when the app's window is minimized or on another Space.

**Out of sight, nothing is presented.** While Orca's window is hidden or fully covered
(`NSWindow.occlusionState`), Metal takes no drawable: a covered layer gets drawables back late, and
`nextDrawable` blocked the CPU thread (which a session shares with the GPU) for up to a second, down
to 14 fps, dragging the friend down too. After any wait over 50 ms, drawables come from a background
acquire until the window is next out of sight. `Tools/orca/occluded-test.py` measures it.

Nothing in the page can draw over Orca's window, so a sheet over the box must `hide` it; the play
menu's side panel instead cuts `rect`, keeps `view` and sends `dim 50`.

**Not built:** rendering into IOSurfaces shown in the app's own view. A spike
(`Tools/orca/iosurface-spike.c`) passed three 1920x1080 IOSurfaces to another process in about 13 ms
of setup, with per-frame messages in a median 12 us. It would need Metal to present into an
IOSurface ring and a small N-API module in the app.
