#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

// 仅为双人 PIE 的旋转专项复现建立持有态；绕过无关的瞄准布置，后续消息仍走真实客户端 RPC。
static FAutoConsoleCommand ECPSetupRemotePickupFixture(
    TEXT("ECP.Test.SetupRemotePickup"),
    TEXT("PIE only: give a world pickup to the remote player for replication diagnostics."),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        if (!GEngine) return;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            // 严格限定编辑器监听服务器，命令不能改动编辑关卡或打包游戏中的物品。
            if (!World || World->WorldType != EWorldType::PIE || World->GetNetMode() != NM_ListenServer) continue;
            for (TActorIterator<AECPPlayerBase> Player(World); Player; ++Player)
            {
                if (Player->IsLocallyControlled() || !Player->GetController()) continue;
                for (TActorIterator<AECPPickupItem> Item(World); Item; ++Item)
                {
                    if (Item->TryEnterPickup(*Player))
                    {
                        Player->CompleteQueuedPickup(*Item);
                        UE_LOG(LogTemp, Display, TEXT("ECP_PICKUP_FIXTURE_READY Player=%s Item=%s"),
                            *Player->GetPathName(), *Item->GetPathName());
                        return;
                    }
                }
            }
        }
        UE_LOG(LogTemp, Warning, TEXT("ECP_PICKUP_FIXTURE_NOT_READY: start two-player listen-server PIE with a free pickup."));
    }));

#endif
