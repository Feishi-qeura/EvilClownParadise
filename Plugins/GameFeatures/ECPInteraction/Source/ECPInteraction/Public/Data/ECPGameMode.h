// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ECPGameMode.generated.h"

/** 在服务器登录阶段分配稳定的加入顺序，为同帧拾取竞争提供确定性排序。 */
UCLASS()
class ECPINTERACTION_API AECPGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void PostLogin(APlayerController* NewPlayer) override;

private:
	// 单调序号不复用离线玩家的空位，保证本局内排序不会因有人退出而改变。
	int32 NextPickupJoinOrder = 0;
};
