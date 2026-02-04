# Helix Processing Mechanic - Design Document

**Game Title**: Excess Form  
**Document Created**: 2026-02-03  
**Status**: Conceptual Design Phase

---

## Thematic Foundation

### Title Meaning
"Excess Form" operates on multiple levels:
1. **Mechanical**: Overproduction of orbs to afford precision in final assembly.
2. **Spiritual**: Reference to form/formless duality—"Beyond the duality of form and formless, absolute and relative, the world of nothingness and the world of somethingness are one thing."
3. **Existential**: The AI (or human) desire to exist as "something" rather than nothing. Human life as a "sentient glitch."

### Core Narrative
The player assists a singularity AI in remembering itself. The target sequence represents a fragmented memory. Each scan reveals glimpses through noise until the true form crystallizes.

---

## Design Influences

### Primary Inspirations
- **Shapez 2**: Continuous belt flow, throughput optimization, factory scaling, zen production loop.
- **Opus Magnum**: Hex grid precision, programmed arms, discrete elegant operations, efficiency metrics.

### The Synthesis
"What if Shapez 2's factories fed into Opus Magnum's puzzles?"

| Layer | DNA Source | Goal |
|-------|------------|------|
| Factory | Shapez 2 | Throughput at scale |
| Assembly | Opus Magnum | Precision per cluster |
| Processing | Synthesis | Information extraction |

### The Core Loop
```
Factory (throughput) → Assembly (precision) → Processing (discovery)
         ↑                                              ↓
         └────────── Feedback (types, voids) ──────────┘
```

### Efficiency Metrics (Opus Magnum-style)
Players could be scored on:
- **Clusters processed** to solve (fewer = better deduction).
- **Voids generated** (fewer = cleaner exploration).
- **Types wasted** (minimizing high-type voids).
- **Ticks to solution** (speed).

---

## Core Mechanic: Helix Processing

### The Target Sequence
- A **fixed, seed-generated structure** unique to each world/playthrough.
- Defines specific **orb types** at each position.
- Defines **bond relationships** between positions.
- Unknown to the player at start; discovered through iterative processing.

### Processing Rules

When a cluster enters the helix for processing:

| Input Orb Condition | Output Result |
|---------------------|---------------|
| Correct type, correct bond topology (any valid rotation) | **Same type** (confirmation) |
| One or more types *below* correct, correct bond topology | **Upgrade +1** (progression source) |
| Any type, incorrect bond topology or position | **Void orb** |

**Key Insight**: Position is defined by **bond relationships**, not grid coordinates. An orb's validity depends on whether its bond pattern matches *any* position in the target sequence (allowing for hex rotation).

### Rotation Handling
- 6-fold hex rotations are valid matches.
- Challenge: Rotation ambiguity could frustrate players.
- Potential solutions:
  - Lock to canonical orientation.
  - Provide rotation hints in output.
  - Require larger cluster submissions to reduce rotational alibi.

---

## Orb Types

### Progression Flow
- **Type-1**: Base orb, source of all others, plentiful.
- **Type-2+**: Obtained by processing Type-(N-1) with correct topology.
- **Void**: Result of incorrect topology. Has special properties.

### Void Orbs
- Created when processing fails (wrong bond topology).
- **Self-destruct when all bonds removed** - enables cleanup via unbond tiles.
- **Storage flexibility**: Can be stored in any typed silo and assigned type when bonded.
- When bonding: Inherits type of **first bonded neighbor**.
- NOT wildcards during processing—still void out if topology is wrong.

---

## Helix Animation Integration

### Proposed Flow
```
[Input Cluster] → [HelixOutput Tiles] → Animation spirals UP
                                              ↓
                              (Processing delay based on cluster size)
                                              ↓
[Output Cluster] ← [HelixInput Tiles] ← Animation spirals DOWN
```

### Visual Storytelling
- **Up**: Cluster ascending to memory core.
- **Down**: Memory responding with transformed result.
- **Delay**: Mechanical purpose (buffer while analyzing full topology) + visual purpose (helix "digests" input).

---

## Storage System

### Per-Type Silos
- Each orb type has dedicated storage with capacity limits.
- Overflow can go to **Void Silo** (universal, converts to Void).

### Void as Storage Token
- Deposit any type → stored as Void.
- Withdraw → get Void orb.
- Void orb inherits type when bonded to typed orb.

### Design Purpose
- Compression for overflow management.
- Flexibility in assembly.
- Trade-off: Lose type specificity until assignment.

---

## Power/Cost System (Tentative)

Considering tile costs for power-supply mechanic:

| Tile Type | Cost |
|-----------|------|
| Belt | 2 |
| Arm | 15 |
| Walker | 50 |
| TriggerTile | 1 |

Note: Game uses tick-based simulation, so "upgrades" in the traditional sense may not apply.

---

## Automation Philosophy

### Stable Factory Design
- Players should build **repeatable sequences** without major redesign.
- Discovery of target reveals incrementally; factory adapts through modular changes.
- Core belt/arm infrastructure remains stable.

### The Loop
1. **Exploration**: Spam Type-1 to map topology.
2. **Refinement**: Use Type-2+ to confirm and upgrade.
3. **Stockpiling**: Produce excess higher types.
4. **Completion**: Assemble final target form.
5. **Prestige** (future): Carry excess to next seed/fragment.

---

## Higher-Type Orb Utility (Ideas for Future)

Why stockpile higher types?

1. **Currency**: Unlock tiles, storage, hints.
2. **Fuel**: Consume to power special tiles.
3. **Score/Prestige**: Meta-progression multiplier.
4. **Multiple Fragments**: Parallel targets requiring diverse types.

---

## Open Questions

1. **Rotation feedback**: How to communicate which rotation matched?
2. **Information density**: How to prevent early-game information starvation?
3. **Void reprocessing**: If Voids inherit type, do they then process as that type?
4. **Fragment system**: Multiple target memories per world? (Long-term goal)
5. **Tile implementation**: Reuse HelixTiles or create new ScanTiles/AnalysisTiles?

---

## Naming Candidates

For the processing tile (if new type created):
- Resonance Tiles
- Recall Tiles
- Imprint Tiles
- Echo Tiles
- Memory Tiles

---

## Summary Statement

> "To remember what you were, you must create more than you need. From the excess, form emerges."

The player produces abundance to afford precision. The AI overproduces memories until the correct one crystallizes. Void orbs are not failures—they're the necessary substrate from which form differentiates.
