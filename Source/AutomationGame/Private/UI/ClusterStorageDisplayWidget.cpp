#include "UI/ClusterStorageDisplayWidget.h"
#include "Framework/GameManager.h"
#include "Data/OrbMaterialManager.h"
#include "Actors/HexTopDownPawn.h"
#include "Blueprints/BlueprintTile.h"
#include "EngineUtils.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Sim/OrbTypes.h"
#include "Sim/SimWorldHex.h"

UClusterStorageDisplayWidget::UClusterStorageDisplayWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GameManager = nullptr;
	LastKnownClusterCount = 0;
	LastKnownIconScale = 1.0f;
	RefreshInterval = 0.5f;
	TimeSinceLastRefresh = 0.0f;
}

void UClusterStorageDisplayWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Try to auto-find GameManager
	if (!GameManager)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				GameManager = *It;
				break;
			}
		}
	}
	
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

void UClusterStorageDisplayWidget::NativeDestruct()
{
	Super::NativeDestruct();
	
	// Clear button references and remove delegates before destruction
	// This prevents access violations when buttons are destroyed
	for (UButton* Button : ClusterIconButtons)
	{
		if (IsValid(Button))
		{
			// Remove all delegates bound to this widget
			Button->OnClicked.Clear();
		}
	}
	
	// Clear arrays/maps to prevent dangling pointers
	ClusterIconButtons.Empty();
	ClusterButtonToIndex.Empty();
}

void UClusterStorageDisplayWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Auto-refresh at interval if needed
	TimeSinceLastRefresh += InDeltaTime;
	if (TimeSinceLastRefresh >= RefreshInterval)
	{
		TimeSinceLastRefresh = 0.0f;
		if (NeedsRefresh())
		{
			RefreshDisplay();
		}
	}
}

void UClusterStorageDisplayWidget::RefreshDisplay()
{
	if (!GameManager || !ClusterEntriesBox)
	{
		return;
	}

	// Check if icon scale changed - clear icon cache if it did
	if (GameManager->ClusterIconScale != LastKnownIconScale)
	{
		GameManager->ClearClusterIcons();
	}

	// Clear existing entries
	ClusterEntriesBox->ClearChildren();
	ClusterIconButtons.Empty();
	ClusterButtonToIndex.Empty();

	// Get cluster data from GameManager
	TArray<USimWorldHex::FClusterTemplate> Templates;
	TArray<int32> Counts;
	GameManager->GetStoredClusters(Templates, Counts);

	LastKnownClusterCount = Templates.Num();
	LastKnownIconScale = GameManager->ClusterIconScale;

	// UE_LOG(LogTemp, Log, TEXT("ClusterStorageDisplayWidget: Displaying %d unique clusters"), Templates.Num());

	// Get OrbMaterialManager for colors
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);

	// Create a row for each unique cluster
	for (int32 i = 0; i < Templates.Num(); i++)
	{
		const USimWorldHex::FClusterTemplate& Template = Templates[i];
		int32 Count = Counts[i];

		// Create a horizontal box for this cluster entry
		UHorizontalBox* ClusterRow = NewObject<UHorizontalBox>(this);
		if (ClusterRow)
		{
			// Get the icon for this cluster
			UTexture2D* ClusterIcon = GameManager->GetClusterIcon(Template);
			
			// Create an image widget for the icon if available
			if (ClusterIcon)
			{
				UImage* IconImage = NewObject<UImage>(this);
				if (IconImage)
				{
					// First set the texture
					IconImage->SetBrushFromTexture(ClusterIcon);
					
					// Then set the brush size directly (more reliable than SetDesiredSizeOverride)
					FSlateBrush MyBrush = IconImage->GetBrush();
					const float BaseDisplaySize = 150.0f;
					float DisplaySize = BaseDisplaySize * GameManager->ClusterIconScale;
					MyBrush.SetImageSize(FVector2D(DisplaySize, DisplaySize));
					IconImage->SetBrush(MyBrush);
					
					// Also set desired size override as backup
					IconImage->SetDesiredSizeOverride(FVector2D(DisplaySize, DisplaySize));
					
					// Make it clickable by wrapping in a transparent button
					UButton* IconButton = NewObject<UButton>(this);
					if (IconButton)
					{
						// Create completely transparent/no-draw brushes for all button states
						FSlateBrush NoDrawBrush;
						NoDrawBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
						
						FButtonStyle ButtonStyle;
						ButtonStyle.Normal = NoDrawBrush;
						ButtonStyle.Hovered = NoDrawBrush;
						ButtonStyle.Pressed = NoDrawBrush;
						ButtonStyle.Disabled = NoDrawBrush;
						ButtonStyle.NormalPadding = FMargin(0.0f);
						ButtonStyle.PressedPadding = FMargin(0.0f);
						
						IconButton->SetStyle(ButtonStyle);
						IconButton->SetBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
						
						// Store button and map to template index
						ClusterIconButtons.Add(IconButton);
						ClusterButtonToIndex.Add(IconButton, i);
						
						// Add click handler
						IconButton->OnClicked.AddDynamic(this, &UClusterStorageDisplayWidget::HandleClusterIconClicked);
						
						IconButton->AddChild(IconImage);
						
						// Add button to the row
						UHorizontalBoxSlot* BoxSlot = ClusterRow->AddChildToHorizontalBox(IconButton);
						if (BoxSlot)
						{
							BoxSlot->SetHorizontalAlignment(HAlign_Center);
							BoxSlot->SetVerticalAlignment(VAlign_Center);
						}
					}
					else
					{
						// Fallback: add image directly if button creation fails
						UHorizontalBoxSlot* BoxSlot = ClusterRow->AddChildToHorizontalBox(IconImage);
						if (BoxSlot)
						{
							BoxSlot->SetHorizontalAlignment(HAlign_Center);
							BoxSlot->SetVerticalAlignment(VAlign_Center);
						}
					}
				}
			}

			// Create a count text block
			UTextBlock* CountText = NewObject<UTextBlock>(this);
			if (CountText)
			{
				CountText->SetText(FText::FromString(FString::Printf(TEXT("x%d"), Count)));
				CountText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
				// Get default font and modify size
				FSlateFontInfo FontInfo = CountText->GetFont();
				FontInfo.Size = 16;
				CountText->SetFont(FontInfo);

				// Add count to the row
				ClusterRow->AddChildToHorizontalBox(CountText);
			}

			// Add the row to the vertical box
			ClusterEntriesBox->AddChildToVerticalBox(ClusterRow);
		}
	}
}

