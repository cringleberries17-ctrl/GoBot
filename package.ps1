<#
.SYNOPSIS
    Build and zip a ready-to-run GoBot package.

.DESCRIPTION
    Produces dist\GoBot-<version>-win64.zip containing the two programs a
    player uses, the config files with everything that acts on the game
    switched off, the game's own pictures and glyphs, a Vietnamese note
    (DOC-TRUOC.txt), an English one (START-HERE.txt), VERSION.txt, and the
    licences of the libraries linked into the programs
    (THIRD-PARTY-NOTICES.txt).

    Nothing else is needed on the target machine: the CRT is linked statically,
    and wpcap.dll is delay-loaded so the keyboard macro runs with no Npcap
    installed at all.

    Run it with GoBot closed: a running copy holds its own .exe open, and this
    refuses rather than close it -- it may be in the middle of a run.

.PARAMETER Version
    Version string used in the archive name, VERSION.txt and START-HERE.txt.
    Defaults to today's date.

.PARAMETER SkipBuild
    Package whatever is already in build\bin\Release.
#>
[CmdletBinding()]
param(
    [string]$Version = (Get-Date -Format 'yyyy.MM.dd'),
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rel   = Join-Path $root 'build\bin\Release'
$dist  = Join-Path $root 'dist'
$stage = Join-Path $dist "GoBot-$Version-win64"

function Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Warn2{ param($m) Write-Host "    $m" -ForegroundColor Yellow }

# A value out of an ini file, by section and key, both in any case; $null when
# it is not there. Comments and blank lines are passed over, and a BOM in
# front of the first line is no part of it.
function Get-IniValue {
    param([string]$Path, [string]$Section, [string]$Key)
    $in = ''
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        $t = $line.Trim().TrimStart([char]0xFEFF)
        if ($t -eq '' -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
        if ($t -match '^\[(.+)\]') { $in = $Matches[1].Trim(); continue }
        if ($in -ine $Section) { continue }
        $eq = $t.IndexOf('=')
        if ($eq -lt 0) { continue }
        if ($t.Substring(0, $eq).Trim() -ieq $Key) { return $t.Substring($eq + 1).Trim() }
    }
    return $null
}

# Every key=value of one section, as pairs.
function Get-IniSection {
    param([string]$Path, [string]$Section)
    $in = ''
    $out = @()
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        $t = $line.Trim().TrimStart([char]0xFEFF)
        if ($t -eq '' -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
        if ($t -match '^\[(.+)\]') { $in = $Matches[1].Trim(); continue }
        if ($in -ine $Section) { continue }
        $eq = $t.IndexOf('=')
        if ($eq -lt 0) { continue }
        $out += ,@($t.Substring(0, $eq).Trim(), $t.Substring($eq + 1).Trim())
    }
    return ,$out
}

# Files searched for what must not be in them are read whole, as one string
# of Latin-1 characters, one per byte, so that a binary is searched too.
$latin1 = [System.Text.Encoding]::GetEncoding(28591)

# ---------------------------------------------------------------- build
if (-not $SkipBuild) {
    Step "Building Release"

    # A running instance holds its own exe open and the link fails, which is
    # easy to miss: the build reports an error but the stale binary stays put
    # and gets packaged as if it were current. It is not closed from here --
    # the GUI may be in the middle of a run, with the game in a dialog -- the
    # person running this closes it.
    $running = Get-Process gobot_gui, gobot_cli, gobot_sniff -ErrorAction SilentlyContinue
    if ($running) {
        throw "Close these first, then run this again (they hold their own .exe open, and are " +
              "not closed from here): " +
              (($running | ForEach-Object { "$($_.Name) [PID $($_.Id)]" }) -join ', ')
    }

    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        $bundled = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $bundled) { $cmake = $bundled } else { throw "cmake not found. Run .\bootstrap.ps1" }
    } else { $cmake = $cmake.Source }

    & $cmake --build (Join-Path $root 'build') --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
    Ok "built"
}

Step "Checking the build"
foreach ($exe in 'gobot_gui.exe','gobot_cli.exe','gobot_test.exe') {
    if (-not (Test-Path (Join-Path $rel $exe))) { throw "Missing $exe -- build first" }
}

# Self-tests gate the package. Shipping a build whose own tests fail is not a
# thing to do quietly.
#
# Run from a directory that is not the build output. A test that writes to the
# working directory passes when run from somewhere writable and fails for the
# person who unzips into Program Files, and a gate that only holds on the
# build machine is not a gate. The test program itself is not shipped: it
# checks the source tree too, which a tester does not have.
Push-Location ([System.IO.Path]::GetTempPath())
try {
    & (Join-Path $rel 'gobot_test.exe') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Self-tests failed -- not packaging" }
} finally { Pop-Location }
Ok "self-tests pass"

# ---------------------------------------------------------------- stage
Step "Staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'configs') | Out-Null

# The two programs a player runs. Not gobot_sniff (protocol tools), not
# gobot_test (it reads the source tree, which a tester does not have), and
# not the GoBotBridge files: those run another bot's code, which no feature
# of the simple screen uses and which a virus scanner rightly distrusts.
Copy-Item (Join-Path $rel 'gobot_gui.exe') $stage
Copy-Item (Join-Path $rel 'gobot_cli.exe') $stage

# Configs come from source, not from build output: the copies beside the
# build are this machine's player's own, with their keys, names and lists.
# Only the .ini files: configs\trash, the player's photographed items, stays.
Copy-Item (Join-Path $root 'configs\*.ini') (Join-Path $stage 'configs')
Copy-Item (Join-Path $root 'README.md')     $stage

# The Vietnamese note, byte for byte: UTF-8 with a BOM, and CRLF, so that
# Notepad shows its Vietnamese on any Windows.
Copy-Item -LiteralPath (Join-Path $root 'DOC-TRUOC.txt') -Destination $stage

# Which build this is, for the title, the simple screen and the log, and for
# whoever reads a report: the version, and the commit it was built from --
# with a + when the tree had changes not committed.
$commit = 'unknown'
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    $hash = (& $git.Source -C $root rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $hash) {
        $commit = "$hash".Trim()
        $dirty = (& $git.Source -C $root status --porcelain --untracked-files=no 2>$null)
        if ($dirty) {
            $commit += '+'
            Warn2 "the tree has changes that are not committed: VERSION.txt says commit $commit"
        }
    }
}
[System.IO.File]::WriteAllText((Join-Path $stage 'VERSION.txt'),
                               "$Version`r`ncommit $commit`r`n",
                               [System.Text.Encoding]::ASCII)

