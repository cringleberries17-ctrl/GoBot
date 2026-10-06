# Client→server protocol: measured findings

Measured from data already on this machine, no new capture:

| Source | What it is |
|---|---|
| `%TEMP%\goc_stream.bin.c2s` | 1,270 B raw client→server stream, captured during a **GoC** session |
| `%TEMP%\goc_stream.bin.s2c` | 18,600 B raw server→client, same session |
| `build/bin/Release/walkonly.bin` | FieldFinder dump (`FFND` v2), 2,009 samples |
| `build/bin/Release/mixed.bin` | FieldFinder dump, 1,984 samples |
| `build/bin/Release/calibration.bin` | FieldFinder dump, 1,984 samples |
| `build/bin/Release/last_capture.s2c` | 21,516 B raw server→client, 246 frames |

The `.bin` files are **not** raw streams — they are `FieldFinder::save()` output
(`src/field_finder.cpp:565`): per sample, `u32 opcode`, `u32 from_server`,
`u32 len`, payload, `4×double truth`, `4×u32 valid`, `u64 at_ms`. They contain
both directions, which is why they are the most useful thing here.

---

## 1. The client half is not encrypted

**`protocol.ini:201` is wrong and should be corrected.** It says:

> Your own position is not in the client's half either — that stream is
> obfuscated, and the last eight bytes of every frame change even while the
> character stands still.

The last eight bytes do change. Everything before them does not.

`0x00C6`, 22 frames from the raw GoC capture, 41-byte payload:

```
offset   0  1  2 ................................ 32 | 33 ........... 40
distinct 22  2  1  1  1  1  1  1  1 ... 1  1  1   1 |  5 21 20 20 22 22 20 22
         ^^^^^                                       ^^^^^^^^^^^^^^^^^^^^^^^^
         sequence        31 of 41 bytes constant     8-byte trailer
```

`0x00C5`, 487 pooled samples, 40-byte payload: offsets 2–15 and 19–23 are
**byte-for-byte identical across all 487 samples**, with the trailer at 32–39.

A stream cipher or block cipher cannot leave thirty bytes identical across 487
frames. An 8-byte trailer that changes every frame is a sequence number plus a
checksum or timestamp. Both framings also tile at 100% with **zero** resyncs,
which does not happen on ciphertext.

Corroboration from GoC: its server returns `UpdatePositionInfo`, `PlayerMove`
and `PlayerJump`, so it derives the character's own position from exactly these
bytes. It reads a field and ignores a trailer.

**Consequence:** the client half is a legitimate source for own position, and
`WorldAnchor`'s landmark inference is not the only route.

## 2. The client→server command family

Discounting framing false positives (see §4), the real inventory:

| opcode | payload | samples | body bytes that vary | trailer |
|---|---|---|---|---|
| `0x00C5` | 40 | 488 | 0,1, 16,17,18, 24–30 | 32–39 |
| `0x01A5` | 40 | 84 | 0,1 only | none |
| `0x00C4` | 39 | 34 | 0,1, 17, 23,24,25, 27,28,29 | 31–38 |
| `0x00BF` | 34 | 32 | 0,1, **13,14**, 20 | 30–33 |
| `0x00C3` | 38 | 26 | 0,1, **13,14**, 22 | 34–37 |
| `0x01A6` | 41 | 18 | 0,1 only | none |
| `0x00B5` | 28 | 9 | 0,1, 4–19 | 20–27 |
| `0x00C6` | 41 | 8 | 0,1,4, **13,14**, 25–30 | 33, 37–40 |
| `0x00BD` | 32 | 7 | 0,1, **13,14** | 28–31 |
| `0x00B6` | 33 | 6 | 0,1, **13,14**, 17, 21,22,23 | 29–32 |
| `0x00E8` | 55 | 4 | 0,1, **13,14**, 21,22, 25,26, 29… | 49–54 |
| `0x0000` | 4 | 39 | — | 0, 2 |

Two things to take from this:

**A tight `0x00Bx`–`0x00Cx` family**, payloads 28–41 bytes, differing by a byte
or two. That is the shape of a command set — move, jump, stop, attack, use —
which matches GoC having three separate position decoders (`PlayerMove`,
`PlayerJump`, `UpdatePositionInfo`) rather than one.

**Offsets 13 and 14 both vary widely in nearly every member of the family**
(`0x00BF`, `0x00C3`, `0x00C6`, `0x00BD`, `0x00B6`, `0x00B5`, `0x00E8`, `0x00A6`).
A two-byte quantity at offset 13, common to the whole command family, is the
strongest position candidate found so far. It is **not** confirmed — see §3.

`0x00C5` at 488 samples (~1/second) does not share the 13/14 pattern and is
more likely a heartbeat than a movement packet. Do not start there.

