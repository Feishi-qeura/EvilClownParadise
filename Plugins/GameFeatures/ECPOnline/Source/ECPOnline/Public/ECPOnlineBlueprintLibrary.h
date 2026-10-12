#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ECPOnlineBlueprintLibrary.generated.h"

class UFU_OnlineSessionSubsystem;

/**
 * 项目 Online Feature 的蓝图边界，仅检查调用环境并返回现有联机后端。
 * Steam Lobby、NULL/LAN、异步回调和 ClientTravel 始终由 FUOnlineSession 负责。
 */
UCLASS()
class ECPONLINE_API UECPOnlineBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * 取得当前游戏 World 所属 GameInstance 的现有 FU 子系统，不创建新的会话管理对象。
     * ECPOnline 尚未 Active、World 正在退出、专服或本地玩家尚未就绪时返回空值。
     * @param WorldContextObject 属于目标游戏 World 的对象；为空、World 无效或正在退出时查询失败。
     * @return 当前本地玩家所属 GameInstance 的已有 FU 子系统；入口条件不成立时为 nullptr。
     * 提交会话操作前先绑定 FU 完成事件；本节点仅查询本进程对象，不登录、不创建会话、不切图。
     */
    UFUNCTION(BlueprintPure, Category = "ECP|Online", meta = (WorldContext = "WorldContextObject"))
    static UFU_OnlineSessionSubsystem* GetOnlineSessionSubsystem(const UObject* WorldContextObject);

    /**
     * 检查 Online Feature 与当前本地玩家是否具备访问后端的条件，失败时提供可读原因。
     * true 只表示桥接入口就绪；Steam 登录、NetDriver 和 LAN 环境仍须调用 FU 的 Check*ProviderStatus。
     * @param WorldContextObject 属于目标游戏 World 的对象；查询不会回退到其他 World 或 PIE 实例。
     * @param OutReason 失败时写入本次可读原因，成功时清空；不要用它代替 Provider 状态查询。
     * @return 当前本地调用环境与现有 FU 子系统均可用时为 true，否则为 false。
     * 该查询不发网络请求，不代表当前已经联网，也不把 Feature 激活完成当作会话连接完成。
     */
    UFUNCTION(BlueprintPure, Category = "ECP|Online", meta = (WorldContext = "WorldContextObject"))
    static bool IsOnlineReadyForLocalPlayer(const UObject* WorldContextObject, FString& OutReason);
};
