# Overhaul: making the protocol primary

GoBot's problems are not spread across the codebase. They trace to one thing:
**the state layer is a third built, and the vision layer is standing in for the
missing two thirds.** Every symptom below is a consequence of that, and the
sequencing in this plan follows from fixing it rather than from fixing symptoms.

The infrastructure is sound and is not what needs overhauling. Both framings are
detected and `verified=true`, the reassembler handles out-of-order and overlap,
the registry is data-driven, `gobot_test` covers 103 cases, and the GUI exposes
all of it. What sits on top of that is one packet layout.

---

## Diagnosis

Measured from `configs/protocol.ini`, `configs/app.ini`, and the two runtime
logs in `build/bin/Release/` (80,357 and ~8,000 lines, 2026-10-05/06).

### The state layer

`protocol.ini` contains exactly one packet layout:

```ini
[Packet.0x0038]
name=EntityUpdate
semantic=monster_update
fields=id:u32@8,speed:f32@20,x:u16@80,y:u16@82
```

The README names three semantics as the minimum viable set — `self_state`,
`self_hpmp`, `monster_update`. **One of three is mapped, and it is broken:**

```
Monster packet 0x38 has no usable id field          × 109   (gobot.log)
```

`id:u32@8` is not an identity. Without a stable id the world model cannot
correlate one update with the next, so it cannot age an entity out, cannot
notice a death, and treats every update as a new creature. `field_finder.cpp:192`
already records this as the thing that kept going wrong.

There is no `monster_remove`, no `self_hpmp` and no `self_state`.

(There is no `map_data` either, but that one does not matter — the game ships
781 `.map` collision files and `MapFile` already reads them. See Phase 4.)

### What the vision layer is covering for

| Missing packet | What stands in | Cost |
|---|---|---|
| own position | `WorldAnchor` — accumulates stationary monsters as landmarks, solves for the camera | Needs 6 sightings per landmark, 4+ agreeing, 10 to settle the scale, re-solves every 12 frames, 2.5 s hold. Cannot converge on a sparse or busy map, or while moving. |
| own position (2nd route) | reading the character's own name off the screen | `Cannot find "ThanhAmCoc" on screen` × 4 — and those are all common letters, so this is not only the glyph gap |
| `self_hpmp` | counting coloured pixels on the HP/MP bars | `PrintWindow returns a blank frame for this window` × 63 → pixels are lifted off the *screen*, so the game must be foreground and unobscured |
| map identity | minimap image fingerprinting | ~20 distinct `Map changed -- minimap fingerprint …` lines in one session, several appearing once. Map identity thrashes, so anything keyed to a map thrashes with it. |

The vitals HUD anchor comes from
`C:\Program Files\TH_VGO_Bot_Helper\TungHaVGOBot.dll` (× 4), and `START-HERE.txt`
already records that that source's party detector is wrong for this build of the
game. Core state should not depend on another bot's assets.

### Operational faults, independent of the above

| Evidence | Fault |
|---|---|
| `Synthetic keystrokes are being filtered -- nothing GoBot presses will reach the game` | Ran un-elevated. `elevated: no` × 25 vs `elevated: yes` × 20 — **over half of all launches could not press a key.** Detected and warned about, but not prevented. |
| `Game window not found (title~'' class='#32770' process='GhostOnlineVTC.exe')` | `#32770` is the Windows **dialog** class and `GhostOnlineVTC.exe` is the **launcher**. Something is searching with `[Crash]`'s launcher identity instead of `[Window]`'s `Game.exe` / `GhostSoul`. 393 window-not-found lines against 1,414 successes. |
| `HunterConfig: cannot read …/build/bin/Release/configs/hunt.ini` × 4 | `hunt.ini` exists in `configs/` but is not staged to the build output. Hunting silently ran on defaults. |
| `Routines: 3 from …/routines.ini` × 2,837 | A config file re-read thousands of times per session. |
| 71,029 of 80,357 lines are `red x=… y=… trough=…` | 88% of the log is bar-scan pixel spam at INFO. Any real signal is unfindable, which is its own reliability problem. |
| `Capture started` × 4 in a whole session | Packet capture almost never ran. The packet path is not just unfinished, it is unexercised. |

### Root cause, once

Monster positions arrive exact, in world units, from the wire. Own position is a
statistical inference from screen pixels. Those two are then compared to decide
range and facing.

**A bot cannot aim when one operand is exact and the other is a guess.** That is
the whole of symptom one. Symptom two is the broken id field. Symptom three is
`PrintWindow` returning blank. Symptom four is mostly the three above plus
running un-elevated.

