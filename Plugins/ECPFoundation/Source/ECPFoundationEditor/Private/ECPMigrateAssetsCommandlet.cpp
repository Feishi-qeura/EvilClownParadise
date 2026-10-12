#include "ECPMigrateAssetsCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Engine/World.h"
#include "GameFeatureData.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogECPAssetMigration, Log, All);

namespace ECPAssetMigration
{
constexpr int32 ExpectedAssetCount = 276;
const FString ProtectedPackageRoot = TEXT("/Game/zxx/BMMCSystem");
const FString DestinationRoot = TEXT("/Game/Project/");

// 强引用跨越全部加载、重命名和保存阶段，避免编辑器加载时 GC 回收尚未写入的包。
struct FEntry
{
    FString SourcePackage;
    FString DestinationPackage;
    FString SourceFilename;
    FString DestinationFilename;
    FString AssetName;
    FString Area;
    FString Feature;
    // 只有快照确认的历史别名允许保持为 redirector；其余源必须是独立真实资产。
    FString AliasCanonicalSource;
    TStrongObjectPtr<UObject> Asset;
    TStrongObjectPtr<UPackage> SourceOuter;
    TStrongObjectPtr<UObjectRedirector> Redirector;
};

// 保存白名单同时约束包名和绝对文件名，不能只检查 /Game 前缀。
using FSaveAllowList = TMap<FString, FString>;
using FProtectedSnapshot = TMap<FString, FMD5Hash>;

bool IsProtectedPackage(const FString& Package)
{
    return Package.Equals(ProtectedPackageRoot, ESearchCase::IgnoreCase)
        || Package.StartsWith(ProtectedPackageRoot + TEXT("/"), ESearchCase::IgnoreCase);
}

FString AbsoluteFilename(const FString& Filename)
{
    FString Result = FPaths::ConvertRelativePathToFull(Filename);
    FPaths::NormalizeFilename(Result);
    FPaths::CollapseRelativeDirectories(Result);
    return Result;
}

FString ObjectPath(const FString& Package)
{
    return Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
}

const TMap<FString, FString>& FeatureAreas()
{
    static const TMap<FString, FString> Areas = {
        {TEXT("Character"), TEXT("ECPCharacter")},
        {TEXT("Interaction"), TEXT("ECPInteraction")},
        {TEXT("AI"), TEXT("ECPAI")},
        {TEXT("Online"), TEXT("ECPOnline")},
        {TEXT("World"), TEXT("ECPWorld")},
        {TEXT("Shared"), TEXT("ECPFoundation")},
        {TEXT("Archive"), TEXT("")}
    };
    return Areas;
}

bool ReadManifest(TArray<FEntry>& Entries)
{
    const FString ManifestFilename = FPaths::Combine(FPaths::ProjectDir(), TEXT("docs/game-feature-asset-migration.json"));
    FString Text;
    TSharedPtr<FJsonObject> Root;
    // 固定读取项目内清单，不接受命令行注入另一个文件或任意写入目标。
    if (!FFileHelper::LoadFileToString(Text, *ManifestFilename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot read migration manifest: %s"), *ManifestFilename);
        return false;
    }

    const TSharedPtr<FJsonObject>* Policy = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    bool bKeepRedirectors = false;
    bool bGlobalFixup = true;
    bool bSaveProtected = true;
    if (!Root->TryGetObjectField(TEXT("policy"), Policy) || !Policy || !Policy->IsValid()
        || !(*Policy)->TryGetBoolField(TEXT("preserveAllOldRedirectors"), bKeepRedirectors)
        || !(*Policy)->TryGetBoolField(TEXT("allowGlobalFixupRedirectors"), bGlobalFixup)
        || !(*Policy)->TryGetBoolField(TEXT("allowProtectedReferencerResave"), bSaveProtected)
        || !bKeepRedirectors || bGlobalFixup || bSaveProtected
        || !Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets || Assets->Num() != ExpectedAssetCount)
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Manifest must contain exactly 276 assets and the protected redirector policy."));
        return false;
    }

    TSet<FString> SourceNames;
    TSet<FString> DestinationNames;
    const FString ProjectContent = AbsoluteFilename(FPaths::ProjectContentDir());
    for (const TSharedPtr<FJsonValue>& Value : *Assets)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        FEntry Entry;
        FString SourceFile;
        if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row || !Row->IsValid()
            || !(*Row)->TryGetStringField(TEXT("sourceFile"), SourceFile)
            || !(*Row)->TryGetStringField(TEXT("sourcePackage"), Entry.SourcePackage)
            || !(*Row)->TryGetStringField(TEXT("destinationPackage"), Entry.DestinationPackage)
            || !(*Row)->TryGetStringField(TEXT("area"), Entry.Area)
            || !(*Row)->TryGetStringField(TEXT("feature"), Entry.Feature))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Manifest asset entry has missing or invalid fields."));
            return false;
        }

        const FString* ExpectedFeature = FeatureAreas().Find(Entry.Area);
        Entry.AssetName = FPackageName::GetLongPackageAssetName(Entry.SourcePackage);
        const bool bSourceRootAllowed = Entry.SourcePackage.StartsWith(TEXT("/Game/_Game/"))
            || Entry.SourcePackage.StartsWith(TEXT("/Game/zxx/"));
        // 禁止跨目录、保护目录、改对象名和同名目标覆盖；Archive 必须接收所有历史备份。
        if (!bSourceRootAllowed || IsProtectedPackage(Entry.SourcePackage)
            || !ExpectedFeature || Entry.Feature != *ExpectedFeature
            || !Entry.DestinationPackage.StartsWith(DestinationRoot + Entry.Area + TEXT("/"))
            || !FPackageName::IsValidLongPackageName(Entry.SourcePackage)
            || !FPackageName::IsValidLongPackageName(Entry.DestinationPackage)
            || Entry.AssetName != FPackageName::GetLongPackageAssetName(Entry.DestinationPackage)
            || (Entry.SourcePackage.Contains(TEXT("/_Backup")) && Entry.Area != TEXT("Archive"))
            || SourceFile.Contains(TEXT("..")) || SourceFile.Contains(TEXT("\\"))
            || SourceNames.Contains(Entry.SourcePackage.ToLower())
            || DestinationNames.Contains(Entry.DestinationPackage.ToLower()))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Unsafe or duplicate mapping: %s -> %s"),
                *Entry.SourcePackage, *Entry.DestinationPackage);
            return false;
        }

        const FString Extension = FPaths::GetExtension(SourceFile, true);
        if (Extension != TEXT(".uasset") && Extension != TEXT(".umap"))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Unsupported source extension: %s"), *SourceFile);
            return false;
        }

        Entry.SourceFilename = AbsoluteFilename(FPaths::Combine(FPaths::ProjectDir(), SourceFile));
        Entry.DestinationFilename = AbsoluteFilename(FPackageName::LongPackageNameToFilename(
            Entry.DestinationPackage, Extension));
        const FString PackageSourceFilename = AbsoluteFilename(FPackageName::LongPackageNameToFilename(
            Entry.SourcePackage, Extension));
        // 文件名必须由对应包名推导且确实留在项目 Content 中，避免 JSON 中两套路径指向不同文件。
        if (Entry.SourceFilename != PackageSourceFilename
            || !FPaths::IsUnderDirectory(Entry.SourceFilename, ProjectContent)
            || !FPaths::IsUnderDirectory(Entry.DestinationFilename, ProjectContent)
            || !IFileManager::Get().FileExists(*Entry.SourceFilename))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Source file does not match an existing project package: %s"), *SourceFile);
            return false;
        }
        SourceNames.Add(Entry.SourcePackage.ToLower());
        DestinationNames.Add(Entry.DestinationPackage.ToLower());
        Entries.Add(MoveTemp(Entry));
    }
    return true;
}


