# Walker Bone Names Reference

This document contains the bone hierarchy for the Automata_Rig skeletal mesh used by the BasicWalker.

## Bone Hierarchy

```
Automata_Rig
└── Body1
    ├── DHipRotatorAxle38
    │   ├── DHipRotatorGear39
    │   └── DHipAxle41
    │       ├── DHipGear40
    │       └── DThigh42
    │           ├── DKneeGear43
    │           └── DKneeAxle44
    │               └── DCalf45
    │                   ├── DFootGear46
    │                   └── DFootAxle47
    │                       ├── DAnkle48
    │                       └── DFoot49
    │
    ├── LHipRotatorAxle14
    │   ├── LHipRotatorGear15
    │   └── LHipAxle17
    │       ├── LHipGear16
    │       └── LThigh18
    │           ├── LKneeGear19
    │           └── LKneeAxle20
    │               └── LCalf21
    │                   ├── LFootGear22
    │                   └── LFootAxle23
    │                       ├── LAnkle24
    │                       └── LFoot25
    │
    ├── RHipRotatorAxle2
    │   ├── RHipRotatorGear3
    │   └── RHipAxle5
    │       ├── RHipGear4
    │       └── RThigh6
    │           ├── RKneeGear7
    │           └── RKneeAxle8
    │               └── RCalf9
    │                   ├── RFootGear10
    │                   └── RFootAxle11
    │                       ├── RAnkle12
    │                       └── RFoot13
    │
    └── UHipRotatorAxle26
        ├── UHipRotatorGear27
        └── UHipAxle29
            ├── UHipGear28
            └── UThigh30
                ├── UKneeGear31
                └── UKneeAxle32
                    └── UCalf33
                        ├── UFootGear34
                        └── UFootAxle35
                            ├── UAnkle36
                            └── UFoot37
```

## Leg Components

Each leg (D, L, R, U) consists of:

- **HipRotatorAxle**: Rotates the entire leg around the body (Z-axis rotation)
- **HipAxle**: Rotates the leg forward/backward or left/right (X or Y-axis rotation)
- **Thigh**: Upper leg bone
- **KneeAxle**: Bends the leg (X or Y-axis rotation)
- **Calf**: Lower leg bone
- **FootAxle**: Rotates the foot to maintain orientation (X or Y-axis rotation)
- **Ankle**: Ankle bone
- **Foot**: Foot bone (end effector for IK)

## Gear Bones

Each joint has a corresponding "Gear" bone (e.g., `DHipGear40`, `DKneeGear43`) which are likely used for visual meshes or mechanical components.

## Usage in Code

The walker calibration system uses these bone names in `GameManagerWalkerCalibration.cpp`:

- **Body1**: Main body bone
- **HipRotatorAxle**: Used for leg rotation around body
- **HipAxle**: Used for leg forward/backward or left/right rotation
- **KneeAxle**: Used for leg bending
- **FootAxle**: Used for foot orientation
- **Foot**: Used as IK target (via FootAxle world position)

## Axis Conventions

- **L/R legs** (Left/Right): HipAxle, KneeAxle, and FootAxle rotate on **Y-axis**
- **U/D legs** (Up/Down): HipAxle, KneeAxle, and FootAxle rotate on **X-axis**
- **All HipRotatorAxles**: Rotate on **Z-axis** (around body)

