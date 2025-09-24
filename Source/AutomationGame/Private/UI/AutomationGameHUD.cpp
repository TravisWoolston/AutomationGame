#include "UI/AutomationGameHUD.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"

AAutomationGameHUD::AAutomationGameHUD()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AAutomationGameHUD::DrawHUD()
{
	Super::DrawHUD();

	if (bSelectionBoxActive)
	{
		DrawSelectionBox();
	}
}

void AAutomationGameHUD::StartSelectionBox(FVector2D StartPosition)
{
	bSelectionBoxActive = true;
	SelectionStart = StartPosition;
	SelectionEnd = StartPosition;
}

void AAutomationGameHUD::UpdateSelectionBox(FVector2D CurrentPosition)
{
	if (bSelectionBoxActive)
	{
		SelectionEnd = CurrentPosition;
	}
}

void AAutomationGameHUD::EndSelectionBox()
{
	bSelectionBoxActive = false;
}

void AAutomationGameHUD::DrawSelectionBox()
{
	if (!Canvas) return;

	// Calculate the selection box bounds
	float MinX = FMath::Min(SelectionStart.X, SelectionEnd.X);
	float MaxX = FMath::Max(SelectionStart.X, SelectionEnd.X);
	float MinY = FMath::Min(SelectionStart.Y, SelectionEnd.Y);
	float MaxY = FMath::Max(SelectionStart.Y, SelectionEnd.Y);

	float Width = MaxX - MinX;
	float Height = MaxY - MinY;

	// Only draw if the selection box has some size
	if (Width > 1.0f || Height > 1.0f)
	{
		// Draw filled background
		FCanvasTileItem FillItem(FVector2D(MinX, MinY), FVector2D(Width, Height), SelectionBoxFillColor);
		FillItem.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(FillItem);

		// Draw border lines
		FCanvasLineItem TopLine(FVector2D(MinX, MinY), FVector2D(MaxX, MinY));
		TopLine.SetColor(SelectionBoxColor);
		TopLine.LineThickness = SelectionBoxThickness;
		Canvas->DrawItem(TopLine);

		FCanvasLineItem RightLine(FVector2D(MaxX, MinY), FVector2D(MaxX, MaxY));
		RightLine.SetColor(SelectionBoxColor);
		RightLine.LineThickness = SelectionBoxThickness;
		Canvas->DrawItem(RightLine);

		FCanvasLineItem BottomLine(FVector2D(MaxX, MaxY), FVector2D(MinX, MaxY));
		BottomLine.SetColor(SelectionBoxColor);
		BottomLine.LineThickness = SelectionBoxThickness;
		Canvas->DrawItem(BottomLine);

		FCanvasLineItem LeftLine(FVector2D(MinX, MaxY), FVector2D(MinX, MinY));
		LeftLine.SetColor(SelectionBoxColor);
		LeftLine.LineThickness = SelectionBoxThickness;
		Canvas->DrawItem(LeftLine);
	}
}
