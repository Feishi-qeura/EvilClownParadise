#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineSessionRequestValidation.h"
#include "FU_CheckSessionStatusAsync.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionRequestValidationTest,
	"FUOnlineSession.Session.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionRequestValidationTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Reject zero players"), FFU_SessionRequestValidation::CanCreate(0, TEXT("Room")));
	TestFalse(TEXT("Reject empty room name"), FFU_SessionRequestValidation::CanCreate(2, FString()));
	TestTrue(TEXT("Accept a valid request"), FFU_SessionRequestValidation::CanCreate(2, TEXT("Room")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionPingNullWorldTest,
	"FUOnlineSession.Ping.NullWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionPingNullWorldTest::RunTest(const FString& Parameters)
{
	TestNull(TEXT("Null world context is rejected"), UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(nullptr, 1.0f));
	return true;
}

#endif
