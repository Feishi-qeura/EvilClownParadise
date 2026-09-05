#pragma once

#include "CoreMinimal.h"
#include "FU_OnlineSessionTypes.generated.h"

/** Blueprint-safe result for a completed join request. */
UENUM(BlueprintType)
enum class EFU_JoinSessionResult : uint8
{
	Success,
	SessionIsFull,
	SessionDoesNotExist,
	CouldNotRetrieveAddress,
	AlreadyInSession,
	UnknownError
};

/** Blueprint-safe snapshot of a discovered online session. */
USTRUCT(BlueprintType)
struct FUONLINESESSION_API FFU_SessionResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "FU|Online Session")
	FString SessionId;

	UPROPERTY(BlueprintReadWrite, Category = "FU|Online Session")
	FString RoomName;

	UPROPERTY(BlueprintReadWrite, Category = "FU|Online Session")
	int32 MaxPlayers = 0;

	UPROPERTY(BlueprintReadWrite, Category = "FU|Online Session")
	int32 CurrentPlayers = 0;

	UPROPERTY(BlueprintReadWrite, Category = "FU|Online Session")
	int32 PingInMs = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFU_OnCreateSessionComplete, bool, bWasSuccessful);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFU_OnFindSessionComplete, const TArray<FFU_SessionResult>&, Results, bool, bWasSuccessful);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFU_OnJoinSessionComplete, EFU_JoinSessionResult, Result);
