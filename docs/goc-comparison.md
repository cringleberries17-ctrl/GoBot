# GoBot against GoC, feature by feature

What GoC does, tab by tab, set against what GoBot does — and what changed after
the comparison. GoC's feature list is taken from its own UI: the Vietnamese
labels and help texts compiled into `go_client.exe`, plus its config files and
screenshots of it running. The labels are quoted as GoC shows
them, so a GoC user can find each one.

Marks: **✓** GoBot does it · **½** in part, or only on the full screen ·
**✗** not built · **—** not wanted.

---

## The one difference everything else follows from

GoC's brain is on its vendor's server: it uploads the game's packets and gets
decisions back (see `goc-reference.md`). So it knows things GoBot can only see:
your position, the map's platforms, every monster's position and HP, your
level, EXP and rage.

Two of GoC's own help texts show how it gets your position, and they agree with
what `protocol-findings.md` found — the server never sends it:

- *"Hãy di chuyển vào Cửa hàng và ra lại để load bản đồ"* — go into a shop and
  back out to load the map. A map change is the one moment the position is
  known; everything after it is worked out from there.
- *"Tự động fix tọa độ sau khi login — Thực hiện xoay hướng zig-zag để cân bằng
  vị trí nhân vật"* — after logging in it walks a zig-zag to settle the
  character's position. That is calibrating an estimate, not reading a value.

So GoC's coordinates are dead reckoning from a map warp. That route is open to
GoBot too. It is the largest piece of work left, and the map view, waypoints,
lure mode and "return to the old spot" all wait on it.

GoBot sees the screen instead: monsters by their name plates and health bars,
HP and MP by reading the figures on the HUD. Most of the gaps below come from
that difference, and most of GoBot's advantages do too. Nothing of GoBot's
depends on a server, a subscription or a login to anyone else.

---

## Main window

| GoC | | GoBot |
|---|---|---|
| Bắt đầu / Dừng (F10) | ✓ | **BẮT ĐẦU (F10)** starts everything ticked. GoBot adds **F12**, which stops everything and lets go of every held key. |
| Bản đồ — live map: platforms, monsters, you, the working area as a box you drag | ½ | GoBot reads the game's own `.map` files (781 ship with it) and has a radar of the name plates it sees. No live map with you on it: that needs your position (above). |
| Thông tin nhân vật — level, EXP, position, direction, HP, rage, target and its HP | ½ | HP and MP, read off the HUD. What it is doing, and the monsters in view. No level, EXP or rage. |
| Đo EXP — EXP this session, EXP per hour | ✗ | |
| Focus Window | ✓ | Keys are only sent while the game is in front, as in GoC. |
| Cài đặt chung — named profiles: Save / Edit / New | ✗ | One set of settings, saved as it changes. |
| Tối / Sáng (theme), Kiểm tra Update | — | |
| The version in the title ("GoC - 0.5.8.6") | ✓ | "GoBot - 2026.10.15", from the VERSION.txt the package carries; also at the bottom of the simple screen and on the first line of gobot_gui.log. A build that was never packaged says *bản tự build*. |

## Tấn công (attack)

