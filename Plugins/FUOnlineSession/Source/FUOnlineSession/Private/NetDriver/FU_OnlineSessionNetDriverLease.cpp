#include "NetDriver/FU_OnlineSessionNetDriverLease.h"

#include "FUOnlineSessionModule.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"

namespace FUOnlineSessionNetDriverLeasePrivate
{
	/** 进程中最多一份租约；同一 GameInstance/Provider 的重入通过 AlreadyOwned 表达。 */
	struct FLeaseState
	{
		TWeakObjectPtr<UGameInstance> Owner;
		EFU_OnlineProvider Provider = EFU_OnlineProvider::Lan;
		FFU_NetDriverLeaseFingerprint Fingerprint;
		bool bReleaseRequested = false;
		FTSTicker::FDelegateHandle DeferredReleaseTicker;
	};

	TOptional<FLeaseState> GLease;
	// 【保守失败】一旦第三方在租约期间改写了定义/数组，进程余生都不再触碰 GameNetDriver。
	// 这比“猜测如何恢复”安全，使用者可重启进程取得干净引擎状态。
	bool bProcessLeasePoisoned = false;
	// RemoveTicker 在当前 ticker 回调内部调用会等待自身结束，因此必须区分“由回调自然返回 false”与外部撤销。
	bool bInsideDeferredReleaseTicker = false;

	const TCHAR* ToText(const EFU_NetDriverLeaseResult Result)
	{
		switch (Result)
		{
		case EFU_NetDriverLeaseResult::Acquired: return TEXT("Acquired");
		case EFU_NetDriverLeaseResult::AlreadyOwned: return TEXT("AlreadyOwned");
		case EFU_NetDriverLeaseResult::GameNetDriverMissing: return TEXT("GameNetDriverMissing");
		case EFU_NetDriverLeaseResult::GameNetDriverDuplicate: return TEXT("GameNetDriverDuplicate");
		case EFU_NetDriverLeaseResult::ActiveOrPendingDriver: return TEXT("ActiveOrPendingDriver");
		case EFU_NetDriverLeaseResult::OwnedByAnotherGameInstance: return TEXT("OwnedByAnotherGameInstance");
		case EFU_NetDriverLeaseResult::StaleOwnerReclaimable: return TEXT("StaleOwnerReclaimable");
		case EFU_NetDriverLeaseResult::ProviderSwitchBlocked: return TEXT("ProviderSwitchBlocked");
		case EFU_NetDriverLeaseResult::DesiredDriverMismatch: return TEXT("DesiredDriverMismatch");
		case EFU_NetDriverLeaseResult::ExternallyModified: return TEXT("ExternallyModified");
		case EFU_NetDriverLeaseResult::NotGameThread: return TEXT("NotGameThread");
		case EFU_NetDriverLeaseResult::OwnerUnavailable: return TEXT("OwnerUnavailable");
		case EFU_NetDriverLeaseResult::Restored: return TEXT("Restored");
		case EFU_NetDriverLeaseResult::ReleaseDeferred: return TEXT("ReleaseDeferred");
		default: return TEXT("Unknown");
		}
	}

	bool HasSameDefinitionIdentity(
		const FFU_NetDriverLeaseFingerprint& Expected,
		const FFU_NetDriverLeaseFingerprint& Current)
	{
		return Expected.DefinitionPointer == Current.DefinitionPointer
			&& Expected.DefinitionIndex == Current.DefinitionIndex
			&& Expected.DefinitionArrayNum == Current.DefinitionArrayNum
			&& Expected.OrderedDefinitionNames == Current.OrderedDefinitionNames
			&& Expected.InstalledValue.Equals(Current.InstalledValue);
	}

