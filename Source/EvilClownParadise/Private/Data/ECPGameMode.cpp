// Fill out your copyright notice in the Description page of Project Settings.


#include "Data/ECPGameMode.h"
#include "Data/ECPPlayerController.h"

void AECPGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	AECPPlayerController* ECPController = Cast<AECPPlayerController>(NewPlayer);
	if (!ECPController)
	{
		return;
	}
	// Listen Server 的本地控制器独立标记为最高优先；专服没有本地控制器，全部按加入序号。
	const bool bListenServerHost = GetNetMode() == NM_ListenServer && ECPController->IsLocalController();
	ECPController->SetPickupJoinOrder(NextPickupJoinOrder++, bListenServerHost);
}
