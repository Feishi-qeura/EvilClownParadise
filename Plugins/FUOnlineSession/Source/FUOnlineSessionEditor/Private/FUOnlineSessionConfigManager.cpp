#include "FUOnlineSessionConfigManager.h"

#include "FU_OnlineSessionSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogFUOnlineSessionConfig, Log, All);

namespace FUOnlineSessionConfig
{
    constexpr const TCHAR* BeginMarker =
        TEXT("; BEGIN FUONLINESESSION AUTO CONFIG");

    constexpr const TCHAR* EndMarker =
        TEXT("; END FUONLINESESSION AUTO CONFIG");
}

EFU_ExternalGameNetDriverState
FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(
    const FString& ExternalConfigContent
)
{
    // 【FU 修复：旧配置迁移】参数仍保留以兼容已有调用接口。
    // 无论当前默认选择 Steam 还是 IP，下列驱动都属于插件认识且能够迁移的历史配置。
    // 其中 OnlineSubsystemSteam.SteamNetDriver 是旧文档路径，只允许识别和迁移，绝不再生成。
    static const TCHAR* SupportedDriverNames[] =
    {
        TEXT("SteamSockets.SteamSocketsNetDriver"),
        TEXT("SocketSubsystemSteamIP.SteamNetDriver"),
        TEXT("OnlineSubsystemSteam.SteamNetDriver"),
        TEXT("OnlineSubsystemUtils.IpNetDriver")
    };

    TArray<FString> Lines;
    ExternalConfigContent.ParseIntoArrayLines(Lines, false);

    bool bFoundCompatibleDefinition = false;

    for (FString Line : Lines)
    {
        Line.TrimStartAndEndInline();

        // 注释中的示例不是有效配置，不能误判为冲突。
        if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
        {
            continue;
        }

        const bool bDefinesGameNetDriver =
            Line.Contains(TEXT("NetDriverDefinitions"), ESearchCase::IgnoreCase)
            && Line.Contains(TEXT("GameNetDriver"), ESearchCase::IgnoreCase);

        if (!bDefinesGameNetDriver)
        {
            continue;
        }

        // 只检查主 DriverClassName，不能搜索整行：
        // 自定义主驱动通常也会把 IpNetDriver 写成 Fallback，搜索整行会把这种冲突误判为兼容。
        const int32 DriverClassIndex = Line.Find(
            TEXT("DriverClassName="),
            ESearchCase::IgnoreCase
        );

        FString PrimaryDriverDefinition =
            DriverClassIndex == INDEX_NONE
                ? FString()
                : Line.Mid(DriverClassIndex);

        int32 PrimaryDriverEndIndex = INDEX_NONE;
        if (PrimaryDriverDefinition.FindChar(TEXT(','), PrimaryDriverEndIndex))
        {
            PrimaryDriverDefinition.LeftInline(PrimaryDriverEndIndex);
        }

        // 同时兼容带 /Script/ 的现代写法以及 UE 旧项目常见的不带前缀写法。
        bool bUsesSupportedDriver = false;
        for (const TCHAR* SupportedDriverName : SupportedDriverNames)
        {
            if (PrimaryDriverDefinition.Contains(SupportedDriverName, ESearchCase::IgnoreCase))
            {
                bUsesSupportedDriver = true;
                break;
            }
        }

        if (!bUsesSupportedDriver)
        {
            // 任何自定义 GameNetDriver 都优先判为冲突，防止被 !NetDriverDefinitions 清除。
            return EFU_ExternalGameNetDriverState::Conflict;
        }

        bFoundCompatibleDefinition = true;
    }

    return bFoundCompatibleDefinition
        ? EFU_ExternalGameNetDriverState::Compatible
        : EFU_ExternalGameNetDriverState::None;
}

FString FFUOnlineSessionConfigManager::BuildManagedConfigBlock(
    const UFU_OnlineSessionSettings& Settings
)
{
    FString Result;

    // 所有行统一使用平台换行符，避免每次启动都因为换行格式不同而重写文件。
    const auto AddLine = [&Result](const FString& Line)
    {
        Result += Line;
        Result += LINE_TERMINATOR;
    };

    AddLine(FUOnlineSessionConfig::BeginMarker);
    AddLine(TEXT(""));

    AddLine(TEXT("[OnlineSubsystem]"));
    AddLine(TEXT("DefaultPlatformService=Steam"));
    AddLine(TEXT(""));

    AddLine(TEXT("[OnlineSubsystemSteam]"));
    AddLine(TEXT("bEnabled=true"));

    AddLine(FString::Printf(
        TEXT("SteamDevAppId=%d"),
        Settings.SteamDevAppId
    ));

    // 【FU 修复：双 Provider 固定职责】Steam Lobby 必须保留 Steam 网络传输；
    // LAN 入口会在 Runtime 模板中切换到 IpNetDriver，不需要关闭全局 Steam 能力。
    AddLine(TEXT("bUseSteamNetworking=true"));

    AddLine(TEXT(""));

    AddLine(TEXT("[/Script/Engine.GameEngine]"));

    // 插件需要控制 GameNetDriver，同时重新补回 DemoNetDriver，
    // 避免 ClearArray 破坏引擎的录像驱动配置。
    AddLine(TEXT("!NetDriverDefinitions=ClearArray"));

    // 【FU 修复：SteamSockets】UE 5.8 的现代 Steam P2P 传输使用独立 SteamSocketsNetDriver。
    // 旧 /Script/OnlineSubsystemSteam.SteamNetDriver 在本引擎版本中无法加载，
    // 加载失败后会回退到 IpNetDriver，进而把 steam.<SteamId> 错当成 DNS 主机名。
    AddLine(
        TEXT(
            "+NetDriverDefinitions="
            "(DefName=\"GameNetDriver\","
            "DriverClassName=\"/Script/SteamSockets.SteamSocketsNetDriver\","
            "DriverClassNameFallback=\"/Script/OnlineSubsystemUtils.IpNetDriver\")"
        )
    );

    AddLine(
        TEXT(
            "+NetDriverDefinitions="
            "(DefName=\"DemoNetDriver\","
            "DriverClassName=\"/Script/Engine.DemoNetDriver\","
            "DriverClassNameFallback=\"/Script/Engine.DemoNetDriver\")"
        )
    );

    AddLine(TEXT(""));
    AddLine(
        TEXT(
            "[/Script/SteamSockets.SteamSocketsNetDriver]"
        )
    );

    AddLine(
        TEXT(
            "NetConnectionClassName="
            "\"/Script/SteamSockets.SteamSocketsNetConnection\""
        )
    );

    AddLine(TEXT(""));
    AddLine(FUOnlineSessionConfig::EndMarker);

    return Result;
}

