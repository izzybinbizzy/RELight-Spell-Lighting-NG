# RELight - Spell Lighting NG - SKSE plugin

Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, see `LICENSE`.

The plugin of RELight - Spell Lighting NG; it ships with the mod and is required by it. RE::Light does the lighting; this
plugin adds what RE::Light cannot do from a config.

## What it does
- **Hand lights** - a light on your hands while you cast, made in memory, with Dynamic Lighting. Spells from mods with no
  patch get one too, in the color of their own light or their element.
- **Held weapon lights** - a staff or weapon this mod lights on the ground carries its light in your hand while drawn
  (made the way ReLight by Truman makes its lights, with his permission).
- **Sprays and bolts lit along their length** - each keeps its own light and wears a row of small lights along it.
- **The lighting is detected automatically** (Community Shaders, ENB or Vanilla) - off Community Shaders the hand lights
  are drawn for the game's own lighting and light the ground too.
- A switch for every installed option and patch, Brightness and Reach, presets, Lights by Item, Fading, the charge gems,
  a budget for big fights, wards (stepping aside for Dynamic Wards), FleshFX colors and VAER Reborn's swirls kept.
- Its effect hooks follow [Light Placer](https://github.com/powerof3/LightPlacer) by powerof3 (GPL-3.0, used with permission).
- **Reticle bars through Reticle Arcs** - with Reticle Arcs installed, each hand's spell light, a bound weapon's time and
  an enchanted weapon's charge are offered to it as add-on bars (its published interface only); no reticle of its own.

## Requires
SKSE, Address Library for SKSE Plugins, RE::Light and SKSE Menu Framework (the settings page and the HUD). In Skyrim VR:
SKSE VR, the VR Address Library and ReLight VR, with the VR build of this plugin (built with `GLOW_VR=1`). Reticle Arcs
is optional.

## Settings
SKSE Menu Framework > RELight - Spell Lighting NG, saved to `Data\SKSE\Plugins\RELightSpellLightingNG.ini`; menu changes apply at
once. Values the menu does not show are in `Data\SKSE\Plugins\RELightSpellLightingNG\RELight - Spell Lighting NG - Advanced
Settings.ini`, read when the game starts. The charge gems' pre-rendered art is `HUD/*.dds`, installed to
`Data\SKSE\Plugins\RELightSpellLightingNG\HUD\` (without it the plugin draws a plain gem).

## Third-party files
`src/SKSEMenuFramework.h` is SKSE Menu Framework's own header by Thiago Kaique, copied unchanged from
[SKSE-Menu-Framework-3-Example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example), MIT licensed
(see `src/SKSEMenuFramework.LICENSE.txt`).

`src/ReticleArcsAPI.h` is Reticle Arcs' add-on interface by Chiktabba, MIT licensed (the notice at its top), copied
unchanged. Reticle Arcs is not required.

`src/HudNGNativeAPI.h` is HUD NG's native API by Ckw25, GPL-3.0-or-later with the Modding Exception (see
`src/HudNGNativeAPI.LICENSE.txt`), copied unchanged. HUD NG is not required.
