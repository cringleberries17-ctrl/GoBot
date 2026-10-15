# The GoC client: what is in it, and what GoBot takes from it

Reference notes on ZTeam's GoC client at `C:\Program Files\ZTeam\GoC`, the
commercial bot GoBot replaces. Written from the shipped binaries and data files
— 543 demangled exports, the INI schemas, and 1,334 PNGs.

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
- Only two game opcodes are hardcoded anywhere: `0x73D` (captcha) and `0x14`
  (channel switch / character selected).
- The handshake verifies a server signature —
  `[Handshake] CRITICAL: Server Signature Verification FAILED! Possible Fake Server/MITM!`
  — so there is no standing up a fake server to watch the protocol either.

So the split is:

| In the folder, reusable | On their server, not reusable |
|---|---|
| 1,334 template PNGs, three glyph sets | Game packet → game state decoding |
| Config schemas and tuned defaults | The opcode table and packet layouts |
| Consumable/pet/seal icons by item id | Monster selection and tactics |
| Feature state machines (in log strings) | Patrol route logic |
| Scancode input, jitter, focus gating | |

**Assets, schemas and tuning values transfer. Protocol and tactics do not.**
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
`STUCK_DISTANCE_THRESHOLD`.

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
`DeriveSessionKey`, 12-byte nonce, `Encrypt`/`Decrypt`.

**Free functions** — `GenerateHWID() → array<u8,32>`, `GetStateName(BotState)`,
`IPv4ToUint32`.

The rest are plain result structs (`LoginResult`, `Subscription`, `Plan`,
`PlatformEdge`, …), ctor/dtor/assign only.

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

### Nothing to take

- **The protocol.** Server-side, and downloaded at login. Deriving it from your
  own traffic with `gobot_sniff` is the only route; that has not changed.
- **Targeting and tactics.** Server-side.
- **`HTTPClient`, licensing, HWID, payment, Telegram.** No server, and not wanted.
- **Captcha detection and solving.** Both out, by the player's decision: GoBot
  neither watches for a captcha nor answers one, and `data/images/captcha` goes
  unused.
- **`data/images/equip`.** Empty in GoC as well.
- **`CryptoHandler`, WinSparkle, `imgui.ini`.**

---

## Facts worth keeping

**The only two game opcodes in GoC's binaries.** `0x73D` captcha, `0x14`
channel-switch / character-selected. Free starting points for `--watch`
correlation; everything else has to be earned.

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
knowing before treating that part as a solved problem to copy.

---

## A note on scope

Reuse here means assets, schemas and tuning values read in place, plus designs
reimplemented from observed behaviour. GoC's binaries are not redistributed,
their `/api/*` surface is not touched, and `credentials.dat` was not opened.
`.gitignore` already keeps the staged `templates/` out of the repository on the
same reasoning: using somebody's artwork is not the same as publishing it.
