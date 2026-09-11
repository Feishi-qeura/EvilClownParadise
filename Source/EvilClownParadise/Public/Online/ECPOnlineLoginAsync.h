// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "ECPOnlineLoginAsync.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPOnLoginComplete, bool, bWasSuccessful, const FString&, ErrorMessage);

/**
 * Logs the local player into the active online subsystem (EOS) and reports the result.
 * The EOS subsystem has no local user until an explicit login completes, so session nodes fail with
 * "Cannot map local player to unique net ID" when called before this node succeeds.
 */
UCLASS()
class EVILCLOWNPARADISE_API UECPOnlineLoginAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FECPOnLoginComplete OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FECPOnLoginComplete OnFailure;

	/** Logs the local player into the active online subsystem. */
	UFUNCTION(BlueprintCallable, Category = "ECP|Online", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UECPOnlineLoginAsync* LoginOnlineSubsystem(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void HandleLoginComplete(int32 InLocalUserNum, bool bWasSuccessful, const FUniqueNetId& UserId, const FString& ErrorMessage);
	void Finish(bool bWasSuccessful, const FString& ErrorMessage);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	IOnlineIdentityPtr IdentityInterface;
	FDelegateHandle LoginCompleteDelegateHandle;
	int32 LocalUserNum = 0;
};
