# GoBot

**Players: read `DOC-TRUOC.txt` (in Vietnamese) or `START-HERE.txt` in the
package, not this file.**

A bot for GhostOnline, the VTC client, that plays by reading the screen -- the
HUD's figures, the monsters' name plates, the game's own dialogs -- and
pressing keys and clicking as a player would. No account system, no licence
server, no subscription — everything runs locally. With Npcap it also reads
the game's own traffic, read-only, for where you and the monsters are and for
your HP and MP -- since 2026.11.17 taken from the packets whenever they have
them, the bars read only without them (*MP bar and skills while hunting
(2026.11.17)* and *Packets first (2026.11.03)*, below). The old bot that ran on the packets alone went in 2026.11.16: it
could not run without your own position, which the server never sends (see
below).

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

Capture, reassembly, framing and decoding are written and working, and the
screen-reading bot uses what they give: the monsters' places and HP, your own
place, HP and MP, the bag. The packet-only bot that once sat on top of them --
its own targeting, skill rotation and pathfinding -- was never able to run
and went in 2026.11.16.

### What is actually known about the protocol

Measured, not assumed. The detail and the evidence are in
[docs/protocol-findings.md](docs/protocol-findings.md).

- **The server stream parses cleanly** — 100% coverage, zero resyncs — and the
  monster world model matches the commercial client exactly: 42 creatures
  against its 43.
- **`0x0038` is a monster's state event**, sent for every monster on the map
  whenever it stops, starts walking or turns, or is hit. Its **sub-type at
  offset 4** says which: 0, 1 or 9, and 6 or 7 for a blow. The monster's slot
  is at `@8` and its position at `@24`, `@26`, not at the `@80`, `@82` that
  `[Packet.0x0038]` read until 2026.11.03, which is a stale point beside it
  (an old `protocol.ini` line is corrected as it loads, with a warning); a
  blow carries who struck (`@40`), the damage (`@44`) and the HP left
  (`@56`), zero or below for a kill. With the map's monster table at each map
  load, that is every monster's position and HP on the whole map, as GoC has
  them (protocol-findings §4f). *Vị trí từ gói tin* uses it (*Packets first
  (2026.11.03)*, below).
- **Your own position is sent by the client, over UDP**, whenever a walk
  starts, stops or turns, a jump starts or lands, and with each skill cast;
  the server sends it only at a map entry (protocol-findings §4e). Memory
  reading is closed off by XIGNCODE3, and an on-screen anchor never
  converged (`WorldAnchor`, gone in 2026.11.16), so these packets are the
  route; *Vị trí từ gói tin* uses them (P0-3).
- **Map geometry needs no protocol work at all.** The game ships 781 `.map`
  collision files and `MapFile` already reads them; that is what ZTeam's server
  was parsing and charging for.

### A warning about this game's framing

Two things here will cost a day each if met without warning, and both are
configured correctly now:

- **Opcode `0x0081` takes twelve bytes more than its declared length.** Miss it
  and the scanner resyncs, slides a byte at a time and swallows real frames —
  one such packet every few hundred frames hid 73% of the stream. The twelve
  bytes are not a trailer: an `0x0081` wraps one frame packed with liblzf, and
  they are its own 8-byte header and four unused bytes, its length field
  counting the packed bytes after them (`docs/protocol-findings.md` §4e).
  GoBot hands out the frame inside too, right after the wrapper and marked as
  found inside it: from byte 13, still packed, as GoC's own parser takes it,
  or unpacked whole by the tools that read recordings. The bot does not act on
  it yet (P0-3).
- **Both directions are framed with `[Framing]`'s layout.** Every frame the
  client sends over TCP is an `0x0081` wrapper with ciphered content, and what
  `[FramingClient]` reads at offset 6 as an opcode is its check word; the
  client's UDP is plain frames in the same layout (`docs/protocol-findings.md`
  §4e).

---

## Build

No C++ toolchain is required up front; `bootstrap.ps1` installs one.

```powershell
.\bootstrap.ps1     # MSVC Build Tools + CMake  (~4 GB, once)
# open a NEW terminal so PATH updates apply
.\build.ps1
```

The Npcap SDK the programs are built against -- its headers and import
libraries -- is part of this tree, in `third_party/npcap-sdk`, so a build needs
no network. It is looked for there alone, and the configure holds every header
and library in it to the hash `third_party/npcap-sdk.sha256` records, stopping
on one that is missing or changed.

Output lands in `build\bin\Release\`:

| binary | what it is |
|---|---|
| **`gobot_gui.exe`** | **the app — start here.** ImGui front-end, everything in one window |
| `gobot_cli.exe` | command-line tools: `--doctor`, `--login`, `--fix-dpi`, and checks that press nothing; with no argument it prints its usage |
| `gobot_sniff.exe` | protocol discovery, command line |
| `gobot_sniff_files.exe` | gobot_sniff's commands that only read recordings (`--verify`, `--detect`, and `--dump`, `--diff`, `--decode`, `--analyze` with `--from`), with no administrator prompt; it captures nothing, and is not packaged |
| `gobot_test.exe` | self-tests |

`gobot_gui` is the bot. The two CLI tools remain for checks, scripting and
working out the protocol (*Running*, below).

`.\package.ps1 -Version <version>` builds, runs the self-tests as a gate -- a
failed check stops it, and a skipped part is named in a warning -- and
zips what a player needs into `dist\`: the GUI and the CLI, the configs as
shipped, the game's pictures and glyphs, the two notes, `VERSION.txt`, and
`THIRD-PARTY-NOTICES.txt` -- the licences of OpenCV, zlib, Berkeley SoftFloat
(compiled into OpenCV's core) and Dear ImGui, which are linked into the
programs. It refuses while GoBot is running, and refuses a package that
carries anything of this machine's player's. It is the only way GoBot is
packaged: GoBot's CMakeLists.txt has no install rules.

Verify the build:

```powershell
.\build\bin\Release\gobot_test.exe
```

Over 1,700 checks covering the layers that can be tested without the game — frame
extraction and resync, frames that carry a trailer, both directions being
framed, framing auto-detection, TCP reassembly (out-of-order, retransmits,
partial overlap), packet decoding, the packets' world model, the glyph
reader, and the shipped configs themselves. These are where a subtle bug is
most expensive: by the time bad framing surfaces as "the bot walks into a
wall", the cause is six layers down.

A part that needs what only some machines have -- the game's pictures or
glyphs, a saved frame of the client, the source tree -- is skipped there, with
a `[skip]` line saying why, and counted: the run ends `P passed, F failed, S
skipped`, S counting parts, not checks. Frames saved from the live client are
read only as they were when the checks were written, each held to a recorded
hash (a frame with none recorded yet, such as `playerlist.png`, is skipped):
`gobot_cli --read` and `--playerlist` rewrite `frame.png` and `playerlist.png`,
and GoBot writes and prunes `trash_debug` and `quest_debug` as it plays, so a
frame changed since is skipped, not failed. `package.ps1` refuses a build with
a failed check, and warns, naming each, when anything was skipped.

The tests run with no real input. `gobot_test` turns GoBot's "no real input"
switch on before its first test, so nothing it runs can press a key, click,
move the pointer, change the focus, post to another program's window, start or
end a program, or read a launcher's path off a running one: each is refused,
and named in the log as an error. The last lines of the run say how many were
refused. Any beyond the switch's own test is a test reaching for the desktop,
and it wants pretend hands: the run fails on it, so `package.ps1` will not
package that build.

Npcap itself must already be installed (it is, if the original client ran) —
get it from <https://npcap.com/#download>. *WinPcap API-compatible Mode* is
not needed: `wpcap.dll` is loaded by its full path from Npcap's own folder,
`System32\Npcap`, or from `System32` when that mode put a copy there — never
from beside the programs.

**Everything below must run as Administrator.** Npcap will not open an adapter
otherwise.

**Where to put it.** `gobot_gui`, `gobot_cli` and `gobot_sniff` run as
administrator, so their folder must be one only an administrator can change.
A folder made under `C:\` itself (`C:\GoBot`), or in your own user folder (the
Desktop, Downloads), can be written by every program you run, and whatever one
of them left there would run as administrator: a program in place of GoBot's,
or another `launcher` in `configs/login.ini`, which the client keeper starts.
Unzip under `C:\Program Files` (`C:\Program Files\GoBot`; Windows asks for
administrator to make it), and run an editor as administrator to change a
config there by hand. Wherever it is, the programs take no DLL from their own
folder: what they import comes from `System32` (`/DEPENDENTLOADFLAG:0x800`),
anything loaded by name later from `System32` or Npcap's folder
(`harden_dll_search`, `gobot/util.hpp`), and OpenCV is built without the
parallel plugins it would look for beside them. (`GoBotBridge.dll`, the one
exception, went in 2026.11.16 with the two developer commands that loaded
it.)

---

## Getting started

`gobot_gui.exe` opens on one simple screen, in Vietnamese, with eight tabs.
**Đánh quái** is laid out in the order things have to happen:

1. **Trước khi bắt đầu** — a checklist: game window found, GoBot not covering
   it, your character's name, HP/MP being read, keys reaching the game. Each
   line that is not ticked has the button that fixes it next to it.
2. **Bot sẽ làm gì** — fight monsters (or stand still and fight what comes),
   use skills, pick up drops, HP potion, MP potion, the pet's potion, buffs,
   revive on death. Click a key box and press the key to change it.
3. **BẮT ĐẦU**, or **F10** in game. **F12**, from any window, stops everything
   at once: every job is told to stop and every key is let go of before any of
   them is waited for, so one press is enough. What leaves the game tidy still
   happens -- a trash pass gives back an item it has taken out of the bag --
   and so does a shut-down of the client that the client keeper has already
   begun. A Tự chat line half typed is left alone: GoBot presses nothing more
   and leaves it in the game's chat line for you to clear (delete the text,
   then Enter); until GoBot sees the chat line shut, the fighting, potions and
   buffs stay stopped and BẮT ĐẦU waits. **DỪNG LẠI**, or F10 again,
   otherwise stops everything the same way.

Everything that acts on the game ships switched **off**: nothing is pressed
until something is ticked (*Nhảy lên bục trên* and *Tự tìm quái trong vùng*,
2026.11.05b, ship ticked, but act only with **Tự đánh quái** and **Chỗ đánh
quái** ticked, in an area drawn on **Bản đồ**). **Tự hồi sinh** revives on
the spot with a revive item -- the death panel's *Sử dụng vật phẩm hồi
sinh*, then Xác Nhận; never the village (−2% EXP), never Cash -- three
tries a round, and when they fail another round 30 s later, without end,
everything else held while the character lies dead (until 2026.11.24 the
whole bot stopped there and said why in red; see *Tự hồi sinh and Tự vào lại
game: never stopped by themselves (2026.11.24)*, below). Since 2026.11.15d
each of its steps waits up to 30 s for the game to show it, so a slow PC
spends no try on a late answer.
**Tự buff** gives each buff key its
own time, 0–7200 seconds -- at 0 it is cast again every 5 s, as GoC casts a
buff whose cooldown is 0 -- and casts every buff again after a death, a revive
or logging back in. While the bot runs, **Luôn đưa game lên trước** brings the
game back to the front at once whenever another window takes it, GoBot's own
included -- a moment (0.2 s) after the last click or key, so a click lands
first, and up to 10 s while a key box waits for its key (less when HP falls
under the HP potion's mark, or 50% if that is higher); untick it to use the PC
meanwhile. **Tự đóng thông báo** (off until ticked) closes, while the bot
runs, the few popups that get in the way, each once it has stayed put 3 s
and only with nothing else up: the check-in reward's notice by its Xác Nhận,
Yêu Quái Bách Khoa by its X (or ESC), GoBot's own ESC menu by its X, and since
2026.11.09 the *Sự kiện và điểm danh* board the game opens at every login and
as the clock passes 00:00, by ESC -- never its X (*Buying by the slot, and the
check-in board (2026.11.09)*, below). Since 2026.11.15d the board waits
longer: its ESC goes 30 s after the character's arrival in the game (or the
board's first sight), and never from 23:55 to 00:03 by the PC's clock -- at
the day's turn it goes at 00:03 (*Thirty seconds for the game to react, and
the board's ESC after a login (2026.11.15d)*, below). The game reads its keyboard once a
frame, so a key held too briefly is now and then not seen (the key is pressed
and nothing happens): the macro
holds each key 70–110 ms (`key_hold_min_ms`, `key_hold_max_ms` in `macro.ini`),
keeps two potions 60 ms apart, and presses a potion again after
`potion_recheck_ms` (1000) when its bar has not risen and still wants the
drink -- once per press, with a `Potion check:` line in the log; 0 in the
file is the old behaviour. A potion whose bar is under half its mark (a 59% MP
potion: under 29%) may drink again `potion_urgent_ms` (1000) after its last
press instead of after its 3-s interval, so that skills draining MP faster
than one potion per 3 s do not leave the bot behind; 0 is the plain interval.
Since 2026.11.10 that half is of the highest mark of the kind on the same key
(a Lv2 on Lv1's key shares its wait, and says so on its row), and a press's
check takes the highest reading in the `MacroConfig::kRecheckPeakMs` (700 ms)
after its key-down, so a hit just after a heal no longer reads as no rise.
Since 2026.11.17 a potion whose reading has not risen after 3 presses in a row
(HP's not falling further either) goes at its own cooldown, with neither of
those two quicker presses, until a rise, HP falling, a reading over the mark
or none: a misread bar no longer drinks once a second (*MP bar and skills
while hunting (2026.11.17)*, below).
A buff is cast the way GoC casts it, by pressing its key
4 times, 200 ms apart (`buff_presses`, `buff_press_gap_ms`): the game drops a
single press now and then, and a buff dropped is gone for its whole interval.
Since 2026.11.17, with *Ra chiêu theo vòng* pacing, the buff's own cast seen
ends the rest of its presses.
The log has a `Buff: key … cast` line for each cast, and why when one cannot
be. The game has to be windowed, 1024x768, 1280x720 or 1366x768 -- the
readers follow the size. 1024x768 has run in full; at 1280x720 the in-game
screens (HUD, hotbar, chat bar) have run on a player's machine; at 1366x768
only the HUD and the hotbar were seen, in one player's picture (2026-10-26) --
its chat, minimap, bag and dialogs are inferred from the other sizes; the
login has not been tried at either (see 2026.11.13c below). Windows' display
scale can be anything on Windows 10 (1607 or later) and 11: GoBot works in
real screen pixels; on an older Windows keep it at 100%. But above 100%
Windows enlarges a game that
does not handle display scale itself, and a game shown enlarged cannot be
read; the checklist then says *Windows đang phóng to game*, and its **Sửa cho
game** button sets the game's own high-DPI override (as its Compatibility tab
would), after which the game must be started again (`gobot_cli --fix-dpi` does
the same). Or set the display scale to 100%. *Windows đang phóng to chính
GoBot* means a high-DPI override on GoBot's own Compatibility tab holds GoBot
to Windows' scaling: **Sửa cho GoBot** takes it off, then start GoBot again.
Logging back in (**Tự đăng nhập**, **Tự vào lại game**) finds VTCZone's and the
launcher's pages by pictures taken at 100%, so a VTCZone that draws itself
larger above 100% is not recognised -- GoBot says so when a crossing does not
get in (and, since 2026.11.24, tries again 30 s on) -- and needs the display
scale at 100%. On a screen 768 pixels high only
1280x720 fits, with the taskbar set to hide itself; a hidden taskbar's strip,
and anything past the screen's edge, is not counted as covering the game.
A game in a window needs a screen taller than the game plus about 31 pixels
of title bar, and the taskbar's height too unless it hides itself. Since
2026.11.13c the checklist says so: *Game không nằm trọn trên màn hình* when
rows at the bottom or top, or columns at the right or left, of the game are
past the screen's edge or under the taskbar, with the cure worked out from
the sizes.

Fighting finds your character by the name typed on the checklist, and only
by that name, whole on its plate: a player whose name has yours inside it
("TuanAnh" for "Anh", "KhiCon2" for "KhiCon") or is a letter away from it is
not you. A plate with bars may be yours or somebody else's -- a party
member's included -- so a plate is never taken for you by its bars. Without
a name, **Tự đánh quái** does not start (nor does F11): the box under
BẮT ĐẦU says why, and while it is ticked the name's row on the checklist has
a red cross.

The **Tự đánh quái** row reads, line by line: *Phím đánh* and *Phím nhảy*
with its **Thử**; *Kiểu đánh*, GoC's class-and-weapon presets with a
**Tầm đánh…** window for the numbers (reach, minimum distance, up and down);
*Đứng yên một chỗ* and **Giữ khoảng cách**, which steps back from a monster
closer than the minimum, never from one off the character's level, and at
most three steps one way -- from whichever monsters -- until the character
has walked back the other way; then it fights where it stands. GoBot cannot
see where a platform ends, so the first step past an edge is not prevented --
but with a **Bản đồ** area in use and its file, since 2026.11.04d, no step
its floor takes out of the box is taken (`walk_stays_in`).
With any *Kiểu đánh*, *Như trước* included, the character turns to a monster
only when it is not facing it already; since 2026.11.12 the swing waits for
the game's report of the turn, and a target walking through the character
turns it back only once past `facing_tolerance` + 25 px for 0.25 s (*Turns,
the area, falls and two bars*, below). A hurt monster is gone for first only
when it is within reach, and one chased for `give_up_ms` (4 s) without coming
within reach -- behind a wall, up on a ledge -- is passed over for twice as
long, one on
*Đánh trước* too; the box under BẮT ĐẦU says so while nothing else is in
view. Since 2026.11.04d, with no hop to follow -- `jump_to_reach` off, no
jump key, or the spot on (`Hunter::hops_to_targets`) -- and a map file to go
by (a **Bản đồ** area's, or with no spot the one the drops trust; this
run's spot with no saved area goes on as before), a monster more than
*Cao nhất* above that does not stand on the character's own floor, followed
to it slopes and all, is not picked once nothing on the level or below is
left: walked under for `give_up_ms` at a time, such picks carried the
character down a slope out of its area on 2026-11-04 (the owner's Q3,
`Hunter::kSkipAboveUnreached`). Since 2026.11.05b, in a **Bản đồ** area
with *Nhảy lên bục trên*, such a monster that a planned jump up reaches is
picked last instead, and with *Tự tìm quái trong vùng* the seek's
`Listed::NoWay` -- no standing point in the box it gets to -- takes Q3's
place (both below). Without a *Kiểu đánh* every fight still
going after `give_up_ms` is picked again, usually the same monster still in
reach: since 2026.11.04
those are counted apart -- *re-picked while in reach* in the full window,
`(M of them in reach: re-picked every 4 s, Như trước)` in the minute's
`Hunter:` line -- and no longer read as given up out of reach. That line
also counts the picks whose nearest monster was off the level (`nearest off
level: below N, above M of P picks`) and the time spent waiting with a
monster below in reach. Under the row, **Lọc quái** opens *Danh sách quái*:
monsters never attacked (*Không đánh*) and monsters gone for first
(*Đánh trước*), picked from the names being read (*Quái đang thấy*;
**Nhìn quanh** reads the screen for up to a minute and presses nothing) or
typed. **Tự dùng kỹ năng**
has **Chỉ khi đang đánh quái** -- skills only while a monster in reach is
being attacked, each on its own cooldown, none under 10% MP -- and
**Chi tiết…** for each skill's cooldown, range and *Luôn bấm theo giờ*,
*Dùng skill tuần tự* and, since 2026.11.08, *Ra chiêu theo vòng* (under
*Keyboard macro*; since 2026.11.16b on the row itself too, on by default, the
skills one after another as the game casts them, while **Tự đánh quái**
hunts as well -- *Skills in turn while hunting too*); a key sequence left on
in macro.ini is shown there with a
**Tắt**. *Chỉ khi đang đánh quái*, and with it *Dùng skill tuần tự*, the
ranges and the MP floor, act only while **Tự đánh quái** runs
(`MacroEngine::following`, the skills then going by `run_skills` -- or, paced
since 2026.11.16b, by `run_paced`, which holds to the same gates): with the
macro alone every skill key keeps its own timer (`run_timers`) or goes in the
rotation, ticked or not. Since 2026.11.12, while **Tự đánh quái** runs, ticked or not, the
hunter has the skill keys held while a turn waits, through a drop, with
nothing to fight in its area and outside it, and the dash's key near an
edge; and a key on its own timer waits beyond its range while the hunter
fights (*Turns, the area, falls and two bars*, below). Until 2026.11.08 the
hover over *Dùng skill tuần tự* began *Như GoC*
and never said so; the owner's macro-only run of 2026-11-08 had both
ticked, for nothing. Each potion row
has a second tier, *Lv2*: under both marks only the lower one drinks, and a
potion due goes before any other key. **Tự bơm máu pet** is the third potion
row, the pet's: its key is pressed when the pet's HP is at or under *khi máu
pet dưới*, after the HP and MP potions and never ahead of them -- since
2026.11.10 over and over, every *Bơm liên tục, mỗi N ms* (`pet_every_ms`,
200-3000, 300 in a file without it or with no macro.ini at all), one value
for its Lv1 and Lv2, as GoC
presses the pet's food; after a press of the pet's a skill that is ready goes
next, so the pet takes at most every other key while skills are due. Once a
minute the log's *Pet heals:* line says how many presses went out and how
many the bar rose on. The pet's HP is the red bar in its slot on the HUD, under the HP and
MP bars -- the wide slot with two pictures and *Thức Ăn* beside them; the
purple bar under the red one is not the pet's HP. GoBot finds that bar by
itself and keeps it through 0. With no pet out the row says it cannot see
the bar, and the key holds. The row starts with no key (ticking it asks for
one), and its slot's potions are not counted. In `macro.ini` the trigger is
`pet_hp_below`. All of these are *Thử nghiệm*, and
each ships off or as the bot fought before: *Như trước*, both ticks off,
empty lists, and no jump key until the player sets one (an existing
hunt.ini keeps its own). **Thử** beside the jump key is the one button here
that presses a key in the game: the jump key, once, with the game brought
to the front. A hop toward a monster above comes only once the last jump
is over (about 0.8 s), never as a second press in the air. *Nhảy lên bục
trên* (2026.11.05b, below) needs the jump key to be ↑, and presses Ctrl + ↑
as well; its **Thử ↑**, **Thử ↑↑** and **Thử Ctrl+↑** press one jump each.
GoBot never
presses Down on its own, whatever key is set to it: standing on a gate, Down
takes the character to another map. Key boxes refuse it, and a jump key set
to it is marked red on the checklist. The one Down GoBot sends is the drop's,
inside Ctrl, with *Nhảy xuống bậc dưới* ticked (below).

**Vị trí từ gói tin**, beside *Giữ khoảng cách* (*Thử nghiệm*), takes your
position and the monsters' from the game's own packets instead of the name
plates, which drop out for a frame or two all the time. It needs Npcap;
GoBot already runs as administrator. From the moment GoBot opens, hunting or
not (since 2026.11.03: *Packets first*, below) -- unless
`packet_positions=off` in `hunt.ini` -- GoBot captures the game's own TCP and
UDP, found by the game window's process -- its connections and UDP sockets
alone, so a second game client on the PC stays out -- and reads them: nothing
is sent or changed. A capture already running (one started by hand under
*Tools > Capture* in **Nâng cao ►**) is used as it is and never stopped by the
hunting; one stopped by hand stays stopped until the next hunt; a game
started again or reconnected is followed within a second or two, and when it
opens a new UDP socket or goes quiet for 5 s the capture is started again at
most every 10 s -- less and less often while the capture itself refuses to
start, on an adapter it cannot use (since 2026.11.24: 10 s, 20 s, then every
30 s, never longer; a capture whose read fails is stopped and started again
the same way; quiet still after that start, again every 30 s while the game
holds its connection). Unticked
(`watch`, the default), the screen still fights and every 10 s
`gobot_gui.log` gets a *Positions* line: what the packets carried, how many
monsters both views see and how far apart,
the scale, and how much of the time the packets could have steered. Ticked
(`on`), the packets steer whenever they are healthy -- Npcap there, the
game's traffic arriving, a report of yours since the capture began (move or
cast once), no other character's reports, the map's monsters settled, no
*Lọc quái* list in use (the packets carry no names) -- and the screen takes
over at once whenever they are not, the packets again only after 2 s of
health. The card under BẮT ĐẦU says which, and why: *Vị trí: từ gói tin*, or
*Vị trí: từ màn hình — …*. Monsters are offered within 0.6 of the game
window's width and height around you, about what the screen shows. None of
this has been measured on the game yet: `packet_scale` (world pixels per
screen pixel, 1) and `packet_dy` (added to each monster's height against
yours, 0) in `hunt.ini` lay the packets over the screen without a new build,
and the *Positions* line prints what it measured beside them. The full
window's hunting settings have the same choice: *Positions from packets*,
Off, Watch or On.

**Chỗ đánh quái**, beside *Vị trí từ gói tin* (*Thử nghiệm*; `spot=false`
in `hunt.ini` as shipped), keeps the hunting round one spot. The spot is where
the character stands, still and on the ground, the first time the game's
packets place it after BẮT ĐẦU -- in the packets' world pixels, which do not
move with the view. The packets place you only once you have walked or cast
since the capture began, and until then the hunter hunts as without a spot,
so the spot can be where the first walk to a monster ended rather than where
BẮT ĐẦU was pressed; **Đặt chỗ này** beside the tick takes it again where the
character stands. Only monsters inside the area round it are walked to
(`spot_half_w`, 300 either side, and `spot_half_h`, 100 above and below, in
the **Tầm đánh…** window); until 2026.11.12 one within reach was fought
wherever it stood -- since then (the owner's Q3, *"Chỉ đánh quái trong
vùng"*) a monster outside the area is never faced, fought or walked to, in
reach or not (`mark_out_of_area`) -- and a target that leaves the area by
more than 60 px before it is reached is let go of. With nothing to fight for `spot_return_delay_ms` (1.5 s) and the
character more than `spot_tolerance` (48 px) off the spot along the ground, it
walks back -- *Đang về chỗ đánh quái (còn N px)* -- and any monster that comes
within reach or into the area takes over at once. A walk back that the
packets' own reports show getting no 20 px nearer in `spot_give_up_ms` (4 s)
is let go of and tried again `spot_retry_ms` (30 s) later; after three in a
row, not until **Đặt chỗ này** or the character is back at the spot by itself.
With the spot on, the hunter never hops toward a monster -- a hop can leave
the character on the platform above -- while the walk back hops toward a spot
up to one jump (120 px) above, with a jump key set and *Jump at things above
you* (`jump_to_reach`) on, and does not walk back to one higher. The walk
back never presses Down, so a spot below is reached only by walking off a
platform's edge toward it, where the platform allows that. An area drawn on
**Bản đồ** goes by its map's file instead (below): its walks kept in its box,
and its walk back along the floors, never a hop -- since 2026.11.05b with
planned jumps up where no walk gets there (*Nhảy lên bục trên*), still
never a hop. Since 2026.11.12 this run's spot goes the same way once the
hunter's `MapTrust` trusts the map (`Hunter::zone_page`), its home set on
the floor under it, the seek alone left to a **Bản đồ** area.
A spot taken so is for the current run only: every BẮT ĐẦU takes it afresh,
and nothing of it is saved -- but a map with a hunting area drawn on
**Bản đồ** (below) is hunted in that area instead, once the character stands
in it or near it. A map entry or a log back in sets the
spot aside, and the character stands and fights only what comes within
reach, as with *Đứng yên một chỗ*. After a map change (a door) it stands
until the character comes back through a door to near where it left -- a
map entry whose spawn is within 150 px of that. After a log back in (by
**Tự đăng nhập**, or a game opened again and found with **Tìm lại**) it
stands until the character stands, still, within 150 px of where it was or
of the spot -- placed by the new game's packets, after its first step or
skill, and not on a map they know is another. A log in after a door keeps
the door's rule. (Since 2026.11.10 a **Bản đồ** area set aside by a map
entry is taken again whichever door brings the character back to its map,
once the map has loaded: below.) Otherwise it stands until **Đặt chỗ này** is pressed. It stands as well while
the packets have lost the character. To take the character somewhere else
while the bot runs: untick **Chỗ đánh quái** (the walk back stops at once and
the spot is dropped), walk there, tick it again and press **Đặt chỗ này** --
which, on a map with an area, moves the area there and saves it.
It needs Npcap -- the packets alone place the character, whether
*Vị trí từ gói tin* is ticked or not -- so without Npcap, or with
`packet_positions=off`, fighting does not start while it is ticked (the rest
does), the box under BẮT ĐẦU says why, and the checklist has a line for
Npcap. The card under BẮT ĐẦU has a *Chỗ đánh quái:* line -- where the spot
is and how far off the character is, or why it stands -- and the 10-s
*Positions* line in `gobot_gui.log` adds the spot, the distance, the walks
back and those given up on.

**Nhảy xuống bậc dưới**, after *Vị trí từ gói tin* (*Thử nghiệm*, since
2026.11.04; `drop_down=false` in `hunt.ini` as shipped), drops through the
platform underfoot with Ctrl + Down to a target on a floor below: one more
than the band's *Thấp nhất* below -- since 2026.11.12 the lower bar on
**Bản đồ** and nothing else; until then `drop_min_px` (100) if that was
larger, a key no longer read -- and no more than `drop_side_px` (120)
across, with nothing to fight on the character's own level, which always goes first, on *Đánh trước*
or not. With nothing on its level and a drop able to go -- the map trusted,
drops not off for the run (until 2026.11.24; nothing turns them off since) --
the nearest monster below is picked before one
above, which a hop would only chase. The hunter walks toward the monster
and, with a drop able to go, lets go of the walk once within 24 px across of
it (`Hunter::kDropStopAbovePx`, the owner's Q6 of 2026.11.04d; before, a
drop's settle only ever began on a look taken mid-walk, 24 px either side),
judges the drop from where the character then stands -- the landing's floor
8 px either side -- waits for the packets to report it standing, and
presses. The tick is greyed out, its reason in the tooltip, while *Đứng
yên một chỗ* is ticked (never a drop then), while *Vị trí từ gói tin* is not
(the packets steer every drop: never in Watch, nor once the screen has taken
over), and while a key of the hunter's -- jump, left, right, attack -- is
Ctrl, or one of GoBot's own (F10, F11, F12, the two hotkeys) is Ctrl or
Down; the full window's *Drop to things below you (Ctrl+Down)* is greyed the
same way. With the hunting spot on, a drop goes only while the spot is in
use, and only when where the character stands, where it lands and the target
all lie inside the area: with the shipped `spot_half_h` of 100, a monster
more than 100 px below the spot is outside it already -- an area drawn on
**Bản đồ** with its home on the floor below and its box run up over the one
above lets the drop go. (04c's notes said to run the box's bottom edge below
the floor beneath instead: that leaves most of the box red, never walked
back from.) The walk back never drops.

Where it may drop comes from the game's own files, read only:
`data\Map\<stem>.map`, read planar -- the floors under every x, with their
attribute -- and `data\Project\<stem>.prj` -- the gates, its warp-zone and
portal objects -- in the folder `MapFile` finds `data\Map` in. Ctrl + Down
is pressed only when the floor a straight fall lands on is the monster's or
one between, never one below it, and the same floor a few pixels either
side -- since 2026.11.04d the landing's floor and the character's own each
followed column by column as they run, sloping or flat, across holes of 1-2
px (`GameMapBook::Page::follow`), where 04c asked for the same height either
side and so did not drop once on t52_s2, whose floors all slope, on
2026-11-04. The monster's floor is the one it last stood on (two of its
frames at one height, 300 ms apart), followed to where it is now, else the
first under its point within 100 px (`Hunter::kDropMonsterFloorPx`): its
point alone is often in the air, a contact frame or a knock; and the landing
is on it when the landing's floor, followed to the monster, is that floor.
Pressed, too, only when the character does not stand on the bottom ground
(only a platform is dropped through); and when no gate lies within `drop_gate_px`
(150) across, from a little above where it stands to a little below where
it lands: Ctrl + Down on a gate changes the map. The map has to be known
and its file trusted. A map entry (`0x0086`) seen since the capture began
names it, and is trusted at once when its spawn lies by one of the file's
gates, or after three standing points on its floors. With GoBot opened
mid-map ("map 0/0") the standing points name it -- your settled reports and
the monsters that stand: at least 12, apart from each other, every one on a
floor of exactly one of the game's `.map` files, with every other file
missing at least 6 of them. A monster stands once two of its frames (the
same slot and generation) put it at the same height, within 2 px, 300 ms
apart or more: a frame alone may be a spawn still in the air, or a monster
thrown. A real entry always wins. Since 2026.11.04b only your own points
let a map go -- two settled reports off its floors, 0.5 s apart or more,
with none on them between (one place twice or two places: either counts;
one alone is logged and let pass) -- and then a named one is named afresh,
an entry's not until the next entry (since 2026.11.24: not until three
standing points on its floors gathered since take it back, never by the
spawn again -- *"Hunter: the game's map file t52_s2 fits where you stand again
(3 standing points on its floors since it was found wrong) -- gone by
again"*; a really wrong file never gathers them). A monster's frame off the floors never
does: it is counted, and logged at most once a minute with the floors above
and below it (in 2026.11.04 one such frame, 31 px above its floor, kept
drops off for a whole stay). Only while an entry's file still waits for its
three standing points does a monster count against it: one that stands off
its floors sets that count back to 0, as a miss does to a name; a file once
trusted it never touches. A map whose `.prj` lists no gate is never
trusted. The hint under the row says how far the naming has got.

The press is `InputManager::drop_ctrl_down`, the one way Down ever goes
down: Ctrl alone for 40-60 ms (Ctrl alone is the game's pick-up, harmless),
then Ctrl and Down again in one `SendInput` array, Down held 70-110 ms, Down
up, 40-60 ms, Ctrl up -- Down always up before Ctrl, and the ups whatever is
in front. `inject()` refuses every array that puts Down down unless it is
that call's own chord, with a Ctrl-down before the Down in that same array;
`[Ctrl↓, ↓↓]` from any other path is refused too, so no key box, setting or
other path can send a bare Down. The coordinator's token is taken only when
nobody holds it, and while a drop runs or owes a key-up, every other
thread's key-downs and clicks are refused. F12, DỪNG LẠI, HUNT OFF and the
game losing focus send the ups at once and cut a drop short; an up that did
not go out is sent again every 100 ms, and after 3 failed tries the hunt
stopped with a red line until 2026.11.24 (*phím ↓ hoặc Ctrl có thể vẫn đang
bị giữ*); since, every key is let go of once (`release_all`, then
`release_stray_drop_keys`), an amber line says so (*Phím ↓ hoặc Ctrl có thể
đang bị giữ: GoBot đã nhả mọi phím; đánh quái tự chạy tiếp khi Windows nhận
lệnh nhả phím.*), the hunter holds still pass by pass while the key-ups are
owed (`kDropOwedPasses`, `Status::drop_keys_stuck`) and goes on by itself
once they went out. With the
switch on, GoBot's start and every BẮT ĐẦU let go of a Down or Ctrl that
Windows still reads down, as a GoBot ended mid-drop would leave them. Since
2026.11.05b the jump up's Ctrl + ↑ is this chord with ↑ for its second key
(`InputManager::high_jump`, `chord_ctrl(ChordKey::Up)`), sent without the
drop's chord flag, so the guard on Down is as it was;
`chord_ctrl(ChordKey::Down)` is `drop_ctrl_down`, its events unchanged.
With either switch on, a stray ↑ is let go of too, ↑ before Ctrl. A high
jump's ↑-up left owed is said with ↑ in the line instead (*Phím ↑ hoặc Ctrl
có thể đang bị giữ*, `InputManager::up_owed`; until 2026.11.24 it stopped the
hunt, `DropStop::JumpKeysStuck`): pressing ↓ would not let go of ↑.

What came of a press is judged from the packets alone: landed on the floor
the file gave, followed to the column it came down at (worked; since
2026.11.04d -- on a slope a few pixels across is as many up or down, and a
good landing read as off the file would have turned drops off and the file
with them), no take-off within 1 s (did nothing), a jump in
place, a rise, or a landing elsewhere. Did nothing and a jump in place
remember the place -- no drop within 64 px of it until GoBot closes (since
2026.11.24 for 30 s, `kLearnedForgetMs`) -- and
pass the monster over for twice `give_up_ms`; 3 in a row, or 5 in a run,
switch drops off until the next start, as does a rise, a landing off the
file (whose file is then not gone by for 2 minutes, since 2026.11.15) or Down
held over 180 ms. (Since 2026.11.15 only the second press that did nothing near one place
remembers it, the first passes its monster over 1.2 s, and the 3 in a row or 5
in a run -- more than worked -- pause the drops 30 s, 60 s, 120 s instead of
switching them off: see the section of 2026.11.15. Since 2026.11.24 nothing of
this pauses or switches the drops off: a press that did not drop -- nothing,
a jump in place, a rise -- is tried again at the next look, the monster
passed over 1.2 s for the first of a row and one fight turn, 3 s, for every
other; a place learned is skipped 30 s, the next press from another column,
then tried again; a landing off the file has `MapTrust` let the file go
until 3 standing points on its floors take it again, no clock; Down held long
is only said: see the section of 2026.11.24.) At most 4 presses a minute
and 2 per monster, 1.5 s apart -- since
2026.11.05b from a jump up's press too, which shares the press record, the
map-change watch and the stuck-key halt -- none within 5 s of a map entry,
and none while a hold is on, the game is behind,
the character is down or HP is not read (since 2026.11.12b; before, also HP
under the HP potion's mark, 50% at the least), the macro holds for a
character out of the world, or the keeper's crossing runs. A map change within 10 s of a press, whatever was judged of it, stopped
the hunt, unticked *Nhảy xuống bậc dưới* and saved `hunt.ini` until
2026.11.24; since, both ends are learned as gates for the session -- as they
were -- and the hunt goes on where the character came, nothing unticked or
saved, with a WARN (`Hunter: MAP CHANGED … -- both ends learned as gates …;
hunting goes on, Nhảy xuống stays ticked`) and an amber line under the tick
(*Lần nhảy xuống lúc … làm đổi bản đồ — chỗ đó đã được nhớ là cửa, GoBot không
nhảy ở gần đó nữa và vẫn đánh quái tiếp.*) until the next start. The log has a line
per press (`Hunter: drop (Ctrl+Down) toward …`) and per verdict, `Hunter:
not dropping … -- <why>` once a target for what the player can act on --
since 2026.11.04d with where the character and the monster were judged to
stand and their floors -- and the minute's `Hunter:` line gains `; drops N
(… worked, … did nothing, …)` and `; drop looks N (settles N; refused: …)`,
the looks at a target below and what refused them, most first.
`gobot_cli`'s hunting never drops.

**Bản đồ**, the second tab (*Thử nghiệm*, since 2026.11.04c; run on the game
as 04c and as 04d, 2026.11.05's changes not yet), draws the map the
character is on as GoC does: the platforms
(attribute 1, green), the bottom ground (2, blue), walls and blocks, the
gates (a pink + in a red box; those learned this session faded), you (blue,
with a small triangle the way the packets last said you face -- read only),
the monsters (red, with an HP bar from zoom 0.5; grey while the hunter goes
by a spot or an area and they are outside it and out of reach -- since
2026.11.05, outside it in reach too while the hunter walks back first, and
since 2026.11.12 outside it whatever the reach) and the
one being fought (a yellow ring). The floors and gates come from the game's
own `.map` and `.prj`, read only, as the drop reads them; the places from the
packets -- so it needs Npcap and `packet_positions` other than `off`, but no
hunt and no tick of *Vị trí từ gói tin*. Which map: a map entry (`0x0086`)
seen since the capture began (*game báo khi vào bản đồ*), or, with none, the
tab's own `MapTrust` naming it from standing points by the drop's rule (12,
and the same margin) (*nhận ra từ chỗ đứng*) -- only with its file trusted,
or its `.prj` holding no gate: such a map is named, though never dropped on.
A named file that is doubtful names nothing; an entry's file found off
(Mismatch, Distrusted) is drawn faded. The name beside the stem is
`tb_Map`'s. The two `MapTrust`s are apart: while the tab knows the map and
the drop's still names it, the drop's line says so. Until the map is known
the tab draws you and the monsters on a 100-px grid. The wheel or [-] [+]
zoom by 1.2, a drag with any button that grips nothing pans, *Theo nhân
vật* follows you (a double-click too), *Cả bản đồ* fits; *Chi tiết* adds,
for the session, the fall line while airborne, the last report, the walk
back and the hunter's monster box. Mouse only: no field takes typing, which
the front keeper would send to the game while a run is on.

Since 2026.11.05 (not yet run on the game; the owner's *lệch chỗ* and *thừa
chấm* of 2026-11-05) the dots go where the game has them by the map's file,
in the drawing alone (`mob_dot`, `you_dot`, `mob_dots` in
`gobot/map_view.hpp`). Between its frames the packets give a walking
monster's x alone and the world keeps its last frame's height
(`follow_mob`), so on t52_s2's ramps -- 586 at x 750, 715 at x 1100, 802 at
x 1400 -- a walker's dot floated over its floor or sank under it by 10-170
px, and jumped back at its next frame; yours did the same between your
reports. A walker -- its x moved off where its last placing frame put it
(`PacketWorld::Mob::frame_x`, `frame_y`, data only: nothing in the hunter
reads them) -- is now drawn on the floor that frame stood on, followed
column by column across holes of 1-2 px (`GameMapBook::Page::follow`) to
its x, at the frame's height over the floor; where that floor ends first,
or a wall, the map's edge or `kDotFollowMaxPx` (2048) columns, it is held
there, as the game turns a monster at a platform's end. A walker whose frame
stood on no floor, or on a wall, is drawn where the world has it -- its x
followed, its frame's height -- as until 04d, and so is every living monster
on the grid, or on an entry's file found off (drawn faded), whose floors are
not gone by (`dot_floors`); so is your dot there. A living monster standing
on no floor -- a spawn point, 15-66 px over its floor until its first stop --
is set down `kDotStandPx` (1) over the floor the hunter takes for it
(`Hunter::monster_floor`: the first at or below it within 100 px).
You, walking on the ground with the fix known, are drawn on your floor from
your last report (`Me::report_x`, `report_y`) the same way; airborne, lost
or standing, as the fix has you. The dead are hidden at once
(`kMapDeadShownMs`, 0, the owner's Q1 (a), as GoC lists only HP above 0;
until 04d the tab kept them faded for `PacketEyes::kDeadShownMs`, 1.5 s,
where the killing blow found them, which stays the hunter's). Under *Chưa
có gói tin của game* or *Vị trí từ gói tin đang tắt* (`map_shows_mobs`) no
monster, ring or monster hover is drawn and the grid's box is not fitted to
them: the world keeps its last monsters, a walker going on for 5 s, until a
capture starts again. The grey (`mob_grey`) and the hunter's box under *Chi
tiết* go by the world's own heights, as the hunter does -- the owner's Q2
(a): the hunter keeps its heights until answered (since 2026.11.05b, while
an area is in use with *Tự tìm quái trong vùng*, the hunter's plates stand
where `mob_place` puts the dots: the seek's Q3 (a), `kPlatesOnFloors`,
below; the grey still goes by the world's heights) -- so near the box's edge
on a slope a dot drawn inside it may be grey, or one outside red. The one
being fought turns grey only once `Hunter::kSpotKeepPx` (60) past the box's
edge, where `Hunter::mark_out_of_area` lets it go. A monster's hover ends
with where its dot is drawn, and the line over the map starts with where
yours is (`Nhân vật: x, y`), each with `(gói tin x, y)`, where the packets
have it, when that is elsewhere.

On it each map gets one **hunting area**: a free box and its *chỗ về* (the
home), a point on a floor inside it that the bot walks back to. It stands in
for the spot and its ±`spot_half_w` / ±`spot_half_h`, and need not be even
round the home: put the home on the lowest floor to be hunted and run the
box up over the floors above it, which the bot comes back down from by
walking off their ends, and where drops land inside. (04c's notes said to
run the box well below the home instead; the owner's box of 2026-11-04 so
drawn had 1068 of its 1814 floor columns red, and the bot left it for good.
Nor is a home best made right after a door: the arrival spawn, where few
monsters come.) *Tạo
vùng quanh nhân vật* (with the character settled) or the right click's *Tạo
vùng ở đây* makes one ±`spot_half_w` by ±`spot_half_h` round the home, cut to
the map. A left drag on the home's pin moves the home, on an edge or a corner
resizes, on the 14-px strip along the top edge moves the whole box; a drag
anywhere else, inside the box too, pans. The camera is frozen from the press
to the release, and only the release commits, only when the box or the home
changed: the home is snapped to the floor at or just under where it was let
go (else the nearest above, else the nearest within 16 columns), and a box
under 100 x 40, off the map, with no floor, or with its home on none is
refused with a note. Otherwise it is saved at once, with *Hoàn tác* (one
level) and *Xoá vùng* (asked first). The right click also has *Đặt chỗ về ở
đây*, *Dời vùng tới đây* and the copy of a coordinate; *Dời vùng tới nhân
vật* moves the box, its size and the home's place in it kept. Worked out when
the area (at every edit), its file, the tab's gates or the drop switch
changes, never a frame at a time, and said and drawn: floors more than 120 px
below the home (red: never walked back from -- since 2026.11.04d, when they
are more than half the box's floor columns, `kZoneRedBadPercent`, said in
red rather than amber, and every save that leaves the box so adds a red note
and a `Map tab: hunting area for … has N of M floor columns red` line),
floors more than 31 px above it (amber: only by walking off an edge), a home
within 60 px of a gate, the page's or one learned (`kZoneHomeGatePx`, since
2026.11.04d: amber, a warning never a refusal -- a map entry's spawn), and,
with drops on, a gate or a no-drop piece in the box and a box too short for
any drop. On a doubtful file
or none, edges and corners only, and the right click neither makes nor moves
one: the home is never moved without a floor to put it on, but to where the
character stands. A left press let go under ImGui's drag threshold saves
nothing. With drops on, the platform pieces where the drop's gate rule
refuses are drawn pale red (`no_drop_marks`, checked against
`Hunter::drop_rule`).

The areas live in `configs/spots.ini`: a section per lowercase stem
(`[t52_s2]`: `home_x`, `home_y`, `left`, `top`, `right`, `bottom`, `saved`),
written by GoBot alone and never shipped; an edit never writes `hunt.ini`.
The tab's worker writes it through `ZoneSaveQueue` -- the latest value per
map, one batch out at a time, one that failed owed again and tried every
5 s, what is still owed written as GoBot closes -- to `spots.ini.new`, read
back, then moved over the file, every section and key it does not know kept
(comments are not). A `spots.ini` that is there and does not read is never
written over: nothing is edited that session, and a red line says why.

With **Tự đánh quái** and **Chỗ đánh quái** ticked, the area of the map the
tab knows is handed to the hunter (`Hunter::set_zone`) for the map entry it
was known at, whether the tab was ever opened or not; a named map's only
once a settled report of this entry lies on its floors and none of yours
off them -- and on a named map, what is put where the character stands
(*Tạo vùng quanh nhân vật*, *Dời vùng tới nhân vật*, **Đặt chỗ này**, this
run's spot kept) is held to the same (`zone_here_ok`, `spot_keep_ok`). With
the positions switched off or the capture stopped by hand no map is known:
nothing is edited or sent. The hunter keeps it waiting until it applies,
drops it once the entries count past it, and takes it -- `zone_take`, the
owner's Q1 -- only
with the character settled inside the box or within 150 px of it, or back
within 150 px of where a spot of it was left (a door there and back, a log
in) -- since 2026.11.10 anywhere on its map when a door brought the
character back to it (`ZoneWait::Loading`, then Back, below); an edit of the
area in use applies at once, wherever the character is.
Otherwise the hunt goes on as before, round where the character stands, and
the card says how far outside the area it is. A map entry sets an area's
spot aside as it does any spot; when the tab no longer knows the character
is on that map (a capture started again, a name let go of, the positions
switched off) it is set aside (`zone_aside`) until the area is sent again,
and the card says so; a deleted area is dropped
(`clear_zone`) and the spot taken again where the character stands.
**Đặt chỗ này** on a map with an area moves the area to where the character
stands, its size kept, and saves it, with *Hoàn tác* on Bản đồ (the owner's
Q2); elsewhere it does what it did. An area may be as large as the map (Q3).
Each of the three answers is one constant or function in
`gobot/hunt_zone.hpp`.

Since 2026.11.04d (run on the game once, 2026-11-05) an area in use goes by its
map's file too, wherever the file is not found wrong (a landing off it -- given
back after 2 minutes or at the next start, since 2026.11.15 -- or, while drops
look, two settled reports of yours off its floors) and the character stands on
one of its floors; otherwise, and for
this run's spot, the hunter goes as it did. The owner's answers after the first live hunt
with an area -- 2026-11-04, when the character walked down t52_s2's sloping
ground out of the box and never came back -- are not in yet, so each is its
(a), in one constant or function:
- **No walk out of the box** (Q1, `kZoneWalkStaysIn`, `kZoneFallInsideOk`).
  Before each walk toward a monster the floor underfoot is followed ahead
  (`walk_stays_in`), as far as the walk goes before it stops for the target
  and `Hunter::kZoneWalkSlackPx` (48) more, 96 px at most: a standing point
  on it further outside the box than the character is now, or a walk off its
  end that lands further outside or on nothing -- worked out at 0, 1/3, 2/3
  and all of GoC's 360 px/s across, under its gravity -- refuses the walk.
  The keys are let go of, the target passed over for twice `give_up_ms`, its
  fight ended, `Hunter: not walking …` said at most once every 5 s and every
  one counted in the minute's `Hunter:` line (`walks stopped at the hunting
  area's edge N`). A fall onto a floor inside the box is walked; so, from
  outside, is any step that comes no further out. A drop refused, its target
  walked to, goes by the same rule.
