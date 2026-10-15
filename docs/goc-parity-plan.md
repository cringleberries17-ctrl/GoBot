# GoBot vs GoC: gaps and the plan to close them

Written by the team's BA from GoC 0.5.8.6 (its configs, templates and the strings in
go_client.exe / GhostCore.dll) and GoBot at commit 09b3836, on 2026-10-14. GoBot line
numbers refer to that commit. Auto login without an account and the new Tự chat tab
were being built at the time and are marked in progress.

## Summary

GoC (ZTeam, version string 'GoC - 0.5.8.6') compared with GoBot at commit 09b3836. GoBot line numbers are from that commit. The login team is editing auto_login.*, gui_app.*, simple_view.cpp and gobot_cli/main.cpp right now. The chat worktree has no changes yet.

**How the two differ.** GoC captures your game traffic, uploads it to ZTeam's server and gets back your position, every monster, your fury and avatar state, and the map. It also needs a paid licence. GoBot reads the screen and makes every decision on your own PC. It needs no subscription, and its packet reading is passive and local.

**Where GoBot already matches or beats GoC:**
- Runs with no licence and uploads nothing.
- F12 panic stop.
- Holds every key while you are dead, loading or disconnected.
- Reads the exact HP/MP figures from the screen.
- Trash cleaning is safer than GoC's: it checks the count, presses Hủy Bỏ on any doubt, and has a 'Thử tìm' dry run.
- A crash, freeze and error-box watcher that reads the error code and limits restarts. It can also start VTCZone itself.
- Party accept and invite rules, stand-still mode, loot, facing the target and target choice are all as good as GoC's.
- A Vietnamese checklist and self-tests.

**Biggest gaps:**
1. **Fighting quality.** Skills fire on timers even when nothing is near. There is no fury, no 'keep distance', one range for every class and no monster list. Your own GoC config has fury and keep-distance switched on.
2. **Features weaker than they look.** The earlier docs/goc-comparison.md overstates these:
   - Your client runs at 1280x720, but the monster finder and the 'pt' chat reader are measured for 1024x768. The hunter probably cannot see the right fifth of the screen, and keyword invites probably read nothing.
   - Revive clicks the middle of the question, which is its text, not Xác Nhận. One revive did succeed in the log, so this needs a live check.
   - All buffs share one 5-minute timer, while your GoC buffs use 60 s and 1600 s.
   - The hunter 'jumps' with Space, which is probably the game's attack key.
3. **No alerts.** Nothing tells you about a death or a disconnect (GoC uses Telegram).
4. **Missing features.** No potion buying or low-stock warning, no pet care, no avatar features, no profiles.
5. **No position.** Without knowing where you stand there are no waypoints, working area, return to spot or map, or live map.

**In progress:** login without account or password (login team) and the 'Tự chat' tab (chat team).

