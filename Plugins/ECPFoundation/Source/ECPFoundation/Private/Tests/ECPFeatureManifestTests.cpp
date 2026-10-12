#if WITH_DEV_AUTOMATION_TESTS
#include "Features/ECPGameFeatureSettings.h"
#include "Misc/AutomationTest.h"

namespace ECPFeatureManifestTest
{
	FECPGameFeatureDefinition Entry(FName Name, TArray<FName> Dependencies = {})
	{
		FECPGameFeatureDefinition Result;
		Result.PluginName = Name;
		Result.Dependencies = MoveTemp(Dependencies);
		Result.ProjectContentRoot = TEXT("/Game/Project/") + Name.ToString();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPFeatureManifestTest, "ECP.Features.ManifestValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPFeatureManifestTest::RunTest(const FString& Parameters)
{
	using namespace ECPFeatureManifestTest;
	TArray<FName> Order;
	FString Error;
	TestTrue(TEXT("Dependencies are sorted before consumers"), ECPFeatureManifest::BuildActivationOrder(
		{ Entry(TEXT("Interaction"), {TEXT("Character")}), Entry(TEXT("Character")) }, Order, Error));
	TestEqual(TEXT("Two unique features"), Order.Num(), 2);
	if (Order.Num() == 2)
	{
		TestEqual(TEXT("Character first"), Order[0], FName(TEXT("Character")));
		TestEqual(TEXT("Interaction second"), Order[1], FName(TEXT("Interaction")));
	}
	TestTrue(TEXT("Success clears old error"), Error.IsEmpty());

	TestFalse(TEXT("Duplicate names rejected"), ECPFeatureManifest::BuildActivationOrder(
		{ Entry(TEXT("A")), Entry(TEXT("A")) }, Order, Error));
	TestTrue(TEXT("No stale success after invalid manifest"), Order.IsEmpty());
	TestTrue(TEXT("Duplicate diagnostic names feature"), Error.Contains(TEXT("A")));
	TestFalse(TEXT("Missing dependency rejected"), ECPFeatureManifest::BuildActivationOrder(
		{ Entry(TEXT("A"), {TEXT("Missing")}) }, Order, Error));
	TestTrue(TEXT("Missing dependency is identified"), Error.Contains(TEXT("Missing")));
	TestFalse(TEXT("Cycles rejected"), ECPFeatureManifest::BuildActivationOrder(
		{ Entry(TEXT("A"), {TEXT("B")}), Entry(TEXT("B"), {TEXT("A")}) }, Order, Error));
	TestTrue(TEXT("Cycle does not yield a partial order"), Order.IsEmpty());
	TestFalse(TEXT("Empty manifest cannot silently start empty game"),
		ECPFeatureManifest::BuildActivationOrder({}, Order, Error));
	TestFalse(TEXT("Unnamed feature rejected"), ECPFeatureManifest::BuildActivationOrder({Entry(NAME_None)}, Order, Error));
	FECPGameFeatureDefinition InvalidRoot = Entry(TEXT("A"));
	InvalidRoot.ProjectContentRoot = TEXT("/Game/zxx/BMMCSystem");
	TestFalse(TEXT("Protected/template root cannot be claimed by feature"),
		ECPFeatureManifest::BuildActivationOrder({InvalidRoot}, Order, Error));
	return true;
}
#endif
