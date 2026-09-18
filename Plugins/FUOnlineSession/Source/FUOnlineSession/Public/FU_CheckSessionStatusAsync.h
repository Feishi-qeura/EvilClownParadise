#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
// FTimerHandle 是按值成员，直接包含定义，避免依赖 PCH 的间接声明。
#include "TimerManager.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "FU_CheckSessionStatusAsync.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFU_OnCheckSessionStatusResult, float, PingValue);

/**
 * 游戏线程上的低频 Ping 监测，无主动探测包或工作线程。
 * 服务端/单机仅广播一次 OnServer；客户端按 RefreshTime 读取 UE 已统计的 Ping。
 * 初始化等待使用 InitialConnectTimeout，状态丢失使用 ConnectionTimeout（无有效配置时 30 秒）。
 * 收包超时使用连接的 GetTimeoutValue；正常切图会静默结束，应在新 World 重新调用节点。
 */
UCLASS()
class FUONLINESESSION_API UFU_CheckSessionStatusAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session")
	FFU_OnCheckSessionStatusResult OnServer;

	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session")
	FFU_OnCheckSessionStatusResult OnClient;

	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session")
	FFU_OnCheckSessionStatusResult ClientConnectionOvertime;

	// 保持现有蓝图签名；RefreshTime 只控制采样频率，不作为连接超时阈值。
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "FUOnlineSession|Online Session")
	static UFU_CheckSessionStatusAsync* FU_CheckSessionStatus(UObject* WorldContextObject, float RefreshTime);

	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;
	virtual void BeginDestroy() override;

private:
	// 弱引用不延长关卡/驱动寿命；World 结束时显式解除 GameInstance 对节点的保活。
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UNetDriver> MonitoredNetDriver;
	float RefreshInterval = 0.0f;
	float ReadinessTimeout = 30.0f;
	double NotReadySince = -1.0;
	bool bActivated = false;
	bool bStopped = false;
	bool bHasSampled = false;
	FTimerHandle RefreshTimerHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle WorldTearDownHandle;
	FDelegateHandle WorldCleanupHandle;

	void FU_UpdatePing();
	void FU_Cleanup();
	void FU_BroadcastTimeout();
	void FU_OnNetworkFailure(UWorld* FailedWorld, UNetDriver* FailedDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void FU_OnWorldTearDown(UWorld* EndingWorld);
	void FU_OnWorldCleanup(UWorld* EndingWorld, bool bSessionEnded, bool bCleanupResources);
};