**The plan.** Effort sizes: S is about a day, M a few days, L a week or more.
- **Phase 1** lands those two features, then fixes the reliability problems above: screen sizes, the revive click, per-buff timers with a re-cast after death, and potion tier 2 (no captcha watch: see the player's decisions at the end). It also adds quick fighting wins: class presets with keep-distance and a jump-key check, a monster list, and skills only while fighting.
- **Phase 2:** Telegram alerts, fury, potion-count warning, party leader/full checks and the advert, walking to find monsters, jump attacks, closing stray popups, a stuck-game watchdog, hotkeys and using fragments.
- **Phase 3:** research into your position, then everything built on it, plus buying potions, pets, avatar, profiles, an EXP meter, remote control and follow.

**Not copied:** the licence and payment system, uploading your traffic, solving captchas, auto-Yes on security prompts, fast-key, GoC-only position and server options, and the vendor auto-update.

**Decisions needed from you** (all answered on 2026-10-14 -- see the player's decisions at the end):
- Whether revive may fall back to the village when you are out of revive items (costs 2% EXP).
- Whether to retry after a failed login.
- Quick quest: yes or no.
- A Telegram bot token, for Phase 2.

## Feature by feature

Status counts: missing 33, partial 28, in_progress 3, equal 10, better 10.

### Fighting

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Auto attack (Tự đánh) | partial | high | M | Attacks monsters inside its working area with its skill list. Positions of you and every monster come from its server decoding the captured game stream. Evidence: GoC configs/bot_s0_default.ini:4 auto_attack=true; label 'Tự đánh' 0x177d68, 'Bật tự động đánh quái' 0x178218; GhostCore export CommandExecutor::AttackThread. | 'Tự đánh quái' name-plate hunter: monsters by their red health bar, your character by its typed name; walks to the nearest reachable monster and presses one attack key every 700 ms (simple_view.cpp:671; src/hunter.cpp:146-357; configs/hunt.ini:37,61). The packet bot (src/bot_brain.cpp) has GoC's targeting but cannot run without your own position (gui_app.cpp:1194-1217). | Sees only what is on screen. The detector's search box is fixed for a 1024x768 client (src/mob_detector.cpp:95-104) and reset to that on every start (gui_app.cpp:275), but all 17 saved frames of the player's client are 1280x720 (build/bin/Release/trash_debug/*.png, frame.png). Inferred: monsters right of x=1024 are invisible and part of the chat/skill bars falls inside the box. Needs the character name typed. No working area. |
| Target choice (Chọn mục tiêu: Tốt nhất / Gần nhất / Thông minh / Theo khu) | better | low | S | Four modes in the UI (0x178118-0x178160), but its own tooltip says only one works: 'Chọn mục tiêu (* Hiện tại chỉ có một chế độ gần nhất)' 0x178550; target_mode=0 (bot_s0_default.ini:17). | Nearest reachable target on the same level, the most wounded first among those; never an empty bar; keeps its target until it dies, stays out of reach for 4 s, or vanishes (src/hunter.cpp:197-269; include/gobot/hunter.hpp:50-73). | None for parity. An optional mode switch would be cosmetic. |
| Monster filter (Loại mob) | missing | medium | S | Dropdown 'Loại mob': 'Lọc monster theo loại. Chọn "Tất cả" để không lọc.' (0x180040/0x180050); sent to its server (ProtocolHandler::SendMonsterFilter export). | Missing in the hunter. Plate names are already read with the game's own font (include/gobot/mob_detector.hpp:60) and listed on the full screen's Monsters panel (gui_app.cpp:1993-2021), but target choice ignores them (src/hunter.cpp:227-269). | No 'do not attack' or 'only these' list. A letter can misread, so matching must be loose. |
| Boss priority (Ưu tiên đánh Boss) | partial | medium | S | 'Tự động ưu tiên tấn công quái vật có HP cao bất thường (Boss) trong tầm đánh' (0x17fe88/0x17fee0); priority_boss (bot_s0_default.ini:38). | Packet bot only (src/bot_brain.cpp:264-270; switch at gui_app.cpp:1984). The hunter has nothing. | On screen the bar is a fraction, so 'unusually high HP' cannot be seen. A 'đánh trước' name list is the screen equivalent (inferred). |
| Ignore flying monsters (Bỏ qua quái bay) | partial | low | S | ignore_fly_monster=true in the player's own GoC config (bot_s0_default.ini:13); label 0x177e98. | Packet bot only (src/bot_brain.cpp:231; gui_app.cpp:1983). | Plates carry no 'flying' flag. The name skip list covers it (inferred). |
| Face the target (Đánh đúng hướng, Hạn chế quay người) | equal | low | S | face_target=true, facing_tolerance=12 (bot_s0_default.ini:10-11; labels 0x177dd8, 0x1780c0). When off, it turns back to its starting direction (GhostCore 0xe91b0). | Always taps the direction key before swinging when the target is more than 12 px to the side (src/hunter.cpp:292-298). | Tolerance hard-coded (hunter.cpp:295); no off switch. |
| Attack ranges and class presets (Tầm đánh, Config Mẫu) | partial | high | S | Min R / Atk Range / HY / LY / HT / LT (bot_s0_default.ini:18-19,25-28 = 120/200/131/80/31/31). 'Config Mẫu': class + weapon picks one of four presets in sample_config.ini (Near/Far x Wide/Narrow); UI 0x1797a8-0x179a00; class guide 0x178840-0x17891d. | Hunter: one reach (150 px) and one up/down tolerance (60 px) (configs/hunt.ini:46,50; src/hunter.cpp:286-287). The four presets are on the full screen but feed only the packet bot (gui_app.cpp:1775-1844). | No minimum distance and no separate up/down reach. The presets are not usable by the engine that actually fights. |
| Keep distance (Giữ khoảng cách / backing away) | partial | high | S | backing_away=true in the player's GoC config (bot_s0_default.ini:8); 'Giữ khoảng cách với quái khi tấn công' 0x178470; 'Backing away from monster' GhostCore 0xe9188. | Packet bot only (src/bot_brain.cpp:393-395). The hunter stops at its reach and never steps back (src/hunter.cpp:289-312). | Ranged characters fight point-blank. Risk: stepping back off a ledge, so steps must be bounded. |
| Skill rotation (Dùng skill tuần tự, per-skill cooldown, AoE, mana check) | partial | high | M | Up to 7 skills (skill_0..6 key/cooldown/enabled), used by the attack thread only while attacking, in order when sequential_skill=true (bot_s0_default.ini:29; data/guide/2.png). Each skill has an AoE box and a minimum number of targets (skill_N_aoe_*, skill_N_targets). Skill keys are skipped when mana is low: 'Mana too low (..), skipping attack keys' (GhostCore 0xe9170); 'Using attack skill' 0xe95c8. | 'Tự dùng kỹ năng': up to 12 macro keys, each on its own jittered timer, pressed whether or not anything is in reach, including while walking and at 0 MP (simple_view.cpp:686-711; src/macro.cpp:434-480; include/gobot/macro.hpp:90). A separate fixed sequence lives on the full screen ([Sequence] in macro.ini). AoE and target counts exist only in the packet bot (src/bot_brain.cpp:325-369). | Skills do not follow the fight. Risk: wasted MP and potions; clock-perfect presses look automated. |
| Fury (Dùng nộ) | missing | high | S | auto_fury=true in the player's GoC config (bot_s0_default.ini:5); 'Sử dụng kỹ năng nộ khi có thể' 0x1784c8; 'Activating Fury! SP=' GhostCore 0xe96a8. Fury key key_fury (bot_s0_default.ini:171), default '`' (data/guide/1.png). Fury level comes from packets (DecodeGameState fury/maxFury, 0xea800). A 'Skill 130' points box sits beside it (fury_skill_points=0, data/guide/7.png); its meaning was not determined. | Missing. A fury key exists only in the packet bot's Game keys panel (gui_app.cpp:2197). The HUD's third bar under MP (frame.png, inferred to be the fury gauge) is not read (include/gobot/vitals_reader.hpp reads HP/MP only). | Fury is never used. |
| Jump attack, double jump, down-jump (Đánh khi nhảy, Nhảy đôi) | partial | medium | M | allow_attack_while_jumping, double_jump (bot_s0_default.ini:7,14; labels 0x177e00, 0x177e28). GhostCore logs 'Jump Attack: dy=' 0xe9080, 'Double Jump Attack' 0xe9060, 'Jump Down Attack' 0xe90d8, with a warp-gate guard 0xe8fa0. | The hunter hops once when the target is above (src/hunter.cpp:346-349). The jump key defaults to Space (configs/hunt.ini:40; hunter.hpp:35). This game appears to use Space as its normal attack: GoC 'Đánh thường ... (Default: Space)' 0x179bb0/0x180788; key_normal_attack=32 in the player's GoC config (bot_s0_default.ini:170); GoC's picture of the game keys shows Space bar = 'Tấn công' (data/guide/1_1.png). | Inferred: the 'hop' presses attack, not jump. No attack in the air and no double or down jump, so monsters on other platforms are given up after 4 s. |
| Stand still (Chế độ lười) | equal | medium | S | lazy_mode (bot_s0_default.ini:9); 'Chế độ lười - nhân vật sẽ đứng yên một chỗ' 0x178330. | 'Đứng yên một chỗ' (simple_view.cpp:678; src/hunter.cpp:317-336). | None. |
| Fast key (Bấm phím nhanh) | missing | low | S | 'Cho phép bấm nhiều phím cùng lúc' with a 'simulated human release delay' (0x177ec0, 0x177ee0, 0x177f28; InputManager::SetFastKey / FastKeyReleaseLoop exports). | Deliberately one key at a time through ExecutionCoordinator (include/gobot/execution_coordinator.hpp; docs/goc-comparison.md 'Bấm phím nhanh' row). | Not copied on purpose (see out_of_scope). |
| Confirmed actions and retries | missing | low | L | Waits for the server to confirm attacks, spells and buffs, and retries ('Buff failed (spellStatus=..), will retry' GhostCore 0xe97c0; OnAttackConfirmed / OnSpellBaseConfirmed exports; MAX_SKILL_RETRIES). | Missing on the screen path. Presses are counted, not confirmed (include/gobot/macro.hpp:181-202). | A press the game ignored goes unnoticed. Confirming would need packets or buff-icon checks. |
| Follow a character (Theo sau) | missing | low | M | 'Theo sau một nhân vật và đánh quái trong tầm đánh', marked 'Tính năng đang phát triển' and greyed out (0x178bf0-0x178c60; data/guide/7.png). | Missing. GoBot can already find a named plate on screen (include/gobot/mob_detector.hpp:193). | GoC has not shipped it either. |

### Movement & position

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Working area (Khu vực hoạt động) | partial | medium | L | A box in map coordinates, working_min/max_x/y (bot_s0_default.ini:20-23), dragged on the live map: 'Chỉnh khu vực hoạt động bằng cách kéo thả ô vuông trắng nền xanh' 0x178696; data/guide/6.png; state 'Di chuyển tới khu vực' 0x175b60. | Packet bot only (gui_app.cpp:1949-1980; src/bot_brain.cpp:211-218). The hunter chases anything on screen. | Needs your map position, which GoBot does not have (docs/protocol-findings.md §4c). |
| Waypoints / patrol (Tọa độ, Lùa quái, KillAll) | partial | medium | L | Per-map lists with '+ Thêm vị trí hiện tại', dwell time, loop or ping-pong, Sweep (Lùa quái) or KillAll with a 50-100% goal, a search box per point, and 'Đánh quái trên đường đi' (0x176530-0x177520). Settings in [Patrol], bot_s0_default.ini:162-166. Patrol file format GhostCore 0xe7ec0-0xe7f18; '[Patrol] Sent patrol start' 0x17cbf0. | The packet bot's patrol (src/bot_brain.cpp:489-530) cannot run. The full screen has saved places and a walker that steers by the scenery scrolling (configs/hunt.ini [Move]; include/gobot/navigator.hpp); docs/goc-comparison.md calls it not good enough to build on. The hunter just stands when the screen is empty (src/hunter.cpp:184-193). | No way to go and look for monsters, and no routes. |
| Return to the spot (Trở về địa điểm cũ) | partial | medium | L | return_to_original_location, return_tolerance_x/y 48/31, return_delay (bot_s0_default.ini:30,46-48). 'chỉ hoạt động với LazyMode' 0x17fcc0; 'Captured original location' 0x17d050. | Packet bot only (src/bot_brain.cpp:472-475). | In 'Đứng yên một chỗ' a knock-back moves the character for good. |
| Return to the previous map (Trở về map cũ, Beta) | missing | medium | L | return_to_previous_map=true in the player's GoC config (bot_s0_default.ini:31). 'Khi tự động đánh mà bị dịch chuyển sang map khác, bot sẽ tự động quay lại map cũ' 0x17fd60; 'Map change detected .. Starting return process' GhostCore 0xe9a88. | Missing. GoBot notices a map change by its minimap fingerprint (gui_app.cpp:366-370) and can name the map from its title (include/gobot/map_table.hpp), but cannot travel. | Needs position plus routes through warp gates. |
| Pathfinding and obstacles (Tìm đường theo bục, Vượt chướng ngại vật) | partial | low | L | enable_tile_pathfinding, overcome_obstacles (bot_s0_default.ini:15,39). Labelled 'Pathfinding (Unstable)', 'Vượt chướng ngại vật (Test)' and 'Thử nghiệm: Tìm đường theo bục đứng (Hoạt động chưa ổn định)' (0x178008-0x178058, 0x178b90). 'Stuck detected! Recalculating' GhostCore 0xe9b88. | A platform pathfinder over the game's own .map files exists for the packet bot (include/gobot/pathfinder.hpp; README). The hunter gives up a target it cannot reach after 4 s (src/hunter.cpp:221-225). | Unusable without position. GoC itself labels it unstable. |
| Live map (Bản đồ) | partial | low | L | Live map of platforms, monsters, you, the working area and warp gates; the data comes from its server (DecodeMapBinaryData / MapSegmentData / WarpGateAreas exports; 'Requesting map data from server' 0x17f338). Map Load Guide: 'Hãy di chuyển vào Cửa hàng và ra lại để load bản đồ' 0x178650; data/guide/6.png. | Reads the game's .map files locally (Map tab, gui_app.cpp:1117; include/gobot/map_file.hpp), names maps from tb_Map.tbl (map_table.hpp), and draws a radar of on-screen plates (gui_app.cpp:4436). There is no 'you are here'. | No position, so no live dot. Better than GoC on not needing a server for the map data. |
| Position fix after login (Tự động fix tọa độ sau khi login) | missing | low | S | fix_location_after_login (bot_s0_default.ini:41); 'Thực hiện xoay hướng zig-zag để cân bằng vị trí nhân vật ngay sau khi đăng nhập thành công' 0x17ffb0. | Missing; there is nothing to calibrate. | Only serves GoC's server-side position estimate (see out_of_scope). |
| Crowded-map load reduction (Giảm tải cập nhật quái) | missing | low | S | reduce_monster_update (bot_s0_default.ini:40); 'Giảm tải khi map đông người' 0x17ff78. | Missing; screen reading has nothing to reduce. | Not applicable (see out_of_scope). |

### Survival

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| HP/MP potions with two tiers (HP Heal Lv1/Lv2, MP Heal Lv1/Lv2) | partial | medium | S | Key + % for each tier ('Priority heal when HP <= %.1f%%' 0x180320; bot_s0_default.ini:133-141; data/guide/4.png). HP/MP come from packets. | 'Tự uống bình HP / MP': key + %, using the figures read off the HUD (simple_view.cpp:745-778; include/gobot/vitals_reader.hpp:15-35; include/gobot/macro.hpp:48-59). The simple row drives only the first key of each kind (simple_view.cpp:750); extra tiers only through the full screen's macro table. | No second tier on the simple screen. |
| Revive on the spot (Tự động hồi sinh tại chỗ, Số lần thử) | partial | high | S | player_auto_revive_enabled, player_revive_max_attempts=3 (bot_s0_default.ini:149-150; 0x1802b0, 0x180298). Uses the death dialog's anchors from inven/*/dialog_Panic.spr ('Failed to create panic dialog anchors' 0x17a190); 'Player revived, restarting bot auto' 0x17bb20. | 'Tự hồi sinh': when the question 'Bạn có muốn sử dụng giấy hồi sinh ...' (templates/grabbed/hoisinh_confirm.png) is up, it runs routine 'revive', 3 tries per death, then a 5-minute rest (simple_view.cpp:833-842; gui_app.cpp:2853-2954). The routine is one step with no click offset (configs/routines.ini:160-163), so DialogDriver clicks the picture's middle (src/dialog_driver.cpp:104-106). In that 198x81 picture the middle is the question's text; 'Xác Nhận' is at about x105-142, y57-74. The death panel itself (grabbed/chardeadpanel, grabbed/revive_button_indicator) is never clicked. | Unverified: one revive succeeded in gobot_gui.log (critical hold 'reviving' for 0.7 s), so the game may accept the click; it only works when the game shows the question on its own. No fallback when revive items run out. Risk: a character left dead for hours. |
| Keep running after death (Không dừng Auto khi nhân vật chết) | equal | medium | S | continue_auto_after_death is off in the player's config, so GoC stops ('Player dead — stopping bot' 0x17cf18); option 0x17fbe8/0x17fc50. | Never stops for a death. Every timed key holds while the HUD is gone and resumes when it returns (gui_app.cpp:390-406). | No 'stop on death' choice (rarely wanted). |
| Re-buff after death or relogin | missing | medium | S | 'Buff state reset: forcing all buffs to cast immediately' (GhostCore 0xe92d0); trigger inferred to be revive or map warp. | Missing. Buff timers keep their old schedule across a death or relogin (src/macro.cpp:434-480; suspend/resume only, gui_app.cpp:400-406). | Up to one full buff interval without buffs after every death. |
| Buff timers (Tự buff) | partial | medium | S | Buff slots with their own cooldown in seconds. The player's GoC config has A and S at 1600 s and F at 60 s (bot_s0_default.ini:118-129; data/guide/3.png). | 'Tự buff': all buff keys share one interval in minutes on the simple screen ('One interval for all of them', simple_view.cpp:806-825). The player's live macro.ini has A, S and F all at 300 s. Per-key intervals exist only on the full screen. | No single interval suits both: a 60-s buff is down most of the time, and a 27-minute buff is re-cast five times too often. |
| Heal on arrival | better | low | S | Not found in GoC (inferred). | heal_on_arrival presses the potion keys right after a relogin (src/auto_login.cpp:693 and include/gobot/auto_login.hpp:256 at 09b3836). | None. |
| Cash-shop HP/MP fill (Xử lý máu cash / mana cash) | missing | low | M | fill_cash_shop_hp/mp_* (bot_s0_default.ini:142-148); 'Tu dong fill khi luong mau binh Cashshop duoi %' 0x180610; FillCashShopHpFeature 0x1864c8-0x186758. | Missing. | Only matters to players who use the cash-shop potion. |

### Pets

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Pet heal (Pet Heal, Bơm liên tục) | missing | medium | M | pet_heal_enabled / keycode / trigger_hp / continuous / interval_ms (bot_s0_default.ini:151-155; 0x180440-0x1804e0). Pet HP comes from packets (DecodePetHPUpdate export). | Missing. The pet HP-bar picture is already cut (configs/templates.ini:159 pet_hp_indicator), and the pet slots with their bars are visible on the HUD (frame.png). | Pets die unattended. |
| Pet revive (Hồi sinh pet) | missing | medium | L | pet_revive_enabled, pet_revive_item_icon (bot_s0_default.ini:156-157). RevivePetFeature: close dialogs, open the bag's Pet tab, find the chosen pet icon, double-click it, verify, heal once (0x186be0-0x186e60). 70 pet icons in data/images/pet/pet. | Missing. Pet icons are loaded only for the trash catalog (include/gobot/bag.hpp:231). | Needs a way to see a dead pet on screen; GoC learns it from packets (DecodePetDie). |
| Pet loot cycle (Pet nhặt đồ định kỳ) | missing | medium | M | pet_loot_enabled, every 300 s for 30 s (bot_s0_default.ini:158-160; 0x180550-0x180588). Switches the pet to 'get item' and back to Guard (btn_pet_action_getitem / guard.spr; key_pet_action=221 at bot_s0_default.ini:176; 'Pet loot period ended. Guard stance restored' 0x186ad0). | Missing. The pet action buttons are already named (configs/templates.ini:46-49). | Not built yet. |

### Loot

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Auto loot (Nhặt đồ) | equal | medium | S | auto_loot presses the loot key continuously (0x1782a0; LootThread). Loot key defaults to Ctrl (data/guide/1.png). | 'Tự nhặt đồ': three presses after each kill and one every 2 s while fighting (simple_view.cpp:718-743; src/hunter.cpp:168-174,308-309; configs/hunt.ini:70-72). | Only while hunting is on. |

### Items (bag)

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Delete junk (Xóa rác) | better | high | S | Item processor every 5-60 min: drag to the bin, using the trash_items list and a colour check (item_processor.ini:4-5,36,39; 'Xóa rác' 0x178ef8; ItemManagementFeature 0x182590-0x183228). | 'Dọn rác' tab (simple_view.cpp:1456-1813; gui_app.cpp:3139-3366; include/gobot/bag.hpp): - pick the junk from the client's icons, or photograph the bag; - items are matched by their drawing; - deletes only when the bin's box shows exactly the slot's count, otherwise presses Hủy Bỏ; - 'Thử tìm' dry run; - runs every 5-60 min and holds every key during a pass. | None. |
| Drop junk (Vứt rác) | missing | low | M | drop_items list (item_processor.ini:37); 'Vứt rác' 0x178f18; 'Dropping' 0x1828d0. | Disabled as 'Vứt xuống đất (chưa dùng được)' (simple_view.cpp:1637-1638, 1781-1782). The drop question has no words to tell it apart from other dialogs: templates/grabbed/drop_confirm.png is a blank box with two buttons. | Needs a reliable way to recognise the drop question. |
| Use fragments (Sử dụng mảnh ghép) | missing | medium | M | enable_use_fragments=true in the player's GoC config (item_processor.ini:6); used_items list (item_processor.ini:35); 'Scroll check (UsedItems)' 0x1827f8. | Missing. Fragments can only be deleted; the player's live trash.ini deletes the 1103xxx fragments. | GoBot has no 'use' action. |
| Buff items from the bag (Buff Item) | partial | low | M | enable_buff_items / buff_items (item_processor.ini:7,38). ItemBuffFeature: if the buff icon (icon/dobufficon.spr) is not on screen, open the Spend tab, right-click the item and confirm; next run in 1 hour (0x1859d0-0x186420). | Partial. A buff item put on the hotbar can be a timed buff key, but there is no icon check and no use from the bag. | No icon check and no use from the bag. |
| Item picker library | better | low | S | Item Selector with Spend / Equip / Other / Seal categories, search and pages (0x181db8-0x181e70). Icons in data/images/spend, seal, pet and other; the equip folder is empty. | Catalog of the client's icons by group, plus 'Chụp túi đồ' for anything else, equipment included (simple_view.cpp:1525-1793). | No search box. |

### Shopping

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Buy potions (Mua sắm, Testing) | missing | high | L | [Shopping] enable_shopping, buy_hp/mp_item_id, quantities, every 30 min to 12 h (item_processor.ini:18-27; 0x1790f0; 0x181708). Flow: 1. open the bag's 'Khác' tab; 2. use the Người giữ sân / Hawker item (data/images/other/8950604.png; 'Item phải có trong kho trước khi chạy' 0x181748); 3. open the Consumable tab and pick the item; 4. Buy, enter the quantity, confirm. Stops when the bag is full (0x184320); 2.4-s dialog timeout (0x184768); waits 2 min after a bag pass (0x1839b0); 'Mua đúng số lượng' (0x181910); F9 key (key_shopping=120). | Missing. Click sequences exist only for a fishing rod, bait and a pickaxe (configs/routines.ini:67-89). Pictures of the Người giữ sân shop are already cut (configs/templates.ini:150-152). | A long run ends when the potions do. Risk: spends in-game money, and a misclick buys the wrong item. |
| Safe place before shopping (SafePlace) | missing | low | L | shopping_use_safe_place (item_processor.ini:24); safeplace.ini per map (0x180d00; '[SafePlace] Set at ..' 0x180ac8); walks there first (0x183a58-0x183b38). | Missing. | Needs position and pathfinding. |

### Quests

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Quick quest (Làm nhiệm vụ nhanh) | missing | low | L | enable_quick_quest, selected_quest, every 10 s (item_processor.ini:12-14); 'Tự động nhận và trả nhiệm vụ nhanh' 0x178d28; QuestFeature 0x183280-0x1837c0. About 420 quest pictures in data/images/quest; quest step from packets ('QQ questId=' 0x17f5e0). | Deliberately dropped (gui_app.cpp:1511-1512). | Large picture library, and GoC reads quest progress from packets. |

### Avatar

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Auto avatar (Tự động dùng avatar) | partial | medium | M | enable_auto_avatar, selected_avatar (bot_s0_default.ini:24,42); avatar_key_code=115, F4 (item_processor.ini:11). 'Tự động dùng avatar để tăng chỉ số' 0x177f98. Presses the key when the avatar is off, after sorting the Spend tab (0x1874f0-0x187540, 0x187750). Avatar state and time come from packets (DecodeAvatarToggle / DecodeAvatarDuration; 0xea800). | Partial. The avatar key can be added as a timed buff key; there is no state check. | How the avatar's state and remaining time show on screen is not known yet. The HUD portrait shows its bound key (frame.png). |
| Avatar reset window (Khung giờ reset avatar) | missing | low | M | avatar_reset_start_hour=17, end_hour=24, threshold_minutes=2 (bot_s0_default.ini:43-45). 'Khi avatarDuration < 120s trong khung giờ này, bot sẽ chạy Đổi Nhân Vật để reset avatar' 0x179d40; see also 0x1876b0. | Missing. | Needs Change Character (see Login & reconnect). |

### Party

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Accept invitations (Tự động nhận Party, Chấp nhận tất cả / Chỉ nhận Party từ) | equal | medium | S | party_auto_accept, party_accept_only (party_s0_default.ini:4,13; 0x177618, 0x177668, 0x177840). Skips invitations while a captcha or a chat line is in progress (0x185790, 0x185880). | 'Tự nhận lời mời vào tổ đội': from everyone except the blacklist, or only from listed names. Reads the inviter's name off the dialog and never answers the bin's dialog (simple_view.cpp:1332-1352; gui_app.cpp:2549-2614). | None. |
| Invite by magic word (Tự động Party, 'pt') | partial | high | S | party_auto_party, party_by_magic_word, party_magic_word=pt (party_s0_default.ini:5,8-9; 0x177698, 0x177970). Chat comes from packets (DecodePlayerChat). It invites through the area player list, matching names drawn in the game font data/font/WOD.ttf and caching name pictures in data/players (0x1849d8-0x184b20). | 'Tự mời người chat từ khoá': reads three chat lines at fixed 1024x768 positions (party.ini [Chat] chat_y=710; gui_app.cpp:2483-2487, 2617-2675), matches the whole word, then presses Req Party in the player list (src/player_list.cpp). | On the player's 1280x720 client the first line box (y 710-726) runs past the bottom of the frame, so the reader stops at once (gui_app.cpp:2620-2621). Inferred; verify live. Name reading is loose: gobot.log shows a garbled name read. |
| Invite list and blacklist (Party theo danh sách, Danh sách đen) | equal | medium | S | party_by_list, party_whitelist, use_blacklist, party_blacklist (party_s0_default.ini:7,10-12; 0x177708-0x177790, 0x177878-0x177a00). | 'Chỉ mời người có tên trong danh sách' and 'Dùng danh sách đen'; the blacklist beats every other list (simple_view.cpp:1374-1398; include/gobot/party.hpp:74-80). | None. |
| Leader rules and full party (Luôn làm chủ party) | missing | medium | M | party_always_leader: 'Luôn làm chủ party' 0x1776c8; 'Chỉ tự động mời khi bản thân đang làm chủ party' 0x177a80. Skips 'Đang trong party nhưng không phải chủ party' (0x17d6f0) and 'Party đã đầy (6/6)' (0x17d740). Leader is seen through inven/*/Party_King_My.spr. | Missing. always_leader is read but unused (include/gobot/party.hpp:46); the loop invites regardless (gui_app.cpp:2617-2675). The party HUD pictures exist (configs/templates.ini:153-158). | Wasted invites and 'party full' errors. |
| Priority list (Danh sách ưu tiên, Giữ Slot) | missing | low | M | party_priority_list, party_use_priority_list (party_s0_default.ini:16-17; 0x177b28-0x177bf0); 'slot is reserved for priority members' 0x17d7f8. | Read but not acted on (include/gobot/party.hpp:52,56). | Settings exist but do nothing. |
| EXP / loot share (Chia EXP, Chia Loot) | missing | low | M | party_exp_share_type, party_drop_share_type (party_s0_default.ini:14-15; 0x177ad0, 0x177af8). Set with the btn_expshare / btn_expmore / btn_lootingbasic / btn_lootingfree buttons. 'Party of 5: Toggling Loot Share to' 0x185468 and 'Restoring original Loot Share to' 0x185540. | Read but not acted on (include/gobot/party.hpp:68-69). | Settings exist but do nothing. |
| Party and area player lists in the window | missing | low | M | Party member table with slot, name, level and LEAD (0x17f2bc-0x17f2f0; DecodePartyUpdate). Area players read by OCR (AreaPlayers 0x17f280; '(OCR: %s)' 0x17f30b). | Missing from the UI. PlayerList reads rows only to invite (src/player_list.cpp). | Nothing shown in the UI. |
| Party advert when alone (Tự chat khi không có party) | missing | medium | S | 'Tự động gửi tin nhắn party mỗi phút khi không có trong party' 0x177ce0, with 'Thời gian retry (giây)' 0x177d48. A per-message 'Party Request: only send when NOT in a party' (0x180be8; chat_N_is_party_request). | Missing. It is optional in the chat spec (ba_spec.json, chat requirement C-R16). | Depends on Tự chat and on knowing whether you are in a party. |

### Chat

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Timed chat (Bật chat, Chat Queue, Kiểm tra Chat Input) | in_progress | medium | M | chat_enabled (bot_s0_default.ini:178-179). Messages with delay, repeat and party-request flags, a queue with 'Add to Queue', and a 'Kiểm tra Chat Input' check (0x1762b8-0x1763c8, 0x180b08-0x180cc8). Types only A-Z, a-z, 0-9 and space (GhostCore SendChat; see ba_spec.json chat.goc_behaviour). | In progress: the 'Tự chat' tab is being built in the GoBot-chat worktree (branch auto-chat; no edits yet). Today InputManager::send_chat is never called (src/input_manager.cpp:536). | Target is BA spec C-R1..C-R17. Open question: per-line delays as in GoC, or one interval. |

### Login & reconnect

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Auto login and re-login (Bật AutoLogin, Kênh 1~8, Nhân vật 1~3) | in_progress | high | M | item_processor.ini:41-48: al_enabled, channel, character, launcher process names. State machine with 3 tries per stage and 'Dynamic Detection' (0x187148-0x189e50). Never types an account: VTCZone must already be running and signed in. | In progress. The login team is editing include/gobot/auto_login.hpp, src/auto_login.cpp, apps/gobot_cli/main.cpp and the GUI to remove the account step, stop at VTCZone's sign-in form and give reasons in Vietnamese. At 09b3836 the tab still shows account boxes (simple_view.cpp:1847-1897). GoBot can start VTCZone itself (src/auto_login.cpp:406-407 at 09b3836); GoC cannot. | Target is BA spec L-R1..L-R13. Still open at 09b3836: - BẮT ĐẦU refuses when there is no game window (simple_view.cpp:1112-1115); - the keeper stops after one failed crossing (gui_app.cpp:2787-2790); - the GUI keeper loads pictures only from the helper DLL (gui_app.cpp:2724-2727). |
| Log in on demand (F8) | missing | low | S | '[HOTKEY] Trigger AutoLogin pressed (F8)' 0x17c5a8. | Missing from the GUI; only 'gobot_cli --login'. An optional 'Vào game ngay' is in BA spec L-R9. | No hotkey or button for it. |
| Disconnect and crash detection | partial | high | M | Counts as disconnected: the game window missing, a 'Connect Error' window or child, or a VC++ runtime box. In game, an OK button (inven/*/ok.png) means click, kill and restart (0x1886f0-0x1888e0, 0x189c40-0x189cb0); 'taskkill /F /PID' 0x1889d8. | CrashWatch: process gone, window not answering for 20 s, any message box (its error code read from the text), and Windows' crash reporter. At most 5 restarts with a 15-s cooldown (include/gobot/crash_watch.hpp:39-44,73-86; gui_app.cpp:2708-2801). | Better than GoC for crashes and hangs. Inferred gap: a disconnect drawn inside the game, with no Win32 box and the process still alive, only reads as 'not in the world'. Keys hold forever and nothing restarts. |
| Change character (Đổi Nhân Vật) | missing | low | M | ESC, main menu, btn_GoSelectChar, character select; 3 tries with an 11-s cooldown, falling back to a full login (0x187de8-0x1886a0). | Missing. The button picture is already named (configs/templates.ini:43 go_select_char). | Needed for the avatar reset. |
| Window placement after login | partial | low | S | Minimises VTCZone, brings the game forward, moves it to (0,0) and drags its title bar 'to fix render' (0x189178, 0x189be8, 0x189d10). | Follows the new game window after a relogin (gui_app.cpp:2809-2821) and moves its own window off the game (simple_view.cpp:425-440). It does not move the game. | Does not place the game window. |
| Security Alert boxes | in_progress | low | S | Clicks '&Yes' on 'Security Alert' (#32770) boxes from the game's launcher (0x187100-0x187128; 'initial Security Alert(s) accepted' 0x188e50). | In progress: the login rework stops and says so instead (include/gobot/auto_login.hpp, uncommitted, Why::SecurityAlert). | Deliberately different (see out_of_scope). |

### Notifications

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Telegram alerts | missing | high | M | notify_telegram_on_death, _chat, _chat_channel, _disconnect and _party_update (bot_s0_default.ini:33-37). Labels: 'Thông báo Telegram khi chết' 0x17fae0, 'Thông báo mất kết nối qua Telegram' 0x17fb00, chat 0x17fac0 and 0x17fb30, party 0x17fb80. Linked by QR code to GoC's own Telegram bot and sent through GoC's server (HTTPClient::NotifyDisconnect / NotifyLoginSuccess exports). | Missing; listed as not built at gui_app.cpp:1500. | Nobody hears about a death or a disconnect at 3 a.m. Risk: the bot token must stay secret. |
| Remote control | missing | low | M | 'Liên kết tài khoản với Telegram Bot để nhận thông báo nhân vật chết và điều khiển từ xa' 0x17f8a0; ProtocolHandler::DecodeCommandChatRemote / DecodeCommandPartyRemote exports. | Missing. | Risk: whoever holds the token can drive the bot, so commands must be accepted only from the player's chat id. |

### Safety & control

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Captcha (Window of Truth) | missing | high | S | Detects it by packet 0x73D ('Triggering solver', GhostCore 0xe8dc0) and by the dialog picture data/images/captcha/captcha.png. Engages critical mode so other features wait ('Captcha Pending' 0x184af8; 'EMERGENCY STOP: Captcha detected!' 0x182758). Then solves it with 36 character templates (data/images/captcha/number; '[CaptchaFeature] Solved.' 0x182138). | None, by the player's decision (2026-10-15): the packet-path detection, its hold and the login's captcha stop were removed, and nothing in GoBot reacts to a captcha. | Not wanted (see the player's decisions below). Solving stays out of scope. |
| Close stray dialogs | partial | medium | S | FeatureUtils CloseDialog finds type 1 and type 2 dialogs, the CloseBox / CloseDialog buttons and the Monster Dictionary, and presses ESC before its own jobs (0x181fb0-0x1820c8, 0x182620, 0x183b70; inven/*/btn_CloseBox.spr, btn_CloseDialog.spr, monster.png, monsterf.png). | The 'close_anything' routine (configs/routines.ini:119-151) and DialogDriver::close_popups (src/dialog_driver.cpp:144-197) run only from 'gobot_cli --close'; nothing closes popups during a GUI run. close_popups also clicks dialog_ok and confirm_button, which agrees to things. | A popup such as the daily check-in reward (grabbed/login_popup1) can sit over the game for hours. |
| One input at a time (coordinator, critical holds) | equal | medium | S | ExecutionCoordinator RequestAction / EngageCriticalMode / IsCaptchaExpectedSoon (exports). Features wait on 'Coordinator Denied (Another action running)' 0x184b78. | Same design: coordinator priorities, critical holds take_hold / end_hold, and one mouse mutex shared by trash, party and revive (include/gobot/execution_coordinator.hpp; gui_app.cpp:3122-3135). | None. |
| Keeping the game in front (Focus Window) | better | medium | S | 'Tự động focus cửa sổ game khi bot đang chạy mà window mất focus' 0x180a00 (WindowFocusMonitor). | Sends keys only while the game is in front, and brings the game back only after 30 s with no mouse or keyboard use (simple_view.cpp:950-957, 1232-1251). | None; it does not fight the user for the desktop. |
| Panic stop | better | medium | S | Not found; only Start/Stop on F10. | F12 stops everything and releases every key, from inside the game (gui_app.cpp:320-340). | None. |
| Hold while not in the world | better | medium | S | Handled feature by feature: 'Player Died. Aborting', 'Map Warped. Aborting' (0x182258, 0x182288). | Every timed key holds while the HUD is missing (gui_app.cpp:390-406). | None. |

### Keys & hotkeys

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Global hotkeys | partial | low | S | [KeyConfig] key_start_stop=121 (F10), key_item_manager=122 (F11), key_shopping=120 (F9), key_buff=119 (F8) (bot_s0_default.ini:172-175); AutoLogin on F8 (0x17c5a8). Registered with RegisterHotKey (0x17be08-0x17c450) and ignored when the game is not in front (0x17c578). | F10 start/stop, F11 hunter on/off, F12 panic. Read with GetAsyncKeyState, so the game still receives the key (gui_app.cpp:302-349). | No 'clean the bag now' or 'log in now' key. F10 also fires while typing in another window. |
| In-game key settings (Cấu hình phím trong Game) | partial | medium | S | Loot (default Ctrl), normal attack (Space), fury (`), pet action (]), bag (I), quest, map (M), avatar (F4) (bot_s0_default.ini:168-176; item_processor.ini:8-11; 0x179a10-0x179c60; data/guide/1.png). | Attack, loot, jump, left and right in hunt.ini (configs/hunt.ini:37-41, 70-72); bag key in trash.ini. Fury and normal attack exist only in the packet bot's panel (gui_app.cpp:2187-2219). | No fury, pet or avatar key on the simple screen. Jump defaults to what appears to be the game's attack key (see the jump row). |

