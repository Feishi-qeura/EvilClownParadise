// Fill out your copyright notice in the Description page of Project Settings.

#include "Online/ECPOnlineLoginAsync.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"

UECPOnlineLoginAsync* UECPOnlineLoginAsync::LoginOnlineSubsystem(UObject* WorldContextObject)
{
	UECPOnlineLoginAsync* LoginAction = NewObject<UECPOnlineLoginAsync>();
	LoginAction->WorldContextObject = WorldContextObject;
	LoginAction->RegisterWithGameInstance(WorldContextObject);
	return LoginAction;
}

void UECPOnlineLoginAsync::Activate()
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(World);
	IdentityInterface = OnlineSubsystem ? OnlineSubsystem->GetIdentityInterface() : nullptr;
	if (!IdentityInterface.IsValid())
	{
		Finish(false, TEXT("No online subsystem identity interface is available"));
		return;
	}

	// Match the session nodes, which resolve the user through the first local player controller.
	if (const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr)
	{
		if (const ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
		{
			LocalUserNum = LocalPlayer->GetControllerId();
		}
	}

	if (IdentityInterface->GetLoginStatus(LocalUserNum) == ELoginStatus::LoggedIn)
	{
		Finish(true, FString());
		return;
	}

	LoginCompleteDelegateHandle = IdentityInterface->AddOnLoginCompleteDelegate_Handle(
		LocalUserNum, FOnLoginCompleteDelegate::CreateUObject(this, &ThisClass::HandleLoginComplete));

	// AutoLogin follows the configured credential path, e.g. Steam session tickets via NativePlatformService=Steam.
	if (!IdentityInterface->AutoLogin(LocalUserNum))
	{
		IdentityInterface->ClearOnLoginCompleteDelegate_Handle(LocalUserNum, LoginCompleteDelegateHandle);
		LoginCompleteDelegateHandle.Reset();
		Finish(false, TEXT("AutoLogin request was rejected"));
	}
}

void UECPOnlineLoginAsync::HandleLoginComplete(int32 InLocalUserNum, bool bWasSuccessful, const FUniqueNetId& UserId, const FString& ErrorMessage)
{
	if (IdentityInterface.IsValid() && LoginCompleteDelegateHandle.IsValid())
	{
		IdentityInterface->ClearOnLoginCompleteDelegate_Handle(InLocalUserNum, LoginCompleteDelegateHandle);
	}
	LoginCompleteDelegateHandle.Reset();

	Finish(bWasSuccessful, ErrorMessage);
}

void UECPOnlineLoginAsync::Finish(bool bWasSuccessful, const FString& ErrorMessage)
{
	if (bWasSuccessful)
	{
		OnSuccess.Broadcast(true, ErrorMessage);
	}
	else
	{
		OnFailure.Broadcast(false, ErrorMessage);
	}

	SetReadyToDestroy();
}