	/**
	 * 丢弃租约前先精确撤销 ticker。若正在 ticker 自身回调中，则只清空 Handle，
	 * 随后的 false 返回值会由 FTSTicker 安全移除条目，避免自等待死锁。
	 */
	void ResetLeaseState()
	{
		if (!GLease.IsSet())
		{
			return;
		}

		if (GLease->DeferredReleaseTicker.IsValid())
		{
			if (!bInsideDeferredReleaseTicker)
			{
				FTSTicker::RemoveTicker(GLease->DeferredReleaseTicker);
			}
			GLease->DeferredReleaseTicker.Reset();
		}

		GLease.Reset();
	}

	/** 外部改写一旦成立就进入进程级保守失败，且不能留下指向本模块代码的 ticker。 */
	void PoisonAndAbandonLease()
	{
		bProcessLeasePoisoned = true;
		UE_LOG(
			LogFUOnlineSession,
			Error,
			TEXT("GameNetDriver 租约检测到外部修改；为避免覆盖第三方配置，本进程停止后续 FU 租约操作。请重启进程。"));
		ResetLeaseState();
	}

	/** 收集当前数组身份；绝不使用保存的指针解引用，防止数组已重分配时触碰悬空地址。 */
	FFU_NetDriverLeaseFingerprint CaptureFingerprintAtIndex(const int32 PreferredIndex)
	{
		FFU_NetDriverLeaseFingerprint Result;
		if (!GEngine)
		{
			return Result;
		}

		const TArray<FNetDriverDefinition>& Definitions = GEngine->NetDriverDefinitions;
		Result.DefinitionArrayNum = Definitions.Num();
		Result.OrderedDefinitionNames.Reserve(Definitions.Num());
		for (const FNetDriverDefinition& Definition : Definitions)
		{
			Result.OrderedDefinitionNames.Add(Definition.DefName);
		}

		if (Definitions.IsValidIndex(PreferredIndex))
		{
			const FNetDriverDefinition& Definition = Definitions[PreferredIndex];
			Result.DefinitionPointer = &Definition;
			Result.DefinitionIndex = PreferredIndex;
			Result.InstalledValue = FFU_NetDriverDefinitionValue::FromDefinition(Definition);
		}

		return Result;
	}

	/** 所有 World 必须同时不存在目标 NamedNetDriver、PendingNetGame 和已排队 Travel，才能恢复全局定义。 */
	bool AreAllWorldsClear(const FNetDriverDefinition* TargetDefinition)
	{
		if (!GEngine)
		{
			return false;
		}

		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			// PendingNetGame 即使 NetDriver 仍为空，也可能在下一帧按 GameNetDriver 创建连接；不能抢先恢复。
			const UWorld* const World = WorldContext.World();
			if (WorldContext.PendingNetGame != nullptr
				|| !WorldContext.TravelURL.IsEmpty()
				|| (World != nullptr && !World->NextURL.IsEmpty())
				|| WorldContext.SeamlessTravelHandler.IsInTransition())
			{
				return false;
			}

			for (const FNamedNetDriver& NamedDriver : WorldContext.ActiveNetDrivers)
			{
				const UNetDriver* const NetDriver = NamedDriver.NetDriver;
				if ((TargetDefinition != nullptr && NamedDriver.NetDriverDef == TargetDefinition)
					|| (NetDriver != nullptr && NetDriver->GetNetDriverDefinition() == NAME_GameNetDriver))
				{
					return false;
				}
			}
		}

