#include "FU_CheckSessionStatusAsync.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "TimerManager.h"

UFU_CheckSessionStatusAsync* UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(UObject* WorldContextObject, const float RefreshTime)
{
	if (!WorldContextObject || RefreshTime <= 0.0f || !GEngine)
	{
		return nullptr;
	}

	UWorld* ResolvedWorld = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	if (!ResolvedWorld)
	{
		return nullptr;
	}

	UFU_CheckSessionStatusAsync* Action = NewObject<UFU_CheckSessionStatusAsync>();
	Action->World = ResolvedWorld;
	Action->RefreshInterval = RefreshTime;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFU_CheckSessionStatusAsync::Activate()
{
	UWorld* ResolvedWorld = World.Get();
	APlayerController* PlayerController = ResolvedWorld ? ResolvedWorld->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		FU_BroadcastTimeout();
		return;
	}

	// The authority has no remote ping to sample, so it receives one explicit server callback and completes.
	if (PlayerController->HasAuthority())
	{
		OnServer.Broadcast(0.0f);
		SetReadyToDestroy();
		return;
	}

	ResolvedWorld->GetTimerManager().SetTimer(RefreshTimerHandle, this, &ThisClass::FU_UpdatePing, RefreshInterval, true);
	FU_UpdatePing();
}

void UFU_CheckSessionStatusAsync::BeginDestroy()
{
	// Blueprint async actions can be garbage collected during map travel; clear the timer before the object disappears.
	FU_StopTimer();
	Super::BeginDestroy();
}

void UFU_CheckSessionStatusAsync::FU_UpdatePing()
{
	UWorld* ResolvedWorld = World.Get();
	APlayerController* PlayerController = ResolvedWorld ? ResolvedWorld->GetFirstPlayerController() : nullptr;
	APlayerState* PlayerState = PlayerController ? PlayerController->PlayerState : nullptr;
	if (!PlayerState)
	{
		FU_BroadcastTimeout();
		return;
	}

	OnClient.Broadcast(PlayerState->GetPingInMilliseconds());
}

void UFU_CheckSessionStatusAsync::FU_StopTimer()
{
	if (UWorld* ResolvedWorld = World.Get())
	{
		ResolvedWorld->GetTimerManager().ClearTimer(RefreshTimerHandle);
	}
}

void UFU_CheckSessionStatusAsync::FU_BroadcastTimeout()
{
	FU_StopTimer();
	ClientConnectionOvertime.Broadcast(0.0f);
	SetReadyToDestroy();
}
