# Resident Evil Survivor (PS1) VR

An unofficial mod that lets you play **Resident Evil Survivor for the PlayStation (2000)** in virtual reality on PC. The
program runs the original game code and redraws the world in true stereoscopic 3D: look wherever you like, walk where
you look, aim and shoot with your hand. Story, enemies, damage and rules are the original game's.

> A fan project, not affiliated with or endorsed by Capcom. "Resident Evil" is a trademark of its respective owners.
> This repository does **not** contain the game or any part of it.

## Requirements

- **Your own copy of the game**: Resident Evil Survivor, **American release (SLUS-01087)**, as a `.cue` + `.bin` disc
  image made from your CD. Other releases (Europe, Japan) do not work: the program tells you at startup.
- Windows 10/11 64-bit, a DirectX 11 graphics card.
- A PC VR headset with two motion controllers and an **OpenXR runtime** (SteamVR, or the Meta / Oculus PC app for
  Meta headsets). Tested with **PS VR2** (Sense controllers, through SteamVR). Controller profiles are also set up for
  Meta Quest / Rift (Touch), Valve Index and HTC Vive.

No PlayStation BIOS is needed.

## Installation

1. Download the zip from the [Releases](../../releases) page and extract it to a folder.
2. Copy your disc image (`.cue` and `.bin`) into the same folder.
3. Start your VR runtime, turn on the headset and controllers, then run **`Play in VR.bat`**.

The game's memory card is the file `memcard0.mcd` in the same folder (created on first start).

## Controls

| Controller | Action |
|---|---|
| Head | look anywhere, up and down too: the game draws what you face |
| Left stick | walk (towards where you look; the stick turns the direction of the steps) |
| Left stick click | run, until the stick is let go |
| Weapon hand (right by default) | holds the weapon: the shot goes where the barrel points |
| Trigger | fire |
| Right stick left / right | turn (in the options: snap 30°, snap 45°, smooth or none) |
| Right stick click, held | recentre the view |
| Y (or Menu) | item / map menu (the game's GunCon A + B) |
| Walking around the room | your character follows you, with the game's collisions |

**Menus** (title, item / map menu, memory card): they appear on a virtual screen in front of you. Point the weapon and
pull the trigger as with the original light gun; A and B are the GunCon's A and B buttons (up / down, the documents'
pages). In the item / map menu the left stick moves the selection too, and Y goes back / closes it.

**Grenade gun**: its aiming laser is drawn as the grenades' arc and ends where they land. **Rocket launcher**: the
rocket flies along the laser.

## VR options

Open them with **X** in a menu. They are saved in `vr_options.ini`.

| Option | Choices (default first) |
|---|---|
| Gun hand | right / left |
| Aiming laser | off / on (a ray from the barrel and a dot where the shot lands; the grenade gun's is an arc) |
| Right stick turning | snap 30° / snap 45° / smooth / none |
| Game HUD | on / off (weapon, bullets and condition on a panel low in the view) |
| HUD distance | normal / near / far |
| Headshot visual feedback | off / on (a zombie killed by the critical shot loses its head, with a burst of blood) |

## The weapons in your hand

The weapons are held with the game's own 3D models. Since this program contains nothing of the game, it takes them
from your game while it runs: the first time a weapon's model is on screen (lying where it is found, or turning when
it is picked up), it is rebuilt for VR and kept in `resvr_weapons.bin` next to the program; from then on that weapon
looks like itself. Until then a generic pistol is held. Handgun A has no 3D model in the game: it is a Glock 17
shaped for this program and coloured, at start, with the game's own picture of it read from your disc.
Weapons already in your inventory when you load a memory card save keep the generic look until their model is seen.
The Custom Magnum is made from the Magnum's model with a longer barrel; the Rocket Launcher, which has no model in the
game, is held as a bazooka made for this program.

## Desktop window

While you play, a 16:9 window on the monitor shows a view from your head (for recording videos), with the menus and
the VR options when they are open; closing it closes the game. `RESVR_NO_WINDOW=1` turns it off.

## Building from source

Visual Studio 2022 (C++ desktop) and CMake 3.20+. From the project folder:

```
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Version 1.0
```

builds the release configuration (`-DPSX_PAL=OFF -DRESVR_RELEASE=ON`) and packs `dist\resurvivorvr-1.0.zip`.

## Credits and licence

The program is released under the MIT licence (see `LICENSE`). It uses the Khronos OpenXR loader (Apache 2.0, see
`licenses/OpenXR-loader-LICENSE.txt` in the release zip).