# ---- the things the executables READ ----
#
# The binaries are self-contained; what they read is not. Two sets of files
# live outside the build on the machine this was written on and are silently
# absent on anybody else's, and each takes a feature down with it:
#
#   templates\     every dialog is recognised by matching one of these
#   bitmap_font\   HP/MP figures, names, the chat and the player list are
#                  read with it
#
# Both are artwork extracted from the game client. They are included so the
# folder works on a machine that has the game and nothing else.

$tpl = Join-Path $rel 'templates'
if (Test-Path $tpl) {
    Copy-Item $tpl $stage -Recurse

    # grabbed\ holds two different things: pictures the bot needs, cut out of
    # live captures, and the screenshots taken while measuring. Only the first
    # kind belongs in a package -- the second shows this player's screen.
    #
    # Which is which is decided by configs\templates.ini rather than by a list
    # kept here, because a list kept here goes stale silently -- the first
    # version of this pruned everything except three files and quietly deleted
    # the eight auto-login pages that had just been put there, so the crossing
    # had no pictures at all on a machine without the other helper installed.
    $named = @()
    Get-Content (Join-Path $root 'configs\templates.ini') |
        Where-Object { $_ -match '=\s*grabbed/(\S+)\s*$' } |
        ForEach-Object { $named += ($Matches[1] + '.png') }

    $grabbed = Join-Path $stage 'templates\grabbed'
    if (Test-Path $grabbed) {
        Get-ChildItem $grabbed -File |
            Where-Object { $named -notcontains $_.Name } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

        $missing = $named | Where-Object { -not (Test-Path (Join-Path $grabbed $_)) }
        if ($missing) {
            Warn2 "named in templates.ini but not present, so those features will not work:"
            $missing | ForEach-Object { Warn2 "  $_" }
        }
        Ok "$((Get-ChildItem $grabbed -File).Count) cut-out pictures kept"
    }
    # Not for a package: seen\ is pictures of this player's screen, captcha\
    # is for a feature GoBot does not have, by the player's decision, and
    # quest\0\category is the English client's quest names, which GoBot does
    # not read.
    foreach ($drop in 'templates\seen', 'templates\captcha', 'templates\quest\0\category') {
        $p = Join-Path $stage $drop
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }

    # The glyphs: the full set of 183 that comes with the pictures, moved to
    # where the programs look first -- beside themselves. GoC's other set, in
    # its data\bitmap_font, has no F, J, W or w, and a name with one of those
    # in it was never found on a tester's machine.
    $glyphSrc = Join-Path $stage 'templates\bitmap_font'
    if (Test-Path $glyphSrc) {
        Move-Item -LiteralPath $glyphSrc -Destination (Join-Path $stage 'bitmap_font')
        Ok "bitmap font staged ($((Get-ChildItem (Join-Path $stage 'bitmap_font') -Recurse -File).Count) glyphs)"
    } else {
        Warn2 "no templates\bitmap_font in the build -- HP/MP figures, names, chat and the player list will be unreadable"
    }
    Ok "templates staged"
} else {
    Warn2 "no templates\ in the build -- nothing that recognises a dialog will work"
}

# Anything a run left behind is not part of the package.
Get-ChildItem $stage -Recurse -File |
    Where-Object { $_.Name -like '*.log' -or $_.Name -like 'frame.png' -or $_.Name -like 'region.png' -or
                   $_.Name -like 'playerlist*.png' -or $_.Name -like 'invite_not_taken.png' -or
                   $_.Name -like 'gobot_gui.ini' -or $_.Name -like 'last_capture.*' } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

# Text a player opens in Notepad: a BOM, and CRLF throughout. This script is
# LF, so a here-string written as it is came out LF, one long line in the
# Notepad of a Windows older than 10 1809.
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
function Write-NoteFile {
    param([string]$Path, [string]$Text)
    $crlf = ($Text -replace "`r`n", "`n") -replace "`n", "`r`n"
    if (-not $crlf.EndsWith("`r`n")) { $crlf += "`r`n" }
    [System.IO.File]::WriteAllText($Path, $crlf, $utf8Bom)
}

# The libraries linked into the programs, and their licences, which ask to
# travel with them: OpenCV's Apache 2.0 a copy of itself, Dear ImGui's MIT
# its notice, zlib's its notice, and Berkeley SoftFloat's BSD licence -- it
# is compiled into OpenCV's core, which the colour conversions use -- its
# notice in what goes with a binary. Read from the sources the build fetched,
# so they are the versions inside these programs.
$deps = Join-Path $root 'build\_deps'
function Get-DefineValue {
    param([string]$Path, [string]$Name)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -match ('^\s*#define\s+' + [regex]::Escape($Name) + '\s+"?([^"\s]+)"?')) { return $Matches[1] }
    }
    return ''
}
$cvVersionFile = Join-Path $deps 'opencv-src\modules\core\include\opencv2\core\version.hpp'
$cvVersion = (@('CV_VERSION_MAJOR', 'CV_VERSION_MINOR', 'CV_VERSION_REVISION') |
              ForEach-Object { Get-DefineValue $cvVersionFile $_ }) -join '.'
$zlibVersion = Get-DefineValue (Join-Path $deps 'opencv-src\3rdparty\zlib\zlib.h') 'ZLIB_VERSION'
$imguiVersion = Get-DefineValue (Join-Path $deps 'imgui-src\imgui.h') 'IMGUI_VERSION'
$softfloatLicence = 'opencv-src\modules\core\3rdparty\SoftFloat\COPYING.txt'
$softfloatVersion = ''
$softfloatFile = Join-Path $deps $softfloatLicence
if (Test-Path -LiteralPath $softfloatFile) {
    $hit = Select-String -LiteralPath $softfloatFile -Pattern 'SoftFloat Release (\S+)' -List
    if ($hit) { $softfloatVersion = $hit.Matches[0].Groups[1].Value }
}
$licenceTexts = @(
    @("OpenCV $cvVersion -- LICENSE", 'opencv-src\LICENSE'),
    @("OpenCV $cvVersion -- COPYRIGHT", 'opencv-src\COPYRIGHT'),
    @("zlib $zlibVersion -- LICENSE", 'opencv-src\3rdparty\zlib\LICENSE'),
    @("Berkeley SoftFloat $softfloatVersion -- COPYING.txt", $softfloatLicence),
    @("Dear ImGui $imguiVersion -- LICENSE.txt", 'imgui-src\LICENSE.txt')
)
$noticeRows = @(
    @("OpenCV $cvVersion, its core and imgproc modules", 'Apache License 2.0'),
    @("zlib $zlibVersion, built as part of OpenCV", 'zlib License'),
    @("Berkeley SoftFloat $softfloatVersion, in OpenCV's core", 'BSD 3-Clause License'),
    @("Dear ImGui $imguiVersion, in gobot_gui.exe", 'MIT License')
)
$noticeTable = ($noticeRows | ForEach-Object { '  {0,-46} {1}' -f $_[0], $_[1] }) -join "`n"
$notices = @"
GoBot $Version -- third-party notices

