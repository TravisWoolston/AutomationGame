# Walker Cluster Pickup and Drop-Off Implementation

## Overview
This document describes the implementation that allows walkers to pick up clusters from PickUp beacons and deliver them to DropOff beacons.

## How It Works

### 1. **PickUp Beacon Detection**
- On each simulation step, the system scans for Ready PickUp beacons
- When a PickUp beacon becomes Ready (has an item on its tile), it finds the nearest idle walker
- The walker automatically receives a high-priority request to move to the PickUp beacon location
- **Special handling**: PickUp beacon tiles are treated as valid foot placement targets (not occupied) and receive a large scoring bonus (-1000) to make feet prioritize stepping on them

### 2. **Pickup Process**
When a walker's foot reaches a PickUp beacon tile:
1. The foot picks up the item from the tile using `TryTakeItem()`
2. The item is stored in `FFootState.HeldItem`
3. The foot's `bHoldingItem` flag is set to true
4. The foot remains **raised** (stays at `StepHeight`) by staying in the `Moving` state
5. The foot's `CurrentTile` is updated but `TargetTile` remains the same (foot stays at pickup location)
6. The walker receives a new high-priority request to move to the DropOff beacon
7. The walker travels with the foot raised until it gets within `ReachDistance` of the DropOff beacon

### 3. **Travel with Cluster**
While the foot is holding an item:
- The foot stays at `StepHeight` (raised position) throughout travel
- The foot **maintains position at the center of its assigned quadrant**
- Center position calculated as: `BodyHex + QuadrantDirection * (ReachDistance * 0.6)`
- Uses 60% of reach distance to keep foot centered but not too close to body
- As the walker moves and rotates, the holding foot steps to maintain this centered position
- When the walker gets within `ReachDistance` of the DropOff beacon, the foot's `TargetTile` switches to the DropOff location
- In `Raising` state: immediately sets height to `StepHeight` and transitions to `Moving`
- In `Moving` state: maintains `StepHeight`
- Normal walking constraints apply (at least 2 feet must remain planted)

### 4. **Drop-Off Process**
When the holding foot reaches the DropOff beacon tile:
1. The cluster rotation is adjusted to match the DropOff beacon's rotation:
   - For single orbs: Updates `Rot60` directly
   - For clusters: Rotates all orbs by updating `GrabFacing` and applying 60° CCW rotations to each orb's local coordinates (`dQ`, `dR`) and `Rot60`
2. The item is placed on the grid using `TryPutItem()`
3. The foot's `bHoldingItem` flag is cleared
4. The foot transitions to `Lowering` state to plant normally
5. Both PickUp and DropOff beacon states are set to `NotReady`

### 5. **Beacon State Management**
- **Ready**: PickUp beacon has a cluster available for pickup (no walker assigned yet)
- **InProgress**: Walker has been assigned to pick up and deliver the cluster
  - Set when request is assigned to prevent re-assignment
  - Remains InProgress during pickup, travel, and dropoff
  - All beacon checks (foot targeting, pickup logic) look for InProgress state
- **NotReady**: No cluster available or recently delivered

**State Flow**: `NotReady` → `Ready` (when item appears) → `InProgress` (when walker assigned) → `NotReady` (after delivery)

PickUp beacons automatically check their tile each tick and update to `Ready` when a cluster appears (only if not already `InProgress`).

## Key Implementation Details

### Modified Files

1. **WalkerTypes.h**
   - Added `bHoldingItem` and `HeldItem` to `FFootState` struct
   - Added `#include "Sim/OrbTypes.h"` for `FItem` definition

2. **SimWorldHex_StepWalkers.cpp**
   - Added automatic walker assignment to Ready PickUp beacons
   - **Special PickUp beacon tile handling**: PickUp beacon tiles are not treated as occupied obstacles
   - **Scoring bonus**: PickUp beacon tiles receive -1000 score bonus to make them highly attractive
   - **Request completion**: PickUp requests only complete when a foot actually picks up the item (not just when body is in range)
   - Implemented pickup logic in `Moving` state when reaching PickUp beacon
   - Implemented drop-off logic with cluster rotation
   - Modified `Planted` state to handle feet holding items differently
   - Modified `Raising` state to keep held items at full height
   - Added check to prevent walkers from sitting down when holding items
   - Updated re-evaluation logic in `Moving` state to also treat PickUp beacons as valid targets

