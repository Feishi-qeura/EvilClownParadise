#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ECPCarryTarget.generated.h"

class APawn;
class AActor;
class UPrimitiveComponent;

DECLARE_MULTICAST_DELEGATE_OneParam(FECPCarryTargetChanged, AActor*);

/** Common native contract: Foundation never needs a concrete item or character feature. */
UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UECPCarryTarget : public UInterface
{
	GENERATED_BODY()
};

class ECPFOUNDATION_API IECPCarryTarget
{
	GENERATED_BODY()
public:
	virtual UPrimitiveComponent* GetCarryBody() const = 0;
	virtual FName GetCarryBone() const { return NAME_None; }
	virtual float GetCarryMassKg() const = 0;
	virtual APawn* GetCarryHolder() const = 0;
	virtual bool CanBeCarriedBy(const APawn* Holder) const = 0;
	virtual bool TryBeginCarry(APawn* Holder) = 0;
	virtual void EndCarry(APawn* Holder) = 0;
	// Native weak UObject delegates notify owners about mass, state and destruction, without a reverse dependency.
	FECPCarryTargetChanged OnCarryTargetChanged;
};