gobot_gui.exe and gobot_cli.exe are built with these libraries, linked into
them. Their licences follow, as their authors ask.

$noticeTable

  Dear ImGui comes with stb_rect_pack, stb_textedit and stb_truetype by Sean
  Barrett (public domain, or MIT), and the ProggyClean and ProggyForever
  fonts by Tristan Grimmer and Disco Hello (MIT License).

  OpenCV's core also carries code from FDLIBM, whose notices ask to be kept:

    Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
    Developed at SunSoft, a Sun Microsystems, Inc. business.
    Permission to use, copy, modify, and distribute this
    software is freely granted, provided that this notice
    is preserved.

    Copyright (C) 2004 by Sun Microsystems, Inc. All rights reserved.
    Permission to use, copy, modify, and distribute this
    software is freely granted, provided that this notice
    is preserved.

"@
$noticesMissing = @()
foreach ($l in $licenceTexts) {
    $src = Join-Path $deps $l[1]
    if (-not (Test-Path -LiteralPath $src)) { $noticesMissing += "build\_deps\$($l[1])"; continue }
    $notices += "`n==== $($l[0]) ====`n`n" + ([System.IO.File]::ReadAllText($src)).TrimEnd() + "`n"
}
Write-NoteFile (Join-Path $stage 'THIRD-PARTY-NOTICES.txt') $notices

$startHere = @"
GoBot $Version

Vietnamese guide: DOC-TRUOC.txt -- read it first. This is the short English
version for whoever looks after the testers.

WHAT IT IS
  GoBot plays Ghost Online (the VTC client) by looking at the screen and
  pressing keys and clicking as a player would: fighting, skills, potions,
  buffs, reviving with a revive item, logging back in, party invitations,
  cleaning the bag, timed chat lines, quick quest, and alerts to Discord.
  It reads no game memory, sends no packets of its own and asks for no
  account or password. Nothing is sent anywhere except the alerts, to the
  player's own Discord channel, when they switch them on.

STARTING
  1. Unzip everything into a new folder, preferably one whose path has no
     Vietnamese letters, such as C:\GoBot. Not inside the zip.
  2. Run gobot_gui.exe. It asks for administrator, and it has to: the game
     runs elevated, and Windows drops the keys of a program that is not.
     SmartScreen may say "Windows protected your PC" -- More info, Run
     anyway: GoBot is not signed.
  3. The game in a window, 1024x768 or 1280x720, with Windows' display
     scale at 100%. Keep every window, GoBot's included, off the game: it
     is read off the screen. On a screen 768 pixels high a 1024x768 game
     does not fit: use 1280x720, with the taskbar set to hide itself.
     Where GoBot cannot sit beside the game, press BAT DAU and minimise
     GoBot: it goes on running, and F10, F11 and F12 still work in the game.

THE SIMPLE SCREEN
  Seven tabs: Danh quai, To doi, Tu chat, Tu dang nhap, Don rac, Nhiem vu,
  Thong bao. Everything that acts on the game ships switched OFF: tick what
  you want first. Settings save themselves.

  Hunting needs the character's name typed on Danh quai: the hunter finds
  the character by that name, whole -- a player whose name has it inside,
  "TuanAnh" for "Anh", is not taken for the character -- and never by a
  plate's bars, which any player may wear. It does not start without it.

  BAT DAU or F10   start / stop everything ticked, on every tab
  F11              hunting on and off
  F12              stop everything at once and let go of every key

  F10 and F11 act only while the game or GoBot is the window in front; F12
  acts from anywhere.

  The version is in the window's title and at the bottom left of the simple
  screen. "Nang cao" is the old full screen, in English; testers do not need
  it. Two optional keys ship with no key: Phim don ngay (Don rac tab: one
  bag pass now) and Phim vao game ngay (Tu dang nhap tab: log in now, when
  the character is not in the game). F1 to F12 only; they act only while
  the game is the window in front, and the key reaches the game too, so
  pick an F key the game does not use.

EXPERIMENTAL
  Marked "Thu nghiem" on the screen -- passed every test here, not yet
  proven on the real game: revive with an item (Tu hoi sinh), per-key buff
  timers (Tu buff), inviting whoever types the keyword (at 1280x720) and,
  with it, inviting only while leading (Chi moi khi dang lam chu to doi),
  logging in without an account (Tu dang nhap) with its new options (Cung
  vao lai khi game van chay ma khong thay nhan vat qua N phut, and Phim vao
  game ngay), Tu chat with its tick Chi khi chua co to doi, quick quest
  (Nhiem vu), the Discord alerts (Thong bao) with the new Sap het binh and
  Het binh, and on Danh quai: the second potion tier (Lv2), potion counts
  (Con N binh), monster lists (Loc quai), attack styles (Kieu danh),
  keeping distance (Giu khoang cach), the jump key (Phim nhay and Thu),
  skills only while fighting (Chi khi dang danh quai) and closing notices
  (Tu dong thong bao); on Don rac, registering monster fragments (Dung manh
  ghep) and the key Phim don ngay. Try one at a time, on a spare character,
  and watch. Bag cleaning (Don rac) has run on the real game; registering
  fragments has not. Only one live run, of about 15 minutes, has tried
  any of the rest: hunting with the macro keys, potions with Con N binh,
  Tu chat, the keyword invite (it read the chat; no invite went out) and
  one death with Tu hoi sinh. See KNOWN ISSUES.

  Two ticks among those are worth knowing. Chi danh nhung con trong danh
  sach Danh truoc (under Sua danh sach... on the Loc quai line, off as
  shipped) attacks only the monsters on that list, so nothing at all
  while it is empty. Dung skill tuan tu (in the Ky nang window, opened by
  Chi tiet... on the Tu dung ky nang line; ticked as shipped, as in GoC)
  presses every skill whose cooldown is over, one after the other, rather
  than one at a time; it counts only with Chi khi dang danh quai ticked.

  The party, by the player's rule: alone means the game shows no party.
  Chi moi khi dang lam chu to doi (To doi tab, off as shipped) invites only
  when the game shows the player's own crown, or no party at all -- no
  member's panel, no crown, the bag not over where the panels go -- and
  the tick Chi khi chua co to doi on Tu chat lines goes by the same
  reading. A game window over the party panels can make GoBot read alone
  while the player is in a party, so close such windows when it matters;
  so can the moment just after joining one, before the game draws the
  panels. A look taken before GoBot's own last accept, decline or invite
  is never used. Thu xem on To doi says what it reads: To doi: mot minh,
  thanh vien, chu to doi. GoBot never takes the player's own Tu chat lines
  for somebody asking to be invited, however short: a line said by the
  character name typed on Danh quai, or a line of just the keyword that
  GoBot itself typed, for as long as that line stays on the chat (GoBot
  remembers its speaker as read in the first minute after typing it).
  GoBot remembers what it typed in the last 30 minutes, lines typed
  before GoBot was started again (a new version in a new folder too) and
  gobot_cli's lines included, in %LOCALAPPDATA%\GoBot\said_lately.txt.
  Somebody else's bare keyword in that first minute is passed over too
  (with no name typed on Danh quai, until their line has been off the
  chat for a minute); with more words ("cho pt") it counts. If GoBot
  does not see its keyword line on the chat in the first minute after
  typing it -- the game dropped the Enter that sends it, the chat could
  not be read, the party part was started after that minute, or GoBot
  was started again -- every bare keyword, anybody's, is passed over
  until GoBot types the keyword again and sees that line, 30 minutes at
  most after the last time it typed it; with a name typed on Danh quai
  or not. A speaker's name is compared with the player's (the one typed
  on Danh quai, or the speaker GoBot remembered) by plain letters a-z
  and digits alone, ignoring case: a letter with a Vietnamese mark (d
  with a stroke too), any other letter, and a leading l, I or L are left
  out, and do not count towards the length either. With 5 or more left,
  a name 80% alike and at most one letter longer or shorter counts as
  the player's too; with 2 to 4, only the same name does; with fewer
  than 2 (such as Duc, Loc or Ly with their Vietnamese marks), the name
  is not compared at all, as if none were typed.

  Potion counts (Con N binh): Sap het binh needs two reads in a row under
  the mark, about 10 seconds apart. A count that falls to a fifth or less
  at once (62 to 6) is not believed until it is read 3 times (reads with
  the hotbar covered do not count); one that comes back is logged as a
  misread, and its screenshot is kept in potion_debug.

  There is no captcha handling, by decision: the player answers one.

