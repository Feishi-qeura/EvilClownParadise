#include "Actors/ECPPickupItem.h"
#include "Components/PrimitiveComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Characters/ECPPlayerBase.h"
#include "Blueprint/UserWidget.h"
#include "Widgets/SWidget.h"

namespace ECPValuePresentation
{
uint8 Encode(const EVisibility& Visibility)
{
	if (Visibility==EVisibility::Collapsed) return 1;
	if (Visibility==EVisibility::Hidden) return 2;
	if (Visibility==EVisibility::HitTestInvisible) return 3;
	if (Visibility==EVisibility::SelfHitTestInvisible) return 4;
	if (Visibility==EVisibility::All) return 5;
	return 0;
}
EVisibility Decode(uint8 Value)
{
	switch (Value)
	{
	case 1: return EVisibility::Collapsed;
	case 2: return EVisibility::Hidden;
	case 3: return EVisibility::HitTestInvisible;
	case 4: return EVisibility::SelfHitTestInvisible;
	case 5: return EVisibility::All;
	default: return EVisibility::Visible;
	}
}
}

void AECPPickupItem::SetValueWidgetVisibility(FValueWidgetPresentation& Presentation,bool bVisible)
{
	UWidgetComponent* Widget=Presentation.Widget.Get();
	if (!Widget) return;
	if (UUserWidget* UserWidget=Widget->GetUserWidgetObject())
	{
		if (Presentation.PresentedUserWidget.Get()!=UserWidget)
		{
			Presentation.PresentedUserWidget=UserWidget;
			Presentation.OriginalUserVisibility=static_cast<uint8>(UserWidget->GetVisibility());
		}
		UserWidget->SetVisibility(bVisible ? static_cast<ESlateVisibility>(Presentation.OriginalUserVisibility) : ESlateVisibility::Collapsed);
	}
	else if (TSharedPtr<SWidget> Slate=Widget->GetSlateWidget())
	{
		if (Presentation.PresentedSlateWidget.Pin()!=Slate)
		{
			Presentation.PresentedSlateWidget=Slate;
			Presentation.OriginalSlateVisibility=ECPValuePresentation::Encode(Slate->GetVisibility());
		}
		Slate->SetVisibility(bVisible ? ECPValuePresentation::Decode(Presentation.OriginalSlateVisibility) : EVisibility::Collapsed);
	}
	// Slate 当帧隐藏，不等待私有 UpdateWidgetOnScreen 在下一次组件 Tick 移除屏幕槽位。
	Widget->SetVisibility(bVisible,false);
	Widget->SetHiddenInGame(!bVisible,false);
}

void AECPPickupItem::InitializeValueWidgets()
{
	if (!bManageValueWidgets || !GetCarryBody()) return;
	TInlineComponentArray<UWidgetComponent*> Widgets(this);
	for (UWidgetComponent* Widget : Widgets)
	{
		if (!ValueWidgetComponentTag.IsNone() && !Widget->ComponentHasTag(ValueWidgetComponentTag)) continue;
		FValueWidgetPresentation& Presentation=ValueWidgets.AddDefaulted_GetRef();
		Presentation.Widget=Widget;
		// 保留蓝图布置的偏移和绘制尺寸，但不随检查旋转绕模型转动。
		Presentation.WorldOffset=GetCarryBody()->GetComponentQuat().UnrotateVector(Widget->GetComponentLocation()-GetCarryBody()->Bounds.Origin);
		const FTransform OriginalWorld=Widget->GetComponentTransform();
		Widget->SetAbsolute(true,true,true); Widget->SetWorldTransform(OriginalWorld);
		Widget->SetWidgetSpace(EWidgetSpace::Screen);
		Widget->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Widget->SetIsReplicated(false);
		Widget->PrimaryComponentTick.TickGroup=TG_PostUpdateWork;
		Widget->AddTickPrerequisiteActor(this);
		SetValueWidgetVisibility(Presentation,false);
	}
	// 相机管理器在 PostPhysics 之后更新；值标签必须使用本帧最终相机。
	if (!ValueWidgets.IsEmpty()) PrimaryActorTick.TickGroup=TG_PostUpdateWork;
}

