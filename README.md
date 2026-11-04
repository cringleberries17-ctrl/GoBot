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
  reading is closed off by XIGNCODE3, and the on-screen `WorldAnchor` never
  converges, so these packets are the route; *Vị trí từ gói tin* uses them
  (P0-3).
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
| `gobot_cli.exe` | headless bot runner |
| `gobot_sniff.exe` | protocol discovery, command line |
| `gobot_sniff_files.exe` | gobot_sniff's commands that only read recordings (`--verify`, `--detect`, and `--dump`, `--diff`, `--decode`, `--analyze` with `--from`), with no administrator prompt; it captures nothing, and is not packaged |
| `gobot_test.exe` | self-tests |

`gobot_gui` covers everything the two CLI tools do. They remain for scripting
and for running the bot without a window.

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
partial overlap), packet decoding, the world model, pathfinding, the glyph
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
parallel plugins it would look for beside them. The one exception is
`GoBotBridge.dll`, which two developer commands, `gobot_cli --bridge` and
`--party`, load from beside `gobot_cli.exe`; no package carries it.

---

## Getting started

`gobot_gui.exe` opens on one simple screen, in Vietnamese, with seven tabs.
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
   buffs -- and, even after DỪNG LẠI, the packet bot and a walk to a saved
   place -- stay stopped and BẮT ĐẦU waits. **DỪNG LẠI**, or F10 again,
   otherwise stops everything the same way but the packet bot and a walk to a
   saved place.