| GoC | | GoBot |
|---|---|---|
| Tự đánh — auto attack | ✓ | **Tự đánh quái**: walks to the nearest monster it can reach and hits it. |
| Tự buff — each buff with its own cooldown in seconds; every buff cast again after a login ("Buff state reset") | ✓ | **Tự buff**: each buff key has its own interval in seconds (60–7200), as GoC's buff list has. After a death, a revive or logging back in — anything that keeps the character out of the world for 10 s or more — every buff is cast again, 1.5 s apart. A buff the game did not get (the keys held for a dialog, the game not in front) is tried again 2 s later instead of a whole interval later. Buffs never go out closer than 1.2 s. A seconds box turned down during a run counts from that key's last press. |
| Nhặt đồ — press the pick-up key continuously | ✓ | **Tự nhặt đồ** *(new)*. Pressed a few times straight after each kill, and every 2 s while fighting. |
| Chế độ lười — stand still | ✓ | **Đứng yên một chỗ** *(new)*. Attacks only what comes within reach. |
| Đánh đúng hướng — face the target | ✓ | Turns to face before each swing. |
| Chọn mục tiêu: Tốt nhất / Gần nhất / Thông minh | ✓ | Nearest that it can reach — ignoring ones on a ledge overhead — and the most wounded first, so it finishes kills instead of spreading damage. Corpses (an empty bar) are never picked. |
| Tầm đánh, and *Config Mẫu*: class + weapon → one of four range presets | ½ | The four presets are on the full screen (taken from GoC's own file), but they feed the packet bot, which cannot run. The hunter has one reach and one height of its own. **Next on the list.** |
| Giữ khoảng cách — keep distance | ✗ | The hunter stops at its reach; it never backs off. |
| Đánh khi nhảy, Nhảy đôi | ✗ | It hops to reach a target above. |
| Tấn công quái thấp/cao hơn mà không nhảy | ✓ | The height tolerance. |
| Bỏ qua quái bay — ignore flying monsters | ½ | A switch exists, for the packet bot only. |
| Ưu tiên đánh Boss | ½ | Likewise. |
| Loại mob — filter by kind | ✗ | The hunter reads names off the plates, so a name list is cheap to add. |
| Dùng nộ — use rage when full | ✗ | Rage is not read. |
| Dùng skill tuần tự — skills in order | ✓ | A sequence, on the full screen. |
| Bấm phím nhanh, nhiều phím cùng lúc | — | GoBot presses one key at a time on purpose: two key-downs interleaved and the game sees neither cleanly. |
| Tự động dùng avatar | ½ | As a buff key on a timer. |
| Vượt chướng ngại vật, Tìm đường theo bục (both *test* in GoC) | ✗ | GoBot has a platform pathfinder, for the packet bot. |
| Theo sau một nhân vật (in development in GoC) | ✗ | |

## Hồi máu (healing)

| GoC | | GoBot |
|---|---|---|
| HP Heal / MP Heal, Lv1 and Lv2 — key and threshold each | ✓ | **Tự uống bình HP / MP**, key and threshold. A second tier is possible on the full screen; the simple screen shows the first. |
| Tự động hồi sinh tại chỗ, Số lần thử | ✓ | **Tự hồi sinh**, the way GoC does it: the death panel is found by GoC's two button corners (dialog_Panic), **Sử dụng vật phẩm hồi sinh** is clicked, then Xác Nhận on the question — only at a death, once the panel has been seen, as GoC only looks for the OK after its own click on the panel; a look-alike question with nobody dead is left alone. The first click is aimed from a fresh look, taken once the mouse is free and the game in front. Three tries for each death (GoC's player_revive_max_attempts). Never the village button (−2% EXP), the Cash button or the Khóa ticks, and nothing at all while the trash bin's dialog is up — the revive question's picture is close to the bin's. When the three tries fail — most likely out of revive items — the whole bot stops and says why in red; GoC gives up silently. A revive that HP read above 0 confirms ends that death there and then, so a character killed again straight after a real revive gets three fresh tries and the bot never stops while items last; with HP unreadable, a try is given back only after a minute up, so a game that only hides the panel for a while with no item left still ends in the stop. **Thông báo** tells of the death and, once it is sure, of the revive. |
| Pet Heal, Bơm liên tục, Hồi sinh pet, pet loot | ✗ | The pet pictures exist. |
| Xử lý máu / mana cash | ✗ | |

## Tọa độ (waypoints)

| GoC | | GoBot |
|---|---|---|
| Lists of points, add where you stand, wait at each, round trip or back and forth, lure (*Lùa quái*), clear N% before moving on, search area around each point | ✗ | All of it needs your position. The full screen has saved places and a walker that steers by the scenery scrolling past, which is not good enough to build this on. |

## Tổ đội (party)

| GoC | | GoBot |
|---|---|---|
| Tự động nhận Party | ✓ | On the new **Tổ đội** tab. |
| Chấp nhận tất cả / Chỉ nhận Party từ player sau | ✓ | **Fixed in this round.** GoBot read the "only accept from" list and never used it: accepting went through the *invite* rules, so the list let everyone in. |
| Tự động Party — invite whoever chats the magic word | ✓ | Whole-word match, so "spotted" is not "pt". |
| Party theo danh sách — only invite names on the list | ✓ | |
| Danh sách đen | ✓ | Wins over every other list. |
| Luôn làm chủ party, chỉ mời khi đang làm chủ | ✗ | |
| Danh sách ưu tiên (giữ slot) | ✗ | Read from the file, not acted on. |
| Tự chat khi không có party | ✗ | The typing is there now (**Tự chat**); a line said only while not in a party is not. |
| EXP / loot share | ✗ | Read from the file, not acted on. |

## AutoLogin