---

## The decisive experiment — do this first

`protocol.ini:198-206` concludes:

> Your own position is not in the client's half either — that stream is
> obfuscated, and the last eight bytes of every frame change even while the
> character stands still.

**That conclusion does not follow from that evidence, and it is worth an hour to
re-test.** If the payload were encrypted, *every* byte would change between
frames, not the last eight. "Only the last eight bytes change while standing
still" is the signature of a **trailer** — a sequence number plus a checksum or
MAC, or a timestamp. Everything before it being stable means everything before
it is plaintext.

Three things support re-testing:

1. **The server-bound framing was already solved** — `[FramingClient]`, 100%
   coverage, 77 frames, 6 opcodes, zero resyncs. A spec does not tile an
   encrypted stream to 100%.
2. **`[Framing]` already has `checksum_offset=6`.** This protocol checksums its
   frames. An 8-byte trailer on the client half is entirely in character.
3. **GoC proves it is readable.** ZTeam's server sent `UpdatePositionInfo`,
   `PlayerMove` and `PlayerJump` back to its client — so it derived the
   character's own position from captured traffic, and its `PacketCapture`
   recorded both directions (`ProcessPacketStream(…, PacketDirection, …)`). They
   did not break cryptography; they read a field and ignored a trailer.

GoBot has also already cracked a weaker obfuscation on this game:
`map_table.hpp` — "every byte is exclusive-ORed with 0x11".

### Settled — it is not encrypted

**Answered, from data already on disk.** See
[protocol-findings.md](protocol-findings.md) for the measurements.

`%TEMP%\goc_stream.bin.c2s` is a raw client→server capture of a GoC session, and
the three `.bin` files in `build/bin/Release/` are `FieldFinder` dumps holding
both directions. Pooled, they give 488 samples of `0x00C5` and 22 of `0x00C6`.

In `0x00C6`, **31 of 41 payload bytes never change**. In `0x00C5`, offsets 2–15
and 19–23 are byte-for-byte identical across all 487 samples. Only a 2-byte
sequence at the head and an 8-byte trailer vary. No cipher leaves thirty bytes
identical across 487 frames, and both framings tile at 100% with zero resyncs.

So `protocol.ini:201` is wrong, and the alternatives below are not needed.

### What is still open

Position has **not** yet been located in the client half. Two reasons the
existing captures cannot settle it, both worth fixing first:

1. **`FieldFinder::position_fields()` only scans f32** (`field_finder.cpp:527`,
   `is_float = true`, range `1.0..limit`). This game's coordinates are **u16** —
   `0x0038` proves it. The search that concluded "nothing correlates" could not
   have found a u16 coordinate. Fix it to scan u16/i16/u32/i32 too.
2. **The FFND ground truth is not a position** — `truth_x -2024..2006`,
   `truth_y 0..6`, signed and zero-centred. It is a displacement measurement, so
   correlating it against an absolute coordinate yields only artifacts.

The best structural candidate is **offsets 13–14**, which vary widely in nearly
every member of the `0x00Bx`/`0x00Cx` command family. Confirming it needs one
deliberate capture:

```powershell
# --record takes a BASENAME; it writes <name>.s2c and <name>.c2s itself.
gobot_sniff --record still --host <server-ip>    # stand still, 30s
gobot_sniff --record walk  --host <server-ip>    # walk ONE direction, far, 30s
```

Then compare offsets 13–14 between the two. Values should land in the `0x0038`
coordinate box — 676..6112 by 395..1120 — since the character shares the
monsters' coordinate space.

**`--diff` is live-only and takes no file argument**, so a recording cannot be
re-analysed without replaying it. Add `--diff <opcode> --file <capture>` plus a
two-capture differential mode *before* the next capture session; it is what
makes this loop fast.

### WorldAnchor does not work, and the framing fix did not rescue it — measured

It was reasonable to hope the `0x0081` fix would fix this too: `WorldAnchor`
needs six sightings per landmark and ten to settle its scale, and it was being
fed 27% of the entity updates. It does not.

`gobot_cli --anchor 40`, with the stream now parsing at zero resyncs:

```
wire  screen  camera is at  evidence        minimap     your character
42    2-4     -             0/0 vs 0.0 b10  91.0,73.6   -
```

- **wire 42** — the packet side is exactly right, matching GoC's `Quái: 43`.
- **screen 2-4** — the nameplate detector finds only a handful, because only a
  handful of the 42 are on screen at once. The map is 1358 x 4152; the viewport
  is about 1030 x 800.