Everything that acts on the game ships switched **off**: nothing is pressed
until something is ticked. **Tự hồi sinh** revives on the spot with a revive
item -- the death panel's *Sử dụng vật phẩm hồi sinh*, then Xác Nhận; never
the village (−2% EXP), never Cash -- three tries a death, and when they fail
the whole bot stops and says why in red. **Tự buff** gives each buff key its
own time, 0–7200 seconds -- at 0 it is cast again every 5 s, as GoC casts a
buff whose cooldown is 0 -- and casts every buff again after a death, a revive
or logging back in. While the bot runs, **Luôn đưa game lên trước** brings the
game back to the front at once whenever another window takes it, GoBot's own
included -- a moment (0.2 s) after the last click or key, so a click lands
first, and up to 10 s while a key box waits for its key (less when HP falls
under the HP potion's mark, or 50% if that is higher); untick it to use the PC
meanwhile. The game reads its keyboard once a frame, so a key held too briefly
is now and then not seen (the key is pressed and nothing happens): the macro
holds each key 70–110 ms (`key_hold_min_ms`, `key_hold_max_ms` in `macro.ini`),
keeps two potions 60 ms apart, and presses a potion again after
`potion_recheck_ms` (1000) when its bar has not risen and still wants the
drink -- once per press, with a `Potion check:` line in the log; 0 in the
file is the old behaviour. A potion whose bar is under half its mark (a 59% MP
potion: under 29%) may drink again `potion_urgent_ms` (1000) after its last
press instead of after its 3-s interval, so that skills draining MP faster
than one potion per 3 s do not leave the bot behind; 0 is the plain interval.
A buff is cast the way GoC casts it, by pressing its key
4 times, 200 ms apart (`buff_presses`, `buff_press_gap_ms`): the game drops a
single press now and then, and a buff dropped is gone for its whole interval.
The log has a `Buff: key … cast` line for each cast, and why when one cannot
be. The game has to be windowed, 1024x768 or 1280x720 -- the readers follow
either size. Windows' display scale can be anything on
Windows 10 (1607 or later) and 11: GoBot works in real screen pixels; on an
older Windows keep it at 100%. But above 100% Windows enlarges a game that
does not handle display scale itself, and a game shown enlarged cannot be
read; the checklist then says *Windows đang phóng to game*, and its **Sửa cho
game** button sets the game's own high-DPI override (as its Compatibility tab
would), after which the game must be started again (`gobot_cli --fix-dpi` does
the same). Or set the display scale to 100%. *Windows đang phóng to chính
GoBot* means a high-DPI override on GoBot's own Compatibility tab holds GoBot
to Windows' scaling: **Sửa cho GoBot** takes it off, then start GoBot again.
Logging back in (**Tự đăng nhập**, **Tự vào lại game**) finds VTCZone's and the
launcher's pages by pictures taken at 100%, so a VTCZone that draws itself
larger above 100% is not recognised -- GoBot says so when it gives up -- and
needs the display scale at 100%. On a screen 768 pixels high only
1280x720 fits, with the taskbar set to hide itself; a hidden taskbar's strip,
and anything past the screen's edge, is not counted as covering the game.

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
see where a platform ends, so the first step past an edge is not prevented.
With any *Kiểu đánh*, *Như trước* included, the character turns to a monster
only when it is not facing it already. A hurt monster is gone for first only
when it is within reach, and one chased for `give_up_ms` (4 s) without coming
within reach -- behind a wall, up on a ledge -- is passed over for twice as
long, one on
*Đánh trước* too; the box under BẮT ĐẦU says so while nothing else is in
view. Without a *Kiểu đánh* every fight still going after `give_up_ms` is
picked again, usually the same monster still in reach: since 2026.11.04
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
**Chi tiết…** for each skill's cooldown, range and *Luôn bấm theo giờ*; a key
sequence left on in macro.ini is shown there with a **Tắt**. Each potion row
has a second tier, *Lv2*: under both marks only the lower one drinks, and a
potion due goes before any other key. **Tự bơm máu pet** is the third potion
row, the pet's: its key is pressed when the pet's HP is at or under *khi máu
pet dưới*, on its own cooldown, after the HP and MP potions and never ahead of
them. The pet's HP is the red bar in its slot on the HUD, under the HP and
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
is over (about 0.8 s), never as a second press in the air. GoBot never
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
is sent or changed. A capture already running (the Protocol tab's,
Autopilot's, the packet bot's) is used as it is and never stopped by the
hunting; one stopped by hand stays stopped until the next hunt; a game
started again or reconnected is followed within a second or two, and when it
opens a new UDP socket or goes quiet for 5 s the capture is started again at
most every 10 s -- less and less often while the capture itself refuses to
start, on an adapter it cannot use. Unticked
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
the **Tầm đánh…** window); one within reach is fought wherever it stands, and
a target that leaves the area by more than 60 px before it is reached is let
go of. With nothing to fight for `spot_return_delay_ms` (1.5 s) and the
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
platform's edge toward it, where the platform allows that.
This version remembers the spot for the current run only: every BẮT ĐẦU takes
it afresh, and nothing of it is saved. A map entry or a log back in sets the
spot aside, and the character stands and fights only what comes within
reach, as with *Đứng yên một chỗ*. After a map change (a door) it stands
until the character comes back through a door to near where it left -- a
map entry whose spawn is within 150 px of that. After a log back in (by
**Tự đăng nhập**, or a game opened again and found with **Tìm lại**) it
stands until the character stands, still, within 150 px of where it was or
of the spot -- placed by the new game's packets, after its first step or
skill, and not on a map they know is another. A log in after a door keeps
the door's rule. Otherwise it stands until **Đặt chỗ này** is pressed. It stands as well while
the packets have lost the character. To take the character somewhere else
while the bot runs: untick **Chỗ đánh quái** (the walk back stops at once and
the spot is dropped), walk there, tick it again and press **Đặt chỗ này**.
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
than `drop_min_px` (100) below -- or the band's *Thấp nhất*, if that is
larger -- and no more than `drop_side_px` (120) across, with nothing to
fight on the character's own level, which always goes first, on *Đánh trước*
or not. With nothing on its level and a drop able to go -- the map trusted,
drops not off for the run -- the nearest monster below is picked before one
above, which a hop would only chase. The hunter walks to above the monster,
lets go of the walk, waits for the packets to report the character standing,
and presses. The tick is greyed out, its reason in the tooltip, while *Đứng
yên một chỗ* is ticked (never a drop then), while *Vị trí từ gói tin* is not
(the packets steer every drop: never in Watch, nor once the screen has taken
over), and while a key of the hunter's -- jump, left, right, attack -- is
Ctrl, or one of GoBot's own (F10, F11, F12, the two hotkeys) is Ctrl or
Down; the full window's *Drop to things below you (Ctrl+Down)* is greyed the
same way. With the hunting spot on, a drop goes only while the spot is in
use, and only when where the character stands, where it lands and the target
all lie inside the area: with the shipped `spot_half_h` of 100, a monster
more than 100 px below the spot is outside it already. The walk back never
drops.