### Info & UI

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Character panel (Thông tin nhân vật) | partial | low | M | Level, EXP, position, direction, HP/MP/Fury, avatar and its time, state, target id and HP, monster count (0x175980-0x176050, 0x175ec8-0x175ff0). All from packets. | HP and MP figures, what the bot is doing, run time, kills, potions and buffs used (simple_view.cpp:558-581, 961-1053). | No level, EXP or fury. Position is not possible yet. |
| EXP meter (Đo EXP) | missing | medium | M | Session EXP and EXP per hour (0x1763d8-0x1764bc; DecodePlayerExp). | Missing. The level is printed on the HUD (frame.png 'Level'); the bar along the bottom is inferred to be EXP. | No EXP or level reading. |
| Nearby monsters list (Quái vật xung quanh) | equal | low | S | List with ΔX, ΔY, distance and HP (0x1759a8, 0x175c28-0x175c68). | Monsters panel and Radar (gui_app.cpp:1993-2021, 4436). | Only monsters that are on screen. |
| Language (English / Tiếng Việt) | partial | low | M | Every label in both languages; config.ini language=1 (0x179f10-0x179f18). | The simple screen is Vietnamese; the full screen is English only (simple_view.cpp:363). | No Vietnamese on the full screen. |
| Theme (Tối / Sáng / Tự động) | missing | low | S | Follows Windows' light/dark setting (AppsUseLightTheme 0x181d10; 'Sáng' 0x179184). | Dark only (inferred). | No light theme. |
| Help and guides | better | medium | S | '?' tooltips plus the map-load and config-selection guides (0x1785a0-0x17891d; data/guide/*.png). | A 'Trước khi bắt đầu' checklist with the fix beside each red line, and plain Vietnamese hints under every control (simple_view.cpp:381-556). | None. |
| Saving settings | equal | low | S | 'Đang tự lưu...' and 'Lưu cấu hình' (0x1781b0-0x1781e8). | Every change is saved at once and file comments are kept (simple_view.cpp:1191-1209). | None. |

### Profiles & multi-client

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Named profiles (Cài đặt chung) | missing | medium | M | Separate bot and party profiles with new, rename, clone and 'Show all' (config.ini last_bot_config, last_party_config; configs/bot_s0_default.ini, party_s0_default.ini; 'Profile Created' 0x180d88; 'Clone Current...' 0x180dd8; Config::RenameBotConfig export). | Missing: one set of files in configs/ beside the exe. | Switching characters or classes means re-entering every key. |
| Several clients | partial | low | M | One GoC per game client, each a paid session (config.ini last_selected_sub / sub_type_<id>; 'Sessions: %d/%d' 0x176148; 'Tạo sub mới (chạy nhiều máy)' 0x18a2d0). The licence part is out of scope. | One game window per GoBot. Inferred: two copies in one folder would share every config file, since all paths are exe-relative (e.g. simple_view.cpp:1119-1122). | No separate config set per copy. |
| Game versions and skins | partial | low | M | Two UI skins (data/images/inven/0 and /1) and a VGO variant ('VGO templates' 0x183600; login/1/start_game_vgo.png). | VTC only. Skin 0 is named (configs/templates.ini:32-85); skin 1 is documented. | Not needed for this player's client (inferred). |

### Logging & diagnostics

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| Logs and debug pictures | equal | low | S | Log lines per feature and debug images on a miss ('Saved debug images to %s_screen.png ..' 0x1872f0; 'debug_not_found_' 0x182da8; log_shopping.txt 0x1838a0). | gobot.log and gobot_gui.log, trash_debug/ frames, and the last log lines shown on each tab (simple_view.cpp:1288-1292, 1806-1811). | None. |
| Self-checks | better | medium | S | Only 'Kiểm tra Chat Input'. | 'gobot_cli --doctor', --crashcheck, --see, --template, --survey, --trash --dry, and the gobot_test suite (apps/gobot_cli/main.cpp:106-318 at 09b3836). | None. |

### Architecture & vendor

| Feature | Status | Value | Effort | GoC | GoBot | Gap |
|---|---|---|---|---|---|---|
| World knowledge | partial | high | L | Its server turns the captured stream into your position and direction, every monster with its HP, your level, EXP, fury, avatar state, the party, chat, and map geometry (DecodeGameState 0xea800; ProtocolHandler Decode* exports). | What is on screen: plates and their bar fractions, your own plate by name, HP/MP figures and the map name. Passive capture also decodes monster ids and positions (configs/protocol.ini [Packet.0x0038]). Your own position is not sent in either direction (docs/protocol-findings.md §4c). | This is the root of every movement gap: working area, waypoints, return to spot or map, live map. |
| Independence and privacy | better | high | S | Needs a paid subscription and its server ('Bản quyền: HẾT HẠN' 0x175d30; 'Không thể đăng nhập đồng thời 1 tài khoản' 0x17d638). Uploads filtered game packets to that server (PacketCapture::BatchThread, ProtocolHandler::EncodePacketBatch / DecodeCommandBatch exports). | Runs entirely locally, with no account, licence or upload. Capture is read-only (README 'Capture is read-only'). | None. |
| Updates | missing | low | S | WinSparkle auto-update ('Kiểm tra Update' 0x175940; appcast string 0x1750a8). | Missing. GoBot is built from its own repo (build.ps1, package.ps1). | Not copied (see out_of_scope). |

## The plan

Effort: S is about a day, M a few days, L a week or more. Every item goes through the team:
BA → coder → reviewer → tester, with a live test on the player's client where marked.

### Phase 1

1. **P1-1 Land auto login without account or password (login team, in progress).** — effort M (live test).
   - Why: GoC never types an account and starts from a signed-in VTCZone (item_processor.ini [AutoLogin]; ba_spec.json login). GoBot's account step fails instantly when the account is empty. The work is under way in the main repo.
   - Done when: BA spec L-A1..L-A5 pass offline and L-A6..L-A10 pass live. The player's decisions are recorded on two points: retry after a failed crossing, and BẮT ĐẦU from a closed game (L-R9). The keeper loads pictures the same way as the CLI on a package without the helper DLL (L-A5).
   - Depends on: login team
2. **P1-2 Land the 'Tự chat' tab (chat team, in progress).** — effort M (live test).
   - Why: GoC has timed chat with a chat-line check (0x1762b8-0x1763c8, 0x180b08-0x180cc8). GoBot's send_chat has never run.
   - Done when: BA spec CA1..CA7 pass offline and CL1..CL9 pass live. The open question on per-line delays versus one interval is answered by the player.
   - Depends on: chat team
3. **P1-3 Make the screen readers follow the client size (1280x720 as well as 1024x768). - Derive the MobDetector search box and its exclusion from the frame size and the HUD (top bars, chat panel, skill bar, minimap), and save it when edited. Today it is fixed at 0,104,1024,566 and 812,0,212,360 (src/mob_detector.cpp:95-104) and reset on start (gui_app.cpp:275). - Find the keyword-chat lines from the chat bar picture (inven/0/dialog_chat_res3.spr/1, at about (85,698) on 1280x720 per ba_spec CA5) instead of party.ini chat_x/chat_y. - Check the player-list and invite geometry (party.ini [PlayerList], [Invite]) the same way. - Load the HP/MP HUD landmark from templates/grabbed/hpmp_indicator2.png when the helper DLL is absent (src/vitals_reader.cpp:84-97).** — effort M (live test).
   - Why: Every saved frame of the player's client is 1280x720 (build/bin/Release/trash_debug/*.png, frame.png). With 1024x768 geometry the hunter cannot see right of x=1024. The keyword reader's first box (y 710-726) lies past the bottom of the frame, so it stops at once (gui_app.cpp:2620-2621). Two shipped features silently do less than they claim.
   - Done when: Offline (new gobot_test cases): - (a) A plate blitted at x=1150, y=400 on a 1280x720 frame is found; the top-right minimap and quest log stay excluded; the existing 1024x768 detector tests still pass. - (b) On frame.png the chat reader locates the chat bar and returns non-empty lines from the chat panel; a 1024x768 frame still reads. - (c) The vitals reader with no helper DLL logs the HUD landmark loaded from templates. - 'gobot_cli --see' on the live client lists plates right of x=1024 when monsters stand there. Live at 1280x720: 1. The hunter walks to and attacks a monster at the right edge. 2. A second character types 'pt' and is invited within 30 s. 3. Point 1 also passes at 1024x768.
5. **P1-5 Fix the revive: press 'Xác Nhận', and start from the death panel. - (a) In routines.ini [Routine.revive], give step 1 the Xác Nhận offset measured on hoisinh_confirm.png (about click_dx=123, click_dy=65) instead of the picture's middle. - (b) Add an optional first step: when the 'Trạng thái bất tỉnh' panel (grabbed/chardeadpanel) is up without the question, click 'Sử dụng vật phẩm hồi sinh' (grabbed/revive_button_indicator) and wait for the question. - (c) After 3 failed tries, say in Vietnamese that revive items may have run out. - (d) Add 'Hồi sinh tại làng nếu hết vật phẩm', off by default (-2% EXP per the panel).** — effort S (live test).
   - Why: The routine has no click offset (routines.ini:160-163), so DialogDriver clicks the middle of the question, which is its text (dialog_driver.cpp:104-106). The death panel's own buttons are never pressed. GoC drives the death dialog itself (inven/*/dialog_Panic.spr anchors, 0x17a160-0x17a190; player_revive_max_attempts=3).
   - Done when: Offline (gobot_test): - With hoisinh_confirm blitted at a known spot, the click lands inside the Xác Nhận rectangle. - With only chardeadpanel up, the click lands inside 'Sử dụng vật phẩm hồi sinh'. - With neither up, there is no click. - Three failures lead to the 5-minute rest and the 'hết vật phẩm' message. - 'gobot_cli --routine revive --dry' prints both click points. Live (player present, a safe low-level map, one revive item in the bag): - Die with 'Khóa' unticked: revived within 15 s and the run resumes. - Repeat with 'Khóa' ticked.
   - Changed by the player's decisions (at the end): no village option (d) and no 5-minute rest -- three failed tries at one death stop the whole bot and say why. Built as a reviver class (src/reviver.cpp) rather than as routines.ini steps.
