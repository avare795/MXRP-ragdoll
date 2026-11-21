# MXRP Ragdoll

Ragdoll plugin for **MX Bikes** (beta21e, Steam). When you crash, your rider no longer snaps straight back onto the bike: he falls for real, gets up, and can walk back to his bike, lift it and ride again — solo and online.

> This repository only hosts the **releases** (ready-to-install plugin files). The source code is not distributed.

## Download

Get the latest version from the [Releases](https://github.com/avare795/MXRP-ragdoll/releases/latest) page: download `MXRP_Beta_x.x.zip`, unzip it and run the `.exe` installer. It finds MX Bikes by itself and keeps your settings.

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

### Online
- Works on servers with **RAGDOLL** in their name — nothing to set up.
- See other players fall, walk and lift their bikes; players collide with each other.
- Cones and hay bales are shared between players.
- Players without the plugin are kicked automatically after 60 s.

### Settings
- Menu **P**: keys, language (English, French, Spanish, German, Italian), rider, helmet and boots.

## Planned

These features are planned for later versions:

- **VR view while walking** — play the on-foot part in VR.
- **Refuelling system** — refuel your bike during sessions.

## Support

- **Discord:** https://discord.gg/XcEYYrB6CX
- Or open an [issue](https://github.com/avare795/MXRP-ragdoll/issues) describing the problem (plugin version, solo or online, and a clip if possible).

Walking animation: CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu).
