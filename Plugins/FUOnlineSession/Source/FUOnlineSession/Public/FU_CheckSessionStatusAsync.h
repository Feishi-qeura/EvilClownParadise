#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "FU_CheckSessionStatusAsync.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFU_OnCheckSessionStatusResult, float, PingValue);

/** Periodically reports the local client's ping while safely stopping when its World becomes invalid. */
UCLASS()
class FUONLINESESSION_API UFU_CheckSessionStatusAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "FU|Online Session")
	FFU_OnCheckSessionStatusResult OnServer;

	UPROPERTY(BlueprintAssignable, Category = "FU|Online Session")
	FFU_OnCheckSessionStatusResult OnClient;

	UPROPERTY(BlueprintAssignable, Category = "FU|Online Session")
	FFU_OnCheckSessionStatusResult ClientConnectionOvertime;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "FU|Online Session")
	static UFU_CheckSessionStatusAsync* FU_CheckSessionStatus(UObject* WorldContextObject, float RefreshTime);

	virtual void Activate() override;
	virtual void BeginDestroy() override;

private:
	// A weak World reference prevents the async node from extending a level's lifetime across travel.
	TWeakObjectPtr<UWorld> World;
	float RefreshInterval = 0.0f;
	FTimerHandle RefreshTimerHandle;

	void FU_UpdatePing();
	void FU_StopTimer();
	void FU_BroadcastTimeout();
};