		return true;
	}

	/** 将全局世界使用情况和租约所有权折叠为纯 Preflight，便于同一规则被 Probe 和 Acquire 使用。 */
	FFU_NetDriverLeasePreflight CapturePreflight(
		UGameInstance* Owner,
		const EFU_OnlineProvider Provider,
		const FName DesiredDriverClass)
	{
		FFU_NetDriverLeasePreflight Result;
		Result.bIsGameThread = IsInGameThread();
		if (!Result.bIsGameThread || !GEngine)
		{
			return Result;
		}

		FNetDriverDefinition* TargetDefinition = nullptr;
		for (int32 Index = 0; Index < GEngine->NetDriverDefinitions.Num(); ++Index)
		{
			FNetDriverDefinition& Definition = GEngine->NetDriverDefinitions[Index];
			if (Definition.DefName == NAME_GameNetDriver)
			{
				++Result.GameNetDriverDefinitionCount;
				TargetDefinition = &Definition;
			}
		}

		// 需要逐个 World 扫描，不能只看调用 Subsystem 的 World；PIE 的另一窗口也共享这个定义。
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.PendingNetGame != nullptr)
			{
				Result.bHasPendingNetGame = true;
			}
			// ClientTravel/ServerTravel 可能已写入 TravelURL，但 PendingNetGame 要到下一帧才出现；
			// 该间隙同样会按 GameNetDriver 创建连接，首租和恢复都必须保守阻止。
			const UWorld* const World = WorldContext.World();
			if (!WorldContext.TravelURL.IsEmpty()
				|| (World != nullptr && !World->NextURL.IsEmpty())
				|| WorldContext.SeamlessTravelHandler.IsInTransition())
			{
				Result.bHasPendingTravel = true;
			}

			for (const FNamedNetDriver& NamedDriver : WorldContext.ActiveNetDrivers)
			{
				const UNetDriver* const NetDriver = NamedDriver.NetDriver;
				if (NamedDriver.NetDriverDef == TargetDefinition
					|| (NetDriver != nullptr && NetDriver->GetNetDriverDefinition() == NAME_GameNetDriver))
				{
					Result.bHasLiveTargetNamedDriver = true;
				}
			}
		}

		if (GLease.IsSet())
		{
			Result.bLeaseExists = true;
			Result.bLeaseOwnerExpired = !GLease->Owner.IsValid();
			Result.bSameOwner = GLease->Owner.Get() == Owner;
			Result.bSameProvider = GLease->Provider == Provider;
			Result.bSameDesiredDriver = DesiredDriverClass.IsNone()
				|| (GLease->Fingerprint.InstalledValue.DriverClassName == DesiredDriverClass
					&& GLease->Fingerprint.InstalledValue.DriverClassNameFallback == DesiredDriverClass);
			const FFU_NetDriverLeaseFingerprint Current = CaptureFingerprintAtIndex(GLease->Fingerprint.DefinitionIndex);
			Result.bInstalledValueStillMatches = HasSameDefinitionIdentity(GLease->Fingerprint, Current);
		}

		return Result;
	}

	void EnsureDeferredReleaseTicker();

	/**
	 * 唯一的恢复写点。任何“不安全”或“外部修改”结果都会保留/放弃租约而非强写旧值。
	 */
	EFU_NetDriverLeaseResult TryRestoreLease()
	{
		if (!GLease.IsSet())
		{
			return EFU_NetDriverLeaseResult::Restored;
		}
		if (!IsInGameThread())
		{
			return EFU_NetDriverLeaseResult::NotGameThread;
		}

		const FFU_NetDriverLeaseFingerprint Current =
			CaptureFingerprintAtIndex(GLease->Fingerprint.DefinitionIndex);
		const bool bAllWorldsClear = AreAllWorldsClear(GLease->Fingerprint.DefinitionPointer);
		const EFU_NetDriverLeaseResult Result = FFU_OnlineSessionNetDriverLease::EvaluateRestore(
			GLease->Fingerprint,
			Current,
			bAllWorldsClear);

		if (Result == EFU_NetDriverLeaseResult::Restored)
		{
			// 指纹已证明 Current 指向仍是原来的数组元素；只恢复 UE 5.8 的五个已快照字段。
			FNetDriverDefinition* const Definition = const_cast<FNetDriverDefinition*>(Current.DefinitionPointer);
			Definition->DefName = GLease->Fingerprint.OriginalValue.DefName;
			Definition->DriverClassName = GLease->Fingerprint.OriginalValue.DriverClassName;
			Definition->DriverClassNameFallback = GLease->Fingerprint.OriginalValue.DriverClassNameFallback;
			Definition->MaxChannelsOverride = GLease->Fingerprint.OriginalValue.MaxChannelsOverride;
			Definition->bRunParallelConnectionTick = GLease->Fingerprint.OriginalValue.bRunParallelConnectionTick;

			UE_LOG(LogFUOnlineSession, Display, TEXT("已安全恢复 GameNetDriver 租约"));
			ResetLeaseState();
		}
		else if (Result == EFU_NetDriverLeaseResult::ExternallyModified)
		{
			PoisonAndAbandonLease();
		}

		return Result;
	}

	bool HandleDeferredReleaseTicker(const float /*DeltaTime*/)
	{
		// 本作用域标记确保恢复成功/中毒时不会从回调内部 RemoveTicker 并自锁。
		TGuardValue<bool> CallbackGuard(bInsideDeferredReleaseTicker, true);
		FFU_OnlineSessionNetDriverLease::TickDeferredRelease();
		return GLease.IsSet() && GLease->bReleaseRequested;
	}

	void EnsureDeferredReleaseTicker()
	{
		if (!GLease.IsSet() || !GLease->bReleaseRequested || GLease->DeferredReleaseTicker.IsValid())
		{
			return;
		}

		// 一秒粒度足以等待 World/PendingNetGame/TravelURL 清空，且避免在销毁流程中每帧扫描所有 PIE World。
		GLease->DeferredReleaseTicker = FTSTicker::GetCoreTicker().AddTicker(
			TEXT("FUOnlineSession.NetDriverLeaseRelease"),
			1.0f,
			[](const float DeltaTime)
			{
				return HandleDeferredReleaseTicker(DeltaTime);
			});
	}
}