- **0/0 explained, b10 collected** — ten landmarks accumulated and not one
  transform ever explained the four it needs.
- **no fix, for forty seconds.**

The shortage is structural, not a tuning problem: matching ten landmarks against
forty-two world positions from two to four visible plates is under-determined,
and no amount of extra packet data changes how many creatures are on screen.

**So the vision route to own position is closed**, as is the memory route below.
Dead reckoning from the bot's own issued commands is what is left.
`world_anchor.cpp` and `camera_tracker.cpp` should be deleted rather than
maintained once that lands.

### Memory reading is not available — measured

Worth stating before anyone proposes it, because it is the obvious answer and it
does not work here.

The game ships **XIGNCODE3** (`XIGNCODE_VN\` in the install root), a kernel-mode
anti-cheat. Probed from an elevated process against `Game.exe`:

| call | result |
|---|---|
| `OpenProcess(QUERY_INFORMATION｜VM_READ)` | **succeeds** — returns a handle |
| `VirtualQueryEx` on that handle | **succeeds** — 2,385 readable regions, 1,098 MB |
| `ReadProcessMemory` on those regions | **fails, 0 of 60, every one `err=5`** |

Access denied on reads while queries on the same handle work is the signature of
a driver stripping `PROCESS_VM_READ` after handle creation (`ObRegisterCallbacks`).
Un-elevated, `OpenProcess` fails outright with the same error.

So own position and HP cannot be read out of the client. Defeating the
anti-cheat is out of scope and not worth the account.

**This vindicates the architecture rather than threatening it.** Passive packet
capture never touches the game process, so there is nothing for XIGNCODE to
deny — which is precisely why the commercial client was built this way:
out-of-process sniffing plus `SendInput`, because in-process access was closed
to them too.

### If position turns out not to be in the client half

Still the question, now that memory reading is ruled out. In descending
preference:

1. **Derive position from what you send, not what you observe.** The bot issues
   the movement keys itself. Dead-reckoning from its own commands, corrected
   whenever `WorldAnchor` does get a fix, is far better than a fix alone — the
   anchor becomes an occasional correction rather than the primary source.
2. **Memory read, position only.** One `ReadProcessMemory` for the character
   struct. Narrow, exact, and breaks on client patches — but it is one offset,
   not an architecture.
3. **Keep `WorldAnchor`, but stop steering on it directly.** Gate every
   positional behaviour on `fresh()` and `agreed >= kMinExplained`, and make the
   bot refuse to act positionally rather than act on a stale fix. Today a bad
   fix produces confident wrong movement.

---

## Target architecture

The pipeline in the README is right. What changes is which layer is load-bearing.

```
                      ┌─────────────────────────┐
   Npcap ──► TcpReassembler ──► Framing ──► OpcodeRegistry ──► ProtocolHandler
                      └─────────────────────────┘
                                                                     │
                                          GameState  ◄───────────────┘
                                      (self, monsters, map — EXACT)
                                                 │
                                                 ▼
                                             BotBrain
                                                 │
                                   ExecutionCoordinator (one input token)
                                                 │
                                             InputManager
                                                 │
                           Vision ──────────────►│  UI only: dialogs, inventory,
                       (templates, glyphs)       │  shops, quests, login screens
                                                 ▼
                                           game window
```

Two rules that are the point of the whole exercise:

**Vision never supplies world state.** No position, no HP, no monster list, no
map identity from pixels. Vision answers "is the shop dialog open", "where is
the OK button", "which inventory tab is selected" — questions about the
interface, which is what template matching is good at and what GoC used it for.

**The brain refuses to act on state it does not have.** Today a missing fix
yields a confident wrong decision. Each behaviour declares the semantics it
needs, and the Dashboard says which are unmapped and therefore which behaviours
are off. The existing "Dashboard refuses to start and names the blockers"
pattern is right; extend it per-behaviour.

---

## Plan

### Phase 1 — Own position

The experiment above, then map `self_move` / `self_state`. Everything else
depends on this; nothing else is worth doing first.

Done when `gobot_cli --status` reports a position that tracks where you actually
are, with the GoBot window sitting on top of the game.

### Phase 2 — Fix `0x0038`, then finish it

The id field is wrong, and it is the highest-value single fix after position.

`x:u16@80` means this packet is at least 84 bytes and only four fields are
mapped. Re-run `--diff` across a long capture with many creatures and look
specifically for:

| Field | Why it matters |
|---|---|
| **id** (stable per creature, differs between) | entity tracking — fixes corpse-hitting |
| **hp / max_hp** | stop attacking something already dead; prefer wounded targets |
| **kind / species** | `ignore_fly_monster`, boss priority, per-mob rules |
| **alive / state flag** | the cheap version of `monster_remove` |

`field_finder.cpp:192` notes the identity heuristic picked the most-repeated
value and that this was wrong. Identity is better found by *grouping*, the same
way the coordinates were settled: split updates by candidate field and keep the
one where each group holds one creature's smoothly-moving trajectory. A wrong id
scatters trajectories; the right one makes them continuous.

Then `monster_remove`, so deaths are events rather than timeouts.

### Phase 3 — `self_hpmp`, and delete the bar scanner

Once HP/MP come off the wire, `vitals_reader.cpp` (42 KB), `bar_spec.hpp`, the
`find_bars` calibration flow and its 71,029 log lines all go. So does the
dependency on `TungHaVGOBot.dll` for the HUD anchor, and so does the
foreground-and-unobscured requirement for healing.

This is what makes backgrounding and multiboxing possible, which no amount of
vision work can deliver.

### Phase 4 — Map identity from the wire

**Correction: map *geometry* was never blocked, and no `map_data` packet is
needed.** The game ships its own collision data and GoBot already reads it.

- `C:\VTCZone\Games\Ghost_Online_PC_VTC\data\Map` holds **781 `.map` files**,
  located from the registry (`Ghost Online VTC` → `DisplayIcon`).
- The format is confirmed on **781 of 781 files, zero mismatches**:
  `[int32 width_cells][int32 height_cells]` then `width*height*72` bytes,
  column-major, each cell two 36-byte layers of per-pixel-column surface
  heights.
- `MapFile::to_map_data()` already rasterises that into what `PathFinder`
  consumes.
- Map dimensions are in **pixels** (1600×1216, 3840×1024, 2560×7680), the same
  space as the coordinates on the wire — monsters at x 676..6112, y 395..1120,
  the character at (1497, 1147). World units *are* map pixels, so positions
  index straight into the grid with no scaling.

That is exactly what ZTeam's server was doing: parsing these same files and
selling the result back over the network. `SendRequestMapData` exists because
their *client* had no local reader, not because the data is remote.

So `enable_tile_pathfinding` is blocked only on knowing **which** of the 781
files you are standing in — and on Phase 1's position, to place yourself in it.

Map identity is currently answered two fragile ways: minimap image
fingerprinting (~20 distinct fingerprints in one session, several appearing
once) and reading the place name off the title bar with the glyph set, which is
missing `F`, `J`, `W` and `w`. `MapTable` then resolves that name through the
game's own `zVietnam\Chart\tb_Map.tbl` (XOR 0x11, strings stored reversed) to a
theme/stage pair, and `t<theme>_s<stage>.map` is the file.

**A map-change packet replaces the fragile half.** With the `0x0081` trailer
fixed the server stream now parses cleanly and ten opcodes are visible; the
rarer ones (`0x004A` 136 B, `0x003F` 44 B, `0x0265` 120 B) are the candidates.
Identify one and `minimap_tracker.cpp` plus the title-bar OCR both stop being
load-bearing, while `map_table.cpp` and `map_file.cpp` keep theirs — they are
the parts that work.

### Phase 5 — Operational hardening

Most of this list turned out to be already handled. Checking first saved
"fixing" three things that were not broken, and the same caution is worth
applying to the rest of this document.

**Done:**

- **Glyph set switched** to `data/images/bitmap_font`. Confirmed loading 183
  glyphs where it previously loaded 179 and was missing `F`, `J`, `W` and
  lowercase `w` — the letters absent from Vietnamese orthography, which is why
  the set was built without them. That is the cause of `ExampleWin` reading as
  `Exampley in`. The short set remains as a fallback.
- **Pixel spam demoted to DEBUG.** Per-candidate band logging was 71,029 of
  80,357 lines in one session — 88% of the log, which is why the warnings that
  mattered went unread. The verdict line stays at INFO; `--verbose` brings the
  detail back.
- **Config staging now reports what it kept.** It deliberately never overwrites
  an existing config — a rebuild once reset `channel_index` and the bot logged
  into the wrong channel — but it did so silently, so editing a shipped default
  and rebuilding appeared to do nothing. It now names the files it left alone.

**Already correct, do not "fix":**

- **Elevation.** `gobot_cli`, `gobot_gui` and `gobot_sniff` all carry
  `requireAdministrator` in their manifests, and the GUI additionally detects
  filtered input and offers a Restart as Administrator button. The
  `elevated: no` lines in the logs predate the manifests.
- **The window identity.** `client_class=#32770` / `client_process=GhostOnlineVTC.exe`
  in `login.ini` correctly describe the launcher's untitled dialog. The
  `Game window not found (class='#32770' …)` lines are the auto-login flow
  probing for a window that has not appeared yet, which is what it should do.
- **`hunt.ini`** is staged and identical to the shipped default. The earlier
  "cannot read hunt.ini" predates its being added.

**Still outstanding:**

- **Load configs once.** `routines.ini` was read 2,837 times in a session.

### Phase 6 — Vision, demoted and rebuilt

Only now, and only for the interface. With state exact, the vision layer is
allowed to be slow, allowed to need the window in front for the few seconds a
dialog is open, and allowed to fail without the bot walking into a wall.

This is where GoC's asset library pays off: 55 consumable icons, 26 shop tabs,
the ~420-template quest set, skin 1 variants, and the shopping state machine its
log strings spell out. All of it is interface work, which is the right job for
template matching.

---

## What gets deleted

An overhaul that only adds code will not fix this. Each of these exists to
substitute for a packet, and each should go when its packet lands:

| Goes | When | Size |
|---|---|---|
| `vitals_reader.cpp`, `bar_spec.hpp` | Phase 3 | 42 KB + |
| `world_anchor.cpp`, `camera_tracker.cpp` | Phase 1, if the c2s read works | 14 + 9 KB |
| name-reading player locator | Phase 1 | — |
| `minimap_tracker.cpp` load-bearing role | Phase 4 | 10 KB |
| `TungHaVGOBot.dll` dependency for the HUD anchor | Phase 3 | — |
| `GoBotBridge` party detector (already known wrong) | Phase 2 | — |

`mob_detector.cpp` (27 KB) is the interesting case. Once monsters come off the
wire with ids and hp, on-screen nameplate detection is no longer needed for
targeting — but it is still the only thing that can tell a player from a monster
visually, and it is what `WorldAnchor` feeds on. Keep it until Phase 1 settles,
then decide.

---

## What not to touch

- **The capture, reassembly and framing stack.** It is measured, tested, and
  correct. Both framings tile at 100%.
- **`ExecutionCoordinator`.** The one-input-token design is right and is the
  reason the bot does not walk left and right in the same tick.
- **The four-thread executor split.** Healing and buffing genuinely run on
  different clocks.
- **Scancodes, jitter, focus gating.** Correct, and the focus gate stays correct
  even after the vision layer stops needing the foreground.
- **`gobot_sniff`.** It is the tool the whole plan runs on. If anything, it earns
  more investment: a `--diff` that takes two captures and reports what differs
  *between* them would have answered the client-half question months ago.
- **`gobot_test`.** 103 checks over the layers that cannot be tested against a
  live game. Extend rather than replace.

---

## Risks

~~**The client half really is encrypted.**~~ Settled: it is not. Thirty bytes of
`0x00C5` are identical across 487 samples.

**Position may still not be in the client half.** The remaining open question.
If offsets 13–14 prove to be a timer and nothing else in the `0x00Bx`/`0x00Cx`
family carries coordinates, Phase 1 falls back to dead-reckoning from issued
commands plus `WorldAnchor` as a correction — better than today regardless,
because today a stale fix is steered on as though it were fresh.

**Analysis that reads its own tooling's bugs as findings.** Two already
happened: `position_fields()` scanning only f32 produced "nothing correlates"
for a u16 coordinate, and a 2-byte length field with `max_frame=32768` produced
"99.98% coverage" on files that are not streams at all. Judge a framing by
resyncs and consistent per-opcode payload sizes, never by coverage.

**Field layouts drift on a client patch.** Mitigated by what is already in place:
everything above `ProtocolHandler` is written against `Semantic`, never opcode
numbers, so re-mapping is an INI edit. Keep it that way.

**Deleting the vision state path before the packet path is proven.** Do not.
Each phase lands its packet, verifies against `--status` with the game obscured,
and only then deletes. The fallback has to stay until the replacement is
measured.

**Captures carry live account data.** `.gitignore` already excludes `*.bin`,
`*.pcap` and the working frames. The `.s2c` / `.c2s` extensions this plan leans
on are **not** currently covered — add them.
