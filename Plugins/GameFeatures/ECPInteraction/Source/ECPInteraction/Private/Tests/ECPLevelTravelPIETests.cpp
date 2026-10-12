#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Actors/ECPPlayerCountActor.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ECPPIEWorldCleanup.h"

namespace ECPLevelTravelPIE
{
bool IsMap(const UWorld* World,const TCHAR* AssetName)
{
	return World && World->GetOutermost()->GetName().EndsWith(AssetName);
}

class FTravelCommand final:public IAutomationLatentCommand
{
public:
	explicit FTravelCommand(FAutomationTestBase* InTest):Test(InTest),Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		UWorld* Server=nullptr; UWorld* Client=nullptr;
		for (const FWorldContext& Context:GEngine->GetWorldContexts())
		{
			UWorld* World=Context.World();
			if (!World || World->WorldType!=EWorldType::PIE) continue;
			if (World->GetNetMode()==NM_ListenServer) Server=World;
			if (World->GetNetMode()==NM_Client) Client=World;
		}
		if (FPlatformTime::Seconds()-Started>75.0)
		{
			Test->AddError(FString::Printf(TEXT("Travel timeout: stage=%d server=%s client=%s"),Stage,
				Server ? *Server->GetOutermost()->GetName() : TEXT("none"),Client ? *Client->GetOutermost()->GetName() : TEXT("none")));
			return Finish();
		}
		if (!Server || !Client) return false;
		if (Stage==0)
		{
			if (!IsMap(Server,TEXT("SinglePlayerMap")) || !IsMap(Client,TEXT("SinglePlayerMap"))) return false;
			TArray<AECPPlayerBase*> Players;
			for (TActorIterator<AECPPlayerBase> It(Server);It;++It)
				if (It->GetController() && It->GetController()->IsPlayerController()) Players.Add(*It);
			if (Players.Num()!=2) return false;
			AECPPlayerCountActor* Counter=nullptr;
			for (TActorIterator<AECPPlayerCountActor> It(Server);It;++It) { Counter=*It; break; }
			UBoxComponent* Box=Counter ? Counter->FindComponentByClass<UBoxComponent>() : nullptr;
			if (!Counter || !Box) { Test->AddError(TEXT("Real lobby has no BP_Actor Box")); return Finish(); }
			// Nonreplicated client actors can report Authority. The runtime function must still reject them.
			AECPPlayerCountActor* ClientCounter=Client->SpawnActor<AECPPlayerCountActor>();
			if (!ClientCounter) { Test->AddError(TEXT("Client guard fixture failed")); return Finish(); }
			Test->TestTrue(TEXT("Client fixture is locally authoritative"),ClientCounter->HasAuthority());
			Test->TestFalse(TEXT("Client world cannot trigger server travel even with local actor Authority"),
				ClientCounter->TravelAllPlayersToLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Project/World/Maps/Demo.Demo")))));
			Test->TestTrue(TEXT("Client rejection leaves its travel state unchanged"),Client->NextURL.IsEmpty());
			ClientCounter->Destroy();
			Test->TestEqual(TEXT("Actual lobby server has two players"),Counter->GetCurrentWorldPlayerCount(),2);
			Center=Box->GetComponentLocation(); First=Players[0]; Second=Players[1]; LobbyCounter=Counter;
			for (AECPPlayerBase* Player:Players) Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Second->SetActorLocation(Center+FVector(600,0,0),false,nullptr,ETeleportType::TeleportPhysics);
			First->SetActorLocation(Center+FVector(0,-45,0),false,nullptr,ETeleportType::TeleportPhysics);
			Stage=1; StageStarted=FPlatformTime::Seconds(); return false;
		}
		if (Stage==1 && FPlatformTime::Seconds()-StageStarted>0.5)
		{
			if (!Second.IsValid() || !LobbyCounter.IsValid()) { Test->AddError(TEXT("Lobby fixture vanished before all players arrived")); return Finish(); }
			Test->TestTrue(TEXT("One player inside leaves server in lobby"),IsMap(Server,TEXT("SinglePlayerMap")));
			Test->TestFalse(TEXT("One player cannot complete the actual Box"),LobbyCounter->AreAllCurrentPlayersInsideBox());
			// Entering the real authored Box must execute its Blueprint condition and new travel node.
			Second->SetActorLocation(Center+FVector(0,45,0),false,nullptr,ETeleportType::TeleportPhysics);
			Stage=2; return false;
		}
		if (Stage==2 && IsMap(Server,TEXT("Demo")) && IsMap(Client,TEXT("Demo")))
		{
			int32 ServerPlayers=0;
			for (TActorIterator<AECPPlayerBase> It(Server);It;++It)
				if (It->GetController() && It->GetController()->IsPlayerController()) ++ServerPlayers;
			APlayerController* Local=Client->GetFirstPlayerController();
			if (ServerPlayers!=2 || !Local || !Cast<AECPPlayerBase>(Local->GetPawn())) return false;
			Test->TestEqual(TEXT("Both players remain connected on Demo server"),ServerPlayers,2);
			Test->TestTrue(TEXT("Demo keeps the server listening"),Server->GetNetMode()==NM_ListenServer);
			Test->TestEqual(TEXT("Server travel preserves a nondefault session port"),Server->URL.Port,17883);
			Test->TestTrue(TEXT("Demo uses its own GameMode rather than the lobby URL override"),Server->GetAuthGameMode()
				&& Server->GetAuthGameMode()->GetClass()->GetPathName()==TEXT("/Game/Project/Interaction/Framework/Zxx/BP_ECGameMode.BP_ECGameMode_C"));
			Test->TestTrue(TEXT("Remote player's client arrives at production Demo"),IsMap(Client,TEXT("Demo")));
			UE_LOG(LogTemp,Display,TEXT("ECP_TRAVEL_PIE_PASS server=%s client=%s players=%d"),
				*Server->GetOutermost()->GetName(),*Client->GetOutermost()->GetName(),ServerPlayers);
			return Finish();
		}
		return false;
	}
private:
	bool Finish() { GEditor->RequestEndPlayMap(); return true; }
	FAutomationTestBase* Test;
	double Started; double StageStarted=0; int32 Stage=0;
	FVector Center=FVector::ZeroVector;
	TWeakObjectPtr<AECPPlayerBase> First,Second;
	TWeakObjectPtr<AECPPlayerCountActor> LobbyCounter;
};

class FWaitEnd final:public IAutomationLatentCommand
{
	bool Update() override
	{
		for (const FWorldContext& Context:GEngine->GetWorldContexts())
			if (Context.WorldType==EWorldType::PIE) return false;
		ECPPIETestCleanup::CleanupSourceWorld(TEXT("/Game/zxx/BMMCSystem/Maps/SinglePlayer/SinglePlayerMap.SinglePlayerMap"));
		ECPPIETestCleanup::CleanupSourceWorld(TEXT("/Game/Project/World/Maps/Demo.Demo"));
		return true;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPMultiplayerLevelTravelTest,"ECP.Interaction.Level.MultiplayerTravelPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPMultiplayerLevelTravelTest::RunTest(const FString& Parameters)
{
	if (!GEditor || GEditor->PlayWorld) { AddError(TEXT("Travel PIE requires an idle editor")); return false; }
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),EAutomationExpectedErrorFlags::Contains,-1);
	ULevelEditorPlaySettings* Settings=DuplicateObject<ULevelEditorPlaySettings>(GetMutableDefault<ULevelEditorPlaySettings>(),GetTransientPackage());
	Settings->SetPlayNetMode(PIE_ListenServer); Settings->SetRunUnderOneProcess(true);
	Settings->SetPlayNumberOfClients(2); Settings->SetServerPort(17883);
	Settings->NetworkEmulationSettings.bIsNetworkEmulationEnabled=false;
	FRequestPlaySessionParams Params;
	Params.EditorPlaySettings=Settings;
	Params.GlobalMapOverride=TEXT("/Game/zxx/BMMCSystem/Maps/SinglePlayer/SinglePlayerMap");
	Params.bAllowOnlineSubsystem=false;
	GEditor->RequestPlaySession(Params);
	AddCommand(new ECPLevelTravelPIE::FTravelCommand(this)); AddCommand(new ECPLevelTravelPIE::FWaitEnd());
	return true;
}

#endif
