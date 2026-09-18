#include "FU_CheckSessionStatusAsync.h"

#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

UFU_CheckSessionStatusAsync* UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(UObject* WorldContextObject, const float RefreshTime)
{
	// 拒绝无效上下文及 NaN/无穷间隔，避免注册一个永远无法更新的保活节点。
	if (!IsValid(WorldContextObject) || !FMath::IsFinite(RefreshTime) || RefreshTime <= 0.0f || !GEngine)
	{
		return nullptr;
	}

	UWorld* ResolvedWorld = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	// 关卡已开始销毁时不能再创建监测器，调用方应等待新关卡初始化。
	if (!IsValid(ResolvedWorld) || ResolvedWorld->bIsTearingDown)
	{
		return nullptr;
	}

	UFU_CheckSessionStatusAsync* Action = NewObject<UFU_CheckSessionStatusAsync>();
	Action->World = ResolvedWorld;
	Action->RefreshInterval = RefreshTime;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFU_CheckSessionStatusAsync::Activate()
{
	// 委托中可能重入 Activate；同一节点只允许启动一次，结束后也不能复活。
	if (bActivated || bStopped)
	{
		return;
	}
	bActivated = true;
	UWorld* ResolvedWorld = World.Get();
	if (!ResolvedWorld || ResolvedWorld->bIsTearingDown)
	{
		SetReadyToDestroy();
		return;
	}

	// 使用 World 的网络身份，专服没有 PlayerController 也必须正确走服务端分支。
	if (ResolvedWorld->GetNetMode() != NM_Client)
	{
		SetReadyToDestroy();
		OnServer.Broadcast(0.0f);
		return;
	}

	// 客户端先注册监听和 timer，再尝试首次采样；PC/PlayerState 未复制就绪时可继续等待。
	MonitoredNetDriver = ResolvedWorld->GetNetDriver();
	NotReadySince = FPlatformTime::Seconds();
	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::FU_OnNetworkFailure);
	}
	WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &ThisClass::FU_OnWorldTearDown);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::FU_OnWorldCleanup);

	// 不主动发包；卡顿后每帧最多读取一次缓存，避免循环 timer 在同一帧补发大量相同值。
	FTimerManagerTimerParameters TimerParameters;
	TimerParameters.bLoop = true;
	TimerParameters.bMaxOncePerFrame = true;
	ResolvedWorld->GetTimerManager().SetTimer(RefreshTimerHandle, this, &ThisClass::FU_UpdatePing, RefreshInterval, TimerParameters);
	FU_UpdatePing();
}

void UFU_CheckSessionStatusAsync::SetReadyToDestroy()
{
	// 先标记结束并解除副作用，再释放保活，保证重入和多个结束事件只产生一次最终输出。
	bStopped = true;
	FU_Cleanup();
	Super::SetReadyToDestroy();
}

void UFU_CheckSessionStatusAsync::BeginDestroy()
{
	// 引擎退出/GC 可能绕过普通结束路径，仍需解除 timer 和全局委托。
	bStopped = true;
	FU_Cleanup();
	Super::BeginDestroy();
}

