#pragma once

#include "CoreMinimal.h"

/** Keeps Blueprint-facing session methods from passing invalid requests to an online provider. */
struct FFU_SessionRequestValidation
{
	static bool CanCreate(const int32 MaxPlayers, const FString& RoomName)
	{
		return MaxPlayers > 0 && !RoomName.TrimStartAndEnd().IsEmpty();
	}
};