- **The way back** (`return_point`): the floor the character stands on
  followed both ways to its ends, across holes of 1-2 px, and off them -- at
  most `kZoneReturnFallsMax` (3) falls, each landing inside the box at every
  one of those speeds. It ends at the home, else at the standing point in
  the box so reached with the fewest falls, then the nearest the home across;
  it is walked with no hop (`WalkFor::Floor`) and never a Down, and stood at.
  Walked past its floor's end with no landing's report, it is held no further
  than the point it walks to; standing on none of the file's floors, the
  walk back goes as before 04d.
  So a home more than one jump up is walked back to when a slope leads there.
  Since 2026.11.05b, with *Nhảy lên bục trên* able to go, up to
  `kZoneReturnJumpsMax` (2) jumps up are legs too, fewer jumps first (below).
  With none, a home no more than one jump above is walked toward as every
  walk back went before, hops and all -- unless *Nhảy lên bục trên* is
  ticked where jumps up may be planned (the jump key ↑, the packets
  steering, the map trusted: `Hunter::jump_area`), where the old hop never
  presses, off for the run too (until 2026.11.24; nothing turns the jumps up
  off for the run since), and under the jumps up's rules no way is
  none. Else the spot's state is *NoReturn*
  (in place of *TooHigh*): it fights where it stands, says so once (`Hunter:
  no way back into the hunting area …`), and the card says so in amber.
  Since 2026.11.12 the way is looked for over the map's other floors too,
  and with none the character stands, *stranded* (below).
- **Home by height** (`zone_at_home`): within `spot_tolerance` across and on
  the home's own floor, followed there; off every floor, within 31 px up or
  down. 04c went by x alone: 882,616, under the ledge's home, was "home".
