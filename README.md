# GoBot

A packet-capture bot for GhostOnline. No account system, no licence server, no
subscription — everything runs locally.

Built as a clean reimplementation of the architecture used by ZTeam's GoC
client, with the one part that made that client dependent on a vendor removed.

---

## Why this needs work before it runs

The commercial client this replaces was **a thin client with a remote brain**.
Its `GhostCore.dll` did four things:

1. sniffed the game's TCP stream with Npcap,
2. extracted frames and filtered them by an opcode allowlist,
3. encrypted the batch (X25519 ECDH + AEAD) and uploaded it to
   `login.chamthoi.com:443`,
4. executed the `BotCommand` list the server sent back.

The opcode allowlist, the game server IP range, the port range and the window
name all arrived **in the login response** — `DecodeLoginResponse` reads
`opcodes[]`, `ipRange`, `portRange`, `gameWindow` off the wire. The protocol
knowledge was never in the binary. That is what made a subscription
load-bearing rather than a bolt-on licence check.

So there is no version of "remove the login system" that leaves a working bot
behind. The brain has to be rebuilt locally, and the one input it needs is the
GhostOnline packet format.

**That format is not in this repository, and it cannot be.** You derive it from
your own traffic. `gobot_sniff` exists to make that a guided process rather
than an archaeology project.

Everything else — capture, reassembly, framing, decoding, world model,
targeting, skill rotation, healing, buffing, pathfinding, input — is written
and working.

### What is actually known about the protocol

Measured, not assumed. The detail and the evidence are in
[docs/protocol-findings.md](docs/protocol-findings.md).

- **The server stream parses cleanly** — 100% coverage, zero resyncs — and the
  monster world model matches the commercial client exactly: 42 creatures
  against its 43.
- **`0x0038` is the entity update.** `id@8`, `x@80`, `y@82` are confirmed. It
  carries a **sub-type at offset 4**: 0 and 1 restate a creature where it
  already is, 9 relocates it and carries the move parameters. Reading a field
  without splitting on that first averages three layouts together.
- **Your own position is not transmitted in either direction.** The server
  broadcasts every other actor and says nothing about you. Memory reading is
  closed off by XIGNCODE3, and the on-screen `WorldAnchor` never converges.
  Dead reckoning from the bot's own issued commands is the remaining route,
  and it is what ZTeam's server must have been doing too.
- **Map geometry needs no protocol work at all.** The game ships 781 `.map`
  collision files and `MapFile` already reads them; that is what ZTeam's server
  was parsing and charging for.

### A warning about this game's framing

Two things here will cost a day each if met without warning, and both are
configured correctly now:

- **Opcode `0x0081` carries twelve bytes past its declared length.** Miss it and
  the scanner resyncs, slides a byte at a time and swallows real frames — one
  such packet every few hundred frames hid 73% of the stream.
- **The two directions are framed differently.** The opcode sits at offset 2
  coming down and offset 6 going up, so a capture given only `[Framing]`
  decodes the server and grinds the client's half into noise.

---

## Build

No C++ toolchain is required up front; `bootstrap.ps1` installs one.

```powershell
.\bootstrap.ps1     # MSVC Build Tools + CMake + Npcap SDK  (~4 GB, once)
# open a NEW terminal so PATH updates apply
.\build.ps1
```