| GoC | | GoBot |
|---|---|---|
| Bật AutoLogin — log back in when disconnected; Kênh (1~8), Nhân vật (1~3) | ✓ | On the **Tự đăng nhập** tab: watches for the client exiting, hanging or showing an error box, ends it, presses Chơi game in VTCZone (starting VTCZone first when it is closed), and picks the channel and character. Like GoC it needs no account and no password: VTCZone stays signed in, and when it shows its sign-in form instead GoBot stops and says so rather than typing into it. With the game closed, BẮT ĐẦU opens it first. VTCZone installed somewhere other than configs/login.ini says is started from where it was last seen running, its path read off the running program. |
| ClickLauncher2Start accepts "Security Alert" boxes (&Yes) | ✗ by choice | GoBot stops and says so: whether to trust a certificate is the player's call. |
| ClickLauncher1Start restores a minimised VTCZone | ✓ | GoBot restores it too, and a window of the crossing that is minimised part way. |
| WaitForGameFocus / ExecuteStage — brings the game to the front and holds a stage until it has the focus | ✓ | GoBot does the same for every window of the crossing — VTCZone, the game's launcher and the game — and also checks that the exact point it is about to click lands on that window. If another window is over the spot (a browser, a chat, GoBot itself) it waits and says what is in the way, and gives the attempt up after 30 s, rather than clicking whatever is on top. A window kept over the game while a page or the HUD is awaited is named the same way -- any window over any part of the game's client area, not only over a few points of it, so a page hidden behind a small window is never taken for one that is not there -- and a game dragged part off the screen is still looked at. |
| Never goes back to VTCZone while a client is open; CheckGameClientOpen minimises VTCZone and brings the game forward | ✓ | Each attempt looks at the game and its launcher before VTCZone. While either is open GoBot never presses Chơi game — not even when VTCZone's own picture still shows it — and carries on from the game's own page; a game already in the world ends the crossing at once with nothing clicked (`gobot_cli --login` included). VTCZone is minimised once the game's window is up. |
| KILL_GAME_PROCESS — a client it cannot place is ended and the sequence starts again | ✓ | A half-started game that stays on a screen the crossing does not know — none of its pages and no HUD — for the page's wait is ended the way a crashed one is, and after the cool-down the next attempt starts from VTCZone. Not after the last attempt: that one is left on screen for the player. |
| A stage that cannot click its target is retried 3 times, then fails | ✓ | A click point outside its window — an offset typed wrong in configs/login.ini — is said at once, naming the `[Page.*]` to put right, rather than waited on and retried. |
| — | ✓ | **Fixed in this round:** after logging back in GoBot now follows the new game window. Before, the client it started was a new window and everything still pointed at the old one, so the run the login saved stopped anyway. |
| — | ✓ | GoBot gives up after five restarts and waits between them, so a locked account or a bad patch does not become a reconnect loop against the server. |

## Hệ thống (system)

| GoC | | GoBot |
|---|---|---|
| Không dừng Auto khi nhân vật chết | ✓ | GoBot does not stop for a death it revives from: the keys hold while the HUD is gone — dead, loading, disconnected — and carry on when it is back, with every buff cast again. When **Tự hồi sinh** fails three times it stops everything, lets go of every key and says why — the player's rule: never revive in the village. (GoC stops at every death unless this is ticked.) |
| Tự động focus cửa sổ game khi mất focus | ✓ | **New.** During a run, when something else has taken the focus and nobody has touched the mouse or keyboard for 30 s. Waiting for the machine to be idle is what stops it fighting a person for their own desktop. |
| Trở về địa điểm cũ, Trở về map cũ, Sai số trở về | ✗ | Needs your position. |
| Tự động fix tọa độ sau khi login | ✗ | Likewise. |
| Kiểm tra Chat Input | ✓ | **Kiểm tra ô chat** on the **Tự chat** tab, and `gobot_cli --chatcheck`: whether the chat line is open or shut, found the way GoC's own check finds it, with the numbers that decided it. Presses nothing. |
| Telegram: death, disconnect, chat, party changes, remote control | ✓ | The **Thông báo** tab, through a **Discord webhook** of the player's own instead of Telegram, plus a beep and a Windows notification on the PC that never take the game's focus. GoC's own client only reports a disconnect and a login, and sends its login token and account id to its server, which makes the death, chat and party messages out of the packets the client uploads; GoBot has no server and sends nothing to anybody but the player's own channel. It tells of a death (from the death dialog, or HP read as 0 when Tự hồi sinh is off), a revive and a revive that failed, a disconnect -- the game gone, hung or showing an error box -- and getting back in, the keeper giving up, the character out of the world for minutes, the trash bin's dialog or the chat line left open, Tự chat stopping itself, and, if ticked, party changes. Every event has its own tick. Repeats within a minute go as one (×N), at most 5 a minute and 30 an hour; what could not go while offline goes later with its own time, and a 429 from Discord is waited out, a 404 stops sending until a new webhook. Messages are fixed Vietnamese sentences and numbers: never the account, a password, the character's name, the map, a screenshot or another player's words (names in party messages only if ticked). The webhook is encrypted with DPAPI -- as GoC keeps its own credentials -- outside the program folder, shown only as its last 4 characters, and never logged; WinHTTP posts it, as in GoC. Not copied: forwarding other players' chat, and remote control (a two-way Zalo bot may come later). |
| Khung giờ reset avatar — switch character to reset the avatar timer | ✗ | |
| Giảm tải khi map đông người | — | Nothing to reduce: GoBot reads the screen, not the monster stream. |

