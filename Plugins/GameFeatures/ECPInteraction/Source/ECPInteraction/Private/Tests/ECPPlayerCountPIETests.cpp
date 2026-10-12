#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Actors/ECPPlayerCountActor.h"
#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ECPPIEWorldCleanup.h"

class FECPPlayerCountPIECommand final : public IAutomationLatentCommand
{
public:
	FECPPlayerCountPIECommand(FAutomationTestBase* InTest, AGameModeBase* InMode, TSubclassOf<APawn> InOldPawn)
		:Test(InTest),Mode(InMode),OldPawn(InOldPawn),Started(FPlatformTime::Seconds()) {}

	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if (Now-Started>45.0) { Test->AddError(TEXT("Two-player Box PIE test timed out")); return Finish(); }
		UWorld* Server=nullptr;
		int32 ClientWorlds=0;
		for (const FWorldContext& Context:GEngine->GetWorldContexts())
		{
			UWorld* World=Context.World();
			if (!World || World->WorldType!=EWorldType::PIE) continue;
			if (World->GetNetMode()==NM_ListenServer) Server=World;
			else if (World->GetNetMode()==NM_Client) ++ClientWorlds;
		}
		if (!Server || ClientWorlds<1) return false;
		TArray<ACharacter*> Players;
		for (TActorIterator<ACharacter> It(Server);It;++It)
			if (It->GetController() && It->GetController()->IsPlayerController()) Players.Add(*It);
		if (Players.Num()!=2) return false;
		if (Stage==0)
		{
			Counter=Server->SpawnActor<AECPPlayerCountActor>(FVector(0,0,200),FRotator::ZeroRotator);
			if (!Counter.IsValid()) { Test->AddError(TEXT("Counter failed to spawn")); return Finish(); }
			UBoxComponent* Box=NewObject<UBoxComponent>(Counter.Get(),TEXT("ArrivalBox"));
			Counter->SetRootComponent(Box);
			Box->SetBoxExtent(FVector(250,250,200));
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionResponseToAllChannels(ECR_Ignore);
			Box->SetCollisionResponseToChannel(ECC_Pawn,ECR_Overlap);
			Box->SetGenerateOverlapEvents(true);
			Box->RegisterComponent();
			Box->SetWorldLocation(FVector(0,0,200));
			for (ACharacter* Player:Players) Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Players[0]->SetActorLocation(FVector(0,0,200),false,nullptr,ETeleportType::TeleportPhysics);
			Players[1]->SetActorLocation(FVector(1000,0,200),false,nullptr,ETeleportType::TeleportPhysics);
			First=Players[0]; Second=Players[1]; Stage=1; StageStarted=Now; return false;
		}
		if (!First.IsValid() || !Second.IsValid() || !Counter.IsValid()) { Test->AddError(TEXT("PIE fixture actor vanished")); return Finish(); }
		if (Stage==1 && Now-StageStarted>0.75)
		{
			Test->TestEqual(TEXT("Server sees both connected pawns"),Counter->GetCurrentWorldPlayerCount(),2);
			Test->TestFalse(TEXT("One player inside cannot complete Box"),Counter->AreAllCurrentPlayersInsideBox());
			Second->SetActorLocation(FVector(0,80,200),false,nullptr,ETeleportType::TeleportPhysics);
			Stage=2; StageStarted=Now; return false;
		}
		if (Stage==2 && Now-StageStarted>0.75)
		{
			Test->TestTrue(TEXT("Both players inside complete Box"),Counter->AreAllCurrentPlayersInsideBox());
			return Finish();
		}
		return false;
	}

private:
	bool Finish() { if (Mode) Mode->DefaultPawnClass=OldPawn; GEditor->RequestEndPlayMap(); return true; }
	FAutomationTestBase* Test;
	AGameModeBase* Mode;
	TSubclassOf<APawn> OldPawn;
	TWeakObjectPtr<AECPPlayerCountActor> Counter;
	TWeakObjectPtr<ACharacter> First;
	TWeakObjectPtr<ACharacter> Second;
	double Started;
	double StageStarted=0;
	int32 Stage=0;
};

class FECPPlayerCountPIEWaitEnd final : public IAutomationLatentCommand
{
public:
	bool Update() override
	{
		for (const FWorldContext& Context:GEngine->GetWorldContexts()) if (Context.WorldType==EWorldType::PIE) return false;
		ECPPIETestCleanup::CleanupSourceWorld(TEXT("/Engine/Maps/Entry.Entry"));
		return true;
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPlayerCountPIETest,"ECP.Interaction.Level.TwoPlayerBoxPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPPlayerCountPIETest::RunTest(const FString& Parameters)
{
	if (!GEditor || GEditor->PlayWorld) { AddError(TEXT("Box PIE test requires an idle editor")); return false; }
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),EAutomationExpectedErrorFlags::Contains,-1);
	ULevelEditorPlaySettings* Settings=DuplicateObject<ULevelEditorPlaySettings>(GetMutableDefault<ULevelEditorPlaySettings>(),GetTransientPackage());
	Settings->SetPlayNetMode(PIE_ListenServer);
	Settings->SetRunUnderOneProcess(true);
	Settings->SetPlayNumberOfClients(2);
	Settings->SetServerPort(17881);
	Settings->NetworkEmulationSettings.bIsNetworkEmulationEnabled=false;
	FRequestPlaySessionParams Params;
	Params.EditorPlaySettings=Settings;
	Params.GlobalMapOverride=TEXT("/Engine/Maps/Entry");
	Params.GameModeOverride=LoadClass<AGameModeBase>(nullptr,TEXT("/Game/Project/Interaction/Framework/GM_InGame.GM_InGame_C"));
	if (!Params.GameModeOverride) { AddError(TEXT("Game mode failed to load")); return false; }
	AGameModeBase* Mode=Params.GameModeOverride->GetDefaultObject<AGameModeBase>();
	const TSubclassOf<APawn> OldPawn=Mode->DefaultPawnClass;
	Mode->DefaultPawnClass=LoadClass<APawn>(nullptr,TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	Params.bAllowOnlineSubsystem=false;
	GEditor->RequestPlaySession(Params);
	AddCommand(new FECPPlayerCountPIECommand(this,Mode,OldPawn));
	AddCommand(new FECPPlayerCountPIEWaitEnd());
	return true;
}

#endif
