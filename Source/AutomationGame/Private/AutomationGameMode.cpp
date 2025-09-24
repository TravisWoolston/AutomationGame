#include "AutomationGameMode.h"
#include "UI/AutomationGameHUD.h"
#include "Actors/HexTopDownPawn.h"

AAutomationGameMode::AAutomationGameMode()
{
	// Set default pawn class
	DefaultPawnClass = AHexTopDownPawn::StaticClass();
	
	// Set custom HUD class
	HUDClass = AAutomationGameHUD::StaticClass();
}
