# xash3d-anniversary

Half-Life on Linux and the Steam Deck, made to be played with a pad on a TV: a menu built for the controller,
Valve's 25th anniversary pad layout, captions for the game's speech, and a new renderer whose three styles switch
while you play.

It is a fork of [Xash3D FWGS](https://github.com/FWGS/xash3d-fwgs) and runs the Half-Life you own in Steam,
25th anniversary update included. Nothing of the game is here or in the release archives.

> [!IMPORTANT]
> This is an unofficial fork. Report its bugs [here](https://github.com/18woldemar/xash3d-anniversary/issues),
> not to FWGS. Builds come only from this repository's
> [releases page](https://github.com/18woldemar/xash3d-anniversary/releases); a "repack" from anywhere else is
> not ours.

**Status: in development, no release yet:** build it from source (below). Linux x86-64 only for now. Windows comes
next, Android after it.

## What is different from Xash3D FWGS

- **Gamepad menu.** Every page is reachable with the pad: banners, a button legend, save and load lists with
  pictures, a server browser with filters, and options laid out in one column. It is the default on the Steam
  Deck and in Steam Big Picture; on a desktop add `-gamepadui`. Text entry uses the system's on-screen keyboard.
  The mouse-and-keyboard menu stays as it was.
- **Valve's pad layout** from the 25th anniversary update: triggers fire, A jump, B duck, X use, Y reload,
  bumpers pick the previous and next weapon, BACK or the right stick click is the flashlight. START opens the
  menu on a tap and quicksaves when held for half a second.
- **Speech captions** in English and Russian (Audio options), from Fograin's Half-Life Subtitles MOD.
- **New renderer** (`ref_gl3`, OpenGL 3.3, the default) with three render styles, switched on the fly in
  Video options:
  - *Original*: the look of the classic hardware renderer.
  - *Software*: the look of the software renderer, with its water and filtering.
  - *Enhanced*: per-pixel lighting, a projected flashlight with shadows, multisampling.
- **Vulkan renderer** (`ref_vk`, Vulkan 1.1), the same renderer on Vulkan. Experimental: start it with `-ref vk`.
- **HD models** switch in Video options, for the 25th anniversary's high definition pack, applied at the next
  map or load.
- Engine fixes found along the way: model and map validation, SIGTERM handling, mounting of `valve_hd` and
  language folders from a read-only game directory.

## Running a release

There is no release yet; this is how it will work. Until then see [Building from source](#building-from-source).

1. Install Half-Life in Steam.
2. Download `xash3d-anniversary-<version>-linux-amd64.tar.gz` from the
   [releases page](https://github.com/18woldemar/xash3d-anniversary/releases) and unpack it anywhere you can
   write to.
3. Run `./xash3d-anniversary`. It finds Half-Life in your Steam libraries, SD cards included. If it cannot, set
   `XASH3D_RODIR` to the game's folder (the one that holds `valve`).

On the Steam Deck: in desktop mode add `xash3d-anniversary` to Steam as a non-Steam game, then play it from
gaming mode.

Arguments go to the engine, for example:

| Argument | What it does |
|---|---|
| `-gamepadui` / `-nogamepadui` | gamepad menu on or off |
| `-ref gl3` / `vk` / `gl` / `soft` | renderer: new OpenGL 3.3, Vulkan, classic OpenGL, software |
| `-windowed`, `-width 1280 -height 720` | window instead of full screen |
| `-dev 2 -log` | developer messages, written to `engine.log` |

Your saves and settings are written next to the binaries, in `valve/`; the Steam copy of the game is only read.

The archive needs SDL2 from the system (SteamOS and the common desktop distributions have it). `ref_gl3` needs
OpenGL 3.3; if the driver has no 3.3 the engine falls back to the classic renderer.

## Building from source

The engine, the menu and the game code are three repositories:
[xash3d-anniversary](https://github.com/18woldemar/xash3d-anniversary) (this one, with the menu
[mainui-anniversary](https://github.com/18woldemar/mainui-anniversary) as a submodule) and
[hlsdk-anniversary](https://github.com/18woldemar/hlsdk-anniversary). Clone them side by side:

```sh
git clone --recursive https://github.com/18woldemar/xash3d-anniversary
git clone https://github.com/18woldemar/hlsdk-anniversary
```

Build dependencies on Debian or Ubuntu (other distributions have the same packages):

```sh
sudo apt install git build-essential python3 libsdl2-dev libfreetype-dev libopus-dev libbz2-dev \
    libvorbis-dev libopusfile-dev libogg-dev libvulkan-dev glslang-tools
```

`libvulkan-dev` and `glslang-tools` are only for `ref_vk`; without them it is not built.

Build both as 64-bit and install them into one directory:

```sh
for tree in xash3d-anniversary hlsdk-anniversary; do
    (cd $tree && ./waf configure -T release -8 --prefix=/ && ./waf build install --destdir=$HOME/xash-anniversary)
done
cd ~/xash-anniversary
XASH3D_RODIR="$HOME/.local/share/Steam/steamapps/common/Half-Life" ./xash3d -game valve
```

This build is for your own machine. A release archive is built by `tools/package` inside Valve's Steam Runtime 3
SDK, so it runs on the Deck's older glibc. The menu art and the captions in a release are made by `tools/mkmenu`
and `tools/mksubs`; see the notes at the top of each.

Upstream's notes on other platforms, build options and tests are in the
[Xash3D FWGS README](https://github.com/FWGS/xash3d-fwgs#readme).

## Credits and licences

- Xash3D FWGS by FWGS and its contributors, from Xash3D by Unkle Mike. The engine and the menu are under the
  GPL v3 or later; the game code is under the Half-Life SDK licence.
- Speech captions: [Fograin's Half-Life Subtitles MOD](https://github.com/Fograin/hl-subs-mod).
- Pad icons: [Gamepad-Asset-Pack](https://github.com/AL2009man/Gamepad-Asset-Pack) by Al. Lopez, MIT.
- Controller picture: "360 controller.svg" by Alphathon, Wikimedia Commons, CC BY-SA 3.0.
- Where the rest of the menu art comes from: [tools/menu/CREDITS.md](tools/menu/CREDITS.md).

Half-Life is a trademark of Valve Corporation. This project is not affiliated with or endorsed by Valve.