EFU_OnlineConfigResult
FFUOnlineSessionConfigManager::WriteManagedConfigBlock(
    const FString& ManagedBlock
)
{
    const FString ConfigPath = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(
            FPaths::ProjectConfigDir(),
            TEXT("DefaultEngine.ini")
        )
    );

    FString ExistingContent;

    const bool bConfigExists =
        IFileManager::Get().FileExists(*ConfigPath);

    if (bConfigExists)
    {
        // 文件存在却读取失败时不能继续写入，
        // 否则可能覆盖用户原本的完整配置。
        if (!FFileHelper::LoadFileToString(
            ExistingContent,
            *ConfigPath
        ))
        {
            return EFU_OnlineConfigResult::Failed;
        }
    }

    const FString OriginalContent = ExistingContent;

    const int32 BeginIndex = ExistingContent.Find(
        FUOnlineSessionConfig::BeginMarker,
        ESearchCase::CaseSensitive
    );

    const int32 EndIndex = ExistingContent.Find(
        FUOnlineSessionConfig::EndMarker,
        ESearchCase::CaseSensitive
    );

    const bool bHasBeginMarker = BeginIndex != INDEX_NONE;
    const bool bHasEndMarker = EndIndex != INDEX_NONE;

    // 只出现一个标记表示文件可能被人工编辑坏了。
    // 此时宁可停止，也不能猜测应该删除哪一段。
    if (bHasBeginMarker != bHasEndMarker)
    {
        return EFU_OnlineConfigResult::Failed;
    }

    if (bHasBeginMarker && bHasEndMarker)
    {
        // End 标记必须位于 Begin 标记之后。
        if (EndIndex < BeginIndex)
        {
            return EFU_OnlineConfigResult::Failed;
        }

        const int32 BlockEndIndex =
            EndIndex
            + FCString::Strlen(
                FUOnlineSessionConfig::EndMarker
            );

        ExistingContent.RemoveAt(
            BeginIndex,
            BlockEndIndex - BeginIndex
        );
    }

    // 移除插件自己的区块后，剩余 GameNetDriver 才是项目或其他插件拥有的配置。
    const EFU_ExternalGameNetDriverState ExternalDriverState =
        AnalyzeExternalGameNetDriver(
            ExistingContent
        );

    if (ExternalDriverState == EFU_ExternalGameNetDriverState::Conflict)
    {
        // 用户自定义驱动的用途未知；停止写入比静默破坏其他联网插件更安全。
        UE_LOG(
            LogFUOnlineSessionConfig,
            Error,
            TEXT("检测到 FU 管理区块之外的自定义 GameNetDriver，已停止自动配置")
        );
        return EFU_OnlineConfigResult::Conflict;
    }

    if (ExternalDriverState == EFU_ExternalGameNetDriverState::Compatible)
    {
        // 兼容旧定义不会被删除；管理区块会先 ClearArray，再建立当前选择的驱动。
        UE_LOG(
            LogFUOnlineSessionConfig,
            Warning,
            TEXT("检测到兼容的旧 GameNetDriver 定义；FU 管理区块将在运行时覆盖它")
        );
    }

    ExistingContent.TrimEndInline();

    if (!ExistingContent.IsEmpty())
    {
        ExistingContent += LINE_TERMINATOR;
        ExistingContent += LINE_TERMINATOR;
    }

    ExistingContent += ManagedBlock;

    // 内容完全相同就不写磁盘。
    if (ExistingContent == OriginalContent)
    {
        return EFU_OnlineConfigResult::Unchanged;
    }

    IFileManager::Get().MakeDirectory(
        *FPaths::GetPath(ConfigPath),
        true
    );

    const bool bSaved = FFileHelper::SaveStringToFile(
        ExistingContent,
        *ConfigPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM
    );

    return bSaved
        ? EFU_OnlineConfigResult::Updated
        : EFU_OnlineConfigResult::Failed;
}

EFU_OnlineConfigResult
FFUOnlineSessionConfigManager::EnsureProjectConfiguration()
{
    const UFU_OnlineSessionSettings* Settings =
        GetDefault<UFU_OnlineSessionSettings>();

    if (!Settings)
    {
        return EFU_OnlineConfigResult::Failed;
    }

    if (!Settings->bAutoConfigureProject)
    {
        // 关闭自动配置只会停止继续更新，
        // 不会擅自删除以前已经生成的配置。
        return EFU_OnlineConfigResult::Disabled;
    }

    if (Settings->SteamDevAppId <= 0)
    {
        return EFU_OnlineConfigResult::Failed;
    }

    return WriteManagedConfigBlock(
        BuildManagedConfigBlock(*Settings)
    );
}
