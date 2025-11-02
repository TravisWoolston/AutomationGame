#include "UI/ClusterStorageDisplayWidget.h"
#include "Framework/GameManager.h"
#include "Data/OrbMaterialManager.h"
#include "EngineUtils.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Sim/OrbTypes.h"

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

	RefreshDisplay();
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
					
					// Add image to the row and configure the slot
					UHorizontalBoxSlot* BoxSlot = ClusterRow->AddChildToHorizontalBox(IconImage);
					if (BoxSlot)
					{
						BoxSlot->SetHorizontalAlignment(HAlign_Center);
						BoxSlot->SetVerticalAlignment(VAlign_Center);
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

