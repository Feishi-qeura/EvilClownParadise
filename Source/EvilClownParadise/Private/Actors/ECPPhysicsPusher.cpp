#include "Actors/ECPPhysicsPusher.h"
#include "Characters/ECPRagdollTypes.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AECPPhysicsPusher::AECPPhysicsPusher()
{
	PrimaryActorTick.bCanEverTick = true;
	// Timeline 在 PrePhysics 移动 Cube；在 PostPhysics 记录与布娃娃一致的采样时刻。
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	bReplicates = true;
	SetReplicateMovement(false);
}

void AECPPhysicsPusher::BeginPlay()
{
	Super::BeginPlay();
	TInlineComponentArray<USceneComponent*> Components(this);
	for (USceneComponent* Component : Components)
	{
		if (Component->GetFName() == MovingComponentName) { MovingComponent = Component; break; }
	}
	if (!MovingComponent) { SetActorTickEnabled(false); return; }
	PreviousLocation = MovingComponent->GetComponentLocation();
	if (HasAuthority())
	{
		PusherFrame.Location = PreviousLocation;
		PusherFrame.ServerTime = ServerTime();
		ForceNetUpdate();
	}
}

double AECPPhysicsPusher::ServerTime() const
{
	const AGameStateBase* State = GetWorld()->GetGameState();
	return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

void AECPPhysicsPusher::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AECPPhysicsPusher,PusherFrame);
}

void AECPPhysicsPusher::OnRep_PusherFrame()
{
	// 新包追加到时间序列，绝不把已经播放的插值进度每次重置成零。
	if (Frames.IsEmpty() || PusherFrame.ServerTime > Frames.Last().ServerTime)
	{
		Frames.Add(PusherFrame);
		if (Frames.Num() > 8) Frames.RemoveAt(0,Frames.Num()-8,EAllowShrinking::No);
	}
}

void AECPPhysicsPusher::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!MovingComponent) return;
	if (HasAuthority())
	{
		const FVector Current = MovingComponent->GetComponentLocation();
		if (ServerTime() >= NextSampleTime)
		{
			const double Elapsed = ServerTime()-PusherFrame.ServerTime;
			PusherFrame.Velocity = Elapsed > UE_SMALL_NUMBER ? (Current-FVector(PusherFrame.Location))/Elapsed : FVector::ZeroVector;
			PusherFrame.Location = Current;
			PusherFrame.ServerTime = ServerTime();
			NextSampleTime = PusherFrame.ServerTime+ECPRagdoll::DefaultPoseInterval;
			ForceNetUpdate();
		}
		PreviousLocation = Current;
		return;
	}
	if (Frames.IsEmpty()) return;
	const double DisplayTime = ServerTime()-InterpolationDelay;
	FVector Location = Frames[0].Location;
	for (int32 Index=1; Index<Frames.Num(); ++Index)
	{
		const FECPPusherFrame& Before=Frames[Index-1];
		const FECPPusherFrame& After=Frames[Index];
		if (DisplayTime <= After.ServerTime)
		{
			const double Alpha = FMath::Clamp((DisplayTime-Before.ServerTime)/(After.ServerTime-Before.ServerTime),0.,1.);
			Location = FMath::Lerp(FVector(Before.Location),FVector(After.Location),Alpha);
			break;
		}
	}
	if (DisplayTime >= Frames.Last().ServerTime)
	{
		const FECPPusherFrame& Last=Frames.Last();
		// 和布娃娃使用相同的短暂外推上限，丢包时不会无限穿过场景。
		Location = FVector(Last.Location)+FVector(Last.Velocity)*FMath::Clamp(DisplayTime-Last.ServerTime,0.,double(ECPRagdoll::MaxExtrapolation));
	}
	MovingComponent->SetWorldLocation(Location,false,nullptr,ETeleportType::TeleportPhysics);
}

bool AECPPhysicsPusher::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
	// 此类没有玩家拥有权；保留显式 AlwaysRelevant，否则以实际 Cube 而非静止 SceneRoot 剔除。
	if (MovingComponent && !bOnlyRelevantToOwner && !bNetUseOwnerRelevancy)
	{
		return bAlwaysRelevant || FVector::DistSquared(MovingComponent->GetComponentLocation(), SrcLocation) < GetNetCullDistanceSquared();
	}
	return Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
}