// 快照只允许六个直接玩家蓝图上的两项类默认值；强引用在重命名前后保持同一个 UClass 对象。
struct FBlueprintDefault
{
    FString SourcePackage;
    FName PropertyName;
    FString SourceClassPath;
    TStrongObjectPtr<UClass> ClassValue;
};

FEntry* FindSourceEntry(TArray<FEntry>& Entries, const FString& Package)
{
    return Entries.FindByPredicate([&Package](const FEntry& Entry) { return Entry.SourcePackage == Package; });
}

bool ReadBlueprintSnapshot(TArray<FEntry>& Entries, TArray<FBlueprintDefault>& Defaults)
{
    FString Text;
    TSharedPtr<FJsonObject> Root;
    const FString Filename = FPaths::Combine(FPaths::ProjectDir(), TEXT("docs/game-feature-blueprint-defaults.json"));
    if (!FFileHelper::LoadFileToString(Text, *Filename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot read reviewed blueprint defaults snapshot: %s"), *Filename);
        return false;
    }
    const TSharedPtr<FJsonObject>* Players = nullptr;
    const TSharedPtr<FJsonObject>* Aliases = nullptr;
    const TSharedPtr<FJsonObject>* Errors = nullptr;
    if (Root->Values.Num() != 3
        || !Root->TryGetObjectField(TEXT("players"), Players) || !Players || !Players->IsValid()
        || !Root->TryGetObjectField(TEXT("existingRedirectors"), Aliases) || !Aliases || !Aliases->IsValid()
        || !Root->TryGetObjectField(TEXT("errors"), Errors) || !Errors || !Errors->IsValid()
        || (*Players)->Values.Num() != 6 || (*Aliases)->Values.Num() != 1 || !(*Errors)->Values.IsEmpty())
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Snapshot must contain exactly 6 players, 1 existing alias, and no errors."));
        return false;
    }

    // 同时限定资产、属性和原值，快照不能扩大本工具的写入范围或悄悄配置其他蓝图。
    const TSet<FString> ApprovedPlayers = {
        TEXT("/Game/_Game/Blueprints/BP_Player"),
        TEXT("/Game/zxx/BluePrint/AnimationBlueprint/_Backup_20260917/BP_ECPlayer"),
        TEXT("/Game/zxx/BluePrint/AnimationBlueprint/_Backup_NativeMigration_20260920/BP_ECPlayertest"),
        TEXT("/Game/zxx/BluePrint/AnimationBlueprint/_Backup_NetworkFix_20260917/BP_ECPlayer"),
        TEXT("/Game/zxx/BluePrint/AnimationBlueprint/_Backup_PoseSync_20260918/BP_ECPlayer"),
        TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest")
    };
    const TMap<FString, FString> ApprovedProperties = {
        {TEXT("PickupActorClass"), TEXT("/Game/zxx/BluePrint/thing/BP_Smallitems.BP_Smallitems_C")},
        {TEXT("NetworkDebugWidgetClass"), TEXT("/Game/zxx/Widget/BP_NetworkDebug.BP_NetworkDebug_C")}
    };
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Player : (*Players)->Values)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        FString Parent;
        if (!ApprovedPlayers.Contains(Player.Key) || !FindSourceEntry(Entries, Player.Key)
            || !Player.Value.IsValid() || !Player.Value->TryGetObject(Row) || !Row || !Row->IsValid()
            || (*Row)->Values.Num() != 2 || !(*Row)->TryGetStringField(TEXT("parent"), Parent)
            || Parent != TEXT("/Script/CoreUObject.Class'/Script/EvilClownParadise.ECPPlayerBase'")
            || !(*Row)->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->IsValid()
            || (*Properties)->Values.Num() != 2)
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Unapproved player snapshot entry: %s"), *Player.Key);
            return false;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : (*Properties)->Values)
        {
            const FString* ApprovedPath = ApprovedProperties.Find(Property.Key);
            const TSharedPtr<FJsonObject>* Reference = nullptr;
            FBlueprintDefault Default;
            Default.SourcePackage = Player.Key;
            Default.PropertyName = FName(*Property.Key);
            if (!ApprovedPath || !Property.Value.IsValid()
                || !Property.Value->TryGetObject(Reference) || !Reference || !Reference->IsValid()
                || (*Reference)->Values.Num() != 1
                || !(*Reference)->TryGetStringField(TEXT("refPath"), Default.SourceClassPath)
                || Default.SourceClassPath != *ApprovedPath
                || !FindSourceEntry(Entries, FPackageName::ObjectPathToPackageName(Default.SourceClassPath)))
            {
                UE_LOG(LogECPAssetMigration, Error, TEXT("Unapproved class default: %s.%s"), *Player.Key, *Property.Key);
                return false;
            }
            Defaults.Add(MoveTemp(Default));
        }
    }

    const FString AliasSource = TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayer");
    const FString CanonicalSource = TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest");
    FString Target;
    FEntry* AliasEntry = FindSourceEntry(Entries, AliasSource);
    if (!AliasEntry || !FindSourceEntry(Entries, CanonicalSource)
        || !(*Aliases)->TryGetStringField(AliasSource, Target)
        || FPackageName::ExportTextPathToObjectPath(Target) != ObjectPath(CanonicalSource))
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Snapshot alias does not match the single approved canonical player."));
        return false;
    }
    AliasEntry->AliasCanonicalSource = CanonicalSource;
    UE_LOG(LogECPAssetMigration, Display, TEXT("Reviewed snapshot: 6 player blueprints, 12 class defaults, 1 existing alias."));
    return Defaults.Num() == 12;
}