## 3. Why the existing data cannot confirm it

Two reasons, both worth knowing before trusting any analysis of these dumps.

**The FieldFinder ground truth is not a position.** Across every opcode the
recorded truth reads `truth_x -2024..2006`, `truth_y 0..6` — signed and centred
on zero, with y essentially unused. It is a camera or displacement measurement,
not an absolute coordinate. Correlating it against an absolute world coordinate
is meaningless, and any correlation it produces is an artifact.

Observed artifacts, for the record: `r ≈ 0.52–0.63` appeared at offsets 14–18
and 22–25 *at every width and type simultaneously* (`@15 i32`, `@16 u32`,
`@17 u16`, `@17 f32`…). One varying region read at every alignment produces
exactly that, and the implied fields had impossible ranges
(`-1877391818`, `f32 = -6.07e30`). None of it is a lead.

**`FieldFinder::position_fields()` can only find floats.** `src/field_finder.cpp:527`
sets `r.is_float = true; r.width = 4` and filters to the range `1.0..limit`.
This game's coordinates are **u16** — `0x0038` carries `x` and `y` as u16, in a
box of roughly 676..6112 by 395..1120. The search that concluded "nothing
correlates" was structurally unable to find a u16 coordinate.

**Fix `position_fields()` to scan u16/i16/u32/i32 as well as f32.** Until then
it will keep returning nothing, however good the capture.

**The captures are also too static.** Bytes 2–15 being identical across 487
samples of `0x00C5` means that in those sessions, whatever those bytes hold did
not change. A position field cannot be constant while the character walks, so
either position is not in `0x00C5`, or the character barely moved in world
terms during the recording.

## 4. Framing false positives — do not be fooled by coverage

Tiling the `.bin` files as raw streams gives "99.98% coverage" under
`[FramingClient]` — and it is meaningless. 91 frames covering 281 KB, with
individual "frames" of 16,716 / 22,670 / 29,969 bytes and opcodes like
`0x58C1`, `0xCCCD`, `0xF993`.

A 2-byte length field with `max_frame=32768` consumes an average of ~16 KB per
bogus header, so a handful of frames swallow any file. **Coverage alone is not
evidence.** The real signals are the ones `last_capture.s2c` shows: 246 frames,
**zero resyncs**, two opcodes, and consistent payload sizes per opcode.

The same artifact explains the ~30 single-sample opcodes with 192-byte payloads
in the inventory above (`0x8ED4`, `0xC4F5`, `0x6F41`…). Discount any opcode seen
once with a 192-byte payload.

## 4b. Live session results — 2026-10-06

Four fresh captures taken with the game and GoC both running, on the gameplay
socket only (`103.82.30.42:15101`; the game's second socket, `103.82.30.35:15112`,
sent nothing in 20 s and is the idle auth connection).

| capture | what | c2s | s2c |
|---|---|---|---|
| `probe` | 20 s, stationary | 4,863 B, 47 frames, **0 resyncs** | 15,950 B |
| `walk` | 60 s, walking far | 12,341 B, 186 frames | 47,369 B |
| `track` | 40 s, moving + 7 screenshots | 8,085 B, 157 frames, **0 resyncs** | 61,289 B |
| `stand` | 25 s | 5,812 B | 38,413 B |

**Ground truth is available on demand.** GoC's window displays
`Nhân vật: (891.3, 1096.0) | Quái: 32`, and that line can be screenshotted and
read directly. Map was `Long Lân Cóc[7]`. Eleven readings this session put the
character in **x ∈ 730..1509, y ∈ 910..1128**, which sits inside `0x0038`'s
monster coordinate box — so the character shares the monsters' coordinate space,
and the displayed figures are already in world units.

### The client does not appear to send its own position

Searched all four c2s captures — ~400 packets, every offset, seven encodings
(u16/i16/u32/i32/f32, both endians), fourteen scales — for:

1. exact matches against each of the eleven known readings;
2. any field keeping ≥90% of its values inside the observed box with a span
   matching the ~780-unit distance actually travelled.

**Nothing qualifies.** The candidates that pass the box test all have spans of
8–150 units against 780 units of real travel; they are fields that happen to sit
in range, not coordinates.

This is a meaningful negative, because the c2s stream parses **perfectly** —
0 resyncs, 100% coverage, on two separate captures. Nothing is being missed.

Two hypotheses refuted along the way, both worth recording so they are not
re-tried:

- **`0x00C6` payload@8, u16be ÷8 = 1096.0.** Matched 22/29 walk and 14/14 still
  packets of that opcode — and then failed against seven fresh readings. 8768 is
  a constant that happened to equal one y value.
- **Offsets 13–14 across the `0x00Bx`/`0x00Cx` family.** They vary while the
  character stands still, so they are a timer, not a position. The still-vs-walk
  differential killed this immediately.

