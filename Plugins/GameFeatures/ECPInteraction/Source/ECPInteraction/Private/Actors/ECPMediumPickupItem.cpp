#include "Actors/ECPMediumPickupItem.h"

AECPMediumPickupItem::AECPMediumPickupItem()
{
	DefaultMassKg = 80.f;
	MinimumMassKg = 50.f;
	MaximumMassKg = 120.f;
	bSupportsEquipment = false;
	bSupportsStorage = false;
}
