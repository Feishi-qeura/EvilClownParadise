#include "Characters/ECPRagdollTypes.h"

#include "Engine/NetSerialization.h"

namespace
{
	/** 先拒绝非有限数和坏旋转，避免损坏的一个骨骼把整个角色包围盒、插值结果污染。 */
	bool IsUsableTransform(const FTransform& Transform)
	{
		return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized();
	}

	/** 位移量化到 0.01 cm、旋转到 16 位；非单位缩放另外发送，不能丢掉缩放角色的数据。 */
	bool SerializeTransform(FArchive& Ar, FTransform& Transform)
	{
		FVector Translation = Ar.IsSaving() ? Transform.GetTranslation() : FVector::ZeroVector;
		bool bSuccess = SerializePackedVector<100, 30>(Translation, Ar);
		// 截断后立即停止，不再读后续字段或把不完整数据构造成可渲染的 Transform。
		if (!bSuccess || Ar.IsError()) return false;
		FRotator Rotation = Ar.IsSaving() ? Transform.Rotator() : FRotator::ZeroRotator;
		Rotation.SerializeCompressedShort(Ar);
		if (Ar.IsError()) return false;

		FVector Scale = Ar.IsSaving() ? Transform.GetScale3D() : FVector::OneVector;
		uint8 bHasScale = Ar.IsSaving() && !Scale.Equals(FVector::OneVector, 1.e-6) ? 1 : 0;
		Ar.SerializeBits(&bHasScale, 1);
		if (Ar.IsError()) return false;
		if (bHasScale != 0)
		{
			// 缩放一般很小，三个 float 既保留负缩放/非均匀缩放，也避免每骨骼总是传三个 double。
			float ScaleX = static_cast<float>(Scale.X);
			float ScaleY = static_cast<float>(Scale.Y);
			float ScaleZ = static_cast<float>(Scale.Z);
			Ar << ScaleX << ScaleY << ScaleZ;
			if (Ar.IsError()) return false;
			Scale = FVector(ScaleX, ScaleY, ScaleZ);
		}

		if (Translation.ContainsNaN() || Rotation.ContainsNaN() || Scale.ContainsNaN()) return false;
		if (Ar.IsLoading())
		{
			Transform = FTransform(Rotation.Quaternion(), Translation, Scale);
		}
		bSuccess &= !Ar.IsError() && !Scale.ContainsNaN() && IsUsableTransform(Transform);
		return bSuccess;
	}

	/** 只有完整的布娃娃样本才参与插值；过期阶段、空姿态和截断数据不作为新的起点。 */
	bool IsUsablePose(const FECPRagdollNetState& State)
	{
		if (State.Phase != EECPMotionPhase::Ragdoll || !FMath::IsFinite(State.ServerTime)
			|| State.LocalTransforms.IsEmpty() || State.LocalTransforms.Num() > ECPRagdoll::MaxPoseBones
			|| State.LinearVelocity.ContainsNaN() || !IsUsableTransform(State.Frame))
		{
			return false;
		}
		for (const FTransform& Transform : State.LocalTransforms)
		{
			if (!IsUsableTransform(Transform))
			{
				return false;
			}
		}
		return true;
	}
}

bool FECPRagdollNetState::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	(void)Map;
	bOutSuccess = false;
	if (Ar.IsError())
	{
		if (Ar.IsLoading()) LocalTransforms.Reset();
		return true;
	}

	// 发送前就拒绝坏数据，不能截掉超限骨骼后继续发送一个看似有效、骨架却不完整的姿态。
	if (Ar.IsSaving())
	{
		bool bValid = static_cast<uint8>(Phase) <= static_cast<uint8>(EECPMotionPhase::JumpLanding)
			&& FMath::IsFinite(ServerTime) && IsUsableTransform(Frame) && !LinearVelocity.ContainsNaN()
			&& LocalTransforms.Num() <= ECPRagdoll::MaxPoseBones;
		for (const FTransform& Transform : LocalTransforms)
		{
			bValid &= IsUsableTransform(Transform);
		}
		if (!bValid)
		{
			Ar.SetError();
			return true;
		}
	}

	// 四种阶段只占两位；阶段、轮次、参考系与姿态在一次 RepNotify 内一起生效。
	uint8 PhaseBits = static_cast<uint8>(Phase);
	Ar.SerializeBits(&PhaseBits, 2);
	Ar.SerializeIntPacked(Sequence);
	Ar << ServerTime;
	if (Ar.IsError() || !FMath::IsFinite(ServerTime))
	{
		if (Ar.IsLoading()) LocalTransforms.Reset();
		Ar.SetError();
		return true;
	}
	bool bValid = SerializeTransform(Ar, Frame);
	if (!bValid)
	{
		if (Ar.IsLoading()) LocalTransforms.Reset();
		Ar.SetError();
		return true;
	}
	bValid &= SerializePackedVector<100, 30>(LinearVelocity, Ar);
	Ar << SkeletonSignature;

	uint32 BoneCount = static_cast<uint32>(LocalTransforms.Num());
	Ar.SerializeIntPacked(BoneCount);
	if (Ar.IsError() || BoneCount > static_cast<uint32>(ECPRagdoll::MaxPoseBones)
		|| !FMath::IsFinite(ServerTime) || LinearVelocity.ContainsNaN() || !bValid)
	{
		// 接收端先验证数量才分配，损坏包不会保留一半旧姿态、一半新姿态。
		if (Ar.IsLoading())
		{
			LocalTransforms.Reset();
		}
		Ar.SetError();
		return true;
	}

	if (Ar.IsLoading())
	{
		Phase = static_cast<EECPMotionPhase>(PhaseBits);
		LocalTransforms.SetNum(static_cast<int32>(BoneCount));
	}
	for (FTransform& Transform : LocalTransforms)
	{
		if (!SerializeTransform(Ar, Transform))
		{
			if (Ar.IsLoading())
			{
				LocalTransforms.Reset();
			}
			Ar.SetError();
			return true;
		}
	}

	bOutSuccess = !Ar.IsError();
	// 没有需要 PackageMap 解析的对象引用；true 表示映射完成，实际编解码结果由 bOutSuccess 返回。
	return true;
}