6. **P1-6 Per-key buff timers, and re-cast after death or relogin. - On the Đánh quái tab, give each buff key its own interval (10 s to 120 min) instead of the shared minutes box (simple_view.cpp:806-825). - When the HUD returns after being gone for 10 s or more, fire every buff key once, 1-2 s apart, then resume its timer.** — effort S (live test).
   - Why: The player's GoC buffs run at 1600, 1600 and 60 s (bot_s0_default.ini:118-129). GoBot uses one 300-s interval (live macro.ini keys 4, 5 and 10). GoC also resets buffs after a death: 'Buff state reset: forcing all buffs to cast immediately' (GhostCore 0xe92d0).
   - Done when: Offline (macro tests): - Over a simulated hour, a 60-s buff and a 1600-s buff each fire at their own rate (within jitter). - After a 15-s suspend and resume, each buff fires once within 5 s and no more. - A 3-s suspend fires nothing extra. - The intervals survive a restart (macro.ini), and the GUI shows one interval box per buff. Live: after a revive or relogin the buff icons are back within about 10 s.
   - As built: a buff's interval is 60 to 7200 s, not 10 s to 120 min -- anything under 60 s is a skill to the macro, and would leave the buff row.
7. **P1-7 Second potion tier on the simple screen. - Add HP Lv1/Lv2 and MP Lv1/Lv2 rows (key + %) to Đánh quái, mapped to the existing conditional macro keys (include/gobot/macro.hpp:48-59). - Lv2 sits below Lv1, with a tooltip.** — effort S.
   - Why: GoC has both tiers (data/guide/4.png; bot_s0_default.ini:133-141). The simple row drives only the first key of each kind (simple_view.cpp:750).
   - Done when: Offline: - The GUI shows four rows, and a changed Lv2 key or % round-trips through macro.ini. - Macro test: at 40% HP only Lv1 fires; at 25% Lv2 fires, with Lv1 on its own cooldown. Live (optional, during the P1-10 test): both thresholds drink.