## Vật phẩm, nhiệm vụ, chat

| GoC | | GoBot |
|---|---|---|
| Xử lý vật phẩm — Xóa rác / Vứt rác: delete or drop junk on a timer | ✓ | **Dọn rác** tab. Pick the junk once from the client's own item icons (fragments, materials, potions, buffs, seals) -- the same library GoC picks from -- or photograph the bag for anything else. While the bot runs it opens the bag shortly after START and then every 5–60 minutes, drags each listed item onto the trash bin, clicks the bin, and answers the bin's own dialog -- told apart from every other dialog by its words. The game fills the quantity box with the whole stack itself, and GoBot never clicks or types in it -- GoC's own trash flow types nothing there either. Xác Nhận is pressed only when the box reads, look after look through a blink of its caret, exactly the count read off the slot in the game's own digits; a box holding anything else -- another number, nothing, a number that does not read -- means the count was misread or the bin holds some other item. That, anything else unexpected, or a stop, and it answers Hủy Bỏ, so the item goes back to the bag. A slot is matched by its drawing, not its colour -- the fragments in the bag pulse gold to red, and GoC's own pictures of them are red -- so picking an icon also picks the same drawing in other colours, and the tab shows which before anything is deleted; a slot that looks as much like an icon that was not picked, drawn differently, is left alone. *Thử tìm* says what it would throw away without throwing anything. Dropping (Vứt rác) is not done: there is no picture of the drop question with its words to tell it from other dialogs. |
| Xử lý vật phẩm — Sử dụng mảnh ghép (use fragments) | ✗ | |
| Mua sắm — buy potions from the Hawker / Người giữ sân, safe place first, exact quantity | ✗ | Click sequences exist for buying a rod and a pickaxe; potions are the same shape. |
| Làm nhiệm vụ nhanh | ✓ | The **Nhiệm vụ** tab, done the way GoC does it for the Vietnamese VTC client, with GoC's own pictures (`quest\1`) and points: during a run, every few minutes (5 by default), GoBot opens the Nhiệm Vụ window with the quest key (U, GoC's default), clicks NV Phụ, and ticks the game's own **Tự Động** there -- but only after reading its box, so an option already on is left alone (GoC clicks it without looking whether it is ticked already, which may untick it; the live test is to settle what a second click does), and only one click a round. It closes the window again only if it opened it, or if an earlier round opened it and could not see it shut. Everything else that presses keys waits for those few seconds; HP falling under the potion's mark ends the round at once so the potion can go. A timed round waits while the game is not the window in front, rather than pull it forward over whatever the player is doing; *Làm ngay 1 lần* brings it forward, but only in the few seconds after the click -- a round asked for that had to wait longer waits for the game to be in front too. A round cut short by the player going to another window is not counted as a failure. It never clicks Hủy Nhiệm Vụ, the quick quest's Accept / Give up / Complete buttons, the hide-low-level-quests checkbox, another tab or anything outside the window, and types nothing. Three failed rounds in a row stop it, with a red line on the tab and a quest_trouble alert. GoBot has no server telling it when a quest finished, so it looks on a timer instead, less often while the option stays on. Not done: GoC's English-client flow -- choosing a quest out of two hundred pictures of English quest names, Accept, Complete -- which the Vietnamese client never shows; with the English client the tab says so and does nothing. *Thử xem* looks without pressing anything. |
| Bật chat — messages on a timer | ✓ | The **Tự chat** tab: up to ten lines, one every 30–3600 s, in turn or at random. Typed as GoC types them -- Enter, six Backspaces, a key per letter, digit or space, Enter -- but with a look at the screen before each key: nothing is typed until the line is seen open and cleared, every key goes only while it still is, Enter only on a fresh look that shows no dialog (it would answer one), and a line that goes wrong is rubbed out rather than sent. Everything else that presses keys waits while a line is typed, and for as long after as the line may still be open, until a look shows it shut. GoC's per-message delay, repeat and party-request flags are not copied, nor any way of typing through the clipboard. |
| Captcha — noticed by its packet, and answered | ✗ by choice | None, by the player's decision: GoBot neither watches for a captcha nor answers one, and nothing in it reacts to one. |

