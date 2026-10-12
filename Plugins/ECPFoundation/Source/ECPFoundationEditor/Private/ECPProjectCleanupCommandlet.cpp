#include "ECPProjectCleanupCommandlet.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "HAL/IConsoleManager.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Engine/StaticMesh.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogECPProjectCleanup, Log, All);

UECPProjectCleanupCommandlet::UECPProjectCleanupCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 UECPProjectCleanupCommandlet::Main(const FString& Params)
{
    FString Text;
    TSharedPtr<FJsonObject> Manifest;
    const FString Filename = FPaths::ProjectDir() / TEXT("docs/project-content-cleanup-2026-10-12.json");
    if (!FFileHelper::LoadFileToString(Text, *Filename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) || !Manifest.IsValid()) return 1;
    const TSharedPtr<FJsonObject>* Copy = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Delete = nullptr;
    if (!Manifest->TryGetObjectField(TEXT("copy"), Copy) || !Manifest->TryGetArrayField(TEXT("delete"), Delete)) return 1;
    TMap<FString, FString> Mapping;
    for (const auto& Pair : (*Copy)->Values)
    {
        const FString Source(Pair.Key);
        const FString Destination = Pair.Value->AsString();
        if (!Source.StartsWith(TEXT("/Game/Test_Map/"))
            || !Destination.StartsWith(TEXT("/Game/Project/World/"))
            || !FPackageName::IsValidLongPackageName(Destination)) return 1;
        Mapping.Add(Source, Destination);
    }
    if (FParse::Param(*Params, TEXT("Copy")))
    {
        for (const auto& Pair : Mapping)
        {
            if (!FPackageName::DoesPackageExist(Pair.Key) || FPackageName::DoesPackageExist(Pair.Value)) return 1;
        }
        // Official package copy rewrites hard/soft references within the copied dependency set.
        // Autosave is disabled so unrelated loaded packages (especially BMMCSystem) remain untouched.
        IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
        // Object duplication invokes Landscape PostEditChange before a world subsystem exists
        // in a commandlet. UE's package header patcher copies/remaps without loading that world.
        IConsoleVariable* Patching = IConsoleManager::Get().FindConsoleVariable(TEXT("AssetTools.UseHeaderPatchingAdvancedCopy"));
        if (!Patching) return 1;
        Patching->Set(1, ECVF_SetByCode);
        const bool bOK = Tools.AdvancedCopyPackages(Mapping, false, false);
        UE_LOG(LogECPProjectCleanup, Display, TEXT("COPY %s packages=%d"), bOK ? TEXT("PASS") : TEXT("FAIL"), Mapping.Num());
        return bOK ? 0 : 1;
    }
    if (!FParse::Param(*Params, TEXT("Finalize"))) return 1;
    const auto Save = [](UObject* Asset)
    {
        const FString Package = Asset->GetOutermost()->GetName();
        // Exact save allowlist, never save BMMCSystem or unrelated assets.
        if (Package != TEXT("/Game/Project/Interaction/Actor/BP_Actor")
            && Package != TEXT("/ECPWorld/PAL_ECPWorld")
            && Package != TEXT("/ECPInteraction/PAL_ECPInteraction")) return false;
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        return UPackage::SavePackage(Asset->GetOutermost(), Asset,
            *FPackageName::LongPackageNameToFilename(Package, TEXT(".uasset")), Args);
    };
    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Project/Interaction/Actor/BP_Actor.BP_Actor"));
    const FString* NewDoor = Mapping.Find(TEXT("/Game/Test_Map/Meshes/SM_door_2"));
    if (!BP || !NewDoor) return 1;
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *(*NewDoor + TEXT(".SM_door_2")));
    if (!Mesh) return 1;
    int32 MeshCount = 0;
    for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
    {
        if (It->GetOutermost() == BP->GetOutermost() && It->GetStaticMesh()
            && It->GetStaticMesh()->GetOutermost()->GetName() == TEXT("/Game/Test_Map/Meshes/SM_door_2"))
        {
            It->SetStaticMesh(Mesh);
            ++MeshCount;
        }
    }
    int32 TravelCount = 0;
    TArray<UEdGraph*> Graphs;
    BP->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (Call && Call->GetFunctionName() == TEXT("OpenLevel"))
            {
                UEdGraphPin* Pin = Call->FindPin(TEXT("LevelName"));
                if (Pin && (Pin->DefaultValue == TEXT("Demo") || Pin->DefaultValue == TEXT("/Game/Project/World/Maps/Demo")))
                {
                    Pin->DefaultValue = TEXT("/Game/Project/World/Maps/Demo");
                    ++TravelCount;
                }
            }
        }
    }
    if (TravelCount != 1) return 1;
    FKismetEditorUtilities::CompileBlueprint(BP);
    if (BP->Status == BS_Error || !Save(BP)) return 1;
    UE_LOG(LogECPProjectCleanup, Display, TEXT("BP_ACTOR mesh=%d travel=%d"), MeshCount, TravelCount);
    TSet<FString> Deleted;
    for (const auto& Value : *Delete) Deleted.Add(Value->AsString());
    for (const TCHAR* Feature : {TEXT("ECPInteraction"), TEXT("ECPWorld")})
    {
        const FString Path = FString::Printf(TEXT("/%s/PAL_%s.PAL_%s"), Feature, Feature, Feature);
        UPrimaryAssetLabel* Label = LoadObject<UPrimaryAssetLabel>(nullptr, *Path);
        if (!Label) return 1;
        Label->ExplicitAssets.RemoveAll([&Deleted](const TSoftObjectPtr<UObject>& Ref)
        {
            return Deleted.Contains(Ref.ToSoftObjectPath().GetLongPackageName());
        });
        if (FString(Feature) == TEXT("ECPWorld"))
        {
            Label->ExplicitAssets.AddUnique(TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/Project/World/Maps/Demo.Demo"))));
        }
        Label->UpdateAssetBundleData();
        if (!Save(Label)) return 1;
    }
    UE_LOG(LogECPProjectCleanup, Display, TEXT("FINALIZE PASS"));
    return 0;
}