KNOWN ISSUES
  Entries 2, 3 and 5 were found in that live run (hunting with the macro
  keys, potions and skills, a timed chat line, the keyword invite on);
  entries 1 (potion counts misread) and 4 (a Tu chat line of just the
  keyword taken for somebody asking) of that run are fixed in this
  package. Later packages fix the rest. Testers need not report these
  symptoms again, but should follow entries 2, 3 and 5, and report with
  the log, when those say so.

  2. A death with the HP potion on (Tu uong binh HP, khi HP duoi 60%). The
     potion count did not change in the 36 seconds before it and the log
     then had no HP readings, so the cause is not known: the potion may not
     have been pressed, or one or two big hits may have taken HP from above
     60% to zero. GoBot's revive clicked the revive-item button at once.
     Now, right after ALERT death, the log has a few Black box lines: HP
     and MP as read over the last 30 seconds (no read when the bars could
     not be read, and then the potions wait), each potion key's presses and
     why it held otherwise (above the mark, cooling, keys held...), what
     the potion slots last showed and when the bars were last read; and
     once a minute a Potions: line.
     Ask for gobot_gui.log and gobot_gui.old.log at once after any death
     with the HP potion on, with the time. When trying potions, watch the
     HP bar: Binh HP: N in the line under BAT DAU counts the presses of the
     HP potion key, and Doc duoc HP va MP on Danh quai must be green.
     Potions, like every key, go only while the game is the window in
     front; Tu chat holds them while it types (entry 3).

  3. Tu chat. 4 of 8 timed lines were not sent: either the chat box did not
     open after Enter twice (log: Khong mo duoc o chat (da bam Enter 2 lan))
     or it opened and the game did not take the typed text (log: Game khong
     nhan chu go vao o chat (cach go vk) -- cau chua gui.). The chat_debug
     screenshots show GoBot read the bar right -- shut, or open and empty:
     the game did not take the keys. Each line tried now leaves a Chat
     trace line in the log: how long each Enter was held, what the bar
     showed after it, and, when nothing showed, whether Unikey, EVKey or
     OpenKey runs and the keyboard layout. Before Game khong nhan chu GoBot
     now looks at the bar three times, up to about 0.65 seconds after the
     last key. A failed line is tried again after 30 seconds; 3 in a row
     stop Tu chat. A line holds fighting, skills, potions and buffs (5.5
     seconds in the run); HP falling under the HP potion's mark ends it at
     once, in any of its waits too, and the potion goes. configs\chat.ini
     has new settings for the live test of key delivery (hold_min_ms,
     max_enters, inject...), each at what GoBot has always done: change
     them only when asked. Advice:
     Unikey/EVKey on English (E), the Windows keyboard on English (US), the
     chat box uncovered, hands off while a line is typed; ask for chat_debug
     and the Chat lines.

  5. Hunting. About 110 fights in 11 minutes, about 1 in 7 given up, no
     back-steps. With Kieu danh "Nhu truoc" (the shipped setting) a monster
     still alive 4 seconds after it was picked is given up and one is
     picked again (usually the same); the log counts them once a minute
     (Hunter: N fights, ... G given up). A chosen Kieu danh gives up only
     after 4 seconds out of reach or 15 seconds without the monster's bar
     going down. Back-steps happen only with Giu khoang cach ticked, which
     ships off. Nothing to do unless the character stands still or walks
     away from the monsters: then F12 and report with the time.