---

## Where GoBot is ahead

- **No server.** GoC stops when its subscription does (*"Bản quyền: HẾT HẠN"*) or
  when the account is in use on another machine. GoBot has no licence, login or
  connection to anyone.
- **F12 panic stop** — everything off and every key released, from inside the game.
- **Holds while not in the world.** A disconnect at 2am does not become six hours
  of potion keys pressed at a dialog.
- **Reads the HP and MP figures**, not just the bar length, and finds the bars by
  itself when the HUD is resized.
- **Restart limits.** Five tries and a cooldown, then it asks for a person.
- **A first screen that says what is missing**, in Vietnamese, with the fix beside
  each line, and settings that save themselves.

---

## Changed in this round (the 2026.10.15 package)

1. The simple screen has seven tabs: **Đánh quái**, **Tổ đội**, **Tự chat**,
   **Tự đăng nhập**, **Dọn rác**, **Nhiệm vụ**, **Thông báo**. BẮT ĐẦU starts
   whatever is ticked on any of them.
2. **Tự hồi sinh** with a revive item, never the village; three tries a death,
   then the whole bot stops and says why.
3. **Tự buff**: each buff key on its own time in seconds, every buff cast again
   after a death, a revive or logging back in.
4. **Tự đăng nhập** needs no account: VTCZone stays signed in. Three tries,
   then it stops and waits. With the game closed, BẮT ĐẦU opens it first.
5. Both client sizes, 1024x768 and 1280x720: the monster box, the chat lines
   and the player list follow the client's size.
6. **Dọn rác**, **Tự chat**, **Nhiệm vụ** and **Thông báo** (Discord alerts).
7. No captcha handling at all, by the player's decision.
8. The package: everything that acts on the game ships off, with no player's
   names or keys in it; a Vietnamese guide, DOC-TRUOC.txt; the version in the
   title and on the simple screen; the full glyph set; and no path of the
   machine that built it inside the programs.
9. A GoBot folder named with Vietnamese letters works: every path GoBot reads
   or writes goes to Windows as Unicode.
10. Whatever has never run on the real game is marked *Thử nghiệm* where the
    player meets it.
11. The hunter finds your character by its name only, on every frame: a plate
    with bars -- another player's, a party member's -- is never taken for you,
    and without a name typed hunting does not start. Before, with any player
    in view, the nearest of them was "you".
12. Auto login counts only what lies over the game on a screen: a taskbar set
    to hide itself, or a window past the screen's edge, no longer holds the
    crossing, and the taskbar's own advice says to let it hide itself.
13. The package: `gobot_gui.log` starts afresh each session (one `.old` kept),
    START-HERE.txt is CRLF, THIRD-PARTY-NOTICES.txt carries the licences of
    OpenCV, zlib and Dear ImGui, and the zip names its entries with '/'.
14. Party switches are off in the code too, and the party worker does not run
    on defaults when party.ini cannot be read.

## Next, by value for the work

1. **Attack style for the hunter** — GoC's *Config Mẫu*: pick class and weapon,
   get the reach and the heights above and below. The four presets are already
   in GoBot; the hunter needs separate up/down heights to use them. Small.
2. **Monster list for the hunter** — names to skip, names to go for first (bosses).
   The names are already read. Small.
3. **Second potion tier on the simple screen** (GoC's Lv1/Lv2). Small.
4. **Buying potions** from the Hawker — a click sequence like the rod and pickaxe
   ones, plus a check for an empty potion slot. Medium.
5. **The party advert** when not in a party, now that lines can be typed. Small.
6. **EXP per hour**, read off the HUD. Medium.
7. **Your position** by dead reckoning from a map warp, as GoC does. Large; it
   unlocks the map view, waypoints, lure mode and returning to the spot.