8. **P1-8 Attack styles for the hunter: presets, keep distance, up/down reach, the right jump key. - (a) A 'Kiểu đánh' picker: class + weapon chooses one of GoC's four presets (sample_config.ini). The preset sets hunter max reach, min distance, up reach and down reach; the hunter uses separate up and down values instead of one height_tolerance (hunter.cpp:286-287). - (b) 'Giữ khoảng cách': if the target is closer than the min distance, step away before attacking, with a bounded number of steps in a row. - (c) A jump-key box next to 'Phím đánh', with a 'Thử' button and a checklist warning when the jump key equals the game's attack key. - (d) Make the facing tolerance configurable (hunter.cpp:295).** — effort M (live test).
   - Why: The player's GoC config uses backing_away=true and min 120 / max 200 (bot_s0_default.ini:8,18-19). GoBot's hunter has reach 150 and height 60, and the presets feed only the packet bot (gui_app.cpp:1775-1844). The hunter jumps with Space (hunt.ini:40), which GoC lists as the default normal-attack key (0x180788; bot_s0_default.ini:170).
   - Done when: Offline (hunter unit tests on scripted plates): - Target at dx=80 with min distance 120: one step away, then attack. - Target 100 px above with up-reach 131: reachable. Target 100 px below with down-reach 80: not reachable. - Each preset writes its six numbers to hunt.ini. - 'Thử' presses the jump key exactly once. Live, for each class the player plays (one melee, one ranged), 10 kills each: - The ranged character keeps at least the min distance in 8 of 10 fights. - The character visibly jumps on 'Thử' and when a target is above.
   - Depends on: P1-3