- **The walk back first** (Q2, `zone_walks_back_first`): more than
  `kZoneOutsideBackPx` (60) outside the box with a way back the file shows,
  monsters outside the box are let be, in reach too, and the walk back comes
  first; within 60 px -- a slope crossing the edge -- in-reach monsters are
  fought as before, and so they are while a walk back that got nowhere waits
  `spot_retry_ms` to be tried again, and after `Hunter::kSpotMaxTries` (3)
  (since 2026.11.12 standing instead, and tried once more after a long wait;
  since 2026.11.24 a new round of three `spot_retry_ms` on, without end).
  Since 2026.11.10 (the owner's answer "Ngưng đánh, đi về trước") the walk
  back first is latched (`Hunter::track_back_walk`) and held until the
  character is inside the box, past the 60 px edge too: nothing is fought
  meanwhile, monsters in the box and in reach among them; `go_home` comes
  before the attack in reach and `choose_in_reach`, with no
  `spot_return_delay_ms`; a wall still gives up after `spot_give_up_ms`, and
  then it fights where it stands (since 2026.11.12 it stands, fighting only
  what is in reach inside the area). This run's own spot has it too, the spot no
  more than one jump up, walked along the ground with no hop. While the
  hunter walks back (`Hunter::returning`) the macro holds every skill key,
  `always_timer` too (`MacroEngine::Fight::Mode::Returning`); potions and
  buffs go on.
  Q4: no hop toward a monster is added (`hops_to_targets`, as it was) --
  reversed by the owner 2026-11-05: in an area of **Bản đồ** a monster above
  is jumped up to, the jump planned by the map's file, with *Nhảy lên bục
  trên* (2026.11.05b, below); the hop itself still never goes toward a
  monster with the spot on. Q3 is above, under the hunting row.
- **Where it stands, said**: outside the box with no other reason to give,
  the card under the map says *Nhân vật đang ở ngoài vùng N px* and the way
  back in amber, never the green *Đang dùng vùng này* (it said that 522 px
  off on 2026-11-04), the card under BẮT ĐẦU turns amber too, and the
  *Positions* line adds `outside the area N px` (`walking back first, to
  x,y`). *Tạo vùng quanh nhân vật* still puts the home where the character
  stands (Q5, `kZoneAroundHomeLowest`; `zone_home_lowest` is ready for (b),
  the box's lowest floor), with the red line above when most of the box
  cannot walk back to it.

Since 2026.11.05b (not yet run on the game) an area in use has two more
switches, both ticked as shipped -- the owner's answer 5 of 2026-11-05,
*Bật sẵn*, and the seek's Q7 (a); a `hunt.ini` without `jump_up` or
`seek_in_zone` reads true -- and both act only while an area of **Bản đồ**
is in use with its map's file (the jumps up only on a map `MapTrust`
trusts), with `packet_positions=on` and *Đứng yên một chỗ* off
(`Hunter::jumps_may_go`, `Hunter::seek_wanted`; the ticks
are greyed with their reasons by `jump_up_tick` and `seek_tick`,
`gobot/hunt_card.hpp`). This run's spot with no saved area and no spot hunt
as 2026.11.05 did; in Watch neither acts, and while the screen's plates
steer -- a *Lọc quái* list in use, the packets unhealthy -- the seek stands
aside and no monster is jumped to.

**Nhảy lên bục trên** (`jump_up`; *Jump up onto floors above (Up / Up Up /
Ctrl+Up)* in the full window) jumps straight up -- no arrow held, no
steering in the air -- onto the floor of a monster above, and onto the
floors of a way back, with the jump key ↑ (0x26) and nothing else. The key
is the cheapest the game's own map file shows landing for sure
(`gobot/jump_plan.hpp`), by a model of the game's own 0x002A speeds -- ↑
−800 px/s (state 1); ↑↑'s second ↑, pressed `kJumpUpSecondAtMs` (340) after
the first went down, −800 again (state 6); Ctrl + ↑ −1440 (state 2) --
under a gravity of 1900 to 2400 px/s² (`kJumpUpGravity`, since 2026.11.12c:
the bracket both readings of the live flights of 2026-11-12 fit -- g≈1905
from GoBot's own Ctrl+Up at face value, GoC's 2400 with a landing report
~0.3 s late; 2240..2640 until then): peaks of 133..169, 250..337 and
432..546 px (`jump_peak`), so a floor up to 121, 238 and 420 px above is
landed on for sure -- `kJumpUpClearPx` (12) under the lowest peak -- with
none in the band from there to 12 over the highest but platforms whose
standing points are inside the box, all of them (2026.11.12c: a landing on
any of them works, `JumpPlan::band_top`). The owner's *Theo GoC*
of 2026-11-05 -- ↑ ~133, ↑↑ ~280, Ctrl + ↑ ~432 -- are nominal peaks within
these ranges: upper bounds, the margins kept (answer 1). Higher floors are
reached only by one in between, at most `kZoneReturnJumpsMax` (2) jumps: on
a way back, and on the seek's way to a monster (`SeekRules::jumps_max`);
without the seek a monster gets one jump (`plan_jump_to`). A take-off is
judged at its
column and `kJumpUpMarginPx` (24) either way every 8 px (`judge_column`): no
floor that is not a platform in the way up (attr 2 and up stop a rise, as
GoC's `SimulateMove` has it; an unknown one too); no floor outside the box
within `kJumpUpCeilingPx` (650) above -- the ceiling guard, so a jump of the
owner's once-feared 600 px still lands inside (since 2026.11.12c for Ctrl + ↑
only: ↑ looks 245 px up and ↑↑ 413, each key's `beyond()` and
`kJumpUpHeadPx` -- a stray ↑ far higher than the model can now land on a
floor outside the box, which latched the jumps off until 2026.11.24 and since
has that take-off skipped a while); the same landing floor
at both ends of the peak's range, linked column to column (since
2026.11.12c, or band floors, all platforms inside the box: see its section);
the landing's standing point inside the box, and the take-off --
or, coming back in, no further out than the character stands
(`walk_stays_in`'s Closer); and the weaker jump -- Ctrl or the second ↑
lost -- landing back or in the box, unblocked. The arc may pass over the
box's top (answer 3). Runs of passing columns `kJumpUpRunMinPx` (64) wide
are walked to `kJumpUpRunInsetPx` (32) inside their ends, the cheapest by
key, walk and headroom (`plan_jump_to`, `jump_legs_from`). Ctrl + ↑ standing
on a gate does not change the map -- the owner's hand test, answer 2 -- so
no band is kept round a gate (`kJumpUpGateBand` false); the 10-s map-change
watch after any press stays.

The hunter walks to the take-off along its floor (`WalkFor::Floor`), lets
go within `Hunter::kJumpUpLeadPx` (24), waits for the packets to report it
standing, judges the column it stopped on -- two fine taps toward a passing
one at most -- and presses once, no arrow held (`src/hunter_jump.cpp`). In
the air it neither walks nor turns, but attacks and casts go out if the
monster comes in reach (answer 4; GoC's jump attack in place was not taken
until 2026.11.12c, whose bar ↑ is just that, for a monster above the upper
bar within one ↑).
Ctrl + ↑ is `high_jump` (above); ↑↑ is `InputManager::jump_twice`, the
second ↑ `kJumpUpSecondAtMs` after the first and sent only while nothing
holds, else judged as a plain ↑; ↑ alone is `jump_once`; every ↑ is held
`kJumpHoldMinMs`-`kJumpHoldMaxMs` (80-110) with the extended flag (E0 48).
`InputManager::jump`, `double_jump` and `BotConfig::key_jump` are never used
for it. What came of a press is judged from your own 0x002A by kind
(`PacketWorld::Stats::air`) and your last reports (`PacketWorld::own_since`):
worked -- on the planned floor, within 12 px -- and the fight goes on up
there; fell back onto the take-off's floor, which keeps that key and the
weaker ones off that floor within `GameMapBook::kNoJumpXPx` (48) of that x
for the session (since 2026.11.24 for `kLearnedForgetMs`, 30 s), and a second
fall onto the same floor takes it as solid from below (the same); landed
lower; did nothing in `kJumpUpJudgeMs` (900 ms); or the
key's kind not seen -- Ctrl + ↑ with no state 2, ↑↑ with no state 6; since
2026.11.12c read off the take-off's speed, and a kind missed only when it
landed elsewhere than the planned floor or its band --
`kJumpUpKindMisses` (2) of which turn that key off for the run. A landing
above the plan (since 2026.11.12c only past the key's `beyond()`: within it,
inside the box, it is `Moved`), outside the box or off the file (a step's
lip is its step's top since the 12c review), ↑ held over 250 ms, or a g
measured outside `kJumpUpGravityLo`..`kJumpUpGravityHi` (1300..3200 since
2026.11.12c; 2100..2800 before) twice turns the jumps up off until the next
start. Until 2026.11.15 `kJumpUpFailsInRow` (3) fails in a row or
`kJumpUpFailsPerRun` (5) in a run did the same; since, they pause the jumps up
for `kFailsCoolMs` (30 s, 60 s, 120 s) -- the run's only while more failed than
worked -- and a first press that did nothing passes its monster over 1.2 s
(`kRetryPassOverMs`). Since 2026.11.24 none of these switches the jumps up off
(the owner: *"khi hỏng thì thử lại luôn, hoặc dừng ra skill rồi nhảy xuống chứ
không phải là dừng 90s"*): not a fall back, a key not taken as its kind, a
landing above the plan or outside the box, ↑ held long, g out of range, nor
fails in a row or in a run -- a jump that did not get there is tried again at
the next look, its monster passed over `kRetryPassOverMs` for the first of a
row and one fight turn, `kRetryFightMs` (3 s), for every other; the take-off
it failed from (a fall back, a landing lower, higher or outside) is skipped
`kLearnedForgetMs` (30 s), the next try from another column, then forgotten
(`Hunter::forget_learned`, as is a floor taken as solid from below); a
landing off the file's floors has the hunter's `MapTrust` let the file go
until `kTrustPoints` (3) standing points on its floors take it again, no
clock. At most `kJumpUpsPerMinute` (4) presses
a minute, `kJumpUpsPerTarget` (2) per monster and 2 settles let go of per
monster -- since 2026.11.24 the minute's only the presses that moved the
character, the monster's those of the last `kTargetCountMs` (10 s) -- (the
bar's ↑s of 2026.11.12c not among them: `kBarUpsPerMinute` apart); 1.5 s from
any drop or jump press; none within 5 s of a map entry,
while a hold is on, the game is behind, loot is owed or the drop's veto
stands -- which now counts a GoBot hotkey on ↑ as well as on Ctrl or Down. A
map change within `kPressWatchMs` (10 s) of a press stopped the hunt, unticked
*Nhảy lên bục trên* and saved `hunt.ini` until 2026.11.24
(`DropStop::JumpMapChanged`; with a drop's press still under its watch too,
either may have done it, both undone, learned and unticked:
`DropStop::BothMapChanged`); since, both ends are learned as gates -- no drop
or jump near them again this session -- and the hunt goes on where the
character landed, nothing unticked or saved (`Hunter::PressGate`, said once in
amber under the tick). After a jump to a monster that worked, no
drop for `kJumpUpStayMs` (8 s, `DropWhy::AfterJump`), and no jump toward a
monster within 8 s of a drop that worked. A monster only a jump reaches is
picked last (`kJumpUpTargetsLast`). With the switch on in an area where
jumps up may be planned (`Hunter::jump_area`: the jump key ↑, the packets
steering, the map trusted), the old hop never presses there: every jump is
planned; anywhere else it hops as 2026.11.05 did.
The tick, greyed for a reason, may still be unticked. `return_point`
takes up to `kZoneReturnJumpsMax` (2) jump legs, fewer jumps first, and
then has no blind hop: no way back is none. Off (or, until 2026.11.24, off
for the run), nothing new is
pressed: `Hunter: a jump up would go here (jump_up is off): …` says where,
once a monster or way and every `kJumpUpSayMs` (30 s) at most, and the map
tab draws its arrow dashed. **Thử ↑**, **Thử ↑↑** and **Thử Ctrl+↑**
(*Đo cú nhảy*, under the row, shown where the switch can work as set) press
one jump each with the bot stopped and measure it from the packets --
take-off lag, flight, rise, g≈ (`JumpCheck::run_kind`) -- refused under a
floor the map shows within 650 px; nothing waits for them.

On t52_s2 with the owner's box (265..1399 × 174..694, home 880,274) the
planner finds Ctrl + ↑ alone, onto the home's ledge: from the ground at x
695..1002 (the run 663..1034; 695..884, 663..916 until 2026.11.12c) and from the
ramp's foot at 1024..1048 (992..1080). Columns 505..638 lie under ledges A and B, outside the box,
and the ceiling guard refuses them. So the home ledge is reached from the
ground -- from 880,634 the way back walks to about x 880 and presses Ctrl + ↑
(where it stands since 2026.11.12c; about x 802 until then) -- and the 04d advice to move the home down to about 1100,714 is no
longer needed; the box stays as drawn. A bottom edge at about 810, to hunt
the ramp's foot as well, adds Ctrl + ↑ from the ground onto the ramp at
1070..1343 (1161..1288 until 2026.11.12c), and Up,Up at 1048..1086. On t53_s2 no key has a usable jump in the owner's box, its top
at 1088 (at 1020, since 2026.11.12c, Up at 2391..2409).

The log: `Hunter: jump up (Ctrl+Up) toward …|on the way home: from x,y on
floor N of t52_s2 (trusted: …), onto floor N at x N (rise N; peak 432..546,
headroom N; no floor outside the area within 650 above), run a..b, …; Up
held N ms`; then `Hunter: jump up worked: landed at x,y on floor N, N px up,
N ms after the key (0x002A: high -36 at +N ms, landed +N; flight N ms,
g≈N)`, or `jump up FELL BACK` (with *bumped from below* or *came up short*
by its flight time), `landed LOWER`, `did nothing in N ms`, `Ctrl+Up came out
as …`, `… OFF until the next BẮT ĐẦU` (since 2026.11.24 never: `… that
take-off skipped 30 s, jumps up go on`), a WARN `MAP CHANGED` (since
2026.11.24 `… hunting goes on, Nhảy lên stays ticked`); `Hunter: not
jumping up to … -- <why>` once a monster for what the player can act on,
the rest at DEBUG, with each own 0x002A and 0x0031 for 3 s after a key. The
minute's `Hunter:` line gains `; jump-ups: looks N, would N, settles N,
pressed N (Up a, Up,Up b, Ctrl+Up c), worked N, fell back N, …; g≈N (n
flights); refused: …`, and the *Positions* line says `002A:N (jumps J
(walk-offs W), second S, high H, landed L, other O)` -- the walk-offs, state
1 falling, among the jumps and said apart (`air_walkoffs`; see
[protocol-findings.md](docs/protocol-findings.md)).

**Tự tìm quái trong vùng** (`seek_in_zone`; *Seek monsters anywhere in the
area*) answers what the owner said after the hunt of 2026-11-05 23:02 on
05 -- *đang chưa tự tìm quái ở trong ô đánh quái. Nên có thể tự tìm quái và
di chuyển tới đánh*. Off the ledge at 23:03, the character spent five
minutes on a strip of the ground, x 691..961; at 23:05:19 it walked back
from 709,579, inside the box where it had just killed seven, to 880,634
under the home; four times it walked right toward a monster and was stopped
at 1035,695, where the box's bottom cuts the ground; and it ended standing
22 s at 907,646 with nothing to pick. In 05 the hunter went for nothing it
had not picked from its view, walked back to the home's floor point
whenever nothing could be picked, even from inside the box, walked toward a
monster's x -- down the slope into the box's bottom cut -- never climbed
back to the ledge, and held the ramp's walkers, whose walks had run out
with no frame since, 1,500 px off at x 2594. Now, from the packets' list of
the whole map (`PacketEyes::world`), the box's floors the character gets to
are worked out (`seek_map`, `gobot/hunt_seek.hpp`): its own floor, cut where
a walk would leave the box; off a floor's end onto one inside it, every
speed landing inside (`zone_falls`); and the jumps up when they may go. For
each monster inside the box the cheapest standing point on those floors
that has it in reach is its attack point (`seek_attack`):
`kSeekReachMarginPx` (24) inside the reach and `kSeekBandMarginPx` (8)
inside the height band, from 24 px toward it too (the turn's step), and
`kSeekEdgeInsetPx` (48) at least inside where a floor leaves the box, so a
walk there is never refused (`walk_kept_in`). The hunter goes there
(`src/hunter_seek.cpp`), not toward the monster's x: from 945,666 to the
ramp foot's monster at 1007,578 it walks left, uphill, and fights from about
867,627. Picks go (Q2 (a), `kSeekTiered`): one in reach; then one a walk on
the character's floor gets to; then a drop's target below, when drops go;
then one below by a fall; then one only a jump up gets to -- cost for cost
within each, a fall `kSeekFallCostPx` (200) and a jump `kSeekJumpCostPx`
(600) more than their walk. The goal is kept: changed at once for one in
reach or in a better tier, else only for one `kSeekSwitchPx` (200) cheaper
after `kSeekCommitMs` (2 s). One that gets no `kSeekProgressPx` (40) nearer
in `kSeekStuckMs` (4 s) of walking, or whose plate is not in reach
`kSeekArriveMs` (1.5 s) after it is reached (*not found*), is passed over
for `kSeekPassOverMs` (20 s); a hold (trash, charms, chat, revive, the
shop) or the game behind counts toward neither, and both clocks start
afresh after a fight. A monster whose jumps up are used up
(`kJumpUpsPerTarget` presses or 2 settles for it) is planned without them
(`SeekNo::JumpsUsed`), and a drop's target below whose Ctrl + Down is
refused from right above it for good -- a gate near, no floor below, past
the monster -- is gone to by its own way for 20 s (`Hunter: slot N -- no
drop to it from above (…); its own way instead`). With no floors from the
map tab to place the monsters on, `PacketEyes::world` says false and the
seek stands aside. The floors are worked out again every
`kSeekPlanEveryMs` (250 ms), so a respawn anywhere in the box is gone after
at once. No new key: walks are `WalkFor::Floor`, falls are walk-offs, and
jumps go through the jump up's own executor and caps (`JumpUpFor::Seek`). A
monster with no way to it is marked (`Listed::NoWay`), greyed on the map
tab with its reason on hover, and said once; with *Nhảy lên bục trên* off,
one only a jump gets to stays grey and the card says to tick it.

The owner's seek questions are not answered yet, so each is its (a), one
constant each: with nothing inside the box to get to, the character stands
where it is -- no walk back to the home; outside the box it walks back as
before, jumps up included (Q1, `kSeekIdleHome` false); the order above (Q2,
`kSeekTiered`); the hunter's plates of the packets' monsters stand where
`mob_place` puts them -- the map tab's dot -- while the area is in use with
the seek on (Q3, `kPlatesOnFloors`: this reverses 2026.11.05's Q2 (a) for
the hunter, on purpose -- attack points are planned on floors, and a plate
at a walk's stale height would read out of reach on arrival); a walk run
out (`PacketWorld::kMobWalkMs`, 5 s) is followed on along its frame's floor
at its speed and held at the floor's end, `kPlaceWalkOnMaxMs` (25 s) since
its frame at most (Q4, `kPlaceProjectWalks`, `DotWhy::Projected`; the tab's
own dots never project); a monster just outside the box is fought only when
in reach already, never gone to (Q5, `kSeekOutsideHit` false); one another
player hurt within `kSeekOthersHurtMs` (10 s) is not gone to, though fought
in reach (Q6); and the switch is on as shipped (Q7). Off, the hunter is
2026.11.05's with its jumps up.

Its log: `Hunter: seeking slot N gen G at x,y (floor N, in view|out of
view) -- attack from x,y: <the way> (<tier>; cost N); in the area N:
reachable N, above with no way up N, passed over N` (INFO at most every 2
s), `Hunter: not going for slot … -- <why>` once a monster, `Hunter: slot N
not found at …; passed over for 20 s`, `Hunter: nothing to seek for N s at
x,y … -- standing where it is` as it begins and every `kSeekSaidMs` (10 s),
`Hunter: monsters at x,y …` every `kSeekSnapshotMs` (30 s) -- every monster
inside the box where it is placed and what is made of it, those within 200
px outside, the rest counted -- and the minute's `; seek: goals N (your floor
a, below b, above c), reached N, switched N, not found N, stuck N, no way to
N; nothing to seek N s; time: attacking …, approaching …, seeking …, nothing
to seek …, walking back …` -- where the minute went -- flushed at a stop
too (`Hunter (last N s, hunt stopped): …`). The *Positions* line adds `the
hunter's plates: N of M living monsters in view (±W x ±H); placed on floors:
N walking (P projected past 5 s), S set down`. On **Bản đồ**: a dashed line
from you to the attack point (a small square) and on to the sought monster,
a dashed ring where its place is projected; the jump's arrow from take-off
to landing, labelled ↑, ↑↑ or Ctrl+↑, dashed for one that would have gone
(`jump_arrow`); learned no-jump points and floors found solid from below in
grey (`solid_marks`); and the seek's greyed monsters (`seek_grey_of`).

The tab's files are its worker's (`MapViewJob`, `gobot/map_view.hpp`): the
look and the naming, the page read and sketched once per map -- floors
linked column to column, one attribute a run, simplified to 1 px -- the
gates and the no-drop marks, `tb_Map`'s name, and `spots.ini`; the GUI thread
reads no file. A capture started again, or more than 2 s between jobs, makes
its `MapTrust` let go first (an entry's map comes back at the next look, a
named one is named afresh); once the tab is made, the job runs every 1.5 s
unseen, and every 0.4 s -- at once after a map entry -- while the tab is
shown or a hunt uses the spot. After a log in (the keeper's, or **Tìm
lại**) nothing known before it is gone by: the map is known again from the
next entry, or named afresh. While the tab is shown the window is drawn
`kMapTabFps` (30) times a second behind the game and in front alike -- 10
from 2026.11.05, the owner's Q3 (a), and 5 before -- and `kMapTabLightFps`
(2) behind the game in light mode, rather than 2 and 1.

The version is in the window's title and at the bottom left of the simple
screen: the one `package.ps1` wrote into `VERSION.txt`, or *bản tự build* for
a build of your own. Whatever has passed every test here but never run on the
real game is marked *Thử nghiệm*: on the first line of a tab that is new as a
whole, at the start of a row's hint for a new switch.

**Tổ đội** answers party invitations and invites whoever chats the keyword,
with an accept-only list, an invite list and a blacklist. Names on the
accept-only and invite lists are matched tightly -- case, accents and spaces
aside, a name of 4 letters or fewer exactly, a longer one with at most one
letter wrong, missing or extra -- and the blacklist loosely, at 70%.
**Tự đăng nhập** logs back in when the client crashes or loses its
connection, to the channel and character you pick, and with the game closed
BẮT ĐẦU opens it first. At most `max_restarts` restarts in any hour
(`[Crash]` in configs/app.ini, 5 as shipped), a restart being every time the
game had to be closed and entered again -- after a crash, a hang, the
watchdog, or a crossing that ended a game stuck part way in, the login key's
included; one more waits until the oldest is an hour old (until 2026.11.24 it
stopped for a person after that many in a run). It needs no account and no
password: VTCZone stays signed in (sign in to it once, with its remember
option) and GoBot only presses *Chơi game*; if VTCZone shows its sign-in form
instead, it says so and looks again every 30 s, never typing into it (it
stopped there until 2026.11.24). **Dọn rác** opens the bag every
few minutes and puts what is on your list in the trash can; you pick the junk
once from the game's item icons (or a photo of your bag for anything else),
and *Thử tìm* shows what would go without throwing anything away. With no tab
ticked under *Tìm trong:* no pass is made at all -- the bag is not opened, and
the tab says why -- and when everything on the list is an icon of the game's
*spend* group (potions, food, fragments, charms, which lie in Tiêu Dùng and
nowhere else) only Tiêu Dùng is gone through (both since 2026.11.10). While a
pass runs the hunter stands; since 2026.11.10 the macro's keys on a letter or
Space -- skills, buffs and potions alike -- go on between its own clicks, on
a look with nothing up and nothing out towards the bin, and a slot is dragged
only from a look taken after they are up and half a second after the last
potion (*the hold's key window*, below); digit keys wait for the pass. Since
2026.11.11 the keys on Home, End, PgUp, PgDn, Insert and Delete go on too, an
HP or MP potion due is given a moment between two items, and with the HP
potion on such a key a timed pass starts at its mark and stops only under
half of it (*The shop after the pass, HP during the bag jobs, and six more
keys (2026.11.11)*, below). Items are
matched by their drawing, whatever their colour -- the fragments in the bag
change colour all the time -- so picking an icon also picks the same drawing
in other colours, and the tab shows which -- except a charm on the *Dùng*
list, of which only the colour picked is used, each colour being another
effect. Only the bin's own dialogs are
answered, told apart by their words (the fragment and charm questions below are
the exceptions), and an item that looks as much like an
icon you did not pick, drawn differently, is left where it is. Two more things
the tab can do with items on its *Dùng* list, both off until ticked: *Dùng mảnh
ghép* registers fragments in Yêu Quái Bách Khoa, and *Dùng bùa* uses charms
(Bùa) by a right-click in Tiêu Dùng. A charm costs one and lasts an hour, so
each kind ticked -- each colour -- is used once and not again until *dùng lại
sau* has passed (kept in a file, so a restart does not use another); all the
kinds due are used in one quick run of their own, about 3 s a kind and 20 to
40 s at most, in which skills, potions and buffs wait. Xác Nhận is pressed only
on a box with the fragment question's frame and buttons (a score of 0.92 or
more, set in trash.ini) whose words are not the bin's, the revive or the sell
question -- the question is told by its frame, the drop question by its frame
alone. The game's message after it ("Bạn đã nhận được hiệu ứng ...") is
closed with its own Xác Nhận, never Enter or Esc -- one that two clicks do
not close is clicked again by the next charm run, a minute on, for as long as
it takes (since 2026.11.24; left to the player before) -- and the stack must
be seen one less before the next kind. A stack of one has no number on it: it is
used only when its count corner is that colour's own icon with nothing over
it, and must then be seen gone. *Dùng bùa ngay* uses them at once. Beside them, *Tự mua bình* (also off until ticked) buys
Thịt bò khô and Rượu lá tre from the shop the Người Giữ Sân item of the bag's
Khác tab opens (found by the game's own icon for it, nothing to photograph).
Since 2026.11.09 it counts nothing: every *mua mỗi* minutes (60 as shipped)
it buys the number of slots set on the card for each potion -- a purchase of
100 a slot, 0 to 999 each (no cap of GoBot's own since 2026.11.10; 20
before), 3 and 3 as shipped -- HP and MP in turn, and the
game's message that the bag is full (*Túi đồ [Tiêu thụ] đã đầy*) ends that
time's round, the box staying ticked (*Buying by the slot, and the check-in
board (2026.11.09)*, below). Each purchase double-clicks the
potion in the shop, types 00 after the game's 1 in the quantity box, reads it
back as 100, and only then presses Xác Nhận; anything unexpected is answered
Hủy Bỏ. Skills, potions and buffs on a digit wait while it buys; since
2026.11.10 those on a letter or Space go on between purchases, never from the
double-click until the quantity box is seen gone and the bag-full message has
had its second -- since 2026.11.11 those on Home, End, PgUp, PgDn, Insert and
Delete too, an HP or MP potion due given a moment before each double-click.
Since 2026.11.11, with *Tự dọn rác* on, a timed round comes right after a
bag pass, under the same hold. *Thử mua* goes as far as the quantity box without buying. It opens the bag only after a second
with nobody at the mouse or keyboard (a round that rides a pass, since
2026.11.11, has no such second: the pass has just clicked through the bag);
otherwise it logs `Shop: not now --
someone is using the mouse or keyboard (held N ms): …`, with what spoiled the
second, and, on the clock, tries again later -- since 2026.11.08 after 30 s,
then 2, 5 and 10 minutes, one spoiled look after another (*Skills on the
battlefield (2026.11.08)*, below; until then 30 s every time) -- while **Mua
ngay** or **Thử mua** says *Chờ: bạn đang dùng chuột hoặc bàn phím…* on the
tab and does nothing. No charm is counted or right-clicked unless the bag's
tab strip reads Tiêu Dùng chosen, nor the Người Giữ Sân item unless it reads
Khác chosen (since 2026.11.09 the shop reads no other tab) -- the chosen tab
drawn darker than the rest, and far darker in
the band of 4 rows under the labels, which since 2026.11.04d is read first:
the game's red glow round a tab lights the rest of it (on 2026-11-04 no tab
read chosen, and the shop stopped) but never that band. The whole tab is
read where the band says nothing, and the two at odds read as no tab. After
a click on a tab the pointer is moved off the strip (a
tab under it lights up), a strip that shows no tab chosen is looked at again
up to 8 times, 150 ms apart, and one that shows another tab for sure is
clicked once more (the shop's Khác up to four times in all, since
2026.11.14b). Still not the tab wanted, the shop and the charms stop (a bag
pass goes on, as it always has), with a `Trash: tab not read as …` line
(each tab's mean grey, and the band's: `under the labels …`) and
one picture per place each session in `trash_debug` (`tab_unsure_…`; the
shop's is `tab_unsure_shop_other`); a
charm run stopped so says so, not "no charm found", and a timed one waits
a minute (`kCharmTabRetryMs`; 10 minutes from 2026.11.08 to 2026.11.24). *Tốc độ bấm nút* (60, 80, 100 or 150 ms)
sets how fast these bag jobs click, drag and type; what they wait for and read
back does not change with it. (*Tốc độ bấm kỹ năng* on the skills row does the
same for skill keys: 50 ms as GoC, or 60, 80, 100, 150 ms between one skill's
key coming up and the next one's going down.) When several jobs fall due at once they run in
turn: a Tự chat line, then the bag pass, then the charms, then the potions --
but since 2026.11.11 a potion round that rides the pass comes right after it,
before the charms.
These jobs, and Tự chat, read the game off the screen, so the game's window has
to lie wholly on the screen. BẮT ĐẦU starts whatever is ticked on any tab.

Since 2026.11.24 the *Tự mua bình* card also buys the **pet's HP food**: with
**Mua HP pet (Thịt bò) khi ra tới TổCổLong** ticked (`buy_pet` in
`trash.ini`, off as shipped) and the bot running, each time the character
comes into TổCổLong (`t60_s19`, Cổ Long's lobby, where a run of the dungeon
leaves you) GoBot opens the same shop, clicks its **Linh Thú** tab -- by its
place from the shop's Mua | Thoát, measured on the live client's own pictures
(the tab's middle 33 px right of the buttons and 353 above them; five tabs of
64 px), the strip of tabs seen where that puts it before the click and the tab
seen chosen, the potions' row gone, after it -- finds **Thịtbò** in the list by
its name (MenuText, the name with no spaces as the list draws it; a find that
is only the start of Dược Phẩm's Thịtbòkhô, or the end of a longer name, has
letters beside it in its cell and is passed over) and buys `buy_pet_slots` of
it (1 as shipped, 0 to 999) the way it buys the potions: a double-click on its
icon, 100 typed and read back, Xác Nhận. One round per arrival
(`PetRestock`), whatever else is ticked -- with no timed bag job a worker is
started for it, as *Mua ngay* starts one -- in the jobs' order after a Tự chat
line, a pass and the charms due, before a timed potion round. The game's
message that the pet's bag is full (*Túi đồ [Linh Thú] đã đầy*, a message GoBot
has no picture of) is closed and ends the round; a round that went wrong is
tried again 30 s on while the character is still there, within 90 s of the
arrival, else at the next arrival. The Phó bản tab's Cổ Long steps in the lobby wait for it -- *Chờ mua
HP pet* -- 90 s at the most from the arrival, then go on without it; nothing is
stopped for it. A failure keeps its picture in `trash_debug` (`pet_tab`,
`pet_no_item`, `pet_item_place`) and says why on the card; the log's lines
start `Pet:`. No picture of the Linh Thú tab's list exists yet: the first live
round's log and pictures are what to read.

**Tự chat** says up to ten lines of your own in the game's chat, one every
so often (30 seconds at the least), in turn or at random, into whatever
channel the button beside the game's chat box is set to. It types the way
GoC does, a key per letter, so the marks come off Vietnamese and only
letters, digits and spaces go in. Each key is pressed only once a look at
the screen shows the chat line open, Enter only on a fresh look that shows
no dialog, and the fighting, potions and buffs wait for the few seconds a
line takes -- on a slow game, since 2026.11.15d, up to 30 s for each step
the game is slow to show (HP under the potion's mark still ends the line at
once) -- and, when GoBot cannot see the chat line shut afterwards, for
as long as that lasts, as every key would go into it. Nothing of GoBot's
uses the clipboard.

**Nhiệm vụ** is quick quest, done as GoC does it for this client: every few
minutes of a run, GoBot opens the Nhiệm Vụ window with its key (U by default),
goes to the NV Phụ tab and ticks the game's own **Tự Động** there if it is
off, then closes the window again -- the fighting, potions and buffs wait
those few seconds (on a slow game, since 2026.11.15d, up to 30 s for each
step the game is slow to show; `round_limit_ms` counts only the round's own
time), and the character down ends a round at once (since
2026.11.12b a round starts and goes on at any HP; before, it waited for HP 10
over the potion's mark and HP falling under the mark ended it). A timed round
waits while the game is not the window in front; only
*Làm ngay 1 lần* brings it forward, and only in the few seconds after the
click -- one that had to wait longer waits for the game to be in front, as a
timed round does, rather than pull it over a window you have gone to since.
(*Luôn đưa game lên trước*, on by default, brings it back anyway a moment
after your last click or key while the bot runs; untick it to keep the game
where you left it.) It reads the box before clicking and clicks only a box
it is sure is clear -- GoC's picture of the clear box matching as a whole
and on its inside alone, where a tick of a few pixels shows -- so an option
already on, or one it cannot be sure of, is left alone. GoC's picture of the
ticked box has never matched the live client, so after its own click GoBot
also takes a plain change for ticked: the window, Tự Động and the box's
frame where they were, the clear inside gone, and the same on two looks with
the pointer back on NV Phụ. That look is learned into
`templates\grabbed\quest_auto_on.png` (the frame kept in `quest_debug` as
`*_autotick_on.png`) and read as ticked from then on. It never clicks Hủy
Nhiệm Vụ, the quest's accept, give-up or complete buttons, the
low-level-quests checkbox or another tab, and types nothing; GoBot itself
never accepts, hands in or picks a quest. (Before 2026.11.03d the area kept
clear of those buttons, GoC's own, reached into Tự Động's box on the VTC
client and every round refused to click; a `quest.ini` still saying
`forbid_buttons=283,412,103,41` is read as the new `283,412,90,41`.)
*Thử xem* looks without pressing anything: open Nhiệm Vụ on
NV Phụ by hand first, and it also says whether a round would click where it
needs to. Three failed rounds in a row are told on the tab and the phone,
with the frames kept in `quest_debug` beside GoBot -- since 2026.11.24 no
longer a stop: the next round comes at its interval, as after any failed
round. The English client, whose quests GoC picks by picture, is not
supported.

**Thông báo** tells you when something needs a person: the character died
(or was revived, or could not be -- since 2026.11.24 GoBot goes on trying),
the game went and GoBot is logging back in (or, until 2026.11.24, gave up;
since, it waits out the hour's restarts or tries again 30 s on), the
character has been out of the world for minutes, a stop
left the trash bin's dialog or the chat line open, Tự chat or quick quest
failed several times in a row (until 2026.11.24, stopped itself; since, it
goes on trying). Since 2026.11.10 an HP of 0 read off the screen is no death
while the game is not in front, while the packets say HP is above 0, or
while the server has been silent on the game's capture past `kLinkQuietMs`
and up to `kDeathQuietMostMs` (60 s) -- a game reloading (`death_doubt`).
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
hunter's finer settings, the HP/MP reader and the capture by hand — is under
**Nâng cao ►** at the bottom, and **◄ Giao diện đơn giản** at the top left of
that window comes back. Since 2026.11.16 that window has two tabs, *Features*
(Attack, Healing, Timed keys, HP / MP reader, Auto party, Keep the client
alive, Watch the session) and *Log*, and with *View > Advanced panels* a
third, *Tools*, holding *HP / MP* and *Capture*.

How this compares with GoC, feature by feature: `docs/goc-comparison.md`.

### Packets first (2026.11.03)

Since 2026.11.03 GoBot takes from the game's own packets whatever they can
give, and keeps the screen for the rest. Nothing is sent or changed, and no
key is pressed for it -- Down least of all. None of it has run on the real game
yet. A use that rests on a layout not proven on today's server acts only once
a check at runtime has compared it with the screen's reading of the same
thing, and the screen takes over again at once when they disagree -- but
not your HP and MP since 2026.11.17, which go by the packets whenever they
have them, unchecked, by the owner's choice (below); every
switch, either way, is written to `gobot_gui.log`. With Npcap missing, no game
found or a capture refused, everything comes from the screen as before.

- **The capture runs all session.** It starts when GoBot opens, hunting or
  not, and is kept across F10, F11, F12, DỪNG LẠI and HUNT ON/OFF. A new game
  process or connection -- a crash, the keeper's log back in -- is followed
  within a second or two; a new UDP socket, or 5 s of silence, restarts it at
  most every 10 s, and silence only once per quiet spell. Finding the game,
  reading its sockets, checking Npcap and every capture start and stop run on
  a thread of their own (`capture-jobs`), never on GoBot's, so F10, F11 and
  F12 answer as before. The same game process with no new connection keeps
  the map it knew ("the world kept"); a new process or connection starts
  afresh. A
  `Packets:` line each minute something changed says what the link carries.
- **Monsters** (with *Vị trí từ gói tin* ticked): each blow -- `0x0038`
  states 6 and 7 -- sets the monster's HP from the damage and the HP left. A
  wounded monster is finished first only once your id has been seen as the
  hitter 20 times (automatic, and logged), and not while another player is
  hitting it. The target is held by its slot and spawn generation, so two
  monsters crossing do not swap it, and one killed and spawned again in the
  same slot is a new monster.
  Since 2026.11.05 (not yet run on the game) a frame more than
  `PacketWorld::kUnknownPlacePx` (200) from where its living monster was
  worked out to be -- the gate a hurt and a state not known were held to --
  still places it, standing, when it proves itself: a hurt whose HP chains
  (the HP before less the damage is the HP after), or a contact (states 3
  and 11) on a monster known (`PacketWorld::kPlaceFarFrames`, the one rule
  `places_far`; false turns it back). About once a minute of the hunt of
  2026-11-05 a fought monster was worked out 1,100-1,900 px from its own
  frames, and refused they left it there until its next stop or walk: a dot
  where nothing was and none on the monster fighting you, for the hunter as
  for the map tab. This is the one change of 2026.11.05 the hunter sees. A
  hurt that does not chain, and any other state far off, are refused as
  before. What sent the model there is not known yet, so each far frame,
  placed or refused, is said -- `Positions: a far monster frame placed --
  state …, slot … gen … at x,y, worked out at x,y …; its last placing frame:
  …`, at most once every 5 s (`kFarSaidEveryMs`), the ones not said counted
  in the next -- and the Positions line adds `far frames placed N refused N,
  walks faster than 4 px/step N`: walks followed faster than a monster
  walks (`kPlausibleWalkSpeed`), as the 7 a blow throws one at, sent as a
  walk, would be followed 1,400 px in 5 s. That line counts from HUNT ON, as
  the rest of it does; the far line's `so far` counts run from the packet
  world's last fresh start (GoBot's start, a capture started by hand, or the
  positions capture started on a new game process or connection; one started
  again on the same game keeps them), so after an earlier hunt, or far frames
  before HUNT ON, the far line's numbers are the larger. The far line is
  worded under the world's lock and written once it is let go of
  (`PacketWorld::on_packets`), so a slow log file holds up neither the map
  tab nor the hunter.
- **Your HP and MP** (wrapped `0x0051` and `0x0050`; since 2026.11.17 also
  `0x02F0`'s MP in a fight) go by the packets whenever the packets have
  them, since 2026.11.17 with no check against the bars on the screen first
  -- the owner's *"chỉ dùng gói tin"* (*MP bar and skills while hunting
  (2026.11.17)*, below). A bar's packet value is usable with `[Packets]
  vitals` not `off`, GoBot's own capture on the game window's process, a value
  of the current epoch, a server frame within 5 s and a maximum known; with
  none usable -- no capture, a gap, nothing yet after a start or a map load,
  the server quiet -- the bars on the screen decide, and the first usable
  value goes back to the packets. The bars are still read all the time:
  whether the character is in the world, a death and the pet's HP are
  theirs, and the minute's `Vitals:` line sets their figures beside the
  packets', for the log only. Until 2026.11.16b they went through a check
  per bar against the bars on the screen, as follows; none of it decides
  anything now. A bar was trusted after 20 agreements
  over at least 3 values with no disagreement among the last 20, HP also
  needing one under 98%. One disagreement put it in doubt, and the screen's
  reading went to the macro at once; 5 agreements in a row cleared the doubt;
  two disagreements in a row, or 3 in 20, distrusted it, and the third
  distrust switched it off for the session. Trust was earned by the printed
  figures exactly, judged 0.5 s after the read; since 2026.11.04 it was kept
  allowing for the HUD running ahead of `0x0051` (a hit or a cast's cost shows
  on the bar 0.4-0.8 s before the packet). While a bar was trusted or in
  doubt, figures within 5% of the maximum (HP) or 12% (MP) of the packet value
  counted for nothing either way; a read whose figures did not agree waited
  `kLeadMs` (0.9 s) for `0x0051` to reach them exactly, and then agreed
  (*caught up*); and reads of one HUD moment against one packet value made
  one disagreement however many there were (*same moment*). The minute's
  `Vitals check:` line counted `caught up` and `same moment`; since
  2026.11.17 it is the `Vitals:` line, which only counts. A read that missed the
  figures is judged by the bar's length only under 54% (it was 60): the
  caption printed right-aligned over the bar paints out the fill under it,
  and an MP of 10000 or more ("12643/15280") begins 56.7% along. The same
  shadow no longer misleads the potions: a fill measured at 54% or more with
  the figures missed takes the last figures read, kept about 2 s, when they
  say more -- and since 2026.11.17 MP's reads as nothing once they are
  forgotten, and figures short of the bar's own fill are refused (the EXP
  bar's glowing tip over them). While a bar is usable -- of the
  current capture (GoBot's own, on the game window's process: never a
  capture started by hand under *Tools > Capture*, which takes every
  client's traffic), a
  server frame within 5 s, a maximum known -- the potions, and with the
  bars' last read seeing the HUD the jobs' HP guards (charms, shop, trash,
  Tự chat, quick quest, closing popups, bringing the game to the front) and
  the skills' MP floor go by the packets, and while anything runs the bars
  are read once a second instead of
  at the run's pace -- since 2026.11.16 every 500 ms (`kRestPetMs`) with a
  pet potion key while the macro runs, as the pet's HP is only on the
  screen (before, not at all) -- but not in a critical hold, during *Tự
  tìm*, while HP reads 0, or within 60 s of a death panel.
- **A map loading** adds a hold: within 3 s of a map entry (`0x0086`), until
  your character reports again, the macro waits as it does out of the world.
  The packets never decide "in the world" on their own; the HUD still does.
- **The bag** (wrapped `0x0068`). A potion key is learned from 3 presses each
  answered within 1.5 s by one fewer in exactly one slot. Once that has
  agreed with the hotbar twice, the key's "Low" (*Sắp hết bình*) goes by the
  item's total in the bag, against its own mark -- `[Potions] warn_bag_below`
  in `app.ini`, 200 as shipped (about ten minutes at a farm's 19 potions a
  minute; 0 for none), not the slot's `warn_below` -- and is said once until
  the bag holds 5 more than that again; "Out" (*Hết bình*) is always the
  hotbar's. The slot's own "Low" names its number as the stack in use, *ô
  phím H (chồng đang dùng) còn 5 bình*, and once the bag has learned what the
  key drinks, before it is trusted, adds the bag's total, *cả túi còn N bình
  (theo gói tin, chưa kiểm)*; the minute's `Bag (packets)` line gives each
  learned key's item with its total in the bag (`key H -> slot 33 (8810121,
  260 in the bag)`), all since 2026.11.04d. Until 2026.11.09, once two
  shop runs' own counts agreed with the bag, a due *Tự mua bình* look could be
  skipped while the bag held enough; the shop counts nothing now, and the
  bag only writes its two potions and the Tiêu Dùng slots used to the log
  either side of a real round, and the slots used when the game says the bag
  is full (`Bag (packets): before the shop …`, `… after the shop …`, `… the
  game said full with N Tiêu Dùng slots used`): it gates and ends nothing
  there, and buying stays on the screen. The bag can also
  stop a charm's right-click when it names another item in that slot, with
  the screen's count, or -- for a stack of one, which has no number on the
  screen -- another count or none -- never cause one.
- **The name-plate reader rests** (with *Vị trí từ gói tin* ticked): every
  2000 ms (600 ms before 2026.11.16) instead of 140 while the packets steer a
  healthy hunt, and 140 again within a tick for **Thử**, *Nhìn quanh*, or the
  screen taking over -- the hunter then keeps the packets' view until the
  reader has a picture taken after the switch, so no plate older than the
  switch is served, 300 ms at most.
- **Map changes** go by the packets' map entries once one has been seen this
  session (a door, a log in). The minimap's fingerprint, which changed six
  times in a minute and a half of one hunt with no door taken, was then only
  logged, and since 2026.11.16 it is not looked at at all.
- **Thử** beside the jump key judges by your client's take-offs (`0x002A`)
  whenever the packets can tell -- GoBot's own capture on the game, your id
  known, no other character reporting -- and says *(theo gói tin)*; otherwise
  by the plate, as before.
- **Log only:** casts and buffs per macro key (`Casts (packets)`,
  `Buffs (packets)`), the bag after a charm's use and beside the trash list,
  and the link beside the keeper's stalls and log-ins (`Keeper:`). Since
  2026.11.04d the hunter's own attack-key presses are paired too (the macro
  skips that key, and its casts went to the macro's keys), and each burst of
  a buff key's presses is judged on its own: `Buff (packets): key S -- …`,
  a kind-2 cast of yours in its window or *maybe not cast*, and `, a kind-2
  cast with N of its M bursts` on the minute's `Casts` line. Nothing acts on
  it. Since 2026.11.08 a buff key is one a buff row *switched on* holds
  (`macro_buff_keycodes`), the minute adds `Cast gaps (packets)` and the
  casts no GoBot key made (below), and your own casts, as they come, pace the
  skills when *Ra chiêu theo vòng* is ticked (`MacroEngine::note_own_cast`):
  the one use of the casts that acts -- off as shipped then, on as shipped and
  with the hunter too since 2026.11.16b.
- **Still the screen's:** death and revive (with the reviver's HP and the
  recast after HP comes back from 0), the pet's HP, the shop's dialogs and
  buying, the captcha, and whether the character is in the world.

Unticking *Vị trí từ gói tin* (`watch`) puts the hunter back on the screen --
the monsters' HP, the slot lock and the reader's rest go with it -- but the
capture still runs, and the bag, **Thử** and map changes still use the
packets once checked, your HP and MP whenever they have them (unchecked since
2026.11.17). `packet_positions=off` in `hunt.ini`
(*Positions from packets: Off* in the full window) turns every packet use off:
no capture at all, everything from the screen.

`[Packets]` in `configs/app.ini`, read when `gobot_gui` starts. Each key is
`auto` or `off`, and anything but `off` counts as `auto`. `off` turns that use
off; its check still runs and still writes its lines.

| key | shipped | what |
|---|---|---|
| `vitals` | `auto` | your HP and MP from the packets whenever they have them, with no check first since 2026.11.17 (until then once each bar's check trusted them): the potions, the jobs' HP guards, the skills' MP floor, and the bars read once a second; the bars only when the packets have none |
| `bag` | `auto` | the bag's potion counts: "Low" by the bag's total; a charm's slot checked before its right-click; and, log only, the bag either side of a shop round (until 2026.11.09 a due shop look could be skipped) |
| `detector_rest` | `auto` | the name-plate reader every 2000 ms (600 before 2026.11.16) instead of 140 while the packets steer |

What to look for in a log sent after one session: `Positions: capturing …`
before any `HUNT ON`; `Packets: capture since …`, `Packets: first 0x…` and a
`frames:` line under `Load:`; `Positions: map entry … (from …)` at a door --
since 2026.11.04d with `; before it: …`, your last place on the map before by
the 1-s sample, GoBot's own last key or click (key names only; whatever was
typed is `typed text`) and the PC's last input, each against the entry, to
tell who took the character through; no
`Capture stopped` at F11; `hurts … (chained …)`, `kills … (yours …)`, `far
frames placed … refused …` and `your id` in the Positions line, and any
`Positions: a far monster frame …`; `wounded picked` in the Hunter line;
`Detector: every 2000 ms …` (600 before 2026.11.16); `Vitals: from the
packets HP …% MP …% of the minute …` and any `Vitals: HP from the screen until
the packets have it again -- …` (until 2026.11.16b `Vitals check: …` and
`Vitals: HP from the packets now`, or `… from the screen again` with both
numbers); `World: map loads by packets …`; `Bag (packets): before the shop
…` (since 2026.11.09; until then `Bag check: … agree (n of 2)`, later
`Shop: skipped -- …`); `Jump test: … (packets: N take-off …)`;
`Casts (packets)`, `Buffs (packets)` and `Bag (packets)`.

Since 2026.11.04 the log is quieter where the first long farm found it
noisy. The trash list read again every 30 s no longer writes
`TemplateStore: N templates` (the line at start-up stays); `0x0038` states 3
and 11, contacts that name an actor, are counted but no longer logged as a
state "which is not known"; `Cannot find "<name>" on screen` is a WARN only
while the packets do not steer the hunt, and a plain line, ending "the
packets steer, so the hunt does not need it", while they do. Quick quest now
logs why a round waits, `Quest: put off -- Chờ: …`, once each time the
reason changes.

Since 2026.11.04d a potion key's hotbar slot that reads as unreadable on
two looks running has a picture of that slot alone, 3 px round it, kept in
`potion_debug` beside GoBot, once a run per key and written off the GUI
thread (`Potions: key … unreadable on two looks running; kept a picture of
the slot alone as …`): on 2026-11-04 key G read nothing all session, so its
"Low" could never be said.

### Skills on the battlefield (2026.11.08)

Since 2026.11.08 (not yet run on the game). On 2026-11-08 the owner ran the
macro alone -- no hunter -- on the battlefield t1981_s2 (22:12-22:43), hands
off, and found the skills not smooth: pauses between them (*khựng giữa các
chiêu*) and an uneven order (*chiêu ra lộn xộn*); A and S, the owner says,
are attack skills, not buffs. The log showed:

- **The order was the timers'.** Each skill key keeps its own timer, and a
  10-ms pass presses the first one due and holds the rest `skill_gap_ms`
  past its key-up, so the intervals became the shares of the presses: of
  about 390 skill presses a minute Z (500 ms) took 26.6%, X, C, V, B and
  Space (1000 ms) 12-14% each, S and A (3000 ms) about 4% each. The game
  cast about 77 a minute and drops a key that goes down while the character
  is still locked in the last cast (650-760 ms) -- it queues nothing -- so
  whichever key came first after the lock was cast: an order by chance, Z
  about every 3 s and S and A every 17-26 s (model).
- **The pauses**, 22:12-22:20 only, were GoBot's own holds: 16, 26.9 s in
  all -- the shop's quiet second spoiled ten times, about 31 s apart, each
  holding every key about 1 s, and charm and quest runs of 1-4.9 s. From
  22:20:43, the shop, the bag and the charms unticked, there was none:
  GoBot pressed without a break, and any later pause was the game's
  (cooldowns, a cast rate wandering between 72 and 83 a minute), which that
  log cannot measure.
- *Dùng skill tuần tự* did nothing (see the hunting row, above). A and S
  were logged as buffs only (`(buff)`, and some 30 `Buff (packets) … maybe
  not cast` lines a minute): two switched-off buff rows still held their key
  codes. MP was read 4798 for the packets' 14798 from about 22:15 -- its
  leading 1 lost -- and F pressed about 40 times a minute: not fixed here
  (below).

With *Ra chiêu theo vòng* unticked, as shipped, the macro presses the same
keys at the same times as 2026.11.05b. What changes:

- **Ra chiêu theo vòng** (`cast_paced`, off as shipped in 2026.11.08; on, and
  with the hunter, since 2026.11.16b): the skills one at a time in list
  order, each once the game has cast the one before -- the owner's Q1 (a) --
  under *Keyboard macro*, below.
- **A spoiled quiet second waits longer each time** (`QuietBackOff`,
  `gobot/quiet_second.hpp`): the shop's timed look and the quick quest's
  timed round look again after 30 s, then 2, 5 and 10 minutes, one spoiled
  look after another (`QuietBackOff::kStepsMs`), never past the job's own
  interval (*kiểm tra mỗi*, *mua mỗi* since 2026.11.09; the quest's minutes)
  and never under the first step. Back to 30 s once a quiet second passes, on
  **Mua ngay**, **Thử
  mua** or *Làm ngay 1 lần*, with the box ticked on again, and for the
  quest at every BẮT ĐẦU; a button's own run never moves it. The owner's Q5
  (a) keeps the shop off in battlefield fights; W4, a check for a hand on
  the real mouse before the hold, waits for Q5 (b). Each spoiled second
  says what spoiled it (`quiet_spoiled_text`): the pointer, the five mouse
  buttons and the program in front, read -- never moved -- through
  `InputGlancer` on the jobs' own threads alone: `Shop: not now -- someone
  is using the mouse or keyboard (held 1012 ms): the pointer moved 35 px
  (812,440 -> 845,452); no mouse button held; in front: chrome.exe; the next
  look in 120 s`, or `Quest: the quiet second was not quiet -- …`, the
  quest's tab saying *Chờ: bạn đang dùng chuột hoặc bàn phím; thử lại sau 2
  phút.*
- **The charms' and the shop's clocks are kept across a MACRO OFF and ON**
  (`resumed_clock`, `GuiApp::TrashClocks`): every BẮT ĐẦU used to look for a
  charm due 8 s on and at the shop 45 s on, and the first minutes' toggles
  brought three charm holds of 2.4-4.9 s. The clock kept is used now, with
  the shop's back-off level, never sooner than those (`kCharmFirstMs`,
  `kShopFirstMs`); *Dùng bùa* or *Tự mua bình* ticked on again starts that
  job's clock afresh. A shop or charm run cut short by DỪNG or F10 leaves
  its clock as it was, so a purchase it cut is looked at again 45 s (8 s)
  after the next BẮT ĐẦU, not an interval later.
- **A key on a buff row and a skill row, both switched on** (A and S at
  22:13-22:15) is warned of under the skills row, the buffs row and the full
  screen's key table (`macro_clash_note_vi`), and once in the log as the
  macro comes on, from its own thread (`macro_clash_line`): `Macro: A, S are
  on a buff row and a skill row, both on -- pressed on both clocks, and
  logged as a buff (a warning only: nothing is changed)`. Nothing is
  refused: Q9 (a). Only while **Repeating keys** is on: off, no row is
  pressed at all, and nothing is said.
- **Words**: *Dùng skill tuần tự* and *Chỉ khi đang đánh quái* say they act
  only with **Tự đánh quái**, and *Như GoC* is gone; `docs/goc-reference.md`
  and `docs/goc-parity-plan.md` no longer say GoC waits for the server's
  confirmation: two static reads of GhostCore.dll agree that its confirm
  flags are written and never read, `MAX_SKILL_RETRIES` is never used and
  `AttackThread` is never started -- GoC presses blind too.
- **Log only**, to measure what the 2026-11-08 log could not:
  - after each minute's `Casts` line, `Cast gaps (packets): N gaps -- p50
    0.74 s, p90 …, longest … ending at …; over 1.0 s N, over 1.5 s N;
    newest press to cast p10 N ms, p50 N ms, p90 N ms (N casts), N with no
    press of GoBot's in the 700 ms before; by skill 51205 N casts, shortest
    re-cast N s / …` (`cast_gaps_line`, from the 256-cast ring): the gaps
    between your casts, and each skill's shortest re-cast, a floor of its
    cooldown with no pairing needed; the delay from GoBot's newest key-down
    within `CastMatch::kPairMs` (700 ms) before the cast. A gap across a
    stop and start of the macro or the hunter, or a capture started again,
    is no pause of a run's: left out and counted (`; N gaps across a stop and
    start or a capture restart, left out`, `CastMatch::set_run`);
  - a cast no GoBot key made: `Cast (packets) with no GoBot key at …: skill
    N, kind K; GoBot's last press N ms before it (key Z), the first after it
    N ms after (key X); no hold then` (or `the keys held for a job then`,
    handed to the capture thread through an atomic, `CastMatch::set_held`),
    at most `CastMatch::kLoneSaid` (5) a minute, the rest counted; a cast
    with no press of GoBot's within `CastMatch::kLoneNearMs` (10 s) either
    side and no hold -- yours by hand -- only counted: `Cast (packets) with
    no GoBot key: N with no GoBot press within 10 s either side and no hold
    (by hand or another tool), not described`;
  - `Simple screen: start asked by the toggle key F10` (or `the big
    button`, or the chat card's button; `stop asked by …` likewise);
    `Settings changed during the run: Keys.key_7_keycode 72 (H) -> 70
    (F)`, worked out in memory against the settings as the run began, or
    as last logged (`settings_changes`); a change made while stopped is
    said at the next start, `Settings changed since the last run:
    Keys.key_4_keycode 65 (A) -> 51 (3)`; and the minute's `Skills:` line
    ends `; refused: game not in front N`;
  - the `(buff)` tag and the `Buff (packets)` bursts only for keys that a
    buff row switched on holds (`macro_buff_keycodes`, `macro_buff_on`):
    about 720 of that run's 763 bursts are gone;
  - MP read short of the packets' value by its leading 1 (4798 for 14798,
    `figure_lost_leading_one`): the MP bar and its caption, cut from the
    reader's newest frame, written once a session off the GUI thread as
    `vitals_debug\mp_caption_YYYYMMDD_HHMMSS.png`, outside `vitals_debug`'s
    rotation (`Vitals: MP read 4798 where the packets say 14798 -- its
    leading 1 lost …`);
  - the minute's `Casts`, `Cast gaps`, lone-cast and buff lines, who
    toggled and what changed are written by a worker of their own
    (`GuiApp::log_later`), in the order they were said, never on the
    window's thread; the last minute's are waited for at the close.

Not in this release, each waiting for the owner: W2, "most overdue first"
on the timers (Q1 (c)); W3, the MP reader's lost digit and `BarCheck`'s
"reader short a digit", with the owner's MP mark and F's rate (Q7); W4, the
hardware-input check (Q5 (b)); W5, the owner's own intervals and pet mark
(Q1, Q8). Nor is the hunter's `run_skills` paced yet. No default interval,
mark or number of the owner's is changed. Meanwhile `DOC-TRUOC.txt` advises
one interval for the eight skill keys -- 1.2 s each, or for Q1 (b) Z 0.7 s
and the rest 1.4 s -- *Tốc độ bấm kỹ năng* left at 50 ms (at 150 the model
gives Z 40% of the casts and S and A next to none), the shop, the bag, the
charms and quick quest off in battlefield fights, and GoBot's window left
alone while the macro runs. The next live run -- 15 minutes or more on
t1981_s2, the shop, the bag and the charms off, first with the tick off and
the intervals equal, then with it on -- gives the `Cast gaps` to tune
`kPaceLockStartMs` and the re-press by.

### Buying by the slot, and the check-in board (2026.11.09)

Since 2026.11.09 (not yet run on the game). On 2026-11-09 the owner asked
for two things: *Tự mua bình* no longer to count the potions -- when the time
comes, buy the slots set, and stop when the game says the bag is full -- and
the daily check-in board, which the game shows at every login and after
00:00, closed with ESC. The owner's answers: Q1 (a), a full bag ends that
time's round and the box stays ticked, to buy again at the next time; Q2, the
numbers are the owner's own setting; Q3 (a), keys G and F drink Thịt bò khô
and Rượu lá tre, the two the shop buys; Q4 (c), the board folded into **Tự
đóng thông báo**, with no tick of its own. Not asked, and built as their (a):
Q5, the board closed only while the bot runs; Q6, the board told by its title
and closed by ESC alone.

- **Tự mua bình counts nothing.** No look at Tiêu Dùng, no first round of one
  of each, no stop after a purchase that added no slot, no recount, no
  packets' skip. When a plan is due it owes the card's numbers,
  `buy_hp_slots` and `buy_mp_slots` in `trash.ini`: 0 to 20 each in
  2026.11.09, and since 2026.11.10, the owner's "Không giới hạn", 0 to 999
  each (`CleanSettings::kBuySlotsPerRunMost`, a bound of the number alone:
  the bag-full message ends the round), 3 and 3 as shipped
  (`kBuySlotsDefault`). A file without them reads its old `buy_hp_to` and
  `buy_mp_to` -- the owner's 10 and 10 -- until the card saves the new key,
  20 at most (`kBuySlotsFromOldMost`), so that only a number set on the card
  buys more; `buy_*_below` is not read, but for 0 -- that potion left alone
  -- which reads 0. The purchases go HP, MP, HP, MP …
  (`bag::shop_order`): the full message covers the whole Tiêu thụ tab, so one
  potion bought first could leave no room for the other. A round on the
  clock makes at most `kShopBuysPerHold` (10) purchases in one hold, the rest
  `kShopPartGapMs` (60 s) later; **Mua ngay** starts the plan afresh and buys
  it in one hold, `kShopMaxBuys` (40) at most -- above that, since
  2026.11.10, the rest in parts as on the clock -- and the next plan comes
  an interval after it. A round's deadline is 30 s and 6 s a purchase,
  `kShopDeadlineMostMs` (300 s) at most, where it was 240 s. Since
  2026.11.15d it is moved on by whatever the game took past 2 s to show each
  input's work (`react_took`). On the card the
  interval is *mua mỗi* (it was *kiểm tra mỗi*), each potion's row *mỗi lần
  mua N ô*, and a line says when the next plan begins and what is left of
  this one.
- **The bag full.** *Túi đồ [Tiêu thụ] đã đầy* -- by GoC's strings it comes
  instead of the quantity box -- is looked for before the bag key, before each
  purchase, after the double-click, for a second after Xác Nhận
  (`kShopFullLateMs`) and before the shop's ESC. Its Xác Nhận is clicked -- its
  label where it is found, else the picture's fixed point -- and it counts as
  closed only when two looks 300 ms apart no longer see it; never an ESC
  while it is up. Then the shop is shut, the round ends and nothing more of
  that plan is bought, MP included; the next plan buys again (Q1 (a)). The
  purchase it came after is not counted. Two clicks that do not close it are
  the last, with a picture and the words; the keys are not held for a
  message alone, only for a shop still open behind it.
- **A message GoBot does not know** in the quantity box's place, seen alone
  on two looks in a row -- no question, no two-button dialog -- is closed by
  its Xác Nhận and ends the plan the same way, its picture kept as
  `shop_message`. A purchase whose end is unclear -- Xác Nhận clicked and the
  question never seen gone, a stop -- counts as bought and ends the round
  (`kShopUnsure`): never bought twice. **A message GoBot does not know after
  Xác Nhận** (2026.11.11c, the owner: "cần sửa để bot tự tắt thông báo
  này"), seen alone on the tail looks, is closed by its own Xác Nhận as a
  charm's message is (`shop_notice`, `close_notice`), its picture kept as
  `shop_notice`: the purchase counts as bought (`kShopNoticed`, in
  `shop_unsure`, never bought twice) and the round goes on -- the next
  double-click meets the full-bag message, or buys. The second notice in a
  round (`kShopNoticesMax`) ends it, closed too, with a trouble. A message
  that comes up later, and is still up when the shop is to be shut, is closed
  the same way before the ESC (`Result::shop_notices` counts both). A round
  in trouble waits `bag::kShopTroubleAgainMs` -- 30 s since 2026.11.15b, 10
  minutes before -- and so does every one after it, without limit: since
  2026.11.24 troubles in a row never drop the plan (until then the second in
  a row, `kShopTroublesToDrop`, dropped what was left of it until the next),
  and from the second in a row the card says *Mua bình bị lỗi N lần liền (…):
  GoBot thử lại sau 30 giây.* A message that two clicks on
  its Xác Nhận did not close (`shop_notice_stuck`, picture
  `shop_notice_stuck`), or that is not surely a lone message -- a question
  drawn with Hủy Bỏ pressed -- gets no ESC, which would go into it, and no
  third click: the shop is left open and the keys held for it.
- **The plan's time is kept on disk**, `%LOCALAPPDATA%\GoBot\shop_clock.txt`
  (`shop_clock_path`), written on the trash worker before anything is
  clicked; a time that cannot be written buys nothing. A GoBot started again
  waits what is left of it, `kShopFirstWaitS` (45 s) at least and one
  interval at most (`bag::shop_first_wait_s`). The box ticked on again owes
  nothing and waits for that clock, so a plan just bought is not bought again
  (**Mua ngay** is for that); a round cut by DỪNG keeps what it still owed for
  the next BẮT ĐẦU. Two GoBot folders on one Windows account share the file,
  as they share `charm_clock.txt`. A plan begins only once the round's gates
  let it run, and a **Mua ngay** that does not get to run (HP, the quiet
  second, the hold, a stop) leaves what is owed and the plan time as they
  were. A number lowered under what is left buys no more and waits for the
  plan's time, never beginning one early; a round cut by HP under the
  potion's mark or a lost hold buys the rest a part's gap (60 s) later --
  since 2026.11.11 a round its guard stopped for HP looks again 15 s on
  (`bag::kHpStoppedAgainMs`), and with the HP potion on a key the round lets
  through it starts at the potion's mark and is stopped only under half of
  it.
- **The check-in board** ("Sự kiện và điểm danh", 878x600, with its *Điểm
  danh* calendar) is told by its 280x30 title, which GoBot already shipped as
  `grabbed/auto_login_success_indicator` -- the client's own
  `Ui\Event\dialog_EventIntergration.spr` pixel for pixel -- under a second
  name, `checkin_board_title` (`gobot/checkin_board.hpp`,
  `kCheckinBoardScore` 0.90; a whole 1024x768 frame takes about 16 ms, and
  `Checkin: a title search on WxH took N ms` says it once a session). With
  **Tự đóng thông báo** ticked, while the bot runs and the game is in front,
  the popup closer (`PopupKind::CheckinBoard`) presses ESC once the board is
  due with nothing else of its list up -- the reward notice's Xác Nhận goes
  first. Due (`PopupCloser::board_due`, since 2026.11.15d; 3 s still before):
  `kCheckinBoardLoginWaitMs` (30 s) after the later of the character's
  arrival in the world (the keeper's crossing) and the board's first sight,
  having stayed put `kSteadyMs` (3 s), and never while the PC's clock reads
  23:55:00 to 00:03:00 (`checkin_night_hold`) -- the wait counts no try and
  sets no backoff. The ESC goes through the monster book's guarded ESC: the
  gates, a hold of at most 2 s, a fresh look, ESC by 1.2 s, and an ESC menu it brings up
  closed by its X. Under the hold the board is looked for again only as its
  title, within 8 px of where it was (a 296x46 search). Its X is never
  clicked: any close button in its top band is its own, so an X a little off
  the art's place does not keep it shut. Since 2026.11.24 an ESC that left it
  up is followed by another `kBackoffMs` (30 s) later, as often as it takes,
  the tab saying *Đã bấm ESC mà bảng Sự kiện và điểm danh vẫn còn: thử lại
  sau 30 giây.*; until then two tries in 10 minutes, then it was left alone
  and the tab said so. Unticked, nothing closes it; with the bot stopped
  nothing is pressed (Q5 (a)).
- **The jobs that click look for the board themselves**: the bag pass, the
  charms and the shop before the bag key -- the shop again before its
  right-click, each double-click and each Xác Nhận -- and the quick quest
  before it takes the keys. With it up the job presses nothing and looks
  again a minute later (`BagCleaner::kBoardWaitMs`, `QuestRun::kBoardMs`),
  not as trouble, and the tab says so; with the box unticked it adds *Bật "Tự
  đóng thông báo" (thẻ Đánh quái) để GoBot tự tắt bảng điểm danh.* A board
  that comes up in the shop stops the buying, and the shop is shut as ever:
  the live board hides the shop whole, so a shop seen open and now hidden
  under the board is not taken for shut -- its one ESC goes to the board, then
  Thoát. A board over the quantity box (`BagCleaner::board_over_question`) is
  never taken for the box gone: nothing more is pressed, ESC included, the
  round's end keeps the hold, and the trash worker's `look_for_clear` keeps
  it while the board's title is up, until the owner has closed the board, the
  box and the shop.
- **Pictures for the owner.** With the box ticked, `popup_debug` beside GoBot
  keeps `checkin_<time>_<nn>_<why>.png`, the newest 20: `first` (the board's
  first sighting of a day), `after` (once it is closed), `in15` (15 s after
  the keeper got back in), `start15` (15 s after BẮT ĐẦU) and `midnight` (the
  first look from 00:00:30) -- written on the popup worker between its turns,
  never under a hold. *Thử xem* names the board and keeps
  `probe_<time>_<nn>.png` from its own thread.

New in the log: `Trash: shop plan HP 10 + MP 10 (owed 10+10, at most 10 a
hold); clock saved, next plan 23:15`; `Trash: shop round of 10 purchase(s):
HP 5 + MP 5`; `Trash: shop purchase 1/10 HP -- bought (100 read back)`;
`Trash: shop full message at x,y (whole …, words …) …` and `… closed by its
label (seen gone twice, 1 click(s))`, or `… NOT closed after 2 click(s), no
ESC; the keys are not held for it`; `Trash: shop full at purchase 4/10 MP,
after 3 bought -- nothing more this plan`; `Trash: shop lone message at x,y …
-- kept shop_message, closing it; nothing more bought this plan`; `Trash:
shop purchase of … unsure (…) -- counted as bought, stopped`; `Trash: shop
done -- bought 5+5, unsure 0, full no; owed 5+5; next part 60 s on`;
`Trash: shop -- 2 trouble(s) in a row: owed 5+5 kept, tried again 30 s on`
(since 2026.11.24; until then `Trash: shop -- 2 troubles in a row: owed
dropped until the next plan …`);
`Trash: shop -- both slot numbers 0, nothing to buy; next plan …`; the
packets' `Bag (packets): before the shop …`, `… after the shop …` and `…
the game said full with N Tiêu Dùng slots used`; `Trash: the check-in board
is up -- the pass put off 60 s` (or the charms, the shop); `Quest: the
check-in board is up at x,y (title 0.97) -- quest put off 60 s`; `Popups:
the check-in board (Sự kiện và điểm danh) is up at x,y (title 0.97 at x,y;
its X at +564,+14 from the title)`; `Popups: closed the check-in board at x,y
(title 0.97) with ESC`; `Popups: kept …\popup_debug\checkin_…_first.png
(board seen)`. The owner's live test is step 12 of section 6 in
`DOC-TRUOC.txt`: *Tự đóng thông báo* ticked through a login and a 00:00;
*Thử mua*, then 1 + 1 with **Mua ngay**, watched, then the real numbers.

### The pet, the walk back, the bag and the shop (2026.11.10)

Since 2026.11.10 (not yet run on the game). On 2026-11-10 the owner wrote
*"Bơm máu pet chậm"*, *"Thao tác dọn rác chậm"*, *"Không tự di chuyển quay về
vùng đánh quái"*, asked for the skills during the bag jobs (*The hold's key
window*, below) and wrote *"Mua HP/MP cần để người chơi tự setting bao nhiêu
lần chứ không phải là max 20"*. The slow bag cleaning, *"dọn rác chậm là từ
user feedback"*, came from other players of the shipped zip, with no log of a
delete pass anywhere: it is judged on the zip's settings (four tabs ticked,
`press_ms` 80). The owner's answers: the pet
*"Liên tục như GoC (0,2–0,5 s)"*; pushed outside the area, (a) *"Ngưng đánh,
đi về trước"*; the keys, letters and Space, *"Bấm cả bình"*, never in a charm
run; the purchases, *"Không giới hạn"* (`kBuySlotsPerRunMost`, under *Buying
by the slot*, above). What the 20:01-22:23 log of that day showed comes with
each.

- **The pet pressed over and over** (`MacroConfig::pet_every_ms`,
  `kPetEveryLeastMs`-`kPetEveryMostMs`, 200-3000 ms; `kDefaultPetEveryMs`,
  300, in a file without it -- the owner's Q at 3000 on both rows goes every
  300). On 2026-11-10 GoBot's own 3-s wait (±15%) between two presses of Q
  kept the pet under its 91% about half the time from 20:03 to 21:12; GoC
  presses its food every 200 ms. The pet's row has *Bơm liên tục, mỗi [N]
  ms*, one value for every pet key, Lv1 and Lv2, applied as it is dragged and
  written once let go of; the full screen's *Every (ms)* of a pet key is that
  value. A skill that is ready takes the press after the pet's own
  (`MacroEngine::pet_yields`, `kPetYieldMostMs` 500 at most, *a skill's turn*
  in the black box), so while skills are due the pet has at most every other
  press; HP and MP potions go first as ever. A pet key quicker than
  `potion_recheck_ms` is never re-checked nor pressed early again.
- **What came of the pet's presses**, once a minute (`PetHealWatch`, on the
  timer pass's thread): `Pet heals: key Q, N pressed; of N judged, N rose
  (median +N% after N ms; two that rose N s apart at the closest), N static
  -- the bar still -- in N runs (N at 71%, …), N no rise with the bar
  moving, N with no reading` -- a press judged `PetHealWatch::kJudgeMs` (700
  ms) after its key-down, a heal put down to the earliest press still open;
  the closest two that rose bound the game's own wait for the pet's food.
  The first press of a still run says `Potion check: Pet key Q pressed at
  71%, the bar within 1% of it for 700 ms -- no rise; pressed on every 300 ms
  all the same …`, and the first still press at
  `MacroEngine::kPlateauLowPct`-`kPlateauHighPct` (70-75%; 248 of the 552
  presses that did nothing on 2026-11-10 began there) keeps a picture of the
  pet's slot, `potion_debug\<time>_pet_<pct>.png`, cut and written off the
  window's thread: whether the game refuses the food there or the bar is
  misread is in it.
- **The walk back first, latched** (the owner's (a); *The walk back first*,
  under the hunting area, above). On 2026-11-10 -- t1901_s2, *Đứng yên một
  chỗ*, a 366-px box and a reach of 450 -- a monster in the box was in reach
  from almost anywhere outside it: the character was outside 23% of the
  time, and all 35 walks back began at a floor's end, each fight cutting the
  one before. Now, more than `kZoneOutsideBackPx` (60) out, nothing is fought
  until the character is inside the box, and the skill keys wait while it
  walks (`Hunter::returning`, `MacroEngine::Fight::Mode::Returning`, read from
  the hunter on every pass); the card says *… — bot ngưng đánh, về vùng
  trước: đi bộ về X, Y, vào tới trong vùng rồi mới đánh tiếp.*, its hover
  takes the reach from `hunt.ini`, and every dot on **Bản đồ** is grey
  meanwhile (`mob_grey`).
- **A door back to the area's map**: an area set aside by a map entry -- not
  by a log in, nor by the tab -- is taken again when the character comes back
  to that map by any door, wherever it lands, once a report of its own has
  come or `Hunter::kDoorLoadMs` (3 s) has passed (`ZoneWait::Loading`, then
  Back), where it used to stand; meanwhile the card says *Vừa qua cửa về lại
  bản đồ này: chờ bản đồ tải xong rồi dùng lại vùng đã lưu và đi bộ về vùng.*
- **No spot where nobody stands**: a report at y 0, or more than
  `Hunter::kOneJumpPx` (120) above the last ground report with no jump of
  the character's between (`Hunter::spot_plausible`; 999,0 for 79 s at
  21:11), is not taken for the spot, nor by **Bản đồ**'s `MapTrust` for a
  standing point (`MapTrust::kOwnRisePx`); `Hunter: a report of yours at
  x,y not taken for the hunting spot -- …` at most once a
  `Hunter::kSpotOddSayMs` (a minute). `PacketWorld::take_own` keeps every
  frame, so facing and speed are as they were.
- **One map of several, by a click**: with the map not told apart -- several
  files fit, as t10_s60, t1901_s2 and t1_s60 did from 21:01 to 21:11 -- and
  just one of them with a saved area, the **Bản đồ** card offers *Dùng vùng
  <stem>* (`zone_choice`, `MapTrust::choose`); GoBot never takes it alone.
- **The walk's log**: `Hunter: N px outside the hunting area -- no fight until
  the character is back inside it: walking back first, to x,y (reach N px)`;
  `Hunter: the walk back first is over after N s, from N px outside -- …`;
  `Hunter: walk back over after N s -- …` (arrived, cut by a fight with slot
  N inside or outside the box, given up, a map entry); the minute's
  `Hunter:` line adds `; outside the area N s, walking back N s` and the
  `Skills:` line `, N walking back`. And for every walk of the character's, on
  the capture thread, `Positions: own walk 25,632 -> 363,632 (+338 px) in
  0.94 s, stopped by 0x0027; 2 casts of yours in it; GoBot: no arrow within
  300 ms; last key Z 85 ms before` (`PacketWorld::own_walk_line`,
  `InputManager::walk_keys_text`, `kWalkKeyMs` 300): what walked the
  character out 32 times on 2026-11-10, about 340 px each, with no walk of
  GoBot's in the log, is still not known -- Z, a melee skill walking to its
  target, is the suspect.
- **A bag pass with nothing to look through makes none** (`bag::pass_tabs`,
  `kNoTabVi`): no tab ticked under *Tìm trong:* and the bag key is not
  pressed nor the hold taken -- the card says *Chưa chọn ngăn nào ở 'Tìm
  trong:', nên không xoá được gì* -- unless something may be left in the bin
  to give back. Every item on the list a catalog icon under `spend/` --
  potions, food, fragments, charms, in Tiêu Dùng and nowhere else -- and only
  Tiêu Dùng is looked through, or none, with `kNoUseTabVi`, when it is not
  ticked; a photographed item or any other icon, and every ticked tab is, as
  before (`Trash: every item on the list is a spend/ icon -- only Tiêu Dùng
  looked through, not …`).
- **Each item quicker, every check kept**: the bag's opening and the item on
  the bin looked for every 80 ms (`kQuickLookMs`; 150 before) up to a clock's
  bound (`kBagOpenWaitMs`, `kInBinWaitMs`; since 2026.11.15d `kReactMs`, 30
  s, both gone), the bin's dialog waited for by the
  clock with "gone" two looks `kDoneApartMs` (150 ms) apart, one dialog search
  a frame, the last frame of a deleted item judged by the next round when
  nothing was up on it, and the drag's ten steps paced by *Tốc độ bấm nút*.
  The quantity box's 750-ms watch with 3 agreeing reads, `back_out`'s polls
  and the close poll are as they were (since 2026.11.15d by the clock: the
  bin's question, its click and the close after Xác Nhận up to 30 s,
  `back_out`'s Hủy Bỏ 6 s a click). By the code's model, not yet measured:
  9-14% an item (2.6 to 2.3 s at 80 ms). With the zip's four tabs ticked and
  every item on the list a spend/ icon, so that only Tiêu Dùng is looked
  through: a pass with two stacks 12.6 to 6.8 s, one with nothing to delete
  7.4 to 2.1 s. With any other item on the list, a picture cut from the bag
  or an icon of another group, all four tabs are still looked through: 12.6
  to 12.0 s and 7.4 to 7.3 s, the item's gain and the bag's quicker opening
  only. All of these come before the key window's waits. Each item logs
  `Trash: item at Tiêu Dùng, hàng 1 ô 1 (giống 97%) deleted in N ms -- drag
  N ms, in the bin after N ms (N
  looks), its dialog N ms after the click, the box read on N looks in N ms,
  closed N ms after Xác Nhận (1 question), then N ms till the item was seen
  gone, last look N ms`, and each pass `Trash: pass steps -- bag opened in N
  ms (N looks); Tiêu Dùng: tab N ms, top N ms, N pages (scrolling N ms), N
  items in N ms; bag shut in N ms; N ms in all`, on the trash worker.
- **The purchases**: *mỗi lần mua [N] ô* from 0 to 999, a hint when the two
  together pass `kShopMaxBuys` (40) -- *Buying by the slot*, above.
- **No death from an HP of 0 the screen alone reads** (`death_doubt`,
  `GuiApp::death_doubt_now`): at 22:12:52 an *ALERT death* came while
  LDPlayer covered the game and the game reloaded, the packets saying
  14585/14585 (11.4 s old) and the server silent since 22:12:41. Not while
  the game is behind with the frames off the screen, not while usable packets
  say HP is above 0, not while the server has been silent on the game's
  capture past `kLinkQuietMs` (5 s) and up to `kDeathQuietMostMs` (60 s);
  `Alerts: HP reads 0/N on the screen -- not taken for a death: …` as the
  reason changes, and a real death alerts 3 s after nothing doubts it.

Still not known, each for the next log: the pet food's own cooldown (between
about 1 and 2.4 s) and why presses do nothing at 70-75%; what walks the
character out of the area; real per-item and per-pass times; whether skills
cast with the bag open. The owner's live test is step 13 of section 6 in
`DOC-TRUOC.txt`: the pet's food counted each hour with its `Pet heals` lines;
the time outside the area, the walks back and the own walks, after a hand
test of Z, X, C, V and Space with the bot stopped; one pass with Tiêu Dùng
alone ticked, *Thử tìm* then *Dọn ngay*, its timing lines and the casts in
it; and one small **Mua ngay**, watched, with the skills between purchases.

### The hold's key window (2026.11.10)

The owner, 2026-11-10: *"Khi dọn rác thì cần ra các skill thuộc phím chữ cái
(z,x,c,v, v.v.)"*, then *"cả khi mua HP và MP cũng cần ra skill nhé"*, *"có
tính phím space, vì phím space không ảnh hưởng"* and *"lúc dùng bùa thì không
cần ra skill"*; the answer, *"Bấm cả bình"*. A bag job still takes
the one hold that refuses every key below Critical -- the hunter, a walk, the
drop and the jumps stand -- but a Dọn rác pass (*Dọn ngay* and *Thử tìm*
too) and a *Tự mua bình* round (*Mua ngay* and *Thử mua* too) arm a key
window beside it (`ExecutionCoordinator::arm_pass_keys`), separate from the
hold's reason, which lets the macro's keys on a letter A-Z or Space through:
skills, buffs and potions alike, built from the macro's keys in memory
(`MacroEngine::pass_keys`). Never a digit, Enter, Esc, an arrow, a modifier,
an F-key or an OEM key (`pass_key_allowed`), the bag key, the quest window's
key or GoBot's own switches; never a step of the key sequence; never in a
charm run, the give-back look, or a job begun with something maybe left in
the bin. The job opens it only on a look just taken with nothing up -- no
dialog, message, check-in board or quantity question, nothing out towards
the bin -- and shuts it before every click, drag, right-click, double-click
and key of its own, waiting up to 600 ms for each key let through to be up;
one still down, and that input is not made and the job stops. A shop round
shuts it from just before each double-click until the question is seen gone
and the bag-full message has had its second; a purchase whose look was judged
with the window open is judged again on a look taken after the shut, and the
double-click goes by that one. Its quiet second is measured with no window
armed, and a second in which GoBot itself sent anything is measured again
(`quiet_said`; `InputManager::sent_count` moves both as an event is handed to
Windows and once it has gone), never taken for the player's. A look the
window opens on is searched for dialogs once. A pass drags or
right-clicks a slot only from a look taken with the window shut and half a
second after the last potion let through. A key the window holds is
`Refused::Gated`: not a refusal of every key, so a digit potion under its
mark holds the letter skills up 300 ms at most, and a skill or a buff it
held is tried again 30 ms on. F12, DỪNG and the job's end close it
(`PassKeysScope`, `stop_jobs`, `stop_trash`), as does any change of the hold.
Each job logs `Trash: pass's key window: Z 5, X 4, G 1 through, 3 times open
(1840 ms)`. Since 2026.11.11 the window also lets through Home, End, PgUp,
PgDn, Insert and Delete (`pass_key_nav`), each key in that line by its name
(`InputManager::key_name`), and opens for an HP or MP potion due; digits
still never go (below).

### Charms, drops, the quick quest and popups at any HP (2026.11.12b)

Since 2026.11.12b (not yet run on the game). The owner, 2026-11-12: *"hiện
tại còn lỗi máu thấp thì không dùng bùa nữa, cái này cũng cần sửa"*, *"kiểm
tra xem còn chức năng nào máu thấp thì không thực hiện nữa không"*; answers:
the drops and jumps up, Nhiệm vụ nhanh and Tự đóng thông báo too, Tự chat
unchanged.

- **Charms.** A charm run -- timed or *Dùng bùa ngay* -- starts and goes on at
  any HP: only the character down (`bag::kJobDownHpPct`) puts it off or stops
  it, and HP not read still waits. Until 2026.11.12 it waited for
  `chat_hp_floor_` + 10 and stopped under the floor -- 90% and 80% with the HP
  potion at 80%, which a fight hardly ever gave. The run presses the HP and MP
  potions itself (`charm_potions`, `BagCleaner::OwnPotions`) at the top of a
  round of `charm_sweep`, between two charms -- the bag on Tiêu Dùng, nothing
  up (`keys_clear` lets a charm run with no key window through), its scroll
  read -- and looks again after it; with the macro running too, since the
  run's hold lets no window through, but then only once the macro's own
  cooldown for that key is over (`MacroEngine::vital_potion_due`). Never a
  digit, never the pet's; skills and buffs still wait the run out. Logged as
  `Trash: own potion press between two charms -- ...`.
- **Drops and jumps up.** `GuiApp::drop_veto_now` no longer says "HP is low": a
  monster's drop or jump up and the way back's go at any HP -- the macro's
  potions are not held by them. "HP is not known" stays.
- **Nhiệm vụ nhanh.** The gate's floor and the round's `low_hp` are
  `bag::kJobDownHpPct`: a round starts at any HP and is ended part way only by
  the character down; the potions still wait its few seconds out.
- **Tự đóng thông báo.** Its `hp_low` is `bag::kJobDownHpPct` too.
- **Unchanged.** Tự chat (a line starts at the floor + 10, 95 at most, and
  ends under the floor: a potion key would type into the chat box) and a key
  box waiting in GoBot's window (`keep_game_in_front`) still go by
  `chat_hp_floor_`.
- **From the review.** `MacroEngine::own_potion_due` now takes the caller's
  `cooling` and `macro_cooled`: a key still cooling -- the job's own press, or
  with `macro_cooled` the engine's (`potion_cooled`) -- lets the other kind be
  looked at, so an MP potion due is no longer kept waiting through HP's whole
  cooldown; and the cooldown is `potion_urgent_ms` under half the highest mark
  on the key, as the timer pass has it. `own_potions` (the pass and the shop
  with the macro off) goes by the same. In a charm run `keys_clear` refuses
  any dialog on the look and ignores `late_ask_` (a right-click that brought
  nothing up held every later potion of the run). The drop veto says "the
  character is down" under `bag::kJobDownHpPct`: drops and jumps up pressed on
  a dead character were judged to do nothing and went off for the run.

### The bag jobs at any HP, and their own potions (2026.11.11b)

Since 2026.11.11b (not yet run on the game). The owner, 2026-11-11: *"Hiện
tại máu thấp đang không được mua bình, làm cho có thể mua bình ở bất cứ mức
máu nào và vừa mua vừa bơm máu"*; answers *"GoBot tự bấm bình"*, *"Giữ chặn
phím số"*, *"Cả dọn rác"*.

- **Any HP.** A Dọn rác pass and a Tự mua bình round -- timed, *Dọn ngay*,
  *Mua ngay* -- start and go on at any HP: `bag::hp_gates` gives
  `bag::kJobDownHpPct` (1%) for both gates whatever the potion, so only the
  character down holds a job back or stops it (put off, never trouble, again
  in 15 s), and HP not read still waits. This replaces the 11.11 gates below
  (the mark and half of it, or the floor + 10). Charms and chat are unchanged.
  Since 2026.11.21 (the audit's B2; the owner: *"không để các chức năng block
  nhau"*): with the macro running and its HP potion due on a key the job
  cannot let through or press itself -- a digit, the shipped 1 -- a timed
  pass, shop round or charm run that has held the game 10 s
  (`bag::kDrinkYieldAfterMs`) gives the hold back at its next safe point
  (between two items, before a purchase's double-click, between two charms;
  `BagCleaner::set_yield`, `bag::drink_yield`), the macro drinks, and the job
  goes again 15 s on. No HP mark gates any job. And a hold kept for something
  left up (the check-in board over a quantity box at 00:00, a dialog nobody
  answers) is swept with ESC by the trash worker itself under that hold once
  kept 30 s -- the board up 30 s and past 00:03 (`bag::kept_sweep_due`,
  `BagCleaner::sweep_kept`) -- and let go on a clean look, as ever.
- **Own potions.** With the macro running, the HP and MP potions go between
  the job's clicks through the key window, as in 11.11. With it not running
  (the bot stopped, the macro off) the job presses them itself
  (`BagCleaner::OwnPotions`, `MacroEngine::own_potion_due`): the tier the
  macro would press for the reading, HP's first, never the pet's, each key's
  cooldown kept across the jobs; at the moments of a potion break (the shop
  seen, before each double-click, between two items), on `keys_clear` (nothing
  up, never in the money stretch or a charm run), through the job's own
  `press`, then half a second waited and a fresh look taken before a drag or a
  double-click. Logged as `Trash: own potion press ...`.
- **Never a digit.** A potion on a digit (the simple screen's 1 and 2) is never
  pressed by a bag job, said once a job (`Trash: a potion due on key 1 is not
  pressed by a bag job ...`); with no HP wait any more, move it to a letter or
  to Home/End/PgUp/PgDn/Insert/Delete.

### The shop after the pass, HP during the bag jobs, and six more keys (2026.11.11)

Since 2026.11.11 (not yet run on the game). Users of the zip, as the owner
passed it on on 2026-11-11: *"- Sau khi dọn rác thì không lập tức mua bình
ngay mà bị delay tới tận vài phút. Việc này dẫn tới khi bắt đầu thực hiện mua
bình thì rác đã đầy túi đồ => Cần cho thực hiện mua bình ngay sau khi dọn rác
xong. - Làm cho có thể bơm HP và MP khi đang dọn rác và mua bình. => Hiện tại
khi HP thấp thì sẽ ưu tiên bơm xong mới mua bình dẫn tới việc mua bình bị
delay. Cần sửa vấn đề này. Ngoài ra làm cho các phím như home, pgup, pgdown,
end, delete có thể sử dụng được"* The owner's answers: the shop after the
pass, *"Tới giờ mua: dọn rồi mua"*; HP and MP during the bag jobs, *"Mua
luôn, bơm xen"*; digits, *"Không cho phím số"*; the keys, *"Home End PgUp
PgDn Ins Del"*.

- **The shop rides a pass** (`bag::ShopRide`, `bag::ride_turn`,
  `bag::ride_refused`, `GuiApp::trash_loop`). The pass and the shop had two
  clocks that knew nothing of each other, so a round came anywhere up to an
  interval after a pass, the bag full of loot again by then. The shop keeps
  its own clock and plan rules -- `buy_every_minutes`, `shop_clock.txt`,
  never early, `kShopBuysPerHold` (10) a hold and the rest `kShopPartGapMs`
  (60 s) on, **Mua ngay**'s `kShopMaxBuys` (40), the bag-full end -- but a
  timed look come due, a plan or an owed part, with **Tự dọn rác** on pulls
  the pass to now, and the round is bought right after it under the same
  hold: the "trash cleaning" hold and the game's mouse lock carried from the
  pass into `run_shop` (`after_pass`), nothing let go between, no quiet
  second and no HP start gate of its own -- the pass passed its gate a moment
  before, and the guard watches HP through the round. A look come due while
  a timed pass runs rides that pass too. The pulled pass is a timed pass like
  any other: the next comes `interval_minutes` after it. With a pass ended
  less than `bag::kRideFreshMs` (60 s) ago none is pulled, and the round goes
  on its own at once by its old path. The first pass runs 15 s after a start
  and the shop's first look 45 s after it (since 2026.11.12 a whole interval
  after it, *the start count*, below), and each clock counts from its
  own runs, so when `buy_every_minutes` is a multiple of `interval_minutes`
  -- the shipped 10 and 60, or 5 or 15 with 15 -- each timed round usually
  comes within that minute of a timed pass and is bought on its own right
  after it. A pass is pulled when a round comes due longer after the last
  one: other numbers (10 with 15), an owed part a minute after a round
  (usually a short pass), a plan time read from `shop_clock.txt` on a start
  or a re-tick, a **Mua ngay**, a round or a pass put off. None is pulled
  where the shop's own cheap gates would put the look off -- the chat line in
  doubt, no game window or the game off the screen, HP not read (the login
  or character screen, a map loading, the death panel), a keeper's crossing
  or cold start -- nor for an interval after a pulled pass found an empty
  list or no tab ticked (`no_pull_before_ms`): the look waits by the shop's
  rules, and the first one after pulls a pass. No ride after a pass that did
  not run or did not end clean -- an empty list, no tab, no game window, the
  chat line in doubt, another job's hold, HP, a stop, trouble, a kept hold,
  the board -- and the round then goes on its own by its own rules; a ridden
  round still waits while HP is not read. A ridden round
  comes before a charm run due at the same moment; otherwise the order is as
  it was: a chat line, the pass, the charms, the shop. **Dọn ngay**, *Thử
  tìm*, **Mua ngay**, *Thử mua* and the CLI are unchanged; a **Dọn ngay**
  never buys. The shop opens the bag itself, as before. With both ticked the
  card says *Dọn rác đang bật: tới giờ mua, GoBot dọn rác trước rồi mua bình
  liền sau đó, trong cùng một lần dừng phím, không chờ thêm. Vừa dọn xong chưa
  tới 1 phút thì mua luôn, không dọn lại.*
- **HP and the bag jobs** (`bag::hp_gates`, `PassKeys::hp_potions`,
  `PassKeys::hp_mark`, `PassKeys::hp_all_through`). With the HP potion's key
  one the job's key window lets through -- a letter, Space or one of the six
  below, the macro on, the job not begun with something left in the bin --
  and, with two HP lines, every one whose mark is at half the highest or
  over on such a key (the macro presses only the tier HP is under, so a Lv1
  on the digit 1 above a Lv2 on G would leave HP undrunk from its mark down
  to G's) -- a timed pass or shop look
  starts with HP at the HP potion's mark (the highest enabled HP mark, 95 at
  most) rather than 10 over it, looks again `bag::kHpLookAgainMs` (5 s) on,
  and the job's guard stops it only under half that mark: a mark of 80
  started at 90 and stopped under 80, and now starts at 80 and stops under
  40. With the HP potion on a digit -- or one such line of two -- the macro
  off or no HP potion, as before -- started at min(95, floor + 10), stopped
  under the floor -- but looked at again in 5 s, not 15. `chat_hp_floor_`,
  the charms' gate and guard, and the pet are unchanged.
- **Potion breaks** (`BagCleaner::KeyWindow::potion_due`,
  `MacroEngine::vital_potion_due`, `BagCleaner::potion_break`): with an HP or
  MP potion due whose key the window lets through -- never the pet's -- the
  window is opened through `keys_open`, with all its checks, and kept open
  `BagCleaner::kPotionBreakMs` (400 ms) at most while one still is: before
  each purchase's double-click (before `money_`), on the look that first sees
  the shop, and in a pass between two items on the look handed over clean.
  The look the double-click goes by, and the slot dragged next, are then
  taken after the shut, as after any open look. `Trash: potion break before
  the double-click -- the key window open N ms for an HP or MP potion due`
  (or `as the shop is seen`, `between two items`; `, one still due` after the
  whole 400 ms).
- **A stop by HP is no trouble**: a pass its guard stopped for HP is tried
  again `bag::kHpStoppedAgainMs` (15 s) on, not an interval; a shop round so
  stopped ends Paused (`bag::shop_end`, by the guard's own `hp_cut`), looked
  at again in 15 s whatever HP reads by its end; a key of the macro's not
  seen up in time (`Result::keys_late`) is Paused too, a part's gap on. The
  bag-full message, or any lone message in the quantity box's place, is
  closed whatever HP is (`set_hold_check`: a stop or the hold lost only).
- **Home, End, PgUp, PgDn, Insert and Delete** (`pass_key_nav`): exactly VK
  0x21, 0x22, 0x23, 0x24, 0x2D and 0x2E go through the key window as the
  letters do, for skills, buffs and potions alike, at the same moments;
  never a range -- the arrows 0x25-0x28 (↓ among them) and 0x29-0x2C stay out
  -- and digits, the number pad and the F-keys stay out too. What the game
  does with them on the bag or the shop is not known, so after one came up
  (`ExecutionCoordinator::pass_nav_ms`, set in `release()`) every input of
  the job's own, and the look it goes by, waits until
  `BagCleaner::kNavSettleMs` (200 ms) after it (`keys_shut`). A shop's
  double-click goes only by a look on which the potions' row itself matched
  (`row_score > 0`): one that found only the buttons is looked at again
  `BagCleaner::kShopRowRelooks` (3) times, a look apart, and then the round
  stops with nothing clicked and no box up, as trouble, `shop_row_unseen`
  kept, and for each look `Trash: shop row not matched before the
  double-click on Thịt bò khô -- its buttons alone at x,y (row 0.00, buttons
  0.97), look 1 of 4`. `shop_now`'s fallback stays for closing the shop and
  for the run's end.
- **Naming**: `InputManager::key_name` says Home, End, PgUp, PgDn, Insert and
  Delete, and `parse_key` also takes `pgup`, `pgdn`, `del` and `ins`, which
  set P, P, D and I before; the window's line names its keys (`Trash: pass's
  key window: Z 1, Home 2, PgDn 1, Delete 1 through, …`). On the simple
  screen Delete is a key like any other, and Backspace alone clears a key
  box: *Nhấn phím muốn dùng. Esc để huỷ, Backspace để bỏ phím này.*

New in the log, on the trash worker: `Trash: shop due -- a pass first, then
the round under the same hold (the last pass ended N s ago)`, once until a
job runs or a new reason puts a look off; `Trash: shop rides the pass -- the
same hold, no quiet second (the look that pulled it)` or `(come due while it
ran)`; `Trash: shop -- no ride on the pass it pulled (…): it goes on its
own`; the shop job line ends `; right after the pass, the
last pass ended N s ago` (or `on its own`, `Mua ngay`, `Thử mua`); `Trash:
timed shop put off -- HP 62% under the HP potion's mark 80%` and the like
(`timed pass`, another job's hold, the quiet second touched or GoBot's own
input, the board), once each time the reason changes; `Trash: pass stopped by
HP -- HP 38% under the level 40%`; `Trash: shop round stopped by the hold --
…'s now; HP N%, the level N%`.

Still not known: what the six keys do in the game with the bag or the shop
open, and whether the live shop's row still matches with a cell selected
after purchase 1 -- if it does not, rounds now stop after one purchase with
the lines above, where 2026.11.10 went on by the buttons alone. An HP or MP
potion on a digit -- the simple screen fills in 1 and 2, and so does the
zip's `macro.ini` -- still waits through a pass and a round: move it to a
letter or one of the six. The owner's live test is step 14 of section 6 in
`DOC-TRUOC.txt`: a due shop with *Dọn rác* on -- every 10 minutes, buying
every 15, so the round comes due minutes after a pass; with 5 or 15 it goes
on its own within the minute after one, as it should -- a pass and then the
round at once with no quiet second; HP under the mark during a round, the potions
between purchases and the round going on; HP under half the mark, the round
stopped and bought later; each of the six pressed by the macro with the bag
open and with the shop open, watched.

### Turns, the area, falls and two bars (2026.11.12)

Since 2026.11.12 (not yet run on the game). The owner, after the hunt of
2026-11-11 on ĐồiĐen[5] (`t62_s5`) and PhòngSinhTử[4] (`t23_s14`): *"Quay
mặt sai, đánh hụt"*, *"Rơi xuống, không về chỗ"*, *"Đi lung tung, đi lạc
xa"*. The answers: the dash key, *"C, V hoặc Space"* (Q1); the skills,
*"GoBot giữ skill lúc nguy hiểm"* (Q2); monsters outside the area in reach,
*"Chỉ đánh quái trong vùng"* (Q3); fallen out with no way back, *"Tìm cách
quay lại chỗ cũ, khi bị rơi xuống thì tìm platform có thể nhảy lên theo map
rồi thực hiện nhảy cao nếu cần"* (Q4); the jump to the top of `t62_s5` at
21:50:37, *"không, nhân vật bị rơi xuống vực nên bị di chuyển lên trên
đỉnh"* (Q5); the two-level area on `t23_s14`, *"nó vẫn thuộc vùng đánh quái
nhưng bot không biết cần cho nhảy xuống. Nên thể hiện 2 thanh trên và dưới
trên bản đồ để có thể thiết định xem bot sẽ nhảy xuống ở mức bao nhiêu
pixel."* (Q6); *"Ngoài ra hiện tại đang giới hạn không cho bot nhảy xuống
hoặc nhảy lên nếu không quá 30 pixel, cần bỏ giới hạn này"* (Q7); and the
bag jobs at a start, *"vậy thêm 1 chỉnh sửa vào lần fix lần này, đó là khi
bật thì bắt đầu đếm thời gian. Lúc nào tới thời gian thì mới bắt đầu thực
hiện"* (Q8).

The log behind them: a skill that carries the character about 214 or 319 px
with no walk -- 43% of the travel across on `t23_s14` -- pressed every 0.5 s
by the macro's `always_timer` keys whatever the hunter did; turn taps the
game dropped during casts (about 40%) with the hunter swinging as if they
had turned; the attack key Z, which reached about 76 px, pressed from up to
450 px; a drop's landing on its own take-off slope judged off the file, and
mid-fall reports taken for standing points, each dropping a right file (on
`t23_s14` no drop, jump up or way back for 8.5 minutes); a report at y 0
that walked the character 2910 px; and a spot taken at the map entry's
spawn, 33 px in the air.

- **Turns** (`Hunter::face_target`, `tap_turn`, `Hunter::turn_`). With the
  packets steering and a `0x0027` facing seen since the map entry, a turn is
  pending from its tap until a `0x0027` its way stamped at or after the tap
  (`WorldFix::cast_ms`, `PacketWorld::Me::cast_ms`); while it is, the hunter
  does not swing and publishes `turning()`. It is tapped again
  `Hunter::kFacingMissedMs` (300 ms; 500 before) after the last tap once no
  cast of yours has come for `kTurnQuietMs` (150 ms), and taken as made after
  `kTurnCapMs` (1000 ms). `kTurnTapMs` is 80 ms (60 before). A new pick, the
  hunter's own walk or step, or a facing not known is turned to at once;
  otherwise only once the target has stayed the other side more than
  `facing_tolerance` + `kTurnSlackPx` (25) for `kTurnStayMs` (250 ms), or past
  `facing_tolerance` at all for `kTurnBandStayMs` (600 ms) -- a melee monster
  stopped behind to hit, a dash ending just past the target -- and until then
  it is fought where the character faces. The turn's hold is out before its
  tap goes down (`tap_turn`), and no tap goes off a pit's edge counting the
  taps not yet confirmed (`turn_pit`: `Hunter: not turning right toward …
  (a pit) …`, the monster passed over). The pick order is
  unchanged. The minute's `Hunter:` line: `N turns (T taps; C confirmed by
  the packets, the report A ms after its tap on average, M at most; K not
  confirmed in 1000 ms; P facings put right by the packets; keys the game
  missed: X turn taps, Y walk holds)`.
- **The attack's reach** (`Hunter::attack_limit_px`): the hunter's own
  attack key is pressed only within `attack_range_px` (`hunt.ini`, 0-1000, 0
  as shipped) or, at 0, the reach `CastMatch::learned_reach` has seen its
  skill make once `CastMatch::kReachCasts` (5) casts named a monster, plus
  `kAttackReachSlackPx` (24); with neither, at any distance as before. And a
  macro key on its own timer -- `always_timer`, or any with *Chỉ khi đang
  đánh quái* off -- waits beyond its `range_px` while the hunter fights
  (`SkillWait::Range`). `; own attack key let be N times beyond its reach (L
  px), M at an edge as the dash's` in the minute's line. With a style, a
  swing let be counts as one for the no-damage give-up (`DamageWatch::held`):
  a monster nothing hurts is passed over after `no_damage_ms` as before; one
  the macro's skills hurt is fought on, never walked in on.
- **The holds** (Q2; `Hunter::SkillHold`, `skill_hold()`,
  `MacroEngine::Fight::Hold`, `skill_waits`). Skill keys -- `always_timer`
  and the sequence's steps too, never potions or buffs -- are held while a
  turn is pending (Turning), through a drop's settle and fall (Drop), with
  the spot in use and nothing to fight in the area (NoTarget), and more than
  `kZoneOutsideBackPx` (60) outside it with no walk back, or standing after a
  put-back (Outside); the walk back keeps its Returning hold. Nothing is held
  while walking to a target, nor under a critical hold. `Skills: N held for
  the hunter -- T turning, D a drop settling or in its fall, I nothing to
  fight in the area, O outside the area or stranded, L a leg of the seek's
  way, J a jump up; K of the dash's key near an edge` once a minute (L and J
  since 2026.11.12c).
- **The dash** (Q1; `PacketWorld::note_move`, `learn_dash`,
  `Hunter::dash_lands_out`). A report of yours at least
  `PacketWorld::kNoWalkMovePx` (100) across from the last, neither walking
  nor in the air, with no `0x002A` and neither at y 0, is logged --
  `Positions: moved +214 px with no walk, A,B -> C,D by 0x0027, facing
  right; your cast of skill S M ms before; its key C; GoBot: …`, 30 a minute
  at most -- and the cast before it
  within 1.5 s gets a vote. A skill with `kDashLearnMoves` (3) votes or more
  and `kDashLearnPct` (70%) of the last 20 is the dash, its length the
  longest move with another within 24 px (`Positions: the dash is skill S,
  about 214 px -- …`; each minute `Dash (packets): N moves of yours with no
  walk this minute (N px); the dash is skill S, key C, about 214 px (A of
  B)`). Its key is the one `CastMatch` pairs with that skill; until one is
  learned, C, V and Space, at `Hunter::kDashGuessPx` (320) -- since
  2026.11.17 no key until one is learned, `kDashGuessPx` only the length of a
  learned dash with none measured (*MP bar and skills while hunting
  (2026.11.17)*: the guess passed C and V over near every edge). The dash's key
  alone is held, spot or not, where its landing would be further outside the
  area's box than the character is, or past a floor's end of a trusted page
  (`Page::follow(...).ledge`).
- **Only monsters in the area** (Q3; `mark_out_of_area`, `mob_grey`). With
  the spot in use, a monster outside the box is OutOfArea in reach or not:
  never faced, attacked or walked to; the current target keeps
  `Hunter::kSpotKeepPx` (60). Free hunting is unchanged. With the spot in
  use and no drop able to go, a monster more than `down()` below is passed
  over unless it can be walked to (`choose_target`'s `skip_below`,
  `own_floor_marks`): on the character's floor or the one a walk off either
  end lands on. With no file, one clearly on another level: more than
  `max(down(), kOneJumpPx)` below, or more than across + `kSpotFloorPx`; one
  picked a few px down its own slope that the walk closes across and not in
  height is passed over on the way (`below_unwalkable`: `Hunter: X passed
  over -- N px below, …`). A **Bản đồ** area with no map file
  and the character inside it walks back no more (`go_home`); its *more
  than one jump* is judged from the box, not the home.
- **The way back** (Q4; `return_point` with `zone_whole_map`,
  `ZoneReturn::travel`, `track_zone_way`). Where the box's own floors give
  no way back, one is looked for over the map's other floors: walks, walk-offs
  (`kZoneTravelFallsMax`, 32) landing anywhere but a pit, and jumps up ↑, ↑↑
  and Ctrl+↑ by the 05b planner (`kZoneTravelJumpsMax`, 4; take-offs every
  1000 px along a floor too), the goal inside the box; jumps on it are judged
  in the whole map's box (`over floors outside the area` in the press line),
  so landing outside the area latches nothing. 05b's limits stand: 4 jumps a
  minute, off after 3 failed in a row or 5 in a run (paused, `kFailsCoolMs`,
  since 2026.11.15, the 5 only with more failed than worked), a map change
  stopping the hunt -- since 2026.11.24 never off or paused, a map change
  learned as a gate with the hunt going on (see the section of 2026.11.24).
  With *Nhảy xuống* ticked, drops not latched (none is since 2026.11.24) and the map trusted,
  Ctrl+Down through platforms too (`zone_drop_from`, `ZoneReturn::drop`,
  `home_drop`): the drop rule's own tests but the monster's (`DropFacts::home`)
  -- a platform, a landing holding either side, no gate within
  `drop_gate_px` in the fall's band, no no-drop point, never a pit --
  `kZoneTravelDropsMax` (8) a walk back, settled at the take-off, held still
  in the fall (`Hunter: drop (Ctrl+Down) on the way back: …`). With them off,
  the stranded line says `no way back the map shows but by Ctrl+Down, and
  Nhảy xuống is off`. Never a gate. This run's spot gets the same once
  the hunter's `MapTrust` trusts the map (`zone_page`; `drop_watch` runs
  for the spot too), its home on the floor under it -- so a ramp back to
  it is walked, never *TooHigh* -- but not the seek (`ZoneHere::from_map`).
- **Stranded** (`ZoneHere::stranded`): more than 60 px outside with walks back
  given up, one to be tried again, no way the map shows, or more than one
  jump below with no map to go by, the character stands: no walk, step, drop
  or jump toward a target, only in-reach monsters inside the area fought,
  the skills held; `Hunter: N px outside the hunting area at X,Y -- <why>:
  standing where it is until a way back shows (no walk, nothing outside the
  area fought, the macro's skills held)` once an episode;
  `Status::spot_stranded` puts it on the card. Until 2026.11.12 it fought
  where it stood. *GaveUp* is never for good (`track_give_up`): since
  2026.11.24 the round's wait is `spot_retry_ms` (30 s), then a new round of
  `Hunter::kSpotMaxTries` (3) tries, round after round -- sooner once a report
  has the character more than `kSpotProgressPx` from where it gave up, or the
  map has learned something (`Hunter: walks back given up on 3 times in a row
  -- the next round of tries in N s, or sooner once you are somewhere else`,
  `Hunter: trying the walk back again -- …: a new round of 3 tries`). From the
  review of 2026.11.12 it was four `spot_retry_ms` (60 s to 300 s,
  `kSpotGaveUpRetryMinMs` and `kSpotGaveUpRetryMaxMs`, gone) and one more walk
  back a round. With no file, a spot more than `kSpotFarBelowPx` (461, a view) below
  is not walked back to blind: stranded (`more than a view above it, no map
  to go by`).
- **Pits and put-backs** (Q5; `judge_place`, `pit_ahead`). A report at y 0
  drives nothing that pass; with a real height before it on the map,
  `PacketWorld::take_own` keeps that height and marks it (`Me::y_kept`,
  `WorldFix::y_kept`, `Positions: your report at X,0 (opcode …) -- y 0 is no
  place: height kept at Y` once a minute at most), so t1901_s2's 999,0 wall
  stalls nothing; no landing, standing point, put-back or spot is made of
  it. A report more than `Hunter::kPutBackPx` (1000)
  above the last good one with no `0x002A` and no map entry between is a fall
  into a pit the game put back up top: the spot stands (`SpotWhy::PutBack`),
  any drop or jump ends, and once a report within `kPutBackAgreePx` (400)
  agrees, or none comes for `kPutBackWaitMs` (3 s), `Hunter: the game put you
  at X,Y, N px from A,B (N px up) … the way back is worked out afresh from
  here`; a next report back near the old place drops it as a misread. No
  walk or step back goes past a floor's end whose walk-off lands on nothing
  of the file (`Hunter: not walking left -- the floor ends at X,Y with
  nothing of the map file under it (a pit …)`, `; walks stopped at a pit's
  edge N`); a drop through a platform over nothing is `NoLanding`, as it was.
- **Trust** (`drop_follow`, `MapTrust::own_judged`, `spot_y_for`). A drop's
  landing on another floor of the file is `DropVerdict::OtherFloor` -- landed,
  nothing learned or distrusted (`N on another floor` in the minute's line);
  one on the take-off floor more than `GameMapBook::kNoDropXPx` (64) across
  is Unknown (`… something else moved you`); only a landing on no floor is
  OffMap. A point of yours off the floors counts against a file only once a
  report at least `MapTrust::kMobStandMs` (300 ms) later, not in the air,
  finds it at the same height. This run's spot is set on the floor under it
  when the map is trusted (`-- on the floor under X,Y, by the map file`);
  with none trusted, never at the entry's spawn (`Hunter: no hunting spot at
  the map entry's spawn X,Y -- …`) while the newest report is the entry or its
  `0x0028`s; a cast or a walk's report there sets it.
- **The two bars** (Q6; `band_bars`, `band_span`, `band_grip`, `band_drag`,
  `map_band_row`, `HunterConfig::save_band`). **Bản đồ** draws the band from
  the character's feet less `packet_dy` x `packet_scale` (`band_feet_y`, as
  the target ring and the *Chi tiết* box) as two dashed lavender bars across the area's box (or
  the view), labelled `+10 px` and `-20 px`, dragged in steps of
  `kBandStepPx` (5) with the camera frozen, or typed in *Vạch trên* and
  *Vạch dưới* under the map (0-`kBandMaxPx`, 1000); the release sets the
  hunter's config at once, and the map tab's worker writes `up_reach` and
  `down_reach` alone into `hunt.ini`. They are *Cao nhất* and *Thấp nhất*
  of **Tầm đánh…** (now to 1000, 400 before): *Quái thấp hơn vạch dưới: bot
  nhảy xuống; cao hơn vạch trên: bot nhảy lên; giữa hai vạch: đánh tại chỗ.*
  On the game's own `t23_s14` file with the owner's area (746..2844 x
  1145..1549, home 1987,1245), a monster on the lower floor at 2020,1374 is
  reached by two drops -- 1246 to 1314, then 60 px to 1374 -- the second
  refused before.