3. **BasicWalker.h/.cpp**
   - Added `bDFootHoldingItem`, `bLFootHoldingItem`, `bRFootHoldingItem`, `bUFootHoldingItem` properties
   - Added `DFootHeldItem`, `LFootHeldItem`, `RFootHeldItem`, `UFootHeldItem` properties
   - Updated `HandleFixedTickSeconds` to sync held item states from simulation

4. **RequestBeacon.h/.cpp**
   - Added beacon state management in `CheckReadiness()` and `Tick()`
   - PickUp beacons automatically transition to `Ready` when a cluster appears on their tile

### Cluster Rotation Logic
The rotation algorithm ensures clusters are placed with the correct orientation:
```cpp
// Calculate rotation difference in 60° increments
int32 CurrentRot = (int32)GrabFacing % 6;
int32 RotDiff = (BeaconRotation - CurrentRot + 6) % 6;

// For each orb in cluster, rotate RotDiff times by 60° CCW
for each rotation:
    dQ_new = -dR_old
    dR_new = dQ_old + dR_old
    Rot60_new = (Rot60_old + 1) % 6
```

### Walker Selection
When a PickUp beacon becomes Ready:
- System finds all idle walkers (Standing or Sitting, no requests or processing requests, no held items)
- Calculates hex distance from each walker to the beacon
- Assigns the pickup request to the nearest walker
- Request has priority 1000 (high priority)
- **Beacon immediately transitions to InProgress** to prevent re-assignment
- Sitting walkers will automatically stand up when they receive the request

### Visual Representation
**Automatic Visual Attachment (Implemented in C++):**
- When a foot picks up a cluster, it's automatically removed from the grid and attached to the foot socket
- Cluster meshes are spawned as child components of a parent `HeldOrbComp` attached to the foot socket
- Each orb in the cluster is positioned using its `dQ`, `dR` coordinates
- Uses `BondedOrbMesh` and `BondedOrbMaterial` from `UOrbRenderer`
- Orbs maintain correct relative positions as the foot moves
- On drop-off, cluster is detached and placed back on the grid visually

**Blueprint-Accessible Properties:**
- `bDFootHoldingItem`, `bLFootHoldingItem`, `bRFootHoldingItem`, `bUFootHoldingItem` - Boolean flags indicating which foot is holding an item
- `DFootHeldItem`, `LFootHeldItem`, `RFootHeldItem`, `UFootHeldItem` - Full `FItem` data for held clusters
- `DFootAssignedTileWorldLocation` etc. - World positions of foot tiles
- Foot height offsets are synced for IK

**Socket Names Used:**
- `DFoot_Socket` - Down/South foot
- `LFoot_Socket` - Left/West foot  
- `RFoot_Socket` - Right/East foot
- `UFoot_Socket` - Up/North foot

## Usage

### Setting Up Beacons
1. Place a **PickUp** RequestBeacon (set `BeaconRole = PickUp`)
2. Place a **DropOff** RequestBeacon (set `BeaconRole = DropOff`)
3. Set the PickUp beacon's `PairedBeacon` to reference the DropOff beacon
4. Set beacon grid coordinates (`Q`, `R`) to match their tile positions
5. Place a cluster on the PickUp beacon's tile

### Behavior
- Walker automatically moves to PickUp beacon when it becomes Ready
- Picks up cluster with one foot (which foot depends on quadrant/reach)
- Travels to DropOff beacon with foot raised
- Rotates cluster to match DropOff beacon orientation
- Places cluster on DropOff tile
- Returns home after delivery

## Notes
- Walkers can only hold one item per foot at a time
- A walker won't sit down if any foot is holding an item
- Held items don't count as obstacles for other walkers (only the holding foot knows about them)
- Drop-off will wait if the target tile is occupied
- System handles both single orbs and multi-orb clusters
