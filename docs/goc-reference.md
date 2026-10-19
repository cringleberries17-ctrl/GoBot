# The GoC client: what is in it, and what GoBot takes from it

Reference notes on ZTeam's GoC client at `C:\Program Files\ZTeam\GoC`, the
commercial bot GoBot replaces. Written from the shipped binaries and data files
— 543 demangled exports, the INI schemas, and 1,334 PNGs. On 2026-10-17 both
binaries were also decompiled in full. What the code itself shows is in
[Code-level findings](#code-level-findings-full-decompile-2026-10-17), and it
corrects a few statements below.

Two things to get from this document:

1. **What GoC actually is**, including the architectural fact that decides what
   can be reused at all.
2. **A reuse ledger** — what GoBot already takes, what is still worth taking,
   and what there is no point looking for.

The headline is in [Worth taking](#worth-taking): the glyph set GoBot loads is
missing four characters, and that is the cause of a misread already written up
in `START-HERE.txt`.

---

## GoC is a thin client with a remote brain

This is the fact everything else follows from, and it is worth restating because
it bounds reuse so sharply.

`GhostCore.dll` sniffs the game's TCP stream with Npcap, filters frames against
an opcode allowlist, encrypts the batch (X25519 ECDH + AEAD) and uploads it to
`login.chamthoi.com:443`. The server replies with a `BotCommand` list, which
`CommandExecutor` replays as keystrokes.

The evidence, in the binaries:

- `ProtocolHandler::DecodeCommandBatch(…) → vector<BotCommand>` — the client
  *receives* its decisions. It does not make them.
- This format string in `GhostCore.dll`:

  ```
  DecodeLoginResponse: captureConfig - opcodes=%zu, ipRange=%s-%s, portRange=%u-%u
  ```

  The opcode allowlist, the game server IP range and the port range all arrive
  **in the login response**. The protocol knowledge was never in the binary.
- Few game opcodes are hardcoded: `0x73D` (captcha), `0x14` (channel switch /
  character selected), `0x38`/`0x42`/`0x51` (login or map load done, watched
  only during auto login), the `0x81` wrapper, and `0x2E`/`0x31` (dropped on
  UDP). See [protocol-findings.md §4d](protocol-findings.md#4d-what-gocs-own-parser-does-2026-10-17).
- The handshake verifies a server signature —
  `[Handshake] CRITICAL: Server Signature Verification FAILED! Possible Fake Server/MITM!`
  — so there is no standing up a fake server to watch the protocol either.

So the split is:

| In the folder, reusable | On their server, not reusable |
|---|---|
| 1,334 template PNGs, three glyph sets | Game packet → game state decoding |
| Config schemas and tuned defaults | The opcode table and packet layouts |
| Consumable/pet/seal icons by item id | Monster selection (which target) |
| Feature state machines (in log strings) | Patrol route logic |
| Scancode input, jitter, focus gating | |
| Fight rules, movement physics, input timings (in GhostCore's code) | |

**Assets, schemas, tuning values and the client-side fight rules transfer. The
protocol and target choice do not.**
That is also why "remove the login check from GoC" was never going to leave a
working bot behind — the subscription was load-bearing, not a bolt-on.

---

## Files

### Binaries

| File | What it is |
|---|---|
| `go_client.exe` | 1.8 MB, x64, linked 2026-09-10. Dear ImGui on Direct3D9. Hosts every **vision** feature: `CaptchaFeature`, `ShoppingFeature`, `ItemManagementFeature`, `ItemBuffFeature`, `FillCashShopHpFeature`, `QuestFeature`, `PartyFeature`, `RecoveryFeature`, `RevivePetFeature`, `AutoLogin`, `FeatureUtils`. |
| `GhostCore.dll` | 1.2 MB, x64, 543 exports in namespace `GhostClient`. Capture, protocol, crypto, input, config, pathfinding. Imports `wpcap.dll` + `packet.dll`. |
| `Uninstall.exe` | x86, 2024. Installer leftover. |

Third-party DLLs are all stock: `opencv_world4` (19 MB, the whole vision stack),
`libpng16`/`jpeg62`/`tiff`/`libwebp`/`zlib`/`liblzma` (OpenCV imagecodecs),
`WinSparkle` (auto-update).

**`libprotobuf.dll` (13 MB) and `abseil_dll.dll` are not imported by either
binary.** They are `opencv_world`'s DNN dependency. Protobuf is not the wire
format — there are no `.proto` files to find.

### Config files

| File | Holds |
|---|---|
| `config.ini` | `server_host=login.chamthoi.com`, port 443, heartbeat 10 s, reconnect 5 s, `packet_batch_interval=50` |
| `credentials.dat` | 336 bytes, their subscription login. Not opened. |
| `item_processor.ini` | Vision-feature config: intervals, key codes, shopping, detection thresholds, AutoLogin process/window names |
| `configs/bot_s0_default.ini` | The combat parameter set — 7 skill slots, 3 buff slots, `[Heal]`, targeting, movement, patrol, `[KeyConfig]` |
| `configs/party_s0_default.ini` | Auto-accept, magic word `pt`, lists, share types |
| `sample_config.ini` | **Four named attack-geometry presets.** See [Worth taking](#worth-taking). |

`bot_s0_default.ini` is worth reading in full even now — it is a thought-through
feature list, and GoBot's `bot_default.ini` already carries its schema and
defaults verbatim (`attack_range=350`, `facing_tolerance=12`,
`highest_hit_y=150`, `lowest_hit_y=63`).

### Data: 1,334 PNGs

| Path | Count | What |
|---|---|---|
| `data/bitmap_font/{upper,lower}` | 179 | Per-glyph OCR templates. **Incomplete — see below.** |
| `data/images/bitmap_font/{upper,lower}` | 183 | The complete set |
| `data/bitmap_font_en/{upper,lower}` | 62 | ASCII subset |
| `data/images/inven/0`, `/1` | 28 + ~60 each | Inventory chrome, buttons, dialogs — **two skins** |
| `data/images/login/1` | 30 | Server/channel/character-select anchors |
| `data/images/quest/{0,1}` + `category/0..13` | ~420 | Quest dialogs and per-quest text crops, indexed by `categories.json` |
| `data/images/spend/{hp,mana,buff,fragment,craft}` | 55 | Consumables, filenames are game item ids |
| `data/images/pet/pet` | 70 | Pet icons, all 32×32 |
| `data/images/seal` | 22 | Seal items |
| `data/images/captcha` + `number` | 37 | Not used, by policy |
| `data/images/equip` | **0** | Empty in GoC too. Equipment detection was planned, never shipped. |
| `data/guide` | 10 | Help screenshots for their own UI |

Shape of the set: 489 RGBA (alpha-masked), 424 grayscale (the glyphs), 417 RGB,
4 palette; all 8-bit but two.

The `inven/*/​*.spr/` subfolders are named after the game's own sprite archives
with the frames extracted inside — which is where all of this came from.

Glyphs are **proportional**, 1–9 px wide and 6–13 px tall, so segmentation
cannot assume a grid. `upper/` and `lower/` are separate directories because
`a.png` and `A.png` cannot coexist in one folder on Windows.

---

## The 543 functions

Grouped by subsystem, largest first. Useful mainly as a map of what a finished
version of this looks like.

**`CommandExecutor` (109)** — the actuator. Threads: `ExecutionThread`,
`AttackThread`, `BuffThread`, `HealThread`, `LootThread`, `ChatThread`,
`PathfindingThread`, `ReturnToOriginThread`, `ReturnOldMapThread`. Handlers for
move/jump/face/skill/buff/target/attack. Confirmation hooks
(`OnAttackConfirmed`, `OnSpellBaseConfirmed`, `OnMapWarp`) — it waits for the
server to confirm an action landed before advancing. Constants:
`MAX_SKILL_RETRIES`, `BUFF_RETRY_DELAY_MS`, `STUCK_THRESHOLD_MS`,
`STUCK_DISTANCE_THRESHOLD` (3, 1000 ms, 2000 ms, 5.0 px). `PerformHealCheck`
is an empty function: potions are pressed by go_client.exe (see
[Code-level findings](#code-level-findings-full-decompile-2026-10-17)).

**`ProtocolHandler` (84)** — the client↔vendor protocol. Senders for config,
patrol, map requests, monster filter, heartbeat. ~30 `Decode*` methods:
`DecodeGameState`, `DecodeMonsterUpdate/Remove`, `DecodePlayerMove/Jump/Exp`,
`DecodeHPMPUpdate`, `DecodeParty*`, `DecodeMap*`, `DecodeWarpGateAreas`,
`DecodeSelectMonsterInfo`. **Read this list as a target spec** — it says which
packets are worth having without saying how any of them are laid out.

**`InputManager` (56)** — `CaptureGameWindow() → cv::Mat`, `GetScanCode`,
**`GetRandomDelay`**, key down/up/press, movement and jump primitives,
`SendChat`, `SetBlockAttack`/`SetBlockMovement`, `SetFastKey`.

**`Config` (53)** — INI singleton, profile management, `LoadPatrolMap`,
`ShouldForwardOpcode`, `MigrateOldConfig`.

**`PacketCapture` (34)** — Npcap. `StartCapture(iface, ipRange, portLo, portHi)`,
`ExtractGamePacket`, `ShouldForward(opcode)`, `BatchThread`, adapter
enumeration, callbacks for captcha/disconnect/login/char-select/channel-switch.

**`PathFinder` (20)** — platform-graph A*: `BuildPlatformGraph`,
`FindClosestPlatform`, `GetWalkCost`, `Heuristic`, and crucially
`SimulateMove`/`SimulateDoubleJump` to validate each edge against a tile
attribute grid (`GetRawAttr`, `GetHeightAt`). Needs map data, which their server
supplies.

**`TCPClient` (20)**, **`HTTPClient` (20)** — Winsock wrapper; and the licence
REST surface (`/api/login`, `/api/subscriptions`, `/api/payment/create`,
`/api/coupons/redeem`, `/api/telegram/*`). None of the second is of any use here.

**`ExecutionCoordinator` (17)** — one input token, by priority, with
`EngageCriticalMode`. The best idea in the codebase; GoBot already has it.

**`WindowFocusMonitor` (16)** — polling thread with `POLL_INTERVAL_MS` and
`DEBOUNCE_MS`. Input is gated on focus.

**`CryptoHandler` (14)** — X25519 `GenerateKeyPair`/`ComputeSharedSecret`/
`DeriveSessionKey`, 12-byte nonce, `Encrypt`/`Decrypt`. Constants in the DLL
point to ChaCha20-Poly1305 and an Ed25519-style (SHA-512) signature check.

**Free functions** — `GenerateHWID() → array<u8,32>`, `GetStateName(BotState)`,
`IPv4ToUint32`. `GenerateHWID` returns 32 zero bytes and nothing calls it.

The rest are plain result structs (`LoginResult`, `Subscription`, `Plan`,
`PlatformEdge`, …), ctor/dtor/assign only.

---

## Code-level findings (full decompile, 2026-10-17)

Both binaries were decompiled in full with Ghidra 12.1.4: 2,622 functions in
`GhostCore.dll` and 2,635 in `go_client.exe`. This is static reading only;
nothing was run. GhostCore's exported names make most of it readable.
go_client.exe has no symbols, so its functions are cited by address. Inferences
are marked as such. Option names are the INI keys in `bot_s0_default.ini`.

### Corrections to the sections above

- **More of the fight runs on the client than "a remote brain" suggests.** The
  server picks the target and sends move and skill commands. How a command is
  carried out is GhostCore code with fixed rules, given below: keep distance,
  facing, jump attacks, skill gating, buffs, fury and loot. Those rules transfer.
- **Healing is not in GhostCore.** `CommandExecutor::PerformHealCheck` is an
  empty function; it shares its address with the empty `HandleStartAttack`.
  Potions are pressed by go_client.exe (`FUN_1401174b0`).
- **There is no hardware ID.** `GenerateHWID` returns zeros and is never called.
  The licence rests on the account login and server-side session counts
  (`maxSessions` and `currentSessions` in the login response).
- **More game opcodes are hardcoded** than the two listed in earlier versions of
  this page. See [protocol-findings.md §4d](protocol-findings.md#4d-what-gocs-own-parser-does-2026-10-17).

### Input timing (`InputManager`)

| Action | GoC |
|---|---|
| Any key press | Down, hold 60–131 ms (uniform, MT19937 seeded from `random_device`), up. A caller asking for less than 51 ms gets a random hold instead. |
| Injection | `SendInput` with scancodes only (`wVk = 0`, `KEYEVENTF_SCANCODE`). The extended flag is set for the arrows, PgUp/PgDn, Home/End, Insert and Delete. |
| Double jump | Jump, 341 ms, jump. |
| Jump down | Hold the modifier, 50 ms, tap Down, 50 ms, release. |
| High jump | Hold the modifier, 50 ms, tap jump, 50 ms, release. |
| Facing | Tap Left or Right. |
| Default keys | Jump = Up arrow, down = Down arrow, modifier = Shift (constructor). The modifier is the key GoC's setters call the loot key. |
| Fast-key mode | A background loop releases keys 100–131 ms after they go down. At most 10 are held at once. |
| Chat | Enter, then a screen check that the chat box opened (3 tries). Then 6 × Backspace, and letters, digits and spaces only, 33 ms apart, with Shift for capitals (Caps Lock respected); anything else is skipped silently. Then Enter and a check that the box closed (5 tries, then Right arrow). |

### Fight rules (`CommandExecutor`)

- **Attack loop** (`AttackThread`). Runs only while the server reports state 5
  (attacking). It presses each enabled attack skill once, 300 ms apart, and
  idles for 100 ms when there is none.
- **Server skill command** (`HandleUseSkill`, command `0x10`, GhostCore
  0x180046A80). In this order: the jump attacks, the MP check, keep distance,
  facing, then the skills -- or the normal attack when no skill went out.
  - Skipped in two player states, inferred to be dead and loading. While a chat
    line is being typed, it waits 500 ms instead.
  - Below 10% MP it presses no attack keys at all -- no skill, and no normal
    attack -- and logs this at most every 5 s. The 10% is fixed in code. Only
    the jump attacks come before this check: keep distance, facing and the
    skills all come after it.
  - **Keep distance** (`backing_away`, never in `lazy_mode`). When the target
    is closer than `min_range` (50 px if unset), it steps back
    (min_range − distance) + 20 px. The step is clipped to the platform's ends
    and to the working area. It holds the arrow for distance ÷ 355 px/s,
    clamped to 50–800 ms, then waits 50 ms.
  - **Facing** (`face_target`). It taps toward the target when the target is
    more than `facing_tolerance` px to the side. With facing off, it turns back
    to the direction it started in.
  - **Skills**, sequential (`sequential_skill` on, as in the player's config).
    It presses every enabled skill in list order, each a random 60–131 ms press
    and then 50 ms, with no cooldown check in the client: ZTeam's server paces
    them. A skill is skipped when its `skill_N_aoe_max_x` is set and smaller
    than the horizontal distance (0 means any distance).
  - **Skills**, not sequential. It presses only the first enabled skill in range
    whose cooldown, in seconds, has passed.
  - Either way, when no skill went out it presses the normal attack
    (`key_normal_attack`). A press stays pending until the server confirms it
    (`OnAttackConfirmed`, `OnSpellBaseConfirmed`). GoBot's "Chỉ khi đang đánh
    quái" follows this, with a cooldown of its own on every skill in both
    modes, since no server paces GoBot's presses.
  - **Jump attack** (mode 1). A target more than `higher_threshold` px off
    vertically, and within that + 132 px, gets one jump. Further than that gets
    a double jump.
  - **Jump-down attack** (mode 2). Only from the ground, at most once per
    800 ms, and never on a warp gate. Then a 341 ms pause.
- **Buffs** (`BuffThread`, every 500 ms):
  - A buff is queued when its cooldown has passed and at least 5 s have passed
    since it was last queued.
  - Each cast presses the key 4 times, 200 ms apart (`HandleUseBuff`, "(4x)").
  - A cast counts only when the server confirms it (`OnSpellJobConfirmed`,
    status 0). Otherwise it is retried.
  - At start every timer is set to 225 s ago, so buffs with a cooldown of 225 s
    or less go at once. `ResetBuffState` sets them to 1,800 s ago, which fires
    every buff.
- **Fury** (inside `HealThread`). With `auto_fury` on, it presses the fury key
  when SP ≥ (`fury_skill_points` + 10) × 120, at most once per 3 s. That is
  1,200 SP with the default 0. `fury_skill_points` is the 'Skill 130' box
  beside 'Dùng nộ'. Fury is not tied to attacking.
- **Loot** (`LootThread`). Presses the loot key every max(100 ms, interval),
  but skips it while Up or Down is physically held (`GetAsyncKeyState`), so a
  climb is not broken.
- All of these pause while the game window lacks focus (`WindowFocusMonitor`:
  100 ms poll, 100 ms debounce), and while a captcha or a chat line is in
  progress.

### Movement

**Physics** (`PathFinder::SimulateMove`, `SimulateDoubleJump`,
`BuildPlatformGraph`):

| Quantity | Value |
|---|---|
| Simulation step | 0.05 s |
| Gravity | 2400 px/s² (+120 px/s per step) |
| Sideways speed, walking and jumping | 360 px/s (355 px/s in the executor's walking maths) |
| Normal jump | −800 px/s, so about 133 px high |
| High jump | −1440 px/s, so about 432 px high |
| Double jump | a second push at the top of the first |
| Tile | 32 px |

Graph edges and their costs:

- Walk off an edge, left or right: 0.1.
- Fall: 0.3.
- Normal jump: 0.8 straight up, 0.7 to the left or right.
- High jump: 2.5 straight up, 2.4 to the left or right.
- Drop down, and double jump.

**Path following** (`PathfindingThread`, 40 times a second). It holds Left or
Right toward each step's x and fires the step's action when it gets there: a
plain jump, `JumpH`, `DownJump` or `DoubleJump`. Stuck means it moved less than
5 px in 2 s, checked every 500 ms. When stuck it drops the path, releases the
arrows, jumps if on the ground, and plans again.

**Return to the spot** (`ReturnToOriginThread`, lazy mode only):

- The tolerance is `return_tolerance_x/y` (48/31).
- It walks in chunks of at most 500 ms at 355 px/s.
- Spot above by less than 132 px: jump. 132–280 px: double jump. More: high
  jump. More than 484 px above is not tried this way.
- Spot below: drop down, unless on a warp gate.

**Map collision** comes from ZTeam's server, gzipped, and is decoded in
go_client.exe at `FUN_1400aa740`:

- A u32 width and a u32 height in 32-px cells.
- Then two layers of width × height records, 36 bytes each, column-major.
- GoC reads bytes 0–1 as a u16, **byte 2 as the attribute**, and bytes 3–34 as
  32 column heights. It ignores byte 35.
- Attribute meanings, from `SimulateMove`: 4 is a wall and wins over the other
  layer; 5 and 6 also block; 2 blocks only while rising; 1 is another platform
  type, inferred to be one you can jump up through.

GoBot's `include/gobot/map_file.hpp` reads the same 36-byte record shape from
the game's own .map files. But it treats byte 35 as the attribute and does not
use byte 2, so check it against GoC's reading.

### What go_client.exe does itself

- **Healing** (`FUN_1401174b0`) runs on every HP/MP update:
  - Only while the bot runs, the game has focus, and no captcha or chat is in
    progress. At most every 100 ms.
  - For HP it takes the enabled tiers whose threshold is above the current %,
    and presses only the one with the lowest threshold. MP works the same way,
    20 ms after HP.
  - There is no potion cooldown, so while you are low it presses roughly every
    100 ms.
- **Avatar** (`FUN_1400c0bf0`). On every avatar-time update, it presses the
  avatar key when the time left is at or under the threshold.
- **Answering a party invite** (`FUN_14012ac70`). It presses the accept key
  every 200 ms while an invite is pending.
- **Screenshots** (`FUN_1400ffe70`, `FUN_140100050`). A BitBlt from the
  **desktop** over the game window's rectangle, so anything covering the game
  ends up in the picture.
- **Template matching** (`FUN_140100960`). Grayscale `TM_CCOEFF_NORMED`. Dialog
  headers (`FUN_140100220`) are matched as edges: CLAHE (clip 2, 8×8 tiles),
  then Canny 100/200, against the bottom 30 rows of the template.
- **Finding a player by name** (`FUN_140120e10`):
  - It draws the name with GDI in the game's font (WOD.ttf, family "Ghost") or
    in Arial, depending on game version.
  - It matches that drawing in the map's area list, scrolling page by page.
  - The list panel must score at least 0.8 and the name above 0.6. Scores of
    0.5–0.6 are logged as a near miss.
- **Chat box check** (`FUN_140100cb0`). In the bottom-left 401 × 145 px it looks
  for a 4 × 18 px patch of the chat box border at ≥ 0.99, then compares a patch
  77 px to its left.
- **Mouse** (`FUN_140101730` and its callers):
  - The cursor moves along a curve through a random point near the midpoint, in
    8–28 steps (distance ÷ 12), 4–8 ms per step.
  - It lands with ±1 px of jitter. The button goes down after 20–49 ms and up
    40–69 ms later.
- **Other.** Auto login ends the game with `taskkill /F /PID`. The only
  registry read is Windows' light/dark setting.

### How GoC loads a map

GoC never reads the game's map files. ZTeam's server sends the map, and only
after it has seen the game load one.

1. **The server notices the map change** in the game packets GoC uploads and
   sends `0x12`: map name, map key, and your x, y on the new map. go_client.exe
   (`FUN_1400d0120`) then:
   - writes that x, y into your position;
   - empties the monster list, the target and the path, and resets the
     'Loại mob' filter;
   - sets a placeholder 1024 × 1024 map and asks for the real one with `0x60`
     (the map key);
   - loads the per-map files named by the key: `patrol/<key>` waypoint lists
     and `safeplace.ini` (`[Maps] <key>=x,y`).
2. **The server answers with `0x61`**: the map key and a gzipped collision grid.
   If the key still matches the current map, the client parses it on a
   background thread (`FUN_1400d1090` → `FUN_1400aa740`). It gunzips the data
   when it starts `1F 8B`, then reads the u32 width and height and two layers of
   36-byte records (format under [Movement](#movement)).
3. **On the same background thread it builds a fresh `PathFinder`** from the
   grid. `PathFinder::Init` builds the platform graph.
4. **The UI thread swaps it in** (`FUN_1400d1340`), again only if the key still
   matches. It hands the grid and pathfinder to the executor
   (`CommandExecutor::SetMapData`), logs 'Applied map', and bakes the map
   picture into a Direct3D texture for the Bản đồ view (`[MapRTT]`).
5. **Extras**, each checked against the current map key:
   - `0x41` sends a path graph (waypoints and connections), drawn on the map.
   - `0x6a` sends the warp-gate areas. The executor uses them for its "on a
     warp gate" checks (`IsStandingOnWarpGate`), and the map draws them.
   - `0x40` (ground segments) arrives, but no client handler is registered for
     it.

Two consequences:

- GoC only knows the map once the game loads one while GoC is capturing. That is
  why its UI tells you to walk into a shop and back out to load the map: 'Hãy di
  chuyển vào Cửa hàng và ra lại để load bản đồ'.
- The map-change message carries your spawn position, so every warp gives an
  absolute fix. GoBot reads the same grids from the game's own .map files
  (`include/gobot/map_file.hpp`). What it lacks is the map key and the spawn
  position, and those come from packets.

### How GoC knows about monsters

Entirely from packets; there is no screen reading. None of the OpenCV calls in
go_client.exe is in monster code, and the only monster pictures it uses are for
closing the Monster Dictionary.

- **Snapshot, `0x11`.** The game-state message carries every monster as a 29-byte
  entry: id, type id, HP, max HP, x and y (floats), a distance and a flag. The
  client (`FUN_1400cfcd0`) replaces its list with this one. It keeps the names
  it already has, because the snapshot carries none.
- **Updates through a queue.** GhostCore puts five kinds of message into a
  locked queue (`ProtocolHandler::SetMapUpdateQueue`): `0x14` and `0x15` (your
  move and jump), `0x16` (monster update), `0x17` (monster removed) and `0x21`
  (batched positions: id, x, y). go_client drains the queue every frame
  (`FUN_1400b8980`):
  - An update inserts or refreshes the monster by id and works out its distance
    to you. Updates in the first 2 s after a map change are ignored.
  - A remove deletes the monster, and clears the target if that was the one.
  - A position batch moves monsters by id.
- **The record** is 0x68 bytes:

  | Offset | Field |
  |---|---|
  | +0 | id |
  | +4 | type id |
  | +8, +0xC | x, y (floats) |
  | +0x10, +0x14 | HP, max HP |
  | +0x20–0x32 | about a dozen 16-bit stats |
  | +0x38 | name |
  | +0x58 | distance to you |
  | +0x5C | a flag |
  | +0x67 | the "ThậpNhịYêu" flag |

  Only the `0x16` update carries the name and the stats.
- **The list on screen** (`FUN_1400ce440`, "Quái vật xung quanh"). Only monsters
  with HP above 0, sorted, showing id, name, HP as current/max, distance, and ΔX
  and ΔY from you. The current target is highlighted.
- **'Loại mob' is one number.** The dropdown lists the type ids of the live
  monsters around you, plus 'Tất cả' (0). Picking one sends `0x33` with that
  type id, and a map change resets it to 0.
- **Target choice is the server's.** The target arrives as `0x1c`, or as command
  `0x20` in a batch. The client stores the id, highlights it, and gives it to
  the executor (`SetPathTargetMonster`). When no id is set, the executor falls
  back to the server's target id in the game state, then to the first monster
  with the +0x5C flag.

So everything GoC shows about monsters (positions, HP, names, types) is decoded
by ZTeam's server from the game stream. GoBot's passive `0x0038` decoder sees the
same monsters: protocol-findings §4c counted 43 ids, matching GoC's count.

### ZTeam protocol message map

These messages travel on GoC's own encrypted link; the first byte is the
message type. Each server message was matched to the client handler it reaches,
through its slot in the callback table go_client.exe registers
(`FUN_1400b03a0`). So the meanings below come from what the handler does.
"(inferred)" marks the few where that is not certain.

| Direction | Type | Content |
|---|---|---|
| to server | `0x02` | Login token |
| | `0x05` | Heartbeat (client time) |
| | `0x07` | Start/stop auto, and focus changes (both senders compile to one function) |
| | `0x10` | Packet batch: u16 count, then per game frame: direction u8, opcode u16, length u16, the frame |
| | `0x32` | Bot config |
| | `0x33` | Monster filter |
| | `0x34` / `0x35` | Patrol start / stop |
| | `0x60` | Map data request (map key) |
| from server | `0x03` | Login result: session token, expiry, session counts, opcode allow-list, game server IP and port range, game window title |
| | `0x06` / `0x08` | Heartbeat ack / start-auto ack (plus the server's focus state) |
| | `0x11` | Game state: you (session id, HP/max, MP/max, fury/max, x, y, moving, facing, avatar state, avatar time) and **every monster** as 29-byte entries (see [How GoC knows about monsters](#how-goc-knows-about-monsters)) |
| | `0x12` | Map changed: map name, map key, and your x, y on the new map (two floats). Starts the map download. |
| | `0x13` | A raw game packet relayed back (opcode u16, length u16, bytes). The client uses opcodes `0x0027` and `0x002A` for your own position. |
| | `0x14` / `0x15` | Player move / jump (into the map update queue) |
| | `0x16` / `0x17` | Monster update / remove (into the map update queue) |
| | `0x19` | HP/MP update (three i32) |
| | `0x1a` / `0x1b` | Spell confirmations: base skill / job skill (buffs) |
| | `0x1c` | Target chosen (monster id) |
| | `0x1d` | Attack confirmed |
| | `0x1e` | You died (session id); queues the auto revive |
| | `0x1f` | Level and EXP (u16 + i32) |
| | `0x20` | Command batch: u8 count, then per command type u8, length u8, data |
| | `0x21` | Monster positions, batched (12-byte entries: id, x, y) |
| | `0x30`, `0x40` | No client handler is registered; ignored. `0x40` carries map ground segments. |
| | `0x41` | Map path graph: 16-byte waypoints and 14-byte connections |
| | `0x48` | Quick quest state |
| | `0x50` / `0x51` / `0x52` | Subscription expired / expiry updated / used on another machine |
| | `0x61` | Map data: map key and the gzipped collision grid |
| | `0x62` | Player chat (feeds party-by-magic-word) |
| | `0x6a` | Warp-gate areas for a map (name + 32 bytes each) |
| | `0x70`–`0x75` | Party invite, leave, disband, update, member HP |
| | `0x76` | Sets flags in a feature (inferred: you were revived) |
| | `0x77` / `0x80` | Avatar on/off / avatar time left (i16; presses the avatar key at the threshold) |
| | `0x78` | Pet HP (i16, inferred) |
| | `0x81` | Pet died |
| | `0x7e` / `0x7f` | Remote chat / remote party command (queued as chat lines) |
| | `0xff` | Error text |

Command types inside `0x20`:

| Type | Command | Type | Command |
|---|---|---|---|
| `0x01` | move left | `0x10` | use skill |
| `0x02` | move right | `0x11` | use buff |
| `0x03` | stop | `0x20` | target monster |
| `0x04` | jump | `0x30` | double jump |
| `0x05` | high jump | `0x31` | release keys |
| `0x06` | jump down | `0x32` | double down |
| `0x07` / `0x08` | face left / right | `0x42` | path to (x, y) |
| `0x09` | down | `0x43` | stop pathing |

Moves are ignored while a path is being followed.

The monster record (`0x16`) carries the id, position, about a dozen 16-bit
stats, the name, and a flag. The flag is set when the name contains
"ThậpNhịYêu". GoC checks for that name in three encodings — UTF-8, GBK, and a
two-byte form — so names in the stream do not all use one encoding.

### Trust and security

- **No memory reading or writing, no hooks.** Neither binary imports the APIs
  for them.
- **No packet injection found.** The send functions belong to the bundled
  PcapPlusPlus library, and no GoC code was found calling them. This is from the
  static call graph; calls through the library's virtual functions could not be
  fully traced.
- **Account calls use HTTPS**, with normal certificate checks
  (`WINHTTP_FLAG_SECURE` is set whenever the port is 443). `credentials.dat` is
  encrypted with DPAPI.
- **The link to ZTeam's server can drive the keyboard**, through command
  batches and remote chat and party commands.
- **Updates are not signed.** WinSparkle has no signature key: no key resource
  and no key API imported. Updates rest on HTTPS alone, and they install with
  admin rights because go_client.exe always runs as admin.

---

## Reuse ledger

### Already taken

No work outstanding on any of these.

| From GoC | Where it landed |
|---|---|
| `bitmap_font` glyph OCR | `src/glyph_font.cpp` reads GoC's path directly |
| `data/images` template library | `GOBOT_UI_IMAGES` in `CMakeLists.txt`, staged beside the exe at build |
| Bot config schema and defaults | `configs/bot_default.ini`, verbatim |
| Party config schema | `configs/party.ini`; `gobot_test` reads GoC's `party_s0_default.ini` |
| One-input-token priority arbiter | `ExecutionCoordinator`, `command.hpp` (`Critical = 100`) |
| Multi-thread executor split | Four threads: exec, heal, buff, pathfind |
| Scancodes + jitter + focus gate | `src/input_manager.cpp` |
| Two-skin awareness | Documented in `configs/templates.ini` |

Alpha masking is already handled, and handled better than a straight port would
be: `template_matcher_cv.cpp` uses masked `matchTemplate` to find candidates and
then rescores each with a real masked correlation, because masked
`TM_CCOEFF_NORMED` returns false perfect scores on flat colour under a mostly
transparent template. Nothing to take here.

### Worth taking

Ranked by time saved per unit of work.

#### 1. The glyph set GoBot loads is missing four characters

`src/glyph_font.cpp:78` points at `data\bitmap_font`. That set has 179 glyphs
and a complete Vietnamese diacritic range, but it is **missing `F`, `J`, `W` and
lowercase `w`** — exactly the Latin letters that do not occur in native
Vietnamese orthography. Whoever built it worked from Vietnamese text only.

`data\images\bitmap_font` is the same set plus those four: 183 glyphs, a strict
superset.

This is already a known symptom. From `START-HERE.txt`:

> Names are read off the screen and so are matched loosely — the reader turns
> "ExampleWin" into "Exampley in" often enough that an exact match is a match
> that never happens.

`Example**W**in` → `Example**y i**n`. The `W` cannot be read because there is no
`W` to match it against.

The fix is to prefer `data\images\bitmap_font`, or load both and let the
superset win. It affects everything that reads text: HP/MP figures, chat,
the player list, and party-by-keyword — which is the feature where a loose match
is currently doing the work a correct glyph set should do.

Worth re-checking the loose matching afterwards. It was built to absorb these
misreads, and with the full set it may be absorbing less than it costs.

**Effort: one line. Then re-measure.**

#### 2. `sample_config.ini` — four attack-geometry presets

Not in GoBot anywhere. Four named shapes:

| Preset | minRange | maxRange | highestHitY | lowestHitY | thresholds |
|---|---|---|---|---|---|
| `Near_Wide` | 120 | 200 | 131 | 80 | 31 / 31 |
| `Near_Narrow` | 120 | 200 | 63 | 63 | 31 / 31 |
| `Far_Wide` | 150 | 350 | 150 | 63 | 63 / 63 |
| `Far_Narrow` | 150 | 350 | 63 | 63 | 31 / 31 |

These map to how a class's attacks actually reach, and they are somebody's
tuning work. GoBot's `bot_default.ini` carries one geometry; these belong beside
it as selectable presets.

**Effort: half an hour. Saves days of guessing at ranges.**

#### 3. 53 unmapped consumable icons

`configs/templates.ini` maps two: `hp_potion_8820131`, `mp_potion_8810111`.
There are 55 — 17 HP, 14 mana, 8 buff, 9 fragment, 7 craft — each named after
its game item id, so they need no naming work.

Mapping the rest makes auto-potion indifferent to which potion is in the bag,
and is a prerequisite for anything that buys or sorts.

#### 4. Shopping, with the state machine written out

26 shop category tabs in `tab_NpcShop_Category.spr`. And GoC's own log strings
in `go_client.exe` spell out the flow:

```
FindOpener → OpenInven → SelectCategory → WaitDialog → BuyItems
```

with retry counting, a `wrongShop` flag, and a **2.4 s** shop-dialog timeout.
That is the design settled for you.

#### 5. Skin 1 variants

`templates.ini` maps skin 0 only. Every button and dialog has an `inven/1`
counterpart. `templates.ini` already records why this matters:

> The first attempt at this used a set extracted from the other one and every
> match silently failed — the matcher was right and the pictures were of a
> different program.

#### 6. The quest library

~420 templates in 14 categories plus `categories.json`, and the dialog buttons
to drive them. A whole feature GoBot does not have. Large, self-contained, no
dependency on the protocol.

#### 7. `item_processor.ini` constants

Detection thresholds **0.7 for UI, 0.4 for item icons** — tuned values worth
comparing against GoBot's. Also the key codes (`73`=I inventory, `85`=U quest,
`77`=M map, `115`=F4 avatar) and the interval defaults.

#### 8. 70 pet and 22 seal icons

For the pet features — heal, revive, loot.

#### 9. Look inside opcode `0x0081` (from the decompile)

GoC's parser treats every `0x0081` frame as a wrapper around a whole game frame.
GoBot drops that content as a trailer. GoC also captures UDP, which GoBot never
has. Its client reads your own x, y as two floats from game opcodes `0x0027` and
`0x002A`, which appear in no GoBot capture. This is the cheapest lead there is on
your own position and the HUD values. Details are in
[protocol-findings.md §4d](protocol-findings.md#4d-what-gocs-own-parser-does-2026-10-17),
and plan items P0-1 to P0-3 cover the work.

#### 10. The movement constants

The constants are gravity, jump speeds, walking speed and the jump thresholds in
[Movement](#movement). They are what dead reckoning (plan P3-1) and every jump
decision need, and GoC tuned them against the real game.

#### 11. The fight rules as numbers

These are the keep-distance step, the 10% MP cut-off, the per-skill range check,
the jump-attack thresholds, the 4× buff press, the fury formula and the loot
guard ([Fight rules](#fight-rules-commandexecutor)). Plan items P1-7, P1-8,
P1-10, P2-2 and P2-7 now carry them.

#### 12. The map attribute byte

This is byte 2 of each 36-byte map record, with GoC's meanings for walls and
platforms. GoBot's map reader uses only the heights today (plan P3-16).

### Nothing to take

- **The protocol.** Server-side, and downloaded at login. Deriving it from your
  own traffic with `gobot_sniff` is the only route; that has not changed.
- **Target choice.** Server-side. How a chosen target is fought is client-side,
  and is in [Fight rules](#fight-rules-commandexecutor).
- **`HTTPClient`, licensing, HWID, payment, Telegram.** No server, and not wanted.
- **Captcha detection and solving.** Both out, by the player's decision: GoBot
  neither watches for a captcha nor answers one, and `data/images/captcha` goes
  unused.
- **`data/images/equip`.** Empty in GoC as well.
- **`CryptoHandler`, WinSparkle, `imgui.ini`.**

---

## Facts worth keeping

**The game opcodes in GoC's binaries.** `0x73D` captcha; `0x14`
channel-switch / character-selected; `0x38`/`0x42`/`0x51` login or map load done;
`0x81` a wrapper around another frame; `0x2E`/`0x31` dropped on UDP. They are
free starting points for `--watch` correlation; everything else has to be
earned.

**GoC's process names are the launcher, not the game.** `item_processor.ini`
says `al_launcher1_process=VTCZone.exe`, `al_launcher2_process=GhostOnlineVTC.exe`,
`al_game_window=GhostOnlineVTC`. GoBot measured the actual game as **`Game.exe`,
class `GhostSoul`**, title `GhostOnlineVTC`. GoC's names describe the launcher
chain it waits on. Do not take them as the capture target.

**If GoC ran on a machine, Npcap is installed.** `GhostCore.dll` imports
`wpcap.dll` and `packet.dll` directly.

**GoC waits for server confirmation before advancing an action.**
`OnAttackConfirmed`, `OnSpellBaseConfirmed`, `OnSpellJobConfirmed` exist because
a skill key that did not land needs re-pressing, and only the server knows. A
packet-driven GoBot can do the same once `skill_confirm` and `attack_confirm`
are mapped — both are already names in `protocol.ini`.

**Their pathfinder validates edges by simulation, not geometry.**
`SimulateMove`/`SimulateDoubleJump` per candidate edge, against a tile attribute
grid. Their own UI still labels it *Pathfinding (Unstable)*, which is worth
knowing before treating that part as a solved problem to copy. The constants it
simulates with are in [Movement](#movement).

---

## A note on scope

Reuse here means assets, schemas and tuning values read in place, plus designs
reimplemented from observed behaviour. Since 2026-10-17 that includes rules and
numbers read from the decompiled code. They are re-implemented; no GoC code is
copied. GoC's binaries are not redistributed,
their `/api/*` surface is not touched, and `credentials.dat` was not opened.
`.gitignore` already keeps the staged `templates/` out of the repository on the
same reasoning: using somebody's artwork is not the same as publishing it.