bool CheckMigrationFiles(const TArray<FEntry>& Entries, bool bRequireWritable)
{
    int32 ReadOnlyCount = 0;
    for (const FEntry& Entry : Entries)
    {
        // 同包的两种二进制扩展名都检查；不能在旧地图旁留下同包名的另一个资产文件。
        const FString AssetDestination = FPackageName::LongPackageNameToFilename(Entry.DestinationPackage, TEXT(".uasset"));
        const FString MapDestination = FPackageName::LongPackageNameToFilename(Entry.DestinationPackage, TEXT(".umap"));
        if (IFileManager::Get().FileExists(*AssetDestination)
            || IFileManager::Get().FileExists(*MapDestination)
            || FindPackage(nullptr, *Entry.DestinationPackage))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Destination already exists; refusing overwrite: %s"), *Entry.DestinationPackage);
            return false;
        }
        if (IFileManager::Get().IsReadOnly(*Entry.SourceFilename))
        {
            ++ReadOnlyCount;
            if (bRequireWritable)
            {
                UE_LOG(LogECPAssetMigration, Error, TEXT("Source is read-only. Acquire its LFS lock before retrying: %s"), *Entry.SourceFilename);
            }
        }
    }
    UE_LOG(LogECPAssetMigration, Display, TEXT("Manifest: %d assets, %d read-only source files; no files were unlocked."),
        Entries.Num(), ReadOnlyCount);
    return !bRequireWritable || ReadOnlyCount == 0;
}


bool SnapshotProtectedFiles(FProtectedSnapshot& Snapshot)
{
    TArray<FString> Filenames;
    const FString ProtectedDirectory = AbsoluteFilename(
        FPaths::Combine(FPaths::ProjectContentDir(), TEXT("zxx/BMMCSystem")));
    IFileManager::Get().FindFilesRecursive(Filenames, *ProtectedDirectory, TEXT("*"), true, false, false);
    // 快照覆盖整个保护目录，不只覆盖当前已知的地图，能发现意外新增、删除或写入。
    if (Filenames.IsEmpty())
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Protected directory is missing or empty; refusing a write mode."));
        return false;
    }
    UE_LOG(LogECPAssetMigration, Display, TEXT("Protected snapshot START: %d files."), Filenames.Num());
    for (const FString& Filename : Filenames)
    {
        // 使用 API 默认缓冲区，消除传入空复用缓冲区的可疑调用条件；不依赖未经确认的引擎内部实现。
        const FMD5Hash Hash = FMD5Hash::HashFile(*Filename);
        if (!Hash.IsValid())
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot hash protected file: %s"), *Filename);
            return false;
        }
        Snapshot.Add(AbsoluteFilename(Filename), Hash);
    }
    UE_LOG(LogECPAssetMigration, Display, TEXT("Protected snapshot COMPLETE: %d files."), Snapshot.Num());
    return true;
}

