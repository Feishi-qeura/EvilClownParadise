#include "ECPOnlineBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FU_OnlineSessionSubsystem.h"
#include "GameFeaturesSubsystem.h"
#include "GameFramework/PlayerController.h"

namespace ECPOnline::Private
{
    /** 两个蓝图入口共用同一组只读校验，避免 Ready 为真但取得的子系统属于另一 World。 */
    UFU_OnlineSessionSubsystem* ResolveOnlineSessionSubsystem(
        const UObject* WorldContextObject,
        FString& OutReason)
    {
        OutReason.Reset();

        // 蓝图构造、结束 PIE 或无引擎环境都可能早于正常游戏生命周期，不能解引用无效上下文。
        if (!IsValid(GEngine) || !IsValid(WorldContextObject))
        {
            OutReason = TEXT("引擎或 World 上下文尚未就绪。");
            return nullptr;
        }

        // 只使用调用者所属 World；禁止回退到全局 World，防止多 PIE 实例之间取得错误会话。
        UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
        if (!IsValid(World) || !World->IsGameWorld() || World->bIsTearingDown)
        {
            OutReason = TEXT("需要有效且未退出的游戏 World；编辑器预览和销毁中的 World 不可使用联机入口。");
            return nullptr;
        }

        // 现有 FU 创建、搜索与加入均依赖 LocalPlayer；专服不能复用这个本地前端入口。
        if (World->GetNetMode() == NM_DedicatedServer)
        {
            OutReason = TEXT("当前是专用服务器，本入口仅供本地玩家使用。");
            return nullptr;
        }

        // 模块已加载不等于 Feature 已激活；停用后保留的蓝图调用也必须经过真实 Active 状态检查。
        const UGameFeaturesSubsystem* GameFeatures = GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>();
        if (!IsValid(GameFeatures) || !GameFeatures->IsGameFeaturePluginActiveByName(TEXT("ECPOnline")))
        {
            OutReason = TEXT("ECPOnline Game Feature 尚未完成激活或已经停用。");
            return nullptr;
        }

        // 切图期间旧 World 仍可能有回调到达；拒绝使用已经切换到其他 World 的 GameInstance。
        UGameInstance* GameInstance = World->GetGameInstance();
        if (!IsValid(GameInstance) || GameInstance->GetWorld() != World)
        {
            OutReason = TEXT("GameInstance 尚未关联当前游戏 World，或正在切换地图。");
            return nullptr;
        }

        // FU 后端当前取 FirstPlayerController；此处采用相同选择规则，避免桥接层误报另一本地玩家就绪。
        APlayerController* PlayerController = World->GetFirstPlayerController();
        if (!IsValid(PlayerController)
            || PlayerController->GetWorld() != World
            || !PlayerController->IsLocalController()
            || !IsValid(PlayerController->GetLocalPlayer()))
        {
            OutReason = TEXT("当前 World 的首个 PlayerController 或其 LocalPlayer 尚未就绪。");
            return nullptr;
        }

        // 只读取 GameInstance 已管理的 FU 子系统；绝不 NewObject、手动 Initialize 或重建在线提供方。
        UFU_OnlineSessionSubsystem* OnlineSession = GameInstance->GetSubsystem<UFU_OnlineSessionSubsystem>();
        if (!IsValid(OnlineSession))
        {
            OutReason = TEXT("当前 GameInstance 的 FUOnlineSession 子系统不可用。");
            return nullptr;
        }

        return OnlineSession;
    }
}

UFU_OnlineSessionSubsystem* UECPOnlineBlueprintLibrary::GetOnlineSessionSubsystem(const UObject* WorldContextObject)
{
    // 无原因输出的便捷节点仍执行完整校验；需要显示失败原因时使用 IsOnlineReadyForLocalPlayer。
    FString Reason;
    return ECPOnline::Private::ResolveOnlineSessionSubsystem(WorldContextObject, Reason);
}

bool UECPOnlineBlueprintLibrary::IsOnlineReadyForLocalPlayer(const UObject* WorldContextObject, FString& OutReason)
{
    // 复用相同的解析入口确保两个节点契约一致；Provider 是否可用继续交给 FU 的原有状态查询。
    return ECPOnline::Private::ResolveOnlineSessionSubsystem(WorldContextObject, OutReason) != nullptr;
}
