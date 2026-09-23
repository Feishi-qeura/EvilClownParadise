#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "ECPPhysicsPusher.generated.h"

class USceneComponent;

// 最新状态作为属性复制，静止后也保留；后加入客户端不必等待下一次运动。
USTRUCT()
struct FECPPusherFrame
{
	GENERATED_BODY()
	UPROPERTY() double ServerTime = 0;
	UPROPERTY() FVector_NetQuantize100 Location;
	UPROPERTY() FVector_NetQuantize10 Velocity;
};

// 保留 NewBlueprint1 的服务器 Timeline 路径，只替换客户端持续追赶目标的插值。
UCLASS()
class EVILCLOWNPARADISE_API AECPPhysicsPusher : public AActor
{
	GENERATED_BODY()
public:
	AECPPhysicsPusher();
	virtual void Tick(float DeltaSeconds) override;
	// Cube 可以远离原摆放点；相关性按真实推动面的位置判断。
	virtual bool IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const override;
	UPROPERTY(EditDefaultsOnly, Category="ECP|Network") FName MovingComponentName = TEXT("Cube");
	UPROPERTY(EditDefaultsOnly, Category="ECP|Network", meta=(ClampMin="0.02",ClampMax="0.15")) float InterpolationDelay = .04f;
protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
	UFUNCTION() void OnRep_PusherFrame();
	UPROPERTY(ReplicatedUsing=OnRep_PusherFrame) FECPPusherFrame PusherFrame;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> MovingComponent;
	TArray<FECPPusherFrame> Frames;
	FVector PreviousLocation = FVector::ZeroVector;
	double NextSampleTime = 0;
	double ServerTime() const;
};
