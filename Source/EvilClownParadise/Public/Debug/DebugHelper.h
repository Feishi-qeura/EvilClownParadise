// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

namespace DebugHelper
{
	inline void Print(const FString& Message)
	{
		GEngine -> AddOnScreenDebugMessage(-1, 2.0f, FColor::Black, Message);
	}
}
