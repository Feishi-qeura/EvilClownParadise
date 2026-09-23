#include "Characters/ECPRagdollTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/AssertionMacros.h"
#include "Tests/EnsureScope.h"
#include "Misc/EngineNetworkCustomVersion.h"
#include "UObject/CoreNet.h"

namespace
{
	/** 用真实网络位流读写，验证量化误差和数组边界，而不是仅检查内存赋值。 */
	bool RoundTrip(FECPRagdollNetState Source, FECPRagdollNetState& Result, int64& OutBitCount)
	{
		FNetBitWriter Writer(nullptr, 0);
		Writer.SetAllowResize(true);
		Writer.SetEngineNetVer(FEngineNetworkCustomVersion::LatestVersion);
		bool bSaved = false;
		Source.NetSerialize(Writer, nullptr, bSaved);
		OutBitCount = Writer.GetNumBits();
		if (!bSaved || Writer.IsError())
		{
			return false;
		}
		FNetBitReader Reader(nullptr, Writer.GetData(), Writer.GetNumBits());
		Reader.SetEngineNetVer(FEngineNetworkCustomVersion::LatestVersion);
		bool bLoaded = false;
		Result.NetSerialize(Reader, nullptr, bLoaded);
		return bLoaded && !Reader.IsError() && Reader.GetPosBits() == Writer.GetNumBits();
	}

