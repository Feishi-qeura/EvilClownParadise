#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPPlayerBase.h"
#include "Actors/ECPPickupItem.h"
#include "Data/ECPPlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "DrawDebugHelpers.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

namespace ECPNetworkDebugTests
{
struct FFixture
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    AECPPlayerBase* Player = nullptr;
    AECPPlayerController* Controller = nullptr;
    TSharedPtr<SWidget> DebugSlate;

    FFixture()
    {
        UGameInstance* Instance = NewObject<UGameInstance>();
        World->SetGameInstance(Instance);
        World->InitializeActorsForPlay(FURL());
        Controller = World->SpawnActor<AECPPlayerController>();
        Controller->SetAsLocalPlayerController();
        ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
        Controller->Player = LocalPlayer;
        LocalPlayer->PlayerController = Controller;
        Player = World->SpawnActor<AECPPlayerBase>();
        UCameraComponent* Camera = NewObject<UCameraComponent>(Player, TEXT("Camera"));
        Player->AddInstanceComponent(Camera);
        Camera->SetupAttachment(Player->GetRootComponent());
        Camera->RegisterComponent();
        USkeletalMeshComponent* Carry = NewObject<USkeletalMeshComponent>(Player, TEXT("SkeletalMesh"));
        Player->AddInstanceComponent(Carry);
        Carry->SetupAttachment(Camera);
        Carry->RegisterComponent();
        Player->NetworkDebugWidgetClass = LoadClass<UUserWidget>(nullptr,
            TEXT("/Game/Project/Online/UI/BP_NetworkDebug.BP_NetworkDebug_C"));
        Controller->Possess(Player);
        Player->DispatchBeginPlay();
        // 无窗口测试保留 Slate 根控件；真实游戏中由视口持有这一强引用。
        if (UUserWidget* Widget = GetDebugWidget()) DebugSlate = Widget->TakeWidget();
    }

    UUserWidget* GetDebugWidget() const
    {
        FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(
            AECPPlayerBase::StaticClass(), TEXT("NetworkDebugWidget"));
        return Property ? Cast<UUserWidget>(Property->GetObjectPropertyValue_InContainer(Player)) : nullptr;
    }