Where it may drop comes from the game's own files, read only:
`data\Map\<stem>.map`, read planar -- the floors under every x, with their
attribute -- and `data\Project\<stem>.prj` -- the gates, its warp-zone and
portal objects -- in the folder `MapFile` finds `data\Map` in. Ctrl + Down
is pressed only when the floor a straight fall lands on is the monster's or
one between, never one below it, and the same floor a few pixels either
side; when the character does not stand on the bottom ground (only a
platform is dropped through); and when no gate lies within `drop_gate_px`
(150) across, from a little above where it stands to a little below where
it lands: Ctrl + Down on a gate changes the map. The map has to be known
and its file trusted. A map entry (`0x0086`) seen since the capture began
names it, and is trusted at once when its spawn lies by one of the file's
gates, or after three standing points on its floors. With GoBot opened
mid-map ("map 0/0") the standing points name it -- your settled reports and
the monsters' stops: at least 12, apart from each other, every one on a
floor of exactly one of the game's `.map` files, with every other file
missing at least 6 of them. A real entry always wins; a later standing
point on none of the floors lets the map go (a named one is named afresh,
an entry's not until the next entry); a map whose `.prj` lists no gate is
never trusted. The hint under the row says how far the naming has got.

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
stops with a red line (*phím ↓ hoặc Ctrl có thể vẫn đang bị giữ*). With the
switch on, GoBot's start and every BẮT ĐẦU let go of a Down or Ctrl that
Windows still reads down, as a GoBot ended mid-drop would leave them.

What came of a press is judged from the packets alone: landed on the floor
the file gave (worked), no take-off within 1 s (did nothing), a jump in
place, a rise, or a landing elsewhere. Did nothing and a jump in place
remember the place -- no drop within 64 px of it until GoBot closes -- and
pass the monster over for twice `give_up_ms`; 3 in a row, or 5 in a run,
switch drops off until the next start, as does a rise, a landing off the
file (whose file is then not gone by again this session) or Down held over
180 ms. At most 4 presses a minute and 2 per monster, 1.5 s apart, none
within 5 s of a map entry, and none while a hold is on, the game is behind,
HP is under the HP potion's mark (50% at the least), the macro holds for a
character out of the world, or a navigator walk or the keeper's crossing
runs. A map change within 10 s of a press, whatever was judged of it, stops
the hunt, unticks *Nhảy xuống bậc dưới* and saves `hunt.ini`, with a WARN
(`Hunter: MAP CHANGED …`) and a red line under the tick until the next
start; both ends are learned as gates for the session. The log has a line
per press (`Hunter: drop (Ctrl+Down) toward …`) and per verdict, `Hunter:
not dropping … -- <why>` once a target for what the player can act on, and
the minute's `Hunter:` line gains `; drops N (… worked, … did nothing, …)`.
`gobot_cli`'s hunting never drops.

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
BẮT ĐẦU opens it first. It stops for a person after `max_restarts` restarts
(`[Crash]` in configs/app.ini, 5 as shipped), a restart being every time the
game had to be closed and entered again -- after a crash, a hang, the
watchdog, or a crossing that ended a game stuck part way in, the login key's
included. It needs no account and no password: VTCZone stays signed in (sign
in to it once, with its remember option) and GoBot only presses *Chơi game*;
if VTCZone shows its sign-in form instead, it stops and says so. **Dọn rác** opens the bag every
few minutes and puts what is on your list in the trash can; you pick the junk
once from the game's item icons (or a photo of your bag for anything else),
and *Thử tìm* shows what would go without throwing anything away. Items are
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
closed with its own Xác Nhận, never Enter or Esc, and the stack must be seen
one less before the next kind. A stack of one has no number on it: it is
used only when its count corner is that colour's own icon with nothing over
it, and must then be seen gone. *Dùng bùa ngay* uses them at once. Beside them, *Tự mua bình* (also off until ticked) buys
Thịt bò khô and Rượu lá tre from the shop the Người Giữ Sân item of the bag's
Khác tab opens (found by the game's own icon for it, nothing to photograph),
when Tiêu Dùng holds fewer slots of them than you set: it double-clicks the
potion in the shop, types 00 after the game's 1 in the quantity box, reads it
back as 100, and only then presses Xác Nhận; anything unexpected is answered
Hủy Bỏ. Skills, potions and buffs wait while it buys, and *Thử mua* goes as far
as the quantity box without buying. It opens the bag only after a second
with nobody at the mouse or keyboard; otherwise it logs `Shop: not now --
someone is using the mouse or keyboard` and, on the clock, tries again in
30 s, while **Mua ngay** or **Thử mua** says *Chờ: bạn đang dùng chuột hoặc
bàn phím…* on the tab and does nothing. Nothing
is counted, bought or right-clicked unless the bag's tab strip reads Tiêu
Dùng chosen: after a click on a tab the pointer is moved off the strip (a
tab under it lights up), a strip that shows no tab chosen is looked at again
up to 8 times, 150 ms apart, and one that shows another tab for sure is
clicked once more. Still not Tiêu Dùng, the shop and the charms stop (a bag
pass goes on, as it always has), with a `Trash: tab not read as …` line and
one picture per place each session in `trash_debug` (`tab_unsure_…`); a
charm run stopped so says so, not "no charm found", and a timed one waits
10 minutes. *Tốc độ bấm nút* (60, 80, 100 or 150 ms)
sets how fast these bag jobs click, drag and type; what they wait for and read
back does not change with it. (*Tốc độ bấm kỹ năng* on the skills row does the
same for skill keys: 50 ms as GoC, or 60, 80, 100, 150 ms between one skill's
key coming up and the next one's going down.) When several jobs fall due at once they run in
turn: a Tự chat line, then the bag pass, then the charms, then the potions.
These jobs, and Tự chat, read the game off the screen, so the game's window has
to lie wholly on the screen. BẮT ĐẦU starts whatever is ticked on any tab.

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

### Packets first (2026.11.03)

Since 2026.11.03 GoBot takes from the game's own packets whatever they can
give, and keeps the screen for the rest. Nothing is sent or changed, and no
key is pressed for it -- Down least of all. None of it has run on the real game
yet. A use that rests on a layout not proven on today's server acts only once
a check at runtime has compared it with the screen's reading of the same
thing, and the screen takes over again at once when they disagree; every
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
- **Your HP and MP** (wrapped `0x0051` and `0x0050`) go through a check per
  bar against the bars on the screen. A bar is trusted after 20 agreements
  over at least 3 values with no disagreement among the last 20, HP also
  needing one under 98%. One disagreement puts it in doubt, and the screen's
  reading goes to the macro at once; 5 agreements in a row clear the doubt;
  two disagreements in a row, or 3 in 20, distrust it, and the third distrust
  switches it off for the session. Trust is earned by the printed figures
  exactly, judged 0.5 s after the read, as ever; since 2026.11.04 it is kept
  allowing for the HUD running ahead of `0x0051` (a hit or a cast's cost shows
  on the bar 0.4-0.8 s before the packet). While a bar is trusted or in
  doubt, figures within 5% of the maximum (HP) or 12% (MP) of the packet value
  count for nothing either way; a read whose figures did not agree waits
  `kLeadMs` (0.9 s) for `0x0051` to reach them exactly, and then agrees
  (*caught up*); and reads of one HUD moment against one packet value make
  one disagreement however many there are (*same moment*). A real fault is
  then caught 1 s after the read rather than 0.5 s. A read that missed the
  figures is judged by the bar's length only under 54% (it was 60): the
  caption printed right-aligned over the bar paints out the fill under it,
  and an MP of 10000 or more ("12643/15280") begins 56.7% along. The same
  shadow no longer misleads the potions: a fill measured at 54% or more with
  the figures missed takes the last figures read, kept about 2 s, when they
  say more. The minute's `Vitals check:` line counts `caught up` and `same
  moment`. While a bar is usable -- trusted, of the
  current capture (GoBot's own, on the game window's process: never a
  Capture-tab or Autopilot capture, which takes every client's traffic), a
  server frame within 5 s, a maximum known -- the potions, and with the
  bars' last read seeing the HUD the jobs' HP guards (charms, shop, trash,
  Tự chat, quick quest, closing popups, bringing the game to the front) and
  the skills' MP floor go by the packets, and while anything runs the bars
  are read once a second instead of
  at the run's pace: not with a pet potion key while the macro runs, in a
  critical hold, during *Tự tìm*, while HP reads 0, or within 60 s of a death
  panel.
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
  hotbar's. Once two
  shop runs' own counts agree with the bag, a due *Tự mua bình* look may be
  skipped while the bag holds enough -- never two due times running, never
  more than 2 h without a real look, and only when the bag has come since the
  last potion press. Any disagreement voids that trust. **Mua ngay** and
  **Thử mua** always run, and buying stays on the screen. The bag can also
  stop a charm's right-click when it names another item in that slot, with
  the screen's count, or -- for a stack of one, which has no number on the
  screen -- another count or none -- never cause one.
- **The name-plate reader rests** (with *Vị trí từ gói tin* ticked): every
  600 ms instead of 140 while the packets steer a healthy hunt, and 140 again
  within a tick for **Thử**, *Nhìn quanh*, the Radar, a walk to a saved point,
  or the screen taking over -- the hunter then keeps the packets' view until
  the reader has a fresh picture, 300 ms at most.
- **Map changes** go by the packets' map entries once one has been seen this
  session (a door, a log in). The minimap's fingerprint, which changed six
  times in a minute and a half of one hunt with no door taken, is then only
  logged.
- **Thử** beside the jump key judges by your client's take-offs (`0x002A`)
  whenever the packets can tell -- GoBot's own capture on the game, your id
  known, no other character reporting -- and says *(theo gói tin)*; otherwise
  by the plate, as before.
- **Log only:** casts and buffs per macro key (`Casts (packets)`,
  `Buffs (packets)`), the bag after a charm's use and beside the trash list,
  and the link beside the keeper's stalls and log-ins (`Keeper:`).
- **Still the screen's:** death and revive (with the reviver's HP and the
  recast after HP comes back from 0), the pet's HP, the shop's dialogs and
  buying, the captcha, and whether the character is in the world.

Unticking *Vị trí từ gói tin* (`watch`) puts the hunter back on the screen --
the monsters' HP, the slot lock and the reader's rest go with it -- but the
capture still runs, and your HP and MP, the bag, **Thử** and map changes still
use the packets once checked. `packet_positions=off` in `hunt.ini`
(*Positions from packets: Off* in the full window) turns every packet use off:
no capture at all, everything from the screen.

`[Packets]` in `configs/app.ini`, read when `gobot_gui` starts. Each key is
`auto` or `off`, and anything but `off` counts as `auto`. `off` turns that use
off; its check still runs and still writes its lines.

| key | shipped | what |
|---|---|---|
| `vitals` | `auto` | your HP and MP from the packets once each bar's check trusts them: the potions, the jobs' HP guards, the skills' MP floor, and the bars read once a second |
| `bag` | `auto` | the bag's potion counts: "Low" by the bag's total, and a due shop look skipped; and a charm's slot checked before its right-click |
| `detector_rest` | `auto` | the name-plate reader every 600 ms instead of 140 while the packets steer |

What to look for in a log sent after one session: `Positions: capturing …`
before any `HUNT ON`; `Packets: capture since …`, `Packets: first 0x…` and a
`frames:` line under `Load:`; `Positions: map entry … (from …)` at a door; no
`Capture stopped` at F11; `hurts … (chained …)`, `kills … (yours …)` and
`your id` in the Positions line; `wounded picked` in the Hunter line;
`Detector: every 600 ms …`; `Vitals check: …` and
`Vitals: HP from the packets now`, or `… from the screen again` with both
numbers; `World: map loads by packets …`; `Bag check: … agree (n of 2)`,
later `Shop: skipped -- …`; `Jump test: … (packets: N take-off …)`;
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

Worth leaving on:

- **Only send while the game has focus.** Off, keys go to whatever window is in
  front — which will eventually be a browser or a document.
- **Jitter.** Each key has its own, plus a global amount on top. Perfectly
  regular timing is the clearest sign a client is automated.

Every key is a press: a key's or a step's own hold (the **Hold (ms)** columns,
`hold_ms` in the file) is at most 500 ms, and 0 -- as shipped -- is the usual
70–110 ms.

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
| **Dashboard** | Start/stop with START BOT / STOP BOT -- no key starts it here: F10 is the macro's, and F12 stops it along with everything else. Live HP/MP, position, monsters. Lists exactly what is still blocking a run, with the automatic setup button. |
| **Capture** | Only if you want manual control: adapter, server address, **game window picker** |
| **Protocol** | **Auto** does everything; the numbered tabs are the same steps by hand |
| **Calibrate** | Works out which packet carries your position and mana by walking the character through a short routine. **Start calibration** brings the game to the front itself and holds everything else off for its 20 seconds or so; it refuses while hunting, a walk to a saved place or the packet bot runs. F12 or **Abort** stops it, and a run stopped part way is not analysed. |
| **Bot** | Combat behaviour, targeting, ranges, timing |
| **Skills** | Skill slots, movement keys |
| **Heal** | Revive. Potions and buffs are the macro's keys (Timed keys / the simple screen), the packet bot's too |
| **Log** | Live log |

Pick the game window from the table on the Capture tab rather than relying on
the title box (a title on its own counts only when it is the window's whole
title). On this machine the game shows up as **`GhostOnlineVTC`
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

`gobot_cli` refuses to start the bot while `verified=false` in the `[Framing]`
block. `gobot_cli --status` still runs: it presses nothing, and it is how the
framing is checked -- more than a couple of resync bytes per frame decoded
means it is wrong. GoBot's window confirms the framing by itself from live
traffic, and writes it down when its protocol is saved.
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

No input is sent: the bot's key threads are not even started. Confirm HP,
position and monster count track reality. While `[Framing]` is not
confirmed (`verified=false`), `gobot_cli` will not start the bot; `--status`
still runs.

Then:

```powershell
gobot_cli
```

**F10** switches the bot on, with the game or this console in front, and off
from anywhere. Until it is on, nothing is pressed, whatever HP the packets
say. The bot's potions and buffs are configs/macro.ini's: `gobot_cli
--with-macro` runs the macro beside it, on and off with it, on the HP and MP
the packets carry; without it the bot neither drinks nor buffs. Switched off, by F10, by a death with
`continue_auto_after_death` off or by the traffic going quiet, it halts at
once: nothing queued is carried out, its route is dropped and every key is let
go of. Whenever the game goes behind another window, every key held is let go
of too.

**Ctrl+C**, **Ctrl+Break** or closing the console stops it the same way, and
the capture with it; a closing console gives it up to 4 s for that. Should it
still be running 5 s after a Ctrl+C, a second one ends it at once.

---

## Configuration

| file | holds |
|---|---|
| `configs/app.ini` | the game's window, the client keeper, the simple screen's switches (light mode among them: `[Simple] light_mode`); how hard GoBot works the PC (`[Performance]`, below); what it may take from the game's packets (`[Packets]`, under *Packets first (2026.11.03)*); the potion counts' warnings (`[Potions]`: `warn_below` for a hotbar slot, `warn_bag_below` for the packets' whole bag); capture interface and server address; whether `gobot_gui.log` starts afresh each session (`new_log_each_start`, on as shipped: the session before is kept as `gobot_gui.old.log`) |
| `configs/macro.ini` | the macro's keys -- skills, potions, buffs -- and the sequence |
| `configs/hunt.ini` | hunting: your character's name, the attack and pick-up keys, the reach; where positions come from (`packet_positions`: off, watch, on) and how the packets lie over the screen (`packet_scale`, `packet_dy`); the hunting spot (`spot`, its area and when it walks back); the drop to a floor below (`drop_down`, off as shipped, with `drop_min_px` 100, `drop_side_px` 120 and `drop_gate_px` 150) |
| `configs/party.ini` | party: accepting, inviting by keyword, the lists of names |
| `configs/login.ini` | logging back in: where VTCZone is, the pages, the channel and character |
| `configs/trash.ini`, `configs/trash\` | bag cleaning: the icons picked, and the photographed items |
| `configs/chat.ini` | Tự chat: the lines, and how often |
| `configs/quest.ini` | quick quest: the key, and how often |
| `configs/alerts.ini` | alerts: which events, the limits -- never the webhook |
| `%LOCALAPPDATA%\GoBot\alerts_webhook.bin` | the Discord webhook, encrypted for your Windows account |
| `configs/protocol.ini` | framing + packet layouts (**you write this**) |
| `configs/bot_default.ini` | the packet bot: combat behaviour, skills, revive, keys -- and `key_start_stop`, gobot_cli's on/off key (F10; gobot_gui does not read it). Its potions and buffs are macro.ini's |

Every one ships with whatever acts on the game switched off, and with no
player's names, keys or lists in it -- `bot_default.ini` included: the packet
bot's attack and skills are off until switched on, so **START BOT** in
`gobot_gui` and F10 in `gobot_cli` press nothing before that (and say so).
The attack and skills are ticked under **Attack** in **Nâng cao ►** once the
protocol map can place your character, and until then only in the file. Its
potions and buffs are the macro's keys and go while the macro runs (in
`gobot_cli`: `--with-macro`).

`bot_default.ini` follows the original client's schema, so existing configs
port with minimal editing.

Field syntax for `protocol.ini` is documented inline in the file itself:
`name:type[@offset][*scale]`, with `@` pinning an absolute offset and `*`
applying a fixed-point scale.

### How hard it works the PC

Since 2026.11.02b `gobot_gui` draws its window only when there is something
to show (the frame pacer, `gobot/frame_pacer.hpp`): at once on a click or a
key, then up to 30 frames a second while the window is used, 5 while it is in
front and nobody touches it, 2 while another window -- the game -- is in
front, and none while it is minimised or covered. It used to build and present
a frame on every pass, about 60 a second, every one of them drawn by the CPU
in a VMware guest without 3D. The loop itself never waits more than 10 ms, so
it still comes round on every Windows clock tick (about 15.6 ms), and F10,
F11, F12 and the potions are looked after exactly as before, whatever the
frame rate. HP and MP are
read once a second while nothing runs, and as before while anything does. A
start reads them at once, and the macro does not take a reading from before
it: its first potion, skill, buff or step waits for the new one, 500 ms at
most -- for each bar that had a reading before the start; one never read is
not waited for.

**Light mode** (`[Simple] light_mode=auto|on|off`; the *Nhẹ máy (máy ảo / máy
yếu)* box on the simple screen, which writes `on` or `off`) is for a VMware
guest or a PC with no real graphics card. It lowers the frame rates to 20, 3
and 1; never asks PrintWindow again of a window this process has already seen
refuse it (otherwise once every 5 minutes); and reads HP and MP every 250 ms
instead of 150 while anything runs. `auto`, as shipped, turns it on when any
of these is seen: GoBot's Direct3D device is WARP, or the adapter is the
Microsoft Basic Render Driver (vendor 0x1414) or VMware SVGA (0x15AD); SMBIOS
names VMware (`HKLM\HARDWARE\DESCRIPTION\System\BIOS`); CPUID's hypervisor leaf
says `VMwareVMware` with the hypervisor bit set. The hypervisor bit alone does
not count: a Hyper-V or VBS host sets it too. The `Machine:` line near the top
of `gobot_gui.log` says what was seen and which signals fired.

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
CommandExecutor ─── 2 threads: exec, pathfind
  │                 arbitrated by ExecutionCoordinator;
  │                 armed by the bot's start, halted by its stop
  ▼
InputManager ────── SendInput with scancodes: down only into the
  │                 game in front, up wherever focus has gone
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

**Two executor threads.** Command dispatch and pathfinding run on different
clocks; one loop makes the slower set the latency for both. Potions and buffs
are MacroEngine's: while the packet bot runs it is given the HP and MP its
packets carry, so there is one potion engine, not two.

**Nothing pressed while the bot is stopped.** The two threads press nothing
until the bot is started: **START BOT** in
`gobot_gui`, F10 in `gobot_cli`. Every stop -- STOP BOT or F12 in `gobot_gui`,
F10 in `gobot_cli`, a death with `continue_auto_after_death` off, the traffic
going quiet -- halts them at once: the queue emptied, the route dropped, every
key let go of. A death is read from HP reaching 0, and its end from HP coming
back, as well as from `self_dead` and `self_revive` packets.

**One input token.** Those threads all want the keyboard. `ExecutionCoordinator`
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
`gobot_gui` and `gobot_cli`'s packet bot let go of every key they hold —
otherwise the character keeps running into a wall after you alt-tab.

**Down only inside Ctrl.** Standing on a gate, Down takes the character to
another map. `InputManager::never_pressed` keeps Down out of every key call,
and `inject()` -- the one place `SendInput` is called -- refuses every array
that puts Down down unless it is `drop_ctrl_down`'s own chord, with a
Ctrl-down before the Down in that same array; `[Ctrl↓, ↓↓]` from any other
path is refused too. `drop_ctrl_down` is the one call that builds Down's
events: it holds Ctrl first, sends Ctrl and Down together in one array, lets
Down up before Ctrl wherever focus has gone, and owes any up that did not go
out until it does.

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
  of which is solid — and knowing where in it you are. Since 2026.11.04
  `GameMapBook` reads those files' floors, and the `.prj` files' gates, for
  the hunter's drop alone, and `MapTrust` names the map from where you and
  the monsters stand when no map entry was seen -- strictly: one file alone
  fitting at least 12 standing points, the next best missing 6.

---

## A note on use

Botting violates GhostOnline's terms of service, and accounts do get banned for
it. That's between you and the operator — but the randomised input timing in
here is a mitigation, not a cloak.

Use this on your own account and your own traffic.