bool VerifyProtectedFiles(const FProtectedSnapshot& Before)
{
    FProtectedSnapshot After;
    if (!SnapshotProtectedFiles(After) || Before.Num() != After.Num())
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Protected file inventory changed; manual investigation required."));
        return false;
    }
    for (const TPair<FString, FMD5Hash>& Pair : Before)
    {
        const FMD5Hash* Actual = After.Find(Pair.Key);
        if (!Actual || *Actual != Pair.Value)
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Protected file changed: %s"), *Pair.Key);
            return false;
        }
    }
    UE_LOG(LogECPAssetMigration, Display, TEXT("Protected BMMCSystem verified unchanged: %d files."), Before.Num());
    return true;
}

bool SaveExactPackage(UPackage* Package, UObject* Asset, const FString& Filename, const FSaveAllowList& Allowed)
{
    // 任何调用方都必须经过本检查；不给 SaveAll、保存脏包或递归目录接口机会。
    if (!Package || !Asset)
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot save a null package or asset."));
        return false;
    }
    const FString PackageName = Package->GetName();
    const FString* AllowedFilename = Allowed.Find(PackageName);
    if (IsProtectedPackage(PackageName) || !AllowedFilename
        || *AllowedFilename != AbsoluteFilename(Filename) || Asset->GetOutermost() != Package)
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Save rejected by exact allowlist: %s -> %s"), *PackageName, *Filename);
        return false;
    }
    if (IFileManager::Get().FileExists(*Filename) && IFileManager::Get().IsReadOnly(*Filename))
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Save target became read-only; no permission changed: %s"), *Filename);
        return false;
    }
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot create allowed destination directory: %s"), *Filename);
        return false;
    }

    // 显式 InOuter 将序列化范围限制为本包；同包的蓝图附属类与重定向器一并保留。
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_None;
    Args.bSlowTask = false;
    Args.Error = GWarn;
    const bool bSaved = UPackage::SavePackage(Package, Asset, *Filename, Args);
    UE_LOG(LogECPAssetMigration, Display, TEXT("SAVE %s %s"), bSaved ? TEXT("OK") : TEXT("FAILED"), *Filename);
    return bSaved;
}

bool MountFeatureModules()
{
    // 旧蓝图通过 CoreRedirects 指向这些原生类；ExplicitlyLoaded 插件必须先挂载才能解析类。
    const TCHAR* Names[] = {TEXT("ECPCharacter"), TEXT("ECPInteraction"), TEXT("ECPAI"), TEXT("ECPOnline"), TEXT("ECPWorld")};
    for (const TCHAR* Name : Names)
    {
        if (!IPluginManager::Get().FindPlugin(Name).IsValid()
            || !IPluginManager::Get().MountExplicitlyLoadedPlugin(Name))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot mount required feature plugin: %s"), Name);
            return false;
        }
    }
    return true;
}

bool LoadAllSources(TArray<FEntry>& Entries)
{
    for (FEntry& Entry : Entries)
    {
        // 现存别名必须按 redirector 的实际类型加载，LOAD_NoRedirects 禁止把 canonical BP 误当别名源。
        UObject* Asset = Entry.AliasCanonicalSource.IsEmpty()
            ? LoadObject<UObject>(nullptr, *ObjectPath(Entry.SourcePackage), nullptr, LOAD_NoRedirects)
            : LoadObject<UObjectRedirector>(nullptr, *ObjectPath(Entry.SourcePackage), nullptr, LOAD_NoRedirects);
        if (!Asset || Asset->IsTemplate() || Asset->GetPathName() != ObjectPath(Entry.SourcePackage)
            || (Asset->IsA<UObjectRedirector>() != !Entry.AliasCanonicalSource.IsEmpty()))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Source type/path does not match the reviewed snapshot: %s"), *Entry.SourcePackage);
            return false;
        }
        if (Asset->IsA<UWorld>() != Entry.SourceFilename.EndsWith(TEXT(".umap")))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Source extension does not match its asset type: %s"), *Entry.SourcePackage);
            return false;
        }
        if (UWorld* World = Cast<UWorld>(Asset))
        {
            // 当前清单没有外部 Actor/Object 包；遇到这类世界就停止，不能隐式搬迁清单外文件。
            if (World->IsPartitionedWorld() || (World->PersistentLevel
                && (World->PersistentLevel->IsUsingExternalActors() || World->PersistentLevel->IsUsingExternalObjects())))
            {
                UE_LOG(LogECPAssetMigration, Error, TEXT("World uses external packages not present in manifest: %s"), *Entry.SourcePackage);
                return false;
            }
        }
        Entry.Asset.Reset(Asset);
        Entry.SourceOuter.Reset(Asset->GetOutermost());
        Entry.SourceOuter->FullyLoad();
        if (UObjectRedirector* Alias = Cast<UObjectRedirector>(Asset))
        {
            Entry.Redirector.Reset(Alias);
        }
    }
    // 全部源加载完毕才比对别名指针，确保它直接指向本次白名单内的 canonical 资产。
    for (const FEntry& Entry : Entries)
    {
        if (!Entry.AliasCanonicalSource.IsEmpty())
        {
            const FEntry* Canonical = FindSourceEntry(Entries, Entry.AliasCanonicalSource);
            if (!Canonical || !Canonical->Asset.IsValid() || Canonical->Asset->IsA<UObjectRedirector>()
                || Entry.Redirector->DestinationObject != Canonical->Asset.Get())
            {
                UE_LOG(LogECPAssetMigration, Error, TEXT("Existing alias target differs from the reviewed canonical asset: %s"), *Entry.SourcePackage);
                return false;
            }
        }
    }
    return true;
}

