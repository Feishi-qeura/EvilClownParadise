#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "FU_OnlineSessionTypes.h"

class UGameInstance;

/**
 * GameNetDriver 是 GEngine 进程级数组中的共享定义，不能由任意 GameInstance 随意改写。
 * 该结果同时覆盖纯快照测试和真实协调器，使每种拒绝原因可以在上层转成可诊断事件。
 */
enum class EFU_NetDriverLeaseResult : uint8
{
	Acquired,
	AlreadyOwned,
	GameNetDriverMissing,
	GameNetDriverDuplicate,
	ActiveOrPendingDriver,
	OwnedByAnotherGameInstance,
	StaleOwnerReclaimable,
	ProviderSwitchBlocked,
	DesiredDriverMismatch,
	ExternallyModified,
	NotGameThread,
	OwnerUnavailable,
	Restored,
	ReleaseDeferred
};

/**
 * FNetDriverDefinition 在 UE 5.8 中真正影响本任务的全部五个字段。
 * 以值对象保存它们，避免在恢复时用整结构复制覆盖引擎未来追加的字段。
 */
struct FFU_NetDriverDefinitionValue
{
	FName DefName = NAME_None;
	FName DriverClassName = NAME_None;
	FName DriverClassNameFallback = NAME_None;
	int32 MaxChannelsOverride = INDEX_NONE;
	bool bRunParallelConnectionTick = false;

	static FFU_NetDriverDefinitionValue FromDefinition(const FNetDriverDefinition& Definition);
	bool Equals(const FFU_NetDriverDefinitionValue& Other) const;
};

/**
 * 恢复前必须满足的完整身份指纹。
 *
 * 指针、索引、数组长度和名称顺序共同防止数组重分配、插入/删除或重排后，
 * 把保存的旧值写回到错误的 NetDriverDefinition 上。
 */
struct FFU_NetDriverLeaseFingerprint
{
	const FNetDriverDefinition* DefinitionPointer = nullptr;
	int32 DefinitionIndex = INDEX_NONE;
	int32 DefinitionArrayNum = 0;
	TArray<FName> OrderedDefinitionNames;
	FFU_NetDriverDefinitionValue OriginalValue;
	FFU_NetDriverDefinitionValue InstalledValue;
};

/** 把真实引擎状态压缩为可无引擎运行的纯决策输入。 */
struct FFU_NetDriverLeasePreflight
{
	bool bIsGameThread = false;
	int32 GameNetDriverDefinitionCount = 0;
	bool bHasLiveTargetNamedDriver = false;
	bool bHasPendingNetGame = false;
	bool bHasPendingTravel = false;
	bool bLeaseExists = false;
	bool bLeaseOwnerExpired = false;
	bool bSameOwner = false;
	bool bSameProvider = false;
	bool bSameDesiredDriver = true;
	bool bInstalledValueStillMatches = false;
};

/**
 * 释放完成查询的纯快照。该查询与 ProbeAcquire 完全分离：能再次获取不等于原租约已经安全恢复。
 */
struct FFU_NetDriverLeaseReleaseSnapshot
{
	bool bIsGameThread = false;
	bool bOwnerValid = false;
	bool bProcessLeasePoisoned = false;
	bool bLeaseExists = false;
	bool bSameOwner = false;
	bool bSameProvider = false;
};

/**
 * 进程级 GameNetDriver 租约。
 *
 * Provider traits 仍是 Steam/LAN 类名的唯一来源；本类只负责把已选择的类名安全地临时安装到
 * 引擎唯一的 GameNetDriver 定义，并在所有 World 安全后恢复。它不插入或删除任何数组元素。
 */
class FFU_OnlineSessionNetDriverLease final
{
public:
	/**
	 * 根据原定义生成将要安装的完整值快照。
	 * 该纯函数明确保证只替换主/回退类名，其余三个 UE 5.8 字段保持不变，并可被自动化测试直接验证。
	 */
	static FFU_NetDriverDefinitionValue BuildInstalledValue(
		const FFU_NetDriverDefinitionValue& Original,
		FName DesiredDriverClass);

	static EFU_NetDriverLeaseResult EvaluateAcquire(const FFU_NetDriverLeasePreflight& Snapshot);
	static EFU_NetDriverLeaseResult EvaluateRestore(
		const FFU_NetDriverLeaseFingerprint& Expected,
		const FFU_NetDriverLeaseFingerprint& Current,
		bool bAllWorldsClear);
	static bool EvaluateReleaseComplete(const FFU_NetDriverLeaseReleaseSnapshot& Snapshot);

	/** 只读取当前进程状态；用于 ProviderStatus 在不改写 GEngine 的前提下预报租约冲突。 */
	static EFU_NetDriverLeaseResult ProbeAcquire(
		UGameInstance* Owner,
		EFU_OnlineProvider Provider,
		FName DesiredDriverClass = NAME_None);

	/**
	 * 在通过所有前置检查后原位设置 GameNetDriver 的主/回退类名。
	 * 成功或同租约复用时才允许调用方继续 Create/Join；其余结果均不能改变任何引擎定义。
	 */
	static EFU_NetDriverLeaseResult Acquire(
		UGameInstance& Owner,
		EFU_OnlineProvider Provider,
		FName DesiredDriverClass);

	/**
	 * 请求恢复而不是强制恢复；Owner 与 Provider 必须同时匹配，避免同一 GameInstance 的错误蓝图入口
	 * 释放另一种 Provider 的租约。存在任一目标驱动、PendingNetGame 或已排队 Travel 时会由 ticker 延迟重试。
	 */
	static void RequestRelease(
		UGameInstance* Owner,
		EFU_OnlineProvider Provider,
		const TCHAR* Reason);
	static void TickDeferredRelease();

	/**
	 * 只读确认进程级租约已经完全不存在；错误线程、poisoned 状态或任意现存租约都保守返回 false。
	 * Owner+Provider 作为调用身份参与快照，绝不调用 Acquire、Restore、Cancel 或改写 GEngine。
	 */
	static bool IsReleaseComplete(UGameInstance* Owner, EFU_OnlineProvider Provider);

	/** 只读扫描全部 WorldContext；任一活动 GameNetDriver、PendingNetGame 或已排队 Travel 都返回 false。 */
	static bool AreAllWorldsClearForRecovery();

	/** Runtime 模块卸载时撤销 ticker，并且只在完整安全指纹仍成立时进行最后一次恢复。 */
	static void ShutdownModule();

	static bool IsAcquireSuccess(EFU_NetDriverLeaseResult Result);
	/** Probe 可把“失效旧 Owner 且当前安全”视为可尝试；Acquire 会先恢复旧基线再重新获取。 */
	static bool IsProbeSuccess(EFU_NetDriverLeaseResult Result);
	static const TCHAR* GetResultName(EFU_NetDriverLeaseResult Result);
};
