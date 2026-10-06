# How Orca works

Orca adds rollback netplay to the Dolphin emulator. It runs Super Smash Bros. Brawl and Project+
online on [YouGame](https://yougame.co). This page explains the design. It also covers how Orca
differs from Slippi and Brawlback, the projects that came before it and informed it.

## The short version

> Orca rewinds the entire emulated console, exactly, at one point in each frame. Before any
> netcode existed, we proved that by rewinding every frame of full matches and comparing memory
> byte for byte. Because the restore is exact, Orca never has to change the game's code to make
> rollback work, and the game can't tell it was rewound.

## Rollback in one paragraph

Online, your opponent's button presses arrive a few milliseconds late. Rather than wait for them,
the game guesses (usually "they kept doing what they were doing") and keeps going. When the real
input arrives and the guess was wrong, the game rewinds a few frames and replays them with the
correct input, faster than you can see. This only works if a replayed frame comes out _exactly_
like the original would have, apart from that input. If anything drifts, the two players' games
slowly stop agreeing. That is a desync.

So the whole problem is making a rewind perfect.

## How Orca does it

### 1. Save and restore the whole machine, at one point

Every frame or few frames, Orca saves a snapshot of the entire emulated Wii:

- all 88 MB of RAM;
- the CPU, including its registers and the stack it is in the middle of using;
- the timing system that schedules hardware events;
- the audio hardware, the disc drive and the Wii's system software tables;
- everything else Dolphin knows how to save, except copies of the screen kept on your graphics card,
  which the game never reads back during a match.

Saving and loading happen at the **same point** in the frame. That detail matters most. Restoring
the CPU means resuming at a specific instruction. If you save at one point and load at another, you
can't restore the CPU without landing the game in the wrong place, so something has to be left out.

Even a plain full copy takes only about 2 ms on an Apple M4 Pro, so cost was never the obstacle.
Orca still saves copy-on-write: a snapshot marks memory read-only and keeps a page's old contents
only when the game first writes to it, and a rewind puts back only the pages that changed. That keeps
snapshots cheap on slower machines too.

### 2. Put input in at the controller hardware

The opponent is just another controller plugged into port 2. Orca feeds each player's inputs into
the emulated controller hardware. Brawl reads them through its normal code, on first runs and replays
alike. Nothing inside the game knows about the network. To Brawl, an online match is two people
sharing one console in local Versus mode.

### 3. On a replay, run the game normally and skip only the real drawing

Replayed frames are never shown, so drawing them is wasted work. Orca skips that work **in the
emulator, not in the game**:

- Brawl runs every instruction of the frame, including its own drawing code.
- Dolphin reads the drawing commands the game sends to the emulated graphics chip, so its timing and
  "finished drawing" signals happen exactly as before.
- Dolphin then skips the expensive part, the actual drawing on your graphics card.

This halves the cost of a replayed frame, from about 4.6 ms to 2.2 ms. It matters because a game's
drawing code doesn't only draw. It also changes game memory: allocations, counters, command
buffers. Skipping it inside the game changes the match. We measured that: skipping Brawl's draw call
just 7 times permanently changed 277 blocks of memory.

### 4. Prove it before building on it

The most useful thing we built wasn't netcode but a test. On one machine, Orca plays a full match
and, on _every_ frame, rewinds 5 frames, replays them and compares all of memory, byte for byte,
against a run with no rewinds. The first difference stops the run and names the frame and the exact
bytes.

That test turned "it desyncs sometimes" into a list of specific causes. Each one was a gap in the
restore, so we fixed the restore and never had to patch the game. After that, the result was 0
mismatches over about 70,000 replayed frames, across 2-player and 4-player matches with items,
pauses, KOs and the results screen.

### 5. Same start, same machine

Both players boot the same build, the same disc (checked by hash), a fresh copy of the same Wii
system memory and the same forced settings. They then run in step from power-on. Because everything
starts identical, the game's random numbers match without any special syncing.

Macs and Windows PCs compute the same game, so they can play each other. Getting there took fixes
to both of Dolphin's JIT compilers (the parts that translate the console's code into your CPU's),
mostly floating-point edge cases that the main Dolphin project also gets wrong. A few very rare differences
remain on M1-M3 Macs; the regular checksum exchange catches them as a desync rather than letting the
games drift.

## Around the rollback core

- **Netcode.** Players connect directly to each other when their networks allow it, and through a
  relay server when they don't. Input delay is 2 frames, like Slippi. Orca can adapt it to the link,
  but the YouGame app keeps it at 2 for everyone. The match is hosted in a YouGame room, which also
  handles matchmaking, chat and results. Matchmade Casual and Ranked games are 1v1; a friends' room
  takes up to four players, who can drop in mid-session.
- **Online menus.** Orca doesn't modify the Brawl disc or Project+'s files. It applies small,
  checked patches to the game's memory while it runs. Each patch says "at this address, if this
  original value is there, write this one." The patches boot straight to the main menu and reuse
  Brawl's own Online screen with Nintendo's network cut off, relabelled with replacement images for
  its buttons and titles. Every
  option then leads into ordinary local Versus. The patch files are part of the version check both
  players must match. Rollback works without them; they are a separate layer on top.
- **Results.** After each game, Orca reads the winner, stage, characters and stocks from memory, at a
  frame both players have confirmed, and reports them to the room. (Separately, Orca does write a
  few things into game memory, such as player name tags and the online rules. Those are part of the
  menu layer, not of rollback.)
- **Per-game profiles.** Game-specific knowledge is small: a disc hash, a few settings and the address
  of the end of the game's frame loop. Brawl and Project+ are the first profiles. The core itself is
  a general GameCube and Wii rollback engine.

## How it compares

### Slippi (Super Smash Bros. Melee)

Slippi is the gold standard for rollback in a Dolphin game. It works differently, and for Melee,
very well:

- It saves only Melee's own data, rather than the whole console.
- Code added to Melee picks one clean moment in the frame to save and load. Nothing important is held
  in the CPU at that moment.
- On a replay it runs only Melee's game logic and draws nothing.

That is leaner than Orca, but it depends on knowing exactly which parts of memory matter and how to
run the game's logic on its own. The Melee community has spent years reverse-engineering the game to
know that. Brawl is far less documented, and it also talks to the Wii's system software during a
match. Orca's design avoids needing that knowledge.

### Brawlback (Super Smash Bros. Brawl)

Brawlback pioneered rollback for Brawl, and its published research informed our first experiments.
Over time it moved to whole-console snapshots, like Orca. It kept Slippi's model of triggering saves
and loads from code inside the game, though, and its save and load happen at two different points in
the frame. That combination means the CPU state can't be restored exactly, so a few pieces were left out:

- the CPU registers and stack;
- the audio state;
- the Wii's system software state.

The rest of the design works around those differences. It uses about 16 hooks: small patches in
Brawl's code that call into the emulator. They skip the draw call on replays, cut graphics waits
short and keep random seeds in sync.

Our first experiment, before Orca existed, ran our byte-for-byte test on Brawlback's approach. Every
replayed frame differed in memory from the original, and restoring the missing pieces fixed that.
With an exact restore, two of the hooks turned out to change the match by themselves: the draw skip
and the shortened graphics waits. These are findings from that experiment under our test; we can't
say which of them, if any, caused the desyncs Brawlback's players saw. What we learned there led to
Orca's design, which we then built on current Dolphin with our own rollback code.

### Side by side

|                                 | Slippi                            | Brawlback                                        | Orca                                                      |
| ------------------------------- | --------------------------------- | ------------------------------------------------ | --------------------------------------------------------- |
| What a snapshot holds           | Melee's own data                  | Most of the console                              | The whole console                                         |
| Where save and load happen      | One point, chosen inside the game | Two different points, chosen inside the game     | One point, chosen by the emulator                         |
| What a replay runs              | Game logic only                   | The full frame, with hooks that skip parts of it | The full frame, unchanged; the emulator skips the drawing |
| Game-specific code for rollback | A lot (Melee code patches)        | About 16 hooks in Brawl                          | None beyond the frame-boundary address                    |
| Knowledge of the game needed    | Deep                              | Hook points in Brawl's main loop                 | Very little                                               |
| Cost per replayed frame         | Not measured by us; no drawing    | Not measured by us                               | About 2.2 ms on an M4 Pro                                 |

## Trade-offs and limits

- **Our replays cost more than Slippi's.** Brawl's drawing code still runs on the emulated CPU. Short
  rollbacks of 1 to 3 frames fit easily in a 16.7 ms frame; long ones are tighter.
- **We never benchmarked Slippi or Brawlback against Orca.** Brawlback reports 2–3 ms per save.
  Don't read this page as claiming Orca is faster per frame.
- **Sounds that start during a replay** play late or not at all.
- **Matchmaking is 1v1 today.** Up to four players can play in a friends' room.

## Credits

- **[Dolphin](https://github.com/dolphin-emu/dolphin)** is the emulator Orca is built on.
- **[Slippi](https://github.com/project-slippi)** is the reference for how rollback should feel. We
  took ideas and constants from it, not code.
- **[Brawlback](https://github.com/Brawlback-Team)** gave us Brawl's hook addresses and documented
  findings. Orca contains no Brawlback code.
- **The Brawl modding community:** some of Orca's game patches come from community Gecko codes
  (cheat-code-style patches), and
  are credited where they are used.

## Explaining it to people

**One sentence:**

> "Rollback means rewinding the console a few frames and replaying them. We rewind all of it, at
> one exact moment, so a replayed frame comes out identical to the original."

**Thirty seconds:**

> "When your opponent's input arrives late, the game rewinds a few frames and replays them with the
> right input. That only works if the replay is identical to the original. Earlier approaches for
> Brawl rewound most of the console and patched the gaps with hooks inside the game. We rewind the
> whole console at one exact point, and we test it by rewinding every frame of full matches and
> checking memory byte for byte. Once that passes, the game needs no changes at all."

**An analogy:**

> "Imagine rewinding a chess game. One approach puts the pieces back but leaves the clocks and the
> scoresheet as they were, then adds house rules to cover the mismatches. We put back the pieces,
> the clocks and the scoresheet, so no house rules are needed."

**If someone asks what the new idea was:**

> "There wasn't a new technique. Restoring the whole machine exactly is the textbook way to do
> rollback in an emulator. The difference was testing: we made the emulator prove a rewound frame is
> byte-for-byte identical before writing any netcode. That turned vague desyncs into specific bugs
> we could fix."

**What not to say:** that Brawlback "did it wrong", or that Orca is faster per frame. Brawlback's
research informed the experiments that led to Orca's design, and we haven't measured the speed
claim.
