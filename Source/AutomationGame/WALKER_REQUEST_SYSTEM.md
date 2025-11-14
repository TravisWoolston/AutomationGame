# Walker Request System

## Overview

Walkers now use a request-based command system instead of pre-programmed instructions. Requests are issued to the nearest available walker, which will then fulfill them using a sophisticated walking animation system.

## Key Changes

### 1. Request System

**FWalkerRequest** - Movement requests with priority
- `TargetHex`: Destination hex tile
- `Priority`: Higher priority requests processed first

**FindNearestAvailableWalker()** - Finds closest walker to a location
**RequestWalkerMoveTo()** - Issues a move request to a specific walker

### 2. Walking Animation

Walking is broken down into multiple phases:
- **Sitting/Standing Transitions**: 3 GTM steps
- **Each Step**: 3-6 GTM steps total
  - Raise foot (3 ticks)
  - Move horizontally to target tile (3 ticks)
  - Lower foot (3 ticks)

**Constraints:**
- Maximum 2 feet raised at once
- No adjacent feet raised simultaneously
- Feet assigned to unoccupied tiles only

### 3. Stance States

**EWalkerStance:**
- `Sitting`: Idle at home position
- `StandingUp`: Transitioning to standing (3 ticks)
- `Standing`: Active, can walk
- `SittingDown`: Transitioning to sitting (3 ticks)

### 4. Foot Tracking

**FFootState** - Per-foot state tracking
- `CurrentTile`: Current hex position
- `TargetTile`: Target hex position
- `StepState`: Planted, Raising, Moving, or Lowering
- `StepProgress`: Current progress within step phase (0-3 ticks)
- `HeightOffset`: Visual height offset for raised foot

Each walker has 4 feet: D, L, R, U (Down, Left, Right, Up)

### 5. Obstacle Detection

Feet avoid stepping on:
- Arms (base positions)
- Orbs (on ground or belts)
- Belt tiles
- Held clusters (gripped by arms)

### 6. Body Position

Body position lerps to the center of all four feet (X, Y only):
```cpp
BodyTarget = (DFootWorld + LFootWorld + RFootWorld + UFootWorld) / 4.f
```

### 7. Idle Behavior

When a walker has no requests:
1. Returns to `HomeHex` (starting position)
2. Once at home, sits down (3-tick transition)

## Input Binding

To use walker requests, add an input action in **Project Settings → Input**:

**Action Name:** `RequestWalkerMove`
**Suggested Key:** `W` or Middle Mouse Button

This binding will issue move requests to the nearest walker when you click on a tile.

## Usage

1. **Place a Walker** - Use placement mode (Kind4 cycles between Arm/Walker)
2. **Click a Tile** - Press the `RequestWalkerMove` key while hovering over a tile
3. **Watch It Walk** - The nearest walker will:
   - Stand up (3 ticks)
   - Walk to the target using proper foot sequencing
   - Sit down when idle (3 ticks)

## Parameters

Configurable in `FWalkerState`:
- `StepHeight`: How high feet raise (default: 50.0)
- `StepRaiseTicks`: Ticks for raising phase (default: 3)
- `StepMoveTicks`: Ticks for horizontal movement (default: 3)
- `StepLowerTicks`: Ticks for lowering phase (default: 3)
- `ReachDistance`: Hex tiles each leg can reach (default: 2)

## Legacy Support

Old program-based commands are still supported for backward compatibility with saved scenarios. The walker will process programs only when no requests are active.

## Technical Details

### Adjacent Feet Detection
Feet are considered adjacent based on their quadrant positions:
- D-L (Down-Left)
- L-U (Left-Up)
- U-R (Up-Right)
- R-D (Right-Down)

### Foot Tile Assignment
Feet choose target tiles that:
1. Are within `ReachDistance` hexes
2. Are not occupied by obstacles
3. Are not used by other feet
4. Move the body closer to the target hex

### Visualization
- Body position smoothly lerps to center of feet
- Foot height offsets applied for raised feet
- IK Rig goals updated based on foot positions

## Files Modified

- `Public/Sim/WalkerTypes.h` - Added enums and structs
- `Private/Sim/SimWorldHex_StepWalkers.cpp` - New walking logic
- `Private/Sim/SimWorldHex.cpp` - Added request API
- `Public/Actors/HexTopDownPawn.h` - Added RequestMoveToTile
- `Private/Actors/HexTopDownPawn.cpp` - Implemented request binding
- `Public/Actors/BasicWalker.h` - Added foot tracking
- `Private/Actors/BasicWalker.cpp` - Updated visual sync

