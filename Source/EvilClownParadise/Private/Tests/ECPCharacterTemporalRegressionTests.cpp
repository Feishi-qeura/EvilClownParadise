#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPCharBase.h"
#include "Characters/ECPPlayerBase.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimTypes.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"

namespace ECPCharacterTemporalTests
{
constexpr float StepSeconds = 1.f / 60.f;

// 资源级查询只看最新登记实例；检查全部实例才能发现 Landing 结束后仍贡献姿势的旧空中循环。
bool TestNoResidualAirMontages(FAutomationTestBase& Test, UAnimInstance* Anim,
    UAnimMontage* Montage, const FString& Context)
{
    bool bPassed = true;
    for (const FAnimMontageInstance* Instance : Anim->MontageInstances)
    {
        // 仅检查当前跳跃资源，避免空项或其他动作影响验证结果。
        if (!Instance || Instance->Montage != Montage) continue;
        const FName Section = Instance->GetCurrentSection();
        const bool bAirSection = Section == FName(TEXT("Default")) || Section == FName(TEXT("DefaultLoop"));
        if (bAirSection && (Instance->IsPlaying() || Instance->GetWeight() > KINDA_SMALL_NUMBER))
        {
            Test.AddError(FString::Printf(
                TEXT("%s：遗留空中实例 ID=%d Section=%s Playing=%d Stopped=%d Weight=%.3f"),
                *Context, Instance->GetInstanceID(), *Section.ToString(), Instance->IsPlaying(),
                Instance->IsStopped(), Instance->GetWeight()));
            bPassed = false;
        }
    }
    return bPassed;
}

// 测试拥有独立的游戏世界和地板，真正推进 CMC、动画、Chaos 和定时器；不启动或接管编辑器 PIE。
class FCharacterWorld
{
public:
    ~FCharacterWorld()
    {
        if (World)
        {
            // 动画任务持有组件引用，销毁测试世界前先等待已提交的评估完成。
            if (Player && Player->GetMesh())
            {
                Player->GetMesh()->HandleExistingParallelEvaluationTask(true, true);
            }
            // 正式分发 EndPlay，先释放定时器和组件，再销毁物理测试世界。
            World->EndPlay(EEndPlayReason::Quit);
            World->DestroyWorld(false);
            if (GEngine) GEngine->DestroyWorldContext(World);
        }
    }

    bool Initialize(FAutomationTestBase& Test, const FName WorldName)
    {
        // 只允许这条已知的无关卡 PoseSearch 历史提示；其他动画、物理和蓝图错误照常使测试失败。
        Test.AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
            EAutomationExpectedErrorFlags::Contains, -1);
        UWorld::InitializationValues Values;
        Values.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(true)
            .EnableTraceCollision(true).SetTransactional(false).CreateFXSystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, WorldName, nullptr, true,
            ERHIFeatureLevel::Num, &Values);
        if (!Test.TestNotNull(TEXT("创建独立时序测试世界"), World)) return false;
        if (GEngine) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());

        // 盒体顶面位于 Z=0，同时参与角色扫掠和布娃娃物理碰撞，避免用手动 Landed 掩盖时序问题。
        AActor* Floor = World->SpawnActor<AActor>();
        if (!Test.TestNotNull(TEXT("创建测试地板"), Floor)) return false;
        UBoxComponent* FloorShape = NewObject<UBoxComponent>(Floor, TEXT("RegressionFloor"));
        Floor->AddInstanceComponent(FloorShape);
        Floor->SetRootComponent(FloorShape);
        FloorShape->SetBoxExtent(FVector(3000.f, 3000.f, 50.f));
        FloorShape->SetCollisionProfileName(TEXT("BlockAll"));
        FloorShape->SetCollisionObjectType(ECC_WorldStatic);
        FloorShape->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        FloorShape->RegisterComponent();
        Floor->SetActorLocation(FVector(0.f, 0.f, -50.f));

        UClass* PlayerClass = LoadClass<AECPCharBase>(nullptr,
            TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
        if (!Test.TestNotNull(TEXT("加载真实 BP_ECPlayertest"), PlayerClass)) return false;
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Player = World->SpawnActor<AECPCharBase>(PlayerClass, FVector(0.f, 0.f, 96.f),
            FRotator::ZeroRotator, Spawn);
        Controller = World->SpawnActor<APlayerController>();
        if (!Test.TestNotNull(TEXT("创建角色"), Player)
            || !Test.TestNotNull(TEXT("创建本地控制器"), Controller)) return false;
        // 测试没有 LocalPlayer 或视口；只关闭当前实例的调试 UI，不修改蓝图默认值。
        if (AECPPlayerBase* PlayerInstance = Cast<AECPPlayerBase>(Player))
        {
            PlayerInstance->NetworkDebugWidgetClass = nullptr;
        }
        Controller->SetAsLocalPlayerController();
        Controller->Possess(Player);

        // 没有渲染视口时仍刷新真实 ABP 的姿态，避免测试仅推进蒙太奇计数却没有更新物理骨骼。
        Player->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
        Player->GetMesh()->bEnableUpdateRateOptimizations = false;
        World->BeginPlay();
        // 此世界不启动项目 GameMode/在线会话；无 GameMode 的 BeginPlay 不会分发 Actor BeginPlay。
        World->GetWorldSettings()->NotifyBeginPlay();
        Anim = Player->GetMesh()->GetAnimInstance();
        Montage = Player->JumpMontage.Get();
        if (!Test.TestNotNull(TEXT("创建真实 ABP_Player 动画实例"), Anim)
            || !Test.TestNotNull(TEXT("加载 AMT_Jump"), Montage)) return false;
        if (!Test.TestTrue(TEXT("角色已执行 BeginPlay 并建立恢复缓存"), Player->HasActorBegunPlay())) return false;

        // 让起始胶囊落在地板上，并等待初始落地动画正常结束。
        for (int32 Frame = 0; Frame < 240; ++Frame)
        {
            Step();
            if (Frame > 30 && Player->GetCharacterMovement()->IsMovingOnGround()
                && !Player->IsMovementLocked()) return true;
        }
        Test.AddError(TEXT("测试前置条件失败：角色未能在地板站稳并结束初始 Landing"));
        return false;
    }

    void Step()
    {
        // 引擎正常每帧递增此计数；同步测试函数内必须模拟不同帧，否则定时器/动画会跳过后续 Tick。
        TGuardValue<uint64> FrameGuard(GFrameCounter, InitialFrame + ++SteppedFrames);
        World->Tick(LEVELTICK_All, StepSeconds);
        Player->GetMesh()->HandleExistingParallelEvaluationTask(true, true);
    }

    UWorld* World = nullptr;
    AECPCharBase* Player = nullptr;
    APlayerController* Controller = nullptr;
    UAnimInstance* Anim = nullptr;
    UAnimMontage* Montage = nullptr;

