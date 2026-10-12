#if WITH_EDITOR

#include "Actors/ECPPlayerCountActor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "HAL/IConsoleManager.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Explicit editor repair, scoped to this one Blueprint; no runtime asset writes.
static FAutoConsoleCommand ECPBindLevelTravel(
	TEXT("ECP.Level.BindServerTravel"),
	TEXT("Replace BP_Actor's OpenLevel node with the server-authoritative map travel function."),
	FConsoleCommandDelegate::CreateLambda([]
	{
		UBlueprint* BP=LoadObject<UBlueprint>(nullptr,TEXT("/Game/Project/Interaction/Actor/BP_Actor.BP_Actor"));
		if (!BP || !BP->ParentClass->IsChildOf(AECPPlayerCountActor::StaticClass())) return;
		TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
		TArray<UK2Node_CallFunction*> OldNodes;
		for (UEdGraph* Graph:Graphs)
		{
			for (UEdGraphNode* Node:Graph->Nodes)
			{
				UK2Node_CallFunction* Call=Cast<UK2Node_CallFunction>(Node);
				if (!Call) continue;
				UE_LOG(LogTemp,Display,TEXT("ECP_TRAVEL_BIND_CALL %s function=%s"),*Call->GetName(),*Call->FunctionReference.GetMemberName().ToString());
				const FName Name=Call->FunctionReference.GetMemberName();
				if (Name==TEXT("OpenLevel") || Name==TEXT("OpenLevelBySoftObjectPtr")) OldNodes.Add(Call);
			}
		}
		if (OldNodes.Num()!=1)
		{
			UE_LOG(LogTemp,Error,TEXT("ECP_TRAVEL_BIND aborted: expected one OpenLevel, found %d"),OldNodes.Num()); return;
		}
		UK2Node_CallFunction* Old=OldNodes[0];
		UEdGraph* Graph=Old->GetGraph();
		UFunction* Function=AECPPlayerCountActor::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(AECPPlayerCountActor,TravelAllPlayersToLevel));
		if (!Function || !Old->FindPin(TEXT("execute"))) return;
		BP->Modify(); Graph->Modify();
		UK2Node_CallFunction* Call=NewObject<UK2Node_CallFunction>(Graph);
		Call->SetFromFunction(Function); Call->CreateNewGuid(); Call->AllocateDefaultPins();
		Call->NodePosX=Old->NodePosX; Call->NodePosY=Old->NodePosY;
		Graph->AddNode(Call,false,false);
		const UEdGraphSchema* Schema=Graph->GetSchema();
		for (const FName Name:{FName(TEXT("execute")),FName(TEXT("then"))})
		{
			UEdGraphPin* Source=Old->FindPin(Name); UEdGraphPin* Target=Call->FindPin(Name);
			if (!Source || !Target || !Schema->MovePinLinks(*Source,*Target).CanSafeConnect())
			{
				UE_LOG(LogTemp,Error,TEXT("ECP_TRAVEL_BIND failed to move %s links; no package saved"),*Name.ToString()); return;
			}
		}
		UEdGraphPin* Map=Call->FindPin(TEXT("DestinationLevel"));
		if (!Map) return;
		Schema->TrySetDefaultValue(*Map,TEXT("/Game/Project/World/Maps/Demo.Demo"));
		FBlueprintEditorUtils::RemoveNode(BP,Old,true);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
		if (BP->Status==BS_Error)
		{
			UE_LOG(LogTemp,Error,TEXT("ECP_TRAVEL_BIND Blueprint compilation failed; no package saved")); return;
		}
		FSavePackageArgs Args; Args.TopLevelFlags=RF_Public | RF_Standalone;
		const bool bSaved=UPackage::SavePackage(BP->GetOutermost(),BP,
			*FPackageName::LongPackageNameToFilename(TEXT("/Game/Project/Interaction/Actor/BP_Actor"),TEXT(".uasset")),Args);
		UE_LOG(LogTemp,Display,TEXT("ECP_TRAVEL_BIND %s"),bSaved ? TEXT("PASS") : TEXT("FAIL"));
	}));

#endif