bool ECPRagdoll::SamplePose(const TArray<FECPRagdollNetState>& Frames,
	double DisplayTime, TArray<FTransform>& OutLocalTransforms)
{
	OutLocalTransforms.Reset();
	if (!FMath::IsFinite(DisplayTime))
	{
		return false;
	}

	// 不按最大的 Sequence 选轮次，因为 uint32 会回绕；调用端的收到顺序才是当前轮次依据。
	const FECPRagdollNetState* Current = nullptr;
	for (int32 Index = Frames.Num() - 1; Index >= 0; --Index)
	{
		if (IsUsablePose(Frames[Index]))
		{
			Current = &Frames[Index];
			break;
		}
	}
	if (!Current)
	{
		return false;
	}

	const FECPRagdollNetState* Before = nullptr;
	const FECPRagdollNetState* After = nullptr;
	for (const FECPRagdollNetState& Candidate : Frames)
	{
		// 参考系在一轮内应固定；不把不同骨架或不同坐标系中的局部变换混在一起。
		if (Candidate.Sequence != Current->Sequence || Candidate.SkeletonSignature != Current->SkeletonSignature
			|| Candidate.LocalTransforms.Num() != Current->LocalTransforms.Num()
			|| !Candidate.Frame.Equals(Current->Frame, 1.e-4) || !IsUsablePose(Candidate))
		{
			continue;
		}
		if (Candidate.ServerTime <= DisplayTime && (!Before || Candidate.ServerTime > Before->ServerTime))
		{
			Before = &Candidate;
		}
		if (Candidate.ServerTime >= DisplayTime && (!After || Candidate.ServerTime < After->ServerTime))
		{
			After = &Candidate;
		}
	}

	if (Before && After && After->ServerTime > Before->ServerTime)
	{
		// 时间轴连续推进，后续包只是补齐取样区间，不会反复把插值 Alpha 重设为零。
		const float Alpha = static_cast<float>(FMath::Clamp(
			(DisplayTime - Before->ServerTime) / (After->ServerTime - Before->ServerTime), 0.0, 1.0));
		OutLocalTransforms.SetNum(Before->LocalTransforms.Num());
		for (int32 BoneIndex = 0; BoneIndex < OutLocalTransforms.Num(); ++BoneIndex)
		{
			const FTransform& A = Before->LocalTransforms[BoneIndex];
			const FTransform& B = After->LocalTransforms[BoneIndex];
			OutLocalTransforms[BoneIndex] = FTransform(
				FQuat::Slerp(A.GetRotation(), B.GetRotation(), Alpha).GetNormalized(),
				FMath::Lerp(A.GetTranslation(), B.GetTranslation(), Alpha),
				FMath::Lerp(A.GetScale3D(), B.GetScale3D(), Alpha));
		}
		return true;
	}

	const FECPRagdollNetState* Endpoint = Before ? Before : After;
	if (!Endpoint)
	{
		return false;
	}
	OutLocalTransforms = Endpoint->LocalTransforms;
	if (!After && DisplayTime > Endpoint->ServerTime)
	{
		// 只短暂预测整体平移；四肢保持服务器姿态，碰撞后也不会产生第二套分歧的物理解算。
		const double Ahead = FMath::Clamp(DisplayTime - Endpoint->ServerTime, 0.0,
			static_cast<double>(MaxExtrapolation));
		const FVector LocalOffset = Endpoint->Frame.InverseTransformVector(Endpoint->LinearVelocity * Ahead);
		OutLocalTransforms[0].AddToTranslation(LocalOffset);
	}
	return true;
}
