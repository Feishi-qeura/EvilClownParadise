#pragma once

#include "Actors/ECPPickupItem.h"
#include "ECPMediumPickupItem.generated.h"

/** Reflection compatibility for existing blueprints; all behaviour is inherited from the common item. */
UCLASS()
class ECPINTERACTION_API AECPMediumPickupItem : public AECPPickupItem
{
	GENERATED_BODY()
public:
	AECPMediumPickupItem();
};