9. **P1-9 Monster list for the hunter (Loại mob / đánh trước). - Two lists on Đánh quái, 'Không đánh' and 'Đánh trước', filled by clicking names from a 'Quái đang thấy' list (detector_.plates()). - Loose matching (at least 80% of the letters), because a letter can misread. - The skip list also covers flying monsters; 'Đánh trước' stands in for boss priority.** — effort S (live test).
   - Why: GoC has 'Loại mob' (0x180040) and 'Ưu tiên đánh Boss' (0x17fee0). GoBot already reads plate names (mob_detector.hpp:60), but target choice ignores them (hunter.cpp:227-269).
   - Done when: Offline (hunter tests with plates A on the skip list and B on the priority list): - A is never targeted, even when it is nearest. - A reachable B is chosen over a nearer unlisted plate. - A one-letter misread of A is still skipped. Live: on a map with two monster kinds, put one on 'Không đánh'. Over 10 minutes it is never attacked (the tester counts).
   - Depends on: P1-3
10. **P1-10 Skills follow the fight. - When Tự đánh quái is on (walking or Đứng yên), skill keys (kind Skill) fire only while the hunter is 'Đang đánh quái', meaning a target in reach and faced. - With 'Dùng skill tuần tự' ticked they fire in list order, each with its own cooldown. - While MP% is under a set threshold, only the attack key is pressed. - A per-key 'luôn bấm theo giờ' tick keeps today's timer behaviour for self-buff skills. - Buffs and potions are unchanged.** — effort M (live test).
   - Why: GoC uses skills only inside its attack thread, in order, and skips them when mana is low (bot_s0_default.ini:29; GhostCore 0xe95c8, 0xe9170). GoBot's skill timers fire with nothing in reach (src/macro.cpp:434-480), wasting MP and potions, and the clock-perfect pattern looks automated.
   - Done when: Offline (scheduler tests): - No target for 60 simulated seconds: zero skill presses. - Target in reach: skills fire in order, none before its cooldown. - MP 10% with a 20% threshold: only the attack key. - 'luôn bấm' keys keep their timers. Live: - Two minutes with no monsters on screen: zero skill presses in gobot_gui.log. - Ten minutes of fighting: skills only while attacking. - The tester records MP potions used over 30 minutes, compared with a 30-minute timer run.
   - Depends on: P1-3, P1-8

### Phase 2

1. **P2-1 Alerts through the player's own Telegram bot, plus a local sound. - A 'Thông báo' tab: bot token and chat id. The token is stored with Windows DPAPI and never logged. A 'Gửi thử' button. - Events: - chết; hồi sinh không được (raised today through GuiApp::alert, which only logs); - mất kết nối / đã vào lại / cần người (from the keeper); - sắp hết bình (P2-3); - optional: tổ đội thay đổi, lời mật. - Messages are rate-limited, with a local beep or toast for the same events. - Talks only to api.telegram.org; nothing goes through a vendor.** — effort M (live test).
   - Why: GoC has notify_telegram_* (bot_s0_default.ini:33-37; 0x17fae0-0x17fb80), routed through its own server. GoBot has nothing (gui_app.cpp:1500). An overnight run needs someone to be told when it stops.
   - Done when: Offline: - With a mock sender, each event produces exactly one message with the right Vietnamese text. - Repeats within 60 s are merged. - The token appears neither in gobot*.log nor in clear text in configs. - No network call is made while the tab is off. Live: - 'Gửi thử' reaches the player's phone. - Ending Game.exe in Task Manager produces 'mất kết nối' and then 'đã vào lại'.
   - Depends on: P1-1
