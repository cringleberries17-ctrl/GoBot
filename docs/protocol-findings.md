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

**Corrected 2026-10-19:** over TCP it is ciphered after all. Every frame the
client sends there is an `0x0081` wrapper whose content does not unpack; the
"opcodes" below are the wrappers' check words, and the "8-byte trailer" is the
end of their ciphered content. The client's UDP is plain, and carries your
position. See [§4e](#4e-what-the-p0-2-recordings-show-2026-10-18), *The
client's TCP half* and *What this corrects outside §4c and §4d*.

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

**Corrected 2026-10-19:** these are not opcodes. Each number below is the
check word of one of the client's `0x0081` wrappers, w0 + `0x0081` + L
(`0x00C5` = `0x20` + `0x81` + `0x24`), so the table sorts ciphered messages by
their two lengths, and its "trailer" column is the end of their ciphered
content ([§4e](#4e-what-the-p0-2-recordings-show-2026-10-18)).

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

**Corrected 2026-10-19:** the client "opcodes" this section names (`0x00C5`,
`0x00C6`, `0x00EA` and the rest) are the check words of ciphered `0x0081`
wrappers, and the twelve bytes it calls a trailer are a wrapper's header and
four unused bytes ([§4e](#4e-what-the-p0-2-recordings-show-2026-10-18)). Its
server-side results stand.

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

**Corrected 2026-10-19:** not a trailer. An `0x0081` is one frame packed with
liblzf: the twelve bytes are its 8-byte header and four unused bytes, and its
length field counts the packed bytes after them
([§4e](#4e-what-the-p0-2-recordings-show-2026-10-18)). The framing below stays
right.

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

**Update 2026-10-17:** GoC does not treat those 12 bytes as a trailer. Its parser
reads a whole game frame from byte 13 up to the declared length + 12. See
[§4d](#4d-what-gocs-own-parser-does-2026-10-17). The framing above stays
correct; what it throws away is content.

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

## 4c. Re-run after the framing fix — own position not found over TCP

**Corrected 2026-10-19:** this section's conclusion was wrong. Your own position
is sent, by the client, over UDP, which none of these captures included; and the
server sends your spawn position at every map entry. See
[§4e](#4e-what-the-p0-2-recordings-show-2026-10-18). The searches below stand for
what they searched: the server's plain TCP frames and the client's TCP half.

Everything in §4b was measured through the broken parse, so it was all re-run on
captures that tile at 100% with zero resyncs, against ground truth read directly
off GoC's `Nhân vật:` line.

`hunt.flow0.s2c` — 798 frames, 0 resyncs, 15 opcodes, with eight paired position
readings taken during it.

**`0x0038` carries the monsters, and none of them is the character.** Grouping by
`id@8` gives **43 distinct ids — exactly the `Quái: 43` GoC displayed**. Not one
id's trajectory passes within 6 units of any of the eight readings. The layout
also now reads sensibly: ids 28–42 are stationary (zero span), ids 19–21 and
39–41 patrol. (Corrected 2026-10-19: the x, y read here, payload `@80` and
`@82`, is a stale point beside each monster, not where it is, so "stationary"
and "patrol" say nothing about the monsters; the position is payload `@24`,
`@26`. [§4f](#4f-monsters-and-kills-in-the-stream-2026-10-18-recordings).)

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

**P0-2, 2026-10-18:** probably not item drops. In the P0-2 fight `0x003F` came
122 times, exactly as often as the loot lists (`0x004A`, which name the items
dropped and your id as their owner) and `0x005B`, and it carries a monster slot
number (0–23, the slots of `0x0038`) at frame byte 32. So it belongs to a kill,
most likely the dead monster's record (likely, not confirmed); the drops
themselves are in `0x004A`. See [§4e](#4e-what-the-p0-2-recordings-show-2026-10-18).
**Found since (§4f):** it is a new monster spawning in the dead one's slot,
2–4 s after the kill, at one of the map's spawn points: its type id at 12, x, y
at 20 and 22, max HP at 28. So the two "entity ids" above were likely that
map's two monster types.

**Conclusion at the time: the character's own absolute position is not
transmitted in either direction.** The server broadcasts every *other* actor —
monsters in `0x0038`, players in `0x003F` — and says nothing about you. The
client sends inputs, not coordinates. Both sides integrate from an anchor.

**Wrong, as P0-2 showed (§4e).** The client sends its own x, y over UDP: as
floats whenever a walk starts, stops or turns and when it leaves the ground and
lands, and as whole-pixel u16 with every skill cast; and the server sends your
spawn x, y in `0x0086` at every map entry. None of the captures here held UDP.
And the one other actor seen moving in P0-2 came from the server in the same
frame layouts as the client's own reports (`0x0027`, `0x0029`, `0x002A`).

This is the same conclusion §4b reached, but it was an artifact there and is
evidence here: 15 server opcodes and every client opcode, correctly framed,
against real ground truth. (Corrected: those "client opcodes" were the check
words of `0x0081` wrappers whose content is ciphered, so the client's TCP half
was never readable; §4e.)

**Caveat, 2026-10-17:** none of these searches looked inside `0x0081`. The
trailer fix keeps the framing aligned but discards what GoC's own parser treats
as a wrapped game frame ([§4d](#4d-what-gocs-own-parser-does-2026-10-17)).
`0x0081` is the second most common server opcode (125 per 20 s in §4b). No
capture included UDP either, and GoC's client reads your position from two
opcodes, `0x0027` and `0x002A`, that never appear in GoBot's captures (§4d). So
the conclusion holds for the opcodes that were searched, not yet for the stream
as a whole. P0-2 settled it the other way: §4e.

### Useful by-product

`0x003F` gives other players' ids, positions and facing **directly**. GoBot
currently finds them by OCR of the player list, which `START-HERE.txt` records
as unreliable ("ExampleWin" → "Exampley in"). Mapping `0x003F` retires that for
anything positional, and is worth doing regardless of the position question.

**P0-2, 2026-10-18:** `0x003F` turned out to come once per kill (above): it is a
monster spawning (§4f), not other players. Another actor's moves did come from
the server, as plain `0x0027`, `0x0029` and `0x002A` frames in the client's own
layouts, naming that actor's id instead of yours (8 frames, one other id, in the
fight). That is the better lead for other players' positions; see §4e.

## 4d. What GoC's own parser does (2026-10-17)

From a full decompile of `GhostCore.dll`: `PacketCapture::ProcessPacket`,
`ProcessPacketStream` and `ExtractGamePacket`. This is static reading; nothing
was run. The rest of the decompile is summarised in
[goc-reference.md](goc-reference.md#code-level-findings-full-decompile-2026-10-17).

**The P0-2 results** — own position, HP/MP, fury, level and EXP, avatar time and
pet HP, one by one — are in [§4e](#4e-what-the-p0-2-recordings-show-2026-10-18),
which also corrects this section where the recordings disagree with it.

**Frame header, as GoC reads it.** It reads the marker u16 at 0 (`0x0005` in
`configs/protocol.ini`), the opcode as a u16 at 2, and the total frame length
(header included) as a u16 at 4. It does not check the checksum at 6 that
GoBot's spec verifies. GoC captures TCP and UDP on the server's port range and
parses each segment or datagram from its first byte. It sets aside a partial
frame for the next segment, but on a second reading (2026-10-18, below) it
throws that away again before it parses the next one. (P0-2: the first word
reads `0x0105`, bytes `05 01`, not `0x0005`, on every frame that is not an
`0x0081`: 2,860 of 2,860, server TCP and UDP both ways. In an `0x0081` it is not
a marker at all but a length; see below.)

### `0x0081` is a wrapper

For opcode `0x0081` the frame occupies its declared length + 12 bytes: the
"12-byte trailer" of §4b. GoC then takes bytes 13 to the end as **a complete
inner game frame**, with its own header, opcode and length, and handles it like
any other frame:

- it checks the inner frame against the opcode allow-list;
- it runs the character-select, login and captcha hooks on it;
- it forwards it to its server.

If the inner opcode is `0x0081` again, GoC unwraps that too. Its log strings
call these "sub-packets".

**What the bytes are (P0-2, §4e).** GoC's reading takes the right bytes but
misnames them. An `0x0081` is one frame **compressed with liblzf**: its first
word is the length of that frame unpacked, its length field counts the packed
bytes, bytes 8–11 mean nothing, and the packed stream runs from byte 12. That
stream always opens with a run of nine literal bytes, the frame's 8-byte header
and its first body byte, so bytes 13–20 do hold a valid header (opcode, length
= the unpacked length, check word); everything after them is packed. No room
held a second frame or padding, and none nested (616 server wrappers). The
client's own `0x0081` wrappers are ciphered as well and do not unpack.

One rule GoC applies is not understood: when an `0x0081` frame's marker reads
`0x0020` instead of `0x0005`, it stops parsing the rest of that TCP segment.
(P0-2 explains the number: that first word is the unpacked length, so the rule
leaves shut every wrapper holding a 32-byte frame. In the fight those were the
server's two `0x005A` frames, a buff's effect, and 406 of the client's 794
wrappers, all holding a 32-byte message. Why GoC skips them is still not known.)

GoBot keeps bytes 8 to the declared length as the payload (its header is 8
bytes) and drops the last 12. So for every `0x0081` it reads a payload that
starts 5 bytes before the inner frame and is cut 12 bytes short. Until
2026-10-18 it never decoded the inner frame. Since then (plan item P0-1) it
still hands out that payload unchanged, and right after it the inner frame as a
frame of its own, with its own opcode, marked by depth (`GamePacket::depth`;
`FramingSpec::inner_offset`, `inner_frame_offset=13` in `configs/protocol.ini`).
What acts on the game ignores inner frames until P0-3; `gobot_sniff --watch`,
`--verify` and `--inner` show them. (P0-2: the inner frame's opcode and length
are right, but its body is still packed past the first byte, so no field can be
read from it. Since round 3 the tools that read recordings unpack each wrapper
instead, so `--verify` and `--inner` on a recording show the frames inside
whole; the live capture does not unpack yet, which is the first step of P0-3,
§4e.)

**Checked again, 2026-10-18** (`ProcessPacketStream` at `0x180040a10` in this
GhostCore.dll):

- The positions hold, counting from 0: the inner frame is bytes 13 up to the
  declared length L + 12, so it is L − 1 bytes long, and bytes 8–12 are five
  bytes GoC skips.
- GoC never reads the inner frame's own length. It reads its marker and opcode,
  needs at least 6 bytes, and forwards all L − 1 bytes as the frame. GoBot takes
  the same room, and counts how often the inner length field disagrees with it.
  (P0-2: it disagrees whenever compression shrank the frame, because it is the
  unpacked length: 32 of 38 server wrappers in shop, 459 of 576 opened in the
  fight. It never claims less than the room.)
- An inner `0x0081` goes back through the whole parser, so its room is read as
  a run of frames, each by its own length, any `0x0081` among them opened in
  turn. GoBot does the same, to a depth of 4. (P0-2: no wrapper held another.)
- The `0x0020` rule is checked only once a whole `0x0081` frame is in the
  buffer: GoC then forwards nothing from it and stops reading the rest of that
  TCP segment or UDP datagram. GoBot skips only the wrapper's inside and counts
  it, since its own framing runs on the reassembled stream with every header
  proven by its check word; inside a wrapper it stops the run, as GoC does.
- GoC parses each segment from its first byte. It saves an incomplete frame
  under the connection, but the next call for that connection removes the
  saved bytes before parsing, without joining them on. So a frame split across
  two segments is lost to GoC; GoBot's reassembler keeps it.

**Why it matters.** In §4b, `0x0081` arrived 125 times per 20 s, second only to
`0x0038`, at 16–28 bytes: about the size of a small movement or stat update.
§4c's conclusion that your own position is never sent was reached without
looking inside any of them. GoC's server shows your position, HP/MP, fury, level,
EXP and avatar time, all derived from the frames GoC uploads. Any of those not
found in the plain opcodes may be inside `0x0081`. That is an inference, not
confirmed. (P0-2: HP and MP are inside it, in `0x0051`; your position is not, it
comes from the client over UDP. §4e.)

### GoC also captures UDP

GoC's capture filter is `(tcp or udp)` on the server's port range (the strings
`(tcp or udp) and port ` and `(tcp or udp) and portrange ` are in GhostCore.dll),
and it frames UDP payloads the same way as TCP. It drops `0x002E` and `0x0031`
when they arrive over UDP. GoBot's filter was `tcp` only, so no GoBot capture has
ever contained the game's UDP traffic, if there is any.

P0-2 found plenty. The client sends its moves, jumps and skill casts over UDP,
plain, in the server's frame layout: 686 frames in the 123-second fight, none
while standing still. The server sent UDP only during the fight: 88 frames of
`0x02F0`, carrying MP. The `0x002E` and `0x0031` that GoC drops are the
client's own frames: `0x0031` is a skill cast and `0x002E` follows each buff
cast (§4e).

Since 2026-10-18 GoBot's filter is `(tcp or udp)` on the same address and ports;
an explicit `--filter` or `bpf_filter` still replaces it. With `--process`, UDP
is also taken from any host on the servers' ports, as GoC's own filter takes it,
and on the game's own UDP sockets by their local port. Each datagram is framed
on its own, as GoC frames it, and its frames are marked `udp`. `--record` writes
UDP to `<name>.udp.s2c` and `<name>.udp.c2s`, apart from the TCP stream. Unlike
GoC, GoBot drops nothing: `0x002E` and `0x0031` stay in the recording and in
`--watch`.

### Your position, as GoC's client reads it

ZTeam's server relays some raw game packets back to the client: message `0x13`,
carrying the opcode, the length and the bytes. go_client.exe (`FUN_1400c1e90`)
handles two of them and writes the result straight into your position
(`FUN_1400b9320`):

| Opcode | Minimum size | Your x, y |
|---|---|---|
| `0x0027` (a move) | 29 bytes | floats at bytes 16 and 20 |
| `0x002A` (a jump) | 36 bytes | floats at bytes 20 and 24 |

It is not known whether those offsets count the 8-byte frame header. If they do,
the floats sit at payload offsets 8 and 12 (move) or 12 and 16 (jump).
(P0-2: they do. The floats are at frame bytes 16 and 20, and 20 and 24; the
minimum sizes end with the move's facing byte at 28 and the jump's vertical
speed at 32–35.)

Neither opcode appears in any capture GoBot has made: not in the client-side
inventory of §2, and not in the server table of §4b. So they travel somewhere
GoBot has not looked. They could be inside `0x0081`: if §4b's 16–28 B are
payload sizes, its inner frames are about 23–35 bytes, the right size. Or they
could come over UDP. (P0-2: over UDP, from the client, together with two more
position frames, `0x0029` and `0x0028`. The server sent `0x0027`, `0x0029` and
`0x002A` in the same layouts for another actor. §4e.)

The map-change message GoC receives (`0x12`) also carries your x, y on the new
map as floats. So the game's own map-load packet holds an absolute position as
well. (P0-2: it is the server's plain `0x0086`, with x, y as u16 at frame bytes
20 and 22. §4e.)

### Other opcodes GoC names in code

| Opcode | What GoC does with it |
|---|---|
| `0x0014` | Character selected (inside `0x0081`) or channel switch (plain), during auto login |
| `0x0038`, `0x0042`, `0x0051` | Login or map load done; watched only during auto login |
| `0x073D` | Captcha: starts its solver |
| `0x002E`, `0x0031` | Dropped when they arrive over UDP |
| `0x005B` | Logged |

`0x0038` is also the monster `EntityUpdate` of §6. GoC's hook fires on it only
during auto login, where the first one means the world has loaded.

What P0-2 showed of the others (§4e): `0x0042` and `0x0051` come inside
`0x0081` at every map load; `0x0042` is the map's 24-slot monster table and
`0x0051` carries HP and MP. `0x0031` is the client's skill cast and `0x002E`
follows each buff cast. `0x005B` comes inside `0x0081` once per kill.

### Text encodings

GoC flags monsters whose name contains "ThậpNhịYêu". It looks for that name in
three spellings: UTF-8, GBK, and a form where each non-ASCII letter is its
two-byte UTF-16 code. So names in the stream may not all use one encoding.

### What ZTeam's server extracts

These are the fields its decoders return, so each of them is somewhere in the
stream. The types are those of GoC's own messages, not necessarily the game's.

- **You:** session id, HP and max HP, MP and max MP, fury and max fury, x and y
  (floats), moving, facing, avatar state, avatar seconds left.
- **Each monster:** id, position, about a dozen 16-bit stats, name.
- **Movement:** player move (session id, x, y, moving, facing) and player jump.
- **Spell results:** spell id, session id, status (0 = success).
- **Other:** level (u16) and EXP (i32); party members' HP; pet HP and pet death;
  chat lines (sender, channel, text); warp-gate areas; map changes.

### What can be tested now

Checked on 2026-10-17. The only capture still on disk,
`build/bin/Release/last_capture.s2c`, holds 246 frames (`0x0038` ×221, `0x03C6`
×25) and no `0x0081` at all. The `probe`, `walk`, `track`, `stand` and
`hunt.flow0` captures are gone. Testing the lead needs one new recording that
includes UDP, which is plan item P0-2 in [goc-parity-plan.md](goc-parity-plan.md).
It was made on 2026-10-18; the results are in §4e.

## 4e. What the P0-2 recordings show (2026-10-18)

Plan item P0-2: four passive recordings, made by the player on 2026-10-18 with
`gobot_sniff --record --process Game.exe`, TCP and UDP, both directions. GoBot
sent nothing to the game.

| Run | Clock | Length | What the player did |
|---|---|---|---|
| still | 22:15:57–22:16:29 | 32 s | stood still |
| walk | 22:17:04–22:17:36 | 32 s | walked left, into the map's left edge |
| shop | 22:18:01–22:19:03 | 62 s | went through doors: six map loads |
| fight | 22:21:18–22:23:21 | 123 s | fought monsters; took one potion and cast one buff by hand |

The files keep the order of the frames but no times. A time below ("t = 20") is
seconds into the run: the server sends a tick once a second (`0x03C6`), and the
client's UDP frames are placed against it through the skill casts, each of which
costs MP the moment it lands. That is good to about a second. The player noted
no HUD figures during the recordings. Since then the HUD has been read off the
live client's own frames (2026-10-19, `build\live\shots\fragq_A_085.png`, HP
and MP full): Level 162, HP 9232/9232, MP 8902/8902. The two maxima are the
packets' (*HP and MP*, below); the level is not the candidate this section
first named, 88 (*Fury, level and EXP*). The HUD reads Level 162 on the saved
live frames of 2026-10-09 and 2026-10-16 as well, so the character was level
162 when these recordings were made.

The BA worked these results out of the bytes, and a skeptic then re-derived each
claim with separate scripts. Only what held up is written here, in the skeptic's
corrected form where it corrected one. **Found** means the bytes show it.
**Likely** means the evidence points one way but is not proof. A **candidate**
is a field that is there as described, but whose meaning needs a reading of the
HUD. No chat, names or other players' ids are quoted.

**Offsets** in this section count from a frame's first byte, header included,
as GoC's do. GoBot's payload offsets (the `@N` of `[Packet.*]` blocks and of
`gobot_sniff_files --rows`) start after the 8-byte header, so they are 8 less:
the x float at frame byte 16 is payload `@8`, and HP, frame byte 12 of
`0x0051`, is `--rows 0x0051 --inner` `@4`. Where this section means a payload
offset, it says so. Every value is little-endian.
`gobot_sniff_files --rows` reads the UDP and plain frames as they are, and with
`--inner` the frames inside the server's `0x0081`, which the tools that read
recordings unpack (below). The live capture does not unpack them yet.

### The six values

| Value | Result | Where it is | Type |
|---|---|---|---|
| Own position | **Found** | Client → server over UDP, plain: `0x0027` (x, y at bytes 16, 20), `0x002A` (20, 24), `0x0029` (24, 28), `0x0028` (20, 24), and every skill cast `0x0031` (16, 18). An absolute fix at every map entry: server → client `0x0086` (20, 22). | f32 world pixels; u16 in `0x0031` and `0x0086` |
| HP and MP | **Found** | Server → client over TCP, inside `0x0081`: `0x0051` (HP at 12, MP at 16) and `0x0050` (max HP at 68, max MP at 76). MP also over server UDP, plain: `0x02F0` (MP at 12). The maxima, 9232 and 8902, are the HUD's own 9232/9232 and 8902/8902. | u32 |
| Fury | **Candidate** | `0x0051` bytes 20 and 22: 3600 and 3600, never changed | u16 pair |
| Level and EXP | Level: **not found**. The HUD reads Level 162, so the 88 at `0x001E` byte 408, once the candidate, is not the level. EXP: **not found** | inside `0x0081` | u32 |
| Avatar time | **Two candidates**, not told apart: `0x0244` byte 24 (about 20 minutes left) and `0x01BD` byte 16 (about 20 hours left) | plain TCP | u32 seconds |
| Pet HP | **Not found** | — | — |

### `0x0081`: one frame, compressed with liblzf

Found. An `0x0081` frame is laid out like this:

| Frame bytes | Content |
|---|---|
| 0–1 | **w0**: the length of the frame inside, unpacked. Not a marker. |
| 2–3 | `0x0081` |
| 4–5 | **L**: how many packed bytes follow byte 11 |
| 6–7 | check word: w0 + `0x0081` + L |
| 8–11 | no meaning: zero in 420 of 616, otherwise leftovers that look like memory addresses |
| 12 to L + 11 | the frame, packed with plain liblzf |

So a wrapper takes L + 12 bytes, which is where §4b's "twelve bytes past the
declared length" come from: the 8-byte header and the 4 unused bytes.

All 616 server wrappers in the recordings (38 in shop, 578 in the fight) unpack
to exactly w0 bytes and use up all L. Each gives **one** ordinary frame: first
word `0x0105`, its own opcode, a length field equal to w0, and a check word that
holds. No room held a second frame or padding, and none nested. The packed
stream always opens with control byte `0x08`, a run of nine literal bytes: the
8-byte header of the frame inside and its first body byte. That is why reading
from byte 13 finds a valid header. The rest is packed: in the fight, 297,256
bytes of frames came in 67,017 packed bytes, and in shop 50,588 in 3,660. Where
liblzf cannot shrink a frame, L = w0 + 1 (125 wrappers; `0x0051` is 24 bytes
unpacked and 25 packed).

**Unpacking** (plain liblzf; `unpack_wrapper()` in `src/framing.cpp` does this
since round 3). Read a control byte c. If c < 32, copy the next c + 1 bytes.
Otherwise the length is c >> 5, plus the next byte when that is 7; the distance
is ((c & 31) << 8) + the next byte; copy length + 2 bytes, one at a time so that
they may overlap, starting distance + 1 bytes back from the end of the output.
Back-references can reach into the header, so the output must keep it. Accept
the result only when it is exactly w0 bytes, all L bytes were used, it begins
`05 01`, its length field is w0 and its check word holds; otherwise leave the
wrapper shut and count it. Only the server's wrappers unpack: the client's do
not (below), and no UDP frame was a wrapper (0 of 865).

**What GoBot does today.** Since P0-1, the live capture hands out bytes 13 to
L + 12 as the inner frame, as GoC's own parser does. Its opcode, length and check
word are right, but its body is packed past the first byte, so no body field can
be read from it. The counters `length_differs` and `length_longer` count
compression, not second frames. GoC's client does not unpack either, so its
server must (an inference).

Since round 3 (2026-10-19) GoBot can unpack: `FramingSpec::unpack` makes each
wrapper give its one frame, whole, or be left shut and counted when it does not
unpack. No config key sets it. The tools that read recordings do, so
`gobot_sniff_files --verify` lists the frames inside whole (578 of 578 wrappers
in fight.s2c, 38 of 38 in shop.s2c; none of the client's) and `--rows <opcode>
--inner` reads their fields. The live capture does not set it yet: turning it
on, and handing the frames to what acts on the game, is the first step of P0-3.

**GoC's `0x0020` rule** is w0 = 32: GoC leaves shut every wrapper holding a
32-byte frame. In the fight that was the server's two `0x005A` frames (a buff's
effect) and 406 of the client's 794 wrappers.

### The client's TCP half: ciphered wrappers

Found. Every frame the client sent over TCP is an `0x0081` in the layout above
(794 of 794 in the fight), but none unpacks (0 of 963 in the four runs).

- w0 is the unpacked length: 32 (406 in the fight), 24 (197), 52 (149), 256
  (25), 16 (13), 28 (3) and 2572 (1). L is the packed length.
- Bytes 8–11 are a sequence number that grows by exactly 8 per wrapper: 793 of
  793 steps in the fight, and the same in still, walk and shop. The client's UDP
  `0x0028` and `0x0026` carry the sequence number of a wrapper, which ties a UDP
  moment to a TCP one.
- The L bytes from 12 are ciphered. Bytes 13 and 14 are the same for every
  message of one L all session (963 of 963), and byte 12 nearly so: 81 of the
  963 open with a byte other than the one most common for their L. L = 36
  opens `55 43 1D` in 324 of 325 messages; L = 37 opens `F0 E6 B8` 115 times
  and `EC E6 B8` 66 times. Key bytes worked out at two positions agree between
  two different messages of the same L. It is probably liblzf under an XOR key
  chosen by L, its first control byte varying (likely; not decoded). Nothing
  needed for the six values is in it.

Three classes of message line up with what the player did (likely, by count and
timing only):

| w0, L | In the fight | Probably |
|---|---|---|
| 16, 21 | 11, each within about 1.5 s of one of the 11 potions; none in still or walk | using a potion |
| 24, 28 or 29 | 197, following the 195 pickups second by second | picking up |
| 32, 28–37 (mostly 36 or 37) | 406 in the 123-s fight (about 3 a second); one a second standing or walking (32 in 32 s) | a regular report whose rate follows what the character does, and which probably also carries the position: its packed length changed once the character stood at the wall in the walk |

The one 2572-byte message follows the server's one 2572-byte `0x0342`; the pair
looks like a challenge and its answer, probably an integrity check. Nothing the
bot needs.

**AA55 units** (found). shop.c2s and fight.c2s also hold eight 18-byte units
that are not frames (six and two): `AA 55`, u16 10, u16 0, a u16 counter that
runs on across the session (`0x5C` to `0x63`), 8 bytes, `55 AA`. They sit
between wrappers, and each of the shop's six and the fight's first comes right
before the wrapper whose sequence number that map entry's `0x0028` carries (7 of
7). They are what derails `[FramingClient]` on fight.c2s (82.9%): not mixed
connections, and not large frames. Whether they travel on the game connection,
the per-connection `.flowN.c2s` files of the next recording will show.

### Own position: sent by the client over UDP

Found. The client's UDP datagrams hold plain frames in the server's layout:
first word `0x0105`, check word holding, 100% tiled under `[Framing]` (walk 3
frames, shop 88, fight 686). Under `[FramingClient]` 1.8% of shop's bytes and
10.5% of walk's are in a frame, and the fight's 79.6% is 7 false frames, one of
them 17,602 bytes. Five types carry the character's position:

| Opcode | Size | Sent | Fields (frame bytes) |
|---|---|---|---|
| `0x0027` move | 48 | when a walk starts, stops or turns | your id u32 at 8 and 12; **x f32 at 16, y f32 at 20**; speed f32 at 24 (9.0 walking, 0.0 stopped); facing u8 at 28 (`0x01` toward larger x, `0xFF` toward smaller x) |
| `0x002A` air | 40 | on leaving the ground, jumping in the air and landing | your id at 8 and 12; state u32 at 16; **x at 20, y at 24**; speed at 28; vertical speed f32 at 32 |
| `0x0029` | 44 | not settled | your id at 8 and 12; facing u32 at 16 (1 or 255); speed at 20; **x at 24, y at 28** |
| `0x0028` | 36 | four identical copies at every map entry; five more in the fight | a TCP wrapper's sequence number at 8; your id at 12 and 16; **x at 20, y at 24**; speed at 28; flags u32 at 32 (`0x024600xx`, low byte = facing) |
| `0x0031` skill cast | 92 | with every cast | **x u16 at 16, y u16 at 18** (whole pixels); skill id u32 at 20 |

`0x002A`'s state, as read from the vertical speed that comes with it: 0 landed;
1 left the ground (vertical speed −20 for a jump, +1.4 or +12.2 for walking off
an edge); 6 a second jump in the air (−20); 3 a vertical speed of −10; 2 a high
jump (−36).

Evidence:

- GoC's client reads x, y at bytes 16 and 20 of `0x0027` and 20 and 24 of
  `0x002A` (§4d). Confirmed, and those offsets count the header.
- The facing byte agrees with the direction of the next x change in 84 of 84
  cases (shop 5, fight 79): each moving `0x0027` (speed 9) against the next
  position frame after it, when that is an `0x0027`, `0x002A` or `0x0029` with
  another x. Left out: the walk's one, which no position frame follows; 4
  followed by the same x; and 5 followed by an `0x0028`, of which 3 agree and 2
  cross a map change, where x jumps.
- Units: speed 9.0, jump −20 and high jump −36 make 360, 800 and 1440 px/s at
  one step per 25 ms, which are GoC's walking, jump and high-jump speeds exactly
  ([goc-reference.md](goc-reference.md#movement)). No stretch between two skill
  casts implies more than 360 px/s. So the speeds are pixels per 25 ms step (an
  inference from those matches), and y grows downward. Take the speed from the
  frame rather than as a constant: another actor's frames read 9.5.
- At all 7 map entries the client's four `0x0028` hold the server's spawn
  position (below).
- The skill cast's pair is within 2 px of a neighbouring position frame in 47 of
  63 casts, and otherwise at a point the character had walked to.

**Sent on events, not on a clock.** Standing still, the client sent no UDP at
all (0 bytes in 32 s). The walk sent a single `0x0027`, the start (x 3288.0,
y 1155.4, speed 9, facing left), and **no stop**: the character walked into the
map's left edge with the key held. Elsewhere a stop comes as an `0x0027` with
speed 0. In the 123-second fight: 123 `0x0027`, 87 `0x002A`, 34 `0x0029` and 9
`0x0028`, about two a second, plus 63 casts. So between events the position has
to be worked out from the last one (its speed along its facing), and that dead
reckoning must stop at walls and platform ends from the map, because walking
into a wall sends nothing. In 39 of the fight's 123 `0x0027`, bytes 29–31 read
`FF C0 00` and more words follow; what that second form means is untested.

**An absolute fix at every map entry** comes from the server: plain `0x0086`
(24 B) with map u16 at 16, area u16 at 18, **x u16 at 20, y u16 at 22**. The
same x, y are u16 at 60 and 62 of your own record `0x001E` (inside `0x0081`), and
the f32 pair of the client's `0x0028`. All 7 map entries agree:

| Run | (map, area) | Spawn x, y |
|---|---|---|
| shop | (10, 60) | 325, 220 |
| shop | (10, 4) | 690, 1145 |
| shop | (10, 5) | 350, 2430 |
| shop | (10, 4) | 2600, 1120 |
| shop | (10, 5) | 350, 2430 |
| shop | (10, 4) | 2600, 1120 |
| fight | (11, 1) | 4690, 1180 |

**Not the character: `0x010D` and `0x0117` are the pet** (likely, on strong
evidence).

- `0x010D` (52 B; 318 in the fight, 14 in shop, 2 in walk): state u8 at 16 (0
  idle, 1 following with your id at 20, 4 fighting); x, y f32 at 24 and 28;
  speed at 32; an offset pair f32 at 40 and 44: (50, −70) following, (1, −10)
  fighting, (0, 0) at a map entry. It appears at the spawn + (60, −50) on every
  map entry and comes to rest exactly 50 px to the side and 70 px above where
  the character stopped.
- `0x0117` (28 B; 21 in the fight, in bursts of three) moves that point from
  540–1068 px away to near the character: a teleport back.
- `0x0026` (124 B, the fight's first UDP frame: a sequence number at 8, your id
  at 12) holds the character's last point on the previous map (x, y at 16 and
  20) and two of the pet's (at 52, 56 and at 92, 96).
- Your record `0x001E` holds item 9210052 at byte 116, one of GoC's pet icons
  (`data\images\pet\pet\9210052.png`).

The walk's other two frames are the pet's, so the character's end point in the
walk is known only through it: the pet came to rest at x = 75, which puts the
character within about 50–100 px of the left edge.

**The server does not echo your moves.** During still and walk it sent nothing
but its tick. It does send another actor's moves in the client's layouts: the
fight's server stream holds plain `0x0027` (4), `0x0029` (2) and `0x002A` (2),
all naming one other id and none naming yours. So your position comes from what
the client sends, and from the server only at a map entry.

### HP and MP

Found.

| Value | Frame | Frame bytes | Type |
|---|---|---|---|
| HP | `0x0051`, inside `0x0081` (24 B unpacked) | 12 | u32 |
| MP | `0x0051` | 16 | u32 |
| the fury candidate | `0x0051` | 20, 22 | u16, u16 |
| max HP, HP, max MP, MP | `0x0050`, inside `0x0081` (200 B unpacked, the stat sheet) | 68, 72, 76, 80 | u32 |
| MP | `0x02F0`, server UDP, plain (16 B) | 12 | u32 |

Byte 8 is 0 in every `0x0051` and every `0x02F0`. `0x0051` comes at every map
load and after HP or MP changes (116 in the fight), though small drains of 2 are
folded into the next one. `0x0050` comes at every map load and after a buff
lands (3 in the fight). `0x02F0` came only in the fight, 88 times, and samples
more thinly: 44 large drops for 63 casts, 15 rises and 26 drains of 2. In these
recordings max HP was 9232 and max MP 8902.

Evidence: the 63 skill casts the client sent (`0x0031`) match the 63 MP drops of
more than 400 in `0x0051`, in order, at a fixed cost per skill: each of the 63
drops lies in the cost range below of the skill cast in the same place. Put in
a random order, the same drops match 14.5 of the casts on average, and 30 or
more with a chance of about 6 in a million (counted exactly over every order).

| Skill id | Casts | MP cost |
|---|---|---|
| 51102 | 21 | 1200–1204 |
| 51105 | 17 | 1229–1235 |
| 51103 | 9 | 1223–1227 |
| 31111 | 8 | 502–506 |
| 31110 | 2 | 502 |
| 13101 | 3 | 432–434 |
| 13104 | 1 | 760 |
| 13111 | 2 | 1299–1303 |

MP never went above 8902 and HP never above 9232, and the three `0x0050` repeat
the `0x0051` values of the same moment. MP is proven by the casts. HP is the
other pool by elimination and by GoC's own order for the game state (HP, MP,
fury). **Checked on the HUD (2026-10-19):** with both full it reads 9232/9232
and 8902/8902 (`build\live\shots\fragq_A_085.png`), the maxima here.

What the fight did to them, which is not what the plan expected:

- **MP** fell with every cast and rose 15 times: 11 MP potions and 4 drops that
  act when picked up (see *Drops that act when picked up* below). The potions
  were item 8810121 in bag slot 6, its count going from 71 to 60, each worth
  about 5,000 MP (less when it hit the maximum), at t = 20, 38, 50, 61, 71, 87,
  95, 98, 108, 115 and 120. The player took one by hand, so the other ten were
  used automatically; by what, the recordings cannot tell.
- **HP** moved 8 times: down 1 seven times (t = 15, 56, 57, 62, 77, 86 and 100)
  and up 4 once (t = 74), back to its maximum, when a pickup of item 9900005
  filled HP and MP together. No HP potion was drunk: the HP potion stacks (item
  8820141) never changed. No hit on the character shows in any frame.

### Fury, level and EXP, avatar time, pet HP

**Fury: a candidate.** The u16 pair 3600, 3600 at bytes 20 and 22 of `0x0051`
(and 104 and 106 of `0x0050`) never changed: 122 of 122 samples in shop and
fight, 9 of 9 in `0x0050`, and the pair is in no other frame or datagram. For
it: it is the third pool after HP and MP, GoC's own order, and 3600 = 30 × 120
fits GoC's fury rule (SP ≥ (n + 10) × 120). Against it: no fury was used, so any
resource sitting at its maximum would look the same.

**Level: not found. EXP: not found.** 88 is a u32 at byte 408 of your record
`0x001E` (inside `0x0081`, 416 bytes unpacked, one per map load; 7 of 7) and at
byte 196 of `0x0050`, and at no other aligned place in shop or fight (§4f).
It was this section's level candidate, but it is not the level: the HUD reads
Level 162 (2026-10-19, and on the saved frames of 2026-10-09 and 2026-10-16),
so what the 88 is stays open. It was odd for a level from the start, which
other players can normally see: the same `0x001E` field reads `0xFFFFFFFF` in
another player's record. The level is a field of 162 that these notes have not
found yet; the next search looks for that value. For
EXP, three u32 at bytes 160, 164 and 168 of `0x001E` (and 1512, 1516 and 1520 of
the snapshot `0x0065`) read 616, 179,986 and 885,300. 179,986 is 20.3% of
885,300, the shape of "EXP and EXP to the next level"; but they never changed
across 7 records and 5 snapshots spanning about 122 kills, and another player's
record holds 100, 100 and 0 there, which does not fit. Nor does the game's own
EXP table (`zVietnam\Chart\tb_Exp.tbl` in its install folder), which has no row
of 885,300 or 179,986 at any of its levels, so the triple is not "EXP and EXP
to the next level" (§4f). Searched:
every changing u16 and u32 field of every server frame type in the fight,
wrapped ones unpacked. What moves is counters, gold, bag counts, timers and
positions. The three per-kill counters are tested in §4f, and none is EXP:
`0x005B` u32 at 16 steps +3 on every kill, the same for both monster types
(29 to 392); `0x03A0` at 460 counts the kills of one type only (+1 each, 362 to
423); and `0x0065` u16 at 398 reads 798, 804, 805, 806 and 812 in snapshots
taken after 3, 5, 18, 63 and 112 kills, not one step per kill. Either the fight
earned no EXP (a level cap, or monsters far below the character: levels 39 and
44 to its 162), or EXP travels in a frame these recordings do not hold.

**Avatar time: two candidates.** Two exact countdowns in seconds, both falling
one second per second of clock:

| | A | B |
|---|---|---|
| Frame | `0x0244`, plain, 44 B | `0x01BD`, plain, 20 B |
| Sent | once per map load (7 times) | every 60 s |
| Seconds left | u32 at 24 | u32 at 16 |
| Beside it | item id 6810004 at 16; the same item in `0x001E` at 308 and in `0x0242` | a fixed word `0x23726374` at 8, your id at 12; the same count in `0x001E` at 208 and `0x0065` at 1564 |
| Read | 1428 down to 1393 in the shop run, 1226 at the fight's start (167 s later by count, 166 s by the clock) | 71,767 in shop, 71,587 and 71,527 in the fight: a countdown from exactly 72,000 (20 hours) that began at 22:15:00, ±1 s |

Which one is the avatar's is open. GoC presses the avatar key at
`avatar_reset_threshold_minutes` (2) left, and ZTeam relays the time as an i16.
That holds A's 1226 seconds but not B's 71,600, unless ZTeam relays minutes, in
which case B fits too. B's once-a-minute update suits a threshold in minutes;
A came only at map loads here. GoC's window of `avatar_reset_start_hour` 17 to
`avatar_reset_end_hour` 24 fits neither. Plain `0x0077` (24 B: skill id at 12,
72,000 at 16) follows the buffs 13104 and 13111; its unit is unproven (25 ms
steps, milliseconds and seconds all fit), and 72,000 is also where B started.

**Pet HP: not found.** The pet was out in every run (`0x010D`, above), but none
of its fields is HP-like (byte 36 is a float of 3.3–5.5 or about 151, byte 48
never changes), no server frame holds a second HP-like pool, and nothing fired
once the way a death would. Searched: every server frame of shop and fight
(wrapped ones unpacked), every u16 and u32 field with at least four values or a
step pattern, and the client's `0x010D`, `0x0117` and `0x0026`. The pet item
9210052 at byte 116 of `0x001E` (and 2120 of `0x0065`) never changed.

### Other findings for P0-3 and Phase 3

- **Your id** is one small number, the same all session. It is in all 777 of
  the client's UDP frames: at bytes 8 and 12, except in `0x0028` and `0x0026`,
  where byte 8 is a sequence number and the id is at 12. The server uses it in
  your record `0x001E` (12), pickups `0x004D` (12), loot lists `0x004A` (96),
  your hits on monsters `0x0038` (48, the attacker; §4f), `0x01BD` (12) and
  `0x0109` (16). It is not in the monsters' slot space (0–23).
- **A map load** carries only (map, area, spawn x, y), in `0x0086`: no collision
  grid and no map data. Within about a second come wrapped `0x001E` (your
  record), `0x004F` (5,016 bytes of zeros), `0x0042`, `0x0051`, `0x024E` (16 B,
  the same in all 7 loads) and `0x0050`; plain `0x0244` and `0x0242` (the timed
  item, A above), `0x0263`, `0x0739`, `0x03D6` (zeros), `0x073F` (your skills,
  as pairs of skill id and slot << 16 | level) and `0x0727` (zeros); a second
  later, plain `0x0109`. `0x0042` (2,568 B unpacked) is **the map's monster
  table**: every monster on the map, with its slot, type id, position, HP and
  max HP, in columns of 50 entries (§4f). It held 24 on the fight's map and
  none on the shop run's.
  With each map entry the client sends over UDP one or two `0x002C`, three
  `0x0024`, four `0x0028` and one `0x0113`. A table from (map, area) to the
  game's `.map` files is still to be made.
- **Pickups** are plain `0x004D` (40 B), not combat events: your id at 12, the
  drop's id at 16 (the one the loot list gave it; §4f), item id at 20, the bag
  slot the item went to at byte 28 (`0xFF` for gold), and a u32 amount at 32
  (the gold amount; 0 for items). Gold is item 9800002: 106 pickups in the
  fight, 166–223 each. Before each gold pickup comes a wrapped `0x006B` (28 B)
  whose u32 at 12 is your running gold total (105 of 105 increments equal the
  next pickup). Before each of the 89 item pickups comes the list it changed:
  `0x0068` (38), `0x0069` (40), `0x0066` (2), `0x0065` (5) or `0x0051` (4).
- **The bag** is wrapped `0x0068` (1,528 B unpacked): 72 slots, item id u32 at
  12 + 4j and count u16 at 300 + 2j for slot j. Its loot slots match the pickup
  tallies exactly. After each potion an `0x0068` shows that slot one lower, and
  its u16 at 1238 reads 3571 in those 11 frames only (perhaps a cooldown). So
  the potion count can come from packets: the count of the slot holding the
  potion's item id.
- **Drops that act when picked up** (items 99000xx): 9900002 gave MP +6230
  (t = 30), 9900001 filled MP (t = 52 and 73), 9900005 filled HP and MP (t = 74).
  Plain `0x038E` (24 B) comes before all 9 pickups of 99000xx items, and each of
  the 5 pickups of 9900004 comes right after a wrapped `0x0065`, a full snapshot
  of your character. So HP and MP can rise without a potion.
- **A skill cast** is the client's UDP `0x0031` (92 B): your x, y as u16 at 16
  and 18; skill id u32 at 20; byte 24 is 1 for an attack and 2 for a buff;
  byte 26 is 2 when the cast names a target monster (4 of 63 casts), whose slot
  follows as u16 at 28, padded with `0xFFFF` (§4f); and an action counter u32
  at 88 that steps by 2 per attack cast and by 1 per buff cast and per
  `0x002E`.
- **Buffs.** Skills 13104, 13101 and 13111 were cast at t = 17–19 (13101 again
  at 44 and 86, 13111 at 84; the player cast one buff by hand), each followed by
  a client `0x002E` (72 B). The
  server answered each with an `0x0051` for the MP cost. For 13104 and 13111 it
  also sent wrapped `0x005A` (skill id at 12, change i32 at 20, new value at 24:
  −1002 to 2150, then +1060 to 4212, which replaces the first), a new `0x0050`
  (a stat u32 at 136 going 3152, 2150, 4212; the u32 at 92 going 26 to 35),
  plain `0x0263`, `0x0739`, `0x03D6`, `0x073F`, and `0x0077` (skill id,
  72,000). 13101 changed only MP; the recast of 13111 brought only `0x0077`.
  The client's `0x002D` (76 B) carries the same stat as u16 at 20 (3152 before,
  4212 after); what `0x002D` is, is not known.
- **Kills:** 122 in the fight, each a plain `0x0038` hit that leaves the
  monster's HP below zero, with a wrapped `0x005B` right before it (§4f).
  `0x004A` (144 B) is the loot list: up to seven drops, item id u32 at 40 + 4j,
  x u16 at 68 + 2j and y u16 at 82 + 2j for j = 0–6, all three 0 in an empty
  slot (854 of 854 slots read so: 207 filled, 647 empty; gold, 9800002, at 64
  in 108 of the 122), and your id as owner at 96; each drop also has an id, u32
  at 12 + 4j (§4f). `0x003F` (52 B), which came as often, is not part of the
  kill: it is the next monster spawning in the dead one's slot, 2–4 s later
  (§4f).
- **Not monsters chasing you** (corrected, §4f): the 122 plain `0x0038` that
  name your id at byte 48 all have sub-type 9, with speed 7.0 in 112 of them
  and 18.0 in 10. They are your hits, each with its damage and the monster's HP
  after it, and their u16 pair at 32 and 34 is where the monster is, as in
  every `0x0038`. `[Packet.0x0038]` in protocol.ini reads x, y at payload `@80`
  and `@82` instead: a stale point beside the monster.
- **The tick:** plain `0x03C6` (12 B: a 4-byte body, zero in every tick) once a
  second: 32, 32, 62 and 122 in the four runs. With no monsters about, it is
  all the server sent.

### What P0-3 can build on

In this order:

1. Unpack the server's `0x0081` (above). HP, MP, the bag, gold and the map-load
   records are all inside it. The unpacking is written and tested
   (`FramingSpec::unpack`, used by the tools that read recordings); the live
   capture has to turn it on, and what acts on the game to take its frames.
2. Own position from the client's UDP `0x0027`, `0x002A`, `0x0029` and `0x0028`,
   and the pair in `0x0031`; never from `0x010D` or `0x0117`, which are the pet.
   Snap to every event, dead-reckon between events at the frame's speed along
   its facing (jumps at their vertical speed), and stop at walls and platform
   ends, since a walk into a wall sends no stop. `read_own_position()` in
   `include/gobot/own_position.hpp` reads one such frame; nothing uses it yet.
3. Plain `0x0086` for each map change and its spawn x, y.
4. `0x0051` and `0x0050` for HP, MP and their maxima; plain `0x02F0` as a quick
   MP update. Rises also come from drops that act when picked up, not only from
   potions.
5. `0x006B` for gold and `0x0068` for bag counts, the potion count among them.
6. The timers `0x0244` and `0x01BD`, once it is known which is the avatar's.
7. Every monster of the map, from `0x0042`, `0x003F` and `0x0038`: §4f.

Live capture must keep UDP in both directions; today everything that acts on
the game takes plain TCP frames only (`GamePacket::plain()`). The wrapped frames
need the unpacking; the UDP ones need nothing.

### What this corrects outside §4c and §4d

Those two sections are corrected in place. Since 2026-10-19 the places below
carry a short correction in place too, pointing here; this is what each got
wrong:

- **§1**, "The client half is not encrypted": over TCP it is. Every frame the
  client sends there is an `0x0081` whose content does not unpack, and the
  constant bytes §1 measured are what a fixed key per length leaves on repeated
  messages. The client's UDP is plain, and that is where the position is.
- **§2**: `0x00C5`, `0x01A5`, `0x00C4`, `0x00BF` and the rest are not opcodes
  but the wrappers' check words, w0 + `0x0081` + L: `0x00C5` = `0x20` + `0x81` +
  `0x24`, and `0x01A5` = `0x100` + `0x81` + `0x24`. The "8-byte trailer" is the
  end of the ciphered content.
- **§4b**: the twelve bytes are the wrapper's 8-byte header and its four unused
  bytes, and the length field counts the packed bytes after them. The framing it
  gave stays right.
- **configs/protocol.ini**, in comments: `[Framing]`'s "constant 0x0005" is
  `0x0105`. In the `0x0081` comment, bytes 8–12 are four unused bytes and the
  packed stream's first control byte, and the "inner frame" from byte 13 is
  packed. `[FramingClient]`'s opcode at 6 is the wrapper's check word. The
  commented-out `[Packet.0x004D]` is a pickup, not damage: payload `@8` is the
  drop's id (§4f) and payload `@24` the gold amount.
- **README.md**, "The two directions are framed differently": both directions
  use `[Framing]`'s layout. `[FramingClient]` reads the client's wrappers by
  their check words. Its `0x0081` warning: the twelve bytes are the wrapper's
  header and four unused bytes, and the frame inside is packed.

### What the next recording needs

Each candidate needs the HUD read at a known second. The next recording should
note, with the clock time of each reading: fury, EXP percent and the avatar
time left. HP and MP need no more checking (the maxima are the HUD's own), and
the level is known, 162: what is wanted is the field that holds it, so 162 is
the value to look for. It should include one fury
use, one kill that gives EXP (or a level-up), the avatar put on or renewed, the
pet hurt and then killed, and the character hit by a stronger monster (for HP
loss and the death frames). A long walk with GoC's `Nhân vật:` line read during
it would check the dead reckoning and show whether UDP datagrams are ever lost.
`--record` now writes each connection's client half to its own
`<name>.flowN.c2s`, which will show where the AA55 units travel.

## 4f. Monsters and kills in the stream (2026-10-18 recordings)

The player asked whether GoBot can know every monster's position and HP on the
whole map, as GoC does. GoC has them from ZTeam's messages
([goc-reference.md](goc-reference.md#how-goc-knows-about-monsters)): a
snapshot with every monster as id, type id, HP, max HP, x, y, a distance and a
flag; updates that carry the name and stats; removals; and batched positions.
Those are ZTeam's own messages, which its server makes out of the game stream.
This section says which of the **game's** frames carry the same facts.

It rests on the P0-2 fight (§4e): 123 seconds on a map with monsters, and 122
kills. The other three runs had no monster activity. As in §4e, the BA worked
the results out of the bytes, and a skeptic re-derived each one with separate
scripts and its own liblzf decoder; both count the fight as
`gobot_sniff_files --verify` does (2,392 server frames, 578 of 578 wrappers
unpacked). Only what held up is written here, in the skeptic's corrected form
where it corrected one, and **found** and **likely** mean what they mean in
§4e. Offsets are frame bytes, as in §4e. Times are counted in the server's
one-second ticks, so they are good to about a second. The type ids, skill ids
and item ids below are the game's own numbers; no name, chat or other player's
id is quoted.

### The answer

**Yes, all of it but the name.** Every fact GoC's snapshot carries, except a
monster's name, is in the game's own server frames, for every monster on the
map, near the character or not:

- At each map load, `0x0042` (inside `0x0081`) is a table of every monster on
  the map, with its slot, type id, position, facing, speed, HP and max HP.
- Plain `0x0038` is one monster's state event, sent at each change: it stops,
  it starts walking or turns, or it is hit. It carries the monster's position
  and HP. The fight held 1,050 of them, about 8.6 a second, for all 24 monsters.
- A kill is a `0x0038` hit that leaves the HP below zero. No other frame
  removes the monster.
- Plain `0x003F` brings a new monster into the dead one's slot 2–4 s later.

ZTeam's messages to GoC match these frames by what each carries (likely: its
server cannot be seen). The distance is worked out from your own position, and
the name does not come from the game stream:

| ZTeam's message | The game's frames |
|---|---|
| snapshot (`0x11`) | the table `0x0042`, then the `0x0038` events |
| monster update (`0x16`) | `0x003F` for a new monster, and `0x0038` |
| monster removed (`0x17`) | the `0x0038` hit that kills |
| batched positions (`0x21`) | the `0x0038` events |

### The eight questions

| | Question | Answer |
|---|---|---|
| M1 | What tells monsters apart? | **Found.** The slot, a monster's place on the map (0–23 on the fight's map): u32 at 16 of `0x0038`, u16 at 32 of `0x003F`, a u16 column of `0x0042`. A new monster takes a dead one's slot, so a slot is not one monster for good, though it keeps its type (122 respawns of 122). The **type id**, a u32, is in `0x0042` and `0x003F` only. The **name** is in no frame: not found. |
| M2 | Where is each monster, live? | **Found.** x and y, u16 at 32 and 34 of `0x0038`, at every event; between events, worked out from the last one (*How monsters move*). |
| M3 | HP and max HP? | **Found.** HP at 64 of `0x0038` and in the table; max HP in the table and in `0x003F`. A hit carries its damage and the HP it left. |
| M4 | How does a monster die, go and come back? | **Found.** It dies in a `0x0038` hit, state 9, that leaves its HP below zero. No frame removes it: its slot falls silent until `0x003F` brings the next monster, 2–4 s later. |
| M5 | Is the whole map sent at once? | **Found.** The table `0x0042`, at every map load: every monster on the map, as it stands at that moment. |
| M6 | Do monsters target or chase you? | **Not found.** No frame shows it, and no monster hurt the character; likely because they were far below its level (*Monsters and the character*). |
| M7 | What lies on the ground? | **Found.** Plain `0x004A` after each kill lists the drops, each with an id; the pickup `0x004D` and the expiry `0x004B` name that id. |
| M8 | EXP? | **Not found.** Nothing that changed with the kills can be EXP (*Names, EXP, level and fury*). |

### `0x0038`: a monster's state event

Plain, 96 bytes; 1,050 in the fight, naming all 24 slots and no others. One is
sent whenever a monster's state changes; for one slot they came a median 2.0 s
apart (p90 5.0 s, at most 5.6 s).

| Bytes | Type | Content |
|---|---|---|
| 12 | u8 | the state: 0 a stop (409), 1 a walk starting or turning (519), 9 a hit (122) |
| 16 | u32 | the slot (bytes 17–19 are 0 in all 1,050) |
| 24 | i8 | facing: +1 toward larger x, −1 toward smaller x |
| 28 | f32 | speed in pixels per 25 ms step: 2.6 walking, 0 stopped; in a hit 7.0 (112 of 122) or 18.0 (10, all by skill 51103) |
| 32, 34 | u16 | **x and y of the monster now**, in world pixels with y growing downward: the space of your own position |
| 40 | f32 | not understood, and not the length of a walk (*How monsters move*) |
| 48 | u32 | the attacker: your id in all 122 hits, `0xFFFFFFFF` (none) in all 928 stops and walks |
| 52 | u32 | in a hit, its damage (30,431 to 96,450); otherwise 0 |
| 56 | u8 | in a hit, an effect (likely; its meaning needs the HUD): 0 in 87 hits, 1 in 8 (all by skill 51102), 11 in 27 (all by skill 51105) |
| 60 | u32 | 162 with effect 11, otherwise 0 |
| 64 | u32 | **HP now**; in a hit, the HP it left, read as an i32: below zero for a kill (−25,648 to −93,406) |
| 68 | u32 | 200 with effects 1 and 11, otherwise 0 |
| 88, 90 | u16 | **not the position**: a point 1–44 px to the side of the monster and up to 102 px above it, rewritten when the monster is attacked and stale otherwise. `[Packet.0x0038]` in protocol.ini reads this pair as x, y (payload `@80`, `@82`) |
| 92 | u8 | a byte that goes with the monster's type: 1 or 2 on this map, as in `0x0042` and `0x003F` |
| 93 | u8 | in a hit, 1 in 17 of the 122, whose mean damage was 1.40–1.53 times that of the same skill's other hits: likely a critical hit |

In a hit, bytes 57–59 are bytes 1–3 of the monster's type id, left over in
memory with the low byte lost, so even there the type cannot be read. The table
leaves out the bytes that held no field the analysis could find.

Evidence:

- The position: after a stop, the slot's next event has the same x, y (402 of
  402). After a walk, x has moved along the facing (499 of 502; the other 3 by
  0 px), at a median 102 px/s, against 2.6 × 40 = 104. The table's 5 standing
  monsters started their first walk from the table's x, y, and its 19 walking
  ones walked on along the table's facing.
- HP: counting from the table and from each respawn, every stop and walk
  carries exactly the HP so far (928 of 928), and in every hit the HP before
  less the damage is the HP after (122 of 122), the three monsters already hurt
  when the map loaded included.
- The pair at 88, 90 keeps its value from one event to the next except at a
  hit (121 of 122) and in 17 frames right after a respawn, in which the monster
  was struck without damage (*Monsters and the character*).

### `0x0042`: every monster of the map, at each map load

Inside `0x0081`, 2,568 bytes unpacked; one per map load. The fight's map had
24 monsters; all six loads of the shop run had none. The table is laid out in
columns of 50 entries, the room it has: entry k is a monster for k below the
count, and the slot column reads `0xFFFF` past it. It is the map as it stands
at that moment: each HP equals the slot's first `0x0038` HP (24 of 24), three
monsters were already hurt, and the walking ones were mid-walk.

| Bytes | Type | Content |
|---|---|---|
| 12 | u32 | the count of monsters (24, or 0) |
| 16 + k | u8 | 1 walking (19), 0 standing (5) |
| 66 + k | u8 | 44 or 39: likely the level (the middle digits of the type ids, below) |
| 116 + k | u8 | 3 for one type, 0 for the other: not understood |
| 166 + k | i8 | facing |
| 216 + k | u8 | 1 for every monster |
| 266 + k | u8 | the type's byte, as at 92 of `0x0038` |
| 316 + 2k | u16 | the slot (`0xFFFF` past the count) |
| 416 + 2k, 516 + 2k | u16 | **x, y** |
| 616 to 1015 | u16, four columns | 0 in every table |
| 1016 + 2k, 1116 + 2k, 1216 + 2k | u16 | three numbers, likely stats: not understood |
| 1316 + 4k | u32 | a fourth one |
| 1516 + 4k | u32 | **the type id** |
| 1716 + 4k | f32 | speed: 2.6 walking, 0 standing |
| 1916 + 2k | u16 | not understood (under 20,000, different for each monster) |
| 2016 + 4k | u32 | **HP** |
| 2216 + 4k | u32 | **max HP** |

Bytes 2416 to the end are 0. The fight's map had two types, 12 slots each:
1004401 (level 44, max HP 7,676) and 1003901 (level 39, max HP 3,044). An
older capture from another map (`last_capture.s2c`, 2026-10-04) used slots
0–16 and 27–37 in `0x0038`, so other maps use more of the 50.

### `0x003F`: a monster spawns

Plain, 52 bytes; 122 in the fight, one 2–4 s after each kill (by the ticks: 2 s
50 times, 3 s 69 times, 4 s 3 times), in the dead monster's slot and of its
type (122 of 122).

| Bytes | Type | Content |
|---|---|---|
| 12 | u32 | **the type id** |
| 16 | u8 | likely the level, as in the table |
| 18 | i8 | facing |
| 20, 22 | u16 | **x, y**: one of the map's spawn points |
| 28 | u32 | **max HP**; the monster spawns with it full |
| 32 | u16 | **the slot** |
| 34 to 41 | u16, four | four numbers, likely stats: not understood |
| 44 | u8 | the type's byte, as at 92 of `0x0038` |

The slot's next `0x0038` is a stop at the same x, with the same facing and type
byte and HP equal to max HP (122 of 122); its y is 9–36 px lower, as the
monster settles on the ground. The new monster appears at one of the map's
spawn points, not where the last one died: the 122 respawns used 77 different
points, none more than 4 times.

### How monsters move

A monster's position at any moment follows from its last event, to within
about 25 px (p5 −27, median 0, p95 +22 px over about 1,026 events; that
includes up to a second of guessing at times, as the files keep none):

- A stopped monster stays where it is until its next event.
- A walking monster moves speed × 40 px a second along its facing (104 px/s at
  speed 2.6; a median of 102 measured).
- A walk lasts about 200 steps of 25 ms (a median of 5.1 s) and covers speed
  × 200 px: about 525 px at speed 2.6 (517–534 px, p10 to p90, over 249 walks).
  A turn is another walk frame with the facing flipped (136 of 136), and the
  200 steps count from the start of the walk, not from the turn. An older
  capture from another map gives the same: 202 steps (median) at speeds 1.5 to
  1.7.
- A walk that reaches the map's wall ends there at once, with a stop frame (10
  walks at x 47–50).
- After a stop, the next walk starts a median 2.0 s later (p10 0.9 s, p90
  3.0 s).
- y changes by about 60–69 px with no event where a monster walks over a ledge
  or a slope, up or down, so y is exact only at events.
- The f32 at 40, shaped like a length in seconds in about 230 of the 519 walk
  frames, is not one: it follows neither the distance walked nor the time it
  took (correlations 0.16 and 0.12 over 96 walks).

The events cover the whole map: 62% of the 928 stops and walks were more than
1,000 px from the character and the farthest about 4,400 px. The map is only
about 4,700 px wide, so this map cannot tell the whole map from a radius above
4,400 px; the table, though, holds every monster on it.

### Kills

All 122 hits killed: the HP after was below zero every time. A kill comes as:

1. Most often (94 of 122) a step of the counter `0x0723` just before (below).
2. For a level-39 monster only, two identical `0x03A0` (62 pairs for its 62
   kills, none for the 60 level-44 kills).
3. A wrapped `0x005B`, directly before the hit every time (122 of 122); the
   frame after each `0x005B` is a hit (122 of 122).
4. The `0x0038` hit: state 9, the attacker, the damage, the HP after.
5. The loot list `0x004A`, in kill order (*Drops*).
6. 2–4 s later, `0x003F` in the same slot, then the slot's `0x0038` stop.
   Between the hit and the respawn no frame names the slot.

HP never changed without a hit (928 of 928), so no monster was seen to
regenerate. The other frames:

| Frame | Size | Content |
|---|---|---|
| `0x005B`, wrapped | 40 B | u32 at 16 steps +3 on every kill, for both types (29 to 392, 121 of 121 steps); the rest never changes |
| `0x03A0`, wrapped | 1,016 B | u32 at 460 steps +1 per pair (362 to 423): likely a count of that type's kills. Bytes 12–211 are the characters `0` and `1`; the rest never changes |
| `0x0723`, plain | 16 B | u32 at 12 steps +1 (likely once per monster struck) and goes back to 0 3.0–3.8 s after its last step (6 times); 149 steps for 122 kills |

### Monsters and the character

No targeting or aggro by monsters is visible:

- `0x0038` has only the states 0, 1 and 9;
- byte 48 names your id only in the 122 hits, which are yours;
- walks do not lean toward the character at any distance (within 300 px, 28
  walks toward it and 25 away; at 300–1,000 px, 56 and 79; at 1,000–2,000 px,
  93 and 91);
- nothing hurt the character: its HP fell only by 1, seven times (§4e).

Limits: every monster died at the first hit, and the monsters (levels 39 and 44)
were far below the character (level 162 on its HUD), so a monster
that survives and turns on the character was never seen.

Targeting is the character's. Each attack cast killed 0–4 monsters, all within
−511 to +525 px in x and −161 to +148 px in y of where the cast was made. 4 of
the 63 casts named a target: byte 26 of the client's UDP `0x0031` is 2, and the
slot follows as a u16 at 28. Three of those four slots were killed in that
cast's window.

**Attacks that do no damage** (likely). The 27 steps of `0x0723` beyond the 122
kills fall in 17 casts, and the same 17 casts hold the only 17 frames whose
pair at 88, 90 changed without a hit: each the first or second `0x0038` of a
monster that had just spawned, with its HP still full. So a monster struck just
after it spawns sends no state-9 frame at all (likely spawn protection). Three
casts of skill 51102 showed 5 contacts each this way, for one kill. A reader
that counts state-9 frames as the attacks that landed misses these.

### Drops

| Frame | Bytes | Content |
|---|---|---|
| `0x004A`, plain, 144 B: the drops of one kill | 12 + 4j | u32 drop id of drop j = 0–6 (0 to 19 seen; 0 is an id too, so an empty place is told by its item id) |
| | 40 + 4j | u32 item id; 0 for an empty place. Gold, 9800002, always at j = 6 |
| | 68 + 2j, 82 + 2j | u16 x, y: one x, y for the whole list |
| | 96 | u32 the owner: your id |
| | 100 + 4j | u32 1 where there is a drop |
| | 128 | f32 3.0 to 5.6: not understood |
| `0x004D`, plain, 40 B: a pickup | 12 | u32 your id |
| | 16 | u32 the drop id |
| | 20 | u32 the item id |
| | 28 | u8 the bag slot the item went to; `0xFF` for gold and for items that act when picked up |
| | 32 | u32 the gold amount; 0 for items |
| `0x004B`, plain, 20 B: a drop expires | 12, 16 | u32 the drop id, u32 the item id |

- One list per kill, in kill order: 122 lists, 3 of them empty, 207 drops.
- The drops lie at the dead monster's x − 78 × its facing, exact to 1 px in 95
  of the 119 lists with drops, and 15–23 px above its y. Kills by skill 51103
  (hit speed 18) and kills at the wall land elsewhere, likely because the blow
  throws the monster back.
- Kept by drop id, the books balance: each of the 195 pickups names a drop on
  the ground with the same item (195 of 195); 2 drops expired by `0x004B`, each
  30 s after it fell (likely the rule, from 2 cases); and the other 10 were all
  dropped less than 30 s before the end. No id was used again while its drop
  lay on the ground.
- Byte 28 of `0x004D` is the slot holding that item in the bag frame that comes
  just before it (78 of 78 for `0x0068` and `0x0069`). Which bag frame comes
  before each pickup is as §4e says.

### Names, EXP, level and fury

**Names: not in the stream.** No server frame and no client datagram of the
recordings holds a monster's name. Text appears only in your own records, a
challenge (`0x0342`), and runs of numbers that happen to be printable. The
stream gives the type id, so a name has to come from the game client's own
data, keyed by type id. The game's install folder (here
`C:\VTCZone\Games\Ghost_Online_PC_VTC`) holds it in `zVietnam\table`:

- `mob.mdd` is a u32 count, 554, and then 554 u32 type ids, the fight's two
  among them;
- `Mon_Data.etb` (66,017 bytes) is encoded: it starts with the u32s 2, 6334, 0
  and 156,035, and is neither zlib nor plain liblzf. It is the likely table of
  names and stats; it is not decoded yet.

**EXP: not found**, as §4e said. A scan of every server frame type for u16 and
u32 fields that move one way only found counters, bag counts, gold, MP and
timers, and nothing that could be EXP: `0x005B` steps +3 on every kill, the
same for both types, where EXP would differ by monster; `0x03A0` counts one
type's kills; and `0x0065`'s u16 at 398 reads 798, 804, 805, 806 and 812 in
snapshots taken after 3, 5, 18, 63 and 112 kills, not one step per kill. The
triple 616, 179,986 and 885,300 of §4e is not EXP either: the game's EXP table
(`zVietnam\Chart\tb_Exp.tbl`, 200 levels) has no row of 885,300 or 179,986.
Likely the kills paid nothing, the monsters being 118–123 levels below the
character (162 on its HUD); or EXP comes in a frame these recordings do not
hold. The same table can test any EXP field a later recording turns up.

**Level, fury and avatar time:** as §4e. An aligned u32 of 88 is still only at
byte 408 of `0x001E` and 196 of `0x0050`, and it is not the level: the HUD
reads 162 (§4e); the fury candidate 3600, 3600 never
changed (122 of 122 `0x0051`, 9 of 9 `0x0050`); and both avatar-time
countdowns stand (`0x0065`'s u32 at 1564 runs with B, within a second of
`0x01BD`'s).

### What a reader gets without unpacking `0x0081`

Plain: `0x0038` (1,050 in the fight), `0x003F` (122), `0x004A` (122), `0x004D`
(195), `0x0723` (155) and `0x004B` (2). That is the position, HP and death of
every monster, each one heard from within a few seconds of the map load (the
longest gap between one slot's events was 5.6 s), and the drops on the ground.
Wrapped: the table `0x0042`, `0x005B` and `0x03A0`, and the bag and gold frames.
So without unpacking, a reader lacks the type id, level and max HP of the
monsters already there at the load until each one dies and respawns. The type
byte at 92 of `0x0038` happens to tell this map's two types apart, but it
cannot stand in for the type id.

### What it means for GoBot

**Whole-map targeting, as GoC does it.** GoBot can keep the list GoC keeps,
from the game's own frames and with no screen reading: every monster on the
map by slot, with its type id, x, y, HP and max HP, the dead ones marked until
they respawn, the distance worked out from your own position (§4e). GoC's
"Loại mob" filter is a type id. A monster's name needs `Mon_Data.etb` decoded,
or a table of type ids made by hand.

**P0-3** gets one more part, after unpacking `0x0081` in the live capture
(§4e), which the table needs:

1. Build the monster list on `include/gobot/monster_frames.hpp`:
   `read_map_monsters` (`0x0042`), `read_monster_spawn` (`0x003F`) and
   `read_monster_state` (`0x0038`). They are written and tested on hand-built
   frames, and nothing calls them yet. Replace the list at each table; set a
   slot at each spawn; at each event set the position, facing, speed and HP,
   and mark the monster dead at a kill until its slot's next spawn. Between
   events, move each walker as *How monsters move* says.
2. Mend what reads `0x0038` today. `[Packet.0x0038]` in protocol.ini reads x, y
   at payload `@80`, `@82`, the stale pair; the position is at payload `@24`,
   `@26`. `src/protocol_handler.cpp` drops every monster in slot 0 (lines 238
   and 244), and its death test (line 231: HP 0 with a max HP over 0) never
   fires on `0x0038`, whose killing hit leaves HP below zero and which carries
   no max HP: a dead monster would stay listed, with HP below zero, until its
   respawn. Until this is done, the x, y GoBot holds for a monster is a stale
   point beside it, not where it is.
3. Take state-9 frames for kills and damage, not for every attack that landed:
   a monster struck just after it spawns sends none.

**P3-1.** Your position comes from the client's UDP (§4e), so P3-1 shrinks to
wiring and checking, as P0-3 says. Its candidate (b), plates on screen matched
to the passive `0x0038` positions, would have been matched against the stale
pair. With the right x, y, a monster's plate on screen and its world x, y give
where the screen sits on the map: a check on the position from UDP (an
inference, not tested).

### What this corrects

In place: §4c (`0x0038`'s grouping, `0x003F`), §4e (*Other findings*: your id,
the map load, pickups, the skill cast, kills, the frames that were read as
monsters chasing you, and the tick; *Level and EXP*) and §6 (`0x0038`'s fields
and the `id == 0` rejection). Also the `[Packet.0x0038]` and `[Packet.0x004D]`
comments in configs/protocol.ini, and README.md.

### What the next recording needs

- Hits that do not kill: monsters with much HP (a boss, or monsters of the
  character's level), to see an HP after above zero, a chase, regeneration and
  any state besides 0, 1 and 9. Standing next to such monsters would show
  whether and how they attack.
- Another player hitting monsters: whether their hits use the same frame with
  their id, and whether their kills bring `0x005B` and `0x0723` too.
- EXP: a kill that pays some, with the HUD's EXP read at known times; the
  game's EXP table can then check the field.
- The HUD's damage numbers for skills 51102 and 51105, for the effects (bytes
  56, 60 and 68) and the critical-hit flag (93).
- A map with more than 24 monsters, for the table's other columns and the
  numbers not understood.
- Per-frame arrival times in `--record`, which would take the second of
  guessing out of every figure here.

## 5. The capture that settles it

**Note, 2026-10-17:** §4b has since refuted the offsets 13–14 lead below. The
next capture to take is the one in [§4d](#4d-what-gocs-own-parser-does-2026-10-17).

The existing data cannot answer §2 because nothing in it moved far enough. What
is needed is one deliberate session, in one map, on one connection:

```powershell
# --record takes a BASENAME; it writes <name>.s2c and <name>.c2s itself
# (and, since 2026-10-18, <name>.udp.s2c and <name>.udp.c2s).
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

**Corrected 2026-10-19 ([§4f](#4f-monsters-and-kills-in-the-stream-2026-10-18-recordings)):**
the id (the monster's slot) and the speed are right, x and y are not. Payload
`@80`, `@82` is a point beside the monster, rewritten when it is attacked and
stale otherwise, which is why it looked constant per id. The monster's position
is payload `@24`, `@26`, restated at every change of its state. The offsets
below are payload offsets, as measured then; §4f gives each field.

Bytes 9–11 are constant, so `id` is effectively a u8 in a u32 slot.

Never-varying offsets: 0–3, 9–15, 28–33, 36–39, **64–79**, 84–87.
Live data: 4–8, 16–27, 34–35, 40–45, 48–52, 56–60, 80–83.
Two-valued flag candidates: 16, 40–43, 48, 52 — one of these is plausibly
`alive`, which would serve as a cheap `monster_remove`. (§4f: 16 is the facing,
40–43 the attacker, 48 a hit's effect and 52 its number. No flag says alive: a
monster dies in a hit, sub-type 9 at `@4`, that leaves its HP, `@56`, at 0 or
below.)
High entropy: 24 (256 distinct), 26 (174), 34 (99).

`0x03C6` — 4-byte payload, ~511 samples, fires steadily. Likely a tick or ack.
(§4e: the server's tick, once a second; its 4 bytes are 0 in every tick.)

### The `id == 0` rejection

`src/protocol_handler.cpp:244` (line 242 when this was written) logs `Monster
packet 0x%04X has no usable id field` and returns whenever `m.id == 0`; line
238 drops id 0 the same way from a list. It fired 109 times. Since ids here are
small integers starting near zero, slot 0 is probably a legitimate entity being
silently dropped. Use a separate "field present" signal rather than treating 0
as absent. It is `GB_DEBUG`, so this is a suspicion, not a confirmed fault.

**Confirmed 2026-10-19 (§4f):** slot 0 is a monster like any other: 52 of the
fight's 1,050 `0x0038` and 7 of its 122 kills. And the death test at line 231
(HP 0 with a max HP over 0) never fires on `0x0038`, whose killing hit leaves HP
below zero and which carries no max HP.
