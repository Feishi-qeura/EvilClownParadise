#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FU_OnlineSessionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionDefaultResultTest,
	"FUOnlineSession.Types.DefaultResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionDefaultResultTest::RunTest(const FString& Parameters)
{
	FFU_SessionResult Result;
	TestEqual(TEXT("Default session id"), Result.SessionId, FString());
	TestEqual(TEXT("Default max players"), Result.MaxPlayers, 0);
	TestEqual(TEXT("Default current players"), Result.CurrentPlayers, 0);
	TestEqual(TEXT("Default ping"), Result.PingInMs, 0);
	return true;
}

#endif
