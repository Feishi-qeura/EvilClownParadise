// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Characters/ECPCharacterMovementComponent.h"
#include "Characters/ECPCharBase.h"
#include "ECPPlayerBase.generated.h"

class UCameraComponent;
class USceneComponent;
/**
 *
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPPlayerBase : public AECPCharBase
{
	GENERATED_BODY()
public:
	AECPPlayerBase(const FObjectInitializer& ObjectInitializer);

	// 构造器创建，不开放逐实例编辑：置空会让相机构建与抖动注入直接崩在这里
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ECP|3C")
	TObjectPtr<USceneComponent> CameraRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ECP|3C")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(BlueprintReadOnly, Category = "ECP|3C|Input")
	FVector2D MoveInput = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "ECP|3C|Input")
	bool bWantsToCrouch = false;

	UPROPERTY(BlueprintReadOnly, Category = "ECP|3C|Input")
	bool bWantsToSprint = false;

	bool bJumpRequested = false;

	void RequestJump();

	void ReleaseJump();

	UFUNCTION(BlueprintPure, Category = "ECP|3C")
	UECPCharacterMovementComponent* GetECPMovement() const;

	/** 给本地相机的抖动层叠加一档（0~1）。受击 / 开火 / 爆炸时调用。
	 *  多次事件取最大值，不会叠加成白屏抖动。 */
	UFUNCTION(BlueprintCallable, Category = "ECP|3C|Camera")
	void AddCameraTrauma(float Amount);

protected:
	virtual void BeginPlay() override;
};