### What that implies

The client almost certainly sends movement **inputs** — direction, key state,
start/stop — and the server is authoritative for position. The evidence fits:
walking produces seven opcodes that are absent when stationary (`0x00EA` ×25,
`0x00C4`, `0x00E8`, `0x00C3`, `0x00BF`, `0x00BD`, `0x00BE`), while `0x00C5`
continues at a flat ~1/s rate throughout and is a heartbeat.

So **GoC's `Nhân vật:` readout is computed by ZTeam's server**, not read off a
field — it integrates movement from an anchor. That is consistent with GoC's
architecture: the server thinks, and returns `UpdatePositionInfo`.

**Consequence for the overhaul:** "read own position out of the client half" is
probably not available. The realistic design is an absolute anchor from the
**server** stream (spawn / map-load / warp) plus dead reckoning from the
movement commands the bot itself issues, re-anchoring whenever a fresh absolute
arrives. That was listed as a fallback in [overhaul.md](overhaul.md); it should
now be the primary plan.

### FIXED: opcode 0x0081 carries 12 bytes past its declared length

The server stream used to resync constantly — 512 to 829 times per capture,
against **zero** in the older `last_capture.s2c`. Two hypotheses were wrong
before the right one:

- **"Capture loss on WiFi."** Disproved by printing the driver's drop counter
  (`PacketCapture::Stats::pcap_dropped`, which `gobot_sniff` collected but never
  showed). A 25-second capture reported **801 packets, 801 TCP segments, 0
  dropped**. No loss. The Boot Camp wireless adapter was never the problem and
  an ethernet adapter would not have helped.
- **"The checksum rule is too strict."** Removing it makes things *worse* — 54
  frames instead of 192 — so it is finding real headers.

The actual cause: **opcode `0x0081` is followed by twelve bytes that its length
field does not count.** The scanner finished the frame, met twelve bytes it
could not parse, and resynced. A resync does not cost one frame; it costs
alignment, sliding a byte at a time and swallowing real frames until something
parses again. One `0x0081` every few hundred frames hid most of the traffic.

Measured across four captures, frames recovered / resyncs:

| trailer | stand | track | walk | probe |
|---|---|---|---|---|
| 0 | 339 / 275 | 192 / 721 | 317 / 829 | 143 / 512 |
| 11 | 339 / 55 | 192 / 402 | 317 / 312 | 143 / 259 |
| **12** | **450 / 0** | **715 / 0** | **603 / 0** | **160 / 0** |
| 13 | 336 / 1124 | 232 / 2395 | 306 / 2068 | 134 / 1415 |

Sharply peaked at exactly 12, and 100% coverage on every capture.

**`track` went from 192 frames to 715 — the bot had been seeing 27% of what the
server sent.** That is a large part of why so little of the world model was ever
right, and it is independent of any field-layout question.

Implemented as `trailer_opcode` / `trailer_bytes` on `FramingSpec`
(`total_size_for()`), stripped from the payload so field offsets are unchanged,
parsed by all three config readers, and covered by `test_frame_trailer()`.
Confirmed live: `gobot_sniff --watch` now reports **482 frames, 0 resync bytes**
over 20 seconds where it previously resynced hundreds of times.

The full server vocabulary is visible for the first time:

| opcode | count / 20 s | size |
|---|---|---|
| `0x0038` EntityUpdate | 294 | 88 B |
| `0x0081` | 125 | 16–28 B |
| `0x03C6` | 21 | 4 B |
| `0x0723` | 15 | 8 B |
| `0x003F` | 9 | 44 B |
| `0x004A` | 8 | 136 B |
| `0x004D` | 4 | 32 B |
| `0x004B`, `0x01BD` | 1 | 12 B |
| `0x0265` | 1 | 120 B |

Earlier negative results about s2c were taken under the broken parse and should
be re-run — including "the spawn position is not in there", which was never
properly tested.

## 4c. Re-run after the framing fix — own position is genuinely not sent

Everything in §4b was measured through the broken parse, so it was all re-run on
captures that tile at 100% with zero resyncs, against ground truth read directly
off GoC's `Nhân vật:` line.

`hunt.flow0.s2c` — 798 frames, 0 resyncs, 15 opcodes, with eight paired position
readings taken during it.

**`0x0038` carries the monsters, and none of them is the character.** Grouping by
`id@8` gives **43 distinct ids — exactly the `Quái: 43` GoC displayed**. Not one
id's trajectory passes within 6 units of any of the eight readings. The layout
also now reads sensibly: ids 28–42 are stationary (zero span), ids 19–21 and
39–41 patrol.

**`0x003F` is not the character, and not another player either** — there were no
other players on the map. It was one of the opcodes the `0x0081` bug was hiding.
Structure, stable across captures:

