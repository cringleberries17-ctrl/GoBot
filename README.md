# GoBot

**Players: read `DOC-TRUOC.txt` (in Vietnamese) or `START-HERE.txt` in the
package, not this file.**

A bot for GhostOnline, the VTC client, that plays by reading the screen -- the
HUD's figures, the monsters' name plates, the game's own dialogs -- and
pressing keys and clicking as a player would. No account system, no licence
server, no subscription — everything runs locally. A packet-capture bot is in
here too, optional and unfinished: it cannot run without your own position,
which the server never sends (see below).

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

`.\package.ps1 -Version <version>` builds, runs the self-tests as a gate and
zips what a player needs into `dist\`: the GUI and the CLI, the configs as
shipped, the game's pictures and glyphs, the two notes, `VERSION.txt`, and
`THIRD-PARTY-NOTICES.txt` -- the licences of OpenCV, zlib, Berkeley SoftFloat
(compiled into OpenCV's core) and Dear ImGui, which are linked into the
programs. It refuses while GoBot is running, and refuses a package that
carries anything of this machine's player's.

Verify the build:

```powershell
.\build\bin\Release\gobot_test.exe
```

Over 1,700 checks covering the layers that can be tested without the game — frame
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

`gobot_gui.exe` opens on one simple screen, in Vietnamese, with seven tabs.
**Đánh quái** is laid out in the order things have to happen:

1. **Trước khi bắt đầu** — a checklist: game window found, GoBot not covering
   it, your character's name, HP/MP being read, keys reaching the game. Each
   line that is not ticked has the button that fixes it next to it.
2. **Bot sẽ làm gì** — fight monsters (or stand still and fight what comes),
   use skills, pick up drops, HP potion, MP potion, buffs, revive on death.
   Click a key box and press the key to change it.
3. **BẮT ĐẦU**, or **F10** in game. **F12** stops everything at once.

Everything that acts on the game ships switched **off**: nothing is pressed
until something is ticked. **Tự hồi sinh** revives on the spot with a revive
item -- the death panel's *Sử dụng vật phẩm hồi sinh*, then Xác Nhận; never
the village (−2% EXP), never Cash -- three tries a death, and when they fail
the whole bot stops and says why in red. **Tự buff** gives each buff key its
own time, 60–7200 seconds, and casts every buff again after a death, a revive
or logging back in. The game has to be windowed, 1024x768 or 1280x720 -- the
readers follow either size -- with Windows' display scale at 100%. On a
screen 768 pixels high only 1280x720 fits, with the taskbar set to hide
itself; a hidden taskbar's strip, and anything past the screen's edge, is
not counted as covering the game.

Fighting finds your character by the name typed on the checklist, and only
by that name, whole on its plate: a player whose name has yours inside it
("TuanAnh" for "Anh", "KhiCon2" for "KhiCon") or is a letter away from it is
not you. A plate with bars may be yours or somebody else's -- a party
member's included -- so a plate is never taken for you by its bars. Without
a name, **Tự đánh quái** does not start (nor does F11): the box under
BẮT ĐẦU says why, and while it is ticked the name's row on the checklist has
a red cross.

The version is in the window's title and at the bottom left of the simple
screen: the one `package.ps1` wrote into `VERSION.txt`, or *bản tự build* for
a build of your own. Whatever has passed every test here but never run on the
real game is marked *Thử nghiệm*: on the first line of a tab that is new as a
whole, at the start of a row's hint for a new switch.

**Tổ đội** answers party invitations and invites whoever chats the keyword,
with an accept-only list, an invite list and a blacklist. **Tự đăng nhập**
logs back in when the client crashes or loses its connection, to the channel
and character you pick, and with the game closed BẮT ĐẦU opens it first. It
needs no account and no password: VTCZone stays signed in (sign in to it once,
with its remember option) and GoBot only presses *Chơi game*; if VTCZone shows
its sign-in form instead, it stops and says so. **Dọn rác** opens the bag every
few minutes and puts what is on your list in the trash can; you pick the junk
once from the game's item icons (or a photo of your bag for anything else),
and *Thử tìm* shows what would go without throwing anything away. Items are
matched by their drawing, whatever their colour -- the fragments in the bag
change colour all the time -- so picking an icon also picks the same drawing
in other colours, and the tab shows which. Only the bin's own dialogs are
answered, told apart by their words, and an item that looks as much like an
icon you did not pick, drawn differently, is left where it is. BẮT ĐẦU starts
whatever is ticked on any tab.

**Tự chat** says up to ten lines of your own in the game's chat, one every
so often (30 seconds at the least), in turn or at random, into whatever
channel the button beside the game's chat box is set to. It types the way
GoC does, a key per letter, so the marks come off Vietnamese and only
letters, digits and spaces go in. Each key is pressed only once a look at
the screen shows the chat line open, Enter only on a fresh look that shows
no dialog, and the fighting, potions and buffs wait for the few seconds a
line takes -- and, when GoBot cannot see the chat line shut afterwards, for
as long as that lasts, as every key would go into it. Nothing of GoBot's
uses the clipboard.

**Nhiệm vụ** is quick quest, done as GoC does it for this client: every few
minutes of a run, GoBot opens the Nhiệm Vụ window with its key (U by default),
goes to the NV Phụ tab and ticks the game's own **Tự Động** there if it is
off, then closes the window again -- the fighting, potions and buffs wait
those few seconds, and HP falling under the potion's mark ends a round at
once. A timed round waits while the game is not the window in front; only
*Làm ngay 1 lần* brings it forward, and only in the few seconds after the
click -- one that had to wait longer waits for the game to be in front, as a
timed round does, rather than pull it over a window you have gone to since. It reads the box before clicking, so an
option already on is left alone, and it never clicks Hủy Nhiệm Vụ, the
quest's accept, give-up or complete buttons, the low-level-quests checkbox
or another tab, and types nothing; GoBot itself never accepts, hands in or
picks a quest. *Thử xem* looks without pressing anything: open Nhiệm Vụ on
NV Phụ by hand first, and it also says whether a round would click where it
needs to. Three failed rounds in a row stop it, with the frames kept in
`quest_debug` beside GoBot. The English client, whose quests GoC picks by
picture, is not supported.

**Thông báo** tells you when something needs a person: the character died
(or was revived, or could not be), the game went and GoBot is logging back
in (or gave up), the character has been out of the world for minutes, a stop
left the trash bin's dialog or the chat line open, Tự chat or quick quest
stopped itself.
Each is one short message in Vietnamese to a Discord channel of yours,
through a webhook, so the Discord app on your phone shows it, and a beep and
a Windows notification on this PC that never take the focus from the game.
Every event has its own tick; repeats within a minute come as one message
(×N), at most 5 go a minute and, unless one needs a person, 30 an hour, and
what could not go while the network was down goes when it is back, with its
own time. A message is fixed
sentences and numbers only: never your account, a password, your character's
name, the map or position, a screenshot or another player's words -- a party
message names the other player only if you tick that.

The webhook is a secret: whoever has it can post in your channel. Paste it on
the tab and GoBot keeps it encrypted by Windows (DPAPI), for your Windows
account on this PC only, in `%LOCALAPPDATA%\GoBot\alerts_webhook.bin` --
outside the GoBot folder, so copying or zipping GoBot never takes it along,
and never in a log or in `configs/alerts.ini`. The tab shows only its last
four characters. **Xoá webhook** on the tab deletes it; so does deleting that
file. Alerts are off until **Bật thông báo** is ticked.

Settings save themselves as they change. Everything else — every timer, the
radar, the packet tools — is under **Nâng cao ►** at the bottom, and
**◄ Giao diện đơn giản** at the top left of that window comes back.

How this compares with GoC, feature by feature: `docs/goc-comparison.md`.

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
| **Sequence** | An ordered combo: key, pause, next key. Optionally looping. Off as shipped; the simple screen does not show it. |

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
| `configs/app.ini` | the game's window, the client keeper, the simple screen's switches; capture interface and server address; whether `gobot_gui.log` starts afresh each session (`new_log_each_start`, on as shipped: the session before is kept as `gobot_gui.old.log`) |
| `configs/macro.ini` | the macro's keys -- skills, potions, buffs -- and the sequence |
| `configs/hunt.ini` | hunting: your character's name, the attack and pick-up keys, the reach |
| `configs/party.ini` | party: accepting, inviting by keyword, the lists of names |
| `configs/login.ini` | logging back in: where VTCZone is, the pages, the channel and character |
| `configs/trash.ini`, `configs/trash\` | bag cleaning: the icons picked, and the photographed items |
| `configs/chat.ini` | Tự chat: the lines, and how often |
| `configs/quest.ini` | quick quest: the key, and how often |
| `configs/alerts.ini` | alerts: which events, the limits -- never the webhook |
| `%LOCALAPPDATA%\GoBot\alerts_webhook.bin` | the Discord webhook, encrypted for your Windows account |
| `configs/protocol.ini` | framing + packet layouts (**you write this**) |
| `configs/bot_default.ini` | the packet bot: combat behaviour, skills, healing, keys |

Every one ships with whatever acts on the game switched off, and with no
player's names, keys or lists in it.

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