void UClusterStorageDisplayWidget::SetGameManager(AGameManager* InGameManager)
{
	GameManager = InGameManager;
	RefreshDisplay();
}

bool UClusterStorageDisplayWidget::NeedsRefresh() const
{
	if (!GameManager)
	{
		return false;
	}

	TArray<USimWorldHex::FClusterTemplate> Templates;
	TArray<int32> Counts;
	GameManager->GetStoredClusters(Templates, Counts);

	// Check for cluster count changes
	if (Templates.Num() != LastKnownClusterCount)
	{
		return true;
	}

	// Check for icon scale changes
	if (GameManager->ClusterIconScale != LastKnownIconScale)
	{
		return true;
	}

	return false;
}

void UClusterStorageDisplayWidget::HandleClusterIconClicked()
{
	if (!GameManager || !Pawn)
	{
		return;
	}
	
	// Find which button was clicked by checking hover state
	UButton* ClickedButton = nullptr;
	for (UButton* Button : ClusterIconButtons)
	{
		if (IsValid(Button) && Button->IsHovered())
		{
			ClickedButton = Button;
			break;
		}
	}
	
	if (!IsValid(ClickedButton) || !ClusterButtonToIndex.Contains(ClickedButton))
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleClusterIconClicked: Could not identify clicked button"));
		return;
	}
	
	int32 TemplateIndex = ClusterButtonToIndex[ClickedButton];
	
	// Get the cluster template
	TArray<USimWorldHex::FClusterTemplate> Templates;
	TArray<int32> Counts;
	GameManager->GetStoredClusters(Templates, Counts);
	
	if (TemplateIndex < 0 || TemplateIndex >= Templates.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleClusterIconClicked: Invalid template index %d"), TemplateIndex);
		return;
	}
	
	const USimWorldHex::FClusterTemplate& Template = Templates[TemplateIndex];
	
	// Find or register the TemplateId for this template
	uint8 TemplateId = GameManager->FindOrRegisterTemplateId(Template);
	
	// Generate blueprint from cluster template
	TArray<FBlueprintTile> BlueprintTiles;
	
	// Create a ClusterInput tile for each orb in the template
	for (const FOrbData& Orb : Template.Orbs)
	{
		FBlueprintTile BlueprintTile;
		BlueprintTile.LocalHex = FHex(Orb.dQ, Orb.dR);
		BlueprintTile.TileType = ETileKind::ClusterInput;
		BlueprintTile.TileDirection = Orb.Type; // Store the orb type (not TemplateId) - this becomes T.Arg for ClusterInput
		BlueprintTile.bIsArm = false;
		
		BlueprintTiles.Add(BlueprintTile);
	}
	
	// Set the blueprint for placement
	Pawn->SetBlueprintForPlacement(BlueprintTiles);
	
	UE_LOG(LogTemp, Log, TEXT("ClusterStorageDisplayWidget: Generated blueprint from cluster template %d with %d ClusterInput tiles"), 
		TemplateId, BlueprintTiles.Num());
}

