// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ECPPlayerController.generated.h"

UCLASS()
class EVILCLOWNPARADISE_API AECPPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	/** 测试用：对自己的 Pawn 造成伤害。PIE 控制台（~键）输入：TestDamage 30 */
	UFUNCTION(Exec)
	void TestDamage(float Amount);
};
