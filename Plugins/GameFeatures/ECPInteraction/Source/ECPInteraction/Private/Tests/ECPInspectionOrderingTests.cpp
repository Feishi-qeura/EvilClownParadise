#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupTypes.h"
#include "Misc/AutomationTest.h"

namespace
{
FECPInspectionRotationSample MakeInspectionSample(uint32 Generation, uint32 Session,
    uint32 Sequence, double Yaw, bool bFinal = false)
{
    FECPInspectionRotationSample Sample;
    Sample.Generation = Generation;
    Sample.Session = Session;
    Sample.Sequence = Sequence;
    Sample.Rotation = FRotator(0.0, Yaw, 0.0);
    Sample.bFinal = bFinal;
    return Sample;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPInspectionOrderingTest,
    "ECP.Pickup.Rotation.AbsoluteOrdering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPInspectionOrderingTest::RunTest(const FString& Parameters)
{
    FECPInspectionRotationSample State = MakeInspectionSample(7, 1, 1, 10.0);
    const auto Receive = [&State](const FECPInspectionRotationSample& Incoming)
    {
        if (!ECPPickupRules::CanAcceptInspectionSample(State, Incoming)) return false;
        State = Incoming;
        return true;
    };

    // 故意缺少第二包；第三包是绝对目标，不能像增量协议那样永远少转一次。
    TestTrue(TEXT("Missing middle sample does not block the newest target"),
        Receive(MakeInspectionSample(7, 1, 3, 90.0)));
    TestEqual(TEXT("The surviving sample fully catches up"), State.Rotation.Yaw, 90.0);
    TestFalse(TEXT("A delayed sample cannot rewind the item"),
        Receive(MakeInspectionSample(7, 1, 2, 40.0)));
    TestFalse(TEXT("Duplicate samples cannot accumulate rotation"),
        Receive(MakeInspectionSample(7, 1, 3, 90.0)));

    // 回到零仍然是一份有效可靠定稿，不能用 IsNearlyZero 判断是否需要提交。
    TestTrue(TEXT("Returning to zero still closes the drag"),
        Receive(MakeInspectionSample(7, 1, 4, 0.0, true)));
    TestEqual(TEXT("The final target may be zero"), State.Rotation.Yaw, 0.0);
    TestFalse(TEXT("A stream packet after Final cannot reopen the session"),
        Receive(MakeInspectionSample(7, 1, 5, 160.0)));
    TestTrue(TEXT("A new drag can start after Final"),
        Receive(MakeInspectionSample(7, 2, 1, 15.0)));
    TestFalse(TEXT("An old reliable Final cannot overwrite a newer drag"),
        Receive(MakeInspectionSample(7, 1, 6, 120.0, true)));

    TestFalse(TEXT("Re-pickup rejects packets from the previous holding generation"),
        ECPPickupRules::CanAcceptInspectionSample(
            MakeInspectionSample(8, 0, 0, 0.0), MakeInspectionSample(7, 99, 99, 20.0, true)));
    TestFalse(TEXT("A client cannot advance the server holding generation"),
        Receive(MakeInspectionSample(8, 3, 1, 20.0)));
    TestTrue(TEXT("Sequence comparison handles unsigned wrap"),
        ECPPickupRules::IsNewerInspectionSequence(1, MAX_uint32));
    TestFalse(TEXT("Wrapped old sequence is rejected"),
        ECPPickupRules::IsNewerInspectionSequence(MAX_uint32, 1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPInspectionAcknowledgementTest,
    "ECP.Pickup.Rotation.PredictionAcknowledgement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPInspectionAcknowledgementTest::RunTest(const FString& Parameters)
{
    const FECPInspectionRotationSample Predicted = MakeInspectionSample(4, 2, 8, 30.0);
    // 角度相同可能只是往返拖动恰好经过同一姿态，不能据此确认尚未处理的输入。
    TestTrue(TEXT("Matching angle with an old sequence preserves prediction"),
        ECPPickupRules::ShouldRetainInspectionPrediction(MakeInspectionSample(4, 2, 7, 30.0), Predicted));
    TestTrue(TEXT("An older drag acknowledgement cannot clear a new drag"),
        ECPPickupRules::ShouldRetainInspectionPrediction(MakeInspectionSample(4, 1, 99, 30.0, true), Predicted));
    TestFalse(TEXT("Acknowledging the newest sequence clears prediction even if authority corrected the angle"),
        ECPPickupRules::ShouldRetainInspectionPrediction(MakeInspectionSample(4, 2, 8, 25.0), Predicted));
    TestFalse(TEXT("A new holding generation always invalidates old prediction"),
        ECPPickupRules::ShouldRetainInspectionPrediction(MakeInspectionSample(5, 0, 0, 0.0), Predicted));
    TestFalse(TEXT("Newer authority wins over an older local sample"),
        ECPPickupRules::ShouldRetainInspectionPrediction(MakeInspectionSample(4, 2, 9, 40.0), Predicted));
    return true;
}
#endif

