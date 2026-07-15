# 🎮 Featured Native Port — Uncharted 3: Drake's Deception (BCES01175)

> **Project Status:** 🚧 Active Development

## About the Project

**Uncharted 3: Drake's Deception** is the flagship project built using the **ps3recomp** framework.

The goal is to transform the original PlayStation 3 executable into a fully native desktop application without relying on runtime emulation. Instead of interpreting the Cell processor instructions at runtime, the game's PPU and SPU code is statically recompiled into native C/C++ and linked against the ps3recomp runtime.

This project demonstrates the capabilities of static recompilation on one of the most technically demanding PlayStation 3 games ever released.

## Current Progress

The project has already reached several important milestones:

- ✅ Native PPU recompilation
- ✅ SPU recompilation and runtime integration
- ✅ Runtime, threading and virtual memory operational
- ✅ File system, archives and asset loading
- ✅ Audio subsystem initialization
- ✅ SPURS initialization
- ✅ RSX command processing
- ✅ Native Direct3D 12 rendering backend
- 🔄 Graphics pipeline completion
- 🔄 SPU task scheduling and rendering synchronization

### Status Summary (current focus)

Boot, runtime, filesystem/asset loading, SPURS initialization and the native
Direct3D 12 render pipeline are operational (the loading screen renders
correctly). The active frontier is **Phase 11 — SPU Handling** of the porting
guide: the SPU/SPURS asset-decode **completion** path.

Concretely, the front-end **main menu is not yet reached**. It is gated on the
render state-machine marking each resource *decode-ready* once its SPU-decoded
geometry lands, and on the menu-scene transition trigger. The deterministic SPU
executor and the lifted geometry decode are validated live end-to-end; the
remaining work is wiring the decode/residence **completion** back into the
render-readiness chain and the menu-scene construction. This is the documented
"hard part" for a complex AAA title.

**Next milestone:** a functional, rendered main menu (Phase 12).

## Future Goals

After the game reaches a fully playable state, future work will include:

- Native keyboard and mouse support
- Controller improvements
- High and unlocked frame rate support
- Performance optimization
- Modern multiplayer support using contemporary networking technologies
- Modding tools and debugging utilities

## Vision

The long-term objective is to preserve **Uncharted 3** as a native PC application while showcasing the capabilities of the **ps3recomp** framework.

The knowledge and improvements gained during this project will benefit future PlayStation 3 recompilation efforts.

## Legal Notice

This repository does **not** include any copyrighted Sony Interactive Entertainment or Naughty Dog assets.

Users must provide their own legally obtained PlayStation 3 game files.
Only clean-room tooling, runtime libraries and original source code are distributed.