bool PrepareBlueprintDefaults(TArray<FEntry>& Entries, TArray<FBlueprintDefault>& Defaults)
{
    for (FBlueprintDefault& Default : Defaults)
    {
        FEntry* Entry = FindSourceEntry(Entries, Default.SourcePackage);
        UBlueprint* Blueprint = Entry ? Cast<UBlueprint>(Entry->Asset.Get()) : nullptr;
        UClass* ClassValue = LoadObject<UClass>(nullptr, *Default.SourceClassPath, nullptr, LOAD_NoRedirects);
        FClassProperty* Property = Blueprint && Blueprint->GeneratedClass
            ? FindFProperty<FClassProperty>(Blueprint->GeneratedClass, Default.PropertyName) : nullptr;
        UObject* CDO = Blueprint && Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        // 默认值来自审定快照；只接受直接原生父类、正确的 Class 属性类型和兼容的目标类。
        if (!Blueprint || !Blueprint->ParentClass
            || Blueprint->ParentClass->GetPathName() != TEXT("/Script/ECPInteraction.ECPPlayerBase")
            || !ClassValue || ClassValue->GetPathName() != Default.SourceClassPath
            || !Property || !Property->MetaClass || !ClassValue->IsChildOf(Property->MetaClass) || !CDO)
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot restore reviewed class default: %s.%s"),
                *Default.SourcePackage, *Default.PropertyName.ToString());
            return false;
        }
        UObject* Current = Property->GetObjectPropertyValue_InContainer(CDO);
        if (Current && Current != ClassValue)
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Blueprint has a different current override; refusing to replace: %s.%s"),
                *Default.SourcePackage, *Default.PropertyName.ToString());
            return false;
        }
        Default.ClassValue.Reset(ClassValue);
    }
    return true;
}

bool RestoreBlueprintDefaults(TArray<FEntry>& Entries, const TArray<FBlueprintDefault>& Defaults)
{
    for (const FBlueprintDefault& Default : Defaults)
    {
        FEntry* Entry = FindSourceEntry(Entries, Default.SourcePackage);
        FEntry* ClassEntry = FindSourceEntry(Entries, FPackageName::ObjectPathToPackageName(Default.SourceClassPath));
        UBlueprint* Blueprint = Entry ? Cast<UBlueprint>(Entry->Asset.Get()) : nullptr;
        FClassProperty* Property = Blueprint && Blueprint->GeneratedClass
            ? FindFProperty<FClassProperty>(Blueprint->GeneratedClass, Default.PropertyName) : nullptr;
        UObject* CDO = Blueprint && Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        // UBlueprint::Rename 应当移动同一个生成类；先确认软路径已变为目标，再向六个 CDO 写入原语义。
        if (!Entry || !ClassEntry || !Property || !CDO || !Default.ClassValue.IsValid()
            || Default.ClassValue->GetPathName() != ObjectPath(ClassEntry->DestinationPackage) + TEXT("_C"))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Class default did not follow its renamed asset: %s.%s"),
                *Default.SourcePackage, *Default.PropertyName.ToString());
            return false;
        }
        Blueprint->Modify();
        CDO->Modify();
        Property->SetObjectPropertyValue_InContainer(CDO, Default.ClassValue.Get());
        Blueprint->MarkPackageDirty();
        if (Property->GetObjectPropertyValue_InContainer(CDO) != Default.ClassValue.Get())
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Restored CDO class did not persist in memory: %s.%s"),
                *Default.SourcePackage, *Default.PropertyName.ToString());
            return false;
        }
        UE_LOG(LogECPAssetMigration, Display, TEXT("DEFAULT RESTORED %s.%s -> %s"),
            *Entry->DestinationPackage, *Default.PropertyName.ToString(), *Default.ClassValue->GetPathName());
    }
    return true;
}