2. **P2-2 Use fury (Dùng nộ). - Read the third HUD bar under MP (frame.png) as the fury gauge, measured like the HP bar (BarSpec). - Press the fury key once when the gauge is full and the hunter is attacking. Add a fury-key box on Đánh quái. - Fallback: press it every N s while attacking if the gauge cannot be read.** — effort S (live test).
   - Why: The player's GoC config has auto_fury=true (bot_s0_default.ini:5; 0x1784c8; 'Activating Fury! SP=' 0xe96a8). GoBot never uses fury.
   - Done when: Offline: on frames cut live with the gauge empty, half and full, the reader returns about 0, 50 and 100%, and the key fires once per full gauge. Live: - The tester confirms the bar is the fury gauge: it fills while fighting and empties on the fury key. - Fury fires within 2 s of the gauge filling, five times in a row.
   - Depends on: P1-10
3. **P2-3 Potion stock warning. - Read the stack counts printed on the hotbar slots that hold the HP and MP potion keys, using the same 5x8 digits CountReader reads in bag slots (include/gobot/bag.hpp:166-217). The counts are visible in frame.png. - Show 'Còn N bình' on Đánh quái. - Warn once below a threshold (default 20) and notify through P2-1.** — effort S (live test).
   - Why: GoC buys potions on a timer and detects a full bag (0x1842b0, 0x184320). GoBot keeps pressing an empty slot, and the run ends in death.
   - Done when: Offline: on frame.png the two hotbar counts read as printed; a slot with no number reads as 1 or 'không đọc được'. Live: the counts match the hotbar over 10 minutes, and dropping below the threshold raises one warning.
   - Depends on: P1-3
4. **P2-4 Party: invite only when it can work (leader, not full). - Before inviting, read the party HUD (grabbed/party_indicator, party_leader_key; GoC Party_King_My.spr). - Skip the invite when the party has 6 members, or when 'Chỉ mời khi đang làm chủ party' (party_always_leader) is ticked and the bot is not the leader. Log the reason. - Optionally match names in the player list by drawing the wanted name in the game's font, as GoC does with data/font/WOD.ttf.** — effort M (live test).
   - Why: GoC skips both cases (0x1776c8, 0x177a80, 0x17d6f0, 0x17d740). GoBot ignores always_leader (party.hpp:46) and sends invites that fail.
   - Done when: Offline: tests on frames of alone, member, leader and a 6-member party. Inviting is allowed only when alone, or as leader with room. Live, with a second character: - With the option on and the bot a member, 'pt' does not trigger an invite. - As leader, it does. - With 6 members, there is no invite.
   - Depends on: P1-3
5. **P2-5 Party advert while alone (Tự chat khi không có party). - A per-line 'Chỉ khi chưa có tổ đội' tick in Tự chat (chat spec C-R16), using the party indicator from P2-4.** — effort S (live test).
   - Why: GoC has it (0x177c50-0x177d48; 0x180be8 is_party_request). The player already uses the magic-word party flow.
   - Done when: Offline: the chat gate postpones the line in a party and sends it when alone. Live: alone, the advert goes out at its interval; after joining a party it stops.
   - Depends on: P1-2, P2-4
6. **P2-6 Look for monsters when the screen is empty. - Option 'Đi tìm quái khi không thấy quái'. After N s (default 8) of 'Không thấy quái', walk one way in steps until one of these happens: - a monster appears; - the scenery stops scrolling (wall or map edge, CameraTracker); - T s pass. - Then walk the other way. - Time away from the start is capped, so it walks back the way it came. - Never jump down. If the minimap fingerprint changes (a warp), stop and walk back.** — effort M (live test).
   - Why: GoC's patrol keeps it fighting (0x1767a8-0x176a16; bot_s0_default.ini:162-166). GoBot's hunter stands still once the screen is clear (src/hunter.cpp:184-193).
   - Done when: Offline (hunter tests with scripted frames): - An empty view for 8 s starts walking. - A plate appearing stops the walk and is targeted. - Reaching the time cap turns it back. Live: - On a map where monsters respawn off screen, compare 20 minutes with the option on and 20 with it off: more kills with it on. - It never leaves the map (fingerprint unchanged); the tester watches the first 5 minutes.
   - Depends on: P1-3, P1-8
7. **P2-7 Reach monsters on other platforms. - Target above, within up-reach plus jump height: jump and attack in the air ('Đánh khi nhảy'). - An optional 'Nhảy đôi'. - Target below: down-jump (Down + jump), but not while standing on a warp gate.** — effort M (live test).
   - Why: GoC does all three (0x177e00, 0x177e28; GhostCore 0xe9060-0xe90d8, warp guard 0xe8fa0). GoBot only hops (src/hunter.cpp:346-349) and gives up after 4 s.
   - Done when: Offline (hunter tests): - Target 120 px above: jump, then the attack key within 300 ms. - Target below: down + jump. - With the options off, behaviour is unchanged. Live: on a two-level map, 10 minutes give more than zero kills on the other level, and the status line's 'given up on' count drops.
   - Depends on: P1-8
8. **P2-8 Close stray popups during a run. - While running with the HUD present, close a known popup that stays up longer than 3 s, with its close button or ESC: the daily check-in reward (grabbed/login_popup1), the Monster Dictionary, close_ic/close_1..8. - Never press OK or Xác Nhận, except on the check-in reward. - Only popups on that allow-list are touched; anything else, such as the bin's dialogs, a party invite or the revive question, is left alone. - Log what was closed.** — effort S (live test).
   - Why: GoC closes dialogs before its own jobs (CloseDialog, 0x181fb0-0x1820c8). GoBot's closer runs only from the CLI, and it also clicks confirm buttons (src/dialog_driver.cpp:144-197).
   - Done when: Offline: - Each popup blitted on a frame is closed with the right button. - With the bin's dialog, a party invite, hoisinh_confirm or any unknown window up, nothing is clicked. Live: open the Monster Dictionary, or let the check-in popup appear at login. It closes within 5 s and fighting resumes.
9. **P2-9 Watchdog for a game that is running but not playing. - The keeper also acts when the HUD has been missing for more than X min (default 5) while Game.exe runs and the screen is neither the death panel nor a loading screen that is still changing (no captcha detection, by the player's decision). - It reads any in-game OK popup (as GoC does with ok.png), then ends the client and logs back in, counted against max_restarts.** — effort M (live test).
   - Why: GoC restarts on an in-game OK box (0x189c40-0x189cb0). GoBot's CrashWatch only sees exits, hangs and Win32 boxes (crash_watch.hpp:39-44), while the HUD watcher only holds keys (gui_app.cpp:400-406). A disconnect drawn inside the game would hold the bot all night (inferred).
   - Done when: Offline (keeper decision tests on scripted states): - Death panel for 10 min: no restart. - Character select idle for 6 min: restart. - HUD back after 2 min: nothing. Live (player present): disable the network adapter for 2 minutes during a run. Within X+2 minutes the client restarts and the character is back; the restart is counted on the tab.
   - Depends on: P1-1
10. **P2-10 Hotkeys for 'clean the bag now' and 'log in now'. - Configurable, with no key by default. Acted on only while the game is in front, as GoC does (0x17c578). - F10 and F11 also ignore presses while another window is in front; F12 always works.** — effort S (live test).
   - Why: GoC has F11 for the item processor, F9 for shopping and F8 for login (bot_s0_default.ini:172-175; 0x17c5a8). GoBot can do neither on demand from inside the game.
   - Done when: Offline (key-handling tests): - With the game focused, the keys trigger request_trash and the keeper login. - With another window in front, nothing happens, except F12. Live: pressing the trash key in game starts a pass.
   - Depends on: P1-1
11. **P2-11 Use fragments (Sử dụng mảnh ghép). - In Dọn rác, a third action per item, 'Dùng': right-click the slot and confirm the use dialog only when its words match; otherwise press Hủy Bỏ. - It runs in the same bag pass, before deleting. - 'Thử tìm' lists 'sẽ dùng'.** — effort M (live test).
   - Why: The player had enable_use_fragments=true in GoC (item_processor.ini:6). GoBot can only delete fragments.
   - Done when: Offline: - A bag-pass test right-clicks the 'use' slot, recognises the use dialog by its words and sees the count drop. - An unknown dialog gets Hủy Bỏ. Live: one fragment stack is used and nothing else in the bag changes (the tester compares before and after). First grab a live picture of the use dialog.

### Phase 3

1. **P3-1 Research where the character stands, with no memory reading and no injected packets. Candidates: - (a) minimap scroll registered against the map picture (minimap_tracker.hpp:69-87: the marker stays centred and the picture moves); - (b) named plates matched to the passive 0x0038 monster positions; - (c) dead reckoning from GoBot's own moves, anchored at map loads (what GoC's server did, docs/protocol-findings.md §4b). Deliverable: 'gobot_cli --whereami --track <s>' printing x,y.** — effort L (live test).
   - Why: Working area, return to spot, waypoints, return to previous map, live map and safe-place shopping all need it. Memory reading is closed off by XIGNCODE3.
   - Done when: Offline: replaying a recorded session gives a continuous track. Live: a 10-minute walk on one map with 10 checkpoints, error at most 40 px at each, and a written go/no-go.
2. **P3-2 Working area, return to the spot, waypoints and a live map, built on P3-1. - Area box. - Return within 48/31 px after a knock-back in Đứng yên mode. - Waypoint lists per map with dwell, loop or ping-pong, and Sweep or KillAll %. - A live dot on the Map tab.** — effort L (live test).
   - Why: GoC has all of these (bot_s0_default.ini:20-23,30,46-48,162-166; 0x176530-0x177520).
   - Done when: Live: - 30 minutes with no target outside the box. - After a knock-back it is back within tolerance in 10 s. - A 4-point route runs for 20 minutes in each mode. - The map dot is within 40 px of the truth.
   - Depends on: P3-1
3. **P3-3 Return to the previous map after an unwanted warp. - Map names come from the minimap title (map_table.hpp); warp gates come from the .map files.** — effort L (live test).
   - Why: The player's GoC config has return_to_previous_map=true (bot_s0_default.ini:31; 0x17fd60).
   - Done when: Live: after a forced warp on 3 tested maps, it is back on the original map within 3 minutes. After 2 failures it gives up and alerts (P2-1).
   - Depends on: P3-1, P3-2, P2-1
