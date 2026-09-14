#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Diagnostics/FU_OnlineSessionDiagnostics.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUProviderLogPersistenceTest,
	"FUOnlineSession.Diagnostics.ProviderLog.Persistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUProviderLogPersistenceTest::RunTest(const FString& Parameters)
{
	// 使用真实写入器并重新读磁盘：能抓住覆盖写入、错误路由、未脱敏及历史清理误删文件等回归。
	FFU_OnlineDiagnosticDispatchConfig Config;
	Config.HistoryLimit = 1;
	Config.bEnableOverlay = false;
	FFU_OnlineSessionDiagnostics Diagnostics(Config, {}, {},
		[](const FFU_OnlineDiagnosticEvent&, const FString&) {});
	const FString SteamPath = Diagnostics.GetProviderLogPath(EFU_OnlineProvider::Steam);
	const FString LanPath = Diagnostics.GetProviderLogPath(EFU_OnlineProvider::Lan);
	TestTrue(TEXT("Steam 文件在专用目录"), FPaths::GetPath(SteamPath).EndsWith(TEXT("FUOnlineSession/OnlineSubsystemLog/steam_log")));
	TestTrue(TEXT("LAN 文件在专用目录"), FPaths::GetPath(LanPath).EndsWith(TEXT("FUOnlineSession/OnlineSubsystemLog/lan_log")));
	TestFalse(TEXT("公开路径为绝对路径"), FPaths::IsRelative(SteamPath));
	TestTrue(TEXT("文件位于游戏日志根目录内"), FPaths::IsUnderDirectory(SteamPath, FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir())));
	TestFalse(TEXT("读取路径不会提前创建文件"), IFileManager::Get().FileExists(*SteamPath));

	FFU_OnlineDiagnosticEvent Event;
	Event.Provider = EFU_OnlineProvider::Steam;
	Event.Code = TEXT("FU.Test.SteamFirst");
	Event.TimestampUtc = FDateTime(2026, 9, 13, 1, 2, 3);
	Event.Message = TEXT("中文日志追加测试");
	FFU_OnlineDiagnosticField Password;
	Password.Key = TEXT("RoomPassword");
	Password.Value = TEXT("provider-file-secret");
	Event.Fields.Add(Password);
	Diagnostics.Emit(Event);
	Event.Code = TEXT("FU.Test.SteamSecond");
	Diagnostics.Emit(Event);
	Event.Provider = EFU_OnlineProvider::Lan;
	Event.Code = TEXT("FU.Test.LanOnly");
	Diagnostics.Emit(Event);
	Diagnostics.ClearHistory();

	FString SteamText;
	FString LanText;
	TestTrue(TEXT("Steam 日志已自动写入"), FFileHelper::LoadFileToString(SteamText, *SteamPath));
	TestTrue(TEXT("LAN 日志已自动写入"), FFileHelper::LoadFileToString(LanText, *LanPath));
	TestTrue(TEXT("历史溢出及清除均不丢失第一条磁盘记录"), SteamText.Contains(TEXT("FU.Test.SteamFirst")));
	TestTrue(TEXT("第二条追加而非覆盖"), SteamText.Contains(TEXT("FU.Test.SteamSecond")));
	TestTrue(TEXT("文件保留事件 UTC 时间"), SteamText.Contains(TEXT("2026-09-13T01:02:03")));
	TestTrue(TEXT("UTF-8 中文可完整读回"), SteamText.Contains(TEXT("中文日志追加测试")));
	TestTrue(TEXT("文件保留统一事件序号"), SteamText.Contains(TEXT("[Seq=2]")));
	TestFalse(TEXT("Steam 文件不混入 LAN 事件"), SteamText.Contains(TEXT("FU.Test.LanOnly")));
	TestTrue(TEXT("LAN 事件进入 LAN 文件"), LanText.Contains(TEXT("FU.Test.LanOnly")));
	TestFalse(TEXT("LAN 文件不混入 Steam 事件"), LanText.Contains(TEXT("FU.Test.SteamFirst")));
	TestFalse(TEXT("磁盘文件不能泄露原始密码"), SteamText.Contains(TEXT("provider-file-secret")));
	TestTrue(TEXT("磁盘文件保留脱敏标记"), SteamText.Contains(TEXT("RoomPassword=<redacted>")));

	// 同一进程的多个 PIE GameInstance 也必须生成不同文件；避免共享文件时序号互相覆盖。
	FFU_OnlineSessionDiagnostics OtherInstance(Config, {});
	TestNotEqual(TEXT("不同诊断实例不复用同一文件"), OtherInstance.GetProviderLogPath(EFU_OnlineProvider::Steam), SteamPath);
	TestEqual(TEXT("当前实例路径保持稳定"), Diagnostics.GetProviderLogPath(EFU_OnlineProvider::Steam), SteamPath);
	// 只清理本测试生成的两个唯一文件，不递归删除目录或其他运行留下的日志。
	IFileManager::Get().Delete(*SteamPath);
	IFileManager::Get().Delete(*LanPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUProviderLogFailureTest,
	"FUOnlineSession.Diagnostics.ProviderLog.FailureIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUProviderLogFailureTest::RunTest(const FString& Parameters)
{
	// 仅替换磁盘边界模拟拒绝写入：验证真实分发器不会递归重试，且故障只隔离对应 Provider。
	FFU_OnlineDiagnosticDispatchConfig Config;
	Config.bEnableOverlay = false;
	int32 SteamAttempts = 0;
	int32 LanAttempts = 0;
	TArray<FFU_OnlineDiagnosticEvent> Broadcasts;
	TArray<FString> LogLines;
	FFU_OnlineSessionDiagnostics Diagnostics(Config,
		[&](const FFU_OnlineDiagnosticEvent& Event) { Broadcasts.Add(Event); }, {},
		[&](const FFU_OnlineDiagnosticEvent&, const FString& Line) { LogLines.Add(Line); },
		[&](const FString&, const FString& Destination)
		{
			if (Destination.Contains(TEXT("/steam_log/")))
			{
				++SteamAttempts;
				return false;
			}
			++LanAttempts;
			return true;
		});
	FFU_OnlineDiagnosticEvent Event;
	Event.Provider = EFU_OnlineProvider::Steam;
	Event.Code = TEXT("FU.Test.Original");
	Diagnostics.Emit(Event);
	Diagnostics.Emit(Event);
	Event.Provider = EFU_OnlineProvider::Lan;
	Diagnostics.Emit(Event);
	TestEqual(TEXT("失败 Provider 只尝试一次，不递归或刷屏"), SteamAttempts, 1);
	TestEqual(TEXT("另一个 Provider 仍能写入"), LanAttempts, 1);
	TestEqual(TEXT("三条原事件和一条故障均保留历史"), Diagnostics.GetHistory().Num(), 4);
	TestEqual(TEXT("所有事件继续送往 UE 日志出口"), LogLines.Num(), 4);
	TestEqual(TEXT("所有事件继续公开给 Blueprint"), Broadcasts.Num(), 4);
	if (Broadcasts.Num() == 4)
	{
		TestEqual(TEXT("原事件先广播"), Broadcasts[0].Code, FString(TEXT("FU.Test.Original")));
		TestEqual(TEXT("故障具有稳定错误码"), Broadcasts[1].Code, FString(TEXT("FU.Diagnostics.ProviderLogWriteFailed")));
		TestEqual(TEXT("故障归属正确 Provider"), Broadcasts[1].Provider, EFU_OnlineProvider::Steam);
		TestFalse(TEXT("故障提供后续处理建议"), Broadcasts[1].RecommendedAction.IsEmpty());
	}

	// 禁用日志与非法枚举都不能产生文件；Blueprint/历史仍是可用的诊断通道。
	Config.bEmitToLog = false;
	int32 UnexpectedWrites = 0;
	FFU_OnlineSessionDiagnostics Disabled(Config, {}, {}, {},
		[&](const FString&, const FString&) { ++UnexpectedWrites; return true; });
	Disabled.Emit(Event);
	TestEqual(TEXT("关闭日志禁止磁盘写入"), UnexpectedWrites, 0);
	TestEqual(TEXT("关闭日志仍保留历史"), Disabled.GetHistory().Num(), 1);
	Event.Provider = static_cast<EFU_OnlineProvider>(255);
	TestTrue(TEXT("非法 Provider 无写入路径"), Diagnostics.GetProviderLogPath(Event.Provider).IsEmpty());
	Diagnostics.Emit(Event);
	TestEqual(TEXT("非法 Provider 不误写 LAN"), LanAttempts, 1);
	return true;
}

#endif