FFU_NetDriverDefinitionValue FFU_NetDriverDefinitionValue::FromDefinition(const FNetDriverDefinition& Definition)
{
	FFU_NetDriverDefinitionValue Result;
	Result.DefName = Definition.DefName;
	Result.DriverClassName = Definition.DriverClassName;
	Result.DriverClassNameFallback = Definition.DriverClassNameFallback;
	Result.MaxChannelsOverride = Definition.MaxChannelsOverride;
	Result.bRunParallelConnectionTick = Definition.bRunParallelConnectionTick;
	return Result;
}

bool FFU_NetDriverDefinitionValue::Equals(const FFU_NetDriverDefinitionValue& Other) const
{
	return DefName == Other.DefName
		&& DriverClassName == Other.DriverClassName
		&& DriverClassNameFallback == Other.DriverClassNameFallback
		&& MaxChannelsOverride == Other.MaxChannelsOverride
		&& bRunParallelConnectionTick == Other.bRunParallelConnectionTick;
}

FFU_NetDriverDefinitionValue FFU_OnlineSessionNetDriverLease::BuildInstalledValue(
	const FFU_NetDriverDefinitionValue& Original,
	const FName DesiredDriverClass)
{
	// 值拷贝先保留 DefName、通道上限与并行 Tick；随后只改两项传输类名。
	FFU_NetDriverDefinitionValue Installed = Original;
	Installed.DriverClassName = DesiredDriverClass;
	Installed.DriverClassNameFallback = DesiredDriverClass;
	return Installed;
}

