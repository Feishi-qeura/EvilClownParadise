// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ECPGameMode.generated.h"

/**
 * 
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void PostLogin(APlayerController* NewPlayer) override;

private:
	// 单调序号不复用离线玩家的空位，保证本局内排序不会因有人退出而改变。
	int32 NextPickupJoinOrder = 0;
};