bool AECPPickupItem::CanShowPickupValueTo(const APlayerController* Viewer) const
{
	const UPrimitiveComponent* Body=bOwnerDisplayActive && OwnerDisplayMesh ? static_cast<const UPrimitiveComponent*>(OwnerDisplayMesh.Get()) : GetCarryBody();
	if (!bManageValueWidgets || !IsValid(Viewer) || !Body || Viewer->GetWorld()!=GetWorld()
		|| GetPickupState()!=EECPPickupState::Pickup || IsHidden() || IsActorBeingDestroyed()) return false;
	// 只给实际控制持有者的本地玩家显示，队友和看向持有者的旁观者都不显示。
	if (!Viewer->IsLocalController() || !GetPickupOwner() || Viewer->GetPawn()!=GetPickupOwner()) return false;
	FVector Eye; FRotator View; Viewer->GetPlayerViewPoint(Eye,View);
	if (FVector::DotProduct(Body->Bounds.Origin-Eye,View.Vector())<=0.f) return false;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPValueOcclusion),false,this);
	Query.AddIgnoredActor(Viewer->GetPawn());
	return !GetWorld()->LineTraceTestByChannel(Eye,Body->Bounds.Origin,ECC_Visibility,Query);
}

void AECPPickupItem::UpdateValueWidgets()
{
	UPrimitiveComponent* Body=bOwnerDisplayActive && OwnerDisplayMesh ? static_cast<UPrimitiveComponent*>(OwnerDisplayMesh.Get()) : GetCarryBody();
	for (FValueWidgetPresentation& Presentation : ValueWidgets)
	{
		UWidgetComponent* Widget=Presentation.Widget.Get();
		if (!Widget) continue;
		if (Body) Widget->SetWorldLocation(Body->Bounds.Origin+Presentation.WorldOffset);
		APlayerController* Viewer=nullptr;
		// 分屏时控件必须归属真正持有者的本地视口，不能默认选择第一个本地玩家。
		if (GetNetMode()!=NM_DedicatedServer)
		{
			for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
			{
				if (APlayerController* Candidate=It->Get(); Candidate && Candidate->IsLocalController() && Candidate->GetPawn()==GetPickupOwner()) { Viewer=Candidate; break; }
			}
		}
		if (Viewer && Viewer->GetLocalPlayer() && Widget->GetOwnerPlayer()!=Viewer->GetLocalPlayer()) Widget->SetOwnerPlayer(Viewer->GetLocalPlayer());
		bool bVisible=Viewer && Viewer->IsLocalController() && CanShowPickupValueTo(Viewer);
		if (bVisible)
		{
			AActor* ItemOwner=GetOwner();
			const bool bOwnView=ItemOwner && (ItemOwner==Viewer->GetPawn() || ItemOwner==Viewer->GetViewTarget());
			bVisible=(!Widget->bOnlyOwnerSee || bOwnView) && (!Widget->bOwnerNoSee || !bOwnView);
			FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPValueLabelOcclusion),false,this); Query.AddIgnoredActor(Viewer->GetPawn());
			FVector Eye; FRotator View; Viewer->GetPlayerViewPoint(Eye,View);
			bVisible=bVisible && !GetWorld()->LineTraceTestByChannel(Eye,Widget->GetComponentLocation(),ECC_Visibility,Query);
		}
		SetValueWidgetVisibility(Presentation,bVisible);
	}
}

void AECPPickupItem::HideValueWidgets()
{
	for (FValueWidgetPresentation& Presentation : ValueWidgets) SetValueWidgetVisibility(Presentation,false);
	ValueWidgets.Reset();
}
