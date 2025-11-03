#include "UI/BlueprintLibraryDisplayWidget.h"
#include "Actors/HexTopDownPawn.h"
#include "Blueprints/PlayerBlueprintLibrarySave.h"
#include "EngineUtils.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Engine/Texture2D.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Data/OrbMaterialManager.h"

UBlueprintLibraryDisplayWidget::UBlueprintLibraryDisplayWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Pawn = nullptr;
	LastKnownBlueprintCount = 0;
	RefreshInterval = 0.5f;
	TimeSinceLastRefresh = 0.0f;
	BlueprintIconScale = 1.0f;
}

void UBlueprintLibraryDisplayWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Try to auto-find HexTopDownPawn
	if (!Pawn)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AHexTopDownPawn> It(World); It; ++It)
			{
				Pawn = *It;
				break;
			}
		}
	}

	RefreshDisplay();
}

void UBlueprintLibraryDisplayWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Auto-refresh at interval if needed
	TimeSinceLastRefresh += InDeltaTime;
	if (TimeSinceLastRefresh >= RefreshInterval)
	{
		TimeSinceLastRefresh = 0.0f;
		if (Pawn)
		{
			TArray<FBlueprintData> Blueprints = Pawn->GetSavedBlueprints();
			if (Blueprints.Num() != LastKnownBlueprintCount)
			{
				RefreshDisplay();
			}
		}
	}
}