    ~FFixture() { World->DestroyWorld(false); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPNetworkDebugToggleTest,
    "ECP.Pickup.Debug.ToggleVisibilityAndCursor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPNetworkDebugToggleTest::RunTest(const FString& Parameters)
{
    ECPNetworkDebugTests::FFixture Fixture;
    UUserWidget* Widget = Fixture.GetDebugWidget();
    if (!TestNotNull(TEXT("本地拥有者创建调试控件"), Widget)) return false;
    TestEqual(TEXT("默认折叠，不占用布局或鼠标命中"), Widget->GetVisibility(), ESlateVisibility::Collapsed);
    TestFalse(TEXT("默认隐藏鼠标"), Fixture.Controller->bShowMouseCursor);
    UFunction* Toggle = Fixture.Controller->FindFunction(TEXT("ToggleNetworkDebug"));
    if (!TestNotNull(TEXT("存在 Tab 使用的切换入口"), Toggle)) return false;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Fixture.Controller->ProcessEvent(Toggle, nullptr);
        TestEqual(TEXT("打开面板"), Widget->GetVisibility(), ESlateVisibility::Visible);
        TestTrue(TEXT("打开时显示鼠标"), Fixture.Controller->bShowMouseCursor);
        Fixture.Controller->RefreshPickupInputMode();
        TestTrue(TEXT("拾取输入刷新不会抢走调试面板鼠标"), Fixture.Controller->bShowMouseCursor);
        Fixture.Controller->ProcessEvent(Toggle, nullptr);
        TestEqual(TEXT("再次切换折叠面板"), Widget->GetVisibility(), ESlateVisibility::Collapsed);
        TestFalse(TEXT("关闭时隐藏鼠标"), Fixture.Controller->bShowMouseCursor);
        TestTrue(TEXT("切换复用同一控件"), Fixture.GetDebugWidget() == Widget);
    }
    UClass* ItemClass = LoadClass<AECPPickupItem>(nullptr,
        TEXT("/Game/Project/Interaction/Items/BP_Smallitems.BP_Smallitems_C"));
    AECPPickupItem* Item = ItemClass ? Fixture.World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
    if (TestNotNull(TEXT("检查态测试物品"), Item))
    {
        Item->DispatchBeginPlay();
        TestTrue(TEXT("进入真实物品检查状态"), Item->TryEnterPickup(Fixture.Player));
        FObjectPropertyBase* Slot = FindFProperty<FObjectPropertyBase>(AECPPlayerBase::StaticClass(), TEXT("InspectedPickup"));
        if (!TestNotNull(TEXT("检查槽复制属性"), Slot)) return false;
        Slot->SetObjectPropertyValue_InContainer(Fixture.Player, Item);
        Fixture.Controller->RefreshPickupInputMode();
        TestTrue(TEXT("控制器进入检查输入模式"), Fixture.Controller->IsPickupInputModeActive());
        Fixture.Controller->ProcessEvent(Toggle, nullptr);
        Fixture.Controller->RefreshPickupInputMode();
        TestTrue(TEXT("检查期间 Tab 打开后鼠标仍显示"), Fixture.Controller->bShowMouseCursor);
        Fixture.Player->RequestStorePickup();
        Fixture.Controller->RefreshPickupInputMode();
        TestTrue(TEXT("面板打开时离开检查态仍保留鼠标"), Fixture.Controller->bShowMouseCursor);
        Fixture.Controller->ProcessEvent(Toggle, nullptr);
        TestFalse(TEXT("离开检查态后关闭面板仍隐藏鼠标"), Fixture.Controller->bShowMouseCursor);
    }
    Fixture.Controller->ProcessEvent(Toggle, nullptr);
    Fixture.Controller->UnPossess();
    TestFalse(TEXT("角色失去控制时立即隐藏鼠标"), Fixture.Controller->bShowMouseCursor);
    TestNull(TEXT("角色失去控制时释放调试控件"), Fixture.GetDebugWidget());
    Fixture.Controller->ProcessEvent(Toggle, nullptr);
    TestFalse(TEXT("无 Pawn 时 Tab 不会留下鼠标"), Fixture.Controller->bShowMouseCursor);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPNetworkDebugTraceRenderingTest,
    "ECP.Pickup.Debug.TraceRenderingSwitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPNetworkDebugTraceRenderingTest::RunTest(const FString& Parameters)
{
#if ENABLE_DRAW_DEBUG
    ECPNetworkDebugTests::FFixture Fixture;
    FlushPersistentDebugLines(Fixture.World);
    auto LineCount = [&Fixture]()
    {
        const ULineBatchComponent* Lines = Fixture.World->GetLineBatcher(UWorld::ELineBatcherType::World);
        const ULineBatchComponent* Persistent = Fixture.World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent);
        return (Lines ? Lines->BatchedLines.Num() : 0) + (Persistent ? Persistent->BatchedLines.Num() : 0);
    };
    Fixture.Player->RequestPickup();
    TestEqual(TEXT("默认不绘制拾取射线"), LineCount(), 0);
    Fixture.Player->bDrawPickupDebugTrace = true;
    Fixture.Player->RequestPickup();
    TestTrue(TEXT("开启后真实拾取路径绘制红线"), LineCount() > 0);
    FlushPersistentDebugLines(Fixture.World);
    if (ULineBatchComponent* Lines = Fixture.World->GetLineBatcher(UWorld::ELineBatcherType::World)) Lines->Flush();
    Fixture.Player->bDrawPickupDebugTrace = false;
    Fixture.Player->RequestPickup();
    TestEqual(TEXT("运行时关闭后停止新增射线"), LineCount(), 0);
    TestNull(TEXT("切换绘制不改变空场景拾取结果"), Fixture.Player->GetScannedPickup());
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPNetworkDebugTraceDefaultsTest,
    "ECP.Pickup.Debug.TraceRenderingDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPNetworkDebugTraceDefaultsTest::RunTest(const FString& Parameters)
{
    FBoolProperty* Property = FindFProperty<FBoolProperty>(AECPPlayerBase::StaticClass(), TEXT("bDrawPickupDebugTrace"));
    if (!TestNotNull(TEXT("角色暴露射线绘制布尔值"), Property)) return false;
    TestFalse(TEXT("原生角色默认关闭射线绘制"), Property->GetPropertyValue_InContainer(GetDefault<AECPPlayerBase>()));
    UClass* PlayerClass = LoadClass<AECPPlayerBase>(nullptr,
        TEXT("/Game/Project/Interaction/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
    if (TestNotNull(TEXT("加载实际角色蓝图"), PlayerClass))
    {
        TestFalse(TEXT("已有蓝图继承关闭默认值"), Property->GetPropertyValue_InContainer(PlayerClass->GetDefaultObject()));
    }
    return true;
}

#endif