- **The minimums** (Q7). Gone: `drop_min_px` (100, applied as
  `max(down(), drop_min_px)` -- an older file's line is kept and not read);
  `Hunter::kDropMinFallPx` from 60 to `kDropLandPx` + 1 (25), the least fall
  a landing can be told from standing by; the no-jump pair, *Cao/Thấp không
  cần nhảy: 30 px* -- the owner's "30 pixel" -- never above the bars
  (`up_nj()`, `down_nj()`, the raw value kept); the seek's band margin
  clamped at 0. The 05b jump up already went by `up()` alone. Kept, as
  safety or not a height: the gate rules (`drop_gate_px`, `drop_side_px`),
  the pit's `NoLanding`, the minute caps and fail latches, `MapTrust`,
  `kOneJumpPx` (120, a maximum), `kJumpUpCeilingPx` (650) and the floor
  tolerances.
- **The start count** (Q8; `bag::start_due_ms`, `start_counting`,
  `pass_afresh`, `shop_afresh`). A start -- BẮT ĐẦU, F10, a box ticked on
  with the bot running -- has the first timed pass `interval_minutes` on and
  the shop's first look `buy_every_minutes` on, not 15 s and 45 s; a re-tick
  counts afresh. A clock the last worker left still counting (a quick DỪNG
  and BẮT ĐẦU) is kept, never under `kPassFirstMs` (15 s) or `kShopFirstMs`
  (45 s); one run out counts a whole interval. **Dọn ngay**, **Mua ngay**,
  *Thử mua* and *Thử tìm* go at once, and a worker here for them alone
  leaves the clocks. `shop_clock.txt` is still written and no longer read;
  `bag::shop_first_wait_s` is gone. The charms are unchanged. A bin that may
  hold an item at the start still gets its pass 15 s on. `Trash: counting
  from the start -- the first timed pass at HH:MM (10 min from the start),
  the shop's first look at HH:MM (60 min from the start)`, and `Trash: Tự
  dọn rác ticked on -- …`, `Trash: Tự mua bình ticked on -- …`.
- **The Bản đồ tab smoother** (the owner, 2026-11-12: "chưa mượt, chưa khớp
  mà bị delay khoảng 0.5 -1s"). Your dot follows your walk
  `PacketWorld::kOwnReckonMs` (3 s) on the tab, whoever walks: followed
  `kOwnStepReckonMs` (0.6 s), a walk of yours or one the hunter does not hold
  stood still -- Lost, at your last place -- until its stop report, and
  jumped. Drawing only (`map_look`): the hunter and the tab's worker follow as
  before, and `along_floor` still holds the dot where its floor ends. The tab
  is drawn `kMapTabFps` (30) times a second while shown, behind the game and
  in front alike (10 from 2026.11.05). Once a minute with the capture
  running: `Capture delay: last 60 s | N packets read A ms on average, M ms
  at most, after the quickest -- Npcap's stamp to GoBot's read`
  (`PacketCapture::take_read_delay`, `DelayMeter`); the minute's quickest
  packet is taken as read at once, as Npcap's clock and Windows' may stand
  apart.

From the top of `t62_s5` (3166,723) every walk and fall ends on the platform
spanning the map at about y 3000: with *Nhảy xuống* ticked the way back drops
through it and on down to the spot (5 drops on the game's own file in the
tests); with it off the character stands, said once. From 3166,5446 the way
back is found (along the bottom to x 1242, ↑ onto floor 5386, up its slope).
The way back's drops are not yet run on the game. The trust behind the floor-end dash hold and the pit guard runs with the
spot on, drops on or the map tab's jumps, not for free hunting with drops
off. Still not known: which key is the dash -- learned on the game. The
owner's live test is step 16 of section 6 in `DOC-TRUOC.txt`: the bars set
on `t23_s14` and the drop to the lower floor inside the area, the turns and
holds watched, the dash's key read off the log, and a fall on `t62_s5`
finding its way back with jumps.

### Jumps past the two bars, the wall foot, and the way back (2026.11.12c)

Since 2026.11.12c (not yet run on the game). The owner, the evening of
2026-11-12: "có trường hợp không quay lại được vùng train quái" (in the log
he pressed Ctrl+Up himself to get back into the area); of `t49_s5`'s step,
"Chỉ Ctrl+↑ tại chỗ"; "khi ở trên ngưỡng pixel thì chỉ cần nhảy thường (1 lần
mũi tên lên) còn khi ở dưới thì ctrl + xuống là được"; and of the plan, "phần
tự quay về vùng đánh quái khi ở ngoài hoạt động chưa ổn đâu".

- **The jump model.** `kJumpUpGravity` is 1900..2400 px/s² -- the bracket both
  readings of the live flights fit: g≈1905 from GoBot's own Ctrl+Up of 23:23:03
  taken at face value, and GoC's 2400 with a landing report ~0.3 s late. Peaks
  133..169, 250..337 and 432..546 px; landed on for sure up to 121, 238 and
  420 px (420 holds the owner's ~418 px climb from `t23_s14`'s bottom).
- **The column rule** (`judge_column`): the take-off's own floor followed to
  each margin column -- its own slope -- is no floor above (it gave Ceiling on
  ~105 of `t23_s14`'s bottom columns); the ceiling guard reaches only as high
  as each key does (↑ 245, ↑↑ 413, Ctrl+↑ 650); and floors in a key's uncertain
  band are accepted when every one is a platform whose standing point is in
  the box (the whole map for a way over other floors): the plan lands on the
  sure floor, a landing on any of them up to `JumpPlan::band_top` works.
- **The verdict** (`Hunter::jump_follow`): the 2026-11-12 client sent no 0x002A
  state 1, 2 or 6 in the air, so the kind is read off the take-off's vertical
  speed (`kJumpUpHighVyStep` -28: -36 is Ctrl+↑, -20 ↑) and a second push off a
  speed more upward than a free flight's (`kJumpUpSecondPushStep`); a landing
  on the planned floor is Worked whatever the kind; on a band floor Worked; on
  a floor above it inside the box within the key's reach `Moved` (no fail, no
  latch, the way asked again); only past `beyond()` the Physics latch (since
  2026.11.24 no latch: that take-off skipped `kLearnedForgetMs`, the jumps on).
  The measured-g guard is gross (1300..3200), and since 2026.11.24 only said.
  `PacketWorld` counts the air reports
  the same way, and says each unknown state once ("Packets: your 0x002A state
  ...").
- **A step of the ground is a wall** (`MapTrail::step`, `kStepMaxPx` 200): the
  first column past a floor's end holding no floor at or under the feet, its
  lowest solid ground or a wall no more than 200 px up, stops a walk at its
  foot -- `t49_s5`'s 2816 is no pit, so turns and walks toward the ledge go.
  At its foot the **wall-foot jump** (`judge_wall_foot`, `wall_foot_jumps`):
  `kWallFootKind` (Ctrl+↑) in place onto the step's top, planned, sought and
  taken on the way back like any jump; landed elsewhere, only wall-foot jumps
  are off for the run (`jump_wall_off_`), the way back takes over -- since
  2026.11.24 only that take-off is skipped, for `kLearnedForgetMs`, and every
  jump stays on.
- **The way back.** `kZoneTravelJumpFloorsMax` 96, `ZoneReturnStats` says what
  a search looked at; stranded, the way is asked again every
  `kStrandedReaskMs` (3 s since 2026.11.15; 10 s) and the stranded line names each key's reason at
  the stand ("here: ..."); the area is taken when the character is under it
  (`ZoneTake::Below`, 600 px, `kZoneTakeFallen`); stranded on a file not
  trusted yet, up to three steps along its own floor earn MapTrust's standing
  points (`kTrustSteps`) -- since the review each to a place of its own,
  `kTrustStepPx` (80) times 1, 2 or 3 either way of where they began, the
  nearest first, at least `MapTrust::kTrustSpreadXPx` from that place and
  every step's end, never off an end and no more than `kTrustStepOutPx` (24)
  further out (a relog's spawn is no standing point, and at `t23_s14`'s
  bottom two 80 px steps one way left no room for the third: the file was
  never trusted). Three steps -- or no place left for another -- and still no
  trust: since 2026.11.24 the steps come again `kTrustStepsAgainMs` (5 s; 30 s until the review of 2026.11.24)
  on, a new round from where it stands then (`Hunter: stranded on t87_s1, its
  file still not trusted after 3 steps -- the steps again in 30 s`); until
  then none until the next map entry. `way_back_cover_test.cpp` asks the way back from every
  floor every 32 px within 400 px beside and 500 px under the owner's five
  areas: `t23_s14` had no way from any of its 232 places before, and has one
  from all 232 now. Walked leg by leg, every way gets in at g 2400 and 1905
  with no jump landing where it was not planned -- but only the straight jumps
  are simulated afresh: walk-offs land by the planner's own
  `walk_off_landing`, drops and wall-foot jumps where planned. So it shows
  GoBot's plan reaches the area from every place given its own model of
  walk-offs, drops and jumps, not that the bot gets back: at 23:47:45 of the
  owner's log a walk off the small 1425 platform at x 1728 fell past the 1465
  platform (on the file at x 1707) to the bottom, a landing this model would
  count as made (why is not known; the charm job had just taken the hold).
- **The bars.** A monster above the upper bar within ↑'s lowest peak and in
  reach across (`Hunter::bar_reach`) gets one plain ↑ in place
  (`JumpUpFor::Bar`, `bar_up_toward`): faced, settled, every floor it may come
  down on inside the area (`bar_up_landings`), attacked in the air; 10 a
  minute (`kBarUpsPerMinute`, not among `kJumpUpsPerMinute`'s 4), 1.2 s after
  each landing, two that did not hurt it -- or came down on a floor neither
  its own nor the monster's -- pass the monster over, three that did nothing
  in a row (five a run) or one landing outside latch it for the run. (Since
  2026.11.15 the three, or the five with more failed than worked, only pause it
  -- `kFailsCoolMs`; the landing outside still latches it for the run. Since
  2026.11.24 neither: one that did nothing is tried again `kBarUpGapMs` after
  it, and a landing outside has its take-off skipped `kLearnedForgetMs` while
  the walk back takes over -- the ↑ stays on; the two that did not hurt it
  count over `kTargetCountMs`.) It
  replaces answer 4 of 05b (no jump attack in place) for those monsters.
  Below the lower bar, Ctrl+↓ as before, now landing within one ↑ under the
  monster's floor (`kDropBelowOneUp`) where that ↑, in place on the landing,
  comes down on no floor but its own or the monster's (`DropCall::one_up`);
  the monster's floor looked for `kDropMonsterFloorPx` (100) down as before;
  a level monster goes first when it is in reach, or nearer across than the
  target (`Hunter::level_before_drop`, `kBarNearestFirst`). The macro's skills
  are held on the seek's walk-off and jump legs and in a jump's settle
  (`SkillHold::SeekLeg`, `Jump`).
- **The review of 2026.11.12c.** A bar ↑ at a step's foot that climbs it is
  reported a few px past the step's face on its top: `Page::step_lip_at`
  makes that landing a stand on the step for every jump's verdict (it read
  as off the file: `t49_s5` distrusted for the session, every jump up off),
  and a bar ↑ with no take-off reported whose settled report is on a floor
  above is a climb, not a ↑ that did nothing. A drop's one-↑ landing that
  would have the ↑ come down on the floor the fall went through (`t23_s14`,
  1390 over 1465 at x 1700, the monster on the small 1425 at 1745) is
  `PastMonster`, as in 11.12b; one that goes holds no bar ↑ by `AfterDrop`
  (`last_drop_one_up_`). A bar ↑ `Moved` onto another floor counts toward
  the monster's `kBarUpDryPerTarget`, and up a floor holds drops for
  `kJumpUpStayMs` (`AfterJump`). A hit from below restarts the give-up clock
  (the monster was passed over 4 s into ↑s that hurt it). In a bar ↑'s
  flight `dash_lands_out` judges the dash from the take-off, so the macro
  holds the dash's keys near an edge as on the ground, and the in-air swing
  is held the same when the attack key is the dash's. The trust steps go to
  places of their own (above).

The owner's live test is step 17 of section 6 in `DOC-TRUOC.txt`.

### HP and MP figures with a digit missed (2026.11.13)

Since 2026.11.13 (not yet run on the game). In the owner's log of the night
of 2026-11-12, the bars' figures "11552/18590" read as 1/552 (0%), ten
times for HP and MP between 11,000 and 11,999, and "18590/18590" as
1859/18590 (10%). The game's set has lookalikes for two digits -- 'l', 'i'
and 'I' are 1 px stems beside the 3 px '1', and 'O' and 'D' sit over a '0'
and the slash beside it -- and `VitalsReader::read_numbers` passes over
every glyph that is not a digit, so a digit read as one left no trace. The
packets' check (`BarCheck`) took the read for the game's own numbers: "HP
from the screen again ... the bars 1/552 (0%)", 0% handed to the macro, and
MP went by the screen for the rest of the session at 00:40.

- **A hole on the pitch.** Measured on the owner's captures ("14915/16209",
  "15047/18590", "18873/18873"): digits 6 px apart, the '1' at its cell's
  start, a 3 px slash, so 9 px (1.5 cells) across it. A digit missed beside
  the slash makes that 15 px (2.5 cells); one missed inside a figure is a
  12 px break of its own beside the slash's. A first break of more than
  `kMissedStepCells` (2.0) cells, or a second break within it, refuses the
  read (`read_numbers`' `refused`): it counts as a miss, the kept figures
  stand. Writing past the caption stands 43 px or more off.
- **The believed maximum.** `VitalsReader::max_believed`: a maximum is
  believed once seen twice in a row, as before, and a read whose maximum is
  not the believed one now gives no figures until it is; a real change (a
  level, a buff) is taken on its second read. A misread held for two reads
  is believed as any maximum is -- the holes and the check below stand
  before it.
- **A digit lost where no hole shows.** A first or last digit missed
  ("11552" as 1552, "18590" as 1859) leaves no hole. `BarCheck::judge` now
  takes a figure, or a maximum, that is the packets' value less one digit
  (`figure_lost_a_digit`) as Between, not Disagree: the trust in the packets
  stands, as it did not on 2026-11-08 (MP 4798 for 14798) or 2026-11-13.
- **No landmark search for a refusal.** A refused read found the caption
  where it is looked for, so it no longer counts toward the HUD landmark's
  search (`hp_blank_`/`mp_blank_`); all misses still forget the figures.

### Buy right after the pass, and the game's message at login (2026.11.13b)

Since 2026.11.13b (not yet run on the game). Two requests from users,
relayed by the owner on 2026-11-13.

- **"Mua ngay sau khi dọn rác".** A box on the Tự mua bình card
  (`[Trash] buy_after_trash`, off as shipped). With it and Tự dọn rác on
  (`bag::buys_after_pass`), every timed pass that ran owes the shop a look
  (`look_owed` in `GuiApp::trash_loop`): the round rides the pass as in
  2026.11.11 -- the same hold, no quiet second -- or, the ride refused,
  goes on its own at once by the shop's rules. The clock begins no plan of
  its own (`bag::shop_clock_looks`, `bag::shop_plan_begins`): a plan begins
  with the pass's look, and its parts still owed are bought on as before --
  a pass owes a look only with nothing of a plan owed (`bag::pass_owes_look`;
  the review caught a part's pass beginning the plan afresh, buying until the
  bag was full, as a part's look always pulls a pass). A timed pass with
  nothing it could do -- the list empty, no tab ticked -- owes the look too,
  bought on its own. The owed look is kept across workers
  (`TrashClocks::look_owed`) and given back when a Mua ngay did not run.
  "mua mỗi" is greyed on the card. Tự dọn rác off, the clock buys as before;
  the box ticked off, the clock counts a whole interval from then. Dọn ngay
  and Thử tìm never buy.
- **The game's message after Kết Nối.** Users meet "Tài khoản đã bị chặn"
  after Kết Nối; the owner: press its Xác Nhận and connect again at once,
  with no limit. The client has one message box (`Ui\Common\Dialog_Message.spr`,
  its Xác Nhận `btn_Ok`): `grabbed/dialog_ok` is its frame 0, so
  `AutoLogin::message_in` is `bag::lone_ok` over the game's picture. While
  a page of the game is awaited, a message seen on `kMessageLooks` (2) looks
  in one place is closed (`close_message`: the label's middle clicked, the
  pointer then put `kMessageParkPx` (50) under the label, off the box, so
  its button is not lit -- lone_ok takes no lit button -- and the message
  gone on two looks; up to `kMessageClicks` (3) clicks -- since 2026.11.15d
  spread over 30 s from the first (`AutoLogin::react_ms()`), each watched
  `kMessageGoneMs` (2.5 s) at least -- else `Why::Message`
  ends the attempt; a click held back for `kHeldMaxS` ends it as Covered),
  and the crossing goes back to Kết Nối after `kMessagePauseMs` (1.5 s), not
  counted as an attempt (`messages_closed`). Without the pictures nothing
  changes. No real picture of the login-screen message has been seen: the
  first one of a crossing is kept as `login_debug\login_*_message.png`.

### The game at 1024x768, 1280x720 and 1366x768 (2026.11.13c)

Since 2026.11.13c (not yet run on the game). The owner on 2026-11-13: "nhớ
là cần làm cho đối ứng các độ phân giải: 1080 x 768 [1024 x 768], 1280 x 720,
1366 x 768". Every feature that reads or clicks the game was gone through for
the three client sizes. The game draws its UI 1:1, pinned to the client's
edges (the Level plate at 44,4, the hotbar 95 rows above the bottom, the
closed chat bar at 84,h-22, the minimap at W-210,26) or centred (the dialogs,
the check-in board) -- as seen at 1024x768, 1280x720 and 1600x900, and for
the HUD and the hotbar once at 1366x768; that size's chat, minimap, bag and
dialogs are inferred. Every bag, shop, charm, dialog, quest, check-in, reviver
and popup job finds its picture anywhere in the frame and clicks from it, and
the detector's box follows the frame's size (the minimap's did too, until
the minimap reader went in 2026.11.16). What changed:

- **The character select.** The login's art is a 1024x768 canvas, and how the
  game lays it out on another client has not been seen. The slots go by where
  `select_char_indicator` is found (`AutoLogin::slot_layout`, `slot_point`,
  summed by `slot_click` from the size of the picture `await` found it in):
  where the 1024x768 block centred or top-left puts it, the offsets as
  measured; pinned to the bottom-right corner, from the middle of the game
  (`w/2-188+202k`, `h/2+111` with the shipped offsets); anywhere else the
  offsets and a warning. The corner is a bet that can be worse than before:
  the whole 1024x768 block anchored bottom-right puts the picture in the same
  place (1184,655 on 1366x768, 1098,607 on 1280x720) with the stage beside
  it, where the offsets would be right and the middle is empty stage -- Bắt
  Đầu Game then starts the character already chosen, and nothing checks which
  one is. A GB_WARN says so when the corner is taken. `slot_anchor` in
  `[Page.character]` of login.ini (`auto` as shipped, `picture`, `centre`;
  `AutoLogin::slot_layout_for`) settles it without a new build once a kept
  picture shows the layout. On a client 720 rows tall the picture's bottom can
  run past the frame, where it is never matched: `AutoLogin::page_found` then
  takes its top `kCharStartRows` (50), "Bắt Đầu Game", at `kCharStartScore`
  (0.85) -- only when the whole picture would run past the bottom, never on
  1024x768, and never with the Kết Nối bar in the same frame (`by_start`).
  A 1366x768 window on a screen 768 rows tall with the taskbar hidden has its
  bottom 31 rows read black, the picture's bottom 17 among them: the
  character select is still found whole (about 0.90 against 0.70, pinned in
  the tests); for a page that is not -- Kết Nối's bar, unmeasured there --
  the Character and Connect reasons say the bottom of the game was hidden
  (`AutoLogin::fit_note`, from the fit taken as the game is read off the
  screen). The keeper's look at a hung game (`login_page_up`) goes by the
  same lookup, the channel list's panel included. At any size but 1024x768
  the screen is kept once a crossing as
  `login_debug\login_*_character_<W>x<H>.png` -- only while nothing is over
  the game -- and the log says `slot N at x,y on a WxH game (layout:
  picture|centre|unknown, slot_anchor=...)`.
- **The channel list.** The title's offsets are clicked only on a 1024x768
  game; on another with no panel found nothing is clicked, Kết Nối is not
  pressed, and `login_*_channel_<W>x<H>.png` is kept.
- **Game không nằm trọn trên màn hình.** Frames of this game are read off the
  screen on most machines (PrintWindow draws it blank), so rows past its edge
  read black and a taskbar over the game is read as the game.
  `AutoLogin::fit_of` / `game_fit` count the rows at the bottom and the top,
  then the columns at the right and the left, off every screen or under a
  taskbar drawn above the game, looked at with the display scale every 3 s (a
  minimised game keeps its last look); the checklist shows the row with the
  cure (`fit_advice`: 1280x720 with the taskbar hidden on a screen too short,
  else the taskbar hidden, and the game moved clear of it only where the
  screen has room for that). A red cross while the vitals reader's frames
  come off the screen (or before it has read); a dot, saying nothing is lost,
  where the game draws itself. With more than 10 rows hidden at the bottom
  the potion counts are not looked at, and with more than 6 Tự chat and the
  keyword's chat lines wait, each saying why -- only for frames read off the
  screen (`GuiApp::game_unseen`'s `from_screen`: the vitals reader's
  `capture_from_screen`, the party look's `from_screen`, Tự chat's own
  screen-only capture). The bag jobs are as before. Whether a game in Full
  Mode raises the row (game_fit counts only taskbars above the game) has not
  been tried.
- **Display scale in the login's words.** A game Windows enlarges adds
  `AutoLogin::game_scale_note` to the Connect, Channel, Character and World
  reasons.
- **Pictures kept.** `vitals_debug\size_<W>x<H>_<when>.png`, once a session
  for each size other than 1024x768 with HP read by the landmark, off the
  reader's thread, the newest 6 -- none at a size where the HUD is not found,
  which is when a player's own screenshot is asked for.
- At 1280x720 and 1366x768 the hunt's packet box is larger, so monsters a
  little farther away are offered.
- **What to ask a player at 1280x720 or 1366x768.** The game all on the screen
  (no *Game không nằm trọn trên màn hình* row) with nothing over it; an
  account with 2 or 3 characters, the 2nd or 3rd chosen, and whether that one
  entered; one Tự đăng nhập and a minute in the world; then `login_debug`,
  `vitals_debug`, `gobot_gui.log` and `gobot_gui.old.log`, with the screen's
  size, the display scale, Full or Window Mode (and in Full Mode whether the
  row appears), and whether the taskbar hides itself -- and a screenshot of
  the game in the world (Alt+PrtSc) when `vitals_debug` has no `size_` file.

### The game's window in the corner after a login (2026.11.14)

Since 2026.11.14 (not yet run on the game). The owner, asked whether GoBot
should move the game's window up: "Mặc định sẽ kéo lên góc trên bên trái
đi", then "chỉ kéo lên sau khi auto login thôi" -- and players at 1366x768
play in a window ("cửa sổ").

- Once a crossing has logged in -- "logged in", or "in the world after
  all" -- `AutoLogin::move_to_corner` moves the game's window to the
  top-left corner of the work area (the screen less its taskbar) of the
  screen it is on, `[Login] move_game_to_corner=true` as shipped: the
  keeper's crossings (Tự vào lại game, the login key) and `gobot_cli
  --login`/`--keepalive`. Only after a run that clicked or started
  something (`acted_`; the review: "in the world after all" with nothing
  clicked is no login), never at any other time.
- Where (`AutoLogin::corner_place`, pure): the visible frame (DWM's extended
  frame bounds, not the invisible resize borders) in the corner when the
  game's client then fits the work area whole; otherwise the client itself
  in the corner, the title bar pushed off the work area -- 1280x720 on a
  screen 768 high with the taskbar shown then fits -- the most of the
  picture on the screen (rows under a taskbar come back when it hides; the
  frame in the corner would put rows past the screen, black in every
  capture). Never resized (`SWP_NOSIZE`), never a minimised or maximised
  game, nor one whose client covers its screen already (Full Mode, or a
  window put there).
- The move itself: behind `refuse_real_input`, never into a game that does
  not answer `WM_NULL` within 500 ms, asked with `SWP_ASYNCWINDOWPOS` and
  looked at 400 ms later. Pretend hands -- any of them -- move no window but
  by their own `Hands::place`.
- After a crossing that moved it, GoBot's own window is moved beside the
  game (`move_clear_of_game`, on the window's thread) when it now covers
  it.
- The log says where it went: "the game's window moved from x,y to the
  top-left of its screen, x,y", with "(its picture in the corner, its title
  bar above it, off the work area)", "-- the game is still bigger than the
  screen less its taskbar", or "-- but it is at x,y" when so. A player with
  the title bar out of sight moves the game with Alt+Space, Move.

### The shop after a pass, ESC after 10 s stuck, and the game's message after an invite (2026.11.14b)

Since 2026.11.14b (not yet run on the game). The owner's three reports of
2026-11-14, from a live log on the VM:

- *"Khi dọn rác xong thì tắt luôn túi đồ khiến xử lý mua HPMP bị dừng =>
  trước khi thực hiện xử lý mua HPMP cần check xem túi đồ có đang mở không,
  nếu có thì thực hiện luôn, còn không thì mở túi đồ lên và thao tác mua"*.
  At 20:31 the shop round that rode a pass looked a tenth of a second after
  the pass's bag key, still saw the bag, and clicked Khác on a bag no longer
  there. Now a shop round looks twice, 250 ms apart (`shop_bag_ready`): open
  on both, it buys with no key; shut on both, the bag key once -- never
  within 250 ms of a look that saw the bag -- and the bag seen open, and
  again 250 ms later. Every job's end (`close_bag`) presses the bag key only
  on a bag seen, then looks every 80 ms until it is gone, 1.5 s at most
  (30 s since 2026.11.15d), never a second key ("bag still seen N ms after
  its key" when not). Khác is
  clicked only on the bag of the latest look, up to `kShopTabClicks` (4)
  times, the third and fourth 400 ms after a fresh look (since 2026.11.15d
  spread over 30 s from the first, only while the strip reads another tab
  for sure); a pass and a charm
  run keep two. No bag key on a look with a dialog or the check-in board
  come up since the round's first. 21:37's stop -- the game taking no click
  for some 16 s after a failed drag -- is not cured by the extra clicks: a
  round that stops with "không chuyển được sang ngăn Khác" again asks the
  owner for `*_not_in_bin.png` and `*_tab_unsure_shop_other_tab.png`.
- *"Khi thao tác mua nếu không may click vào chỗ khác sẽ khiến không ấn được
  số 00 để mua. Trường hợp bị dừng quá 10s thì tắt toàn bộ hiển thị shop,
  túi đồ trên màn hình bằng phím ESC. Xử lý này thực hiện cho cả khi hộp nhập
  số lượng không biến mất"*, then *"[c]hắc chắn khi dialog mua HPMP hiện ra
  và không click vào đâu nữa thì sẽ có thể ấn được 00 luôn"*: the typing is as
  it was, no click into the box first. A step not seen through in
  `kStuckMs` (10 s; since 2026.11.15d 30 s, `kReactMs`, counted from the
  step's last input of GoBot's own) -- the shop asked for by its right-click, a purchase
  from its double-click and from its question seen, an item from its drag
  towards the bin -- ends with `esc_out`: ESC on every fresh look that shows
  something up (the board, a message, a dialog or the question, the shop,
  the bag), `kEscMax` (6) at most, until nothing is on three looks; one ESC
  that closes nothing is let go, the second ends it. After one that closed
  nothing seen, the next goes only on two looks `kEscSettleMs` (500 ms)
  apart that agree -- a slow game may still be shutting what it took -- and
  the game's menu an ESC brings up anyway is shut by its X (found within
  3 px of where the popup closer has it, looked at again, two clicks at
  most), the sweep going on. The three clear looks are taken even when the
  sweep's time is up, and the round's end looks again after any sweep. The
  bin's question, seen, gets its Hủy Bỏ first. ESC is taken to delete
  nothing (only Xác Nhận deletes) -- not yet seen on the live client; the
  owner is asked to try it once by hand -- and what stays in the bin goes
  back at the next pass. Never in a charm run (no Esc, no Enter), a
  look-only pass, after a stop, with the hold another job's or the guard's
  no -- the guard stops a job for HP only with the character down, and no
  ESC goes over the death panel (`set_hold_check` is the shop round's alone
  again); never on a look that shows nothing or none; once a run. The
  purchase's fix loop and `shop_cancel` stop at 10 s (`kShopStuck`; 30 s
  from the last keys or click since 2026.11.15d, keys typed that show
  nothing watched 30 s before the scan-code retry), and the
  bin is not clicked again after it. A step ends where it is seen through:
  the right-click's when the shop is seen, a purchase's when its question,
  or the full-bag message or a message in its place, is seen gone; and the
  sweep at a round's end goes only with something of the step still up --
  the question, a dialog, a message -- else the shop is shut as ever. The
  round is trouble (10 minutes, as any; 30 s since 2026.11.15b). A full-bag
  message or a notice two
  clicks did not close -- left for the player before -- is shut by the same
  ESCs.
- *"Khi thực hiện auto pt, nếu chẳng may pt trúng người đã vào pt thì sẽ bị
  hiện dialog mà không tự động tắt => chỉ cần ấn enter hoặc click xác nhận
  trong dialog"*: after each invite the party worker watches 2.5 s for the
  game's one-button message (`InviteReply::watch`, `bag::lone_ok` on two
  looks at one place) and clicks its Xác Nhận -- never Enter, which opens
  the chat line -- seen gone on two looks, twice at most (since 2026.11.15d
  the first click watched up to 30 s, `InviteReply::kFirstGoneMs`, the
  second 2.5 s); the player is held
  off 10 minutes (`PartyChat::hold_off`; `InviteReply::kHoldMs`, 30 s since
  2026.11.15b). A message coming up to 30 s later
  ("Đối phương đã hủy yêu cầu tham gia nhóm.") is closed the same way, and
  one up before an invite is closed first. One that two clicks did not
  close -- or whose click was refused, another job holding the game (Tự chat
  typing) -- stops the invites for a minute. The watch's time is counted by
  the clock as well as by its naps, so slow looks on the 1-CPU guest do not
  hold the game's mouse much past it. The log: "Party: invite to X
  answered by the game's message at x,y, ...: "words" -- closed with its
  Xác Nhận".

### One lost key press, pauses instead of "off until the next start", and the way back asked sooner (2026.11.15)

Since 2026.11.15 (not yet run on the game). The owner, the night of
2026-11-14 to 15: *"check log moi nhat, co 1 so loi nhu khi ra ngoai vung thi
khong tu quay vao lai, khi danh quai o duoi nguong px thi khong chiu nhay
xuong hoac nhay len"* -- out of the area it does not walk back in; with
monsters beyond the two bars, above or below, it will not jump up or down. In
his `t60_s1` log (00:01-00:20) the game lost about 30-40% of GoBot's short key
presses at random, most when it ran behind: Ctrl+Up did nothing at 4 of 5 first
tries (the second, 1-2 s on, worked every time), Ctrl+Down at 2 of 6, about
30% of the turn taps. GoBot held every one against the place, the monster and
the key: one Ctrl+Down that did nothing was a no-drop point for the session,
three in a row or five in a run switched drops, or jumps up, off until the next
start -- the way back's and the seek's too.

- **A place is learned at the second.** A press that did nothing, or jumped in
  place, makes `learn_no_drop` only as the second near an earlier one --
  within `GameMapBook::kNoDropXPx` and `kNoDropYPx` (64 and 24 px) of its
  standing point, within `Hunter::kNoDropSuspectMs` (120 s), and with no drop
  that worked there between (`drop_suspects_`, `drop_second_strike`,
  `drop_worked_at`). The first is only noted: "Hunter: drop did nothing in
  1000 ms at x,y -- not held against the place yet (a second near it within
  120 s is); ...". A rise is as it was: learned at once, drops off for the
  run. A jump up that did nothing never learned a point.
- **The first miss passes the monster over 1.2 s.** The first press of a row
  (`drop_fails_row_`, `jump_fails_row_` at 0) that did nothing -- `DropVerdict::
  Nothing`, `JumpUpVerdict::Nothing`, no other verdict -- passes its monster
  over for `kRetryPassOverMs` (1.2 s) in `gave_up_` and, for a jump, in the
  seek's `seek_skip`, so the next look presses again from the same place; the
  second in a row, and any other fail, `2 x give_up_ms` as before.
- **A pause in place of "off until the next start".** `DropOff::Fails` and
  `JumpUpOff::Fails` -- `kDropFailsInRow` (3) in a row, or `kDropFailsPerRun`
  and `kJumpUpFailsPerRun` (5) in the run -- are now a pause of `kFailsCoolMs`
  (30 s, 60 s, 120 s: the first, the second, the third and every later one
  since the last press that worked, or since the start). `cool_drops` and
  `cool_jumps` latch it with `Status::drop_off_until_ms` and
  `jump_off_until_ms`; `cool_downs_end`, first thing in every pass, clears
  the latch, the status and the row of fails when its time is past, and says
  "Hunter: drops on again after 30 s" (`jumps up` likewise). The way back, the
  seek and the marks see the keys able to go again as they see any latch go.
  The other latches -- Physics, MapChanged, KeysStuck, HeldLong, Rose -- are as
  they were, and OffMap's goes with its file after `kDistrustMs` (below); one
  set during a pause replaces it. The plain ↑ past the
  upper bar (`bar_latch_`, 2026.11.12c) is a pause the same way: three in a
  row, or `kBarUpFailsPerRun` (5) with more of them failed than worked
  (`bar_worked_run_`: a flight that worked, that was in the air only, or moved
  onto another floor), pause it for `kFailsCoolMs` (`bar_cool_until_ms_`,
  `bar_cools_`; `end_bar_flight`), and `cool_downs_end` ends it with "Hunter:
  Up for monsters above on again after 30 s"; only a landing outside the
  area still latches it for the run.
- **The run's count weighs what worked.** A fail trips the run's count only
  when `fails_run >= kPerRun && fails_run > worked_run` (`drop_worked_run_`,
  `jump_worked_run_`: a drop that worked, or landed on another floor; a jump
  that worked, or `Moved`; a map change takes a counted press back,
  `PressRecord::worked`): a place where it never works still trips (at its
  fifth fail); a 40% miss rate over a long run trips it only by chance -- about
  4 runs in 9, mostly early on -- a pause at the first fail that finds more
  failed than worked (30 s; longer when no press worked since the last pause),
  and again at each later fail until the ones that worked catch up (a pause
  clears the row of fails, not the run's counts).
- **The cards.** `JumpUpCard::pause_ms` and `SeekAbove::Paused`: the card says
  "Nhảy lên tạm nghỉ thêm N giây ...", the drop's line "Nhảy xuống tạm nghỉ
  thêm N giây ..." -- not in red -- and a pause whose time is past reads as on
  again whatever the hunter last wrote (`jump_up_card` takes its `now` against
  `jump_off_until_ms`).
- **The way back asked again every 3 s.** `kStrandedReaskMs` is 3 s (10 s
  before): stranded on a standing point with no way yet, the map is asked
  again that often. The 8 s stand of 00:05:56 was not this -- the character
  had no standing point then, and nothing is asked without one (see "A fall no
  report follows").
- **Walls the map file holds with no surface.** `parse_map` read only the
  cells that have a surface, so a cell of attribute 4 (GoC's wall), 5 or 6
  (its blocks) with none was invisible -- t60_s1 has 47, t49_s5 30, t53_s2 7
  (3 more of its 10 attr-4 cells hold a surface, and stay floors).
  The owner's log at 00:18:28-00:18:56: a dash (skill 13206) threw the
  character onto the pocket ledge, floor 1035, x 4480..4599, whose left is a
  block of attr 6 (x 4448..4479, y 960..1055); `Page::follow` called x 4480 a
  floor's end, the seek planned "walk left to 4480, off its end onto floor
  1240", and the walk pressed against the wall 15 times over for 12 s (the
  seek's stuck clock is per goal, and the goal switched 9 times).
  `MapFloors::blocks` now holds those cells (one byte a cell; `block_in`; a
  cell with a surface stays a floor, and so does a cell the other layer holds
  a surface of: a layer-1 record under the ground of layer 0 is its fill, not
  a wall -- the review's find on t100_s3 x 1440..1471, 606 such records in 76
  maps, none in the owner's six), and `follow` stops a walk at a wall
  within `GameMapBook::kBodyPx` (48 px) above the feet in the next column --
  `MapTrail::wall`, `wall_x`, `stop` -- and never calls it a ledge. A step of
  the ground (2026.11.12c) is still judged first. Tests: `test_blocks`
  (`map_book_test.cpp`), and the pocket, the block counts of the owner's
  maps, t100_s3's ground and the hillside ledge of t60_s1 in
  `way_back_cover_test.cpp` (skipped where the game's map is not).
- **A fall no report follows.** At 00:05:56 a dash ended in the air: the only
  report was a cast's (0x0031) at 2245,867, on none of the floors, and the
  character landed at 2245,1006 -- standing still sends nothing, so
  `ZoneHere::stand` stayed empty, no way back was asked, and it stood
  "stranded" 8 s until it moved 27 px by itself (the map had a way from there:
  right, one fall, two jumps up). `Hunter::blind_landing`, called from
  `track_zone_way`: the last report is a cast's (`WorldFix::report_op`
  0x0031), not airborne or walking, on no floor or step's lip of the file, and
  `kBlindLandMs` (700 ms) with no other -- the character is taken as standing
  on the first floor under that column, within `kBlindLandMaxPx` (600 px), and
  the way is worked out from there; the first report of its walk puts it
  right. Said once a report: "Hunter: no report of yours since the cast at
  x,y, 0.7 s ago -- on no floor of the file: taken as landed on floor N, M px
  under it". A report of another kind on no floor (a walk's, a jump's) stays
  stranded, as before. A way whose first leg is a jump up or a Ctrl+Down right
  at that column would press from the character's own report -- still the
  cast's, on no floor -- and be refused (OffFloor) again and again, the
  give-up clock running (the review's find, reproduced on t52_s2 and t60_s1):
  `ZoneHere::blind` marks the projection, and while it stands `go_home_zone`
  does not press but walks -- a step of 1 px when it is at the take-off -- and
  the first report of that walk puts the stand right, the press following
  from it. Tests: the blind-landing block of `test_zone_walk_back`
  (`zone_hunt_test.cpp`, its 700 ms pinned by the time of the first Left) and
  `test_jump_blind_landing` (`jump_hunt_test.cpp`).
- **A file distrusted all night.** At 00:27:54 a Ctrl+Up hit at the top of its
  arc sent a 0x002A of state 0 -- a "landing" -- in mid-air at 2956,815 (426
  px up, on no floor); the landing on the planned floor came 450 ms later.
  Judged as the first, the verdict was `JumpUpVerdict::OffMap`:
  `GameMapBook::distrust("t60_s1")` for the rest of the process and
  `JumpUpOff::OffMap` (jumps off until the next start). With the file gone,
  `zone_page` gave nothing, the way back was the map-less walk, and from 00:28
  to 01:10 jumps up were off, the character pacing under monsters it could not
  reach; at 01:10:45 it dashed off the floor's left end into the left trench,
  and from then to 08:07 it stood there (2077,1339 -- 140 px outside its area)
  pressing Right into a wall, "walks back given up on" every 60-120 s, nearly
  seven hours. Now a landing on none of the file's floors
  waits `kLandSettleMs` (900 ms) for the next report -- a jump up takes a
  later 0x002A of state 0 on a floor of the file for the landing, a drop reads
  the fix again -- and stands as before after that (of any kind of report
  since 2026.11.15c, below); a file distrusted
  (`Hunter::distrust_map`, `distrusted_`) is given back after `kDistrustMs`
  (120 s) by `cool_downs_end` -- `GameMapBook::undistrust`, `jump_epoch_` and
  `drop_epoch_` moved, the `OffMap` latches cleared, "Hunter: t60_s1's map
  file trusted again after 120 s ..." -- and at a start; a landing off its
  floors distrusts it again for another 120 s. Test: `test_jump_apex_report`
  (`jump_hunt_test.cpp`).
- **Not done, by the owner's answer: the second dash.** Skill 13206 (the
  owner's key D) carries the character 150-317 px, but `PacketWorld::dash`
  learns ONE dash skill (51203, key V), and only its key (or C, V and Space
  before it is learned; since 2026.11.17 none) is held near an edge; D threw the character twice in
  17 minutes. A per-key "dash" tick or a second learned dash would hold it
  too; the owner (2026-11-15): *"sao lại phải cần GoBot học skill lướt ? chỉ
  đơn giản là ra ngoài vùng thì dừng đánh quái và quay lại thôi mà"* -- the
  rule is to stop fighting outside the area and walk back (`track_back_walk`,
  `kZoneOutsideBackPx`), so the work is on the way back, not on staying in.

The tests are `test_drop_misses_2026_11_15` (`drop_hunt_test.cpp`) and
`test_jump_misses_2026_11_15` (`jump_hunt_test.cpp`), on the pretend game with
its own clock.

### The shop and the party again after 30 s (2026.11.15b)

Since 2026.11.15b (not yet run on the game). The owner, 2026-11-15:
*"về việc thực hiện lại xử lý thì giờ mua HPMP nếu bị kẹt sẽ thực hiện lại
sau 10 phút tương tự với việc tổ đội. Cần thực hiện lại sau 30s chứ không
phải 10 phút"*.

- A shop round that ends in trouble (`bag::ShopEnd::Trouble`: stuck and shut
  with ESC, the Khác tab not switched, a step that did not go) is tried again
  `bag::kShopTroubleAgainMs` (30 s) later, not 10 minutes. The
  `kShopTroublesToDrop`-th trouble in a row dropped what was left of the plan
  until the next until 2026.11.24, so a trouble that stayed did not hold the
  hunt off every 30 s; since then every trouble is tried again 30 s on,
  without limit (*Mua bình and Bùa: tried again, never dropped (2026.11.24)*,
  below).
- A player whose invite the game answered with its one-button message, closed
  by its Xác Nhận, is held off `InviteReply::kHoldMs` (30 s), not 10 minutes
  (`PartyChat::hold_off`). A message two clicks did not close still stops the
  invites for `InviteReply::kStuckMs` (a minute).
- The log: "Trash: shop done -- ...; next part 30 s on" after a trouble, and
  "Party: invite to X ... not invited again for 30 s".

### A report in mid-air is no landing, whatever its kind (2026.11.15c)

Since 2026.11.15c (not yet run on the game). The owner's first live run of
2026.11.15, 10:15:51 (map t60_s1): a Ctrl+Up from 2327,1240 toward floor 908
was judged "landed at 2327,979, on no floor of t60_s1's file -- jumps up OFF
until the next BẮT ĐẦU (or 120 s)", and for 120 s (to 10:17:51) there was no
jump up: two monsters above were given up on. The report taken for the landing
came 478 ms into the flight, 261 px up, and was neither a 0x0031 nor a 0x002A
(both are traced in the log, this one was not): `JumpCheck::flight_of` takes
the first report after the take-off that is neither a cast's nor a 0x002A for
the landing while no 0x002A of state 0 has come. The real landing, "0x002A
state 0 at 2318,908", came at 1221 ms. 2026.11.15's wait of `kLandSettleMs`
(900 ms) covered only an Air-kind "landing".

- `jump_follow`: a "landing" on none of the file's floors -- of any kind of
  report -- waits `kLandSettleMs` from the newest report of the flight that
  puts the character `kLandSamePx` (12 px) or more from it (a cast's too: the
  character is still moving), and no longer than `kJumpUpAirMaxMs` (2.2 s)
  after the key; a later 0x002A of state 0 on a floor of the file is the
  landing. A character quiet for 0.9 s at a place on no floor is judged there
  as before (OffMap, the file distrusted for `kDistrustMs`).
- Tests (`jump_hunt_test.cpp`, `test_jump_mid_air_move`, on `Answer::MoveEarly`
  and `MoveRenewed`): a 0x0027 150 ms into the flight and the landing 0.7 s on
  worked; the landing 1.35 s into the flight with a cast at 700 ms worked;
  the same flight with no cast is OffMap at 1.05 s (the control); a 0x0027 as
  the landing, nothing after, is not judged before 0.9 s and is OffMap after.
  Undone, the Air-only condition fails three of them, the no-renewal one the
  second.
- The second dash skill stays undone, by the owner's answer (above).

### Drops and jumps up tried again at once, never switched off (2026.11.24)

Since 2026.11.24 (not yet run on the game). The owner, on a plan to pause
the drops 90 s: *"không, 2. Nhảy xuống (Ctrl+↓) khi hỏng thì thử lại luôn,
hoặc dừng ra skill rồi nhảy xuống chứ không phải là dừng 90s"* -- a Ctrl+↓
that fails is tried again at once, or after a fight turn, never paused. So
nothing turns a drop, a jump up or the plain ↑ for monsters above off, or
pauses it, any more: no `DropOff`, `JumpUpOff`, `DropWhy::Latched`,
`JumpUpWhy::Latched`, `kFailsCoolMs`, run's counts, `bar_latch_` or
`jump_wall_off_`, no `DropStop` for GoBot's window to stop the hunt on, and
nothing unticked or saved in `hunt.ini`. The ticks alone switch them.

- **A press that failed is tried again.** Its monster is passed over
  `kRetryPassOverMs` (1.2 s) for the first of a row and `kRetryFightMs` (3 s,
  one fight turn: the macro's skills go out meanwhile) for every other, then
  pressed again; the way back's own drop passes nothing over, the way worked
  out again at once, and its jump up that fell back or landed lower is a
  strike of the walk back as before. A drop that did nothing, a jump in place, a rise;
  a jump up that did nothing, fell back, landed lower, higher than its key
  goes or outside the area, a key not taken as its kind, a wall-foot jump that
  landed elsewhere: each is that, and nothing more. Down held over 180 ms, ↑
  held over 250 ms and a g out of range are said in the log, the press judged
  as any other.
- **A place learned is skipped 30 s.** A no-drop point (the second miss near
  one, a rise at once), a take-off a jump fell back, landed lower, higher or
  outside from, a floor taken as solid from below: `Hunter::note_learned` notes it,
  the next try comes from another column, and `forget_learned`, first thing
  in every pass, gives it back `kLearnedForgetMs` (30 s) on --
  `GameMapBook::unlearn_no_drop`, `unlearn_no_jump`, `unlearn_solid`, the way
  back and the seek asked again: "Hunter: the place at x,y on t52_s2 where
  Ctrl+Down did not work is tried again (30 s on)". Up to `kLearnedMax` (64)
  notes, the oldest given back at once to make room; kept across a start.
- **The counts only of what moved.** `kDropsPerMinute` and `kJumpUpsPerMinute`
  (4) count the presses that moved the character: a drop that did nothing or
  jumped in place, a jump up that did nothing or fell back, is taken out of
  the minute again. A monster's 2 presses and 2 settles count over the last
  `kTargetCountMs` (10 s) only (`TargetPresses::fresh`), then it is tried
  again.
- **A landing off the file** lets the hunter's `MapTrust` go of the file
  (`trust_.lose`) until `kTrustPoints` (3) standing points on its floors take
  it again -- no clock; the map book distrusts nothing for the hunter any more
  (until then for `kDistrustMs`, 120 s, the jumps up off until the next start).
- **A map change within `kPressWatchMs` (10 s) of a press** learns both ends
  as gates for the session -- no drop or jump near them again, as before --
  counts `Status::press_gates`, and the hunt goes on where the character came,
  the way back and the plans asked again: `Hunter: MAP CHANGED … -- both ends
  learned as gates (no drop or jump near them again this session); hunting
  goes on, Nhảy xuống stays ticked` (`Nhảy lên` for a jump's). GoBot's window
  takes `Hunter::take_press_gate` once and says it in amber under the tick:
  *Lần nhảy xuống lúc … làm đổi bản đồ — chỗ đó đã được nhớ là cửa, GoBot
  không nhảy ở gần đó nữa và vẫn đánh quái tiếp.*
- **Keys owed** (a ↓, ↑ or Ctrl whose key-up did not go out after
  `kDropOwedPasses` (3) passes): every key is let go of once an episode
  (`release_movement`, the stray drop keys), `Status::drop_keys_stuck` is set,
  and the hunter holds still pass by pass while they are owed -- nothing
  pressed -- then goes on by itself: "Hunter: the owed key-ups went out --
  hunting goes on". The window's own watch lets every key go once too, with
  the amber line *Phím ↓ (↑ for a jump's) hoặc Ctrl có thể đang bị giữ: GoBot
  đã nhả mọi phím; đánh quái tự chạy tiếp khi Windows nhận lệnh nhả phím.*
- **The cards** lose "tạm nghỉ" and "đã tắt": `JumpUpCard` keeps the switch,
  the would-be presses and the counts; `SeekAbove` is `None` or `TickOff`.

The tests are `test_drop_misses_2026_11_15` (`drop_hunt_test.cpp`, its
numbers checked without the game's files too), `test_jump_fails`,
`test_jump_misses_2026_11_15` and `test_jump_off` (`jump_hunt_test.cpp`), the seek's Seek 13 and keys
owed (`seek_hunt_test.cpp`), `drop_key_test.cpp`, `hunt_card_test.cpp` and
`zone_hunt_test.cpp`, on the pretend game with its own clock.

### The way back, the trust steps, a file found wrong, the positions capture: tried again (2026.11.24)

Since 2026.11.24 (not yet run on the game), by the same request (*"Kiểm tra
các phần khác xem có phần nào không làm được thì dừng không, cần thay cơ chế
này bằng 1 cơ chế khác"*) and its correction (retry at once or after a short
gap, never a long pause): four more of the hunter's and the window's
give-ups wait a short while and try again, without end.

- **Walks back given up on** (`Hunter::track_give_up`): a round of
  `kSpotMaxTries` (3) walks back that got nowhere waits the player's own
  `spot_retry_ms` (30 s), then a new round of 3 -- sooner once a report has the
  character elsewhere or the map learned something. Until then four
  `spot_retry_ms` (60-300 s) and one more try. The card says it in amber:
  *Chỗ đánh quái: đi về 3 lần liền không tới được — GoBot thử lại sau 30 giây
  (sớm hơn nếu nhân vật bị đẩy đi chỗ khác). …*
- **The trust steps** (`Hunter::trust_step`): three steps along its own floor
  and still no trust -- a monster standing off the file's floors sets the
  points back -- arm the steps again `kTrustStepsAgainMs` (5 s; 30 s until the review of 2026.11.24) on, under the
  same guards. Until then none until the next map entry.
- **A map file found wrong** (`MapTrust::by_entry`, the hunter's and the map
  tab's alike): two standing points of yours off its floors no longer drop it
  for the whole map entry; `kTrustPoints` (3) standing points on its floors
  gathered since -- yours, and monsters that stand -- take it back
  (`How::Points`), never the entry's spawn; a second mismatch counts them from
  none again. A really wrong file never gathers them.
- **The capture for positions** (`GuiApp::position_reattach`,
  `position_started`, `position_job`): a start the capture refused is tried
  again after 10 s, 20 s, then every 30 s (`GameLink::refused_wait_after`,
  `kRefusedMaxMs`; until then doubling up to 10 minutes), a new game link at
  once; the row ends only once a capture has heard the game's TCP. A read that
  fails (`pcap_next_ex`) marks the capture dead (`PacketCapture::dead`) instead
  of leaving a deaf thread counted as running: a job stops it on the worker,
  and it is started again as after a refusal. Started again for the game's TCP
  gone quiet and still hearing none, it is started again every
  `GameLink::kReattachSilentAgainMs` (30 s) while the game holds a connection,
  the adapter looked up afresh each time (until then once a quiet spell). The
  capture's own error lines are said once an episode -- until a packet is read
  again -- then go to the debug log.
- Unchanged on purpose: stranded with no map file to go by (no blind walk off
  floors into a pit), and the player's own stops.

The tests: `test_spot_give_up` and `test_spot_give_up_wait`
(`hunter_test.cpp`), the wall in `zone_hunt_test.cpp`, `test_trust_steps_again`
and `test_way_back_jump_rounds` (`jump_hunt_test.cpp`: the steps' second round
30 s on, and the way back's Ctrl+Up falling back three times a round, the next
round `spot_retry_ms` on), `test_trust_entry` and `test_trust_found_wrong`
(`map_book_test.cpp`), `test_reattach` (the backoff, `packet_eyes_test.cpp`)
and `test_position_capture_kept` (the wiring, `capture_load_test.cpp`).

### Mua bình and Bùa: tried again, never dropped (2026.11.24)

Since 2026.11.24 (not yet run on the game), by the same request (*"Kiểm tra
các phần khác xem có phần nào không làm được thì dừng không, cần thay cơ chế
này bằng 1 cơ chế khác"*) and its correction (retry at once or after a short
gap, never a long pause): three of the bag jobs' give-ups try again instead.

- **A shop round in trouble** (`bag::ShopEnd::Trouble`) keeps what is owed:
  `bag::shop_owed_after` only counts it (`ShopOwed::troubles`), and the round
  is tried again `bag::kShopTroubleAgainMs` (the owner's 30 s of
  2026.11.15b) later, every time, without limit, until one goes clean or the
  game says no. `ShopOwed::dropped` and `kShopTroublesToDrop` are gone: until
  then the second trouble in a row dropped the rest of the plan until the
  next, an hour on as shipped. A purchase that may have gone through still
  counts as bought (never bought twice), a retry begins no plan of its own
  (`bag::shop_plan_begins` unchanged), and no digit goes through the key
  window for it. A clean round, the bag full or a message reset the count;
  the bag full and a message still end the plan, as the game said no. The
  log: `Trash: shop -- 2 trouble(s) in a row: owed 5+5 kept, tried again 30 s
  on`; the card, from the second in a row: *Mua bình bị lỗi 2 lần liền
  (…lỗi…): GoBot thử lại sau 30 giây.* -- otherwise *Còn N lần, mua tiếp
  sau …*, in seconds under a minute.
- **A charm's message that two clicks did not close** is no longer the
  player's: the run hands it over (`Result::notice_left`, and
  `Result::notice_clicked` since it clicked it), not clicking it a third
  time itself; the next charm run, `kCharmNoticeAgainMs` (a minute) on,
  clicks its own Xác Nhận again (`set_own_notice`, `close_own`: twice at
  most, the second no sooner than `kNoticeRetryMs` after the first, only at
  the place remembered, never a key, never with a two-button dialog up), and
  so on without limit -- each run
  that clicked it renewing the two minutes it is GoBot's. No charm is
  right-clicked while it is up; everything else goes on between the runs
  (each run holds the keys while it watches its clicks, up to `kReactMs`, 30
  s, for the two on a message that stays, as every wait for the game does
  since 2026.11.15d). The words: *thông
  báo của game chưa đóng sau 2 lần bấm Xác Nhận — GoBot bấm lại ở lần dùng
  bùa sau (khoảng 1 phút)*; its picture `charm_notice_stuck` once for the run
  that met it and once a GoBot start for the runs handed it.
- **The bag's strip not read as Tiêu Dùng** in a charm run: tried again a
  minute on (`kCharmTabRetryMs`, the charm job's own gap after a run stopped
  on trouble), not ten -- *…; thử lại sau 1 phút.* A charm not found, or not
  taken, still waits `CharmClock::kRetryMinutes` (10): it cannot come sooner.

The tests: `test_shop_plan` (two and five troubles in a row, unsure
purchases, the resets) and the charm message's case 3 in `test_charm_notice`
(`use_test.cpp`: handed over, clicked twice again in each of two more runs,
then closed), `test_hp_gates` (`pass_keys_test.cpp`), the wiring in
`jobs_gui_test.cpp` and `test_charm_sources` (`main.cpp`).

### Tự chat, the quick quest and the popups: never stopped by themselves (2026.11.24)

Since 2026.11.24 (not yet run on the game), by the same request (*"Kiểm tra
các phần khác xem có phần nào không làm được thì dừng không, cần thay cơ chế
này bằng 1 cơ chế khác"*) and its correction (retry at once or after a short
gap, never a long pause): the three features that switched themselves off
for the session now try again on their own next turn, and tell the phone
once.

- **Tự chat** (`GuiApp::chat_loop`): a line not sent keeps its turn and is
  tried again `kChatRetryMs` (30 s) on, however many fail; the count reaching
  `max_failures` (3) is told once a streak on the tab and the phone --
  `chat_stopped`, its key kept, its tick now *Tự chat gửi không được nhiều
  lần*, its words *Tự chat: 3 lần liền không gửi được câu chat — GoBot vẫn
  thử lại.* -- and a line sent starts the count again
  (`ChatSchedule::sent`). A bar like neither picture after an Enter, or a
  line that would not close after sending (`ChatSender::Result::stop`), no
  longer stops it for the session (`stopped_self` is gone): its doubt holds
  every key until the line is seen shut, as ever, and the next line waits a
  whole line interval from then (`chat_retry_ms`; never under 30 s), as each
  such try may leave the line in doubt again. *Gửi thử* meeting one no longer
  ends the timed run. The sender's words end *— GoBot thử lại ở lượt sau*.
  Only DỪNG, F12, closing GoBot and unticking stop it.
- **The quick quest** (`QuestRun::step`): `max_failures` (3) failed rounds in
  a row are told once a streak -- *Không làm được 3 lần liền (…) — GoBot vẫn
  thử lại mỗi lượt. Xem ảnh trong quest_debug.* and `quest_trouble`, *Làm
  nhiệm vụ nhanh: 3 lần liền … — GoBot vẫn thử lại.* -- and the next round
  stays where `QuestSchedule::done` put it, a whole interval on, as after any
  failed round (faster would open the window and hold every key that much
  more often); a done round starts the count again, and a window GoBot left
  open is still handed to the next round. Its pictures missing
  (`QuestOutcome::NoPictures`) put the next look off `kPostponeMs` (30 s),
  said once until a round finds them, and the worker -- which used not to
  start at all -- loads them again (`QuestPictures::load_best`) before each
  round while they are missing, nothing held or pressed. Only the English
  client, a setting GoBot does not support, still stops it.
- **Tự đóng thông báo** (`PopupCloser::turn`): a popup still up after its
  click or ESC is tried again `kBackoffMs` (30 s) later, every time, as often
  as it takes -- `kTries`, `kTriesWindowMs` and `Turn::LeftAlone` are gone
  (until then two tries in ten minutes, then it was left alone: *Bảng Sự kiện
  và điểm danh vẫn còn sau 2 lần bấm ESC: GoBot để yên, bạn tắt giúp.*). The
  log: `Popups: the check-in board is still up after closing it with ESC
  (try 3); trying again in 30 s`; the tab: *Đã bấm ESC mà bảng Sự kiện và
  điểm danh vẫn còn: thử lại sau 30 giây.* (*Đã thử đóng … mà vẫn còn: thử
  lại sau 30 giây.* for the others). Unchanged: the board's 30 s after the
  arrival and the 00:03 night hold, never its X, no ESC with another popup
  up, every gate and the character-down veto, GoBot's own ESC menu closed by
  its X.

The tests: `test_schedule` and `test_left_open` (`quest_test.cpp`: the alert
once, the rounds going on at the interval, a done round resetting, the
pictures missing waited on and found again, the English client still a
stop), `test_board_menu` and `test_book` (`popup_test.cpp`: tries 3, 4, 5
and on, 30 s apart), the texts in `alerts_test.cpp`, `test_failures`
(`chat_alone_test.cpp`: the chat's turns on a pretend clock) and the wiring
in `jobs_gui_test.cpp`.

### Tự hồi sinh and Tự vào lại game: never stopped by themselves (2026.11.24)

Since 2026.11.24 (not yet run on the game), by the same request (*"Kiểm tra
các phần khác xem có phần nào không làm được thì dừng không, cần thay cơ chế
này bằng 1 cơ chế khác"*) and its correction (retry at once or after a short
gap, never a long pause): the revive and the keeper, the last parts that
stopped the bot or themselves when they could not do something, try again.

- **Tự hồi sinh out of its 3 tries** stops nothing any more. Until then
  `stop_after_revive` stopped every job as F12 does and left the words in red
  until the next BẮT ĐẦU -- the player's older rule, *dừng bot, không hồi sinh
  ở làng*. Now the revive worker keeps its *reviving* hold while the character
  lies dead -- nothing presses a key at the death panel -- and
  `Reviver::kRoundGapMs` (30 s) after a round ran out asks for another
  (`Reviver::again`: three more tries, never the village, never Cash), without
  end, until the character is up -- by a revive of its own or by hand; then
  everything goes on by itself. The out-of-tries hook is still called once a
  death, so the phone hears `revive_failed` once a death; the card says it in
  amber with the seconds left. The words: *Hồi sinh tại chỗ 3 lần chưa được —
  có thể đã hết giấy/bùa hồi sinh. GoBot không hồi sinh tại làng; thử lại sau
  30 giây. Hồi sinh bằng tay thì bot tự chạy tiếp.* The log: `Revive: out of
  tries (round 1) -- nothing stopped: …` and `Revive: round 2 at this death:
  another 3 tries -- still never the village, never Cash`.
- **max_restarts counts over an hour** (`CrashWatch::restart_allowed`,
  `kRestartWindowMs`): at most 5 restarts (as shipped) in any hour, the
  stall watchdog's and a crossing's ending of a stuck client counted as a
  crash's. One more waits -- nothing ended or clicked; a stop ends the wait,
  and the login key has the game looked at again at once -- until the oldest
  is an hour old, then the keeper goes on; said once a wait and told to the
  phone as `keeper_gave_up`, its tick now *Tạm nghỉ tự vào lại*: *Đã vào lại
  game 5 lần trong 1 giờ — GoBot chờ tới hh:mm:ss rồi tự vào lại.* A crossing
  that has to end its client with the hour used up is
  cancelled, the game left as it is, and made again once one more is allowed
  (`Crossed::Wait`). Until then the keeper stopped for a person after
  max_restarts in a run (`gobot_cli --keep`, a tool for a person at the PC,
  still does). `back_in` says *Đã vào lại game (lần N trong 1 giờ qua). Bot
  chạy tiếp.*, and the disconnect alert goes once a trouble, not again after
  a wait.
- **A crossing that does not get in** (`Crossed::Retry`) -- every attempt
  used on a page that never came, the login not set up, a picture missing,
  VTCZone not where login.ini says, VTCZone's sign-in form, the launcher's
  Security Alert, a point off its window -- is made again
  `CrashWatch::kCrossAgainMs` (30 s) on, without end, login.ini and the
  pictures read afresh, from wherever the game is. The sign-in form and the
  Security Alert are never clicked or typed into: the next round goes through
  once a person has answered them (their words end *— GoBot tự thử lại sau
  đó.*, no longer *rồi bấm BẮT ĐẦU lại* -- with the run on, that button says
  DỪNG LẠI). The keeper's line, in amber: *Vào lại game chưa được (VTCZone
  đang hiện màn hình đăng nhập) — GoBot thử lại sau 30 giây.*, with the whole
  reason in red under it until back in; `keeper_gave_up` once an episode:
  *Vào lại game chưa được: đã thử 3 lần (…) — GoBot thử lại sau 30 giây.* The
  login key's own crossing is asked again the same way, unless the key is
  pressed again meanwhile. Only GoBot not running as administrator still
  stops the keeper (`Crossed::NotElevated`): only starting it again as
  administrator puts that right.
- **The game's window after a log in**, not found at the first look
  (`reattach_game`): looked for again every `kReattachAgainMs` (5 s) while
  the keeper runs, and the look that finds it does all a log in does -- the
  capture's where, the Bản đồ tab's map let go, every buff cast again, the
  hunting spot set aside, a cold start's jobs started. Until then a relogin's
  window missed once was looked for no more.

The tests: the rounds and the hook once a death in `test_reviver`, the
rolling hour's cases in `test_crash_watch`, the window's wiring in
`test_gui_stop` (`main.cpp`), the texts in `alerts_test.cpp`, the watchdog's
restarts in the hour in `stall_test.cpp`, and the wiring and these notes in
`jobs_gui_test.cpp`.

### Check-in board closed even after a missed look (2026.11.17c)

Since 2026.11.17c (not yet run on the game). The owner's report: *"check lại
phần tắt thông báo, có bảng điểm danh hiển thị mà không thấy tự tắt"* -- the
"Sự kiện và điểm danh" board stayed up with "Tự đóng thông báo" on.

The log of that run (2026-11-17, 21:36): the keeper back in the world at
21:36:16.977, the board seen at 73,34 at 21:36:17.139, its ESC due 30 s on; at
21:36:55.449 the same board said as newly up, its 30 s started over, and the
run stopped at 21:37:09 before they ran out. No line said why no ESC went at
21:36:47 -- nothing blocked, gated, refused or pressed: a look in between had
missed the board, and a miss reset its first sight (`Track::since` in
`PopupCloser::turn`). The worker's own frames (popup_debug, 21:36:17 and
21:36:32, read off the screen: PrintWindow gave blank frames) show how a look
misses it: the game draws its lines of news ("Khôi phục năng lực Quỷ Hồn.",
the clan's guards) and the map's name over the board, the news just under its
title bar. Drawn 22 to 45 px higher, as more lines push them up -- the buffs
re-cast at 21:36:44-49 bring such lines -- they bring the title from 1.00 to
0.89-0.71, under `kCheckinBoardScore` (0.90). The look under the hold, the
same search round the title, misses it the same way, and returned "nothing"
without a word.

- **The first sight kept.** A board seen again where it was within
  `PopupCloser::kBoardGapMs` (15 s) of the first look that missed it is the
  same board (a while with no look at all, the game behind another window, is
  not counted): its
  30 s still count from the arrival or its first sight, and what was said
  about it is not said again. Said once a sighting:
  `Popups: the check-in board is up again at X,Y after N ms out of sight
  (...): the same board -- its wait still counts from its first sight, N s ago`.
  A board closed, one out of sight for over 15 s, or one seen after a new
  arrival in the world starts afresh as before; the night hold (23:55 to
  00:03) is as it was. Kept short because a look cannot tell a board it
  missed from one the player closed with its X: one the player opens again
  later to do the check-in is a new board, with its own 30 s to be read.
  Even when due, a board seen again after a miss gets no ESC until it has
  been back `kSteadyMs` (3 s): the look that found it again came after looks
  that saw nothing, so most often without GoC's picture of the monster book,
  and a book opened meanwhile would have taken the ESC.
- **The board under the news.** Where the board was seen whole in the last
  60 s (`PopupCloser::kBoardGraceMs`), its title is also taken at
  `kCheckinBoardCoveredScore` (0.60), within
  3 px of where it was (`PopupEyes::board_covered_at`): on the turn's look, on
  the look under the hold and in the watch after its ESC, so that a covered
  board is not counted closed. Never for a first sight. With no board up, the
  title scores 0.44 at most on the saved frames. Said once:
  `Popups: the check-in board's title at X,Y is partly covered (S, the game's
  lines of news over it?): still taken as up`.
- **Every reason for no ESC said**, once a sighting: not found again under the
  hold (`Popups: the check-in board is due its ESC, but the look under the hold
  did not find it again round X,Y, whole or covered: nothing pressed; looked
  for again on the next turn` -- the turn is now "held", looked at again 1 s
  later, not "nothing" 2 s later; a second miss in a row puts it off 5 s, so
  that no hold is taken every second for nothing), no picture under the hold,
  another popup of
  the list up beside it (`Popups: the check-in board is due its ESC, but
  <popup> is up too and an ESC would go to it first: no ESC while it is`). A
  gate (a hold on, the trash cleaner, the HUD not up...) puts the due board off
  `kBlockedMs` (5 s), not `kBackoffMs` (30 s), each reason said once; so does
  the hold refused as it is taken (another job's hold or the party job's click
  on the mouse just then, a key under way, the player's hand), said once as
  `Popups: not closing the check-in board now: not the moment`; the game
  behind another window for 10 s with the board up is said once
  (`Popups: the check-in board is up and gets no ESC: the game has not been in
  front for 10 s -- nothing is looked at or pressed until it is`).
- Unchanged: no ESC while another window or popup that ESC would reach first
  is up, nor under the trash, shop or other holds, nor while the player types;
  the board is never clicked; two ESCs in 10 minutes, then it is left alone.
- Tests (`popup_test.cpp`, `test_board_missed_look`, `test_board_live_frame`):
  a board missed at 20 s gets its ESC 30 s after the arrival; out of sight
  over 15 s, after a new arrival, or closed and up again: its own 30 s; closed
  by the player at 8 s and opened again at 25 s: no ESC before 55 s; due while
  out of sight and seen again at 33 s: the ESC at 36 s, and none at all with
  the book opened meanwhile beside it; a gate over at 41 s: the ESC by 46 s,
  said once; the hold refused until 32 s: the ESC at 35 s, said once; missed
  twice under the hold: said once, put off 5 s, then the ESC; the book without
  its X beside it: said once.
  On the saved frame of 21:36:32
  (`build\handoff\popups\testdata\checkin_live_1024x768.png`, skipped when
  absent): found whole by both looks;
  with the news drawn over its title, missed as a first sight but told
  covered; the ESC at 30 s through a covered look and a missed one; an ESC the
  game ignores while it is covered leaves it "still up", not closed. With
  2026.11.17b's `turn()` seven of them fail.

### Party: inviting somebody already in a party (2026.11.17b)

Since 2026.11.17b (not yet run on the game). The owner's report: *"Hiện tại
có bug khi pt người đã có pt thì hiển thị thông báo sau đó bot không phản ứng
gì nữa và cứ mở khung pt."* -- inviting somebody already in a party brought a
notice, then the bot seemed to stand still and kept opening the player list.
Two ways it could happen: the notice not recognised (another shape, later, or
under the list), so it stayed up and the asker was never held off -- every new
keyword line opened the list and invited again; or recognised and not closed
by the click, the game's mouse held up to `kReactMs` (30 s) each time while
the other jobs waited. No log and no picture of that notice exist yet, so the
fix does not depend on recognising it. The owner chose *"ok làm A B C đi"*:

- **A -- hold off by what the invite led to** (`InviteTries`,
  `party_invite.hpp`). Every invite (sent or not: Req Party may have been
  clicked) is judged `kJoinWaitMs` (15 s) later on a later pass's party HUD
  (`GuiApp::party_read`), never waited for: one member's panel more than the
  HUD showed before the invite (`PartyHud::size`) is a join. The HUD cannot
  name who joined, so two invites out at once share it, the first invited
  first -- but an earlier invitee who had joined before a later invite's HUD
  was read is already in that invite's "before" and is not taken off it a
  second time. A HUD unread then or before counts as no join; a look from
  before GoBot's last change to the party waits up to `kStaleWaitMs` (10 s)
  more.
  The game's message after the invite counts at once. No join holds the
  player off `kFailHoldMs` (1 min), `kFailHoldLongMs` (10 min) from the
  `kFailsForLong`-th (2) in a row; a join resets the count. The hold goes to
  `PartyChat::hold_off` too, so `saw()` does not give the name back, and the
  worker checks `tries.held()` before `ensure_open` -- a held-off player never
  opens the list. An asker not on the list was sent no invite and is no miss:
  only held off in the chat for `kNotListedHoldMs` (60 s), the same every
  time, never counted towards the 10 minutes. 2026.11.15b's 30-s
  `InviteReply::kHoldMs` is gone.
- **B -- the notice closed more surely, the mouse never held long.** Every
  watch of the party worker's (after an invite, the late look, a message up
  before an invite) is `InviteReply::watch(..., kCloseHoldMs, 1)`: one click
  on the lone Xác Nhận (`bag::lone_ok`, anywhere on the frame, never a box
  with two buttons, never Enter or ESC) and at most `kCloseHoldMs` (2.5 s)
  with the mouse held after it. Still up: the mouse let go, clicked again on a
  later pass `kRetryGapMs` (1 s) on at least, `kCloseTries` (3) clicks in all,
  then no invite for `kStuckMs` (60 s), said once on the tab. While it waits
  for its next click nobody is counted as asking, and one left up by an
  asker's turn ends that pass's other askers too, so its clicks are never
  spent at once. A message already up before an invite names no player on
  the tab ("Thông báo có sẵn: ..."). `kFirstGoneMs` (`kReactMs`)
  stays the default for any other caller.
- **C -- the list shut after every turn** (`PlayerList::close`). Once the
  worker opened it (`opened_it()`) or left it open before, after the asker's
  turn -- invited, not on the list, a message in the way -- it is shut by the
  same button that opens it, `player_in_map_icon`, found over the whole frame
  as `ensure_open` finds it (956,139 at 1024x768, 1212,139 at 1280x720; no
  1366x768 capture yet). Not up on two looks `kCloseSettleMs` (300 ms) apart:
  nothing pressed -- a good invite shuts it by itself, and the button flips
  it. Else one press, looked at every 250 ms up to `kCloseWaitMs` (3 s); never
  a second press, never ESC. Not shut: `Party: the player list did not shut
  after <who>'s turn -- <why>; left as it is`, once until a close works.
- Log lines: `Party: invite to <who> did not lead to a join (<why>) -- N in a
  row; not invited again for N s`, `Party: <who> joined after the invite --
  ...`, `Party: <who> asked; held off N s more after an invite that did not
  lead to a join -- the list not opened`, `Party: the game's message after
  the invite to <who> at x,y, ...`, `Party: the game's message is still up
  after N clicks on its Xác Nhận -- no invite for 60 s`.
- Tests (`party_hud_test.cpp`): `test_invite_tries` (holds, the count, a join,
  a message at once, an unread or stale HUD, two invites at once, a name read
  differently), `test_reply_short_hold` (one click, 2.5 s, on a slow guest
  too), `test_list_close` (one press at each of the three sizes, none when
  shut or shutting by itself, given up after 3 s, a stop), and the worker's
  source checked for the order of all this.
- Still wanted: `gobot_gui.log` with its `Party:` lines from a run where the
  bug shows, and a screenshot of the "already in a party" notice.

### MP bar and skills while hunting (2026.11.17)

Since 2026.11.17 (not yet run on the game). The owner, after a run of
2026.11.16b on 2026-11-17 (hunting 23:45-00:23, then the macro alone,
standing, to 00:39): *"check lại log mới nhất nhé, tìm thanh MP lỗi, skill ra
không đồng đều"*. Two faults, two changes.

**The MP bar.** Standing, the MP potion G (under 62%) went 58-59 times a
minute while MP was 92-100%. The frame of 00:23:27 shows why: the EXP bar's
animated glowing tip, where its fill ends, sat over the left digits of MP's
figure, and "15542/16813" read `542/16813` (3%); once those figures were
forgotten, the fill measured where the caption begins, about 57%, still under
62. The packets had MP right all along, but until 2026.11.16b nothing went by
them before a gate had compared them with the bars, read by read: HP passed it
(the packets from 00:24:10), MP never did -- with the skills cast one after
another MP moved at almost every read, and only reads with MP still were
judged. The owner's call: *"không mình muốn đổi sang theo phương án chỉ dùng
gói tin"* -- HP and MP by the packets, with no check against the screen; the
bars only when the packets do not have them.

- **The packets whenever they have it** (`vitals_usable`,
  `GuiApp::gate_vitals`): a bar's packet value goes at once when `[Packets]
  vitals` is not `off`, the capture is GoBot's own on the game window's
  process, of the same start, the value of the current epoch, the server heard
  within `kLinkQuietMs` (5 s) and a maximum known. Otherwise the screen: no
  Npcap or capture, a gap or a restart of the capture (until its first value),
  the server quiet for 5 s. A map load does not switch to the screen: the
  packets' last values carry across it, the HUD is out while it loads (HP
  unread, out of the world) and the macro waits for the load
  (`map_load_holds`, at most `kLoadHoldMs`); the server's entry burst brings
  `0x0051`/`0x0050` again. There is no learning, doubt or distrust any more
  -- `BarCheck`, `HandOver` and the 20 agreements are deleted -- and the
  first usable value after a gap goes straight back to the packets. The
  pet's HP, whether the character is in the world, and the death panel and
  the revive stay the screen's; a screen HP of 0 is not taken for a death
  while the packets say HP is above 0 (`death_doubt_now`, as since
  2026.11.10). `vitals=off` is the screen, as before.
- **0x02F0** keeps MP up in a fight: its MP is yours once a `0x0051`/`0x0050`
  has given HP, the newer of the two winning (`PacketSelf`); before any HP
  frame it is for the log only.
- **The switches**, each way at most once a minute a bar (`kSwitchGapMs`),
  the rest counted on the next line (capture gaps flipped a still HP every
  ~10 s on 2026-11-17): `Vitals: MP from the screen until the packets have it
  again -- <why>` and `Vitals: MP from the packets -- the potions, the jobs'
  HP guards and the pace`.
- **For the log only** (`BarCompare`): each read's own figures are set
  beside every packet value from `kBeforeMs` (400 ms) before it to `kLeadMs`
  (900 ms) after, and called *same*, *short* (digits lost:
  `figure_lost_a_digit`, `figure_lost_leading_digits`) or *other*. Nothing
  acts on it. The minute's line, in place of `Vitals check: …`: `Vitals: from
  the packets HP 100% MP 100% of the minute (to and from the screen: HP 0, MP
  0); the bars' figures beside them, for the log: HP same N, short N, other N;
  MP …`, then the last read not the same, the reader's pace and the frames.
  `Vitals: MP disagreement …`, `… from the packets now -- N agreements` and
  `distrusted` are gone. Should the server ever send HP or MP otherwise,
  GoBot will not notice by itself: the *other* counts are where it shows.
- **Covered figures never read as low** (`VitalsReader::figure_behind_fill`),
  with or without packets: figures with fewer digits than their maximum and
  more than `kBehindFillPct` (5) points under the bar's measured fill are
  refused, as a hole is, the kept figures standing in -- `Vitals: MP figures
  542/16813 (3%) refused -- the bar is filled to 57%: its first digits covered
  (the EXP bar's tip?)`, once a minute a bar, counted on the minute's line.
  Once the kept figures are forgotten, an MP fill measured at
  `kCaptionShadowPct` (54%) or more reads as nothing, not 57%: without
  packets an MP potion goes only once the fill shows under about 54%. HP is
  never made unread that way (an unread HP is out of the world); the tip is
  about 20 px from HP's caption.
- **The potions' brake** (`kBrakeAfterChecks`, 3): a potion whose reading
  has not risen after 3 presses in a row goes at its own cooldown from then
  on -- no early re-press, no `potion_urgent_ms` -- with `Potion check: MP
  key G pressed 3 times and the reading never rose (3%) -- the bar may be
  misread, or the potion does nothing: at its own N ms cooldown until it
  rises`. Lifted, with `… its quicker presses back`, by a rise, by HP falling
  further (a fight being lost), by a reading over the mark, or by none; a
  stop of the macro clears it too, since nothing keeps it up to date then,
  so a bag or charm job's own potion after the stop keeps its urgent second.
- **Where it shows:** the HP / MP tab and the black box, `…; gone by: HP yes,
  MP no -- <why>`; the simple screen's checklist, *Đọc được HP; MP theo gói
  tin (số MP trên thanh đang bị che)*, or without packets *Đọc được HP; số MP
  đang bị che (đầu thanh EXP) -- chờ*. `gobot_cli`'s HUD read gives the same
  verdict.
- Tests: `vitals_trust_test.cpp` (the comparison, the switches, the EXP tip
  synthetic and on a crop of the 00:23:27 frame at 1024x768, 1280x720 and
  1366x768: MP never under 50%, then unread, HP 92.88%), `packet_self_test.cpp`
  (0x02F0), `macro_fight_test.cpp` (the brake, HP falling under it, and a
  stop clearing it).

**The skills.** Hunting, Z, X, B, A and D were cast 14-16 times a minute,
C and V once (`Paced: … per key pressed/cast: … C 1/1, V 1/1, …`). Four
causes in the log:

1. **A guessed dash.** With no dash learned -- and the learned one was
   dropped twice, `Positions: the dash is no longer skill 13206 …` -- C, V
   and Space were all taken for the dash's key and passed over near an edge,
   which the hunt was nearly always near.
2. **Floors of 20-70 s** learned for skills cast again every 3 s (C 33.0 s,
   X 70.9 s, D 70.3 s): skips just after a hold, a landing or a turn, casts
   by hand that started a cooldown the rotation did not know of, a pile of
   monsters -- and their check came after `kPaceFloorUses` (20) turns, never
   in time once the round had shrunk.
3. **The timers flooding**: with the character dead (HP 0/15625 on the
   screen at 00:07:17, then the map reloaded, to 00:09) no cast of yours came
   for 2 s and the keys went to the timers for 25.8 s and 29.0 s of those
   minutes, 220-230 presses a minute. The 169 of the minute to 00:22:55 were
   not that: its drops to the timers were short (0.3-1.9 s, 4.9 s in all,
   HP from the packets at 64% or more and the fight going on); its presses
   were mostly re-presses (62) of X and D against floors of 70 s -- cause 2.
4. **A buff's burst** logged as *4 pressed, 2 cast*: the next skill's cast
   was paired with the buff key.

What changed (`macro_pace.cpp`):

- **No guessed dash key** (the least risky of the diagnosis's options; no
  per-skill dash learning, by the owner's answer of 2026-11-15):
  `Hunter::dash_keys()` is the learned key alone, none until one is learned,
  so only a learned dash's key is held near an edge; `kDashGuessKeys` is
  deleted and `Hunter::kDashGuessPx` (320) is only the length of a learned
  dash with none measured. A dash out of the area is the walk back's, as the
  owner wants. `Dash (packets): … the dash not learned yet -- no key held for
  it`.
- **Floors bounded.** A cast of a key's skill by anyone starts that key's
  cooldown (`pace_stamp`), and each key keeps the shortest gap its skill was
  ever cast again after (`min_recast_ms`). A floor is never above that, nor
  `kPaceFloorMaxMs` (12 s), and is raised by half and a lock at most. A skip
  teaches nothing when it is a probe's, within `kPaceSettleMs` (1 s) of a
  hold's, fallback's or probe's end or a start, after the key's turn waited,
  with another's cast given back that turn, or when the key's last cast is
  already as long ago as its cap. A floor is checked after
  `kPaceFloorCheckTurns` (3) turns passed over, the gap doubling after each
  check not cast, up to `kPaceFloorUses` (20). `pace_arm` clears floors at
  every start, keeping each key's learned skill.
- **Probing, not flooding.** No cast of yours for `kPaceStaleMs` (2 s) while
  the packets still come (`MacroEngine::note_packets`): the rotation stays
  and presses one key every `kPaceProbeMs` (1 s), nothing learned, until your
  next cast -- `Paced: no cast of yours for … the packets coming -- the game
  takes no key: one key a second until your next cast`, then `Paced: your
  cast came after … of the game taking no key -- …`. The packets quiet too
  -- once casts were seen this run -- the timers, but skill presses at least
  a lock apart and each key again after (rotation keys × lock) at least
  (`fallback_spacing_ms`): about 85 a minute, not 220-230. With no cast ever
  seen (no capture), the timers as before.
- **Held while down** (`skills_down`): HP under 0.5% for `kSkillsDownMs`
  (800 ms) holds every skill key, following the fight or not, paced or on
  the timers; potions, buffs and `always_timer` keys still go. It goes by the
  HP GoBot goes by -- the packets' while they have it. In the 00:07 death the
  packets kept HP at 27% while the screen read 0, so there it is the probe's
  one key a second that keeps the presses down, until a gap or a quiet server
  hands HP to the screen. A screen 0 alone never holds the skills while the
  packets say more: the false death of 2026-11-10 was the screen reading 0
  with the packets at 14585/14585.
- **The swing never relearns a key**: a skill another key of the rotation is
  known to cast is never this key's (`pace_ours`), and the hunter stamps its
  swing before the press (`Hunter::last_swing_ms`), so a swing a moment ahead
  of the rotation is told apart.
- **One buff press per cast** while paced: the buff's own cast seen ends the
  rest of its burst (`pace_buff_cast`); presses really lost still get up to
  `buff_presses`. `CastMatch` credits one cast per burst, a kind-2 first.
- **The log:** the minute's `Paced:` line ends, after the 2026.11.16b part,
  `; skips taught N, not taught: N after a hold or wait, N beside another's
  cast, N longer than its re-cast, N probing; floors capped N; probing N.N s;
  held dead N.N s; buff presses spared N`, and a key's floor in it shows `,
  re-cast N.N s`.
- Tests (`cast_pace_test.cpp`): casts by hand for 2 minutes (the longest
  floor 2.31 s, every key 26-27 casts); Z refused for 25 s (its floor 2.34 s,
  cast 1.6 s after); holds then refusals (no floor); the owner's hunt with no
  dash learned, near an edge all the time (each of Z X C V B A D 11-12 casts
  a minute); no flood (probing, HP at 0, the capture gone: 86 presses a
  minute); the arm clearing floors; a check under its floor refused teaching
  nothing, so the gap really doubles (S on a 10-s cooldown beside Z: 3 wasted
  presses in a minute, 18 when every check reset it to 3 turns).
  `cast_match_test.cpp`: one cast per buff burst.

Risks: a player whose dash really is C, V or Space has no edge hold until it
is learned (the walk back covers it); with the capture broken after casts
were seen, the skills go at about the lock's rate, not each key's interval;
a key with a real cooldown over 12 s costs a press or two a cycle.

### Skills in turn while hunting too (2026.11.16b)

Since 2026.11.16b (not yet run on the game). The owner: *"Hiện tại dùng skill
đang không được đều, cần chỉnh lại"* -- both while **Tự đánh quái** hunts and
standing with only the timed keys: pauses of 1-2 s with no skill (*"Khựng
giữa các chiêu"*), and the long-wait keys rarely cast while the short ones
went all the time (*"Có chiêu ít ra"*). The hunt of 2026-11-15 13:31: Z, X and
C each pressed about 90 times a minute for 14-17 casts each, cast gaps p50
0.84 s, p90 1.38 s, the longest 2.25 s -- every key on its own timer, the
hunter swinging its attack key Z every 700 ms on top of the macro's Z, and the
game dropping every key pressed inside the last cast's lock. The owner chose
*"Ra chiêu theo vòng cả khi đánh quái"*, on by default.

- **On by default** (`MacroConfig::cast_paced`): in code, in the shipped
  `configs/macro.ini`, and for every existing user. Since 2026.11.16b the
  switch is read from `cast_paced_v2` (`MacroConfig::kCastPacedKey`); a file
  without it was written before and reads on, whatever its `cast_paced` says
  -- that `false` was 2026.11.08's default, written back by every save, not a
  choice. Saves write both keys, the same, so an untick from now on is kept
  across restarts and an older GoBot still reads `cast_paced`.
- **The tick** *Ra chiêu theo vòng* is on the simple screen's **Tự dùng kỹ
  năng** row itself, on a line of its own right below *Tốc độ bấm kỹ năng*,
  with a short tooltip -- the
  same switch as the **Kỹ năng** window's *Ra chiêu theo vòng (chờ game ra
  chiêu rồi mới bấm chiêu kế)* and the full screen's **Timed keys** one.
- **With the hunter** (`MacroEngine::pace_pass`): the hunter running no
  longer turns the rotation off, nor do the skills following the fight
  (`skills_follow_fight`): paced, `run_paced` has the skill keys and
  `run_skills` only the timers' fallback. Following the fight, a key on
  *Luôn bấm theo giờ* (`always_timer`) stays on its own timer, waiting out a
  cast's lock as a buff does -- and, like a buff, the cast of the buff or
  own-timer key pressed just before it (`pace_buff_ms_`, until that cast
  shows or `kPaceConfirmMs` passes): two of them due together after a hold
  went back to back, the second dropped inside the first's lock. Only a key
  sequence with a key in it keeps the rotation off (`PaceStatus::others`).
- **The holds** (`run_paced`): walking back (`Fight::Mode::Returning`), the
  hunter's moments of danger (`Fight::Mode::Held`: turning, a drop, nothing
  to fight, outside the area, a seek leg, a jump up) and, following the
  fight, no fight, MP under the floor and the character down
  (`skill_gate`): nothing of the rotation goes out, it waits at the key whose
  turn it is, and goes on from it once free -- one press at a time, never a
  burst. A press waiting for its cast when a hold comes keeps its window
  (`kPaceConfirmMs`); with no cast it is given back, no try and nothing
  learned. Counted as the timers count their holds, a look every 100 ms, on
  the minute's `Skills:` lines. A key whose target is past its `range_px`, or
  the dash's key near an edge, passes its turn (`PaceStatus::Turn::Waiting`);
  so, following the fight, does the hunter's attack key while the target is
  further than the hunter lets its own swing of it go (`Fight::attack_far`,
  `Hunter::attack_far`: `attack_limit_px`, F3) -- before, only the hunter
  pressed that key then, never beyond that reach.
- **The hunter's swing** (R2): the hunter's attack key, when it is a key of
  the rotation too (the owner's `attack_key=Z`), is the rotation's: while it
  paces under no hold, `MacroEngine::swing_yields` tells the hunter
  (`Hunter::set_swing_yield`) to let the swing be -- counted as a swing let
  be for the no-damage give-up, as one held for the key's reach is. The same
  while any key of the rotation is ready or waits for its cast: the swing
  would take that key's slot. With every key on its interval or cooldown the
  hunter swings in the gap; on the timers or off, it swings as ever, and so
  under the holds where its swing is wanted (walking back, MP under the
  floor, the character down, a drop, a jump's flight, a seek leg, outside the
  area). Under a turn not yet confirmed, nothing to fight and, following the
  fight, no fight, the hand-off is kept: the hunter ends those and swings in
  the same pass of its own, before the macro has seen the change, and that
  first swing of every fight and after every turn took the rotation's slot.
  This was chosen over leaving the attack key out of the rotation (as
  `run_skills` does): out of it, the hunter's 700-ms swing of Z would still
  be the one skill cast all the time -- the owner's complaint. A cast that
  is not the waited key's own -- a swing on the hunter's thread a moment
  ahead of the rotation, a key by hand -- is told apart (`pace_ours`): by
  the skill each key was seen to cast, `kPaceSkillLearn` (3) casts in a row
  with no swing near (learned afresh after `kPaceSkillUnlearn` (3) casts of
  another skill), and, not known yet, by the swing's key-down
  (`Fight::swing_ms`, `GuiApp::more_note_attack`) within `kPaceConfirmMs`
  before the press. Such a cast gives the press back: the key goes again
  after that cast's lock, `kPaceGiveBackMost` (2) times a turn at most,
  then its turn is over with no cast. Taken for the key's, it passed the
  key's turn with the key never cast.
- **No key starved** (R3): a key's interval is only its least time from its
  own last cast, and a key not due, or under a cooldown learned for it,
  passes its turn at once. The owner-like set (Z 0.5 s; X C V B Space 1 s;
  A S 3 s) casts every key once a round, 10-11 times each a minute in the
  test; keys of 10 s and 30 s beside three short ones are cast every
  10.1-12.5 s and 30.8-31.3 s.
- **No idle gap** (R4): the next key goes at the lock's end; a press that
  met another key going (`Refused::Busy`: the hunter's arrows and swing take
  the keys all the time) is tried again after `kSkillRetryMs` (30 ms), not
  100. Measured in `cast_pace_test.cpp`: the longest cast-to-cast gap 740 ms
  with no press lost, 900 ms with a first press lost, 1120 ms with both of a
  key's lost.
- **No packets** (R8): no cast of yours seen, every key on its own timer as
  before, `run_skills` following the fight, the hunter swinging its own key.
  Potions first as ever; a buff due inside a lock goes at its end.
- **The log** (R10): `Paced: on, with the hunter -- …` and `Paced: the hunter
  runs -- the rotation goes on beside it, under its holds`; the minute's line
  ends `…; other casts 0; now paced with the hunter; given back N; held N.N
  s; per key pressed/cast: Z 9/9, X 9/8, …` -- each key's presses beside its
  casts. `Casts (packets):` has each key's presses and casts as before.
  `pace_status_vi`: *Đang theo vòng cùng Tự đánh quái — khóa chiêu ước 680
  ms, 12 vòng.*
- Tests (`cast_pace_test.cpp`): the round while hunting, both keys, the
  swing's and a hand's casts, every hold, long keys, the worst gap, the
  skills following the fight, no casts, potions, and the migration.

### Lighter on weak PCs, and old features removed (2026.11.16)

Since 2026.11.16 (not yet run on the game). A player's PC -- one CPU core,
3.5 GB of RAM, the game running on it too -- stutters while GoBot runs. The
owner asked for GoBot to be made lighter, and for the old features nobody
uses any more to be deleted, without asking about each. Nothing on the simple
screen changed but one warning (below), and what a ticked box does is the
same; what changed is how often GoBot looks, and how much it does per look.

What was measured, on the owner's 20-CPU PC (the log of 2026-11-15, hunting
with the macro, *Tự đóng thông báo* and the client keeper on): about 11.5 s
of CPU a minute, and about 8.5 grabs of the whole screen a second. The survey
behind this release estimated about 4 s a minute if all its proposals were
done -- an estimate, not a measurement, and it counted one change this
release does not make (grabbing only the HUD for HP and MP), so expect less
saving than that. The `Load:` line (*How hard it works the PC*, below) is
what to compare before and after.

**Fewer looks at the screen.**

- *Tự đóng thông báo* (`PopupCloser::kIdleLookMs`): after a look that found
  nothing, the next comes 2 s later (3 s in light mode,
  `kIdleLookLightMs`) instead of 1 s; after any other outcome -- a popup
  waiting out its 3 s, a hold, a block, a close -- 1 s as before. The 3 s a
  popup must stay put, and the check-in board's 30 s after a login and its
  23:55-00:03 hold, are as before; a popup is only first seen up to 2 s (3 s)
  later. The search for the GoC book's key, the costliest of its pictures,
  goes on every fifth look that finds nothing, or at once when the book's
  title is found without its X. It no longer copies the frame (3 MB) on
  every grab: the HUD check reads the closer's own frame.
- *The name-plate reader's rest* while the packets steer
  (`DetectorPace::kRestMs`): 2000 ms instead of 600, with
  `MobDetector::kPlayerMemoryMs` and the waits tied to it scaled so the rest
  still fits inside them. When the screen takes over again the reader wakes
  at once, as before, and the hunter is given no plate from a picture taken
  before the switch.
- *HP and MP with the pet's potion key*: the bars' rest
  (`vitals_rest_period_ms`) no longer refuses a pet key. With one in use,
  the bars are read every 500 ms (`kRestPetMs`) while the packets carry HP
  and MP, as the pet's HP is only on the screen -- about 2 reads a second
  instead of 6; the macro's limit on how old a reading may be (1500 ms) is
  unchanged. Every other reason the rest is refused stays.
- *The HUD's landmark* while lost (the HUD covered or gone) is searched for
  at most once a second.
- *`vitals_debug` pictures*: the first 20 of a run, then at most one every 10
  minutes.
- *Lines written per read*, such as `MP disagreement (learning)`: at most one
  every 10 s, the next saying how many were not written.

**Less work per look, and fewer wake-ups.**

- The picture matcher keeps its OpenCV buffers between searches -- the
  converted frame, the score map, each picture's mask. *Tự đóng thông báo*'s
  look of the whole screen, and its look for windows in the way when that runs
  on one thread, turn the frame into grey once for all their searches. Its answers are the same to
  the bit: `load_test.cpp` compares the old way and the new on made-up
  frames.
- On a PC with two logical CPUs or fewer, the bag jobs' and *Tự đóng thông
  báo*'s searches that ran side by side on extra threads (`std::async`) run
  one after another on the job's own thread: the same answers, without the
  threads and their memory. The bag jobs' waits for the game reuse one frame
  instead of a fresh one per look.
- The game's pictures are loaded once for the whole program instead of once
  per job (3 to 9 copies of about 18 MB each before).
- GoBot's window loop runs its work at most once per 10 ms however many
  window messages come (moving the mouse over GoBot used to multiply it, 184
  passes a second were seen); its Direct3D device is made for one thread, and
  the window library no longer polls for game pads. F10, F11 and F12 are
  looked at as often as before.
- The macro and the hunter copy their settings only when they changed, not
  on every tick or pass, and the macro's key sequence, while off, sleeps
  until it is switched on instead of looking every 50 ms. The 10 ms tick of
  the timed keys is unchanged.
- The party job (*Tổ đội*) reads `configs/party.ini` again only when the file changed
  (still looked at every 2 s); the client keeper's look for a crash comes
  every 6 s instead of 3.
- The background threads -- *Tự đóng thông báo*, the client keeper, the
  name-plate reader, the alerts, the party, the reviver, the one-off workers,
  the capture's watchdog and the bag, charm, shop, chat and quest jobs -- no
  longer get Windows' wake-up boost (`quiet_this_thread`, in
  `gobot/load_meter.hpp`). Their priority stays Normal; on one core they just
  stop jumping in front of the game each time they wake. The macro, the
  hunter, the HP/MP reader, the input, the window and the capture's own
  threads are as they were.

**Light mode turns itself on for a weak PC.** `auto` now also counts two or
fewer logical CPUs, or under 4096 MB of RAM; the grey line under *Nhẹ máy*
says so with the figures Windows gave (*Tự bật vì máy chỉ có 1 nhân.*, *… chỉ có 3,5 GB RAM dùng
được.*). RAM is what Windows counts as usable, so a PC sold with 4 GB, which
shows a little under 4096 MB, gets light mode too. The owner's 20-CPU PC is
not affected, and unticking the box still turns it off.

**MP no longer distrusted for a misread.** A figure of the bar that is the
packet value with any number of its leading digits lost -- 16462 read as 462
-- is now *between*, not a disagreement, for the maximum too. 2026.11.13's
rule allowed for one lost digit only, so such a read distrusted MP and handed
the macro a wrong low MP, which could press an MP potion for nothing.

**Removed.** The owner: what is no longer needed goes, without asking.

- The packet-only bot: `BotBrain`, `CommandExecutor`, `ProtocolHandler`, the
  `GameState` class and its structs (only `SelfVitals`, the HP/MP pair the
  macro takes, stays in `game_state.hpp`),
  `PathFinder`, `configs/bot_default.ini`, *Bot > Start* and *Save bot
  config*, the packet-bot half of *Features > Attack* (the hunter's settings
  there stay), and in **Nâng cao ►** the tabs *Dashboard*, *Protocol*,
  *Autopilot*, *Calibrate*, *Mapper* and *Map* (the game's map file and the
  path finder; the simple screen's **Bản đồ** stays) and the panels *Game
  keys*, *Weapon ranges*, *Working area*, *Monsters*, *Waypoints*, *Buffs*,
  *Click sequences* and *What is not built yet*.
- The walker to saved places (`Navigator`: *Go*, *Patrol these*), the
  *Radar* tab and its movement panels, and what only they used:
  `CameraTracker`, `MinimapTracker` (a map change seen on the minimap; the
  packets' map entries, and their log, stay) and `EdgeAnchor`; the map
  files' book (`GameMapBook`), which **Bản đồ** and the hunter use, stays.
  `hunt.ini`'s `[Move]` is no longer read.
  Its jump key's warning on *Trước khi bắt đầu*, *Phím nhảy khi đi tới điểm
  đã lưu là Space*, went with it -- the one change on the simple screen; the
  hunter's own *Phím nhảy đang là Space* stays.
- `MapLocator`, `WorldAnchor`, `FieldFinder`, the gatherer and
  `configs/gathering.ini` (fishing and mining), the CLR bridge
  (`GoBotBridge`, `bridge/`), the runner of `routines.ini`'s click tables
  (the reviver, `gobot_cli --routine revive`, stays), and `app.ini`'s
  `[Session]` loop and `[Capture] autostart`.
- `gobot_cli`'s packet-only bot (with no argument it now prints its usage)
  and the developer commands built on what went -- `--status`, `--session`,
  `--bridge`, `--party`, `--fields`, `--anchor`, `--whereami`, `--calibrate`,
  `--routines` among them. `--doctor`, `--login`, `--fix-dpi` and the checks
  that press nothing stay.

**Kept:** **Nâng cao ►**'s *Features* (Attack, Healing, Timed keys, HP / MP
reader, Auto party, Keep the client alive, Watch the session) and *Log*;
*View > Advanced panels* with *Tools > HP / MP* and *Tools > Capture* (its
*Stop capture* is a live test step of DOC-TRUOC.txt); `gobot_sniff` and
`configs/protocol.ini`. A `bot_default.ini` or `gathering.ini` copied over
from an older `configs` folder is left alone and not read.

### Thirty seconds for the game to react, and the board's ESC after a login (2026.11.15d)

Since 2026.11.15d (not yet run on the game). The owner, 2026-11-15, for
players on slow PCs (one core, 3.5 GB): *"khi GoBot thực hiện thao tác, sẽ có
thêm 30 giây chờ phản ứng. Nếu màn hình phản ứng luôn thì thực hiện tiếp bước
tiếp theo, còn nếu màn hình không phản ứng gì thì sẽ dừng xử lý."* And: *"Sau
khi chọn nhân vật xong và vào tới màn hình game thì sẽ chờ 30 giây rồi mới ấn
ESC để tắt thông báo. Trường hợp qua ngày thì chờ tới 00:03 mới ấn ESC."*

- **Thirty seconds for each reaction** (`gobot/react_wait.hpp`, `kReactMs`
  = 30 s). After a key, a click, a right-click, a double-click, a drag,
  typing or an ESC, GoBot looks for what it did -- the bag shown, a dialog
  up, a window gone, a tab chosen -- at the look gap that step always had,
  by the clock, for up to `kReactMs`. The first look that shows it goes on
  at once, so a quick game is as quick as before; only after 30 s with
  nothing shown is the step given up, the way it was given up before on its
  shorter wait. Every look still asks the stop (F12, DỪNG) and the job's
  guards. It is never a reason to press again: the bag's key, the chat's
  Enter and ESC toggle, and a second press on a slow game shuts what the
  first opened late; a control a second click cannot undo keeps its extra
  clicks, spread over the 30 s. A reaction later than `kReactSlowMs` (2 s)
  is said in the log ("... the game took N ms to show X"), a wait given up
  too ("nothing after 30 s ...").
- **The bag jobs** (`BagCleaner::react_ms()`): the bag after its key, the
  item in the bin, the bin's question and its Xác Nhận's closing, the item
  gone, `back_out`'s and `shop_cancel`'s Hủy Bỏ (each click
  `max(kCancelLookMs, react_ms() / kCancelClicksShare)`, 6 s), the tabs
  (the shop's Khác clicks spread over 30 s from the first, `kReactSlowMs`
  each but the last), the scroll, a charm's question (`charm_wait_ms` now
  bounds only the watch after a stop), the monster book, the notice, the
  shop's opening, question and going, the full-bag message, a black frame,
  and the bag seen shut after the end's key (`close_bag`; 1.5 s before).
  `BagCleaner::kStuckMs` is `kReactMs`, counted from the step's last input
  of GoBot's own -- for the shop the right-click, a double-click, the
  question seen, each typing, Xác Nhận, a Hủy Bỏ; for a pass the drag, the
  click on the bin, each Xác Nhận -- where 2026.11.14b counted 10 s from the
  step's start. A shop round's deadline (30 s and 6 s a purchase,
  `kShopDeadlineMostMs` at most) and a charm run's are moved on by whatever
  the game took past 2 s to show each input's work (`react_took`). Within a
  wait an HP or MP potion due still gets its moment every
  `kReactPotionMs` (2 s) where the look allows a key. The Vietnamese
  troubles end " sau 30 giây": "không mở được túi đồ (phím B) sau 30 giây,
  hoặc túi đồ bị che", "hộp nhập số lượng của X không thành 100 sau 30
  giây", "kẹt quá 30 giây (mua bình): GoBot đã bấm ESC N lần ...".
- **The quick quest** (`QuestRound::react_ms`): the window after its key,
  Tự Động on NV Phụ after the tab click, the box changed after its click and
  the window gone after the closing key, each looked for every 250 ms up to
  30 s from the key or click; `open_wait_ms`, `open_look_ms`, `tab_wait_ms`,
  `click_wait_ms` and `close_wait_ms` are only the pause before the first
  look and the least a step looks. `round_limit_ms` counts the round's own
  time, not its waits for the game. The round still starts at any HP, but
  HP under the potion's mark ends it as "not now" (máu thấp, tried again
  soon, no failure) once one of those waits has run past `kReactSlowMs`
  (2 s) -- `QuestRound::Hands::under_potion_mark`, from the review: a slow
  game would otherwise hold the potions off 30 s a step.
- **Tự chat** (`ChatSender::react_ms`): each Enter that opens the line is
  watched up to 30 s (`enter_gap_ms` is the least), and the next of
  `max_enters` (2 shipped) goes only after that whole wait, on a look that
  still shows the line shut; the typing, the Backspaces' work and the line
  shut after its Enter are looked for up to 30 s too. Not opened after them
  all: "Không mở được ô chat (đã bấm Enter 2 lần, mỗi lần chờ 30 giây) — bỏ
  qua câu này.", tried again at the line's next time. The fighting, potions
  and buffs wait as long; HP under the potion's mark still ends the line at
  once.
- **The party**: the first click on the Xác Nhận of the game's message after
  an invite is watched up to 30 s (`InviteReply::kFirstGoneMs`), the second
  `InviteReply::kGoneMs` (2.5 s); the 2.5 s watch for the message after an
  invite (`kWatchMs`) is no reaction and stays. The player list: one press of
  its button, the list looked for up to 30 s; a row's menu looked for every
  250 ms and Req Party clicked once two looks show it, up to
  `max(menu_wait_ms, 30 s)` -- never blind -- and the menu seen gone after
  it up to 30 s. Those waits hold the game's mouse, so they end when the
  character is down (HP under `bag::kJobDownHpPct`, or the death panel seen
  with the reviver on): the reviver is never kept waiting behind them.
- **The reviver**: the question after the revive-item button and the death
  panel's going after the last click are each waited for up to 30 s
  (`Reviver::kQuestionWaitMs`, `kReviveWaitMs`), so a slow PC no longer
  spends one of the three tries on a late answer.
- **Auto login**: a channel row and the login message's Xác Nhận get
  `kChannelTries` / `kMessageClicks` (3) clicks spread over 30 s from the
  first (`AutoLogin::react_ms()`), each watched its share and at least
  `kChannelTookMs` (2 s) / `kMessageGoneMs` (2.5 s), the last until 30 s;
  the step goes on as soon as the looks show the list gone, the row lit or
  the message gone (on two looks, as before).
- **The popup closer**: a click or an ESC is checked up to
  `PopupCloser::kCheckMs` (= `kReactMs`) for the popup to go, ending at the
  first look that shows it gone, the stop asked at every look; the hold is
  still let go within `kHoldMs` (2 s), so only the closer's worker waits.
  Once GoBot's ESC is seen to have brought up the ESC menu, the popup is
  watched only `kAfterMenuMs` (1.5 s) longer; an ESC menu drawn as late as
  `kOwnEscMs` (`kCheckMs` and 3 s) after GoBot's ESC counts as GoBot's, and a
  look with that menu over the popup never counts the popup gone. The log: "Popups: the game
  took N ms to show X gone", "Popups: nothing after 30 s: X still up".
- **The check-in board's ESC** (`PopupCloser::board_due`,
  `gobot/checkin_board.hpp`): the board ("Sự kiện và điểm danh") gets its ESC
  only `kCheckinBoardLoginWaitMs` (30 s) after the later of the character's
  arrival in the world -- the keeper's crossing, when GoBot logged in -- and
  the board's first sight at its place (a login by hand: the sight, as the
  board comes up right at the arrival), having stayed put `kSteadyMs` (3 s);
  and never while the PC's clock reads from `kCheckinNightFromS` (23:55:00)
  to `kCheckinNightToS` (00:03:00) (`checkin_night_hold`): the game opens the
  board again as the day turns, and the five minutes before midnight are for
  a PC clock behind the game's. Not a gate: no try counted, no backoff, the
  board looked at every turn and pressed at the first that it is due. The
  check-in notice's Xác Nhận and the monster book keep their 3 s. The tab
  says "Thấy bảng Sự kiện và điểm danh: chờ 30 giây sau khi vào game rồi mới
  bấm ESC." or "Qua ngày: chờ tới 00:03 rồi mới bấm ESC tắt bảng Sự kiện và
  điểm danh."; the log "Popups: the check-in board waits for its ESC until
  30 s after ...: in N s" or "... until 00:03:00: the PC's clock reads ...".
  The bag jobs' `esc_out` leaves the board alone in the night hold too: with
  only the board up, the screen counts as clear.
- **What a slow PC costs**: a key press the game loses now costs up to 30 s
  before its step is given up (and up to 60 s for a chat line with both
  Enters lost), with the potions held meanwhile by Tự chat (HP under the
  potion's mark ends the line) and the quick quest (2 s at most under the
  mark), and given their moment every 2 s by the bag jobs. The board stays up
  30 s after each login, and from 23:55 to 00:03.
- Tests: `use_test.cpp`, `quest_test.cpp`, `popup_test.cpp`,
  `party_hud_test.cpp` and `main.cpp` (the reviver, auto login, the player
  list, Tự chat) on pretend clocks; `jobs_gui_test.cpp`'s
  `test_notes_1115d` pins these notes to the constants.

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
held key. Both work while the game has focus -- F12 from any window -- so you
never have to find the GoBot window to stop it.

Where a key is typed into its box by name -- the macro's keys, and most others
under **Nâng cao ►** -- a single letter or digit is that key (`1` is the 1
key), F1 to F24, Space, Up and the like go by name, and two or more digits, or
`0x` and hex digits, are a Windows key code (`90` and `0x5A` are both Z).
Home, End, PgUp (or PageUp), PgDn (or PageDown), Insert (or Ins) and Delete
(or Del) go by name too, and the box shows them so; before 2026.11.11 `pgup`,
`pgdn`, `del` and `ins` set P, P, D and I.

Worth leaving on:

- **Only send while the game has focus.** Off, keys go to whatever window is in
  front — which will eventually be a browser or a document.
- **Jitter.** Each key has its own, plus a global amount on top. Perfectly
  regular timing is the clearest sign a client is automated.

Every key is a press: a key's or a step's own hold (the **Hold (ms)** columns,
`hold_ms` in the file) is at most 500 ms, and 0 -- as shipped -- is the usual
70–110 ms.

**Ra chiêu theo vòng** (`cast_paced` in `[Macro]`, 2026.11.08, *Thử nghiệm*):
*Ra chiêu theo vòng (chờ game ra chiêu rồi mới bấm chiêu kế)* in the simple
screen's **Kỹ năng** window (**Chi tiết…** on the skills row), after *Dùng
skill tuần tự* and *Tốc độ bấm kỹ năng*, and in the full screen's **Timed
keys** panel, which hands it over on Apply now or Save macro -- and since
2026.11.16b as *Ra chiêu theo vòng* on the **Tự dùng kỹ năng** row itself. On
as shipped since 2026.11.16b, and a `macro.ini` without `cast_paced_v2` reads
on, whatever its old `cast_paced` says (*Skills in turn while hunting too*,
above); unticked, every key on its own timer, the keys going out exactly as
before. Ticked, the skill keys
switched on go one round at a time in list order, each once -- the owner's Q1
(a) -- and the next goes down only once the game has cast the one before and
that cast's lock is over. Your casts are your client's own UDP `0x0031` under
your own id, stamped with `GamePacket::timestamp_ms` and handed from the
capture thread to `MacroEngine::note_own_cast` through a lock-free ring of
`kCastRing` (16), which takes none of the macro's locks. The lock is guessed at
`kPaceLockStartMs` (700) and learned within `kPaceLockMinMs`..`kPaceLockMaxMs`
(550..900): `kPaceLockUpMs` (20) up when a dropped first press's re-press is
cast and `kPaceDropsToRaise` (2) of the last `kPaceDropWindow` (10) first
presses were -- one alone, a stun or a frame hitch, is not the lock;
`kPaceLockDownMs` (5) down after `kPaceLockDownAfter` (20) first presses cast
within `kPaceAtLockMs` (30) of the guess, counted afresh at the start, after
each step down and after a re-press that was cast; a skip, a first press cast
late (after a potion, a buff or a skip) and a spell on the timers neither count
nor reset it. A key's `interval_ms` is then only the least time from its own
last cast: not over, the key is passed over this round. One press waits for its
cast at a time. None by `kPaceRepressMs` (160 ms) after the key went down and
`skill_gap_ms` after it came up: the same key once more -- for a key skipped on
an earlier turn, and not cast on a first press since, not before the longest
lock (`kPaceLockMaxMs`) after the cast before it is over. None `kPaceConfirmMs`
(220 ms) after that: skipped this round, and once another key casts within
`kPaceFloorTakenMs` (1.5 s) the time since its last cast is seen as the least
of its cooldown -- kept on the key's second such skip within `kPaceSeenMs` (60
s), as one stop of the game's (a stun, a hitch, the capture late) skips every
key in it -- the key passed over without a press `kPaceFloorUses` (20) times
and then checked with one. Since 2026.11.17 (*MP bar and skills while
hunting*, above) a cast by anyone starts the key's cooldown, a floor is never
above the shortest gap its skill was cast again after nor `kPaceFloorMaxMs`
(12 s), a skip within `kPaceSettleMs` (1 s) of a hold's end or a start, after
a wait or beside another's cast teaches nothing, and the check comes after
`kPaceFloorCheckTurns` (3) turns, the gap doubling up to `kPaceFloorUses`
(20). A press refused (the keys held, the game not in
front) is no try, and asked again 100 ms on (30 ms when another key was going,
since 2026.11.16b); a key that never goes out (Windows
not taking it, or ↓) has its turn pass with no cast. With no cast of yours for
`kPaceStaleMs` (2 s) while keys go out -- out of the world, a town, no capture
-- every key goes back on its own timer until your next cast; since
2026.11.17 only with the packets quiet too, and then a lock apart once casts
were seen this run: with the packets coming the game is taking no key (down,
stunned), and the rotation presses one key a second until your next cast.
Potions go first
as ever; a buff due while a skill's cast is waited for, or inside that cast's
lock, goes once the lock is over, and the next skill waits for the buff's cast
and its repeats. Since 2026.11.16b it paces with the hunter running too, the
skills following the fight or not, under the hunter's holds, the hunter's
swing told apart and left to it (*Skills in turn while hunting too*, above);
only the key sequence on keeps the tick from doing anything
(`PaceStatus::others`): its steps' casts would answer the key waited on. Fitted
to the 2026-11-08 log, the model gives 77-83 casts a minute, each skill about
every 6 s in list order, from about 80 presses a minute instead of 390. Its
log: `Paced: on -- …`, `Paced: no cast of yours for … -- the skills on their
own timers until your next cast`, `Paced: your cast came after … on the timers
-- the skills one after another again`, `Paced: off -- …`, and once a minute
`Paced: N rounds; N casts confirmed (N on a re-press); N re-presses; skipped
(cooldown): S 5 (4 learned, floor 6.0 s); fallback 0.0 s; lock_est 680 ms;
other casts 0; now paced`, since 2026.11.16b followed by `; given back N; held
N.N s; per key pressed/cast: Z 9/9, X 9/8, …`; under the tick,
`MacroEngine::pace_status_vi` (*Đang theo vòng — khóa chiêu ước 680 ms, 12
vòng.*).

Settings live in `configs/macro.ini` and are editable there or in the GUI.

## The capture by hand, and the game window picker

The packet-driven bot that used to live here -- **Dashboard**, **Protocol**
with its automatic setup, **Autopilot**, **Calibrate**, **Mapper**, the
old **Map** tab, *Bot > Start*, `configs/bot_default.ini` -- went in
2026.11.16, by the owner's word: it could never run, as the server never
sends your own position, and nothing of the simple screen used it. What reads
the packets now is *Vị trí từ gói tin* and the checks of *Packets first
(2026.11.03)*, which start their own capture.

Under **Nâng cao ►**, *View > Advanced panels* shows *Tools > Capture*: the
adapter, the server address, *Start* / *Stop capture* by hand, and the **game
window picker**. Pick the game window from that table rather than relying on
the title box (a title on its own counts only when it is the window's whole
title). On this machine the game shows up as **`GhostOnlineVTC`
(`Game.exe`, class `GhostSoul`)** — and an editor window whose title merely
contained "GhostOnline" sat right next to it in that list. That's the failure
mode the picker exists to prevent: matching the wrong one sends combat
keystrokes into someone's document.

## Deriving the protocol by hand

`gobot_sniff` does this, step by step; this is what to fall back to when a
step needs judgement.

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

It writes `capture.bin.s2c` and `.c2s` for the TCP stream, one `.flowN.s2c`
per connection, and `capture.bin.udp.s2c` and `.udp.c2s` for whatever came
over UDP — captured as GoC captures it, on the same address and ports, and kept
in files of its own. `--seconds <n>` stops it by itself.

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

It is also how the framing is checked: more than a couple of resync bytes per
frame decoded means it is wrong. Wrong framing doesn't fail loudly; it
produces plausible-looking garbage. `verified` in the `[Framing]` block says
the framing was measured; `protocol.ini` ships with it measured.

---

## Running

The bot is `gobot_gui.exe`. `gobot_cli` is a box of tools beside it; with no
argument it prints its usage (until 2026.11.16 it ran the packet-only bot).
What a player may be asked to run:

```powershell
gobot_cli --doctor        # everything GoBot needs on this PC, and what is wrong
gobot_cli --login         # VTCZone to a character in the world, as Tự đăng nhập
gobot_cli --fix-dpi       # the game's high-DPI override, as "Sửa cho game"
```

Many of the rest -- `--see`, `--read`, `--vitals`, `--potions`, `--party-hud`,
`--popups`, `--templates` -- only look and press nothing; `gobot_cli --help`
lists every command and says which ones press or click.

---

## Configuration

| file | holds |
|---|---|
| `configs/app.ini` | the game's window, the client keeper, the simple screen's switches (light mode among them: `[Simple] light_mode`); how hard GoBot works the PC (`[Performance]`, below); what it may take from the game's packets (`[Packets]`, under *Packets first (2026.11.03)*); the potion counts' warnings (`[Potions]`: `warn_below` for a hotbar slot, `warn_bag_below` for the packets' whole bag); capture interface and server address; whether `gobot_gui.log` starts afresh each session (`new_log_each_start`, on as shipped: the session before is kept as `gobot_gui.old.log`) |
| `configs/macro.ini` | the macro's keys -- skills, potions, buffs -- and the sequence; since 2026.11.10 `pet_every_ms`, how often the pet's potions go while the pet is under its mark (200-3000 ms, 300 as shipped and in a file without it) |
| `configs/hunt.ini` | hunting: your character's name, the attack and pick-up keys, the reach; where positions come from (`packet_positions`: off, watch, on) and how the packets lie over the screen (`packet_scale`, `packet_dy`); the hunting spot (`spot`, its area and when it walks back); the band, `up_reach` and `down_reach` (*Cao nhất*, *Thấp nhất*, since 2026.11.12 the two bars on **Bản đồ**, 0-1000, the only heights a drop or a jump up goes by); the hunter's own attack key's reach (`attack_range_px`, since 2026.11.12, 0: what its casts were seen to reach); the drop to a floor below (`drop_down`, off as shipped, with `drop_side_px` 120 and `drop_gate_px` 150; an older file's `drop_min_px` is no longer read); the jump up onto a floor above (`jump_up`) and the seek inside a **Bản đồ** area (`seek_in_zone`), both on as shipped and acting only in such an area |
| `configs/spots.ini` | the hunting areas drawn on **Bản đồ**, one per map (`[t52_s2]`: the home and the box, and when it was saved); made and written by GoBot, never shipped -- copy it over to a new version's `configs` to keep them |
| `configs/party.ini` | party: accepting, inviting by keyword, the lists of names |
| `configs/login.ini` | logging back in: where VTCZone is, the pages, the channel and character |
| `configs/trash.ini`, `configs/trash\` | bag cleaning: the icons picked, and the photographed items; the charms, and the potion shop (`buy_potions`, `buy_every_minutes`, and since 2026.11.09 `buy_hp_slots` and `buy_mp_slots`, 0 to 999 since 2026.11.10 (20 before), 3 as shipped -- a file without them reads its old `buy_hp_to` and `buy_mp_to`, 20 at most, until the card saves the new key); since 2026.11.24 the pet's HP food on reaching TổCổLong (`buy_pet`, off as shipped and in a file without it, and `buy_pet_slots`, 0 to 999, 1 as shipped) |
| `%LOCALAPPDATA%\GoBot\charm_clock.txt`, `shop_clock.txt` | when each charm may be used again, and since 2026.11.09 when the next potion plan begins: written before the click, so GoBot started again neither uses a charm nor buys early -- shared by every GoBot folder of that Windows account |
| `configs/chat.ini` | Tự chat: the lines, and how often |
| `configs/quest.ini` | quick quest: the key, and how often |
| `configs/alerts.ini` | alerts: which events, the limits -- never the webhook |
| `%LOCALAPPDATA%\GoBot\alerts_webhook.bin` | the Discord webhook, encrypted for your Windows account |
| `configs/protocol.ini` | framing + packet layouts, read by GoBot's packet readers and `gobot_sniff` |

Every one ships with whatever acts on the game switched off -- `jump_up` and
`seek_in_zone` ship on, but act only inside an area drawn on **Bản đồ**
with `spot`, which ships off -- and with no
player's names, keys or lists in it. (`bot_default.ini`, the packet-only
bot's file, went with that bot in 2026.11.16, and `gathering.ini` with the
fishing and mining it was written for; a copy left in an older `configs`
folder is not read.)

Field syntax for `protocol.ini` is documented inline in the file itself:
`name:type[@offset][*scale]`, with `@` pinning an absolute offset and `*`
applying a fixed-point scale.

### How hard it works the PC

Since 2026.11.02b `gobot_gui` draws its window only when there is something
to show (the frame pacer, `gobot/frame_pacer.hpp`): at once on a click or a
key, then up to 30 frames a second while the window is used, 5 while it is in
front and nobody touches it, 2 while another window -- the game -- is in
front, and none while it is minimised or covered; while the **Bản đồ** tab is
shown, 30 behind the game and in front alike (2 behind it in light mode; 10
from 2026.11.05 and 5 before), so the character and the monsters move on it. It used to
build and present a frame on every pass, about 60 a second, every one of them
drawn by the CPU in a VMware guest without 3D. The loop itself never waits
more than 10 ms, so it still comes round on every Windows clock tick (about
15.6 ms), and F10, F11, F12 and the potions are looked after exactly as
before, whatever the frame rate. Since 2026.11.16 the loop's own work
(`GuiApp::tick`) runs at most once per 10 ms however many window messages
arrive, so moving the mouse over GoBot no longer multiplies it. HP and MP are
read once a second while nothing runs, and as before while anything does. A
start reads them at once, and the macro does not take a reading from before
it: its first potion, skill, buff or step waits for the new one, 500 ms at
most -- for each bar that had a reading before the start; one never read is
not waited for.

**Light mode** (`[Simple] light_mode=auto|on|off`; the *Nhẹ máy (máy ảo / máy
yếu)* box on the simple screen, which writes `on` or `off`) is for a VMware
guest, a PC with no real graphics card, or a weak PC. It lowers the frame
rates to 20, 3 and 1; never asks PrintWindow again of a window this process
has already seen refuse it (otherwise once every 5 minutes); reads HP and MP
every 250 ms instead of 150 while anything runs; and, since 2026.11.16, has
*Tự đóng thông báo* look every 3 s instead of 2 while nothing is up.
`auto`, as shipped, turns it on when any of these is seen: GoBot's Direct3D
device is WARP, or the adapter is the Microsoft Basic Render Driver (vendor
0x1414) or VMware SVGA (0x15AD); SMBIOS names VMware
(`HKLM\HARDWARE\DESCRIPTION\System\BIOS`); CPUID's hypervisor leaf says
`VMwareVMware` with the hypervisor bit set; and since 2026.11.16 two or fewer
logical CPUs, or under 4096 MB of RAM (a player's PC of one core and 3.5 GB,
which also runs the game, stuttered with GoBot). The hypervisor bit alone does
not count: a Hyper-V or VBS host sets it too. The `Machine:` line near the top
of `gobot_gui.log` says what was seen and which signals fired, and the box's
grey line says why in Vietnamese (*Tự bật vì máy chỉ có 1 nhân.*, *… chỉ có
3,5 GB RAM dùng được.*). Unticking the box still turns it off for good.

`[Performance]`, read when `gobot_gui` starts:

| key | shipped | what |
|---|---|---|
| `gui_pacer` | `on` | `off`: a frame on every pass with vsync, as before 2026.11.02b |
| `vitals_idle_ms` | `1000` | how often HP and MP are read while nothing runs, 150 to 5000 ms; 150 is as before |
| `vitals_run_ms` | `0` | the same while anything runs -- the bot, a job, a one-off pass, *Tự tìm*: 0 is 150 ms, or 250 ms in light mode; anything else is held to 100 to 1000; never slower than `vitals_idle_ms` |
| `load_log_s` | `60` | how often the Load line is written, 10 to 3600 s; 0 writes none |

**The Load line.** Every `load_log_s` seconds, and once at exit, `gobot_gui.log`
gets a `Load:` entry: the process's CPU and each named thread's (gui, vitals,
detector, hunter...), the window's frames and loop passes -- with the longest
gap between passes, so a quick F12 tap that could have gone unseen shows --
screen grabs per owner with BitBlt and PrintWindow times, the readers' rates,
packets, log volume, memory and handles, the front keeper, and which jobs ran.
It spans several lines; the ones after the first are indented and carry no
timestamp. A log with a few of these, and the `GUI:` and `Machine:` lines from
the top, is what to ask for when a player says GoBot makes the PC heavy.

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
PacketWorld,  ───── the monsters, your place and bag, each use
PacketSelf          checked against the screen before it counts;
                    HP and MP straight from the packets (2026.11.17)
  │
  ▼                 the screen: HP/MP reader, name plates, the
Hunter,  ────────── game's own dialogs and pictures
MacroEngine,
the jobs            arbitrated by ExecutionCoordinator
  │
  ▼
InputManager ────── SendInput with scancodes: down only into the
  │                 game in front, up wherever focus has gone
  ▼
game window
```

`OpcodeRegistry` (data-driven layouts from `protocol.ini`) is `gobot_sniff`'s.
Until 2026.11.16 a second path ran from it, through `ProtocolHandler`,
`GameState`, `BotBrain` and `CommandExecutor`, for the packet-only bot; it
went with that bot.

Some deliberate choices:

**Capture is read-only.** Nothing is injected, rewritten or blocked. The bot
observes the connection and acts through the keyboard, which is the same split
the original used.

**Semantics, not opcodes, for the tools.** `gobot_sniff`'s layouts name what
a packet means (`semantic=monster_update`), not just its number. Re-mapping
an opcode there is an INI edit, not a recompile — which matters, because
protocol discovery is an iterative loop of guess, observe, correct.

**One input token.** The hunter, the macro and the jobs all want the
keyboard. `ExecutionCoordinator`
arbitrates by priority with stale-holder reclaim. Without it the bot walks left
and right in the same tick.

**Focus is a hard gate.** `SendInput` goes to whatever window has focus. A key
GoBot presses in the game goes down only while the game is the foreground
window. A click on the game (`click_on_game`) goes out only where the game
itself is under the pointer and, unless told otherwise — a trash pass finishing
with an item it has taken out of the bag — with the game in front; both are
asked again just before the button goes down. The trash pass's drag goes the
same way (`drag_on_game`): both of its points are asked before the pointer
moves, the first again as the button goes down, and once pressed the button is
always let go of. A key already down is always let go of,
wherever focus has gone, because a key-up held back for focus would leave that
key down for every other program. And when the game goes behind another window,
`gobot_gui` lets go of every key it holds — otherwise the character keeps
running into a wall after you alt-tab.

**Down only inside Ctrl.** Standing on a gate, Down takes the character to
another map. `InputManager::never_pressed` keeps Down out of every key call,
and `inject()` -- the one place `SendInput` is called -- refuses every array
that puts Down down unless it is `drop_ctrl_down`'s own chord, with a
Ctrl-down before the Down in that same array; `[Ctrl↓, ↓↓]` from any other
path is refused too. `drop_ctrl_down` is the one call that builds Down's
events: it holds Ctrl first, sends Ctrl and Down together in one array, lets
Down up before Ctrl wherever focus has gone, and owes any up that did not go
out until it does. The jump up's Ctrl + ↑ (2026.11.05b) is the same chord
with ↑ in Down's place (`chord_ctrl(ChordKey::Up)`), never marked as the
drop's, so it cannot carry a Down; while either runs or owes an up, every
other thread's key-downs are refused, and the ups go ↑ or Down first, then
Ctrl.

---

## Limits

- **Encrypted payloads defeat this entirely.** If `--detect` finds no tiling,
  stop here.
- **Position accuracy is what the protocol gives you.** If the server only
  broadcasts movement every 200ms, the bot's world model is 200ms stale. And
  your *own* position is not broadcast at all: the server never echoes your
  moves, and only your client's own UDP reports of them -- on each start,
  stop, jump and cast -- say where you are, which *Vị trí từ gói tin* reads
  and works out between. A walk into a wall sends nothing, so a walk is
  followed for 0.6 s at most, and the screen takes over past that -- or for
  3 s while the hunter holds the walk's key down (`hold_walk`). It lets go of
  that key for a moment every 2.5 s, and at once when the target is getting no
  closer, so the stop and the start the client reports keep the packets'
  position fresh. A held walk into a wall is put right that way at each
  renewal rather than handed to the screen.
- **There is a vision layer, and it is not optional.** This file used to say
  there was none. There is: template matching, a glyph reader, HP/MP bar
  reading, nameplate detection, auto-login and the macro mode all read the
  screen, and the macro works with no packet capture at all. What vision does
  *not* do reliably is supply world state — an on-screen anchor
  (`WorldAnchor`, gone in 2026.11.16) never converged on a position, nor did
  following the minimap or the scenery's slide (`MinimapTracker`,
  `CameraTracker`, gone too) — so it belongs on the interface, not the world.
- **Reading the screen needs the game unobscured.** `PrintWindow` returns a
  blank frame for this client, so pixels are lifted off the screen itself.
  Anything covering the game gets read instead of the game.
- **Moving about a map needs to know *which* map you are in, not map data.**
  The geometry is already there: 781 `.map` files the game ships, which
  `MapFile` reads. What is needed is identifying the current one — the
  packets' map entries do it; minimap fingerprinting and reading the place
  name off the title bar, neither of them solid, went in 2026.11.16 with the
  walker to saved places that used them (`Navigator`, `PathFinder`) — and
  knowing where in it you are. Since 2026.11.04
  `GameMapBook` reads those files' floors, and the `.prj` files' gates, for
  the hunter's drop -- and since 2026.11.04c for the **Bản đồ** tab, and
  since 2026.11.04d for an area's walks and its way back, each floor
  followed as it runs (`Page::follow`), and since 2026.11.05b for the jumps
  up (`jump_plan`) and the seek's floors and attack points (`hunt_seek`) --
  and `MapTrust` names the map from
  where you and the monsters stand when no map entry was seen -- strictly: one file alone
  fitting at least 12 standing points, the next best missing 6; a monster's
  point counts only once two of its frames agree, and only yours let a map
  go.

---

## A note on use

Botting violates GhostOnline's terms of service, and accounts do get banned for
it. That's between you and the operator — but the randomised input timing in
here is a mitigation, not a cloak.

Use this on your own account and your own traffic.
