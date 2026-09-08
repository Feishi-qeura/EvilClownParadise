// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/ECPPlayerController.h"
#include "Kismet/GameplayStatics.h"

void AECPPlayerController::TestDamage(float Amount)
{
	APawn* MyPawn = GetPawn();
	if (!MyPawn)
	{
		return;
	}
	UGameplayStatics::ApplyDamage(MyPawn, Amount, this, MyPawn, nullptr);
}