EFU_NetDriverLeaseResult FFU_OnlineSessionNetDriverLease::EvaluateAcquire(
	const FFU_NetDriverLeasePreflight& Snapshot)
{
	if (!Snapshot.bIsGameThread)
	{
		return EFU_NetDriverLeaseResult::NotGameThread;
	}

	if (Snapshot.bLeaseExists)
	{
		// 现有租约的完整安装值失配优先于所有权判断；这意味着继续运行可能覆盖第三方修改。
		if (!Snapshot.bInstalledValueStillMatches)
		{
			return EFU_NetDriverLeaseResult::ExternallyModified;
		}
		if (Snapshot.bLeaseOwnerExpired)
		{
			// 异常销毁若跳过 Deinitialize，不应永久锁死下一次 PIE；但只有全 World 无使用时才可尝试回收。
			return Snapshot.bHasLiveTargetNamedDriver || Snapshot.bHasPendingNetGame || Snapshot.bHasPendingTravel
				? EFU_NetDriverLeaseResult::ActiveOrPendingDriver
				: EFU_NetDriverLeaseResult::StaleOwnerReclaimable;
		}
		if (!Snapshot.bSameOwner)
		{
			return EFU_NetDriverLeaseResult::OwnedByAnotherGameInstance;
		}
		if (!Snapshot.bSameProvider)
		{
			return EFU_NetDriverLeaseResult::ProviderSwitchBlocked;
		}
		if (!Snapshot.bSameDesiredDriver)
		{
			return EFU_NetDriverLeaseResult::DesiredDriverMismatch;
		}
		return EFU_NetDriverLeaseResult::AlreadyOwned;
	}

	if (Snapshot.GameNetDriverDefinitionCount == 0)
	{
		return EFU_NetDriverLeaseResult::GameNetDriverMissing;
	}
	if (Snapshot.GameNetDriverDefinitionCount != 1)
	{
		return EFU_NetDriverLeaseResult::GameNetDriverDuplicate;
	}
	if (Snapshot.bHasLiveTargetNamedDriver || Snapshot.bHasPendingNetGame || Snapshot.bHasPendingTravel)
	{
		return EFU_NetDriverLeaseResult::ActiveOrPendingDriver;
	}

	return EFU_NetDriverLeaseResult::Acquired;
}

EFU_NetDriverLeaseResult FFU_OnlineSessionNetDriverLease::EvaluateRestore(
	const FFU_NetDriverLeaseFingerprint& Expected,
	const FFU_NetDriverLeaseFingerprint& Current,
	const bool bAllWorldsClear)
{
	// 指纹失配与 World 是否仍活动无关；先毒化可立即停止 ticker，且此分支从不写引擎状态。
	if (!FUOnlineSessionNetDriverLeasePrivate::HasSameDefinitionIdentity(Expected, Current))
	{
		return EFU_NetDriverLeaseResult::ExternallyModified;
	}

	if (!bAllWorldsClear)
	{
		return EFU_NetDriverLeaseResult::ReleaseDeferred;
	}

	return EFU_NetDriverLeaseResult::Restored;
}

bool FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(
	const FFU_NetDriverLeaseReleaseSnapshot& Snapshot)
{
	// 显式恢复是最后一道安全门：错误线程或进程已 poison 时没有足够证据读取/信任全局状态；
	// 任意租约仍存在也必须失败，即使它看似属于另一 owner/provider，避免与刚开始的请求交错复位 Provider。
	return Snapshot.bIsGameThread
		&& Snapshot.bOwnerValid
		&& !Snapshot.bProcessLeasePoisoned
		&& !Snapshot.bLeaseExists;
}

EFU_NetDriverLeaseResult FFU_OnlineSessionNetDriverLease::ProbeAcquire(
	UGameInstance* Owner,
	const EFU_OnlineProvider Provider,
	const FName DesiredDriverClass)
{
	// 全局租约、弱 UObject 所有者和 GEngine WorldContexts 统一限定在游戏线程访问，避免数据竞态。
	if (!IsInGameThread())
	{
		return EFU_NetDriverLeaseResult::NotGameThread;
	}
	// UObject 与 World 有效性也是协调器自己的契约，不能依赖某一个 Subsystem 调用点代为保证。
	if (!IsValid(Owner) || Owner->GetWorld() == nullptr)
	{
		return EFU_NetDriverLeaseResult::OwnerUnavailable;
	}
	if (FUOnlineSessionNetDriverLeasePrivate::bProcessLeasePoisoned)
	{
		return EFU_NetDriverLeaseResult::ExternallyModified;
	}

	return EvaluateAcquire(FUOnlineSessionNetDriverLeasePrivate::CapturePreflight(
		Owner,
		Provider,
		DesiredDriverClass));
}