4. **P3-4 Buy potions from Người giữ sân, following GoC's flow (0x1843c8-0x1847a8). 1. Close dialogs. 2. Open the bag's 'Khác' tab and use the Hawker item. 3. Open the Consumable tab and pick the item. 4. Buy, enter the quantity, confirm. 5. Stop when the bag is full (grabbed/nguoi_giu_san_full_hpmp). Triggered by a low count (P2-3) or a timer, with a 2-minute gap after a bag pass, a 'Mua thử' dry run and a spend cap per run.** — effort L (live test).
   - Why: A long run ends when the potions do. GoC has this (item_processor.ini:18-27).
   - Done when: Offline: grab live frames of each stage first; the routine dry run then prints every click, and the full-bag dialog stops it. Live (player present, small quantity): - It buys exactly N HP potions once. - A run with a full bag stops with a message. - Nothing else is bought.
   - Depends on: P2-3, P2-8
5. **P3-5 Pet care: heal, loot cycle, revive. - First research how pet HP and pet death look on the HUD (pet slot bars; grabbed/pet_hp_indicator). - Heal key when the pet's HP is under %. - Loot cycle every N s for M s, using the pet action key and the btn_pet_action_getitem / guard pictures. - Revive from the bag's Linh Thú tab with a chosen icon from data/images/pet/pet.** — effort L (live test).
   - Why: GoC has all three (bot_s0_default.ini:151-160; 0x180440-0x180588; RevivePetFeature 0x186be0-0x186e60).
   - Done when: Offline: pet bar reading works on cut frames, and the loot-cycle state machine passes its tests. Live: - The pet is healed below the threshold. - The loot cycle switches and restores Guard. - A dead pet is re-equipped within 30 s.
   - Depends on: P1-3
6. **P3-6 Avatar: auto-use and the reset window, with Change Character. - Research how the avatar's state shows on screen. - Press the avatar key when the avatar is off. - A Change Character routine: ESC, 'Chọn nhân vật' (go_select_char), then the same slot through the login character step. Used inside a GoC-style reset window (17-24 h, under 2 minutes left).** — effort M (live test).
   - Why: GoC has both (bot_s0_default.ini:42-45; 0x179cb0-0x179e90; 0x187fa0-0x1886a0).
   - Done when: Offline: the change-character dry run passes, and the window logic passes its tests. Live: - The avatar is re-activated when it ends. - One change-character cycle returns to the same character within 60 s and the run resumes.
   - Depends on: P1-1
7. **P3-7 Buff items from the bag, and dropping junk. - Buff items when the buff icon is missing, as in GoC's ItemBuffFeature. - Dropping junk, once the drop question can be told apart by its words.** — effort M (live test).
   - Why: GoC has both (item_processor.ini:7,37-38; 0x1859d0-0x186420; 0x178f18).
   - Done when: Offline: tests on a live-grabbed use dialog and drop question; an unknown dialog gets Hủy Bỏ. Live: one buff item is used when its icon is missing, and one junk stack is dropped. Nothing else changes.
   - Depends on: P2-11
8. **P3-8 Cash-shop HP/MP fill. Ask the player first whether they use cash-shop potions.** — effort M (live test).
   - Why: GoC has it (bot_s0_default.ini:142-148; FillCashShopHpFeature).
   - Done when: Live: the cash potion bar is refilled from the chosen item when it drops below the set %, and is checked every N s.
   - Depends on: P2-11
9. **P3-9 Profiles and several clients. - Named profiles per character covering macro, hunt, party, trash, login and chat, switchable on the simple screen. - A --profile flag so a second GoBot copy can drive another game window.** — effort M (live test).
   - Why: GoC has bot and party profiles (config.ini last_bot_config; 0x180d78-0x180dd8). GoBot keeps one set of files beside the exe.
   - Done when: Offline: switching profiles restores every key and list. Live: two GoBot copies drive two clients with different profiles for 30 minutes, with no cross-talk in the logs or configs.
   - Depends on: P1-1, P1-2
10. **P3-10 Party extras: priority list (reserved slots), automatic EXP/loot share via the party menu buttons, and a party member list on the tab.** — effort M (live test).
   - Why: GoC has them (party_s0_default.ini:14-17; 0x177b28-0x177bf0; 0x185468-0x185540). GoBot reads these settings but never acts on them (party.hpp:52-69).
   - Done when: Offline: rule tests for reserved slots. Live: - With 5 members, the loot share switches as configured and is restored afterwards. - The member list matches the game's.
   - Depends on: P2-4
11. **P3-11 Telegram remote commands: /trangthai, /dung, /chay, accepted only from the player's chat id.** — effort M (live test).
   - Why: GoC has remote control (0x17f8a0; DecodeCommandChatRemote / DecodeCommandPartyRemote).
   - Done when: Offline: a mock shows commands from another chat id are ignored and logged. Live: /dung stops the run within 5 s, and /trangthai reports state and HP.
   - Depends on: P2-1
12. **P3-12 EXP meter and level: read 'Level' on the HUD and the EXP bar, then show EXP %/h and time to the next level.** — effort M (live test).
   - Why: GoC has 'Đo EXP' (0x1763d8-0x1764bc).
   - Done when: Offline: level digits and bar fill read correctly on cut frames. Live: over 30 minutes, EXP %/h is within 10% of the tester's own reading.
   - Depends on: P1-3
13. **P3-13 Follow a named character: keep within N px of their plate and fight in range.** — effort M (live test).
   - Why: GoC lists 'Theo sau' as under development (0x178bf0-0x178c60). It is useful for a support character.
   - Done when: Live: with a second character walking a route, the bot stays within range for 10 minutes and fights what is in reach.
   - Depends on: P1-8, P2-7
14. **P3-14 Decide with the player whether to build quick quest.** — effort S.
   - Why: GoC has it (item_processor.ini:12-14; QuestFeature), but it needs a large picture library and packet-fed quest progress. It was dropped once before (gui_app.cpp:1511).
   - Done when: The decision is recorded. If yes, a separate BA spec is written.
15. **P3-15 Vietnamese for the full screen, and a light theme that follows Windows.** — effort M.
   - Why: GoC is bilingual and themed (0x179f10-0x179f18; 0x181d10).
   - Done when: Offline: every full-screen label has Vietnamese, and the theme follows Windows' setting.

## Not copied from GoC

- **Licence, subscription, payment, coupons, account registration, password reset, device/session kick, per-machine 'sub' slots**: GoC's commercial account system (config.ini [Server] and [General] last_selected_sub / sub_type_*; strings 0x175c98-0x175d30, 0x1791a8-0x179768, 0x189ea0-0x18a5e8; HTTPClient exports). It is vendor business, not a bot feature. GoBot runs locally with nothing to license.
- **Server-side brain: uploading captured game packets to a vendor server and receiving commands**: GoC sends filtered game traffic to ZTeam (PacketCapture::BatchThread, ProtocolHandler::EncodePacketBatch / DecodeCommandBatch; opcode allow-list delivered in the login response, 'DecodeLoginResponse: captureConfig' 0xea7a0; [Network] heartbeat and batch interval). That ships the player's game data to a third party and makes the bot depend on their server. GoBot decides locally from the screen and passive local packet reading.
- **Captcha solving**: The 'Window of Truth' is the game's check that a human is present. GoC answers it automatically with 36 character templates (CaptchaFeature; data/images/captcha/number; 'Solved' 0x182138), which defeats an anti-bot control. GoBot will not answer it.
- **Auto-accepting 'Security Alert' prompts**: GoC clicks '&Yes' on Windows certificate or security warnings from the game's launcher (0x187100-0x187128, 0x188e50). Accepting a security risk is the player's decision. The login rework stops and explains (in progress).
- **Fast key (concurrent key presses with a simulated human release delay)**: GoBot presses one key at a time through its coordinator on purpose: interleaved key-downs are not read cleanly (docs/goc-comparison.md). The 'simulated human release' part serves speed and evasion, not play.
- **Position fix after login (zig-zag)**: fix_location_after_login only calibrates GoC's server-side position estimate (0x17ffb0). GoBot has no such estimate. Revisit only if P3-1 picks dead reckoning.
- **Crowded-map monster-update reduction (Giảm tải cập nhật quái)**: It throttles GoC's own packet processing (0x17ff78). GoBot reads the screen, so there is nothing to throttle.
- **Reading or writing game memory, forging or injecting packets (for own position or anything else)**: Forbidden by the project's limits, and the game ships XIGNCODE3 (docs/overhaul.md). Position research (P3-1) is limited to the screen, passive packets and GoBot's own inputs.
- **Vendor auto-update (WinSparkle appcast)**: GoBot is built from its own repository (build.ps1, package.ps1). An updater that pulls binaries from an outside server is a supply-chain risk.
- **EULA screen, Discord/Telegram community links, GoC server reconnect overlay, latency and packet counters**: These cover GoC's vendor relationship and its link to its own server (0x17d8e8-0x17ed00, 0x17c720, 0x176168-0x1761b0). GoBot has no such server.

## Decisions by the player (2026-10-14)

- **Revive without revive items:** never fall back to reviving in the village (it costs 2% EXP). Leave
  the revive as it is and, when the revive items run out, stop the bot (with an alert once alerts exist).
- **Failed login:** try the crossing 3 times; if all three fail, stop and wait for the player (no timed
  retries after that).
- **Quick quest:** build it, reusing GoC's own approach and pictures as far as possible (BA first).
- **Captcha watch (P1-4):** not wanted -- removed from the plan entirely; GoBot will not add a screen captcha watch.
- **Alerts:** no Telegram bot. The player chose a **Discord webhook** first (one-way phone alerts through
  the Discord app), plus a local sound/Windows notification; a Zalo bot may follow later for two-way
  control, after a BA checks Zalo's Bot Platform. The webhook URL is stored with DPAPI and never logged.
