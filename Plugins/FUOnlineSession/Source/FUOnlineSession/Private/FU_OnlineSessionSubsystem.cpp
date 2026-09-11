#include "FU_OnlineSessionSubsystem.h"

#include "FU_OnlineSessionRequestValidation.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"

namespace FUOnlineSession
{
	const FName RoomNameSetting(TEXT("FU_RoomName"));
	const FName RoomPasswordSetting(TEXT("FU_RoomPassword"));
}

namespace
{
	// LAN broadcast only works for the Null provider; Steam sessions must use internet lobbies to be discoverable.
	bool FU_UsesLanSessions(const UWorld* World)
	{
		const IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(World);
		return !OnlineSubsystem || (OnlineSubsystem->GetSubsystemName() != STEAM_SUBSYSTEM);
	}
}

void UFU_OnlineSessionSubsystem::Deinitialize()
{
	// Shutdown can occur while provider callbacks are pending, so detach every delegate before releasing session state.
	if (SessionInterface.IsValid() && SessionSearch.IsValid())
	{
		SessionInterface->CancelFindSessions();
	}

	FU_ClearDelegates();
	SessionSearch.Reset();
	CachedSearchResults.Reset();
	PendingJoinResult.Reset();
	SessionInterface.Reset();
	PendingOperation = EFU_PendingOperation::None;

	Super::Deinitialize();
}

IOnlineSessionPtr UFU_OnlineSessionSubsystem::FU_GetSessionInterface()
{
	// Resolve from the active World at call time so PIE and future provider switches use the correct subsystem instance.
	if (IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld()))
	{
		return OnlineSubsystem->GetSessionInterface();
	}

	return nullptr;
}

APlayerController* UFU_OnlineSessionSubsystem::FU_GetLocalPlayerController() const
{
	if (const UWorld* World = GetWorld())
	{
		return World->GetFirstPlayerController();
	}

	return nullptr;
}

void UFU_OnlineSessionSubsystem::CreateCustomSession(const int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword, const bool bUseLobbiesIfAvailable)
{
	if (!FFU_SessionRequestValidation::CanCreate(MaxPlayers, RoomName))
	{
		OnCreateSessionComplete.Broadcast(false);
		return;
	}

	SessionInterface = FU_GetSessionInterface();
	if (!SessionInterface.IsValid())
	{
		OnCreateSessionComplete.Broadcast(false);
		return;
	}

	PendingMaxPlayers = MaxPlayers;
	PendingRoomName = RoomName;
	PendingRoomPassword = RoomPassword;
	bPendingUseLobbiesIfAvailable = bUseLobbiesIfAvailable;

	if (!FU_DestroyExistingSessionForPendingOperation(EFU_PendingOperation::Create))
	{
		FU_CreateSessionInternal();
	}
}

void UFU_OnlineSessionSubsystem::FindCustomSession(const FString& RoomName, const int32 MaxResults, const bool bUseLobbiesIfAvailable)
{
	SessionInterface = FU_GetSessionInterface();
	APlayerController* PlayerController = FU_GetLocalPlayerController();
	if (!SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer() || MaxResults <= 0)
	{
		OnFindSessionComplete.Broadcast({}, false);
		return;
	}

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = MaxResults;
	SessionSearch->bIsLanQuery = FU_UsesLanSessions(GetWorld());
	SessionSearch->QuerySettings.Set(SEARCH_LOBBIES, bUseLobbiesIfAvailable, EOnlineComparisonOp::Equals);
	PendingRoomName = RoomName;
	CachedSearchResults.Reset();

	FindSessionsCompleteDelegateHandle = SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnFindSessionsComplete));

	// A synchronous rejection does not trigger the completion delegate, so release it and report failure immediately.
	if (!SessionInterface->FindSessions(PlayerController->GetLocalPlayer()->GetControllerId(), SessionSearch.ToSharedRef()))
	{
		FU_ClearFindDelegate();
		OnFindSessionComplete.Broadcast({}, false);
	}
}

void UFU_OnlineSessionSubsystem::JoinCustomSession(const FString& RoomPasswordInput)
{
	SessionInterface = FU_GetSessionInterface();
	if (!SessionInterface.IsValid())
	{
		OnJoinSessionComplete.Broadcast(EFU_JoinSessionResult::UnknownError);
		return;
	}

	PendingJoinResult.Reset();
	for (const FOnlineSessionSearchResult& SearchResult : CachedSearchResults)
	{
		FString FoundPassword;
		SearchResult.Session.SessionSettings.Get(FUOnlineSession::RoomPasswordSetting, FoundPassword);
		if (FoundPassword == RoomPasswordInput)
		{
			PendingJoinResult = SearchResult;
			break;
		}
	}

	if (!PendingJoinResult.IsSet())
	{
		OnJoinSessionComplete.Broadcast(EFU_JoinSessionResult::SessionDoesNotExist);
		return;
	}

	if (!FU_DestroyExistingSessionForPendingOperation(EFU_PendingOperation::Join))
	{
		FU_JoinSessionInternal();
	}
}