void UBlueprintLibraryDisplayWidget::RefreshDisplay()
{
	if (!Pawn || !BlueprintEntriesBox)
	{
		return;
	}

	// Clear existing entries and button arrays
	BlueprintEntriesBox->ClearChildren();
	BlueprintIconButtons.Empty();
	BlueprintDeleteButtons.Empty();
	ButtonToBlueprintIndex.Empty();
	
	// Ensure arrays are large enough
	BlueprintIconButtons.SetNum(LastKnownBlueprintCount);
	BlueprintDeleteButtons.SetNum(LastKnownBlueprintCount);

	// Get blueprints from pawn
	TArray<FBlueprintData> BlueprintDataArray = Pawn->GetSavedBlueprints();
	LastKnownBlueprintCount = BlueprintDataArray.Num();

	// Create a row for each blueprint
	for (int32 i = 0; i < BlueprintDataArray.Num(); i++)
	{
		const TArray<FBlueprintTile>& Blueprint = BlueprintDataArray[i].Tiles;

		// Create a horizontal box for this blueprint entry
		UHorizontalBox* BlueprintRow = NewObject<UHorizontalBox>(this);
		if (!BlueprintRow)
		{
			continue;
		}
		
		// Ensure the horizontal box has no background
		BlueprintRow->SetVisibility(ESlateVisibility::Visible);

		// Get or generate the icon for this blueprint
		UTexture2D* BlueprintIcon = nullptr;
		if (UTexture2D** CachedIcon = BlueprintIcons.Find(i))
		{
			BlueprintIcon = *CachedIcon;
		}
		else
		{
			BlueprintIcon = GenerateBlueprintIcon(Blueprint);
			if (BlueprintIcon)
			{
				BlueprintIcons.Add(i, BlueprintIcon);
			}
		}

		// Create an image widget for the icon if available
		if (BlueprintIcon)
		{
			UImage* IconImage = NewObject<UImage>(this);
			if (IconImage)
			{
				// Set the texture
				IconImage->SetBrushFromTexture(BlueprintIcon);
				
				// Set the brush size and ensure transparency is preserved
				FSlateBrush MyBrush = IconImage->GetBrush();
				const float BaseDisplaySize = 150.0f;
				float DisplaySize = BaseDisplaySize * BlueprintIconScale;
				MyBrush.SetImageSize(FVector2D(DisplaySize, DisplaySize));
				MyBrush.DrawAs = ESlateBrushDrawType::Image;
				MyBrush.Tiling = ESlateBrushTileType::NoTile;
				MyBrush.TintColor = FSlateColor(FLinearColor::White); // White tint preserves original colors
				IconImage->SetBrush(MyBrush);
				IconImage->SetDesiredSizeOverride(FVector2D(DisplaySize, DisplaySize));
				
				// Ensure the image widget supports transparency with full opacity
				IconImage->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
				
				// Make image clickable by wrapping in a completely transparent button
				UButton* IconButton = NewObject<UButton>(this);
				if (IconButton)
				{
					// Create completely transparent/no-draw brushes for all button states
					FSlateBrush NoDrawBrush;
					NoDrawBrush.DrawAs = ESlateBrushDrawType::NoDrawType; // Don't draw anything
					
					FButtonStyle ButtonStyle;
					ButtonStyle.Normal = NoDrawBrush;
					ButtonStyle.Hovered = NoDrawBrush;
					ButtonStyle.Pressed = NoDrawBrush;
					ButtonStyle.Disabled = NoDrawBrush;
					ButtonStyle.NormalPadding = FMargin(0.0f);
					ButtonStyle.PressedPadding = FMargin(0.0f);
					
					IconButton->SetStyle(ButtonStyle);
					
					// Set the button's background color to fully transparent
					IconButton->SetBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
					
					// Store button in array
					if (BlueprintIconButtons.Num() <= i)
					{
						BlueprintIconButtons.SetNum(i + 1);
					}
					BlueprintIconButtons[i] = IconButton;
					
					// Map button to blueprint index for click handling (backup)
					ButtonToBlueprintIndex.Add(IconButton, i);
					
					// Add click handler - will use button map to identify which button was clicked
					IconButton->OnClicked.AddDynamic(this, &UBlueprintLibraryDisplayWidget::HandleBlueprintIconClicked);
					
					IconButton->AddChild(IconImage);
					
					// Add button to the row
					UHorizontalBoxSlot* BoxSlot = BlueprintRow->AddChildToHorizontalBox(IconButton);
					if (BoxSlot)
					{
						BoxSlot->SetHorizontalAlignment(HAlign_Center);
						BoxSlot->SetVerticalAlignment(VAlign_Center);
					}
				}
				else
				{
					// Fallback: add image directly if button creation fails
					UHorizontalBoxSlot* BoxSlot = BlueprintRow->AddChildToHorizontalBox(IconImage);
					if (BoxSlot)
					{
						BoxSlot->SetHorizontalAlignment(HAlign_Center);
						BoxSlot->SetVerticalAlignment(VAlign_Center);
					}
				}
			}
		}

		// Create a delete button
		UButton* DeleteButton = NewObject<UButton>(this);
		if (DeleteButton)
		{
			// Create a simple text label for the delete button
			UTextBlock* DeleteText = NewObject<UTextBlock>(this);
			if (DeleteText)
			{
				DeleteText->SetText(FText::FromString(TEXT("X")));
				DeleteText->SetColorAndOpacity(FSlateColor(FLinearColor::Red));
				FSlateFontInfo FontInfo = DeleteText->GetFont();
				FontInfo.Size = 16;
				DeleteText->SetFont(FontInfo);
			}

			DeleteButton->AddChild(DeleteText);
			
			// Store button in array
			if (BlueprintDeleteButtons.Num() <= i)
			{
				BlueprintDeleteButtons.SetNum(i + 1);
			}
			BlueprintDeleteButtons[i] = DeleteButton;
			
			// Map button to blueprint index for click handling (backup)
			ButtonToBlueprintIndex.Add(DeleteButton, i);
			
			// Add click handler - will use button map to identify which button was clicked
			DeleteButton->OnClicked.AddDynamic(this, &UBlueprintLibraryDisplayWidget::HandleDeleteBlueprintClicked);

			// Add delete button to the row
			UHorizontalBoxSlot* BoxSlot = BlueprintRow->AddChildToHorizontalBox(DeleteButton);
			if (BoxSlot)
			{
				BoxSlot->SetHorizontalAlignment(HAlign_Center);
				BoxSlot->SetVerticalAlignment(VAlign_Center);
			}
		}

		// Add the row to the vertical box
		BlueprintEntriesBox->AddChildToVerticalBox(BlueprintRow);
	}
}