| offset | content |
|---|---|
| @4 | entity id — **only two distinct**: 1005601, 1005701 |
| @8 | 568 / 57, constant per id |
| @10 | alternates 257 (`0x0101`) / 511 (`0x01FF`) — facing |
| @12, @14 | u16 coordinates |
| @20 | 11369 / 11619, constant per id |
| @24 | small int, clustered per id (19–21 / 39–42) |
| @26 | 504 / 516, constant per id |

Two entities with fixed per-entity fields and moving coordinates.

Ruled out by measurement, not by argument:

- **Not the character.** On a 72-second capture with 22 paired readings, id
  1005601 covers **18%** of the character's observed positions (24% of its own
  points lie on the character's path). The character would be ~100%. The 43
  monsters in `0x0038` — known not to be the character — score up to 9%, so 18%
  is barely above the noise floor and is explained by both being where the
  fighting is.
- **Not a monster spawn.** Zero of 15 `0x003F` positions coincide with any
  `0x0038` entity position, and no entity id first appears just after one.
- **Not another player.** There were none on the map.

What it probably is: **item drops.** The ids are seven digits (1005601,
1005701), which is this game's item-id shape — GoC's own assets are named
`8820131.png`, `9210011.png`, `1103013.png`. Positions repeat exactly (the same
monsters die in the same places), sit a median 82 units from the nearest monster
against 218 for random points in the same box, and arrive occasionally rather
than continuously. Not confirmed; nothing depends on it.

**Conclusion, now well-founded: the character's own absolute position is not
transmitted in either direction.** The server broadcasts every *other* actor —
monsters in `0x0038`, players in `0x003F` — and says nothing about you. The
client sends inputs, not coordinates. Both sides integrate from an anchor.

This is the same conclusion §4b reached, but it was an artifact there and is
evidence here: 15 server opcodes and every client opcode, correctly framed,
against real ground truth.

### Useful by-product

`0x003F` gives other players' ids, positions and facing **directly**. GoBot
currently finds them by OCR of the player list, which `START-HERE.txt` records
as unreliable ("ExampleWin" → "Exampley in"). Mapping `0x003F` retires that for
anything positional, and is worth doing regardless of the position question.

## 5. The capture that settles it

The existing data cannot answer §2 because nothing in it moved far enough. What
is needed is one deliberate session, in one map, on one connection:

```powershell
# --record takes a BASENAME; it writes <name>.s2c and <name>.c2s itself.
gobot_sniff --record still --host <server-ip>     # stand still, 30s, Ctrl+C
gobot_sniff --record walk  --host <server-ip>     # walk ONE direction, far, 30s
```

Then compare `still.c2s` against `walk.c2s` at offsets 13–14 across the
`0x00Bx`/`0x00Cx` family:

- **13/14 constant while still, and sweeping a wide monotonic range while
  walking** → position found. Map `self_move` and `WorldAnchor` can go.
- **13/14 varying in both** → it is a timer or sequence, not position. Move to
  the next both-vary pair.
- Values should land in the `0x0038` coordinate box (676..6112 × 395..1120),
  since the character shares the monsters' coordinate space.

`--diff` is **live-capture only** and takes no file argument, which is a gap:
there is no way to re-analyse a recording without replaying it. Adding
`--diff <opcode> --file <capture>`, and a two-file differential mode, is the
tooling change that makes this loop fast — and is worth doing before the next
capture rather than after.

## 6. Server→client, for completeness

`0x0038` (`EntityUpdate`, 88-byte payload, 4,483 samples pooled) — the existing
`id:u32@8, speed:f32@20, x:u16@80, y:u16@82` is **correct**. 28 distinct ids,
~170 samples each, small integers; `x` 676..6112 and `y` 395..1120, constant per
id, i.e. static spawn points.

Bytes 9–11 are constant, so `id` is effectively a u8 in a u32 slot.

Never-varying offsets: 0–3, 9–15, 28–33, 36–39, **64–79**, 84–87.
Live data: 4–8, 16–27, 34–35, 40–45, 48–52, 56–60, 80–83.
Two-valued flag candidates: 16, 40–43, 48, 52 — one of these is plausibly
`alive`, which would serve as a cheap `monster_remove`.
High entropy: 24 (256 distinct), 26 (174), 34 (99).

`0x03C6` — 4-byte payload, ~511 samples, fires steadily. Likely a tick or ack.

### The `id == 0` rejection

`src/protocol_handler.cpp:242` logs `Monster packet 0x%04X has no usable id
field` and returns whenever `m.id == 0`. It fired 109 times. Since ids here are
small integers starting near zero, slot 0 is probably a legitimate entity being
silently dropped. Use a separate "field present" signal rather than treating 0
as absent. It is `GB_DEBUG`, so this is a suspicion, not a confirmed fault.
