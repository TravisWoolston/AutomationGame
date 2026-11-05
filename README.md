# Excess Form

*(Repository: `AutomationGame`)*

Excess Form is a work-in-progress automation/factory puzzle game built in Unreal Engine with C++. It combines the puzzle-solving of Opus Magnum with the automated factory building of Factorio. Players build machines on a hex grid: programmable arms run command sequences (grab, drop, rotate, extend, retract, move, wait, reset) to move, bond, and process orbs between input and output tiles. A deterministic fixed-step simulation (20 Hz by default) drives the factory. Request-driven "walker" units fetch and deliver orbs.

## Features in the code

- **Hex-grid simulation** (`Source/AutomationGame/*/Sim/SimWorldHex*`): axial-coordinate hex grid with fixed-step ticking. Covers arms, belts, bonds, clusters, a helix processing mechanic, input/output tiles, predictive collision, safety checks, and save/load.
- **Programmable arms** (`BasicArm`, `ArmTimelineEditorWidget`, `Content/Glyphs/ArmBP`): command tapes, with an in-world timeline widget showing the current command.
- **Walkers** (`BasicWalker`, `SimWorldHex_StepWalkers*`, `RequestBeacon`): request-based movement and pickup/drop-off with procedural foot handling and animation.
- **Glyphs and tiles** (`BondGlyphActor`, `TriggerTile`, `Content/Glyphs`): bonding, trigger tiles, and request beacons.
- **Machine editor** (`Editor/MachineEditor`, `MachineBlueprint`) and a top-down pawn and camera (`HexTopDownPawn`, `Content/TopDown`).
- **Tutorial and test scaffolding** (`TutorialDirector`, `TestLevelDirector`, `ScenarioController`, `GridTestHarness`).
- **Goldberg grid geometry** (`Geometry/GoldbergGrid`).
- **UI** (`Content/UI`): hotbar, command navigation, overlays, and tutorial widgets.

Design notes live in `Source/AutomationGame/*.md` (for example `helix_processing_mechanic.md`, `WALKER_REQUEST_SYSTEM.md` and `ArmTimelineSystem_README.md`).

## Built with

- Unreal Engine **5.6** (`EngineAssociation` in `AutomationGame.uproject`)
- C++ (single runtime module `AutomationGame`) plus Blueprints
- Engine plugins: GameplayStateTree and ModelingToolsEditorMode (editor only)
- Module dependencies include UMG, Slate, ProceduralMeshComponent, AnimationCore, Json and UnrealEd

## Requirements

- Unreal Engine 5.6 (Epic Games Launcher or a source build)
- Windows with Visual Studio 2022 and the "Game development with C++" workload (the project has been built with the VS 2022 toolchain), or JetBrains Rider
- [Git LFS](https://git-lfs.com/) to pull `.uasset` and `.umap` content
- TODO (Travis): confirm any Marketplace/Fab assets that must be installed separately (for example the space skybox collection under `Content/SpaceSkyboxCollection_4`)

## How to build and run

1. Clone the repository and pull the LFS assets:
   ```
   git clone https://github.com/TravisWoolston/AutomationGame.git
   cd AutomationGame
   git lfs pull
   ```
2. Right-click `AutomationGame.uproject` and choose **Generate Visual Studio project files**. In Rider, you can open the `.uproject` directly.
3. Open the generated `AutomationGame.sln`, select the **Development Editor / Win64** configuration, and build.
4. Launch the editor (F5 from the IDE, or open `AutomationGame.uproject`). If prompted to rebuild modules, choose **Yes**.
5. Open the main map and press **Play** (PIE). TODO (Travis): name the startup/demo map to open.

## Project structure

```
AutomationGame.uproject   Project descriptor (UE 5.6)
Config/                   Engine, game, input and editor settings
Content/                  Assets (Blueprints, Glyphs, UI, Materials, Mesh, TopDown, Modes, Actors, ...)
Source/AutomationGame/
  Public/, Private/       C++ by area: Actors, Components, Data, Diagnostics, Editor,
                          Framework, Geometry, Rendering, Sim, UI, Blueprints
  *.md                    Design and system notes
```

## Roadmap

- TODO (Travis): next mechanic or feature
- TODO (Travis): content and levels planned
- TODO (Travis): polish, UX and tutorial work
- TODO (Travis): release or build target

## License

MIT. See [LICENSE](LICENSE).
