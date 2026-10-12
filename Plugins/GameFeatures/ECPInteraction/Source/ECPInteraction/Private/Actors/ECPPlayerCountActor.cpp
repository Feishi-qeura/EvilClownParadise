#include "Actors/ECPPlayerCountActor.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/PackageName.h"

int32 AECPPlayerCountActor::GetCurrentWorldPlayerCount()
{
	const UWorld* World=GetWorld();
	if (!HasAuthority() || !World) return 0;

	int32 Count=0;
	for (FConstPlayerControllerIterator It=World->GetPlayerControllerIterator();It;++It)
	{
		const APlayerController* Controller=It->Get();
		const APawn* Pawn=IsValid(Controller) ? Controller->GetPawn() : nullptr;
		if (IsValid(Pawn) && !Pawn->IsA<ASpectatorPawn>()) ++Count;
	}
	return Count;
}

bool AECPPlayerCountActor::AreAllCurrentPlayersInsideBox()
{
	if (!HasAuthority() || !GetWorld()) return false;
	const UBoxComponent* Box=FindComponentByClass<UBoxComponent>();
	if (!IsValid(Box)) return false;

	int32 PlayerCount=0;
	for (FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
	{
		const APlayerController* Controller=It->Get();
		const APawn* Pawn=IsValid(Controller) ? Controller->GetPawn() : nullptr;
		if (!IsValid(Pawn) || Pawn->IsA<ASpectatorPawn>()) continue;
		++PlayerCount;
		if (!Box->IsOverlappingActor(Pawn)) return false;
	}
	return PlayerCount>0;
}

bool AECPPlayerCountActor::TravelAllPlayersToLevel(TSoftObjectPtr<UWorld> DestinationLevel)
{
	UWorld* World=GetWorld();
	// A non-replicated level Actor can have ROLE_Authority even on a client.
	// Check the world's network mode as well as the Actor's role.
	if (!World || World->GetNetMode()==NM_Client || !HasAuthority() || !World->GetAuthGameMode()) return false;
	if (!World->NextURL.IsEmpty() || World->IsInSeamlessTravel() || DestinationLevel.IsNull()) return false;
	const FString MapPackage=DestinationLevel.ToSoftObjectPath().GetLongPackageName();
	if (!FPackageName::IsValidLongPackageName(MapPackage) || !FPackageName::DoesPackageExist(MapPackage)) return false;
	// Relative travel preserves the bound server port and connection settings.
	// An empty game option removes the lobby override so the destination's WorldSettings win.
	const FString URL=MapPackage+TEXT("?game=")+(World->GetNetMode()==NM_ListenServer ? TEXT("?listen") : TEXT(""));
	return World->ServerTravel(URL,false);
}
