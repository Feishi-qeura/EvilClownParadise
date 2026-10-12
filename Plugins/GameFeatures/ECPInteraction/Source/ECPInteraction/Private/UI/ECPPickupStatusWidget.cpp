#include "UI/ECPPickupStatusWidget.h"
#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/World.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"

TSharedRef<SWidget> UECPPickupStatusWidget::RebuildWidget()
{
	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Canvas;
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrushColor(FLinearColor(0.025f, 0.035f, 0.05f, 0.85f));
	Panel->SetPadding(FMargin(14.f, 10.f));
	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(Panel);
	PanelSlot->SetPosition(FVector2D(24.f, 24.f));
	PanelSlot->SetAutoSize(true);
	UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>();
	Panel->SetContent(Rows);
	auto AddRow = [this, Rows](const TCHAR* Label)
	{
		UTextBlock* Row = WidgetTree->ConstructWidget<UTextBlock>();
		Row->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
		Row->SetText(FText::FromString(Label));
		Rows->AddChildToVerticalBox(Row);
		return Row;
	};
	AddRow(TEXT("物品状态"));
	HitText = AddRow(TEXT("命中：无"));
	PickupText = AddRow(TEXT("拾取：否"));
	EquippedText = AddRow(TEXT("装备：否"));
	StoredText = AddRow(TEXT("入库：否（0）"));
	LoadText = AddRow(TEXT("负重：0 / 200 kg"));
	SetVisibility(ESlateVisibility::HitTestInvisible);
	return Super::RebuildWidget();
}

void UECPPickupStatusWidget::NativeConstruct()
{
	Super::NativeConstruct();
	RefreshStatus();
	GetWorld()->GetTimerManager().SetTimer(RefreshTimer, this, &ThisClass::RefreshStatus, 0.1f, true);
}

void UECPPickupStatusWidget::NativeDestruct()
{
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(RefreshTimer);
	Super::NativeDestruct();
}

void UECPPickupStatusWidget::RefreshStatus()
{
	const AECPPlayerBase* Player = Cast<AECPPlayerBase>(GetOwningPlayerPawn());
	if (!Player || !HitText) return;
	const AECPPickupItem* Hit = Player->GetScannedPickup();
	const AActor* CarryHit = Player->GetScannedCarryTarget();
	const bool bPickup = Player->GetInspectedPickup() != nullptr;
	const bool bEquipped = Player->GetEquippedPickup() != nullptr;
	const int32 Count = Player->GetStoredPickupCount();
	const AECPPickupItem* Active = Player->GetInspectedPickup();
	if (!Active) Active = Player->GetEquippedPickup();
	if (!Active) Active = Hit;
	const bool bEquipmentAllowed = !Active || Active->SupportsEquipment();
	const bool bStorageAllowed = !Active || Active->SupportsStorage();
	auto SetRow = [](UTextBlock* Row, const FString& Label, bool bActive)
	{
		Row->SetText(FText::FromString(Label));
		Row->SetColorAndOpacity(FSlateColor(bActive
			? FLinearColor(0.4f, 1.f, 0.65f) : FLinearColor(0.7f, 0.75f, 0.8f)));
	};
	SetRow(HitText, CarryHit ? FString::Printf(TEXT("命中：%s"), *CarryHit->GetActorNameOrLabel()) : TEXT("命中：无"), CarryHit != nullptr);
	SetRow(PickupText, bPickup ? TEXT("拾取：是") : TEXT("拾取：否"), bPickup);
	SetRow(EquippedText, !bEquipmentAllowed ? TEXT("装备：不可用") : bEquipped ? TEXT("装备：是") : TEXT("装备：否"), bEquipped);
	SetRow(StoredText, !bStorageAllowed ? FString::Printf(TEXT("入库：不可用（已有 %d）"), Count)
		: FString::Printf(TEXT("入库：%s（%d）"), Count > 0 ? TEXT("是") : TEXT("否"), Count), bStorageAllowed && Count > 0);
	SetRow(LoadText, FString::Printf(TEXT("负重：%.1f / %.1f kg%s%s"), Player->GetCarriedWeightKg(), Player->MaxCarryWeightKg,
		Player->IsOverloaded() ? (Player->GetInspectedPickup() ? TEXT("（超重，右键放下）") : TEXT("（超重，G 卸重）")) : TEXT(""),
		Player->GetCarriedTeammate() ? TEXT(" · 搬运队友") : TEXT("")), !Player->IsOverloaded());
}