	FECPRagdollNetState MakePose(double Time, double RootX, uint32 Sequence = 7)
	{
		FECPRagdollNetState State;
		State.Phase = EECPMotionPhase::Ragdoll;
		State.Sequence = Sequence;
		State.ServerTime = Time;
		State.SkeletonSignature = 12345;
		State.LocalTransforms.Add(FTransform(FQuat::Identity, FVector(RootX, 0, 0)));
		State.LocalTransforms.Add(FTransform(FRotator(0, 30, 0), FVector(0, 0, 40)));
		return State;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRagdollSerializationTest,"ECP.Ragdoll.CompactPose.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPRagdollSerializationTest::RunTest(const FString& Parameters)
{
	FECPRagdollNetState Source = MakePose(2345.123456789, 120.123);
	Source.Frame = FTransform(FRotator(14.25, -179.7, 65.4), FVector(-30125.234, 500.671, 97.123), FVector(1.5, 2, .75));
	Source.LinearVelocity = FVector(712.456, -23.789, 100.123);
	Source.LocalTransforms[1] = FTransform(FRotator(13.123, 177.456, -83.789), FVector(2.348, -9.876, 40.123), FVector(-1.2, .75, 1.125));
	FECPRagdollNetState Result;
	int64 Bits = 0;
	if (!TestTrue(TEXT("含旋转和非均匀缩放的数据可以完整网络往返"), RoundTrip(Source, Result, Bits)))
	{
		return false;
	}
	TestTrue(TEXT("阶段原样保留"), Result.Phase == Source.Phase);
	TestEqual(TEXT("轮次原样保留"), Result.Sequence, Source.Sequence);
	TestEqual(TEXT("骨架签名原样保留"), Result.SkeletonSignature, Source.SkeletonSignature);
	TestEqual(TEXT("服务器时间不做有损量化"), Result.ServerTime, Source.ServerTime);
	TestTrue(TEXT("世界参考位置误差小于 0.01 cm"), Result.Frame.GetLocation().Equals(Source.Frame.GetLocation(), .01));
	TestTrue(TEXT("速度量化误差小于 0.01 cm/s"), Result.LinearVelocity.Equals(Source.LinearVelocity, .01));
	TestTrue(TEXT("参考系缩放没有被丢弃"), Result.Frame.GetScale3D().Equals(Source.Frame.GetScale3D(), 1.e-5));
	for (int32 Index = 0; Index < Source.LocalTransforms.Num(); ++Index)
	{
		TestTrue(TEXT("骨骼位移误差有界"), Result.LocalTransforms[Index].GetTranslation().Equals(Source.LocalTransforms[Index].GetTranslation(), .01));
		TestTrue(TEXT("骨骼旋转误差有界"), Result.LocalTransforms[Index].GetRotation().AngularDistance(Source.LocalTransforms[Index].GetRotation()) < .0003);
		TestTrue(TEXT("负缩放与非均匀缩放保留"), Result.LocalTransforms[Index].GetScale3D().Equals(Source.LocalTransforms[Index].GetScale3D(), 1.e-5));
	}

	// 一个真实 50 骨包应能放进约 1 KB 的应用数据，防止以后无意恢复逐骨 double/名字传输。
	Source.LocalTransforms.SetNum(50);
	for (int32 Index = 2; Index < 50; ++Index)
	{
		Source.LocalTransforms[Index] = FTransform(FRotator(10 + Index, 20 + Index, 30 + Index), FVector(1.23, 5.67, 20.12));
	}
	TestTrue(TEXT("50 骨姿态往返成功"), RoundTrip(Source, Result, Bits));
	TestTrue(TEXT("50 骨测试姿态小于 1 KB"), Bits < 8192);
	AddInfo(FString::Printf(TEXT("50 骨紧凑测试姿态：%lld bits（含状态与参考系，不含网络包头）"), Bits));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRagdollBoundsTest,"ECP.Ragdoll.CompactPose.Bounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPRagdollBoundsTest::RunTest(const FString& Parameters)
{
	FECPRagdollNetState Source = MakePose(1, 0);
	Source.LocalTransforms.Init(FTransform::Identity, ECPRagdoll::MaxPoseBones);
	FECPRagdollNetState Result;
	int64 Bits = 0;
	TestTrue(TEXT("恰好 256 根骨骼允许往返"), RoundTrip(Source, Result, Bits));
	TestEqual(TEXT("边界包没有截断骨骼"), Result.LocalTransforms.Num(), ECPRagdoll::MaxPoseBones);
	Source.LocalTransforms.Add(FTransform::Identity);
	TestFalse(TEXT("257 根骨骼拒绝发送，不能悄悄截断"), RoundTrip(Source, Result, Bits));

	// 实际截断网络包，证明接收失败后不会留下可用于渲染的半份姿态。
	Source = MakePose(1, 2);
	FNetBitWriter Writer(nullptr, 0);
	Writer.SetAllowResize(true);
	Writer.SetEngineNetVer(FEngineNetworkCustomVersion::LatestVersion);
	bool bSaved = false;
	Source.NetSerialize(Writer, nullptr, bSaved);
	if (!TestTrue(TEXT("截断测试先生成一个有效包"), bSaved && !Writer.IsError()))
	{
		return false;
	}
	FNetBitReader Reader(nullptr, Writer.GetData(), Writer.GetNumBits() - 8);
	Reader.SetEngineNetVer(FEngineNetworkCustomVersion::LatestVersion);
	bool bLoaded = true;
	{
		// 这是故意制造的坏包。FBitReader 没有 Writer 的 SetAllowOverflow 接口，
		// 用引擎提供的作用域仅捕获这条预期溢出 ensure，其他 ensure 仍按真实错误报告。
		FEnsureScope ExpectedOverflow([](const FEnsureHandlerArgs& Args)
		{
			return Args.Message && FCString::Strstr(Args.Message, TEXT("FBitReader::SetOverflowed() called!")) != nullptr;
		});
		Result.NetSerialize(Reader, nullptr, bLoaded);
	}
	TestTrue(TEXT("截断网络流保留错误标记"), Reader.IsError());
	TestFalse(TEXT("截断包返回失败"), bLoaded);
	TestTrue(TEXT("截断包清除半份姿态"), Result.LocalTransforms.IsEmpty());

	Source.Phase = EECPMotionPhase::Normal;
	Source.LocalTransforms.Reset();
	TestTrue(TEXT("正常阶段允许没有骨骼数据"), RoundTrip(Source, Result, Bits));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRagdollInterpolationTest,"ECP.Ragdoll.PoseSampling.ContinuousTimeline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPRagdollInterpolationTest::RunTest(const FString& Parameters)
{
	TArray<FECPRagdollNetState> Frames = {MakePose(1.0, 0), MakePose(1.1, 10)};
	Frames[0].LocalTransforms[1].SetRotation(FRotator(0, 170, 0).Quaternion());
	Frames[1].LocalTransforms[1].SetRotation(FRotator(0, -170, 0).Quaternion());
	Frames[0].LocalTransforms[1].SetScale3D(FVector(1, 2, 3));
	Frames[1].LocalTransforms[1].SetScale3D(FVector(3, 4, 5));
	TArray<FTransform> Pose;
	TestTrue(TEXT("时间中点能取到姿态"), ECPRagdoll::SamplePose(Frames, 1.05, Pose));
	TestTrue(TEXT("位移在线性中点"), FMath::IsNearlyEqual(Pose[0].GetTranslation().X, 5.0, 1.e-5));
	TestTrue(TEXT("旋转走跨越 180 度的短弧"), Pose[1].GetRotation().AngularDistance(FRotator(0, 180, 0).Quaternion()) < 1.e-5);
	TestTrue(TEXT("缩放连续混合"), Pose[1].GetScale3D().Equals(FVector(2, 3, 4), 1.e-5));

	// 后一包提前到达不能改变已经处于前两帧区间内的显示结果。
	Frames.Add(MakePose(1.2, 20));
	TestTrue(TEXT("补包后原时间仍可取样"), ECPRagdoll::SamplePose(Frames, 1.05, Pose));
	TestTrue(TEXT("新包不会重置显示起点"), FMath::IsNearlyEqual(Pose[0].GetTranslation().X, 5.0, 1.e-5));
	ECPRagdoll::SamplePose(Frames, 1.15, Pose);
	TestTrue(TEXT("跨包仍按同一速度前进"), FMath::IsNearlyEqual(Pose[0].GetTranslation().X, 15.0, 1.e-5));

	Frames.Add(MakePose(2.0, 100, 8));
	ECPRagdoll::SamplePose(Frames, 1.5, Pose);
	TestTrue(TEXT("新轮次不混入旧布娃娃姿态"), FMath::IsNearlyEqual(Pose[0].GetTranslation().X, 100.0, 1.e-5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRagdollExtrapolationTest,"ECP.Ragdoll.PoseSampling.BoundedExtrapolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPRagdollExtrapolationTest::RunTest(const FString& Parameters)
{
	FECPRagdollNetState State = MakePose(10.0, 2);
	State.Frame = FTransform(FRotator(0, 90, 0), FVector(100, 200, 300), FVector(2, 2, 2));
	State.LinearVelocity = FVector(100, 0, 0);
	TArray<FECPRagdollNetState> Frames = {State};
	TArray<FTransform> Pose;
	TestTrue(TEXT("短缺包可以外推"), ECPRagdoll::SamplePose(Frames, 10.02, Pose));
	FVector WorldDelta = State.Frame.TransformPosition(Pose[0].GetTranslation())
		- State.Frame.TransformPosition(State.LocalTransforms[0].GetTranslation());
	TestTrue(TEXT("世界速度正确换算到旋转缩放参考系"), WorldDelta.Equals(FVector(2, 0, 0), 1.e-5));
	TestTrue(TEXT("外推不改变四肢局部姿态"), Pose[1].Equals(State.LocalTransforms[1], 1.e-5));

	ECPRagdoll::SamplePose(Frames, 20.0, Pose);
	WorldDelta = State.Frame.TransformPosition(Pose[0].GetTranslation())
		- State.Frame.TransformPosition(State.LocalTransforms[0].GetTranslation());
	TestTrue(TEXT("长断网也只外推 40 ms"), WorldDelta.Equals(FVector(4, 0, 0), 1.e-5));
	Frames.Reset();
	TestFalse(TEXT("没有样本就明确失败"), ECPRagdoll::SamplePose(Frames, 1, Pose));
	TestTrue(TEXT("失败不能留下上次结果"), Pose.IsEmpty());
	return true;
}

#endif
