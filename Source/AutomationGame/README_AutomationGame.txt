AutomationGame — Orb Scaffolding — Starter C++ Scaffolding (No .cs)
=================================================

How to use
----------
1) Drop the `Public/` and `Private/` folders into your **existing module** folder, e.g.:
   `YourProject/Source/AutomationGame/`
   Result:
     YourProject/Source/AutomationGame/Public/...
     YourProject/Source/AutomationGame/Private/...

2) Because these headers use an `AUTOMATIONGAME_API` macro that's *defined empty*, you do **not** need
   to change your module name or add a .Build.cs. Everything compiles within your module.

3) In your level, add an empty Actor and attach `UOrbRenderer` (component) at runtime, or create
   an `AHexFactoryActor` to visualize hex tiles. The sim ticks via `UOrbSimSubsystem` globally.

4) Minimal test:
   - In `UOrbSimSubsystem::Initialize`, add a couple tiles (Input/Output) and an arm with a short tape.
   - Run PIE. The `AHexFactoryActor` will render hexes and `UOrbRenderer` will draw orbs
     (assign a sphere mesh + basic material to see them).

Notes
-----
- Hex grid uses axial coords, pointy-top layout.
- `USimWorldHex` runs a fixed-step deterministic tick (20 Hz default).
- Arm ops wired: Grab/Drop/RotateL/RotateR/Extend/Retract/MovePos/MoveNeg/Wait/Reset (Pivot TODO).
- `StepGlyphs` includes *very* simple Input (spawns BaseA) and Output (consumes) behavior for smoke tests.