private:
    const uint64 InitialFrame = GFrameCounter;
    uint64 SteppedFrames = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPWorldTickLandingRegressionTest,
    "ECP.Animation.WorldTickLandingDoesNotReenterDefaultLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPWorldTickLandingRegressionTest::RunTest(const FString& Parameters)
{
    using namespace ECPCharacterTemporalTests;
    FCharacterWorld Fixture;
    if (!Fixture.Initialize(*this, TEXT("ECPWorldTickLandingRegression"))) return false;
    AECPCharBase* Player = Fixture.Player;
    UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
    const FVector StandingLocation = Player->GetActorLocation();

    // 从足够高处自然下落，确保真实空中更新多次进入 DefaultLoop，然后由碰撞触发 Landed。
    Player->SetActorLocation(StandingLocation + FVector(0.f, 0.f, 350.f), false, nullptr, ETeleportType::TeleportPhysics);
    Movement->SetMovementMode(MOVE_Falling);
    Movement->Velocity = FVector(0.f, 0.f, -50.f);
    bool bObservedLoop = false;
    bool bObservedLanding = false;
    for (int32 Frame = 0; Frame < 180; ++Frame)
    {
        Fixture.Step();
        const FName Section = Fixture.Anim->Montage_GetCurrentSection(Fixture.Montage);
        bObservedLoop |= Movement->IsFalling() && Section == FName(TEXT("DefaultLoop"));
        if (Movement->IsMovingOnGround())
        {
            bObservedLanding = Section == FName(TEXT("Landing"));
            // 落地当帧检查完整实例列表，不能用最新实例的 Landing 掩盖旧 DefaultLoop。
            if (!TestNoResidualAirMontages(*this, Fixture.Anim, Fixture.Montage, TEXT("自然落地当帧"))) return false;
            break;
        }
    }
    TestTrue(TEXT("真实空中帧曾播放 DefaultLoop"), bObservedLoop);
    if (!TestTrue(TEXT("真实碰撞落地后进入 Landing"), bObservedLanding)) return false;

    // 不再手动设置任何蒙太奇段：验证后续动画帧不会又读回空中自循环。
    const float LandingStartPosition = Fixture.Anim->Montage_GetPosition(Fixture.Montage);
    const FVector LandingLocation = Player->GetActorLocation();
    bool bPositionAdvanced = false;
    bool bFinished = false;
    for (int32 Frame = 0; Frame < 240; ++Frame)
    {
        // 重现玩家按住 W 和空格的情形，落地锁不能被持续输入穿透。
        Player->AddMovementInput(FVector::ForwardVector, 1.f);
        Player->Jump();
        Fixture.Step();
        // 每帧确认没有另一个空中实例继续播放或带权重停留，而非只查询资源登记的最新实例。
        if (!TestNoResidualAirMontages(*this, Fixture.Anim, Fixture.Montage,
            FString::Printf(TEXT("Landing 后第 %d 帧"), Frame))) return false;
        if (Fixture.Anim->Montage_IsPlaying(Fixture.Montage))
        {
            bPositionAdvanced |= Fixture.Anim->Montage_GetPosition(Fixture.Montage) > LandingStartPosition + .1f;
        }
        if (!Player->IsMovementLocked())
        {
            bFinished = true;
            break;
        }
        if (FVector::Dist2D(Player->GetActorLocation(), LandingLocation) > 1.f)
        {
            AddError(TEXT("Landing 结束前持续移动输入使角色发生水平位移"));
            return false;
        }
    }
    TestTrue(TEXT("Landing 的动画位置确实跨帧推进"), bPositionAdvanced);
    TestTrue(TEXT("Landing 完成后自然解锁，未卡住空中循环"), bFinished);
    if (!bPositionAdvanced || !bFinished) return false;

    // 释放持续输入后再观察 90 帧，确保 Landing 结束和角色解锁后仍无遗留空中循环。
    Player->StopJumping();
    Player->ConsumeMovementInputVector();
    Movement->StopMovementImmediately();
    for (int32 Frame = 0; Frame < 90; ++Frame)
    {
        Fixture.Step();
        if (!TestNoResidualAirMontages(*this, Fixture.Anim, Fixture.Montage,
            FString::Printf(TEXT("Landing 解锁后第 %d 帧"), Frame))) return false;
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRepeatedRagdollWorldTickRegressionTest,
    "ECP.Animation.RepeatedRagdollKeepsNewLocationAcrossPhysicsTicks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPRepeatedRagdollWorldTickRegressionTest::RunTest(const FString& Parameters)
{
    using namespace ECPCharacterTemporalTests;
    FCharacterWorld Fixture;
    if (!Fixture.Initialize(*this, TEXT("ECPRepeatedRagdollRegression"))) return false;
    AECPCharBase* Player = Fixture.Player;
    USkeletalMeshComponent* Mesh = Player->GetMesh();
    Player->StartRagdoll();
    if (!TestTrue(TEXT("第一次进入服务器布娃娃"), Player->IsRagdollActive())) return false;

    // 让 Chaos 真正解算倒地；通过正常入口尝试起身，不强改阶段、速度或私有恢复数据。
    bool bRecovered = false;
    for (int32 Frame = 0; Frame < 480; ++Frame)
    {
        Fixture.Step();
        if (Frame >= 60 && Frame % 12 == 0 && Player->TryRecoverFromRagdoll())
        {
            bRecovered = true;
            break;
        }
    }
    if (!TestTrue(TEXT("第一次倒地后可正常找到起身位置"), bRecovered)) return false;
    for (int32 Frame = 0; Frame < 240 && Player->IsMovementLocked(); ++Frame) Fixture.Step();
    if (!TestFalse(TEXT("完整起身 Landing 后已解锁"), Player->IsMovementLocked())) return false;
    if (!TestTrue(TEXT("起身后 Mesh 已重新附着胶囊"), Mesh->GetAttachParent() == Player->GetCapsuleComponent())) return false;

    const FVector FirstLocation = Mesh->GetSocketLocation(Player->RagdollPelvisBone);
    // 通过真实 CMC 移动离开第一次倒地处，不能只用 SetActorLocation 跳过正常的运动学骨骼更新。
    for (int32 Frame = 0; Frame < 150; ++Frame)
    {
        Player->AddMovementInput(FVector::ForwardVector, 1.f);
        Fixture.Step();
    }
    Player->GetCharacterMovement()->StopMovementImmediately();
    for (int32 Frame = 0; Frame < 12; ++Frame) Fixture.Step();
    const FVector SecondLocation = Mesh->GetSocketLocation(Player->RagdollPelvisBone);
    if (!TestTrue(TEXT("第二次倒地前已行走离开原位置至少 150cm"), FVector::Dist2D(FirstLocation, SecondLocation) > 150.f)) return false;

    Player->StartRagdoll();
    if (!TestTrue(TEXT("第二次进入服务器布娃娃"), Player->IsRagdollActive())) return false;
    if (!TestTrue(TEXT("第二次物理开启当帧骨盆保留新位置"),
        FVector::Dist2D(Mesh->GetSocketLocation(Player->RagdollPelvisBone), SecondLocation) < 75.f)) return false;
    for (int32 Frame = 0; Frame < 30; ++Frame)
    {
        Fixture.Step();
        const FVector Pelvis = Mesh->GetSocketLocation(Player->RagdollPelvisBone);
        if (FVector::Dist2D(Pelvis, SecondLocation) > 100.f)
        {
            AddError(FString::Printf(TEXT("第二次布娃娃第 %d 个物理帧离开新起点 %.1fcm，距旧起点 %.1fcm"),
                Frame, FVector::Dist2D(Pelvis, SecondLocation), FVector::Dist2D(Pelvis, FirstLocation)));
            return false;
        }
    }
    return TestTrue(TEXT("跨过多个 Chaos 更新后，骨盆未回跳到第一次倒地位置"),
        FVector::Dist2D(Mesh->GetSocketLocation(Player->RagdollPelvisBone), FirstLocation) > 100.f);
}

#endif
