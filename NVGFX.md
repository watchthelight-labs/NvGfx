# NvGfx

NvGfx is watchthelight's fork of [NullMoth's NVIDIA Metal driver for macOS](https://github.com/nullmoth/nvidia-macos-driver), kept for one machine: a Lenovo Legion 5 15IAX10 (RTX 5070 Laptop GPU, 10DE:2D18, AMOLED panel driven by the NVIDIA GPU) running macOS 15.8.1 through OpenCore. The name follows the same scheme as our MediaTek drivers (MtkWlan, MtkBt).

## Credit

All of the driver is NullMoth's work unless a commit on the `nvgfx` branch says otherwise. The full upstream history is kept here unchanged.

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)

It is distributed under the PolyForm Noncommercial License 1.0.0 (see `LICENSE`, https://polyformproject.org/licenses/noncommercial/1.0.0). Use it for noncommercial purposes only. Third-party parts keep their own terms, listed in `NOTICE`:

- NVIDIA open GPU kernel modules (MIT / GPL-2.0), which the kexts build against
- Mesa NVK (MIT), changed by NullMoth through patches
- metal2vulkan by Anees Iqbal (LGPL-3.0 or later), the base of `translator/`
- NVIDIA GSP firmware, under NVIDIA's firmware licence

## Branches

- `main`: upstream NullMoth, mirrored as is.
- `nvgfx`: our changes on top of an upstream tag.

## Why the fork exists

1. NullMoth 2.1.0 draws new content over old content in Terminal without clearing it (text layers stack; project-legion issue I-048). 2.0.2 does not show it. The goal is to find and fix the cause and to send the fix upstream as well.
2. Window drags run at about 54 frames per second with page flips on (78 in copy mode) on a 165 Hz panel. The goal is smooth motion without the artifacts copy mode brings.

Bug reports and fixes that apply to everyone go to NullMoth first.
