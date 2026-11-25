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

.PARAMETER SourceCommit
    The commit the source was taken from, for VERSION.txt when git cannot
    tell -- a tree that is no git checkout, such as a source zip with the
    fixes copied over it -- as a hash of 7 to 40 hex digits (a short one, as
    git prints it, is best), with a + when it carries changes, as git's is.
    Without it such a tree says "unknown". Ignored when git can tell.
#>
[CmdletBinding()]
param(
    [string]$Version = (Get-Date -Format 'yyyy.MM.dd'),
    [switch]$SkipBuild,
    [string]$SourceCommit = ''
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rel   = Join-Path $root 'build\bin\Release'
$dist  = Join-Path $root 'dist'
$stage = Join-Path $dist "GoBot-$Version-win64"

function Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Warn2{ param($m) Write-Host "    $m" -ForegroundColor Yellow }

# Visual Studio 2022's folders, whichever editions are installed -- the Build
# Tools that bootstrap.ps1 puts in, or Community, Professional, Enterprise --
# under either Program Files. Its cmake and dumpbin are on no PATH.
function Get-VsDirs {
    # ProgramW6432 is the 64-bit Program Files, which a 32-bit PowerShell does
    # not see in ProgramFiles.
    foreach ($pf in (@(${env:ProgramFiles(x86)}, $env:ProgramW6432, $env:ProgramFiles) | Select-Object -Unique)) {
        if (-not $pf) { continue }
        foreach ($edition in 'BuildTools', 'Community', 'Professional', 'Enterprise') {
            $d = Join-Path $pf "Microsoft Visual Studio\2022\$edition"
            if (Test-Path -LiteralPath $d) { $d }
        }
    }
}

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
    if ($cmake) { $cmake = $cmake.Source }
    else {
        $cmake = Get-VsDirs |
                 ForEach-Object { Join-Path $_ 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' } |
                 Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if (-not $cmake) { throw "cmake not found. Run .\bootstrap.ps1" }
    }

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
#
# The exit code says whether a check failed; the last line, how many parts
# were skipped -- left out because what they need is not on this machine (the
# game's pictures, its glyphs, a saved frame of the client or one GoBot has
# rewritten since, the source tree). Those do not stop the package, but they
# are named, so that a build whose tests mostly did not run is not packaged
# without a word. The tests log to stderr as they go -- refusals they provoke
# on purpose among it -- and under 'Stop' a host that hands a program's stderr
# to PowerShell (ISE, or this script's output redirected) took the first such
# line for a failure of this script.
Push-Location ([System.IO.Path]::GetTempPath())
try {
    $ErrorActionPreference = 'Continue'
    $testsOut = @(& (Join-Path $rel 'gobot_test.exe') 2>$null)
    $testsExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = 'Stop'
    Pop-Location
}
if ($testsExit -ne 0) { throw "Self-tests failed -- not packaging" }
$summary = $testsOut | Where-Object { $_ -match '^\d+ passed, \d+ failed, \d+ skipped$' } | Select-Object -Last 1
if (-not $summary) {
    throw "The self-tests did not say how many passed, failed and were skipped -- a gobot_test older than this script? Build it again."
}
$null = $summary -match '^(\d+) passed, (\d+) failed, (\d+) skipped$'
$passed = [int]$Matches[1]
$skipped = [int]$Matches[3]
if ($skipped -gt 0) {
    Warn2 "self-tests pass ($passed checks), but $skipped part(s) were SKIPPED -- they did not run here:"
    $testsOut | Where-Object { $_ -match '^\s*\[skip\] ' } | ForEach-Object { Warn2 "  $($_.Trim())" }
} else {
    Ok "self-tests pass ($passed checks, none skipped)"
}

# ---------------------------------------------------------------- stage
Step "Staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'configs') | Out-Null

# The two programs a player runs. Not gobot_sniff (protocol tools), and not
# gobot_test (it reads the source tree, which a tester does not have).
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
    # git complains on stderr when the tree is no checkout, and under 'Stop'
    # Windows PowerShell 5.1 turns the first complaint into a failure of this
    # script. Only the exit code says how it went.
    $ErrorActionPreference = 'Continue'
    try {
        $hash = (& $git.Source -C $root rev-parse --short HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and $hash) {
            $commit = "$hash".Trim()
            $dirty = (& $git.Source -C $root status --porcelain --untracked-files=no 2>$null)
            if ($dirty) {
                $commit += '+'
                Warn2 "the tree has changes that are not committed: VERSION.txt says commit $commit"
            }
        }
    } finally { $ErrorActionPreference = 'Stop' }
}
# Given by hand for a tree that is no checkout. Shortened as git shortens it:
# the log and --doctor keep only the first 40 characters of the line, and a
# full hash with its + would be cut off.
if ($commit -eq 'unknown' -and $SourceCommit) {
    if ($SourceCommit.Trim() -match '^([0-9a-fA-F]{7,40})(\+?)$') {
        $full = $Matches[1]
        $commit = $full.Substring(0, [Math]::Min(12, $full.Length)) + $Matches[2]
    } else {
        throw "-SourceCommit must be 7 to 40 hex digits of a commit, with an optional +: '$SourceCommit'"
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
    # Or already beside the programs, where a package has them: a build
    # folder set up from a package's pictures and glyphs has them there.
    $glyphBeside = Join-Path $rel 'bitmap_font'
    if (Test-Path $glyphSrc) {
        Move-Item -LiteralPath $glyphSrc -Destination (Join-Path $stage 'bitmap_font')
        Ok "bitmap font staged ($((Get-ChildItem (Join-Path $stage 'bitmap_font') -Recurse -File).Count) glyphs)"
    } elseif (Test-Path $glyphBeside) {
        Copy-Item -LiteralPath $glyphBeside -Destination (Join-Path $stage 'bitmap_font') -Recurse
        Ok "bitmap font staged from beside the programs ($((Get-ChildItem (Join-Path $stage 'bitmap_font') -Recurse -File).Count) glyphs)"
    } else {
        Warn2 "no bitmap_font in the build, in templates\ or beside the programs -- HP/MP figures, names, chat and the player list will be unreadable"
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
  pressing keys and clicking as a player would: fighting, skills, potions
  (the pet's too), buffs, reviving with a revive item, logging back in,
  party invitations, cleaning the bag, timed chat lines, quick quest, and
  alerts to Discord. When hunting on a machine with Npcap it also reads the
  game's own network traffic, to know where the character and the monsters
  are (Vi tri tu goi tin): it only reads it. It reads no game memory, sends
  no packets of its own and asks for no account or password. Nothing is
  sent anywhere except the alerts, to the player's own Discord channel,
  when they switch them on.

STARTING
  1. Unzip everything into a new folder under C:\Program Files, such as
     C:\Program Files\GoBot -- Windows asks for administrator to make it.
     GoBot runs as administrator, so its folder must be one only an
     administrator can change: any program can write into C:\GoBot, the
     Desktop or Downloads, and what it left there would run with GoBot's
     rights. Not inside the zip. To change a file in configs\ by hand, run
     Notepad as administrator.
  2. Run gobot_gui.exe. It asks for administrator, and it has to: the game
     runs elevated, and Windows drops the keys of a program that is not.
     SmartScreen may say "Windows protected your PC" -- More info, Run
     anyway: GoBot is not signed.
  3. The game in a window, 1024x768, 1280x720 or 1366x768. 1024x768 has
     run in full; at 1280x720 the HUD, hotbar and chat bar have run on a
     player's machine; at 1366x768 only the HUD and hotbar were seen, once;
     Tu dang nhap is not yet tried at 1280x720 or 1366x768. Windows'
     display scale can be anything on Windows 10 (1607 or later) and 11; on
     an older Windows keep it at 100%. Above 100% Windows may enlarge the game, and
     an enlarged game cannot be read: when GoBot says Windows dang phong to
     game, press Sua cho game and start the game again (or set the scale to
     100%); Windows dang phong to chinh GoBot has Sua cho GoBot. Tu dang
     nhap knows VTCZone by pictures taken at 100%: if VTCZone draws itself
     bigger, use 100%. Keep every window, GoBot's included, off the game: it
     is read off the screen. On a screen 768 pixels high a 1024x768 or
     1366x768 game does not fit: use 1280x720, with the taskbar set to hide
     itself (GoBot says "Game khong nam tron tren man hinh" when it does not
     fit).
     Where GoBot cannot sit beside the game, press BAT DAU and minimise
     GoBot: it goes on running, and F10, F11 and F12 still work in the game.

THE SIMPLE SCREEN
  Eight tabs: Danh quai, Ban do, To doi, Tu chat, Tu dang nhap, Don rac,
  Nhiem vu, Thong bao. Everything that acts on the game ships switched OFF:
  tick what you want first. Settings save themselves.

  Ban do (since 2026.11.04c; tried on the real game with 04c and 04d, and
  this version's changes not yet) draws the map the character is
  on from the game's own files -- platforms, the bottom ground, gates --
  with the character and the monsters where the
  game's packets put them (Npcap), as GoC does. Since 2026.11.05 a walking
  monster's dot, and the character's, is drawn on its platform as the map
  file has it (the bot itself still judges heights as before), a dead
  monster's dot goes at once, no monster is drawn while no packets come,
  and the tab is redrawn 30 times a second (2 behind the game with
  Nhe may). On it, with the mouse only, each map gets one hunting area:
  a box, and a home on a platform inside it that the bot walks back to.
  Saved on the mouse's release in configs\spots.ini, which GoBot writes
  and no package ships, with Hoan tac to undo. With Cho danh quai ticked
  the bot uses the map's area once the character stands still inside it
  or within 150 px of it (or back near where it left it, through a door
  or after logging back in); farther off it hunts where the character
  stands, as before. Dat cho nay on a map with an area moves the area
  there, and saves it.
  Since 2026.11.04d, by the map's own file, the bot never walks toward a
  monster out of the area's box; more than 60 px outside it, with a way
  back along the floors (or off a floor's end into the box), it walks back
  first, never hopping, and with none it fights where it stands (since
  2026.11.12 it stands still, and the way is looked for over other floors
  too); the card
  under the map says how far outside it is. Put the home on the LOWEST
  floor to be hunted and drag the box UP, not down -- 04c's notes said the
  reverse, and the card now says in red when most of the box cannot walk
  back -- and do not make an area right after coming through a door, where
  the home lands on the arrival spawn. Drops now work on sloping floors,
  and the hunter stops once within 24 px across of the monster below
  before it judges one.
  Since 2026.11.05b (not yet tried on the game) an area has two more ticks,
  both on as shipped (a hunt.ini without them reads them on). Nhay len buc
  tren jumps straight up -- Up, Up Up or Ctrl+Up, chosen by the map's own
  file with a margin -- onto a monster's platform above and on the way
  home, only where the take-off, the landing and the monster all lie
  inside the area and no platform outside it lies within 650 px above
  (since 2026.11.12c for Ctrl+Up only: Up looks 245 px up, Up Up 413); it
  needs the jump key to be Up, and a map change within 10 s of a jump
  stops the hunt and unticks it. On t52_s2 the home ledge is reached from
  the ground by Ctrl+Up at about x 695-1002, so the home can stay where it
  is. Tu tim quai trong vung goes to any monster inside the area the
  character can get to, in view or not, and fights it from a place inside
  the area; with none left it stands where it is (outside the area it
  walks back first, as above). While it is on, the bot judges the
  monsters' heights on their platforms too. Hunt t52_s2 and send
  gobot_gui.log.

  Hunting needs the character's name typed on Danh quai: the hunter finds
  the character by that name, whole -- a player whose name has it inside,
  "TuanAnh" for "Anh", is not taken for the character -- and never by a
  plate's bars, which any player may wear. It does not start without it.

  BAT DAU or F10   start / stop everything ticked, on every tab
  F11              hunting on and off
  F12              stop everything at once and let go of every key

  F10 and F11 act only while the game or GoBot is the window in front; F12
  acts from anywhere.

  GoBot's keys reach the game only while it is the window in front. So
  Luon dua game len truoc khi bot dang chay (under BAT DAU, ticked as
  shipped) brings the game back whenever another window takes the front
  while the bot runs -- GoBot's own too -- a moment (0.2 s) after the
  player's last click or key. Whatever they then type into another window
  goes to the game: untick it to use the PC while the bot runs. A key box
  clicked to change a key waits up to 10 seconds for the key -- less when
  HP falls under the HP potion's mark, or under 50% if that is higher, so
  that the potion can go.

  Keys the game does not see. The game reads its keyboard once a frame, so
  a key held too briefly is now and then lost: pressed, and nothing
  happens. The macro holds each key 70 to 110 ms (key_hold_min_ms and
  key_hold_max_ms in configs\macro.ini; 25 to 60 before), keeps two potions
  60 ms apart, and presses a potion again after potion_recheck_ms (1000)
  when its bar has not risen and still wants the drink: once per press, and
  the log says "Potion check: ...". 0 in the file is the old behaviour. If
  the game's potion takes longer than a second to show, that can cost one
  extra potion. The full screen's Macro tab, Options, has the three
  numbers.

  Skills that drain MP faster than the potions make up. A potion's bar
  under HALF its mark (a 59% MP potion: under 29%) lets it drink again
  potion_urgent_ms (1000) after its last press, not after its whole
  interval (3 s); between the mark and half of it the interval stands.
  potion_urgent_ms in configs\macro.ini, 0 for the plain interval; the
  full screen's Macro tab, Options, has it. A second, bigger potion row
  (Lv2) at a lower mark helps further.

  Buffs that work sometimes. A buff is cast the way GoC casts it: its key
  pressed 4 times, 200 ms apart (buff_presses and buff_press_gap_ms in
  configs\macro.ini; 1 is the old single press). The game drops a single
  press now and then, and a buff dropped is gone for its whole interval. The
  log has a "Buff: key ... cast" line for each cast, and the reason when one
  cannot be cast (the game not in front, the character not in the world...):
  send those lines if buffs still come and go.

  Skills that come out unevenly (2026.11.08, not yet tried on the game). On
  2026-11-08, the macro alone on the battlefield, each skill key went on its
  own timer, so its interval was its share of the presses (Z at 0.5 s took
  27%, S and A at 3 s 4% each), and the game, casting about 77 a minute,
  ignores a key pressed during a cast: which skill came out was chance. One
  interval for all eight skills (1.2 s each, in Chi tiet...) evens it out
  today. The new tick Ra chieu theo vong (in the Ky nang window and on the
  full screen's Timed keys; OFF as shipped in this version, ON and with Tu
  danh quai too since 2026.11.16b, below) presses the skills one at a time
  in the order of the keys, each only once the game has cast the one before
  -- seen in the game's packets, so it needs Npcap -- and goes back to the
  timers after 2 s with no cast seen. In this version only with Tu danh quai
  off. Tu mua binh and quick quest, after someone was
  at the mouse, now wait 30 s, then 2, 5 and 10 minutes rather than 30 s
  every time; keep them, the bag and the charms off during battlefield
  fights, and leave GoBot's window alone while the macro runs. Fight 15
  minutes or more on t1981_s2 with the tick off, then on, and send
  gobot_gui.log.

  Buying by the slot and the check-in board (2026.11.09, not yet tried on
  the game). Tu mua binh counts nothing now: each time it buys the slots set
  on the card for each potion (moi lan mua N o, 0 to 20 then and 0 to 999
  since 2026.11.10, 3 and 3 as shipped; a trash.ini from before reads its old
  buy_hp_to and buy_mp_to, 20 at most, until the card is changed), HP and MP
  in turn, 10 purchases a hold and the rest a minute later; Mua ngay buys up
  to 40 at once. The game's message that the bag is full ends that round --
  closed by its Xac Nhan, never ESC, then the shop -- and the box stays
  ticked for the next time. When the next plan
  begins is kept in %LOCALAPPDATA%\GoBot\shop_clock.txt, so a restart never
  buys early. Tu dong thong bao (OFF as shipped; tick it once) now also
  closes the Su kien va diem danh board the game shows at every login and
  after 00:00, with ESC about 3 s after it shows, never its X, and only while
  the bot runs; while it is up the bag jobs and quick quest wait a minute at
  a time. Try Thu mua first, then 1 + 1 with Mua ngay, watched; send
  gobot_gui.log with the popup_debug and trash_debug pictures.

  The pet, walking back, the bag and the shop (2026.11.10, not yet tried on
  the game). The pet's potion goes over and over while the pet is under its
  mark: every Bom lien tuc, moi N ms on its row (200 to 3000 ms; 300 as
  shipped and in a macro.ini without pet_every_ms, where it was about every
  3 s), as GoC feeds the pet; a skill that is ready takes the next press, so
  the skills are not starved, and HP and MP potions still go first. More
  than 60 px outside the hunting area the bot stops fighting -- monsters in
  the area and in reach too -- and walks back along the ground first, no
  skill keys meanwhile (potions and buffs go on), fighting again once inside;
  back through any door to the area's map, it takes the area again once the
  map has loaded. A Don rac pass with no tab ticked under Tim trong does not
  open the bag; with every item a Tieu Dung icon of the game's it looks
  through Tieu Dung alone; and each item is quicker, every check kept.
  During a Don rac pass and a Tu mua binh round the macro's keys on a letter
  or Space -- skills, buffs and potions -- go on between GoBot's own clicks,
  never with a dialog, a message or the quantity box up; digit keys wait,
  and Dung bua still holds every key. Tu mua binh buys 0 to 999 of each. An
  HP of 0 read off the screen is no death while the game is behind, while the
  packets say HP is above 0, or while the server is silent. Count the pet's
  food each hour, and send gobot_gui.log: its Pet heals, Hunter, Positions:
  own walk, Trash: and Alerts lines say what happened.

  The shop right after the pass, potions during the bag jobs, and six more
  keys (2026.11.11, not yet tried on the game). With Don rac on, when Tu mua
  binh comes due (a plan, or the rest of one) GoBot cleans the bag first and
  buys right after it, in the same stop of the keys, with no quiet second in
  between; after a pass that ended less than 1 minute before, none is made
  again and the shop goes alone, as before (with Don rac every 10 minutes and
  buying every 60, as the zip ships, most rounds go so: alone, right after
  their pass). No pass is made for the shop while the game is not in the
  world (HP not read) or not found. Mua moi N phut, shop_clock.txt,
  10 purchases a hold, Mua ngay, Thu mua and Don ngay are unchanged, and Don
  ngay never buys. With the HP potion on a letter, Space or one of Home,
  End, PgUp, PgDn, Insert, Delete, a pass or a round starts at the HP
  potion's mark (not 10 over it), HP and MP potions go between purchases and
  between two items (up to 0.4 s each time), and the job stops only under
  HALF the mark (80 gives 40), tried again 15 s later and never counted as
  trouble. With two HP potion lines (Lv1, Lv2), each one marked at half the
  highest or over must be on such a key too, or the job goes as before.
  Those six keys now go through as the letters do, for skills,
  buffs and potions; what the game does with them while the bag or the shop
  is open is not known yet, so GoBot waits 0.2 s after one, and double-clicks
  a potion only when it sees the potions' row itself. Digit keys still wait:
  the simple screen fills in 1 and 2 for the HP and MP potions (so does the
  zip's macro.ini), so tell testers to move them to a letter or one of the
  six to have them pressed meanwhile. On the simple screen Delete is now a
  key like any other, and Backspace clears a key box. Send gobot_gui.log:
  its Trash: lines say what happened.

  Turns, the area, falls and two bars (2026.11.12, not yet tried on the
  game). The owner's hunt of 2026-11-11 showed a skill that carries the
  character about 214 or 319 px with no walk, pressed on its own timer
  whatever the hunter did. With the packets steering, a turn now waits for
  the game's report of it (1 s at most; the tap is 80 ms) before the hunter
  swings, and a monster walking through the character no longer turns it
  back and forth. The hunter's own attack key goes only within the reach its
  skill was seen to have (attack_range_px in configs\hunt.ini, 0 = as
  learned). The macro holds the skill keys -- never potions or buffs --
  while a turn waits, through a drop, with nothing to fight in the area and
  outside it with no walk back; and the dash's key (learned from the
  packets, the log names it; until then C, V and Space -- since 2026.11.17
  no key until one is learned) where its landing
  would leave the area or run past a floor's end. With Cho danh quai,
  monsters outside the area are never fought, in reach or not. Fallen out
  of the area, the character finds its way back by the map's file over
  other floors too -- walking, walking off ends, Up / Up Up / Ctrl+Up, 4
  jumps at most, and with Nhay xuong ticked Ctrl+Down through platforms by
  the drop's own rules, 8 at most -- for a Ban do area and, with the file
  trusted, for this run's spot; with no way it stands still, skills held,
  and walks back given up on 3 times are tried once more a few minutes
  later. A rise of more than
  1000 px with no walk, jump or door is a fall into a pit put back up top
  by the game: it stands until a second report agrees, then works the way
  back out from there; walks stop at a pit's edge. Ban do draws the band as
  two dashed bars from the character's feet, dragged or typed under the map
  (0 to 1000 px; up_reach and down_reach in hunt.ini, also Cao nhat and
  Thap nhat): lower than the lower bar the bot drops, higher than the upper
  it jumps, between them it fights where it stands. drop_min_px (100) and
  the other hidden minimums are gone, so check the bars first. Don rac and
  Tu mua binh count from the start: the first timed pass comes
  interval_minutes after BAT DAU, the first shop look buy_every_minutes
  after it (Don ngay and Mua ngay still at once). From the top of t62_s5 the
  way back needs Ctrl+Down: with Nhay xuong off the character stands there.
  The way back's drops are not yet tried on the game. Watch a hunt on
  t23_s14 and t62_s5 and send gobot_gui.log: its Hunter, Skills, Positions
  and Dash (packets) lines say what happened.

  Charms, drops, the quick quest and popups at any HP (2026.11.12b, not yet
  tried on the game). Dung bua (timed and Dung bua ngay) no longer waits for
  HP 10 over the HP potion's mark, nor stops under the mark: it goes at any
  HP but the character down, and presses the HP and MP potions itself
  between two charms (letters, Space and Home/End/PgUp/PgDn/Insert/Delete
  only, never a digit nor the pet's), the bot running or not; skills and
  buffs still wait. The hunter's drops (Ctrl+Down) and jumps up (Up, Up Up,
  Ctrl+Up), for a monster and on the way back, no longer wait for HP over
  the HP potion's mark. Nhiem vu nhanh and Tu dong thong bao start at any HP
  and stop only with the character down. Tu chat is unchanged. Send
  gobot_gui.log: its "Trash: own potion press between two charms" lines say
  when GoBot drank.

  Jumps past the two bars, the wall foot and the way back (2026.11.12c, not
  yet tried on the game). The jump model's gravity is 1900..2400, from the
  two live flights of 2026-11-12: Up reaches 133..169 px, Up Up 250..337,
  Ctrl+Up 432..546. A floor in a key's uncertain band no longer stops a jump
  when it is a platform inside the area, and a take-off's own slope is no
  floor above: from t23_s14's bottom the way back walks to its take-off and
  goes Ctrl+Up, then Up Up, as the owner did by hand (no way from any of 232
  places outside that area before, a way from all of them now, by GoBot's
  own model of falls and jumps in the tests -- not yet measured in the
  game). A monster above the upper bar within one plain Up gets one Up in
  place, attacked in the air, at most 10 a minute; below the lower bar,
  Ctrl+Down as before, now landing within one Up of it where that Up can
  come down on no other floor. The Up-in-place replaces the old rule of no
  jump attack in place for those monsters. The 650 px rule now holds for
  Ctrl+Up only: Up looks 245 px up for a floor outside the area, Up Up 413.
  t49_s5's step at x 2816 is a wall, not a pit: at its foot the bot presses
  Ctrl+Up in place onto the ledge, and an Up there that climbs the step
  counts as standing on it. A jump is judged by its take-off's speed (the
  2026-11-12 client sends no state 1, 2 or 6). Stranded, the way back is
  asked again every 10 s (every 3 s since 2026.11.15); under the area after a
  fall or a relog, the area
  is taken and, with the map file not trusted yet, up to three steps to
  places 80 px apart earn it. Send gobot_gui.log: its Hunter, Packets and
  bars: lines say what happened.

  HP and MP figures with a digit missed (2026.11.13, not yet tried on the
  game). The bars' figures "11552/18590" read as 1/552 (0%) and
  "18590/18590" as 1859/18590 (10%) on 2026-11-13: a '1' taken for 'l' and
  a '0' for 'O' or 'D' were passed over as not digits, and the packets'
  check believed the bars. On the game's 6 px pitch (a 3 px slash) a missed
  digit leaves a hole, and such a read is now refused; a read whose maximum
  is not the one seen twice in a row gives no figures either; and a figure
  that is the packets' value less one digit no longer counts against the
  packets. Send gobot_gui.log if a "Vitals: ... disagreement" line (since
  2026.11.17: "other" on the minute's "Vitals:" line) still shows the bars
  far off the packets.

  Buy right after the pass, and the game's message at login (2026.11.13b,
  not yet tried on the game). A new box on the Tu mua binh card, "Mua ngay
  sau khi don rac" (off as shipped): with Tu don rac on, potions are bought
  right after each timed pass and the "mua moi" clock begins no round. Auto
  login: a message of the game's with only Xac Nhan after Ket Noi ("Tai
  khoan da bi chan") is closed and Ket Noi pressed again at once, with no
  limit and not counted as an attempt; F12 still stops it. Send the
  login_debug folder and gobot_gui.log if it is not closed.

  The game at 1024x768, 1280x720 and 1366x768 (2026.11.13c, not yet tried
  on the game at 1280x720 and 1366x768). Every screen GoBot reads was gone
  through for the three sizes. New on the checklist: "Game khong nam tron
  tren man hinh", when an edge of the game is past the screen's edge or
  under the taskbar; where GoBot reads the game off the screen (most
  machines) the potion count, Tu chat and the keyword's chat lines then
  wait and say why. Tu dang nhap at another size: the character slot is
  picked by where "Bat Dau Game" is, and a picture of that screen is kept
  in login_debug while nothing is over the game; the channel list is never
  clicked by guess. With "Bat Dau Game" in the bottom-right corner the
  slots are taken from the middle of the game, which is a guess: if the
  wrong character enters, report it at once. slot_anchor=picture (or
  centre) in [Page.character] of configs\login.ini changes that without a
  new GoBot. GoBot also keeps vitals_debug\size_<W>x<H>_*.png once a
  session at a new size, when it reads HP there. At 1280x720 or 1366x768:
  first make the game fit the screen (no red "Game khong nam tron tren man
  hinh" row) with no window over it; if you can, use an account with 2 or 3
  characters and choose the 2nd or 3rd; run Tu dang nhap once and play a
  minute; then send the login_debug and vitals_debug folders,
  gobot_gui.log and gobot_gui.old.log, with whether the chosen character
  entered, the screen's size, the display scale, Full or Window Mode (in
  Full Mode: whether the row appears), and whether the taskbar hides
  itself. If vitals_debug has no size_ file, add a screenshot of the game
  in the world (Alt+PrtSc).

  The game's window in the corner after a login (2026.11.14, not yet tried
  on the game). Each time GoBot itself gets into the game (Tu vao lai game,
  the login key, gobot_cli --login), it moves the game's window to the
  top-left corner of the screen it is on (above the taskbar), and at no
  other time; it is never resized. When the whole window would not fit
  there, the game's picture goes in the corner and its title bar out of
  sight above the screen's edge (1280x720 on a screen 768 high with the
  taskbar shown then fits); to move the game then: click it, Alt+Space,
  Move, the arrow keys. A minimised, maximised or Full Mode game is left
  where it is. GoBot's own window is moved beside the game if it now covers
  it. To keep the game where you put it: move_game_to_corner=false in
  configs\login.ini.

  The shop after a pass, ESC after 10 s stuck, and the game's message after
  an invite (2026.11.14b, not yet tried on the game). Before buying, the
  shop round looks at the bag twice: open, it buys at once; shut, it
  presses the bag key once and checks the bag stays open. Khac is clicked
  only on the bag just seen, up to 4 times. At the end of a pass the bag key
  is pressed once and GoBot waits to see the bag shut. A shop step or an
  item in the bin stuck for more than 10 s: GoBot presses ESC, only on a
  look that shows something open, until no shop, dialog, message or bag is
  on the screen (6 at most; the bin's question gets Huy Bo first); the
  round is tried again later. Charms never press ESC or Enter. After each
  party invite, a game message with only Xac Nhan is closed by clicking
  Xac Nhan (never Enter), and that player is not invited again for 10
  minutes (30 s since 2026.11.15b). Send gobot_gui.log and the trash_debug
  folder if it goes wrong.

  Walls, a lost key press and the way back (2026.11.15, not yet tried on the
  game). The map file's walls and blocks -- the pocket ledge of t60_s1 at x
  4480 has one on its left -- now stop a walk: the bot no longer pushes into
  one for seconds, taking it for a floor's end. A key press the game loses --
  about one in three, at random -- no longer switches Nhay xuong, Nhay len or
  the plain jump for a monster above off until the next start, and no longer
  forbids a place: the first miss is tried again at once, and after 3 in a
  row (or 5 in a run, with more misses than hits) that jump or drop pauses
  for 30 s, then 60 s, then 120 s, and goes again by itself. When a dash ends
  in the air and the game sends no landing report, the bot now takes the
  character as landed on the floor under it and walks back, instead of
  standing out of the area for good; the way back is asked again every 3 s.
  One odd landing report no longer makes the bot give up the map file for the
  rest of the session: it waits for the next report, and a file it did
  distrust comes back after 2 minutes or at the next start. Outside the
  area the bot stops fighting and walks back; it does not learn a second dash
  skill to stay in. Send gobot_gui.log if it goes wrong.

  The shop and the party again after 30 s (2026.11.15b, not yet tried on
  the game). A shop round that ends in trouble -- stuck and shut with ESC,
  the Khac tab not switched, a step that did not go -- is tried again 30 s
  later, not 10 minutes; a second trouble in a row still leaves the rest
  until the next "mua moi" time. A player whose invite the game answered
  with its message (already in a party, refused) can be invited again after
  30 s, not 10 minutes.

  A report in mid-air is no landing (2026.11.15c, not yet tried on the
  game). At 10:15:51 of the first live run a report in the air, 0.5 s into a
  jump that landed at 1.2 s, was taken for the landing, judged off the map,
  and the map file was given up on for 2 minutes with the jumps. The bot now
  waits for the real landing whatever kind of report came first, and for as
  long as the character is still moving in the air. Send gobot_gui.log if a
  line "jump up landed ... on no floor of" still shows.

  Thirty seconds for the game to react, and the board's ESC after a login
  (2026.11.15d, not yet tried on the game). For slow PCs: after each key,
  click, drag, typing or ESC, GoBot now waits up to 30 s for the screen to
  show what it did, and goes on at once when it does -- a fast PC is as quick
  as before; only after 30 s with nothing is the step given up. This covers
  the bag jobs (Don rac, Dung bua, Tu mua binh: a step stuck 30 s, not 10 s,
  gets the ESCs), the quick quest, Tu chat (each Enter waited 30 s before
  the next), the party list and its Req Party (never clicked blind), the
  game's message after an invite, Tu hoi sinh, auto login's channel row and
  login message, and Tu dong thong bao. GoBot never presses a key again just
  because the game is slow. The "Su kien va diem danh" board now gets its
  ESC 30 s after the character comes into the game, and never from 23:55 to
  00:03 by the PC's clock: at the day's turn it waits until 00:03. A key
  press the game loses now costs up to 30 s. Send gobot_gui.log if it goes
  wrong: the lines "the game took N ms to show", "nothing after 30 s" and
  "the check-in board waits for its ESC until".

  Lighter on weak PCs, and old features removed (2026.11.16, not yet tried
  on the game). For a player's PC of one core and 3.5 GB that stutters with
  GoBot. On the owner's PC GoBot used about 11.5 s of CPU a minute while
  hunting; the survey estimated about 4 s if all its proposals were done --
  an estimate only, not measured, and one proposal is not in this version.
  Fewer looks at the screen: Tu dong thong bao looks every 2 s (3 s with Nhe
  may) while nothing is up, not every 1 s, so a popup can be seen up to 2 s
  later (its 3 s rule and the check-in board's 30 s and 23:55 to 00:03 are
  unchanged); the monsters' names are read every 2 s, not 0.6 s, while the
  packets steer; with the pet's potion key HP and MP are read every 0.5 s
  while the packets carry them, not at full speed. Less work per look: the
  picture searches reuse their memory (same results), run one after another
  on a PC of 1 or 2 cores, and the game's pictures are loaded once; moving
  the mouse over GoBot no longer makes it work harder; the client keeper
  looks for a crash every 6 s, not 3; background jobs no longer jump ahead
  of the game when they wake (keys, potions and fighting unchanged). Nhe may
  now ticks itself on a PC of 1 or 2 logical CPUs or under 4 GB of RAM (the
  grey line says why; unticking it still turns it off). Fixed: an MP figure
  read with several leading digits lost (16462 as 462) no longer makes GoBot
  distrust the packets' MP. Removed, all on the full screen or in gobot_cli:
  the old packet-only bot (Bot > Start, the Dashboard, Protocol, Autopilot,
  Calibrate, Mapper and Map tabs, configs\bot_default.ini), the Radar tab
  and walking to saved points (hunt.ini's [Move] is no longer read), the
  minimap watch, fishing and mining (configs\gathering.ini), and gobot_cli's
  developer commands for them (with no argument it now prints its usage;
  --doctor, --login, --fix-dpi and the look-only checks stay). The simple
  screen is the same except one warning gone with the walker: "Phim nhay khi
  di toi diem da luu la Space". Send gobot_gui.log, with its Load: lines,
  if the PC still stutters, saying how many cores and how much RAM it has.

  Skills in turn while hunting too (2026.11.16b, not yet tried on the
  game). The owner's report: the skills came out unevenly -- pauses of 1-2 s
  with no skill, and the long-wait keys rarely cast while the short ones went
  all the time (the hunt of 2026-11-15: Z, X and C each pressed about 90
  times a minute for 14-17 casts). The tick Ra chieu theo vong is now ON by
  default, also for a macro.ini saved before (its cast_paced=false was the
  old default, not a choice), and it is on the Tu dung ky nang row itself.
  It works while Tu danh quai runs too: the skill keys go one after another
  in the order of the keys, each once the game has cast the one before; a
  key not due yet passes its turn, so no key is starved. The hunter's holds
  (walking back, turning, a drop, nothing to fight, outside the area, a jump;
  with Chi khi dang danh quai also no fight and MP under the floor) keep the
  round at its key, and it goes on from that key, one key at a time. A key
  whose monster is out of its reach, or the dash key near an edge, only
  passes its turn: the round goes on with the next key. When the attack key (Phim danh) is also a skill, the round presses
  it in its turn and the hunter does not press it on top; the hunter's own
  attack only fills the gaps when no skill is ready. Without the game's
  packets (no Npcap) every key goes on its own timer as before; potions
  always go first. To switch it off: untick Ra chieu theo vong on the Tu dung
  ky nang row (kept after a restart; by hand, cast_paced_v2=false in
  configs\macro.ini with GoBot closed). Send gobot_gui.log after a hunt, with
  its "Paced:" lines (now each key's presses and casts) and "Casts
  (packets):" lines.

  MP bar and skills while hunting (2026.11.17, not yet tried on the game).
  The owner's report after 2026.11.16b: the MP bar read wrong and the skills
  came out unevenly. Standing, the MP potion went about 60 times a minute
  with MP at 92-100%: the glowing tip of the EXP bar sat over the first digits
  of the MP figure, so 15542/16813 read as 542/16813 (3%). HP and MP now go
  by the game's packets whenever GoBot has them, with no check against the
  screen first (the owner's choice); the bars on the screen are used only
  without packets (no Npcap, a gap in the capture or just after a start until
  the game sends HP/MP, the game quiet for 5 s; a door is not one: the last
  values carry across it while the map loads). If the game ever sends HP or MP otherwise,
  GoBot will not notice by itself; the minute's "Vitals:" line still sets the
  bars' figures beside the packets', for the log only. Without packets, a
  figure covered that way is refused, and MP reads as nothing rather than
  3% or 57%. A potion pressed 3 times in a row with no rise goes at its own
  cooldown until the reading rises (HP still falling brings the quick presses
  back). Skills: while hunting, C and V came out once a minute and the
  others 14-16 times. With no dash skill learned yet, C, V and Space were
  all taken for the dash key and held near every edge; now no key is held
  for a dash until one is learned. Cooldowns of 20-70 s learned by mistake
  for skills cast every 3 s are now capped by how soon each skill was ever
  cast again, and at 12 s. With the character dead (00:07-00:09 that night)
  and the packets still coming, the round now tries one key a second instead
  of 220-230 presses a minute on the timers; HP under 0.5% holds the skill
  keys (potions and buffs still go) -- the HP GoBot goes by, the packets'
  while it has them, which kept 27% through that death. A buff's cast seen ends the rest of its
  presses. Nothing to set. Send gobot_gui.log after a hunt with the macro,
  with its "Vitals:", "Potion check:", "Potions:", "Paced:" and "Dash
  (packets):" lines.

  Party: inviting somebody already in a party (2026.11.17b, not yet tried
  on the game). The owner's report: inviting somebody already in a party
  brought a notice, then the bot stood still and kept opening the player
  list. Three changes, the owner's choice. A player whose invite did not
  lead to a join -- one more member's panel on the party HUD 15 s after
  it -- is not invited again for 1 minute, 10 minutes after 2 in a row,
  whether or not GoBot saw the notice; a notice after the invite counts at
  once, a join starts the count again, and a held-off player never has the
  list opened for them. The one-button notice (Xac Nhan only) is clicked
  with the game's mouse held 2.5 s at most, not 30 s; still up, it is
  clicked again on a later pass, 3 clicks in all, then no invite for 1
  minute. Never Enter or ESC, never a box with two buttons. After every
  invite the player list GoBot opened is shut again with the same button
  that opens it (the little figure under the minimap), found by its
  picture at 1024x768, 1280x720 and 1366x768; a good invite shuts it by
  itself and nothing is pressed then. Nothing to set. Send gobot_gui.log
  with its "Party:" lines, and a screenshot of the "already in a party"
  notice -- GoBot has no picture of it yet.

  Check-in board closed even after a missed look (2026.11.17c, not yet
  tried on the game). The owner's report: the "Su kien va diem danh" board
  stayed up with "Tu dong thong bao" on. One look that missed the board --
  the game writes its lines of news over the board's title -- started its
  30 s over, and the run was stopped before they ran out. Now a board seen
  again where it was within 15 s is the same board, its 30 s still counted
  from the arrival (one the player closes and opens again later is a new
  board, with its own 30 s), and seen again it gets no ESC for 3 s; a
  title partly covered by those lines is still told where the board was
  just seen whole, and is not taken for closed after its ESC. Every reason
  no ESC went is said once in the log, and another job's hold puts the
  board off 5 s, not 30 s. Still no ESC beside another
  window, under another job's hold or while the player types; the board is
  never clicked. Nothing to set. Send gobot_gui.log with its "Popups:"
  lines and the popup_debug pictures.

  Walking to a monster, the hunter holds the arrow key down as a player
  does, rather than tapping it (the character stopped after every step).
  It lets go in reach, when the monster is behind, to loot, when the game
  is not in front, and when another job needs the keys. To bring the old
  steps back: on the full screen (Nang cao), the Features tab, the Attack
  panel, untick "Hold the walk key" and press that panel's Save (without
  Save it is ticked again next time); or, with GoBot closed, set
  hold_walk=false in configs\hunt.ini.

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
  timers from 0 seconds (Tu buff; 0 is every 5 seconds, as GoC casts a buff
  whose cooldown is 0), inviting whoever types the keyword (at 1280x720) and,
  with it, inviting only while leading (Chi moi khi dang lam chu to doi),
  logging in without an account (Tu dang nhap) with its new options (Cung
  vao lai khi game van chay ma khong thay nhan vat qua N phut, and Phim vao
  game ngay), Tu chat with its tick Chi khi chua co to doi, quick quest
  (Nhiem vu), the Discord alerts (Thong bao) with the new Sap het binh and
  Het binh, and on Danh quai: the second potion tier (Lv2), the pet's
  potion (Tu bom mau pet), potion counts (Con N binh), monster lists (Loc
  quai), attack styles (Kieu danh), keeping distance (Giu khoang cach),
  positions from packets (Vi tri tu goi tin), the hunting spot (Cho danh
  quai, with Dat cho nay), the Ban do tab with its hunting area per map,
  dropping to a monster on the floor below (Nhay
  xuong bac duoi: Ctrl + Down, off as shipped, never with Dung yen mot cho,
  never near a gate; a map change after a drop stops hunting and unticks
  it), the jump key (Phim nhay and Thu),
  skills only while fighting (Chi khi dang danh quai), skills paced on
  their casts (Ra chieu theo vong, off as shipped) and closing notices and
  the check-in board (Tu dong thong bao); on Don rac, registering monster
  fragments (Dung manh ghep), using charms once an hour (Dung bua, and the
  button Dung bua ngay),
  buying potions from the shop of the Nguoi Giu San item (Tu mua binh, with
  the trial Thu mua; it TYPES 100 into the game, so try the trial first)
  and the key Phim don ngay. Try one at a time, on a spare character,
  and watch. Bag cleaning (Don rac) has run on the real game; registering
  fragments and using charms have not (a charm costs one each time, so try
  it with one spare charm first). Only one live run, of about 15 minutes, has tried
  any of the rest: hunting with the macro keys, potions with Con N binh,
  Tu chat, the keyword invite (it read the chat; no invite went out) and
  one death with Tu hoi sinh. See KNOWN ISSUES.

  Two ticks among those are worth knowing. Chi danh nhung con trong danh
  sach Danh truoc (under Sua danh sach... on the Loc quai line, off as
  shipped) attacks only the monsters on that list, so nothing at all
  while it is empty. Dung skill tuan tu (in the Ky nang window, opened by
  Chi tiet... on the Tu dung ky nang line; ticked as shipped)
  presses every skill whose cooldown is over, one after the other, rather
  than one at a time; it counts only with Chi khi dang danh quai ticked and
  Tu danh quai on: with the macro alone every skill keeps its own timer.

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

  The pet's potion (Tu bom mau pet, a third potion row): ticked, it asks
  for the key of the pet's potion on the hotbar, and presses it over and
  over while the pet's HP is at or under its mark, every Bom lien tuc, moi
  N ms (300 as shipped; about every 3 seconds before 2026.11.10); HP and MP
  potions always go first. The pet's HP is read from the RED bar in the pet's box
  at the top left of the game, under HP and MP -- the purple bar under it
  is not HP. With no pet out the row says so and presses nothing.

  Vi tri tu goi tin (on Danh quai, next to Giu khoang cach; off as
  shipped) takes the character's and the monsters' places from the game's
  own network traffic rather than from the names on screen, which drop out
  now and then. It needs Npcap. Unticked, GoBot still captures the traffic
  when hunting starts and writes a Positions line to the log every 10
  seconds, comparing it with the screen, which does the fighting. The line
  under BAT DAU says which is used: Vi tri: tu goi tin, or tu man hinh and
  why. Testers: hunt 10 minutes unticked and send the log, then tick it,
  hunt 10 minutes more, watch, and send the log again.

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
  home network address, the game server's address and map numbers (when
  capturing packets) and the chat lines that were said -- never a
  password or the webhook. The *_debug folders (chat_debug, trash_debug,
  revive_debug, quest_debug, potion_debug, keeper_debug, login_debug) are
  screenshots of the game: only on request. login_debug holds the game's
  screen each time Tu dang nhap fails on it (only when nothing else was over
  the game), for when logging back in gets stuck.

UPDATING
  Unzip a new version into a NEW folder under C:\Program Files.
  Copy party.ini (and trash.ini with the trash folder) from the old
  configs folder if wanted -- but a party.ini from a package before
  15/10/2026 has both party switches on, and any
  copied party.ini keeps what was ticked: open To doi and look at both
  switches before BAT DAU. Do not copy hunt.ini: one from a package before
  15/10/2026 may hold another player's character name. Type the name and the
  attack key again on Danh quai; if hunt.ini was copied anyway, check that
  "Ten nhan vat" there shows the tester's own name. A hunt.ini copied anyway
  keeps its own jump key, often Space, the game's attack: Danh quai warns
  ("Phim nhay dang la Space"); GoBot never changes the file. Its [Move]
  part, for walking to saved points, is no longer read (2026.11.16). Set
  the other keys again on the screen rather than copying an older
  macro.ini, which may
  have a hidden key sequence switched on. Never open this version's
  configs folder with an older GoBot: an older build may drop the keys past
  the 12th (the Lv2 potions among them), a save from it turns the pet's
  potion into a plain timed key -- which this version then reads the same
  way -- and it drops the mark that keeps a short buff a buff.
  Copy spots.ini too, to keep the hunting areas drawn on Ban do (from
  2026.11.04c on): no package ships one, so nothing is copied over.
  A macro.ini from before 2026.11.10 has no pet_every_ms: its pet keys are
  then pressed every 300 ms while the pet is under its mark, not on their
  old Every (often 3000); Bom lien tuc, moi on the pet's row changes it.
  An HP or MP potion on a digit (the zip's, 1 and 2) still waits through a
  bag pass and a purchase round in 2026.11.11: move it to a letter or to
  Home, End, PgUp, PgDn, Insert or Delete, in the game and on its row.
  A hunt.ini from before 2026.11.12 may hold drop_min_px: it is no longer
  read, and up_reach and down_reach -- the two bars on Ban do -- alone say
  when a drop or a jump up goes: look at them before BAT DAU.

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
  configs\          the settings, written by the app as they change; the
                    app makes spots.ini there for the Ban do areas
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
  Npcap is optional -- it is there if GoC ever ran, otherwise from
  npcap.com: only Vi tri tu goi tin, Cho danh quai and the Ban do tab use
  it. Without it GoBot reads the screen, and both programs start all the
  same -- but Tu danh quai does not start while Cho danh quai is ticked,
  and Ban do has nothing to draw.

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
$dumpbin = Get-VsDirs |
           ForEach-Object { Get-ChildItem (Join-Path $_ 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue } |
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
        # Both run as administrator, so neither may take a DLL from its own
        # folder: the flag that holds their imports to System32 has to have
        # survived the link. A dumpbin that does not show it only warns.
        $loadFlag = $null
        foreach ($l in (& $dumpbin.FullName /loadconfig $exe.FullName 2>&1)) {
            if ("$l" -match '^\s*([0-9A-Fa-f]+)\s+Dependent Load Flag') { $loadFlag = [Convert]::ToInt32($Matches[1], 16); break }
        }
        if ($null -eq $loadFlag) {
            Warn2 "$($exe.Name): dumpbin /loadconfig shows no Dependent Load Flag -- not checked"
        } elseif (($loadFlag -band 0x800) -eq 0) {
            $problems += "$($exe.Name) runs as administrator and may load a DLL from its own folder -- link it with /DEPENDENTLOADFLAG:0x800 (gobot_require_admin in CMakeLists.txt)"
        }
    }
    Ok "no executable needs anything that is not in the package"
} else {
    Warn2 "dumpbin not found -- could not check what the executables need"
}

# 1b. Nor may they look for OpenCV's parallel plugins. OpenCV loads one by
# its full path from beside the exe, where no load flag reaches, and both run
# as administrator. CMakeLists.txt builds OpenCV without them; this is what
# stops that silently coming back.
foreach ($exe in Get-ChildItem $stage -Filter *.exe) {
    $hay = $latin1.GetString([System.IO.File]::ReadAllBytes($exe.FullName))
    if ($hay.IndexOf('opencv_core_parallel_', [System.StringComparison]::Ordinal) -ge 0) {
        $problems += "$($exe.Name) looks beside itself for OpenCV's parallel plugins and would run one as administrator -- build OpenCV with PARALLEL_ENABLE_PLUGINS off (CMakeLists.txt)"
    }
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
foreach ($stranger in 'TungHaVGOBot.dll','GhostCore.dll','go_client.exe',
                      'gobot_test.exe','gobot_sniff.exe') {
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
    @('trash.ini', 'Trash', 'enabled'),
    @('chat.ini', 'Chat', 'enabled'),
    @('quest.ini', 'Quest', 'enabled'),
    @('alerts.ini', 'Alerts', 'enabled'),
    @('app.ini', 'Crash', 'stall_enabled'),
    @('app.ini', 'Simple', 'close_popups'),
    @('trash.ini', 'Trash', 'use_fragments'),
    @('trash.ini', 'Trash', 'use_charms'),
    @('trash.ini', 'Trash', 'buy_potions'),
    @('macro.ini', 'Macro', 'skills_follow_fight'),
    @('hunt.ini', 'Hunt', 'keep_distance'),
    @('hunt.ini', 'Hunt', 'spot'),
    @('hunt.ini', 'Hunt', 'drop_down')
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
# hit by chance, and the hunter's hop.
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
# The Nguoi Giu San item as one player's bag draws it (Tu mua binh): theirs alone.
if (Test-Path (Join-Path $cfg 'shop')) { $problems += "configs\shop\ is in the package -- somebody's photographed item" }
# The hunting areas one player drew on Ban do, map by map: configs\spots.ini,
# which GoBot makes and writes beside itself, and the spots.ini.new a save
# writes first. Theirs alone, and a new version's folder must not come with
# one to be copied over: the configs are copied from source, where one is a
# mistake.
Get-ChildItem $stage -Recurse -File | Where-Object { $_.Name -like 'spots.ini*' } | ForEach-Object {
    $problems += "$($_.FullName.Substring($stage.Length + 1)) is in the package -- somebody's hunting areas (Ban do)"
}
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
foreach ($d in 'maps', 'trash_debug', 'revive_debug', 'quest_debug', 'chat_debug', 'potion_debug', 'keeper_debug', 'login_debug', 'templates\seen', 'templates\captcha') {
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
# Tu dang nhap clicks the channel rows on the game's own channel panel
# (AutoLogin::kChannelPanel, templates.ini channel_dialog); without it a game
# of a size other than 1024x768 is never clicked there (2026.11.13c), and on
# 1024x768 it falls back to the offsets measured on that one.
$wanted += 'login/1/Img_Dlg_Channel.spr/0.png'
# Tu mua binh finds the Nguoi Giu San item by its catalog icon (buy_opener),
# asks for the two potions' (CleanSettings::kBuyHpId, kBuyMpId) beside it --
# it counts nothing since 2026.11.09 -- and knows its shop, the bag-full
# message and the check-in board by pictures that templates.ini names under
# grabbed/ (checked above).
$wanted += 'spend/hp/8820131.png'
$wanted += 'spend/mana/8810111.png'
$wanted += 'other/8950604.png'
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