bool RenameAllSources(TArray<FEntry>& Entries)
{
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TSet<UPackage*> RefusedToFullyLoad;
    for (FEntry& Entry : Entries)
    {
        // 已有别名不参与真实资产 Rename，避免把 canonical BP 再移动一次或生成 redirector 链。
        if (!Entry.AliasCanonicalSource.IsEmpty())
        {
            continue;
        }
        ObjectTools::FPackageGroupName NewLocation;
        NewLocation.PackageName = Entry.DestinationPackage;
        NewLocation.ObjectName = Entry.AssetName;
        NewLocation.GroupName = TEXT("");
        FText Error;
        // 直接使用官方单对象重命名，并强制旧包留下 redirector；不进入自动保存引用方的 RenameManager。
        if (!ObjectTools::RenameSingleObject(Entry.Asset.Get(), NewLocation, RefusedToFullyLoad, Error, nullptr, true))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("RENAME FAILED %s -> %s: %s"),
                *Entry.SourcePackage, *Entry.DestinationPackage, *Error.ToString());
            return false;
        }
        UObjectRedirector* Redirector = FindObject<UObjectRedirector>(Entry.SourceOuter.Get(), *Entry.AssetName);
        if (!Redirector || Redirector->DestinationObject != Entry.Asset.Get()
            || Entry.Asset->GetPathName() != ObjectPath(Entry.DestinationPackage))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Rename did not produce the required exact redirector: %s"), *Entry.SourcePackage);
            return false;
        }
        Entry.Redirector.Reset(Redirector);
        if (const UBlueprint* Blueprint = Cast<UBlueprint>(Entry.Asset.Get()))
        {
            // UBlueprint::Rename 负责 GeneratedClass/SKEL/CDO；仅校验，不做第二次手动改名。
            if (Blueprint->GeneratedClass
                && Blueprint->GeneratedClass->GetOutermost() != Entry.Asset->GetOutermost())
            {
                UE_LOG(LogECPAssetMigration, Error, TEXT("Blueprint generated class remained in another package: %s"), *Entry.DestinationPackage);
                return false;
            }
        }
        UE_LOG(LogECPAssetMigration, Display, TEXT("RENAMED %s -> %s"), *Entry.SourcePackage, *Entry.DestinationPackage);
    }

    // canonical 已就位后，在新的别名位置创建独立 redirector；旧别名直接指向 canonical，不指向新别名。
    for (FEntry& Entry : Entries)
    {
        if (Entry.AliasCanonicalSource.IsEmpty())
        {
            continue;
        }
        FEntry* Canonical = FindSourceEntry(Entries, Entry.AliasCanonicalSource);
        if (!Canonical || !Canonical->Asset.IsValid() || Canonical->Asset->IsA<UObjectRedirector>()
            || !Entry.Redirector.IsValid()
            || Canonical->Asset->GetPathName() != ObjectPath(Canonical->DestinationPackage))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Cannot create reviewed destination alias: %s"), *Entry.DestinationPackage);
            return false;
        }
        UPackage* AliasPackage = CreatePackage(*Entry.DestinationPackage);
        UObjectRedirector* NewAlias = NewObject<UObjectRedirector>(AliasPackage, FName(*Entry.AssetName), RF_Public | RF_Standalone);
        NewAlias->DestinationObject = Canonical->Asset.Get();
        Entry.Redirector->DestinationObject = Canonical->Asset.Get();
        Entry.SourceOuter->MarkPackageDirty();
        Entry.Asset.Reset(NewAlias);
        FAssetRegistryModule::AssetCreated(NewAlias);
        UE_LOG(LogECPAssetMigration, Display, TEXT("ALIAS %s and %s -> %s"),
            *Entry.SourcePackage, *Entry.DestinationPackage, *Canonical->DestinationPackage);
    }
    return true;
}

bool VerifySavedMigration(const TArray<FEntry>& Entries)
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FString> SavedFiles;
    for (const FEntry& Entry : Entries)
    {
        SavedFiles.Add(Entry.SourceFilename);
        SavedFiles.Add(Entry.DestinationFilename);
    }
    // 显式重扫落盘文件，不假定底层 Rename 是否已发送某个版本的注册表通知，也不重复广播改名事件。
    Registry.ScanFilesSynchronous(SavedFiles, true);
    for (const FEntry& Entry : Entries)
    {
        const FAssetData OldData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath(Entry.SourcePackage)), true);
        const FAssetData NewData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath(Entry.DestinationPackage)), true);
        const bool bExistingAlias = !Entry.AliasCanonicalSource.IsEmpty();
        const FEntry* Canonical = bExistingAlias
            ? Entries.FindByPredicate([&Entry](const FEntry& Candidate) { return Candidate.SourcePackage == Entry.AliasCanonicalSource; })
            : &Entry;
        const FString ExpectedTarget = Canonical ? ObjectPath(Canonical->DestinationPackage) : FString();
        FString SavedRedirectorTarget;
        FString SavedAliasTarget;
        const bool bHasSavedTarget = OldData.GetTagValue(FName(TEXT("DestinationObject")), SavedRedirectorTarget);
        const bool bAliasTargetValid = !bExistingAlias
            || (NewData.GetTagValue(FName(TEXT("DestinationObject")), SavedAliasTarget)
                && FPackageName::ExportTextPathToObjectPath(SavedAliasTarget) == ExpectedTarget);
        if (!Canonical || !OldData.IsValid() || !OldData.IsRedirector() || !NewData.IsValid()
            || NewData.IsRedirector() != bExistingAlias || !bAliasTargetValid
            || !bHasSavedTarget || FPackageName::ExportTextPathToObjectPath(SavedRedirectorTarget) != ExpectedTarget
            || NewData.AssetClassPath != Entry.Asset->GetClass()->GetClassPathName()
            || !Entry.Redirector.IsValid() || Entry.Redirector->DestinationObject != Canonical->Asset.Get())
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Saved registry/redirector validation failed: %s"), *Entry.SourcePackage);
            return false;
        }
    }
    return true;
}

