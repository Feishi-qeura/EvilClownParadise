// Fill out your copyright notice in the Description page of Project Settings.

#include "Characters/ECPPlayerBase.h"

#include "Camera/CameraComponent.h"
#include "Camera/ECPCameraModifier.h"
#include "Camera/PlayerCameraManager.h"
#include "Characters/ECPCharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "ECPCore.h"
#include "GameFramework/PlayerController.h"

namespace
{
// CameraRoot 相对胶囊中心的高度（cm）。站姿半高 90 + 70 = 160，
// 和相机层的 StandEyeHeight 对齐（Godot 的 1.6 m）。
// 注意相机的最终 Z 是由 AECPCameraModifier::ApplyEyeHeight 强制成
// "胶囊底面 + SmoothedEyeHeight" 的，这里的值只决定相机组件自己的原始位置，
// 以及相机相对角色的水平挂点（蹲伏过渡的平滑全靠眼高插值，不靠这个偏移）。
constexpr float CameraRootBaseZ = 70.f;
}

AECPPlayerBase::AECPPlayerBase(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UECPCharacterMovementComponent>(
          ACharacter::CharacterMovementComponentName))
{
	// 角色本体目前没有逐帧逻辑，不需要 Tick。
	// 第 2 阶段的眼高平滑放进 CameraModifier（它本来每帧就跑），也不用在这里开。
	PrimaryActorTick.bCanEverTick = false;

	bUseControllerRotationYaw = true;

	// 感知阵营：怪物的视觉只勾"检测敌人"，这里不设玩家会被当成中立（见 ECPCore.h）
	TeamId = ECPTeam::Player;

	GetCharacterMovement()->bOrientRotationToMovement = false;
	// 蹲伏半高对齐 Godot：蹲伏胶囊总高 1.2 m → 半高 60
	GetCharacterMovement()->SetCrouchedHalfHeight(60.f);

	GetCapsuleComponent()->SetCapsuleHalfHeight(90.f);
	GetCapsuleComponent()->SetCapsuleRadius(35.f);

	CameraRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));
	CameraRoot->SetupAttachment(GetCapsuleComponent());
	CameraRoot->SetRelativeLocation(FVector(0.f, 0.f, CameraRootBaseZ));

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraRoot);
	FollowCamera->bUsePawnControlRotation = true;

	// FOV：Godot 的 75° 是【垂直】FOV，UE 的 FieldOfView 是【水平】。
	// 项目用的是引擎默认 AspectRatioAxisConstraint=MaintainYFOV（锁定垂直），
	// 所以填 107.5（= 2*atan(tan(37.5°)*16/9)）才等价于 75° 垂直。
	FollowCamera->FieldOfView = 107.5f;
}

void AECPPlayerBase::AddCameraTrauma(float Amount)
{
	// 只有本地玩家的相机会抖
	if (!IsLocallyControlled())
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}

	// 受击 / 开火时调用这里，把震动叠加到本地相机。
	// 目前还没有注入源，可以用控制台或蓝图手动调来验收。
	if (UECPCameraModifier* Modifier = Cast<UECPCameraModifier>(
	        PC->PlayerCameraManager->FindCameraModifierByClass(UECPCameraModifier::StaticClass())))
	{
		Modifier->AddTrauma(Amount);
	}
}

void AECPPlayerBase::RequestJump() { bJumpRequested = true; }

void AECPPlayerBase::ReleaseJump()
{
	if (UECPCharacterMovementComponent* Move = GetECPMovement())
	{
		Move->CutJump();
	}
}

UECPCharacterMovementComponent* AECPPlayerBase::GetECPMovement() const
{
	return Cast<UECPCharacterMovementComponent>(GetCharacterMovement());
}

void AECPPlayerBase::BeginPlay() { Super::BeginPlay(); }
