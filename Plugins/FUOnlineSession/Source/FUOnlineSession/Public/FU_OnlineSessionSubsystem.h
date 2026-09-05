#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineSessionSubsystem.generated.h"

/** Manages one game-instance session through the currently configured OnlineSubsystem provider. */
UCLASS(BlueprintType)
class FUONLINESESSION_API UFU_OnlineSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	UPROPERTY(BlueprintAssignable, Category = "FU|Online Session")
	FFU_OnCreateSessionComplete OnCreateSessionComplete;

	UPROPERTY(BlueprintAssignable, Category = "FU|Online Session")
	FFU_OnFindSessionComplete OnFindSessionComplete;

	UPROPERTY(BlueprintAssignable, Category = "FU|Online Session")
	FFU_OnJoinSessionComplete OnJoinSessionComplete;

	UFUNCTION(BlueprintCallable, Category = "FU|Online Session")
	void FU_CreateCustomSession(int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword, bool bUseLobbiesIfAvailable = false);

	UFUNCTION(BlueprintCallable, Category = "FU|Online Session")
	void FU_FindCustomSession(const FString& RoomName, int32 MaxResults = 20, bool bUseLobbiesIfAvailable = false);

	UFUNCTION(BlueprintCallable, Category = "FU|Online Session")
	void FU_JoinCustomSession(const FString& RoomPasswordInput);

	UFUNCTION(BlueprintCallable, Category = "FU|Online Session")
	void FU_DestroySession();

private:
	enum class EFU_PendingOperation : uint8
	{
		None,
		Create,
		Join
	};

	// Session state is resolved per operation so PIE worlds and provider changes cannot retain a stale interface.
	IOnlineSessionPtr FU_GetSessionInterface();
	APlayerController* FU_GetLocalPlayerController() const;
	bool FU_DestroyExistingSessionForPendingOperation(EFU_PendingOperation PendingOperation);
	void FU_CreateSessionInternal();
	void FU_JoinSessionInternal();
	void FU_ClearDelegates();
	void FU_ClearCreateDelegate();
	void FU_ClearFindDelegate();
	void FU_ClearJoinDelegate();
	void FU_ClearDestroyDelegate();

	void FU_OnCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void FU_OnFindSessionsComplete(bool bWasSuccessful);
	void FU_OnJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void FU_OnDestroySessionComplete(FName SessionName, bool bWasSuccessful);

	IOnlineSessionPtr SessionInterface;
	TSharedPtr<FOnlineSessionSearch> SessionSearch;
	TArray<FOnlineSessionSearchResult> CachedSearchResults;
	TOptional<FOnlineSessionSearchResult> PendingJoinResult;

	FString PendingRoomName;
	FString PendingRoomPassword;
	int32 PendingMaxPlayers = 0;
	bool bPendingUseLobbiesIfAvailable = false;
	EFU_PendingOperation PendingOperation = EFU_PendingOperation::None;

	FDelegateHandle CreateSessionCompleteDelegateHandle;
	FDelegateHandle FindSessionsCompleteDelegateHandle;
	FDelegateHandle JoinSessionCompleteDelegateHandle;
	FDelegateHandle DestroySessionCompleteDelegateHandle;
};
