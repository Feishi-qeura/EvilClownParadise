#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Engine.h"
#include "Engine/PrimaryAssetLabel.h"
#include "GameFeatureData.h"
#include "GameFeaturesSubsystem.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

// Content cleanup retired the one-off migration manifest and Archive.
// Keep checking the live runtime contract without requiring historical backups.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRuntimeAssetsTest, "ECP.Features.RuntimeAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FECPRuntimeAssetsTest::RunTest(const FString& Parameters)
{
    TArray<TStrongObjectPtr<UObject>> KeepAlive;
    UGameFeaturesSubsystem* Features = GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("Game Features subsystem exists"), Features)) return false;
    const TCHAR* Names[] = {TEXT("ECPCharacter"), TEXT("ECPInteraction"), TEXT("ECPAI"), TEXT("ECPOnline"), TEXT("ECPWorld")};
    for (const TCHAR* Name : Names)
    {
        const FString DataPath = FString::Printf(TEXT("/%s/%s.%s"), Name, Name, Name);
        UGameFeatureData* Data = LoadObject<UGameFeatureData>(nullptr, *DataPath);
        if (TestNotNull(*FString::Printf(TEXT("Feature data loads: %s"), Name), Data)) KeepAlive.Emplace(Data);
        const FString LabelPath = FString::Printf(TEXT("/%s/PAL_%s.PAL_%s"), Name, Name, Name);
        UPrimaryAssetLabel* Label = LoadObject<UPrimaryAssetLabel>(nullptr, *LabelPath);
        if (TestNotNull(*FString::Printf(TEXT("Feature label loads: %s"), Name), Label)) KeepAlive.Emplace(Label);
        TestTrue(FString::Printf(TEXT("Feature activated through normal startup: %s"), Name),
            Features->IsGameFeaturePluginActiveByName(FStringView(Name), false));
    }
    TArray<FString> MapEntries;
    GConfig->GetArray(TEXT("/Script/UnrealEd.ProjectPackagingSettings"), TEXT("MapsToCook"), MapEntries, GGameIni);
    TestTrue(TEXT("Production maps are explicitly configured for Cook"), MapEntries.Num() >= 3);
    for (const FString& Entry : MapEntries)
    {
        FString Map;
        if (!TestTrue(TEXT("Cook map has a valid package name"),
            FParse::Value(*Entry, TEXT("FilePath="), Map) && FPackageName::IsValidLongPackageName(Map))) continue;
        TestTrue(FString::Printf(TEXT("Cook map exists: %s"), *Map), FPackageName::DoesPackageExist(Map));
        TestFalse(TEXT("Cook map must not reference Archive"), Map.StartsWith(TEXT("/Game/Project/Archive/")));
    }
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.SearchAllAssets(true);
    TArray<FAssetData> Assets;
    Registry.GetAssetsByPath(FName(TEXT("/Game/Project")), Assets, true);
    TestTrue(TEXT("Project contains production assets"), !Assets.IsEmpty());
    for (const FAssetData& Asset : Assets)
    {
        if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/Project/Archive/"))) continue;
        TArray<FName> Dependencies;
        Registry.GetDependencies(Asset.PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
        for (FName Dependency : Dependencies)
        {
            const FString Package = Dependency.ToString();
            if (!Package.StartsWith(TEXT("/Game/"))) continue;
            TestFalse(FString::Printf(TEXT("No Archive dependency: %s -> %s"), *Asset.PackageName.ToString(), *Package),
                Package.StartsWith(TEXT("/Game/Project/Archive/")));
            // Validate owned package references. External metadata includes optional
            // preview meshes and string-table IDs resolved by engine redirects;
            // production loading and Cook validate their actual runtime use.
            if (Package.StartsWith(TEXT("/Game/Project/")))
                TestTrue(FString::Printf(TEXT("Project dependency exists: %s -> %s"), *Asset.PackageName.ToString(), *Package),
                    FPackageName::DoesPackageExist(Package));
        }
    }
    UClass* Player = LoadObject<UClass>(nullptr,
        TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
    if (TestNotNull(TEXT("Production player loads"), Player))
    {
        KeepAlive.Emplace(Player);
        UClass* Native = FindObject<UClass>(nullptr, TEXT("/Script/ECPInteraction.ECPPlayerBase"));
        TestTrue(TEXT("Player inherits Interaction base"), Native && Player->IsChildOf(Native));
        for (const TCHAR* Name : {TEXT("PickupActorClass"), TEXT("NetworkDebugWidgetClass")})
        {
            const FClassProperty* Property = FindFProperty<FClassProperty>(Player, FName(Name));
            UClass* Value = Property ? Cast<UClass>(Property->GetObjectPropertyValue_InContainer(Player->GetDefaultObject())) : nullptr;
            if (TestNotNull(*FString::Printf(TEXT("Player default assigned: %s"), Name), Value))
                TestTrue(TEXT("Player defaults use Project assets"), Value->GetPathName().StartsWith(TEXT("/Game/Project/")));
        }
    }
    return true;
}
#endif
