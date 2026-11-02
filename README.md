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
  offset 4** says which: 0, 1 or 9. The monster's slot is at `@8` and its
  position at `@24`, `@26`, not at the `@80`, `@82` that `[Packet.0x0038]`
  still reads, which is a stale point beside it; a hit carries the damage and
  the HP left, below zero for a kill. With the map's monster table at each map
  load, that is every monster's position and HP on the whole map, as GoC has
  them (protocol-findings §4f). Nothing uses this yet: plan item P0-3.
- **Your own position is sent by the client, over UDP**, whenever a walk
  starts, stops or turns, a jump starts or lands, and with each skill cast;
  the server sends it only at a map entry (protocol-findings §4e). Memory
  reading is closed off by XIGNCODE3, and the on-screen `WorldAnchor` never
  converges, so these packets are the route; nothing uses them yet (P0-3).
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
With *Đứng yên một chỗ* the character turns to a monster only when it is not
facing it already. A hurt monster is gone for first only when it is within
reach, and one chased for `give_up_ms` (4 s) without coming within reach --
behind a wall, up on a ledge -- is passed over for twice as long, one on
*Đánh trước* too; the box under BẮT ĐẦU says so while nothing else is in
view. Under it, **Lọc quái** opens *Danh sách quái*: monsters
never attacked (*Không đánh*) and monsters gone for first (*Đánh trước*),
picked from the names being read (*Quái đang thấy*; **Nhìn quanh** reads the
screen for up to a minute and presses nothing) or typed. **Tự dùng kỹ năng**
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
presses Down, whatever key is set to it: standing on a gate, Down takes the
character to another map. Key boxes refuse it, and a jump key set to it is
marked red on the checklist.

**Vị trí từ gói tin**, beside *Giữ khoảng cách* (*Thử nghiệm*), takes your
position and the monsters' from the game's own packets instead of the name
plates, which drop out for a frame or two all the time. It needs Npcap;
GoBot already runs as administrator. Whenever hunting starts -- unless
`packet_positions=off` in `hunt.ini` -- GoBot captures the game's own TCP and
UDP, found by the game window's process -- its connections and UDP sockets
alone, so a second game client on the PC stays out -- and reads them: nothing
is sent or changed. A capture already running (the Protocol tab's,
Autopilot's, the packet bot's) is used as it is and never stopped by the
hunting; one stopped by hand stays stopped until the next hunt; and when the
game is started again, reconnects, opens a new socket or goes quiet for 5 s,
the capture is started again, at most every 10 s -- less and less often while
the capture itself refuses to start, on an adapter it cannot use. Unticked
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
you* (`jump_to_reach`) on, and does not walk back to one higher. GoBot never
presses Down, so a spot below is reached only by walking off a platform's
edge toward it, where the platform allows that.
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
in other colours, and the tab shows which. Only the bin's own dialogs are
answered, told apart by their words (the fragment and charm questions below are
the exceptions), and an item that looks as much like an
icon you did not pick, drawn differently, is left where it is. Two more things
the tab can do with items on its *Dùng* list, both off until ticked: *Dùng mảnh
ghép* registers fragments in Yêu Quái Bách Khoa, and *Dùng bùa* uses charms
(Bùa) by a right-click in Tiêu Dùng. A charm costs one and lasts an hour, so
each kind is used once and not again until *dùng lại sau* has passed (kept in a
file, so a restart does not use another), in a quick run of its own in which
skills, potions and buffs wait; Xác Nhận is pressed only on a box with the
fragment question's frame and buttons (a score of 0.92 or more, set in
trash.ini) whose words are not the bin's, the revive or the sell question --
GoBot has no picture of the charm question itself yet, and the drop question
is told by its frame alone. *Dùng bùa ngay* uses them at once. Beside them, *Tự mua bình* (also off until ticked) buys
Thịt bò khô and Rượu lá tre from the shop the Người Giữ Sân item of the bag's
Khác tab opens (found by the game's own icon for it, nothing to photograph),
when Tiêu Dùng holds fewer slots of them than you set: it double-clicks the
potion in the shop, types 00 after the game's 1 in the quantity box, reads it
back as 100, and only then presses Xác Nhận; anything unexpected is answered
Hủy Bỏ. Skills, potions and buffs wait while it buys, and *Thử mua* goes as far
as the quantity box without buying. *Tốc độ bấm nút* (60, 80, 100 or 150 ms)
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
where you left it.) It reads the box before clicking, so an
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
| `configs/app.ini` | the game's window, the client keeper, the simple screen's switches; capture interface and server address; whether `gobot_gui.log` starts afresh each session (`new_log_each_start`, on as shipped: the session before is kept as `gobot_gui.old.log`) |
| `configs/macro.ini` | the macro's keys -- skills, potions, buffs -- and the sequence |
| `configs/hunt.ini` | hunting: your character's name, the attack and pick-up keys, the reach; where positions come from (`packet_positions`: off, watch, on) and how the packets lie over the screen (`packet_scale`, `packet_dy`); the hunting spot (`spot`, its area and when it walks back) |
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
  of which is solid — and knowing where in it you are.

---

## A note on use

Botting violates GhostOnline's terms of service, and accounts do get banned for
it. That's between you and the operator — but the randomised input timing in
here is a mitigation, not a cloak.

Use this on your own account and your own traffic.
