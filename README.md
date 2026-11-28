# MXRP Ragdoll

Ragdoll plugin for **MX Bikes** (beta21e, Steam — game update of 23 November 2026 supported). When you crash, your rider no longer snaps straight back onto the bike: he falls for real, gets up, and can walk back to his bike, lift it and ride again — solo and online.

> This repository only hosts the **releases** (ready-to-install plugin files). The source code is not distributed.

## Download

Get the latest version from the [Releases](https://github.com/avare795/MXRP-ragdoll/releases/latest) page and choose how to install:

- **Automatic (recommended)** — `MXRP_Beta_x.x.zip`: unzip and run the `.exe` installer. It finds MX Bikes by itself, removes old versions and keeps your settings.
- **Manual** — `MXRP_Beta_x.x_manual.zip`: copy the content of its `plugins` folder into the `plugins` folder of MX Bikes (next to `mxbikes.exe`) and remove any old version of the plugin. Step-by-step instructions are in the release notes.

> **Official servers:** before playing on them, move `MXRP_Ragdoll.dlo` out of the game's `plugins` folder. The ON / OFF button is not enough — the game still loads the plugin.

## Features

### Crash and ragdoll
- Physics ragdoll on every crash; the rider rolls on the real track surface.
- Helmet, boots, arms and legs don't go through the ground, walls or track objects.
- Natural get-up: rolls over, gets on all fours, one knee, then stands.

### On foot
- Walk, run and jump back to your bike, lift it and get back on.
- Crouch (Shift or right stick click) — Beta 3.2.
- Get off the bike when stopped.
- The camera follows the rider and keeps the view you had while riding (1st or 3rd person).
- First person: 4-level zoom (mouse wheel, wheel click or D-pad up) and adjustable field of view in menu P — Beta 3.2.

### Track objects
- **Cones**: place, pick up, carry, push, put down or reset them.
- **Hay bales**: the round bale is a solid wall; the small bale can be pushed and carried. Bikes hit both.
- **FMX ramps**: 8 shapes (kickers, long jumps, landing, quarter pipe) and 4 colours, placed anywhere from the wheel. They follow the slope of the ground and become part of the track, like a track's bridges: rideable at any height (Beta 3.1). Their sides and back are solid. Shown in replays.

### Online
- With the button ON, the plugin works everywhere — offline (practice and race) and online. Host with the button ON and **RAGDOLL** is added to your server name.
- On RAGDOLL servers, nothing to set up:
- See other players fall, walk and lift their bikes; players collide with each other.
- Cones, hay bales and ramps are shared between players.
- Players without the plugin are kicked automatically after 60 s.

### Updates
- **Automatic updates** (Beta 3.2): the plugin downloads new versions from this page and asks you on the track whether to install them. Can be turned off, or set to install without asking, in menu P.

### Settings
- Menu **P**: keys, language (English, French, Spanish, German, Italian — or Auto, following Windows), rider, helmet and boots.
- Gamepad buttons drawn as **Xbox or PlayStation** symbols (detected automatically, Steam Input included).
- Short **tutorial** on first launch (can be shown again from menu P); optional on-screen key hints.

## What the plugin does on your PC

Plain facts, so you know what you install:

- **Internet:** at game start the plugin asks GitHub (this page) whether a new version exists. You can turn this off in menu **P**. A downloaded update is installed only if its SHA-256 matches the one GitHub lists for the release.
- **Local network:** with `reseau = 1` (default) it looks for other players' plugins on your local network or VPN (UDP port 47731) to share walking and crashes.
- **Online:** on RAGDOLL servers it sends other plugin users your rider's movements, crashes, your track objects and the plugin version, through the game's server connection or Steam's peer-to-peer networking (which identifies players by their Steam ID). When you host, your plugin forwards these messages between the other players. Every connection can be switched off in menu **P** or with `reseau = 0`.
- **Steam ID:** on RAGDOLL servers the plugin can send its messages through Steam's peer-to-peer networking (no port to open). Steam delivers each message with the sender's Steam ID, the same ID the game already uses to connect you to the server and to the other players. The plugin only sends to players the game is already connected with, and its own messages carry your rider name and a random number that changes at every launch, not your Steam ID. Steam IDs are written only in the plugin's local log file (`Documents\PiBoSo\MX Bikes\MXRP_Ragdoll`), never uploaded anywhere. To avoid it, turn the online part off (menu **P** or `reseau = 0`).
- **Auto-kick:** only when you host a RAGDOLL server, players without the plugin get a 60 s grace period, a 30 s warning, then are kicked (they can come back with the plugin).
- **Game:** the plugin runs inside MX Bikes as a game plugin. It reads and patches the game's memory and code (rider skeleton, camera, physics, online packets) and reads your game profile for your key bindings, without changing it.
- **Keys:** it reads the keyboard with a low-level keyboard hook for its own keys, and presses the game's respawn key for you when you get back on your bike.
- **Files:** the installer only writes its own files (`MXRP_Ragdoll*` in the game's `plugins` folder). It removes older copies of this plugin, including the old name `mxb_ragdoll*.dlo` only when that file really is an old version of this plugin. Settings, logs and replays are kept in `Documents\PiBoSo\MX Bikes\MXRP_Ragdoll`. No registry changes, nothing starts with Windows.
- **Uninstall:** delete `MXRP_Ragdoll*` from the game's `plugins` folder, and the `MXRP_Ragdoll` folder in `Documents\PiBoSo\MX Bikes` if you don't want to keep your settings.

## Planned

See the [Roadmap](ROADMAP.md) for what's coming in future versions.

## Support

- **Discord:** https://discord.gg/XcEYYrB6CX
- Or open an [issue](https://github.com/avare795/MXRP-ragdoll/issues) describing the problem (plugin version, solo or online, and a clip if possible).

Walking animation: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).
FMX ramps: PROCworks FMX Ramp PACK (PROCworks - Proud Racers Of Canada); landing from the "FMX Ramps" pack; Comp, Super kicker, 120 ft jump, and quarter pipe from "thefluffygecko fmx ramps".
Built with MinGW-w64 (GCC); its runtime is included in the plugin.