void UFU_OnlineSessionSubsystem::DestroySession()
{
	SessionInterface = FU_GetSessionInterface();
	if (!SessionInterface.IsValid() || !SessionInterface->GetNamedSession(NAME_GameSession))
	{
		return;
	}

	PendingOperation = EFU_PendingOperation::None;
	DestroySessionCompleteDelegateHandle = SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnDestroySessionComplete));

	if (!SessionInterface->DestroySession(NAME_GameSession))
	{
		FU_ClearDestroyDelegate();
	}
}

bool UFU_OnlineSessionSubsystem::FU_DestroyExistingSessionForPendingOperation(const EFU_PendingOperation InPendingOperation)
{
	if (!SessionInterface.IsValid() || !SessionInterface->GetNamedSession(NAME_GameSession))
	{
		return false;
	}

	PendingOperation = InPendingOperation;
	DestroySessionCompleteDelegateHandle = SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnDestroySessionComplete));

	if (!SessionInterface->DestroySession(NAME_GameSession))
	{
		FU_ClearDestroyDelegate();
		const EFU_PendingOperation FailedOperation = PendingOperation;
		PendingOperation = EFU_PendingOperation::None;
		if (FailedOperation == EFU_PendingOperation::Create)
		{
			OnCreateSessionComplete.Broadcast(false);
		}
		else
		{
			OnJoinSessionComplete.Broadcast(EFU_JoinSessionResult::UnknownError);
		}
	}

	return true;
}

void UFU_OnlineSessionSubsystem::FU_CreateSessionInternal()
{
	APlayerController* PlayerController = FU_GetLocalPlayerController();
	if (!SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer())
	{
		OnCreateSessionComplete.Broadcast(false);
		return;
	}

	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections = PendingMaxPlayers;
	Settings.bIsLANMatch = FU_UsesLanSessions(GetWorld());
	Settings.bShouldAdvertise = true;
	Settings.bUsesPresence = true;
	Settings.bUseLobbiesIfAvailable = bPendingUseLobbiesIfAvailable;
	Settings.Set(FUOnlineSession::RoomNameSetting, PendingRoomName, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(FUOnlineSession::RoomPasswordSetting, PendingRoomPassword, EOnlineDataAdvertisementType::ViaOnlineService);

	CreateSessionCompleteDelegateHandle = SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnCreateSessionComplete));

	if (!SessionInterface->CreateSession(PlayerController->GetLocalPlayer()->GetControllerId(), NAME_GameSession, Settings))
	{
		FU_ClearCreateDelegate();
		OnCreateSessionComplete.Broadcast(false);
	}
}

void UFU_OnlineSessionSubsystem::FU_JoinSessionInternal()
{
	APlayerController* PlayerController = FU_GetLocalPlayerController();
	if (!SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer() || !PendingJoinResult.IsSet())
	{
		OnJoinSessionComplete.Broadcast(EFU_JoinSessionResult::UnknownError);
		return;
	}

	JoinSessionCompleteDelegateHandle = SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnJoinSessionComplete));

	if (!SessionInterface->JoinSession(PlayerController->GetLocalPlayer()->GetControllerId(), NAME_GameSession, PendingJoinResult.GetValue()))
	{
		FU_ClearJoinDelegate();
		OnJoinSessionComplete.Broadcast(EFU_JoinSessionResult::UnknownError);
	}
}

void UFU_OnlineSessionSubsystem::FU_OnCreateSessionComplete(FName SessionName, const bool bWasSuccessful)
{
	FU_ClearCreateDelegate();
	OnCreateSessionComplete.Broadcast(bWasSuccessful);
}

void UFU_OnlineSessionSubsystem::FU_OnFindSessionsComplete(const bool bWasSuccessful)
{
	FU_ClearFindDelegate();
	TArray<FFU_SessionResult> BlueprintResults;
	CachedSearchResults.Reset();
	

	if (bWasSuccessful && SessionSearch.IsValid())
	{
		for (const FOnlineSessionSearchResult& SearchResult : SessionSearch->SearchResults)
		{
			FString FoundRoomName;
			SearchResult.Session.SessionSettings.Get(FUOnlineSession::RoomNameSetting, FoundRoomName);
			if (FoundRoomName != PendingRoomName)
			{
				continue;
			}

			FFU_SessionResult& BlueprintResult = BlueprintResults.AddDefaulted_GetRef();
			BlueprintResult.SessionId = SearchResult.GetSessionIdStr();
			BlueprintResult.RoomName = FoundRoomName;
			BlueprintResult.MaxPlayers = SearchResult.Session.SessionSettings.NumPublicConnections;
			BlueprintResult.CurrentPlayers = BlueprintResult.MaxPlayers - SearchResult.Session.NumOpenPublicConnections;
			BlueprintResult.PingInMs = SearchResult.PingInMs;
			CachedSearchResults.Add(SearchResult);
		}
	}

	OnFindSessionComplete.Broadcast(BlueprintResults, bWasSuccessful && !BlueprintResults.IsEmpty());
}

