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
- Get off the bike when stopped (offline).
- The camera follows the rider and keeps the view you had while riding (1st or 3rd person).

### Track objects
- **Cones**: place, pick up, carry, push, put down or reset them.
- **Hay bales**: the round bale is a solid wall; the small bale can be pushed and carried. Bikes hit both.
- **FMX ramps** (Beta 3): 9 shapes (kickers, long jumps, landing, quarter pipe, run-in) and 4 colours, placed anywhere from the wheel. They follow the slope of the ground and you ride them like dirt. Limit: a ramp can't go higher than the highest point of the track — the plugin warns you when it does, so you can place it lower.

### Online
- Works on servers with **RAGDOLL** in their name — nothing to set up.
- See other players fall, walk and lift their bikes; players collide with each other.
- Cones, hay bales and ramps are shared between players.
- Players without the plugin are kicked automatically after 60 s.

### Settings
- Menu **P**: keys, language (English, French, Spanish, German, Italian — or Auto, following Windows), rider, helmet and boots.
- Gamepad buttons drawn as **Xbox or PlayStation** symbols (detected automatically, Steam Input included).
- Short **tutorial** on first launch (can be shown again from menu P); optional on-screen key hints.

## Planned

See the [Roadmap](ROADMAP.md) for what's coming in future versions.

## Support

- **Discord:** https://discord.gg/XcEYYrB6CX
- Or open an [issue](https://github.com/avare795/MXRP-ragdoll/issues) describing the problem (plugin version, solo or online, and a clip if possible).

Walking animation: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).
FMX ramps: PROCworks FMX Ramp PACK (PROCworks - Proud Racers Of Canada); landing from the "FMX Ramps" pack; Comp, Super kicker, 120 ft jump, quarter pipe and run-in from "thefluffygecko fmx ramps".
