#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Actors/ECPPlayerCountActor.h"
#if WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Components/BoxComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_CallFunction.h"
#endif
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPlayerCountActorTest,"ECP.Interaction.Level.CurrentWorldPlayerCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPPlayerCountActorTest::RunTest(const FString& Parameters)
{
	UClass* CounterClass=FindObject<UClass>(nullptr,TEXT("/Script/ECPInteraction.ECPPlayerCountActor"));
	if (!TestNotNull(TEXT("BP_Actor 的 ECPInteraction 原生父类存在"),CounterClass)) return false;
	UFunction* Function=CounterClass->FindFunctionByName(TEXT("GetCurrentWorldPlayerCount"));
	if (!TestNotNull(TEXT("父类公开服务器蓝图人数函数"),Function)) return false;
	TestTrue(TEXT("函数可在蓝图调用"),Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	TestTrue(TEXT("函数标记为服务器专用"),Function->HasAnyFunctionFlags(FUNC_BlueprintAuthorityOnly));
	UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,TEXT("ECPPlayerCountWorld"));
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL()); World->BeginPlay();
	AActor* Counter=World->SpawnActor<AActor>(CounterClass);
	auto Count=[Counter,Function]() { int32 Result=-1; Counter->ProcessEvent(Function,&Result); return Result; };
	TestEqual(TEXT("无人加入时为零"),Count(),0);
	APlayerController* First=World->SpawnActor<APlayerController>();
	APlayerController* Second=World->SpawnActor<APlayerController>();
	APawn* FirstPawn=World->SpawnActor<APawn>();
	APawn* SecondPawn=World->SpawnActor<APawn>();
	First->Possess(FirstPawn); Second->Possess(SecondPawn);
	TestEqual(TEXT("服务器统计两个实际控制角色的玩家"),Count(),2);
	Second->UnPossess();
	TestEqual(TEXT("未控制角色的玩家不阻塞全员到齐"),Count(),1);
	Second->Possess(World->SpawnActor<ASpectatorPawn>());
	TestEqual(TEXT("旁观者不计入需要进入 BOX 的玩家"),Count(),1);
	Counter->SetRole(ROLE_SimulatedProxy);
	TestEqual(TEXT("客户端调用不读取本地人数"),Count(),0);
	Counter->SetRole(ROLE_Authority);
	World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPlayerCountBlueprintDiagnostic,"ECP.Interaction.Level.BPActorWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPPlayerCountBlueprintDiagnostic::RunTest(const FString& Parameters)
{
	UBlueprint* BP=LoadObject<UBlueprint>(nullptr,TEXT("/Game/Project/Interaction/Actor/BP_Actor.BP_Actor"));
	if (!TestNotNull(TEXT("BP_Actor"),BP)) return false;
	bool bBoxCollision=false;
	if (BP->SimpleConstructionScript)
	{
		for (USCS_Node* Node:BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate) continue;
			if (UBoxComponent* Box=Cast<UBoxComponent>(Node->ComponentTemplate))
			{
				bBoxCollision=Box->GetGenerateOverlapEvents() && Box->GetCollisionEnabled()==ECollisionEnabled::QueryOnly
					&& Box->GetCollisionResponseToChannel(ECC_Pawn)==ECR_Overlap;
			}
		}
	}
	bool bBoxEvent=false;
	bool bConnectedCondition=false;
	bool bLiveCheck=false;
	bool bSynchronizedTravel=false;
	bool bLocalOpenLevel=false;
	for (UEdGraph* Graph:BP->UbergraphPages)
	{
		for (UEdGraphNode* Node:Graph->Nodes)
		{
			if (const UK2Node_CallFunction* Call=Cast<UK2Node_CallFunction>(Node))
			{
				const UEdGraphPin* Execute=Call->FindPin(TEXT("execute"));
				if (Execute && !Execute->LinkedTo.IsEmpty())
				{
					bLocalOpenLevel |= Call->FunctionReference.GetMemberName()==TEXT("OpenLevel")
						|| Call->FunctionReference.GetMemberName()==TEXT("OpenLevelBySoftObjectPtr");
					if (Call->FunctionReference.GetMemberName()==TEXT("TravelAllPlayersToLevel"))
					{
						const UEdGraphPin* Map=Call->FindPin(TEXT("DestinationLevel"));
						bSynchronizedTravel=Map && Map->DefaultValue.Contains(TEXT("/Game/Project/World/Maps/Demo.Demo"));
					}
				}
			}
			if (const UK2Node_ComponentBoundEvent* Event=Cast<UK2Node_ComponentBoundEvent>(Node))
			{
				if (Event->DelegatePropertyName==TEXT("OnComponentBeginOverlap") && Event->ComponentPropertyName==TEXT("Box")) bBoxEvent=true;
			}
			if (const UK2Node_IfThenElse* Branch=Cast<UK2Node_IfThenElse>(Node))
			{
				const UEdGraphPin* Execute=Branch->FindPin(TEXT("execute"));
				const UEdGraphPin* Condition=Branch->FindPin(TEXT("Condition"));
				bConnectedCondition=Execute && !Execute->LinkedTo.IsEmpty() && Condition && !Condition->LinkedTo.IsEmpty();
				if (bConnectedCondition)
				{
					const UK2Node_CallFunction* Call=Cast<UK2Node_CallFunction>(Condition->LinkedTo[0]->GetOwningNode());
					bLiveCheck=Call && Call->FunctionReference.GetMemberName()==TEXT("AreAllCurrentPlayersInsideBox");
				}
			}
		}
	}
	TestTrue(TEXT("重叠事件绑定 Box 而非 StaticMesh"),bBoxEvent);
	TestTrue(TEXT("Box 对 Pawn 开启查询重叠"),bBoxCollision);
	TestTrue(TEXT("关卡跳转条件分支有执行线和布尔输入"),bConnectedCondition);
	TestTrue(TEXT("条件使用服务器实时 Box 查询"),bLiveCheck);
	TestTrue(TEXT("全员到齐后调用服务器同步跳转并指定生产 Demo"),bSynchronizedTravel);
	TestFalse(TEXT("不执行会断开客户端的本地 OpenLevel"),bLocalOpenLevel);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPServerTravelContractTest,"ECP.Interaction.Level.ServerTravelContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPServerTravelContractTest::RunTest(const FString& Parameters)
{
	UFunction* Function=AECPPlayerCountActor::StaticClass()->FindFunctionByName(TEXT("TravelAllPlayersToLevel"));
	if (!TestNotNull(TEXT("公开同步切换关卡函数"),Function)) return false;
	TestTrue(TEXT("蓝图可调用且仅服务器执行"),Function->HasAllFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintAuthorityOnly));
	UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,TEXT("ECPServerTravelContractWorld"));
	FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	UGameInstance* Instance=NewObject<UGameInstance>(GEngine);
	Context.OwningGameInstance=Instance;
	World->SetGameInstance(Instance);
	World->SetGameMode(FURL());
	World->InitializeActorsForPlay(FURL()); World->BeginPlay();
	AECPPlayerCountActor* Actor=World->SpawnActor<AECPPlayerCountActor>();
	struct FCall { TSoftObjectPtr<UWorld> DestinationLevel; bool ReturnValue=false; };
	auto Travel=[Actor,Function](const TCHAR* Path) {
		FCall Params; Params.DestinationLevel=TSoftObjectPtr<UWorld>(FSoftObjectPath(Path));
		Actor->ProcessEvent(Function,&Params); return Params.ReturnValue;
	};
	TestFalse(TEXT("未指定地图不发起跳转"),Travel(TEXT("")));
	TestFalse(TEXT("不存在的地图不发起跳转"),Travel(TEXT("/Game/Project/World/Maps/MissingTravelMap.MissingTravelMap")));
	TestTrue(TEXT("无效目标不改变世界的待跳转 URL"),World->NextURL.IsEmpty());
	Actor->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("无服务器权限时不接受跳转"),Travel(TEXT("/Engine/Maps/Entry.Entry")));
	Actor->SetRole(ROLE_Authority);
	TestTrue(TEXT("有效目标通过服务器跳转接口"),Travel(TEXT("/Engine/Maps/Entry.Entry")));
	TestTrue(TEXT("世界已排队目标地图"),World->NextURL.Contains(TEXT("/Engine/Maps/Entry")));
	TestFalse(TEXT("跳转过程中其他目标不会重复覆盖"),Travel(TEXT("/Game/Project/World/Maps/Demo.Demo")));
	World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}

#endif
#endif