EFU_NetDriverLeaseResult FFU_OnlineSessionNetDriverLease::Acquire(
	UGameInstance& Owner,
	const EFU_OnlineProvider Provider,
	const FName DesiredDriverClass)
{
	if (DesiredDriverClass.IsNone())
	{
		return EFU_NetDriverLeaseResult::DesiredDriverMismatch;
	}

	EFU_NetDriverLeaseResult PreflightResult = ProbeAcquire(&Owner, Provider, DesiredDriverClass);
	if (PreflightResult == EFU_NetDriverLeaseResult::StaleOwnerReclaimable)
	{
		using namespace FUOnlineSessionNetDriverLeasePrivate;
		// 不能把旧 FU 安装值直接当成新基线；先按完整指纹恢复真正 Original，再重新执行首租快照。
		GLease->bReleaseRequested = true;
		const EFU_NetDriverLeaseResult RestoreResult = TryRestoreLease();
		if (RestoreResult != EFU_NetDriverLeaseResult::Restored)
		{
			if (RestoreResult == EFU_NetDriverLeaseResult::ReleaseDeferred)
			{
				EnsureDeferredReleaseTicker();
			}
			return RestoreResult;
		}

		PreflightResult = ProbeAcquire(&Owner, Provider, DesiredDriverClass);
	}
	if (PreflightResult == EFU_NetDriverLeaseResult::AlreadyOwned)
	{
		using namespace FUOnlineSessionNetDriverLeasePrivate;
		// 延迟释放与新一轮 Create/Join 可能在相邻帧交错；同所有者重租必须取消旧请求，
		// 否则 ticker 会在本次网络驱动创建前突然恢复全局定义。
		if (GLease.IsSet() && GLease->bReleaseRequested)
		{
			if (GLease->DeferredReleaseTicker.IsValid())
			{
				FTSTicker::RemoveTicker(GLease->DeferredReleaseTicker);
				GLease->DeferredReleaseTicker.Reset();
			}
			GLease->bReleaseRequested = false;
		}
		return PreflightResult;
	}
	if (PreflightResult == EFU_NetDriverLeaseResult::ExternallyModified
		&& FUOnlineSessionNetDriverLeasePrivate::GLease.IsSet())
	{
		// Acquire 是写入口；发现既有安装值不再匹配时在这里正式毒化，后续操作一律只读失败。
		FUOnlineSessionNetDriverLeasePrivate::PoisonAndAbandonLease();
		return PreflightResult;
	}
	if (PreflightResult != EFU_NetDriverLeaseResult::Acquired)
	{
		return PreflightResult;
	}

	// EvaluateAcquire 已确认唯一性；在同一游戏线程内重新找目标并立即快照，避免保存一份未经验证的指针。
	FNetDriverDefinition* TargetDefinition = nullptr;
	int32 TargetIndex = INDEX_NONE;
	for (int32 Index = 0; GEngine && Index < GEngine->NetDriverDefinitions.Num(); ++Index)
	{
		FNetDriverDefinition& Definition = GEngine->NetDriverDefinitions[Index];
		if (Definition.DefName == NAME_GameNetDriver)
		{
			TargetDefinition = &Definition;
			TargetIndex = Index;
			break;
		}
	}
	if (TargetDefinition == nullptr)
	{
		return EFU_NetDriverLeaseResult::GameNetDriverMissing;
	}

	FUOnlineSessionNetDriverLeasePrivate::FLeaseState& Lease =
		FUOnlineSessionNetDriverLeasePrivate::GLease.Emplace();
	Lease.Owner = &Owner;
	Lease.Provider = Provider;
	Lease.Fingerprint = FUOnlineSessionNetDriverLeasePrivate::CaptureFingerprintAtIndex(TargetIndex);
	Lease.Fingerprint.OriginalValue = FFU_NetDriverDefinitionValue::FromDefinition(*TargetDefinition);
	Lease.Fingerprint.InstalledValue = BuildInstalledValue(
		Lease.Fingerprint.OriginalValue,
		DesiredDriverClass);

	// 【最小引擎写入】只覆盖本功能确实需要的两个字段；连接通道和并行 Tick 设置原样保留。
	TargetDefinition->DriverClassName = DesiredDriverClass;
	TargetDefinition->DriverClassNameFallback = DesiredDriverClass;

	UE_LOG(
		LogFUOnlineSession,
		Display,
		TEXT("已获取 GameNetDriver 租约 Provider=%s Driver=%s"),
		Provider == EFU_OnlineProvider::Steam ? TEXT("Steam") : TEXT("Lan/NULL"),
		*DesiredDriverClass.ToString());
	return EFU_NetDriverLeaseResult::Acquired;
}