REPORTING A PROBLEM
  Send gobot_gui.log, beside gobot_gui.exe, with the version, the game's
  window size, and what happened when. It holds the last time GoBot was
  opened; the time before is gobot_gui.old.log -- send that too when GoBot
  was closed and opened again after the problem -- and older ones are gone.
  The logs can hold the character name that was typed, the names of other
  players GoBot invited to a party (and of those who joined, with "Kem ten
  nguoi choi khac" ticked on Thong bao) and of players who typed the party
  keyword, invited or not, folder paths with the Windows user name, a
  home network address and the chat lines that were said -- never a
  password or the webhook. The *_debug folders (chat_debug, trash_debug,
  revive_debug, quest_debug, potion_debug, keeper_debug) are screenshots
  of the game: only on request.

UPDATING
  Unzip a new version into a NEW folder. Copy party.ini (and trash.ini with
  the trash folder) from the old configs folder if wanted -- but a party.ini
  from a package before 15/10/2026 has both party switches on, and any
  copied party.ini keeps what was ticked: open To doi and look at both
  switches before BAT DAU. Do not copy hunt.ini: one from a package before
  15/10/2026 may hold another player's character name. Type the name and the
  attack key again on Danh quai; if hunt.ini was copied anyway, check that
  "Ten nhan vat" there shows the tester's own name. A hunt.ini copied anyway
  keeps its own jump keys, often Space, the game's attack: Danh quai warns
  ("Phim nhay dang la Space", "Phim nhay khi di toi diem da luu la Space:
  se danh chu khong nhay"); GoBot never changes the file. The walker's
  jump key (the Radar tab of the full screen, used only when walking to
  saved points) now ships as the Up arrow; a hunt.ini without it gets Up
  too; one copied from an older package keeps its own. Set the other keys
  again on the screen rather than copying an older macro.ini, which may
  have a hidden key sequence switched on.

THE DISCORD WEBHOOK
  Kept encrypted for the Windows user in %LOCALAPPDATA%\GoBot, never in this
  folder; "Xoa webhook" on the Thong bao tab deletes it.

WHAT IS IN HERE
  gobot_gui.exe     the app
  gobot_cli.exe     command-line tools, such as gobot_cli --doctor; and
                    look-only checks that press nothing: --potions (the
                    potion counts it reads on the hotbar), --party-hud (what
                    it reads of the party), --popups (which notices it
                    would close)
  configs\          the settings, written by the app as they change
  templates\        the game's own dialogs and buttons, used to recognise them
  bitmap_font\      the game's own glyphs, used to read numbers and names
  DOC-TRUOC.txt     the guide, in Vietnamese
  VERSION.txt       which version and commit this is
  README.md         the project's notes
  THIRD-PARTY-NOTICES.txt  the licences of the libraries inside the programs

  templates\ and bitmap_font\ are artwork extracted from the game client,
  included so this folder works on a machine that has only the game.

NOTHING ELSE TO INSTALL
  No VC++ redistributable: the runtime is linked in.
  No Npcap: that only matters to the unfinished packet-capture bot, which
  testers do not use.

  Botting is against GhostOnline's terms of service. Your account, your call.
"@
Write-NoteFile (Join-Path $stage 'START-HERE.txt') $startHere

Ok "staged to $stage"

# ---------------------------------------------------------------- prove it
Step "Checking the package can stand on its own, and carries nothing of anybody's"

# Everything below is a thing that was claimed to be true and was not. The
# claims are now checked, and a package that fails one is not produced.
$problems = @()
$textFiles = @(Get-ChildItem $stage -Recurse -File |
               Where-Object { $_.Extension -in '.ini', '.txt', '.md' })

# 1. No executable may need a DLL that is not in here.
#
# wpcap.dll comes with Npcap, which is a separate installation. A STATIC
# import means Windows refuses to start the process when it is missing, so
# gobot_cli.exe would not launch on a machine without Npcap -- and that is the
# program somebody runs first, to see what is wrong with --doctor. It is
# delay-loaded now; this is what stops that silently coming back.
$dumpbin = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" `
            -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -like '*Hostx64\x64*' } | Select-Object -First 1
if ($dumpbin) {
    foreach ($exe in Get-ChildItem $stage -Filter *.exe) {
        $o = & $dumpbin.FullName /imports $exe.FullName 2>&1
        $mode = 'static'
        foreach ($l in $o) {
            if ($l -match 'delay load imports') { $mode = 'delay' }
            if ($mode -eq 'static' -and $l -match '^\s{4}(wpcap|Packet)\.dll') {
                $problems += "$($exe.Name) statically imports $($Matches[1]).dll -- it will not start without Npcap"
            }
        }
    }
    Ok "no executable needs anything that is not in the package"
} else {
    Warn2 "dumpbin not found -- could not check what the executables need"
}

# 2. The glyphs, without which names, figures and chat go unread -- the full
# set: a name with an F, J, W or w in it was unreadable with the short one.
$glyphs = Join-Path $stage 'bitmap_font'
if (-not (Test-Path $glyphs)) {
    $problems += "no bitmap_font\ -- HP/MP figures, names, chat and the player list cannot be read"
} else {
    $n = (Get-ChildItem $glyphs -Recurse -File).Count
    $lacking = @('upper\F.png', 'upper\J.png', 'upper\W.png', 'lower\w.png') |
               Where-Object { -not (Test-Path -LiteralPath (Join-Path $glyphs $_)) }
    if ($n -lt 183) { $problems += "bitmap_font\ has only $n glyphs, not the full 183" }
    elseif ($lacking) { $problems += "bitmap_font\ lacks $($lacking -join ', ')" }
    else { Ok "$n glyphs, F, J, W and w among them" }
}

# 3. Every picture named in templates.ini as a cut-out has to be there, and
# nothing else may be: the rest are screenshots of this player's game.
$tplIni = Join-Path $stage 'configs\templates.ini'
$grabbed = Join-Path $stage 'templates\grabbed'
$namedHere = @()
Get-Content $tplIni | Where-Object { $_ -match '=\s*grabbed/(\S+)\s*$' } | ForEach-Object {
    $png = $Matches[1] + '.png'
    $namedHere += $png
    if (-not (Test-Path (Join-Path $grabbed $png))) {
        $problems += "templates.ini names grabbed/$($Matches[1]) and it is not in the package"
    }
}
if (Test-Path $grabbed) {
    Get-ChildItem $grabbed -File | Where-Object { $namedHere -notcontains $_.Name } | ForEach-Object {
        $problems += "templates\grabbed\$($_.Name) is not named in templates.ini -- a screenshot of this machine's game"
    }
}

# 3b. No cut-out carries what was on screen under it: no picture of
# templates\grabbed -- cut out of live captures -- whose fully transparent
# pixels (alpha 0) hold any colour. The bot never reads them (every picture is
# matched with its alpha as a mask), but a cut of a player's screen would carry
# the items in its slots, or the scenery round it, wherever it is copied. The
# rest of templates\ is the game's own art, extracted as it is. GDI+ keeps that
# colour both ways, so System.Drawing sees it; counted in C#, as a PowerShell
# loop over a million pixels is slow.
Add-Type -AssemblyName System.Drawing
if (-not ('UnderTransparent' -as [type])) {
    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class UnderTransparent {
    // Pixels that are fully transparent (alpha 0) and still hold a colour (any of R, G, B not 0).
    public static int Count(string path) {
        using (Bitmap bmp = new Bitmap(path)) {
            Rectangle all = new Rectangle(0, 0, bmp.Width, bmp.Height);
            BitmapData data = bmp.LockBits(all, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                int stride = Math.Abs(data.Stride);
                byte[] buf = new byte[stride * bmp.Height];
                Marshal.Copy(data.Scan0, buf, 0, buf.Length);
                int n = 0;
                for (int y = 0; y < bmp.Height; y++) {
                    int row = y * stride;
                    for (int x = 0; x < bmp.Width; x++) {
                        int i = row + x * 4;                    // B, G, R, A
                        if (buf[i + 3] == 0 && (buf[i] | buf[i + 1] | buf[i + 2]) != 0) n++;
                    }
                }
                return n;
            } finally { bmp.UnlockBits(data); }
        }
    }
}
'@
}
# The check has to be able to see it: one pixel with colour under alpha 0 is
# counted, one without is not.
$selfPng = Join-Path ([System.IO.Path]::GetTempPath()) 'gobot_alpha0_selftest.png'
$selfBmp = New-Object System.Drawing.Bitmap(2, 2, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$selfBmp.SetPixel(0, 0, [System.Drawing.Color]::FromArgb(0, 10, 20, 30))
$selfBmp.SetPixel(1, 1, [System.Drawing.Color]::FromArgb(0, 0, 0, 0))
$selfBmp.Save($selfPng, [System.Drawing.Imaging.ImageFormat]::Png)
$selfBmp.Dispose()
$selfSeen = [UnderTransparent]::Count($selfPng)
Remove-Item -LiteralPath $selfPng -Force
if ($selfSeen -ne 1) {
    $problems += "the check for colour under transparent pixels cannot see it (counted $selfSeen of 1)"
} else {
    # Allowed to hold colour under alpha 0, each with its reason: none. (The
    # monster book's X and the fragment registration question held the game's
    # parchment there until round 8; both are matched with alpha as a mask,
    # and were blanked.)
    $transparentOk = @()
    if (Test-Path $grabbed) {
        foreach ($png in (Get-ChildItem $grabbed -File -Filter *.png)) {
            if ($transparentOk -contains $png.Name) { continue }
            $n = [UnderTransparent]::Count($png.FullName)
            if ($n -gt 0) {
                $problems += "templates\grabbed\$($png.Name) holds colour under $n of its transparent pixels: a cut-out can carry what was on screen under it"
            }
        }
    }
    if (-not ($problems | Where-Object { $_ -like '*transparent pixels*' })) {
        Ok "no cut-out holds colour under its transparent pixels"
    }
}

# 4. The crossing's pages, by name, resolved out of the package alone.
$loginIni = Join-Path $stage 'configs\login.ini'
$pages = Get-Content $loginIni | Where-Object { $_ -match '^\s*(?:signin_)?picture\s*=\s*(\S+)' } |
         ForEach-Object { $Matches[1] } | Sort-Object -Unique
foreach ($p in $pages) {
    $named = Get-Content $tplIni | Where-Object { $_ -match "^\s*$([regex]::Escape($p))\s*=" }
    if (-not $named) {
        $problems += "configs\login.ini wants the picture '$p' and configs\templates.ini does not say where it is"
    }
}
if (-not $problems) { Ok "every auto-login page has a picture in the package" }

# 4b. Nothing belonging to another program may be in here, nor the programs
# testers are not given.
#
# Shipping TungHaVGOBot.dll is what got the zip flagged as malware. Its
# pictures are cut out as PNGs instead, so if that file ever reappears here it
# is a mistake.
foreach ($stranger in 'TungHaVGOBot.dll','GhostCore.dll','go_client.exe','GoBotBridge.dll',
                      'GoBotBridge.runtimeconfig.json','gobot_test.exe','gobot_sniff.exe') {
    if (Test-Path (Join-Path $stage $stranger)) {
        $problems += "$stranger is in the package -- it is not for testers"
    }
}

# 5. Nothing may point at a path on the machine this was built on: no folder
# under C:\Users, in either slash, and not this Windows user's name as a word.
$userPath = '(?i)[a-z]:[\\/]users[\\/]'
$me = $env:USERNAME
foreach ($f in $textFiles) {
    $relName = $f.FullName.Substring($stage.Length + 1)
    foreach ($h in (Select-String -LiteralPath $f.FullName -Pattern $userPath)) {
        $problems += "${relName}:$($h.LineNumber) points at a folder of this machine's: $($h.Line.Trim())"
    }
    if ($me -and $me.Length -ge 3) {
        $hit = Select-String -LiteralPath $f.FullName -Pattern ('(?i)\b' + [regex]::Escape($me) + '\b') -List
        if ($hit) { $problems += "${relName}:$($hit.LineNumber) has this Windows user's name in it" }
    }
}

# 6. No Discord webhook may travel with the package.
#
# The alerts keep the player's webhook encrypted outside the program folder,
# in %LOCALAPPDATA%\GoBot, so nothing staged here should ever hold one. A file
# that does -- a config it was pasted into, a stray copy of the encrypted
# file -- would let whoever unzips this post in that player's channel. The
# address itself is not printed: the build log is no place for it either.
$hookPattern = '(discord|discordapp)\.com/api/(v\d+/)?webhooks/\d'
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    $relName = $_.FullName.Substring($stage.Length + 1)
    if ($_.Name -like 'alerts_webhook.bin*') {
        $problems += "$relName is a saved Discord webhook -- it must never ship"
    } elseif ($_.Extension -ine '.png') {
        $hit = Select-String -LiteralPath $_.FullName -Pattern $hookPattern -List
        if ($hit) { $problems += "${relName}:$($hit.LineNumber) holds a Discord webhook address -- it must never ship" }
    }
}

# 7. Everything that acts on the game ships switched off. A tester presses
# BAT DAU before reading a word, and what is on in a file here is a key
# pressed, an invitation taken or a line said in their game.
$cfg = Join-Path $stage 'configs'
$mustBeOff = @(
    @('macro.ini', 'Macro', 'sequence_enabled'),
    @('hunt.ini', 'Hunt', 'enabled'),
    @('hunt.ini', 'Hunt', 'loot'),
    @('party.ini', 'Party', 'party_auto_accept'),
    @('party.ini', 'Party', 'party_auto_party'),
    @('app.ini', 'Simple', 'keep_alive'),
    @('app.ini', 'Simple', 'revive'),
    @('app.ini', 'Capture', 'autostart'),
    @('trash.ini', 'Trash', 'enabled'),
    @('chat.ini', 'Chat', 'enabled'),
    @('quest.ini', 'Quest', 'enabled'),
    @('alerts.ini', 'Alerts', 'enabled'),
    @('app.ini', 'Crash', 'stall_enabled'),
    @('app.ini', 'Simple', 'close_popups'),
    @('trash.ini', 'Trash', 'use_fragments'),
    @('macro.ini', 'Macro', 'skills_follow_fight'),
    @('hunt.ini', 'Hunt', 'keep_distance')
)
foreach ($m in $mustBeOff) {
    $v = Get-IniValue (Join-Path $cfg $m[0]) $m[1] $m[2]
    if ($null -eq $v) { $problems += "$($m[0]): [$($m[1])] $($m[2]) is not there to be off" }
    elseif ($v -notmatch '^(false|0|no|off)$') { $problems += "$($m[0]): [$($m[1])] $($m[2])=$v -- it must ship off" }
}
foreach ($pair in (Get-IniSection (Join-Path $cfg 'macro.ini') 'Keys')) {
    if ($pair[0] -match '^key_\d+_enabled$' -and $pair[1] -notmatch '^(false|0|no|off)$') {
        $problems += "macro.ini: $($pair[0])=$($pair[1]) -- every key must ship off"
    }
}
# Keys that press something whenever they are not 0: a hotkey a tester would
# hit by chance, and the hunter's hop. (hunt.ini's [Move] jump_key, the
# walker's, ships as Up by decision: only [Hunt]'s is checked.)
$mustBeZero = @(
    @('app.ini', 'Hotkeys', 'trash_now'),
    @('app.ini', 'Hotkeys', 'login_now'),
    @('hunt.ini', 'Hunt', 'jump_key')
)
foreach ($m in $mustBeZero) {
    $v = Get-IniValue (Join-Path $cfg $m[0]) $m[1] $m[2]
    if ($null -eq $v) { $problems += "$($m[0]): [$($m[1])] $($m[2]) is not there to be 0" }
    elseif ($v -ne '0') { $problems += "$($m[0]): [$($m[1])] $($m[2])=$v -- it must ship as 0 (no key)" }
}
if (-not ($problems | Where-Object { $_ -like '*ship off*' -or $_ -like '*to be off*' -or
                                     $_ -like '*ship as 0*' -or $_ -like '*to be 0*' })) {
    Ok "everything that acts on the game is off"
}

# 8. Nothing of any one player's in the configs: no character, no names, no
# chat lines, no label, no items, no account.
$personal = @(
    @('hunt.ini', 'Hunt', 'character_name'),
    @('party.ini', 'Party', 'party_whitelist'),
    @('party.ini', 'Party', 'party_blacklist'),
    @('party.ini', 'Party', 'party_accept_only'),
    @('party.ini', 'Party', 'party_priority_list'),
    @('alerts.ini', 'Alerts', 'label')
)
foreach ($m in $personal) {
    $v = Get-IniValue (Join-Path $cfg $m[0]) $m[1] $m[2]
    if ($v) { $problems += "$($m[0]): [$($m[1])] $($m[2]) is not empty -- it is somebody's own" }
}
foreach ($pair in (Get-IniSection (Join-Path $cfg 'chat.ini') 'Lines')) {
    if ($pair[0] -match '^line\d+$' -and $pair[1]) { $problems += "chat.ini: $($pair[0]) has a line in it -- somebody's own" }
}
foreach ($pair in (Get-IniSection (Join-Path $cfg 'trash.ini') 'Items')) {
    if ($pair[1]) { $problems += "trash.ini: [Items] $($pair[0]) lists items -- somebody's own" }
}
foreach ($f in Get-ChildItem $cfg -Filter *.ini) {
    foreach ($h in (Select-String -LiteralPath $f.FullName -Pattern '^\s*(webhook|username|password)\s*=')) {
        $problems += "$($f.Name):$($h.LineNumber) has a $($h.Matches[0].Groups[1].Value)= line"
    }
}
if ((Get-IniValue (Join-Path $cfg 'login.ini') 'Login' 'channel_index') -ne '0' -or
    (Get-IniValue (Join-Path $cfg 'login.ini') 'Login' 'character_index') -ne '0') {
    $problems += "login.ini: channel_index and character_index must ship as 0"
}
if (Test-Path (Join-Path $cfg 'trash')) { $problems += "configs\trash\ is in the package -- somebody's photographed items" }
# And a tester's log starts afresh each session, keeping the one before: sent
# with a problem, it holds two sessions, not weeks of names and chat lines.
if ((Get-IniValue (Join-Path $cfg 'app.ini') 'Paths' 'new_log_each_start') -notmatch '^(true|1|yes|on)$') {
    $problems += "app.ini: [Paths] new_log_each_start must ship true -- a tester's gobot_gui.log would hold every session"
}

# 9. No known private string anywhere in here, text or binary: this
# machine's player's character name, read from the configs beside the build
# and never printed; the real player whose name the docs once gave as an
# example; and, in the programs, a path under C:\Users of the machine that
# built them.
$secrets = @()
$liveHunt = Join-Path $rel 'configs\hunt.ini'
if (Test-Path $liveHunt) {
    $name = Get-IniValue $liveHunt 'Hunt' 'character_name'
    if ($name -and $name.Length -ge 2) { $secrets += ,@('the character name in this machine''s hunt.ini', $name) }
}
# Each as UTF-8 and as UTF-16, the two ways a program or a file holds text.
$needles = @()
foreach ($s in $secrets) {
    $needles += ,@($s[0], $latin1.GetString([System.Text.Encoding]::UTF8.GetBytes($s[1])))
    $needles += ,@($s[0], $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($s[1])))
}
foreach ($f in Get-ChildItem $stage -Recurse -File) {
    $relName = $f.FullName.Substring($stage.Length + 1)
    $hay = $latin1.GetString([System.IO.File]::ReadAllBytes($f.FullName))
    $said = @()
    foreach ($n in $needles) {
        if ($said -notcontains $n[0] -and $hay.IndexOf($n[1], [System.StringComparison]::Ordinal) -ge 0) {
            $problems += "$relName holds $($n[0])"
            $said += $n[0]
        }
    }
}
foreach ($exe in Get-ChildItem $stage -Filter *.exe) {
    $hay = $latin1.GetString([System.IO.File]::ReadAllBytes($exe.FullName))
    $uses = [regex]::Matches($hay, '(?i)[a-z]:[\\/]users[\\/]').Count
    if ($uses -gt 0) {
        $problems += "$($exe.Name) holds $uses path(s) under C:\Users -- build with the source path trimmed (CMakeLists.txt, /d1trimfile)"
    }
}
if (-not ($problems | Where-Object { $_ -like '*holds*' })) { Ok "nothing private found, in the text or the programs" }