bool ApplyAssets(TArray<FEntry>& Entries, TArray<FBlueprintDefault>& Defaults)
{
    // 整批通过权限、目标和加载预检后才允许第一次 Rename；不自动申请 LFS 锁或修改只读属性。
    if (!CheckMigrationFiles(Entries, true) || !MountFeatureModules() || !LoadAllSources(Entries)
        || !PrepareBlueprintDefaults(Entries, Defaults) || !CheckMigrationFiles(Entries, true))
    {
        return false;
    }
    FSaveAllowList Allowed;
    for (const FEntry& Entry : Entries)
    {
        Allowed.Add(Entry.SourcePackage, Entry.SourceFilename);
        Allowed.Add(Entry.DestinationPackage, Entry.DestinationFilename);
    }
    if (!RenameAllSources(Entries) || !RestoreBlueprintDefaults(Entries, Defaults))
    {
        return false;
    }

    // 先写齐新资产，再替换旧包为重定向器；若中途失败仍明确保留部分结果，不伪称事务回滚。
    for (const FEntry& Entry : Entries)
    {
        if (!SaveExactPackage(Entry.Asset->GetOutermost(), Entry.Asset.Get(), Entry.DestinationFilename, Allowed))
        {
            return false;
        }
    }
    for (const FEntry& Entry : Entries)
    {
        // 原地图重定向器沿用原 .umap 文件名，避免同一包名同时残留 .umap 与 .uasset。
        if (!SaveExactPackage(Entry.SourceOuter.Get(), Entry.Redirector.Get(), Entry.SourceFilename, Allowed))
        {
            return false;
        }
    }
    return VerifySavedMigration(Entries);
}

// 功能资源使用独立白名单，与 276 项物理迁移互不依赖；标签可先引用尚未迁入的目标软路径。
struct FFeatureOutput
{
    FString PluginName;
    FString DataPackage;
    FString LabelPackage;
    FString DataFilename;
    FString LabelFilename;
};

bool CreateFeatureData(const TArray<FEntry>& Entries)
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (!MountFeatureModules())
    {
        return false;
    }
    TArray<FFeatureOutput> Outputs;
    FSaveAllowList Allowed;
    const TCHAR* Names[] = {TEXT("ECPCharacter"), TEXT("ECPInteraction"), TEXT("ECPAI"), TEXT("ECPOnline"), TEXT("ECPWorld")};
    for (const TCHAR* Name : Names)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
        if (!Plugin.IsValid() || !Plugin->CanContainContent())
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Feature plugin has no content root: %s"), Name);
            return false;
        }
        const FString ExpectedContent = AbsoluteFilename(FPaths::Combine(
            FPaths::ProjectPluginsDir(), TEXT("GameFeatures"), Name, TEXT("Content")));
        if (AbsoluteFilename(Plugin->GetContentDir()) != ExpectedContent)
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Feature content root is outside the approved project plugin: %s"), Name);
            return false;
        }
        FFeatureOutput Output;
        Output.PluginName = Name;
        Output.DataPackage = FString::Printf(TEXT("/%s/%s"), Name, Name);
        Output.LabelPackage = FString::Printf(TEXT("/%s/PAL_%s"), Name, Name);
        Output.DataFilename = AbsoluteFilename(FPaths::Combine(Plugin->GetContentDir(), FString(Name) + TEXT(".uasset")));
        Output.LabelFilename = AbsoluteFilename(FPaths::Combine(Plugin->GetContentDir(), FString(TEXT("PAL_")) + Name + TEXT(".uasset")));
        // 先检查全部十个输出；已有资源属于用户，不能在重试时静默覆盖或重配。
        if (IFileManager::Get().FileExists(*Output.DataFilename) || IFileManager::Get().FileExists(*Output.LabelFilename)
            || FindPackage(nullptr, *Output.DataPackage) || FindPackage(nullptr, *Output.LabelPackage))
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Feature output already exists; refusing overwrite: %s"), Name);
            return false;
        }
        Allowed.Add(Output.DataPackage, Output.DataFilename);
        Allowed.Add(Output.LabelPackage, Output.LabelFilename);
        Outputs.Add(MoveTemp(Output));
    }

    TArray<FString> SavedFiles;
    for (const FFeatureOutput& Output : Outputs)
    {
        UPackage* LabelPackage = CreatePackage(*Output.LabelPackage);
        UPrimaryAssetLabel* Label = NewObject<UPrimaryAssetLabel>(LabelPackage,
            FName(*FPackageName::GetLongPackageAssetName(Output.LabelPackage)), RF_Public | RF_Standalone);
        Label->Rules.CookRule = EPrimaryAssetCookRule::AlwaysCook;
        Label->Rules.bApplyRecursively = true;
        Label->bLabelAssetsInMyDirectory = false;
        Label->bIsRuntimeLabel = false;
        Label->bIncludeRedirectors = false;
        for (const FEntry& Entry : Entries)
        {
            if (Entry.Feature == Output.PluginName && Entry.Area != TEXT("Archive"))
            {
                Label->ExplicitAssets.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(ObjectPath(Entry.DestinationPackage))));
            }
        }
        // 只写最终目标软引用，既不加载未迁移的资源，也不把旧 redirector 或 Archive 强制 Cook。
        Label->UpdateAssetBundleData();
        FAssetRegistryModule::AssetCreated(Label);
        if (!SaveExactPackage(LabelPackage, Label, Output.LabelFilename, Allowed))
        {
            return false;
        }

        UPackage* DataPackage = CreatePackage(*Output.DataPackage);
        UGameFeatureData* Data = NewObject<UGameFeatureData>(DataPackage,
            FName(*Output.PluginName), RF_Public | RF_Standalone);
        // UE5.8 的 bIsEditorOnly=true 会忽略 AlwaysCook；标签本体另由 bIsRuntimeLabel=false 排除运行时。
        FPrimaryAssetTypeInfo Labels(FName(TEXT("PrimaryAssetLabel")), UPrimaryAssetLabel::StaticClass(), false, false);
        FDirectoryPath PluginRoot;
        PluginRoot.Path = FString::Printf(TEXT("/%s"), *Output.PluginName);
        Labels.GetDirectories().Add(PluginRoot);
        Labels.Rules.CookRule = EPrimaryAssetCookRule::AlwaysCook;
        Labels.Rules.bApplyRecursively = true;
        Data->GetPrimaryAssetTypesToScan().Add(MoveTemp(Labels));
        Data->UpdateAssetBundleData();
        FAssetRegistryModule::AssetCreated(Data);
        if (!SaveExactPackage(DataPackage, Data, Output.DataFilename, Allowed))
        {
            return false;
        }
        SavedFiles.Add(Output.LabelFilename);
        SavedFiles.Add(Output.DataFilename);
    }
    Registry.ScanFilesSynchronous(SavedFiles, true);
    for (const FFeatureOutput& Output : Outputs)
    {
        const FAssetData Data = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath(Output.DataPackage)), true);
        const FAssetData Label = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath(Output.LabelPackage)), true);
        if (!Data.IsValid() || Data.AssetClassPath != UGameFeatureData::StaticClass()->GetClassPathName()
            || !Label.IsValid() || Label.AssetClassPath != UPrimaryAssetLabel::StaticClass()->GetClassPathName())
        {
            UE_LOG(LogECPAssetMigration, Error, TEXT("Feature output registry validation failed: %s"), *Output.PluginName);
            return false;
        }
    }
    return true;
}
}