void UBlueprintLibraryDisplayWidget::SetPawn(AHexTopDownPawn* InPawn)
{
	Pawn = InPawn;
	RefreshDisplay();
}

void UBlueprintLibraryDisplayWidget::HandleBlueprintIconClicked()
{
	// Find which button was clicked by checking which one is currently hovered
	// Since OnClicked doesn't provide the sender, we use hover state as a workaround
	UButton* ClickedButton = nullptr;
	
	// Try to find the button that was clicked by checking hover state
	// This works because the button is still hovered when the click event fires
	for (UButton* Button : BlueprintIconButtons)
	{
		if (Button && Button->IsHovered())
		{
			ClickedButton = Button;
			break;
		}
	}
	
	// Look up the blueprint index from our map
	if (ClickedButton && ButtonToBlueprintIndex.Contains(ClickedButton))
	{
		int32 BlueprintIndex = ButtonToBlueprintIndex[ClickedButton];
		if (!Pawn)
		{
			return;
		}
		Pawn->LoadBlueprintFromLibrary(BlueprintIndex);
		UE_LOG(LogTemp, Log, TEXT("BlueprintLibraryDisplayWidget: Clicked blueprint at index %d"), BlueprintIndex);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleBlueprintIconClicked: Could not identify clicked button"));
	}
}

void UBlueprintLibraryDisplayWidget::HandleDeleteBlueprintClicked()
{
	// Find which button was clicked by checking which one is currently hovered
	UButton* ClickedButton = nullptr;
	
	// Try to find the button that was clicked by checking hover state
	for (UButton* Button : BlueprintDeleteButtons)
	{
		if (Button && Button->IsHovered())
		{
			ClickedButton = Button;
			break;
		}
	}
	
	// Look up the blueprint index from our map
	if (ClickedButton && ButtonToBlueprintIndex.Contains(ClickedButton))
	{
		int32 BlueprintIndex = ButtonToBlueprintIndex[ClickedButton];
		if (!Pawn)
		{
			return;
		}
		Pawn->RemoveBlueprintFromLibrary(BlueprintIndex);
		BlueprintIcons.Remove(BlueprintIndex);
		RefreshDisplay();
		UE_LOG(LogTemp, Log, TEXT("BlueprintLibraryDisplayWidget: Deleted blueprint at index %d"), BlueprintIndex);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleDeleteBlueprintClicked: Could not identify clicked button"));
	}
}