# 10. Nothing a run leaves behind: logs, captures, recordings, saved webhooks,
# the debug folders' screenshots, maps learned on this machine.
Get-ChildItem $stage -Recurse -File | Where-Object {
    $_.Extension -in '.log', '.bin', '.s2c', '.c2s' -or
    $_.Name -in 'frame.png', 'region.png', 'gobot_gui.ini' -or $_.Name -like 'playerlist*.png'
} | ForEach-Object { $problems += "$($_.FullName.Substring($stage.Length + 1)) is something a run left behind" }
foreach ($d in 'maps', 'trash_debug', 'revive_debug', 'quest_debug', 'chat_debug', 'templates\seen', 'templates\captcha') {
    if (Test-Path (Join-Path $stage $d)) { $problems += "$d\ is in the package -- something a run, or this machine, left behind" }
}

# 11. The pictures the features load by path, not by templates.ini's names,
# are in here too: quick quest's window, box and tabs for both skins, the chat
# line's two looks, the party's, and the death panel.
$tdir = Join-Path $stage 'templates'
$wanted = @()
$questIni = Join-Path $cfg 'quest.ini'
foreach ($k in 'window', 'window_en', 'auto', 'on', 'off') {
    $v = Get-IniValue $questIni 'Pictures' $k
    if ($v) { $wanted += "$v.png" } else { $problems += "quest.ini: [Pictures] $k is not there" }
}
$tabs = Get-IniValue $questIni 'Pictures' 'tabs'
foreach ($skin in '1', '0') {
    foreach ($t in ($tabs -split ',')) {
        if ($t.Trim()) { foreach ($look in '0', '1') { $wanted += "quest/$skin/$($t.Trim())/$look.png" } }
    }
}
foreach ($k in 'open_picture', 'closed_picture') {
    $v = Get-IniValue (Join-Path $cfg 'chat.ini') 'Chat' $k
    if ($v) { $wanted += "$v.png" }
}
Get-Content (Join-Path $cfg 'party.ini') | Where-Object { $_ -match '^\s*\w*picture\s*=\s*(\S+)' } |
    ForEach-Object { $wanted += ($Matches[1] + '.png') }
