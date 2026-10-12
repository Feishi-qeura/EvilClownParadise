#pragma once

#include "CoreMinimal.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include "ECPPhysicalCarryComponent.generated.h"

class UPhysicsConstraintComponent;
class UCharacterMovementComponent;

/**
一个仅限服务器端的物理抓取器，可用于抓取物品及队友的骨架尸体。
*/
UCLASS()
class ECPINTERACTION_API UECPPhysicalCarryComponent : public UPhysicsHandleComponent
{
	GENERATED_BODY()
public:
	UECPPhysicalCarryComponent();
	/** Hit 仅来自服务器即时重做的射线；布娃娃按命中肢体和表面点抓取。 */
	bool StartCarry(AActor* Target, const FHitResult* Hit=nullptr);
	void StopCarry();
	AActor* GetCarryTarget() const { return CarryTarget.Get(); }
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
protected:
	virtual void OnUnregister() override;
private:
	FTransform LimitTarget(const FTransform& Desired) const;
	FTransform GetCurrentGripTransform() const;
	void SuppressHolderContact(UPrimitiveComponent* Body, FName Bone);
	TWeakObjectPtr<AActor> CarryTarget;
	TWeakObjectPtr<UPrimitiveComponent> ContactBody;
	UPROPERTY(Transient) TArray<TObjectPtr<UPhysicsConstraintComponent>> ContactFilters;
	FTransform SmoothedTarget;
	FQuat GripRelativeRotation = FQuat::Identity;
	FVector LastHolderLocation = FVector::ZeroVector;
	bool bHolderAlreadyIgnored = false;
	bool bTargetAlreadyIgnored = false;
	bool bOriginalCCD = false;
	FName ContactBone;
	TWeakObjectPtr<UCharacterMovementComponent> SavedInteractionMovement;
	bool bSavedPhysicsInteraction=false;
};