UTexture2D* UBlueprintLibraryDisplayWidget::GenerateBlueprintIcon(const TArray<FBlueprintTile>& Blueprint)
{
	if (Blueprint.Num() == 0)
	{
		return nullptr;
	}

	// Create a 256x256 texture
	const int32 IconSize = 256;
	TArray<FColor> Pixels;
	Pixels.SetNum(IconSize * IconSize);

	// Fill with transparent background
	// FColor uses RGBA format where A=0 means fully transparent
	for (FColor& Pixel : Pixels)
	{
		Pixel.R = 0;
		Pixel.G = 0;
		Pixel.B = 0;
		Pixel.A = 0; // Fully transparent
	}

	// Get OrbMaterialManager for colors
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);

	// Calculate bounds to fit all tiles
	int32 MinQ = INT_MAX, MaxQ = INT_MIN;
	int32 MinR = INT_MAX, MaxR = INT_MIN;
	for (const FBlueprintTile& Tile : Blueprint)
	{
		MinQ = FMath::Min(MinQ, Tile.LocalHex.Q);
		MaxQ = FMath::Max(MaxQ, Tile.LocalHex.Q);
		MinR = FMath::Min(MinR, Tile.LocalHex.R);
		MaxR = FMath::Max(MaxR, Tile.LocalHex.R);
	}

	// Calculate the center of the blueprint for proper centering
	float CenterQ = (MinQ + MaxQ) * 0.5f;
	float CenterR = (MinR + MaxR) * 0.5f;

	// Calculate scale to fit everything
	int32 HexWidth = MaxQ - MinQ + 1;
	int32 HexHeight = MaxR - MinR + 1;

	const float BaseTileRadius = 18.0f;
	const float TileRadius = BaseTileRadius * BlueprintIconScale;
	const float IconPadding = TileRadius;

	// Calculate scale with proper padding
	const float ScaleX = (IconSize - IconPadding * 2) / (HexWidth * 1.732f);
	const float ScaleY = (IconSize - IconPadding * 2) / (HexHeight * 1.5f);
	const float Scale = FMath::Min(ScaleX, ScaleY);

	// Draw each tile
	for (const FBlueprintTile& Tile : Blueprint)
	{
		// Convert hex coordinates to pixel coordinates (pointy-top hex)
		float TileCenterX = ((Tile.LocalHex.Q - CenterQ) * 1.732f + (Tile.LocalHex.R - CenterR) * 0.866f) * Scale;
		float TileCenterY = (Tile.LocalHex.R - CenterR) * 1.5f * Scale;

		// Center in the icon
		TileCenterX += IconSize * 0.5f;
		TileCenterY += IconSize * 0.5f;

		// Get color based on tile type
		FLinearColor TileColor = FLinearColor::Gray;
		if (Tile.bIsArm)
		{
			TileColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f); // Cyan equivalent
		}
		else
		{
			switch (Tile.TileType)
			{
			case ETileKind::Input:
				TileColor = FLinearColor::Green;
				break;
			case ETileKind::Output:
			case ETileKind::ClusterOutput:
				if (MaterialManager)
				{
					TileColor = MaterialManager->GetOrbTypeUIColor((EOrbType)Tile.TileDirection);
				}
				else
				{
					TileColor = FLinearColor::Red;
				}
				break;
			case ETileKind::Belt:
				TileColor = FLinearColor::Yellow;
				break;
			case ETileKind::Bond:
				TileColor = FLinearColor::Blue;
				break;
			case ETileKind::Unbond:
				TileColor = FLinearColor(1.0f, 0.0f, 1.0f, 1.0f); // Magenta equivalent
				break;
			default:
				TileColor = FLinearColor::Gray;
				break;
			}
		}

		// Convert to FColor with alpha preserved (use false for linear, true for sRGB)
		// Use false to preserve alpha better
		FColor PixelColor = TileColor.ToFColor(false);
		
		// Ensure alpha is fully opaque for the tile itself (not the background)
		PixelColor.A = 255; // Fully opaque tiles

		// Draw a filled hexagon (simplified as a circle for now)
		for (int32 Y = 0; Y < IconSize; Y++)
		{
			for (int32 X = 0; X < IconSize; X++)
			{
				float Dx = X - TileCenterX;
				float Dy = Y - TileCenterY;
				float Distance = FMath::Sqrt(Dx * Dx + Dy * Dy);

				if (Distance <= TileRadius)
				{
					int32 Index = Y * IconSize + X;
					if (Index >= 0 && Index < Pixels.Num())
					{
						Pixels[Index] = PixelColor;
					}
				}
			}
		}
	}

	// Create texture from pixel data with alpha channel support
	UTexture2D* Texture = UTexture2D::CreateTransient(IconSize, IconSize, PF_B8G8R8A8);
	if (Texture)
	{
		// Configure texture to preserve alpha channel - use uncompressed format for best alpha preservation
		Texture->CompressionSettings = TC_EditorIcon; // Best for UI icons with alpha
		Texture->SRGB = false; // Disable SRGB for better alpha handling
		Texture->CompressionNoAlpha = false; // Ensure alpha is not compressed away
		Texture->LODGroup = TEXTUREGROUP_UI; // Use UI group for better alpha handling
		Texture->MipGenSettings = TMGS_NoMipmaps; // No mipmaps for UI icons
		
		// Ensure the texture never has alpha compression
		Texture->bNoTiling = true;
		
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		
		// Copy pixel data ensuring alpha channel is preserved
		// FColor stores as RGBA (R=Red, G=Green, B=Blue, A=Alpha)
		// PF_B8G8R8A8 expects BGRA format, but FMemory::Memcpy should handle the conversion
		FMemory::Memcpy(TextureData, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
		
		Mip.BulkData.Unlock();

		// Update resource to apply settings
		Texture->UpdateResource();
	}

	return Texture;
}

