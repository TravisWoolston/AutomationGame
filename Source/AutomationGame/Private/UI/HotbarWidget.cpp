#include "UI/HotbarWidget.h"
#include "Actors/HexTopDownPawn.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/ButtonSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "GameFramework/InputSettings.h"
#include "Kismet/GameplayStatics.h"

UHotbarWidget::UHotbarWidget(const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer) {}

void UHotbarWidget::NativeConstruct() {
  Super::NativeConstruct();
  CachedPawn = Cast<AHexTopDownPawn>(GetOwningPlayerPawn());
  SetIsFocusable(false);
  SetVisibility(ESlateVisibility::SelfHitTestInvisible);
  GenerateHotbar();
}

void UHotbarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime) {
  Super::NativeTick(MyGeometry, InDeltaTime);

  EPlaceKind CurrentKind = (CachedPawn && CachedPawn->Placer) ? CachedPawn->Placer->PlaceKind : EPlaceKind::Input;
  APlayerController* PC = GetOwningPlayer();
  bool bAltHeld = PC && (PC->IsInputKeyDown(EKeys::LeftAlt) || PC->IsInputKeyDown(EKeys::RightAlt));

  if (bAltHeld != bWasAltHeld) {
      for (FSlotGroup& G : StandardGroups) if (G.SlotOverlay) G.SlotOverlay->SetVisibility(bAltHeld ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
      for (FSlotGroup& G : AltGroups) if (G.SlotOverlay) G.SlotOverlay->SetVisibility(bAltHeld ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
      bWasAltHeld = bAltHeld;
  }

  TArray<FSlotGroup>& CurrentVisible = bAltHeld ? AltGroups : StandardGroups;

  for (int32 i = 0; i < CurrentVisible.Num(); ++i) {
    FSlotGroup& Group = CurrentVisible[i];
    bool bAnyActionActive = false;
    for (int32 j = 0; j < Group.GroupKinds.Num(); ++j) {
      bool bMatch = (Group.GroupKinds[j] == CurrentKind);
      if (bMatch) {
          bAnyActionActive = true;
          if (Group.MainIcon) {
              FTileIconEntry Entry = GetIconForAction(Group.Actions[j]);
              Group.MainIcon->SetBrushFromTexture(Entry.Texture);
              FWidgetTransform T = Group.MainIcon->GetRenderTransform();
              T.Scale = FVector2D(Entry.Scale, Entry.Scale);
              Group.MainIcon->SetRenderTransform(T);
          }
      }
      if (Group.SubIcons.IsValidIndex(j)) {
          Group.SubIcons[j]->SetColorAndOpacity(bMatch ? FLinearColor::White : FLinearColor(0.2f, 0.2f, 0.2f, 0.7f));
      }
    }

    if (Group.MainButton) {
        FLinearColor ActiveColor(1.0f, 0.9f, 0.6f, 1.0f);
        Group.MainButton->SetBackgroundColor(bAnyActionActive ? ActiveColor : FLinearColor::White);
    }
    
    if (Group.SelectionBorder) {
        Group.SelectionBorder->SetVisibility(bAnyActionActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    }

    Group.TargetSlide = (Group.bIsHovered || bAnyActionActive) && (Group.Actions.Num() > 1) ? 1.0f : 0.0f;

    if (!FMath::IsNearlyEqual(Group.CurrentSlide, Group.TargetSlide, 0.001f)) {
      Group.CurrentSlide = FMath::FInterpTo(Group.CurrentSlide, Group.TargetSlide, InDeltaTime, SlideSpeed);
      if (Group.Drawer) {
        if (Group.CurrentSlide <= 0.01f && Group.TargetSlide <= 0.01f) {
            Group.Drawer->SetVisibility(ESlateVisibility::Collapsed);
        } else {
            Group.Drawer->SetVisibility(ESlateVisibility::HitTestInvisible);
            float TotalMove = (Group.Actions.Num() * 44.0f); 
            FWidgetTransform Transform = Group.Drawer->GetRenderTransform();
            Transform.Translation.Y = -TotalMove * Group.CurrentSlide;
            Group.Drawer->SetRenderTransform(Transform);
            Group.Drawer->SetRenderOpacity(Group.CurrentSlide);
        }
      }
    }
  }
}

void UHotbarWidget::GenerateHotbar() {
  if (!HotbarContainer) return;
  HotbarContainer->ClearChildren();
  StandardGroups.Empty();
  AltGroups.Empty();
  bWasAltHeld = false;

  TMap<FKey, TArray<int32>> StandardMap;
  TMap<FKey, TArray<int32>> AltMap;
  const UInputSettings* Settings = UInputSettings::GetInputSettings();

  for (int32 ActionId = 1; ActionId <= 15; ++ActionId) {
    FName ActionName = (ActionId == 15) ? FName(TEXT("kind15")) : FName(*FString::Printf(TEXT("Kind%d"), ActionId));
    TArray<FInputActionKeyMapping> Mappings;
    Settings->GetActionMappingByName(ActionName, Mappings);
    for (const auto& M : Mappings) {
      if (!M.Key.IsValid()) continue;
      if (M.bAlt) AltMap.FindOrAdd(M.Key).Add(ActionId);
      else StandardMap.FindOrAdd(M.Key).Add(ActionId);
    }
  }

  auto CreateSlots = [&](TMap<FKey, TArray<int32>>& Map, TArray<FSlotGroup>& OutGroups, bool bAlt) {
    TArray<FKey> Keys; Map.GetKeys(Keys);
    Keys.Sort([](const FKey& A, const FKey& B) {
      FString NA = A.GetDisplayName().ToString(), NB = B.GetDisplayName().ToString();
      if (NA.IsNumeric() && NB.IsNumeric()) {
          int32 VA = FCString::Atoi(*NA), VB = FCString::Atoi(*NB);
          return ((VA == 0) ? 10 : VA) < ((VB == 0) ? 10 : VB);
      }
      return NA < NB;
    });

    for (int32 SlotIdx = 0; SlotIdx < Keys.Num(); ++SlotIdx) {
      FKey Key = Keys[SlotIdx];
      TArray<int32> Actions = Map[Key];
      FSlotGroup Group; Group.Key = Key; Group.Actions = Actions; Group.bIsAltGroup = bAlt;
      
      UOverlay* SlotOverlay = NewObject<UOverlay>(this);
      Group.SlotOverlay = SlotOverlay;
      SlotOverlay->SetVisibility(bAlt ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);

      UImage* SelectionBorder = NewObject<UImage>(this);
      SelectionBorder->SetVisibility(ESlateVisibility::Collapsed);
      SelectionBorder->SetColorAndOpacity(FLinearColor(1, 1, 1, 0.4f));
      SlotOverlay->AddChildToOverlay(SelectionBorder)->SetHorizontalAlignment(HAlign_Fill);
      Group.SelectionBorder = SelectionBorder;

      UVerticalBox* Drawer = NewObject<UVerticalBox>(this);
      Drawer->SetVisibility(ESlateVisibility::Collapsed);
      Drawer->SetRenderOpacity(0.0f);
      Group.Drawer = Drawer;
      Group.SubIcons.SetNumZeroed(Actions.Num());
      Group.GroupKinds.SetNumZeroed(Actions.Num());

      for (int32 i = Actions.Num() - 1; i >= 0; --i) {
        FTileIconEntry Entry = GetIconForAction(Actions[i]);
        UImage* SubIcon = NewObject<UImage>(this);
        SubIcon->SetBrushFromTexture(Entry.Texture);
        SubIcon->SetDesiredSizeOverride(FVector2D(40, 40));
        FWidgetTransform ST; ST.Scale = FVector2D(Entry.Scale, Entry.Scale);
        SubIcon->SetRenderTransform(ST);
        
        Drawer->AddChildToVerticalBox(SubIcon)->SetHorizontalAlignment(HAlign_Center);
        Group.SubIcons[i] = SubIcon;
        Group.GroupKinds[i] = GetPlaceKindForAction(Actions[i]);
      }
      UOverlaySlot* DS = SlotOverlay->AddChildToOverlay(Drawer);
      DS->SetVerticalAlignment(VAlign_Top); DS->SetHorizontalAlignment(HAlign_Center);

      UButton* Btn = NewObject<UButton>(this);
      Group.MainButton = Btn;
      FString SlotNum = FString::FromInt(SlotIdx);
      FScriptDelegate D;
      D.BindUFunction(this, FName(*(TEXT("OnSlot") + SlotNum + TEXT("Clicked")))); Btn->OnClicked.Add(D);
      D.BindUFunction(this, FName(*(TEXT("OnSlot") + SlotNum + TEXT("Hovered")))); Btn->OnHovered.Add(D);
      D.BindUFunction(this, FName(*(TEXT("OnSlot") + SlotNum + TEXT("Unhovered")))); Btn->OnUnhovered.Add(D);

      UOverlay* BO = NewObject<UOverlay>(this);
      FTileIconEntry StartEntry = GetIconForAction(Actions[0]);
      UImage* Icon = NewObject<UImage>(this);
      Icon->SetBrushFromTexture(StartEntry.Texture);
      FWidgetTransform BT; BT.Scale = FVector2D(StartEntry.Scale, StartEntry.Scale);
      Icon->SetRenderTransform(BT);
      
      UOverlaySlot* IS = BO->AddChildToOverlay(Icon);
      IS->SetHorizontalAlignment(HAlign_Fill); IS->SetVerticalAlignment(VAlign_Fill);
      Group.MainIcon = Icon;
      
      Btn->SetContent(BO);
      if (UButtonSlot* BS = Cast<UButtonSlot>(Btn->Slot)) {
          BS->SetPadding(FMargin(0));
          BS->SetHorizontalAlignment(HAlign_Fill);
          BS->SetVerticalAlignment(VAlign_Fill);
      }
      SlotOverlay->AddChildToOverlay(Btn);

      USizeBox* SizeWrapper = NewObject<USizeBox>(this);
      SizeWrapper->SetWidthOverride(64.0f);
      SizeWrapper->SetHeightOverride(64.0f);
      SizeWrapper->AddChild(SlotOverlay);

      UHorizontalBoxSlot* BoxSlot = HotbarContainer->AddChildToHorizontalBox(SizeWrapper);
      BoxSlot->SetPadding(FMargin(4, 0));
      BoxSlot->SetVerticalAlignment(VAlign_Center);
      BoxSlot->SetHorizontalAlignment(HAlign_Center);
      OutGroups.Add(Group);
    }
  };

  CreateSlots(StandardMap, StandardGroups, false);
  CreateSlots(AltMap, AltGroups, true);
}

void UHotbarWidget::OnSlotClicked(int32 id) { if(CachedPawn) switch(id){case 1:CachedPawn->SetInputMode1();break;case 2:CachedPawn->SetInputMode2();break;case 3:CachedPawn->SetInputMode3();break;case 4:CachedPawn->SetInputMode4();break;case 5:CachedPawn->SetInputMode5();break;case 6:CachedPawn->SetInputMode6();break;case 7:CachedPawn->SetInputMode7();break;case 8:CachedPawn->SetInputMode8();break;case 9:CachedPawn->SetInputMode9();break;case 10:CachedPawn->SetInputMode10();break;case 11:CachedPawn->SetInputMode11();break;case 12:CachedPawn->SetInputMode12();break;case 13:CachedPawn->SetInputMode13();break;case 14:CachedPawn->SetInputMode14();break;case 15:CachedPawn->SetInputMode15();break;} }

FKey UHotbarWidget::GetKeyForAction(FName N) const { const UInputSettings* S = UInputSettings::GetInputSettings(); if(!S)return EKeys::Invalid; TArray<FInputActionKeyMapping> O; S->GetActionMappingByName(N, O); return (O.Num()>0)?O[0].Key:EKeys::Invalid; }

FTileIconEntry UHotbarWidget::GetIconForAction(int32 i) const { switch(i){case 1:return TileIcons.Input;case 2:return TileIcons.Output;case 3:return TileIcons.Belt;case 4:return TileIcons.Arm;case 5:return TileIcons.Bond;case 6:return TileIcons.Unbond;case 7:return TileIcons.ClusterOutput;case 8:return TileIcons.RandomCluster;case 9:return TileIcons.RequestBeacon;case 10:return TileIcons.TriggerTile;case 11:return TileIcons.ClusterSend;case 12:return TileIcons.AncestralMode;case 13:return TileIcons.HelixOutput;case 14:return TileIcons.GatherScatter;case 15:return TileIcons.Source;default:return FTileIconEntry();} }

FString UHotbarWidget::GetLabelForAction(int32 i) const { return TEXT("Tile"); }

void UHotbarWidget::HandleSlotClick(int32 i) {
  TArray<FSlotGroup>& Groups = bWasAltHeld ? AltGroups : StandardGroups;
  if(!Groups.IsValidIndex(i) || !CachedPawn || !CachedPawn->Placer) return;
  const FSlotGroup& G = Groups[i];
  if(G.Actions.Num()==0) return;
  if(G.Actions.Num()==1){ OnSlotClicked(G.Actions[0]); return; }
  EPlaceKind K = CachedPawn->Placer->PlaceKind;
  int32 cur = -1; for(int32 j=0; j<G.GroupKinds.Num(); ++j) if(G.GroupKinds[j]==K){cur=j;break;}
  OnSlotClicked(G.Actions[(cur+1)%G.Actions.Num()]);
}

EPlaceKind UHotbarWidget::GetPlaceKindForAction(int32 i) const { switch(i){case 1:return EPlaceKind::Input;case 2:return EPlaceKind::Output;case 3:return EPlaceKind::Belt;case 4:return EPlaceKind::Arm;case 5:return EPlaceKind::Bond;case 6:return EPlaceKind::Unbond;case 7:return EPlaceKind::ClusterOutput;case 8:return EPlaceKind::ClusterInput;case 9:return EPlaceKind::RequestBeacon;case 10:return EPlaceKind::TriggerTile;case 11:return EPlaceKind::ClusterSend;case 12:return EPlaceKind::ClusterInput;case 13:return EPlaceKind::HelixOutput;case 14:return EPlaceKind::GatherScatter;case 15:return EPlaceKind::Source;default:return EPlaceKind::Input;} }

void UHotbarWidget::OnSlot0Clicked(){HandleSlotClick(0);} void UHotbarWidget::OnSlot1Clicked(){HandleSlotClick(1);} void UHotbarWidget::OnSlot2Clicked(){HandleSlotClick(2);} void UHotbarWidget::OnSlot3Clicked(){HandleSlotClick(3);} void UHotbarWidget::OnSlot4Clicked(){HandleSlotClick(4);} void UHotbarWidget::OnSlot5Clicked(){HandleSlotClick(5);} void UHotbarWidget::OnSlot6Clicked(){HandleSlotClick(6);} void UHotbarWidget::OnSlot7Clicked(){HandleSlotClick(7);} void UHotbarWidget::OnSlot8Clicked(){HandleSlotClick(8);} void UHotbarWidget::OnSlot9Clicked(){HandleSlotClick(9);} void UHotbarWidget::OnSlot10Clicked(){HandleSlotClick(10);} void UHotbarWidget::OnSlot11Clicked(){HandleSlotClick(11);} void UHotbarWidget::OnSlot12Clicked(){HandleSlotClick(12);} void UHotbarWidget::OnSlot13Clicked(){HandleSlotClick(13);} void UHotbarWidget::OnSlot14Clicked(){HandleSlotClick(14);}

#define IHH(X) void UHotbarWidget::OnSlot##X##Hovered(){ if(bWasAltHeld){ if(AltGroups.IsValidIndex(X)) AltGroups[X].bIsHovered=true;} else { if(StandardGroups.IsValidIndex(X)) StandardGroups[X].bIsHovered=true;} } \
void UHotbarWidget::OnSlot##X##Unhovered(){ if(AltGroups.IsValidIndex(X)) AltGroups[X].bIsHovered=false; if(StandardGroups.IsValidIndex(X)) StandardGroups[X].bIsHovered=false; }
IHH(0) IHH(1) IHH(2) IHH(3) IHH(4) IHH(5) IHH(6) IHH(7) IHH(8) IHH(9) IHH(10) IHH(11) IHH(12) IHH(13) IHH(14)

void UHotbarWidget::OnSlotHovered(int32 i){} void UHotbarWidget::OnSlotUnhovered(int32 i){}
