# Arm Timeline Widget System

This system provides 3D widgets that display command timelines for arms, showing their command sequence with highlighting for the currently executing command.

## Components Created

### 1. UArmTimelineWidget
- **Location**: `Public/UI/ArmTimelineWidget.h`, `Private/UI/ArmTimelineWidget.cpp`
- **Purpose**: 3D Widget component that displays arm command timelines
- **Features**:
  - Shows command sequence as icons
  - Highlights current command being executed
  - Supports command textures for visual representation
  - Configurable scale, offset, and visibility

### 2. UArmTimelineWidgetBlueprint
- **Location**: `Public/UI/ArmTimelineWidgetBlueprint.h`, `Private/UI/ArmTimelineWidgetBlueprint.cpp`
- **Purpose**: Blueprint-friendly widget class for timeline display
- **Features**:
  - Blueprint events for UI updates
  - Command texture management
  - Color customization for different command states

### 3. UArmTimelineUMGWidget
- **Location**: `Public/UI/ArmTimelineUMGWidget.h`, `Private/UI/ArmTimelineUMGWidget.cpp`
- **Purpose**: UMG widget for detailed timeline display
- **Features**:
  - Horizontal layout of command icons
  - Text display for current command and progress
  - Bindable widgets for easy Blueprint integration

## Integration Points

### GameManager Integration
The `AGameManager` class has been extended with timeline system support:

```cpp
// Command texture management
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
TMap<EArmCmd, UTexture2D*> CommandTextures;

// Timeline visibility control
UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
void SetTimelineVisibility(bool bVisible);

// Update timeline for specific arm
UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
void UpdateArmTimeline(ABasicArm* Arm, int32 CommandIndex, float Progress);
```

### BasicArm Integration
The `ABasicArm` class now includes timeline widget support:

```cpp
// Timeline widget component
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timeline")
UArmTimelineWidget* TimelineWidget = nullptr;

// Timeline control functions
UFUNCTION(BlueprintCallable, Category = "Timeline")
void InitializeTimelineWidget();

UFUNCTION(BlueprintCallable, Category = "Timeline")
void UpdateTimelineWidget();
```

## Usage Instructions

### 1. Setting Up Command Textures
1. Create texture assets for each command type (Grab, Drop, Rotate, etc.)
2. In the GameManager, assign textures to the `CommandTextures` map
3. Set a `DefaultCommandTexture` for unknown commands

### 2. Creating Timeline Widgets
1. Create a Blueprint widget class based on `UArmTimelineUMGWidget`
2. Design the UI layout with:
   - `CommandIconsBox` (HorizontalBox) for command icons
3. Implement the Blueprint events:
   - `OnWidgetConstructed`
   - `OnProgramChanged`
   - `OnCurrentCommandChanged`
   - `OnCommandTexturesUpdated`

### 3. Configuring Arms
1. Each arm automatically gets a timeline widget component
2. **Timeline settings are controlled by GameManager, not individual arms**
3. Individual arm timeline properties are private and read-only
4. Use GameManager's "Arm Timeline" category for all configuration

### 4. Runtime Control
```cpp
// Show/hide timelines globally
GameManager->SetTimelineVisibility(false);

// Update specific arm timeline
GameManager->UpdateArmTimeline(Arm, CommandIndex, Progress);

// Control individual arm timeline
Arm->SetTimelineVisible(false);
```

## ⚠️ Important Configuration Notes

### Timeline Settings Control
- **GameManager**: Controls ALL timeline settings globally
- **Individual Arms**: Timeline properties are private and read-only
- **Blueprint Access**: Use GameManager's "Arm Timeline" category for configuration

### What Changed
- Arm timeline properties (`TimelineWidget`, `bShowTimeline`) are now **completely private**
- These properties are **not visible in the editor** and controlled by GameManager only
- This prevents confusion and ensures consistent behavior across all arms

### How to Configure
1. **Open GameManager Blueprint**
2. **Navigate to "Arm Timeline" category**
3. **Configure all timeline settings there**
4. **Do NOT modify timeline settings in individual arm blueprints**

## Command Types Supported

The system supports all arm command types:
- `NoOp` - No operation
- `Grab` - Grab an item
- `Drop` - Drop an item
- `RotateCW` - Rotate clockwise
- `RotateCCW` - Rotate counter-clockwise
- `Wait` - Wait for specified ticks
- `Reset` - Reset to starting position
- `Extend` - Extend arm reach
- `Retract` - Retract arm reach
- `RotateHeldCW` - Rotate held item clockwise
- `RotateHeldCCW` - Rotate held item counter-clockwise
- `WaitForBond` - Wait for bond formation

## Customization Options

### Visual Customization
- Command textures for each command type
- Colors for current, normal, and completed commands
- Timeline scale and positioning
- Maximum number of displayed commands

### Blueprint Integration
- Blueprint events for UI updates
- Customizable command icon creation
- Flexible layout system

## Technical Details

### Performance Considerations
- Timeline widgets are only updated when needed
- Command textures are cached for efficiency
- Widget visibility can be toggled to reduce overhead

### Memory Management
- Timeline widgets are automatically cleaned up
- Command icon widgets are managed properly
- No memory leaks in widget lifecycle

## Future Enhancements

Potential improvements could include:
- Animation for command transitions
- Sound effects for command execution
- More detailed progress indicators
- Command history tracking
- Timeline scrubbing for debugging