$wanted += 'inven/0/dialog_Panic.spr/0.png'
foreach ($w in ($wanted | Sort-Object -Unique)) {
    if (-not (Test-Path -LiteralPath (Join-Path $tdir ($w -replace '/', '\')))) {
        $problems += "templates\$($w -replace '/', '\') is not in the package"
    }
}
if (-not ($problems | Where-Object { $_ -like 'templates\*not in the package' })) {
    Ok "every picture the features load by path is here ($(($wanted | Sort-Object -Unique).Count))"
}

# 12. The notes a player opens, as Notepad needs them: a BOM, and CRLF
# throughout -- the Vietnamese one, the English one and the licences.
foreach ($n in 'DOC-TRUOC.txt', 'START-HERE.txt', 'THIRD-PARTY-NOTICES.txt') {
    $notePath = Join-Path $stage $n
    if (-not (Test-Path -LiteralPath $notePath)) { $problems += "$n is not in the package"; continue }
    $note = [System.IO.File]::ReadAllBytes($notePath)
    $bare = 0
    for ($i = 0; $i -lt $note.Length; $i++) {
        if ($note[$i] -eq 10 -and ($i -eq 0 -or $note[$i - 1] -ne 13)) { $bare++ }
    }
    if ($note.Length -lt 3 -or $note[0] -ne 0xEF -or $note[1] -ne 0xBB -or $note[2] -ne 0xBF) {
        $problems += "$n does not start with a UTF-8 BOM -- Notepad may not show it right"
    } elseif ($bare -gt 0) {
        $problems += "$n has $bare line(s) ending in LF alone -- it must be CRLF"
    } else { Ok "${n}: UTF-8 with a BOM, CRLF" }
}

# 12b. The licences of what is linked into the programs, each of them.
if ($noticesMissing) {
    $noticesMissing | ForEach-Object { $problems += "THIRD-PARTY-NOTICES.txt: $_ is not there to copy -- build first" }
} else {
    # Read with every run of spaces and line breaks as one space: a licence
    # breaks its lines where it likes, SoftFloat's in the middle of its
    # copyright holder's name.
    $said = [System.IO.File]::ReadAllText((Join-Path $stage 'THIRD-PARTY-NOTICES.txt')) -replace '\s+', ' '
    $lacks = @('Apache License', 'Jean-loup Gailly', 'Regents of the University of California',
               'Sun Microsystems', 'Omar Cornut') | Where-Object { $said.IndexOf($_) -lt 0 }
    if ($lacks) { $problems += "THIRD-PARTY-NOTICES.txt lacks $($lacks -join ', ')" }
    else {
        Ok ("THIRD-PARTY-NOTICES.txt: OpenCV $cvVersion, zlib $zlibVersion, " +
            "Berkeley SoftFloat $softfloatVersion, Dear ImGui $imguiVersion")
    }
}

# 13. VERSION.txt says this version.
$first = (Get-Content (Join-Path $stage 'VERSION.txt') -TotalCount 1)
if ($first -ne $Version) { $problems += "VERSION.txt says '$first', not '$Version'" }
else { Ok "VERSION.txt: $Version, commit $commit" }

if ($problems) {
    Write-Host ""
    $problems | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    throw "The package is not ready -- not shipping it."
}
Ok "self-contained, and nothing private in it"

# ---------------------------------------------------------------- zip
Step "Zipping"
$zip = Join-Path $dist "GoBot-$Version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Entry by entry, each named with '/' as the ZIP format has it. Windows
# PowerShell's Compress-Archive names them with '\', and an unzip on a phone,
# a Mac or some tools made flat files called "bitmap_font\lower\0.png".
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipStream = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew)
try {
    $writing = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in (Get-ChildItem $stage -Recurse -File | Sort-Object FullName)) {
            $entryName = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $writing, $f.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $writing.Dispose() }
} finally { $zipStream.Dispose() }

$size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Ok "$zip  ($size MB)"

Step "Contents"
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
$archive.Entries | Where-Object { $_.FullName -notlike 'templates*' -and $_.FullName -notlike 'bitmap_font*' } |
    Sort-Object FullName | ForEach-Object { "    {0,-34} {1,9:N0} bytes" -f $_.FullName, $_.Length }
$pictures = @($archive.Entries | Where-Object { $_.FullName -like 'templates*' }).Count
$glyphCount = @($archive.Entries | Where-Object { $_.FullName -like 'bitmap_font*' }).Count
"    templates/ ($pictures pictures), bitmap_font/ ($glyphCount glyphs)"
# Read back: every file staged is in it, by a name with '/' only.
$staged = @(Get-ChildItem $stage -Recurse -File).Count
$slashed = @($archive.Entries | Where-Object { $_.FullName.Contains('\') }).Count
$entries = $archive.Entries.Count
$archive.Dispose()
if ($slashed -gt 0 -or $entries -ne $staged) {
    Write-Host "    the zip has $entries entries for $staged staged files, $slashed of them named with '\'" -ForegroundColor Red
    Remove-Item $zip -Force
    throw "The zip is not right -- not shipping it."
}
Ok "$entries entries, every one named with '/'"

Write-Host "`nReady: $zip" -ForegroundColor Green