Output lands in `build\bin\Release\`:

| binary | what it is |
|---|---|
| **`gobot_gui.exe`** | **the app — start here.** ImGui front-end, everything in one window |
| `gobot_cli.exe` | headless bot runner |
| `gobot_sniff.exe` | protocol discovery, command line |
| `gobot_test.exe` | self-tests |

`gobot_gui` covers everything the two CLI tools do. They remain for scripting
and for running the bot without a window.

Verify the build:

```powershell
.\build\bin\Release\gobot_test.exe
```

663 checks covering the layers that can be tested without the game — frame
extraction and resync, frames that carry a trailer, both directions being
framed, framing auto-detection, TCP reassembly (out-of-order, retransmits,
partial overlap), packet decoding, the world model, pathfinding, the glyph
reader, and the shipped configs themselves. These are where a subtle bug is
most expensive: by the time bad framing surfaces as "the bot walks into a
wall", the cause is six layers down.

Npcap itself must already be installed (it is, if the original client ran) —
get it from <https://npcap.com/#download> with *WinPcap API-compatible Mode*
ticked.

**Everything below must run as Administrator.** Npcap will not open an adapter
otherwise.

---

## Getting started

`gobot_gui.exe` opens on one simple screen, in Vietnamese, laid out in the
order things have to happen:

1. **Trước khi bắt đầu** — a checklist: game window found, GoBot not covering
   it, your character's name, HP/MP being read, keys reaching the game. Each
   line that is not ticked has the button that fixes it next to it.
2. **Bot sẽ làm gì** — five switches: fight monsters, use skills, HP potion,
   MP potion, buffs. Click a key box and press the key to change it.
3. **BẮT ĐẦU**, or **F10** in game. **F12** stops everything at once.

Settings save themselves as they change. Everything else — every timer, the
radar, the packet tools — is under **Nâng cao ►** at the bottom, and
**◄ Giao diện đơn giản** at the top left of that window comes back.

## Keyboard macro

The macro needs no packet capture and no protocol work. On the simple screen
it is the skill, potion and buff rows. Its full settings are under
**Nâng cao ►**:

1. Pick the game window from the list
2. **Timed keys** → set your keys and intervals
3. **START MACRO**, or press **F10** in game

Two mechanisms, usable together:

| | what it does |
|---|---|
| **Repeating keys** | Each key on its own timer — a skill every 1.2s next to a buff every 5 minutes. Independent, so adding one never disturbs another's timing. |
| **Sequence** | An ordered combo: key, pause, next key. Optionally looping. |

**F10** toggles, **F12** is a panic stop that halts everything and releases every
held key. Both work while the game has focus, so you never have to find the
GoBot window to stop it.

Worth leaving on:

- **Only send while the game has focus.** Off, keys go to whatever window is in
  front — which will eventually be a browser or a document.
- **Jitter.** Each key has its own, plus a global amount on top. Perfectly
  regular timing is the clearest sign a client is automated.

Settings live in `configs/macro.ini` and are editable there or in the GUI.

## Packet-capture bot (optional)

Everything below is the packet-driven bot, which reads real game state —
monster positions, HP — rather than pressing keys on a timer. It needs
Administrator and a protocol map. Set `autostart=true` in `configs/app.ini` to
have it run on launch.

Right-click `gobot_gui.exe` → **Run as administrator**.

### The short version

**Dashboard → "Set up automatically"**, then play normally for about a minute.

It finds the game's own server socket, picks the adapter carrying it, captures,
works out the packet format, and proposes what the packets mean. Nothing to
look up and nothing to type.

It needs to see the numbers move to read them, so walk around, fight something,
and take some damage while it runs.

When it finishes, review the suggestions — each one states the evidence behind
it — apply the ones that look right, and confirm on the Inspect tab that the
decoded values match what the game shows.

### The tabs

| tab | what you do there |
|---|---|
| **Dashboard** | Start/stop, live HP/MP, position, monsters. Lists exactly what is still blocking a run, with the automatic setup button. |
| **Capture** | Only if you want manual control: adapter, server address, **game window picker** |
| **Protocol** | **Auto** does everything; the numbered tabs are the same steps by hand |
| **Bot** | Combat behaviour, targeting, ranges, timing |
| **Skills** | Skill and buff slots, movement keys |
| **Heal** | Potion thresholds |
| **Log** | Live log |

Pick the game window from the table on the Capture tab rather than relying on
the title box. On this machine the game shows up as **`GhostOnlineVTC`
(`Game.exe`, class `GhostSoul`)** — and an editor window whose title merely
contained "GhostOnline" sat right next to it in that list. That's the failure
mode the picker exists to prevent: matching the wrong one sends combat
keystrokes into someone's document.

The Dashboard refuses to start and names the blockers, so you always know what
is missing rather than getting silence.

## Deriving the protocol by hand

The GUI's automatic setup does all of this. This is the scriptable equivalent,
and what to fall back to when a step needs judgement.

### 1. Find your adapter and the game server

```powershell
gobot_sniff --list
gobot_sniff --discover --process Game.exe
```

`--discover` asks Windows which sockets the game process owns and prints the
server address, a ready-made capture filter, and the adapter carrying it. No
`netstat`, and nothing to copy by hand.

### 2. Record

```powershell
gobot_sniff --record capture.bin --host <server-ip>
```

Play normally for two or three minutes. Walk, fight, take damage, kill things.
Variety matters more than volume — you are producing the evidence that the
next two steps read.

Narrow to one server with `--host`; a capture mixing connections will not tile.

### 3. Find the framing

```powershell
gobot_sniff --detect capture.bin
```

This brute-forces every plausible header layout — length field offset, width,
endianness, whether it counts the header, opcode position — and ranks them by
how cleanly they tile the stream with no resyncs.

Paste the winning `[Framing]` block into `configs/protocol.ini`.

Judge the result, don't just take it:

- **≥95% coverage** — almost certainly right.
- **<95%** — a guess. Record a longer, single-connection sample.
- **nothing tiles** — the payload is encrypted, and packet capture is the wrong
  approach for this server.

### 4. Find the opcodes

```powershell
gobot_sniff --watch
```

Live histogram of opcode frequency. Correlate by doing one thing at a time:

| do this in game | what appears |
|---|---|
| walk back and forth | high-rate opcode — position |
| stand still, take a hit | new opcode — HP |
| pull a monster | monster spawn / update |
| kill it | monster remove |
| join a party | party update |

A resync count that outpaces the frame count means the framing is wrong. Go
back to step 3.

### 5. Find the fields

```powershell
gobot_sniff --diff 0x1234
```

Collects many instances of one opcode and reports which byte offsets change,
and how:

- **constant** — sub-type tag, flag, or padding
- **two to four values** — an enum or boolean
- **smoothly changing 4-byte run** — a coordinate
- **changes only when damaged** — HP

It groups adjacent varying bytes into field candidates and prints each 4-byte
run read as both `i32` and `f32`. A float coordinate is unmistakable.

Write the layout into `configs/protocol.ini`:

```ini
[Packet.0x1234]
name=MonsterUpdate
semantic=monster_update
fields=id:u32,kind:u16,x:f32,y:f32,hp:i32,max_hp:i32
```

### 6. Verify

```powershell
gobot_sniff --decode
```

Prints every packet decoded through your layouts. Fields that read as garbage
mean the layout is wrong — the tool tells you, rather than the bot silently
behaving oddly later.

`gobot_cli` refuses to start while `verified=false` in the `[Framing]` block.
Flip it only once positions track where you actually are and HP matches your
bar. Wrong framing doesn't fail loudly; it produces plausible-looking garbage,
and a bot acting on garbage state is worse than one that won't start.

### Minimum viable set

Three layouts get you a fighting bot:

| semantic | why |
|---|---|
| `self_state` or `self_move` | where we are |
| `self_hpmp` | when to drink |
| `monster_update` | what to hit |

Everything else is refinement.

---

## Running

Check it reads the world correctly before letting it press keys:

```powershell
gobot_cli --status
```

No input is sent. Confirm HP, position and monster count track reality.

Then:

```powershell
gobot_cli
```

**F10** toggles the bot. **Ctrl+C** quits.

---

## Configuration

| file | holds |
|---|---|
| `configs/app.ini` | capture interface, server address, window title |
| `configs/protocol.ini` | framing + packet layouts (**you write this**) |
| `configs/bot_default.ini` | combat behaviour, skills, healing, keys |

`bot_default.ini` follows the original client's schema, so existing configs
port with minimal editing.

Field syntax for `protocol.ini` is documented inline in the file itself:
`name:type[@offset][*scale]`, with `@` pinning an absolute offset and `*`
applying a fixed-point scale.

---

## Architecture

```
Npcap
  │  read-only capture, BPF-filtered
  ▼
