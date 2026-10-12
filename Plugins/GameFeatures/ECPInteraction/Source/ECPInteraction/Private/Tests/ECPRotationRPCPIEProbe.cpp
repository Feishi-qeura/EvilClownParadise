#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"

// RPC 必须由游戏世界计时器驱动；编辑器 Python 的脚本执行保护会使网络函数在本地执行。
struct FECPRotationPIEProbe
{
    FTimerHandle Timer;
    int32 Sent = 0;
    FRotator Start = FRotator::ZeroRotator;
    FECPInspectionRotationSample Sample;
    TWeakObjectPtr<AECPPickupItem> Item;
};

static FAutoConsoleCommand ECPProbeRemoteRotation(
    TEXT("ECP.Test.RotateRemotePickup"),
    TEXT("PIE only: stream 60 native owner rotation requests, then finalize."),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        if (!GEngine) return;
        AECPPlayerBase* Client = nullptr;
        AECPPlayerBase* Server = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World || World->WorldType != EWorldType::PIE) continue;
            for (TActorIterator<AECPPlayerBase> Player(World); Player; ++Player)
            {
                if (World->GetNetMode() == NM_Client && Player->IsLocallyControlled()) Client = *Player;
                if (World->GetNetMode() == NM_ListenServer && !Player->IsLocallyControlled()
                    && Player->GetController()) Server = *Player;
            }
        }
        if (!Client || !Server || !Client->GetInspectedPickup() || !Server->GetInspectedPickup())
        {
            UE_LOG(LogTemp, Warning, TEXT("ECP_ROTATION_PROBE_NOT_READY"));
            return;
        }
        const TSharedRef<FECPRotationPIEProbe> Probe = MakeShared<FECPRotationPIEProbe>();
        Probe->Item = Client->GetInspectedPickup();
        Probe->Start = Probe->Item->GetInspectionRotation();
        Probe->Sample.Generation = Probe->Item->GetInspectionGeneration();
        Probe->Sample.Session = Probe->Item->GetLastAcceptedInspectionSession() + 1;
        if (Probe->Sample.Session == 0) ++Probe->Sample.Session;
        const TWeakObjectPtr<AECPPlayerBase> ServerWeak(Server);
        Client->GetWorldTimerManager().SetTimer(Probe->Timer,
            FTimerDelegate::CreateWeakLambda(Client, [Client, ServerWeak, Probe]()
            {
                AECPPickupItem* LocalItem = Client->GetInspectedPickup();
                AECPPickupItem* ServerItem = ServerWeak.IsValid() ? ServerWeak->GetInspectedPickup() : nullptr;
                // 任一端退出持有或结束 PIE 后停止测试，不影响随后开启的游戏世界。
                if (!LocalItem || !ServerItem || Probe->Item.Get() != LocalItem
                    || LocalItem->GetInspectionGeneration() != Probe->Sample.Generation)
                {
                    Client->GetWorldTimerManager().ClearTimer(Probe->Timer);
                    return;
                }
                if (Probe->Sent < 60)
                {
                    ++Probe->Sent;
                    const FRotator Target = (Probe->Start + FRotator(0.f, Probe->Sent * 2.f, 0.f)).GetNormalized();
                    Probe->Sample.Sequence = static_cast<uint32>(Probe->Sent);
                    Probe->Sample.Rotation = Target;
                    LocalItem->PreviewInspectionRotation(Probe->Sample);
                    Client->RequestRotatePickup(LocalItem, Probe->Sample);
                    if (Probe->Sent % 10 == 0)
                    {
                        UE_LOG(LogTemp, Display, TEXT("ECP_ROTATION_PROBE_STREAM Sent=%d Server=%s Local=%s"),
                            Probe->Sent, *ServerItem->GetInspectionRotation().ToString(),
                            *LocalItem->GetRootComponent()->GetRelativeRotation().ToString());
                    }
                }
                else
                {
                    // 在日志记录松键前状态之后才发可靠尾包，严格区分连续同步与最终收敛。
                    UE_LOG(LogTemp, Display, TEXT("ECP_ROTATION_PROBE_BEFORE_FINAL Server=%s"),
                        *ServerItem->GetInspectionRotation().ToString());
                    ++Probe->Sample.Sequence;
                    Probe->Sample.bFinal = true;
                    Probe->Sample.Rotation = (Probe->Start + FRotator(0.f, 120.f, 0.f)).GetNormalized();
                    LocalItem->PreviewInspectionRotation(Probe->Sample);
                    Client->RequestFinalizePickupRotation(LocalItem, Probe->Sample);
                    Client->GetWorldTimerManager().ClearTimer(Probe->Timer);
                }
            }), 0.05f, true);
    }));

#endif
