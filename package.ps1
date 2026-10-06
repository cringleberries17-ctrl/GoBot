<#
.SYNOPSIS
    Build and zip a ready-to-run GoBot package.

.DESCRIPTION
    Produces dist\GoBot-<version>-win64.zip containing the executables, the
    config files and a short getting-started note.

    Nothing else is needed on the target machine: the CRT is linked statically,
    and wpcap.dll is delay-loaded so the keyboard macro runs with no Npcap
    installed at all.

.PARAMETER Version
    Version string used in the archive name. Defaults to today's date.

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

# ---------------------------------------------------------------- build
if (-not $SkipBuild) {
    Step "Building Release"

    # A running instance holds its own exe open and the link fails, which is
    # easy to miss: the build reports an error but the stale binary stays put
    # and gets packaged as if it were current.
    Get-Process gobot_gui, gobot_cli, gobot_sniff -ErrorAction SilentlyContinue |
        ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Seconds 2
    $stuck = Get-Process gobot_gui, gobot_cli, gobot_sniff -ErrorAction SilentlyContinue
    if ($stuck) {
        throw "Close these before packaging (they hold their own .exe open): " +
              (($stuck | ForEach-Object { "$($_.Name) [PID $($_.Id)]" }) -join ', ')
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
foreach ($exe in 'gobot_gui.exe','gobot_cli.exe','gobot_sniff.exe','gobot_test.exe') {
    if (-not (Test-Path (Join-Path $rel $exe))) { throw "Missing $exe -- build first" }
}

# Self-tests gate the package. Shipping a build whose own tests fail is not a
# thing to do quietly.
#
# Run from a directory that is not the build output. A test that writes to the
# working directory passes when run from somewhere writable and fails for the
# person who unzips into Program Files, and a gate that only holds on the
# build machine is not a gate.
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

Copy-Item (Join-Path $rel 'gobot_gui.exe')   $stage
Copy-Item (Join-Path $rel 'gobot_cli.exe')   $stage
Copy-Item (Join-Path $rel 'gobot_sniff.exe') $stage
Copy-Item (Join-Path $rel 'gobot_test.exe')  $stage

# Configs come from source, not from build output: the staged copies pick up
# whatever a previous run wrote into them.
Copy-Item (Join-Path $root 'configs\*.ini') (Join-Path $stage 'configs')
Copy-Item (Join-Path $root 'README.md')     $stage

# ---- the things the executables READ ----
#
# The binaries are self-contained; what they read is not. Three sets of files
# live outside the build on the machine this was written on and are silently
# absent on anybody else's, and each takes a feature down with it:
#
#   templates\     every dialog is recognised by matching one of these
#   bitmap_font\   HP/MP figures, the chat and the player list are read with it
#   grabbed\       the invite dialog's footer, the player-list grid and the
#                  button that opens it, cut out of live captures
#
# Both are artwork extracted from the game client. They are included so the
# folder works on a machine that has the game and nothing else.

$tpl = Join-Path $rel 'templates'
if (Test-Path $tpl) {
    Copy-Item $tpl $stage -Recurse

    # grabbed\ holds two different things: pictures the bot needs, cut out of
    # live captures, and the screenshots taken while measuring. Only the first
    # kind belongs in a package.
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
    $seen = Join-Path $stage 'templates\seen'
    if (Test-Path $seen) { Remove-Item -LiteralPath $seen -Recurse -Force }
    Ok "templates staged"
} else {
    Warn2 "no templates\ in the build -- nothing that recognises a dialog will work"
}

# The glyphs. The executable looks beside itself for these before it looks
# anywhere else, which is what makes a copied folder able to read anything.
$font = @("$env:ProgramFiles\ZTeam\GoC\data\bitmap_font",
          "${env:ProgramFiles(x86)}\ZTeam\GoC\data\bitmap_font") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if ($font) {
    Copy-Item $font (Join-Path $stage 'bitmap_font') -Recurse
    Ok "bitmap font staged ($((Get-ChildItem (Join-Path $stage 'bitmap_font') -Recurse -File).Count) glyphs)"
} else {
    Warn2 "no bitmap font found -- HP/MP figures, chat and the player list will be unreadable"
}

# The one file this project reads out of TH_VGO_Bot_Helper's whole install.
#
# That installation is 259 MB and GoBot touches exactly this file: the pictures
# it needs are .NET resources embedded inside it, which is a file read and not
# a thing that runs, so the assembly works on its own with none of its
# siblings. The other 264 files are Tesseract data and runtimes that GoBot
# never calls -- it reads text with its own bitmap-font matcher.
# Not the assembly itself. Its pictures are cut out as PNGs into
# templates\grabbed and named in configs\templates.ini instead.
#
# Shipping the DLL got the whole zip flagged as malware by Google's scanner --
# a third-party game-bot binary is exactly what such a scanner is looking for,
# and no local scan here objected to anything, so it was never going to be
# found by testing on this machine. None of it was needed: GoBot only ever read
# pictures out of it, which is a file read and not a thing that runs.

# The bridge into that helper's own code. Optional, and different from the
# above: this one RUNS its code, so it needs .NET and the full helper install.
# Without them the one feature that uses it says it could not start.
foreach ($f in 'GoBotBridge.dll','GoBotBridge.runtimeconfig.json') {
    $p = Join-Path $rel $f
    if (Test-Path $p) { Copy-Item $p $stage }
}

# Anything a run left behind is not part of the package.
foreach ($junk in 'gobot.log','frame.png','region.png','playerlist.png',
                  'playerlist_picked.png','invite_not_taken.png') {
    $p = Join-Path $stage $junk
    if (Test-Path $p) { Remove-Item -LiteralPath $p -Force }
}

@"
GoBot $Version

IT ASKS FOR ADMINISTRATOR, AND IT HAS TO
  Windows will not let a program send keystrokes to a window belonging
  to a more privileged one, and this game runs elevated. Without that
  privilege GoBot presses keys that go nowhere: the macro runs, the game
  receives nothing, and nothing looks broken.

  So it asks on launch, the same way the original client does. Say yes.
  Packet capture needs it too.

KEYBOARD MACRO
  1. Run gobot_gui.exe as administrator
  2. Macro tab -> pick your game window from the list at the top
  3. Set your keys and how often each repeats
  4. START MACRO, or press F10 while in the game

  Starting either one brings the game to the front for you. Keys only
  ever go to a focused window, and clicking the button is what took
  focus off the game -- so without that it would start and just sit
  there holding. It is a checkbox beside each start button if you would
  rather it did not. Only on the way on: neither will grab the
  foreground back while it is running.

  F10  toggle the macro on/off
  F11  toggle hunting on/off
  F12  panic stop: halts everything and releases every held key

  Both hotkeys work while the game has focus, so you never have to find
  this window to stop it.

  Keys are only ever sent while your chosen game window has focus. Leave
  that on: with it off they go to whatever is in front, which sooner or
  later is a browser or a document.

AUTO-POTION  -- reads HP and MP off the bars on screen
  A key can wait on a vital instead of a timer: set its "When" column to
  "HP below" and its "%" to the level you want to drink at.

  To make those keys work, the app needs to know where the bars are:

  1. HP / MP tab -> START READING
  2. FIND THE BARS, with your HP and MP full
  3. Check the two percentages against what the game shows
  4. Save

  If it picks the wrong thing, the four numbers per bar are editable and
  the measured percentage updates as you change them.

  Nothing is read out of the game's memory and no packets are decoded --
  the numbers are already drawn on screen, so this counts coloured
  pixels. It keeps working with the game behind another window.

  A waiting key with no reading available HOLDS rather than firing, and
  the Macro tab says so when that happens.

RADAR AND HUNTING  -- finds monsters on screen and goes after them
  Every creature wears a dark name box with a red health bar under it.
  Players have a blue mana bar too; monsters do not. That is how GoBot
  tells what to attack from who to leave alone -- no memory reading and
  no packet decoding.

  1. Radar tab -> START WATCHING
  2. Check the boxes in the view: green is a monster, blue is you
  3. Set the attack key and the reach, then START HUNTING (or F11)

  IMPORTANT: keep the GoBot window off the game. The game will not draw
  itself into a bitmap on request, so the picture is lifted off the
  screen, and anything covering the game gets read instead of the game.
  The tab says when this is happening and has a button to move itself.

  The plan view shows where things are relative to your character. The
  game's own minimap is shown beside it, and GoBot recognises the map
  from what it looks like -- type a name once and it comes back.

  Your position is NOT drawn on the game's minimap: it carries no marker
  for the character, and locating the view inside it by matching the
  artwork was tried and does not work.

  To check what it can see without the GUI in the way:
      gobot_cli.exe --see

PACKET-CAPTURE BOT  -- optional, needs Administrator and Npcap, unfinished
  Reads real game state rather than pressing keys on a timer. Hidden by
  default: View -> Advanced panels. Set autostart=true in configs\app.ini
  and run as administrator. See README.md.

AUTO-LOGIN  -- from a cold machine to a character in the world
  Fill in configs\login.ini once. Put the account in with

      gobot_cli.exe --setlogin

  which reads the password with the echo off and stores it encrypted with
  your own Windows account's key -- the file is of no use on another
  machine or to another user.

  Channel and character are plain numbers counting from 1, in the
  Auto-login section of "Watch the session".

  It does not answer captchas and never will. If one appears it stops and
  says so.

KEEP THE CLIENT ALIVE  -- notices a crash and logs back in
  The session watcher asks whether the HUD is there, which is the right
  question for an interrupted session and the wrong one for a dead
  client: a crash, a loading screen and a death all look the same to it.

  This asks the other question -- has the client exited, stopped
  answering, or put up a message box? When it is a box the error code is
  read out of the dialog rather than matched against a picture, so a code
  nobody has seen before still comes out right.

  It gives up after a few restarts. A client that falls over that often
  is not failing at random, and a program that keeps reconnecting to a
  server which is refusing it is indistinguishable from an attack on it.

AUTO PARTY
  Two halves, one switch.

  Somebody invites you: the client puts up a small box at the bottom of
  the screen for about ten seconds, which is long enough for a person and
  not for an unattended run. The inviter's name is read off it and
  checked against your lists before anything is clicked.

  Somebody types the keyword in chat (default "pt"): the list of players
  on the map is opened, their row found, and Req Party pressed.

  Names are read off the screen and so are matched loosely -- the reader
  turns "ExampleWin" into "Exampley in" often enough that an exact match
  is a match that never happens.

WHAT IS IN HERE
  gobot_gui.exe     the app
  gobot_cli.exe     headless bot runner, and every measuring tool
  gobot_sniff.exe   protocol tools
  gobot_test.exe    self-tests -- run it if something seems wrong
  configs\          your settings; edit in the GUI or by hand
  templates\        the game's own dialogs and buttons, used to recognise them
  bitmap_font\      the game's own glyphs, used to read numbers and names

  The last two are artwork extracted from the game client, included so
  this folder works on a machine that has only the game installed.

  GoBotBridge.dll is optional. It calls into TH_VGO_Bot_Helper's own code
  and needs both .NET and that helper installed; without them the one
  feature that uses it says it could not start and nothing else changes.

WORTH KNOWING
  "gobot_cli --party" reports "alone" even while the HUD plainly shows a
  party. That reading comes from the other helper's detector matching its
  own pictures against this build of the game, and it is wrong.

  The character select was measured on an account with one character. The
  geometry and the two clicks are verified; which slot holds which
  character is worth checking once with

      gobot_cli.exe --login --upto character

  which parks the client on that screen so you can count from the left.

NOTHING ELSE TO INSTALL
  No VC++ redistributable: the runtime is linked in.
  No Npcap for the macro: that dependency loads only if you use capture.

  Botting is against GhostOnline's terms of service. Your account, your call.
"@ | Set-Content (Join-Path $stage 'START-HERE.txt') -Encoding utf8

Ok "staged to $stage"

# ---------------------------------------------------------------- prove it
Step "Checking the package can stand on its own"

# Everything below is a thing that was claimed to be true and was not. The
# claims are now checked, and a package that fails one is not produced.
$problems = @()

# 1. No executable may need a DLL that is not in here.
#
# wpcap.dll comes with Npcap, which is a separate installation. A STATIC
# import means Windows refuses to start the process when it is missing, so
# gobot_cli.exe would not launch on a machine without Npcap -- and that is the
# program somebody runs first, to write down their account. It is delay-loaded
# now; this is what stops that silently coming back.
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

# 2. The glyphs, without which three features go quiet.
$glyphs = Join-Path $stage 'bitmap_font'
if (-not (Test-Path $glyphs)) {
    $problems += "no bitmap_font\ -- HP/MP figures, chat and the player list cannot be read"
} else {
    $n = (Get-ChildItem $glyphs -Recurse -File).Count
    if ($n -lt 100) { $problems += "bitmap_font\ has only $n glyphs" } else { Ok "$n glyphs" }
}

# 3. Every picture named in templates.ini as a cut-out has to be there.
$tplIni = Join-Path $stage 'configs\templates.ini'
$grabbed = Join-Path $stage 'templates\grabbed'
Get-Content $tplIni | Where-Object { $_ -match '=\s*grabbed/(\S+)\s*$' } | ForEach-Object {
    $png = $Matches[1] + '.png'
    if (-not (Test-Path (Join-Path $grabbed $png))) {
        $problems += "templates.ini names grabbed/$($Matches[1]) and it is not in the package"
    }
}

# 4. The crossing's pages, by name, resolved out of the package alone.
$loginIni = Join-Path $stage 'configs\login.ini'
$pages = Get-Content $loginIni | Where-Object { $_ -match '^\s*picture\s*=\s*(\S+)' } |
         ForEach-Object { $Matches[1] } | Sort-Object -Unique
foreach ($p in $pages) {
    $named = Get-Content $tplIni | Where-Object { $_ -match "^\s*$([regex]::Escape($p))\s*=" }
    if (-not $named) {
        $problems += "configs\login.ini wants the picture '$p' and configs\templates.ini does not say where it is"
    }
}
if (-not $problems) { Ok "every auto-login page has a picture in the package" }

# 4b. Nothing belonging to another program may be in here.
#
# Shipping TungHaVGOBot.dll is what got the zip flagged as malware. Its
# pictures are cut out as PNGs instead, so if that file ever reappears here it
# is a mistake.
foreach ($stranger in 'TungHaVGOBot.dll','GhostCore.dll','go_client.exe') {
    if (Test-Path (Join-Path $stage $stranger)) {
        $problems += "$stranger is in the package -- it belongs to another program and will get the zip flagged"
    }
}

# 5. Nothing may point at a path on the machine this was built on.
Get-ChildItem (Join-Path $stage 'configs') -Filter *.ini | ForEach-Object {
    $hits = Select-String -Path $_.FullName -Pattern 'C:\\Users\\' -SimpleMatch
    foreach ($h in $hits) { $problems += "$($_.Name):$($h.LineNumber) points at this machine: $($h.Line.Trim())" }
}

if ($problems) {
    Write-Host ""
    $problems | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    throw "The package is not self-contained -- not shipping it."
}
Ok "self-contained"

# ---------------------------------------------------------------- zip
Step "Zipping"
$zip = Join-Path $dist "GoBot-$Version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

$size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Ok "$zip  ($size MB)"

Step "Contents"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
$archive.Entries | Sort-Object FullName | ForEach-Object {
    "    {0,-34} {1,9:N0} bytes" -f $_.FullName, $_.Length
}
$archive.Dispose()

Write-Host "`nReady: $zip" -ForegroundColor Green