void FFU_OnlineSessionNetDriverLease::RequestRelease(
	UGameInstance* Owner,
	const EFU_OnlineProvider Provider,
	const TCHAR* Reason)
{
	using namespace FUOnlineSessionNetDriverLeasePrivate;
	// 本 API 不做跨线程派发：调用者若已经开始析构，捕获 UObject 再排队反而可能产生悬空语义。
	// 所有 Subsystem/引擎失败委托正常都在游戏线程；异常调用直接保守拒绝且不读取全局租约。
	if (!IsInGameThread())
	{
		UE_LOG(LogFUOnlineSession, Error, TEXT("拒绝从非游戏线程释放 GameNetDriver 租约"));
		return;
	}
	if (!GLease.IsSet())
	{
		return;
	}

	// 另一 GameInstance 不能释放当前租约；Owner=null 只允许在原 Owner 已失效的销毁收尾中使用。
	if (GLease->Provider != Provider)
	{
		UE_LOG(
			LogFUOnlineSession,
			Warning,
			TEXT("忽略不匹配 Provider 的租约释放请求：Requested=%s Lease=%s Reason=%s"),
			Provider == EFU_OnlineProvider::Steam ? TEXT("Steam") : TEXT("Lan/NULL"),
			GLease->Provider == EFU_OnlineProvider::Steam ? TEXT("Steam") : TEXT("Lan/NULL"),
			Reason ? Reason : TEXT("Unknown"));
		return;
	}
	if ((Owner != nullptr && GLease->Owner.Get() != Owner)
		|| (Owner == nullptr && GLease->Owner.IsValid()))
	{
		UE_LOG(
			LogFUOnlineSession,
			Warning,
			TEXT("忽略非租约所有者的 GameNetDriver 释放请求：Provider=%s Reason=%s"),
			Provider == EFU_OnlineProvider::Steam ? TEXT("Steam") : TEXT("Lan/NULL"),
			Reason ? Reason : TEXT("Unknown"));
		return;
	}

	GLease->bReleaseRequested = true;
	UE_LOG(LogFUOnlineSession, Display, TEXT("请求释放 GameNetDriver 租约：%s"), Reason ? Reason : TEXT("Unknown"));

	const EFU_NetDriverLeaseResult Result = TryRestoreLease();
	if (Result == EFU_NetDriverLeaseResult::ReleaseDeferred || Result == EFU_NetDriverLeaseResult::NotGameThread)
	{
		EnsureDeferredReleaseTicker();
	}
}