TcpReassembler ──── per-flow, per-direction, seq-ordered
  │
  ▼
FramingSpec ─────── configurable; auto-detected by gobot_sniff
  │  GamePacket{opcode, payload}
  ▼
OpcodeRegistry ──── data-driven layouts from protocol.ini
  │  DecodedPacket{named fields}
  ▼
ProtocolHandler ─── dispatches on Semantic, never on opcode numbers
  │
  ▼
GameState ───────── self, monsters, players, party, map
  │
  ▼
BotBrain ────────── target selection, skill rotation, patrol
  │  Command{type, priority, ttl}
  ▼
CommandExecutor ─── 4 threads: exec, heal, buff, pathfind
  │                 arbitrated by ExecutionCoordinator
  ▼
InputManager ────── SendInput with scancodes, focus-gated
  │
  ▼
game window
```

Some deliberate choices:

**Capture is read-only.** Nothing is injected, rewritten or blocked. The bot
observes the connection and acts through the keyboard, which is the same split
the original used.

**Semantics, not opcodes.** Everything above `ProtocolHandler` is written
against `Semantic::MonsterUpdate`, never `0x0405`. Re-mapping an opcode is an
INI edit, not a recompile — which matters, because protocol discovery is an
iterative loop of guess, observe, correct.

**Four executor threads.** Command dispatch, healing, buffing and pathfinding
run on genuinely different clocks. Healing must react within a tick of an HP
packet; buffs fire on multi-minute timers. One loop makes the slowest set the
latency for all.

**One input token.** Those threads all want the keyboard. `ExecutionCoordinator`
arbitrates by priority with stale-holder reclaim. Without it the bot walks left
and right in the same tick.

**Focus is a hard gate.** `SendInput` goes to whatever window has focus. Every
send checks the game is foreground first, and losing focus releases held keys —
otherwise the character keeps running into a wall after you alt-tab.

---

## Limits

- **Encrypted payloads defeat this entirely.** If `--detect` finds no tiling,
  stop here.
- **Position accuracy is what the protocol gives you.** If the server only
  broadcasts movement every 200ms, the bot's world model is 200ms stale. And
  your *own* position is not broadcast at all — see the protocol notes above.
- **There is a vision layer, and it is not optional.** This file used to say
  there was none. There is: template matching, a glyph reader, HP/MP bar
  reading, nameplate detection, minimap recognition, auto-login and the macro
  mode all read the screen, and the macro works with no packet capture at all.
  What vision does *not* do reliably is supply world state — `WorldAnchor`
  never converges on a position — so it belongs on the interface, not the
  world.
- **Reading the screen needs the game unobscured.** `PrintWindow` returns a
  blank frame for this client, so pixels are lifted off the screen itself.
  Anything covering the game gets read instead of the game.
- **Captcha suspends the bot rather than solving it.** A dialog that eats
  keystrokes plus a bot that keeps typing combat keys into it is worse than
  stopping.
- **Pathfinding needs to know *which* map you are in, not map data.** The
  geometry is already there: 781 `.map` files the game ships, which `MapFile`
  reads. What is missing is identifying the current one — currently done by
  minimap fingerprinting and reading the place name off the title bar, neither
  of which is solid — and knowing where in it you are.

---

## A note on use

Botting violates GhostOnline's terms of service, and accounts do get banned for
it. That's between you and the operator — but the randomised input timing in
here is a mitigation, not a cloak.

Use this on your own account and your own traffic.