UECPMigrateAssetsCommandlet::UECPMigrateAssetsCommandlet()
{
    // 独立编辑器进程内运行；默认不接入任何游戏网络世界。
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
    ShowErrorCount = true;
}

int32 UECPMigrateAssetsCommandlet::Main(const FString& Params)
{
    const bool bApplyAssets = FParse::Param(*Params, TEXT("ApplyAssets"));
    const bool bCreateFeatureData = FParse::Param(*Params, TEXT("CreateFeatureData"));
    const bool bExplicitDryRun = FParse::Param(*Params, TEXT("DryRun"));
    if ((bApplyAssets && bCreateFeatureData) || (bExplicitDryRun && (bApplyAssets || bCreateFeatureData)))
    {
        UE_LOG(LogECPAssetMigration, Error, TEXT("Choose exactly one mode: default/-DryRun, -ApplyAssets, or -CreateFeatureData."));
        return 1;
    }

    TArray<ECPAssetMigration::FEntry> Entries;
    TArray<ECPAssetMigration::FBlueprintDefault> Defaults;
    if (!ECPAssetMigration::ReadManifest(Entries) || !ECPAssetMigration::ReadBlueprintSnapshot(Entries, Defaults))
    {
        return 1;
    }
    if (!bApplyAssets && !bCreateFeatureData)
    {
        // 默认模式只读两份 JSON 和文件状态，不加载源资产、不挂载功能、不创建目录或写包。
        const bool bValid = ECPAssetMigration::CheckMigrationFiles(Entries, false);
        UE_LOG(LogECPAssetMigration, Display, TEXT("DRY RUN %s; no asset operation executed."), bValid ? TEXT("OK") : TEXT("FAILED"));
        return bValid ? 0 : 1;
    }

    ECPAssetMigration::FProtectedSnapshot ProtectedBefore;
    if (!ECPAssetMigration::SnapshotProtectedFiles(ProtectedBefore))
    {
        return 1;
    }
    const bool bSucceeded = bCreateFeatureData
        ? ECPAssetMigration::CreateFeatureData(Entries)
        : ECPAssetMigration::ApplyAssets(Entries, Defaults);
    const bool bProtectedUnchanged = ECPAssetMigration::VerifyProtectedFiles(ProtectedBefore);
    if (!bSucceeded || !bProtectedUnchanged)
    {
        UE_LOG(LogECPAssetMigration, Error,
            TEXT("FAILED. Partial in-memory or on-disk results may remain. No rollback was attempted; inspect all SAVE/RENAMED/ALIAS lines before retrying."));
        return 1;
    }
    UE_LOG(LogECPAssetMigration, Display, TEXT("%s completed and verified."),
        bCreateFeatureData ? TEXT("CreateFeatureData (5 data assets + 5 labels)") : TEXT("ApplyAssets (275 assets + 1 alias)"));
    return 0;
}
