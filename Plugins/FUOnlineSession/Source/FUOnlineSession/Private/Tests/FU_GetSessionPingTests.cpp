#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FU_OnlineSessionSubsystem.h"
#include "FU_SessionPingQuery.h"
#include "Engine/GameInstance.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUGetSessionPingBlueprintTest, "FUOnlineSession.SessionPing.BlueprintQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUGetSessionPingBlueprintTest::RunTest(const FString& Parameters)
{
	// 通过实际反射入口调用：捕获节点未导出、返回类型错误或无结果时误显示 0ms 的回归。
	UFunction* Function = UFU_OnlineSessionSubsystem::StaticClass()->FindFunctionByName(TEXT("GetSessionPing"));
	if (!TestNotNull(TEXT("蓝图可找到 Get Session Ping"), Function))
	{
		return false;
	}
	TestTrue(TEXT("读取缓存的节点为 BlueprintPure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
	FIntProperty* PingProperty = FindFProperty<FIntProperty>(Function, TEXT("ReturnValue"));
	FBoolProperty* EstimatedProperty = FindFProperty<FBoolProperty>(Function, TEXT("bIsEstimated"));
	if (!TestNotNull(TEXT("Ping 输出为整数毫秒"), PingProperty)
		|| !TestNotNull(TEXT("保留估算值标记"), EstimatedProperty))
	{
		return false;
	}
	TStrongObjectPtr<UGameInstance> Instance{NewObject<UGameInstance>()};
	TStrongObjectPtr<UFU_OnlineSessionSubsystem> Subsystem{NewObject<UFU_OnlineSessionSubsystem>(Instance.Get())};
	FStructOnScope Params(Function);
	EstimatedProperty->SetPropertyValue_InContainer(Params.GetStructMemory(), true);
	Subsystem->ProcessEvent(Function, Params.GetStructMemory());
	TestEqual(TEXT("尚未搜索或空 SessionId 统一返回 999"), PingProperty->GetPropertyValue_InContainer(Params.GetStructMemory()), 999);
	TestFalse(TEXT("无有效 Ping 清除旧估算标记"), EstimatedProperty->GetPropertyValue_InContainer(Params.GetStructMemory()));
	return true;
}


namespace
{
	// 只替代 Provider 专有的不透明身份数据；查询、原生搜索结果和缓存均使用生产类型。
	class FSessionPingTestInfo final : public FOnlineSessionInfo
	{
	public:
		explicit FSessionPingTestInfo(const FString& InId)
			: Id(FUniqueNetIdString::Create(InId, FName(TEXT("FU_Test")))) {}
		virtual const uint8* GetBytes() const override { return Id->GetBytes(); }
		virtual int32 GetSize() const override { return Id->GetSize(); }
		virtual bool IsValid() const override { return Id->IsValid(); }
		virtual FString ToString() const override { return Id->ToString(); }
		virtual FString ToDebugString() const override { return Id->ToDebugString(); }
		virtual const FUniqueNetId& GetSessionId() const override { return *Id; }
	private:
		FUniqueNetIdStringRef Id;
	};

	// 构造完整有效身份，避免因无效测试样本而误以为正常的 Ping 查询也失败。
	FOnlineSessionSearchResult MakePingResult(const TCHAR* SessionId, int32 Ping)
	{
		FOnlineSessionSearchResult Result;
		Result.Session.OwningUserId = FUniqueNetIdString::Create(TEXT("Host"), FName(TEXT("FU_Test")));
		Result.Session.SessionInfo = MakeShared<FSessionPingTestInfo>(SessionId);
		Result.PingInMs = Ping;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUGetSessionPingProviderTest, "FUOnlineSession.SessionPing.ProviderIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUGetSessionPingProviderTest::RunTest(const FString& Parameters)
{
	// 同名 SessionId 在两个 Provider 中必须读取各自的数据，估算标记也不能泄漏到下一次输出。
	const TArray<FOnlineSessionSearchResult> Steam{MakePingResult(TEXT("same"), 42)};
	const TArray<FOnlineSessionSearchResult> Lan{MakePingResult(TEXT("same"), 8)};
	bool bEstimated = false;
	TestEqual(TEXT("Steam 读取对应原生缓存"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("same"), bEstimated), 42);
	TestTrue(TEXT("Steam 值标记为估算"), bEstimated);
	TestEqual(TEXT("LAN 不串用 Steam 缓存"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Lan, TEXT("same"), bEstimated), 8);
	TestFalse(TEXT("LAN 清除 Steam 估算标记"), bEstimated);
	TestEqual(TEXT("非法 Provider 返回 999"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, static_cast<EFU_OnlineProvider>(255), TEXT("same"), bEstimated), 999);
	TestFalse(TEXT("非法 Provider 不返回估算标记"), bEstimated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUGetSessionPingValueTest, "FUOnlineSession.SessionPing.ValuesAndFallback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUGetSessionPingValueTest::RunTest(const FString& Parameters)
{
	// 锁住用户要求的 999 兜底，同时防止实现误把所有大于 999 的真实延迟截断。
	TArray<FOnlineSessionSearchResult> Steam{MakePingResult(TEXT("room"), 42)};
	const TArray<FOnlineSessionSearchResult> Lan;
	bool bEstimated = true;
	for (const int32 InvalidPing : {-1, -5, 9999, 10000})
	{
		Steam[0].PingInMs = InvalidPing;
		TestEqual(TEXT("无有效 Ping 统一返回 999"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("room"), bEstimated), 999);
		TestFalse(TEXT("兜底值不是有效估算"), bEstimated);
	}
	for (const int32 ValidPing : {0, 1, 250, 999, 1200, 9998})
	{
		Steam[0].PingInMs = ValidPing;
		TestEqual(TEXT("有效 Ping 原样返回"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("room"), bEstimated), ValidPing);
		TestTrue(TEXT("有效 Steam 结果仍是估算值"), bEstimated);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUGetSessionPingCacheTest, "FUOnlineSession.SessionPing.CacheLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUGetSessionPingCacheTest::RunTest(const FString& Parameters)
{
	// 搜索缓存更换或清空后，不得返回上一批房间的 Ping；损坏的原生身份也必须安全兜底。
	TArray<FOnlineSessionSearchResult> Steam{MakePingResult(TEXT("old"), 50)};
	const TArray<FOnlineSessionSearchResult> Lan;
	bool bEstimated = true;
	TestEqual(TEXT("空 ID 返回 999"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT(""), bEstimated), 999);
	TestEqual(TEXT("房间不存在返回 999"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("missing"), bEstimated), 999);
	Steam.Reset();
	TestEqual(TEXT("缓存清空后旧 ID 返回 999"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("old"), bEstimated), 999);
	Steam.Add(MakePingResult(TEXT("new"), 25));
	TestEqual(TEXT("新搜索结果立即可读"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("new"), bEstimated), 25);
	Steam[0].Session.SessionInfo.Reset();
	TestEqual(TEXT("无效原生身份返回 999"), FUOnlineSession::GetCachedSessionPing(Steam, Lan, EFU_OnlineProvider::Steam, TEXT("new"), bEstimated), 999);
	TestFalse(TEXT("失败时不残留估算标记"), bEstimated);
	return true;
}

#endif