void UFU_OnlineSessionSubsystem::FU_OnJoinSessionComplete(FName SessionName, const EOnJoinSessionCompleteResult::Type Result)
{
	FU_ClearJoinDelegate();
	EFU_JoinSessionResult BlueprintResult = EFU_JoinSessionResult::UnknownError;

	switch (Result)
	{
	case EOnJoinSessionCompleteResult::Success:
		BlueprintResult = EFU_JoinSessionResult::Success;
		if (SessionInterface.IsValid())
		{
			FString ConnectString;
			if (SessionInterface->GetResolvedConnectString(SessionName, ConnectString))
			{
				if (APlayerController* PlayerController = FU_GetLocalPlayerController())
				{
					// Travel is valid only after the provider supplied a concrete host address for the joined session.
					PlayerController->ClientTravel(ConnectString, ETravelType::TRAVEL_Absolute);
				}
			}
			else
			{
				BlueprintResult = EFU_JoinSessionResult::CouldNotRetrieveAddress;
			}
		}
		break;
	case EOnJoinSessionCompleteResult::SessionIsFull:
		BlueprintResult = EFU_JoinSessionResult::SessionIsFull;
		break;
	case EOnJoinSessionCompleteResult::SessionDoesNotExist:
		BlueprintResult = EFU_JoinSessionResult::SessionDoesNotExist;
		break;
	case EOnJoinSessionCompleteResult::CouldNotRetrieveAddress:
		BlueprintResult = EFU_JoinSessionResult::CouldNotRetrieveAddress;
		break;
	case EOnJoinSessionCompleteResult::AlreadyInSession:
		BlueprintResult = EFU_JoinSessionResult::AlreadyInSession;
		break;
	default:
		break;
	}

	PendingJoinResult.Reset();
	OnJoinSessionComplete.Broadcast(BlueprintResult);
}

void UFU_OnlineSessionSubsystem::FU_OnDestroySessionComplete(FName SessionName, const bool bWasSuccessful)
{
	FU_ClearDestroyDelegate();
	const EFU_PendingOperation CompletedOperation = PendingOperation;
	PendingOperation = EFU_PendingOperation::None;

	if (!bWasSuccessful)
	{
		if (CompletedOperation == EFU_PendingOperation::Create)
		{
			OnCreateSessionComplete.Broadcast(false);
		}
		else if (CompletedOperation == EFU_PendingOperation::Join)
		{
			OnJoinSessionComplete.Broadcast(EFU_JoinSessionResult::UnknownError);
		}
		return;
	}

	if (CompletedOperation == EFU_PendingOperation::Create)
	{
		FU_CreateSessionInternal();
	}
	else if (CompletedOperation == EFU_PendingOperation::Join)
	{
		FU_JoinSessionInternal();
	}
}

void UFU_OnlineSessionSubsystem::FU_ClearDelegates()
{
	FU_ClearCreateDelegate();
	FU_ClearFindDelegate();
	FU_ClearJoinDelegate();
	FU_ClearDestroyDelegate();
}

void UFU_OnlineSessionSubsystem::FU_ClearCreateDelegate()
{
	if (SessionInterface.IsValid() && CreateSessionCompleteDelegateHandle.IsValid())
	{
		SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteDelegateHandle);
	}
	CreateSessionCompleteDelegateHandle.Reset();
}

void UFU_OnlineSessionSubsystem::FU_ClearFindDelegate()
{
	if (SessionInterface.IsValid() && FindSessionsCompleteDelegateHandle.IsValid())
	{
		SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteDelegateHandle);
	}
	FindSessionsCompleteDelegateHandle.Reset();
}

void UFU_OnlineSessionSubsystem::FU_ClearJoinDelegate()
{
	if (SessionInterface.IsValid() && JoinSessionCompleteDelegateHandle.IsValid())
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteDelegateHandle);
	}
	JoinSessionCompleteDelegateHandle.Reset();
}

void UFU_OnlineSessionSubsystem::FU_ClearDestroyDelegate()
{
	if (SessionInterface.IsValid() && DestroySessionCompleteDelegateHandle.IsValid())
	{
		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteDelegateHandle);
	}
	DestroySessionCompleteDelegateHandle.Reset();
}