void UFU_CheckSessionStatusAsync::FU_UpdatePing()
{
	if (bStopped)
	{
		return;
	}
	UWorld* ResolvedWorld = World.Get();
	// 正常切图不是连接超时；旧节点静默释放，由新 World 的调用方重新启动。
	if (!ResolvedWorld || ResolvedWorld->bIsTearingDown)
	{
		SetReadyToDestroy();
		return;
	}

	UNetDriver* NetDriver = ResolvedWorld->GetNetDriver();
	UNetConnection* Connection = IsValid(NetDriver) ? NetDriver->ServerConnection.Get() : nullptr;
	if (IsValid(NetDriver))
	{
		MonitoredNetDriver = NetDriver;
		// 初始化和已连接后的状态短暂缺失分别沿用引擎对应阈值；保留最后有效配置以应对驱动消失。
		const float ConfiguredTimeout = bHasSampled ? NetDriver->ConnectionTimeout : NetDriver->InitialConnectTimeout;
		if (FMath::IsFinite(ConfiguredTimeout) && ConfiguredTimeout > 0.0f)
		{
			ReadinessTimeout = ConfiguredTimeout;
		}
	}

	// Ping 数值不表示数据是否仍在更新：按驱动自身时钟检查最后收包时间，避免断网后持续广播旧值。
	if (IsValid(Connection) && !NetDriver->bNoTimeouts)
	{
		const float ReceiveTimeout = Connection->GetTimeoutValue();
		if (FMath::IsFinite(ReceiveTimeout) && ReceiveTimeout > 0.0f
			&& NetDriver->GetElapsedTime() - Connection->LastReceiveTime >= ReceiveTimeout)
		{
			FU_BroadcastTimeout();
			return;
		}
	}

	APlayerController* PlayerController = ResolvedWorld->GetFirstPlayerController();
	APlayerState* PlayerState = IsValid(PlayerController) ? PlayerController->PlayerState.Get() : nullptr;
	// 缺少对象、连接仍在建立或关闭过程中均先等待；正常离开通常会随 World 清理静默取消。
	if (!IsValid(PlayerState) || !IsValid(Connection) || Connection->GetConnectionState() != USOCK_Open)
	{
		const double Now = FPlatformTime::Seconds();
		if (NotReadySince < 0.0)
		{
			NotReadySince = Now;
		}
		if (Now - NotReadySince >= ReadinessTimeout)
		{
			FU_BroadcastTimeout();
		}
		return;
	}

	// 状态恢复后重置宽限计时；每次有效采样都广播，让现有蓝图继续按 RefreshTime 更新。
	NotReadySince = -1.0;
	bHasSampled = true;
	OnClient.Broadcast(PlayerState->GetPingInMilliseconds());
}

void UFU_CheckSessionStatusAsync::FU_Cleanup()
{
	// 清理必须幂等：超时、World 结束、外部结束和 GC 都可能先后到达。
	if (UWorld* ResolvedWorld = World.Get())
	{
		ResolvedWorld->GetTimerManager().ClearTimer(RefreshTimerHandle);
	}
	RefreshTimerHandle.Invalidate();
	if (GEngine && NetworkFailureHandle.IsValid())
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
	}
	FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDownHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	NetworkFailureHandle.Reset();
	WorldTearDownHandle.Reset();
	WorldCleanupHandle.Reset();
	MonitoredNetDriver.Reset();
}

void UFU_CheckSessionStatusAsync::FU_BroadcastTimeout()
{
	// 显式断线事件和采样超时可能同时到达，最终输出只能发出一次。
	if (bStopped)
	{
		return;
	}
	SetReadyToDestroy();
	ClientConnectionOvertime.Broadcast(0.0f);
}

void UFU_CheckSessionStatusAsync::FU_OnNetworkFailure(UWorld* FailedWorld, UNetDriver* FailedDriver,
	ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	// PIE 有多个 World，游戏也可能有 Beacon 驱动；只响应本节点实际游戏连接的失败。
	UWorld* ResolvedWorld = World.Get();
	if (bStopped || !ResolvedWorld || FailedWorld != ResolvedWorld || !FailedDriver)
	{
		return;
	}
	if (FailedDriver == MonitoredNetDriver.Get() || FailedDriver == ResolvedWorld->GetNetDriver())
	{
		FU_BroadcastTimeout();
	}
}

void UFU_CheckSessionStatusAsync::FU_OnWorldTearDown(UWorld* EndingWorld)
{
	// 主动退出、切图或结束 PIE 都只结束本 World 的监测，不能报告成网络故障。
	if (EndingWorld == World.Get())
	{
		SetReadyToDestroy();
	}
}

void UFU_CheckSessionStatusAsync::FU_OnWorldCleanup(UWorld* EndingWorld, bool bSessionEnded, bool bCleanupResources)
{
	// 部分 World 清理路径不经过 BeginTearDown，保留第二个生命周期出口确保不残留保活节点。
	FU_OnWorldTearDown(EndingWorld);
}