void FFU_OnlineSessionNetDriverLease::TickDeferredRelease()
{
	using namespace FUOnlineSessionNetDriverLeasePrivate;
	if (!IsInGameThread())
	{
		return;
	}
	if (!GLease.IsSet() || !GLease->bReleaseRequested)
	{
		return;
	}

	const EFU_NetDriverLeaseResult Result = TryRestoreLease();
	if (Result != EFU_NetDriverLeaseResult::ReleaseDeferred && Result != EFU_NetDriverLeaseResult::NotGameThread)
	{
		UE_LOG(LogFUOnlineSession, Display, TEXT("GameNetDriver 延迟恢复结果：%s"), ToText(Result));
	}
}

bool FFU_OnlineSessionNetDriverLease::IsReleaseComplete(
	UGameInstance* Owner,
	const EFU_OnlineProvider Provider)
{
	using namespace FUOnlineSessionNetDriverLeasePrivate;

	FFU_NetDriverLeaseReleaseSnapshot Snapshot;
	Snapshot.bIsGameThread = IsInGameThread();
	if (!Snapshot.bIsGameThread)
	{
		return false;
	}
	Snapshot.bOwnerValid = IsValid(Owner);
	if (!Snapshot.bOwnerValid)
	{
		return false;
	}

	Snapshot.bProcessLeasePoisoned = bProcessLeasePoisoned;
	Snapshot.bLeaseExists = GLease.IsSet();
	if (GLease.IsSet())
	{
		// 精确匹配信息仅用于保守判定与测试可见语义；EvaluateReleaseComplete 不会把“不匹配”
		// 当成释放完成，因为该现存租约可能已经开始新的旅行或属于另一 PIE 实例。
		Snapshot.bSameOwner = Owner != nullptr && GLease->Owner.Get() == Owner;
		Snapshot.bSameProvider = GLease->Provider == Provider;
	}

	return EvaluateReleaseComplete(Snapshot);
}

bool FFU_OnlineSessionNetDriverLease::AreAllWorldsClearForRecovery()
{
	using namespace FUOnlineSessionNetDriverLeasePrivate;
	// 与租约恢复复用同一份全进程扫描，避免 TryRecoverProvider 只看当前 PIE World 而与 ticker 规则漂移。
	// poisoned 状态不影响“是否有驱动”的只读事实，但错误线程不能遍历 UObject/WorldContext。
	return IsInGameThread() && AreAllWorldsClear(nullptr);
}

void FFU_OnlineSessionNetDriverLease::ShutdownModule()
{
	using namespace FUOnlineSessionNetDriverLeasePrivate;
	if (!GLease.IsSet())
	{
		return;
	}

	if (GLease->DeferredReleaseTicker.IsValid())
	{
		FTSTicker::RemoveTicker(GLease->DeferredReleaseTicker);
		GLease->DeferredReleaseTicker.Reset();
	}

	// 模块卸载后不允许保留会回调已卸载代码的 ticker；仅尝试一次安全恢复，绝不强制覆盖。
	GLease->bReleaseRequested = true;
	const EFU_NetDriverLeaseResult Result = TryRestoreLease();
	if (GLease.IsSet())
	{
		UE_LOG(
			LogFUOnlineSession,
			Warning,
			TEXT("Runtime 卸载时无法安全恢复 GameNetDriver（%s）；保留当前引擎值，建议重启进程。"),
			ToText(Result));
		ResetLeaseState();
	}
}

bool FFU_OnlineSessionNetDriverLease::IsAcquireSuccess(const EFU_NetDriverLeaseResult Result)
{
	return Result == EFU_NetDriverLeaseResult::Acquired
		|| Result == EFU_NetDriverLeaseResult::AlreadyOwned;
}

bool FFU_OnlineSessionNetDriverLease::IsProbeSuccess(const EFU_NetDriverLeaseResult Result)
{
	return IsAcquireSuccess(Result)
		|| Result == EFU_NetDriverLeaseResult::StaleOwnerReclaimable;
}

const TCHAR* FFU_OnlineSessionNetDriverLease::GetResultName(const EFU_NetDriverLeaseResult Result)
{
	return FUOnlineSessionNetDriverLeasePrivate::ToText(Result);
}
