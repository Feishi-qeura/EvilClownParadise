#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPCharBase.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPJumpLandingMontageTest,
	"ECP.Animation.JumpLandingLeavesDefaultLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPJumpLandingMontageTest::RunTest(const FString& Parameters)
{
	// 无关卡移动数据库时 ABP_Player 的 PoseSearch 会记录一次预期错误，本测试只验证蒙太奇段切换。
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
		EAutomationExpectedErrorFlags::Contains, 1);
	// 使用项目真实角色蓝图、动画实例和蒙太奇，避免只验证一个无法捕获卡循环的字符串常量。
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPJumpLandingTestWorld"));
	if (!TestNotNull(TEXT("创建动画测试世界"), World))
	{
		return false;
	}

	UClass* PlayerClass = LoadClass<AECPCharBase>(
		nullptr, TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	AECPCharBase* Player = PlayerClass ? World->SpawnActor<AECPCharBase>(PlayerClass) : nullptr;
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (Player && Controller)
	{
		// 无视口的自动化世界不会自动把控制器标成本地玩家；显式标记后才能真实覆盖自主客户端落地预测分支。
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Player);
		Player->SetRole(ROLE_AutonomousProxy);
	}
	UAnimInstance* Anim = Player && Player->GetMesh() ? Player->GetMesh()->GetAnimInstance() : nullptr;
	UAnimMontage* Montage = Player ? Player->JumpMontage.Get() : nullptr;
	const bool bReady = TestNotNull(TEXT("加载 BP_ECPlayertest"), Player)
		&& TestNotNull(TEXT("创建 ABP_Player 动画实例"), Anim)
		&& TestNotNull(TEXT("加载 AMT_Jump"), Montage);

	if (bReady)
	{
		TestTrue(TEXT("测试角色走自主客户端落地分支"), Player->IsLocallyControlled() && !Player->HasAuthority());
		Anim->Montage_Play(Montage, 1.f);
		Anim->Montage_SetNextSection(TEXT("DefaultLoop"), TEXT("DefaultLoop"), Montage);
		Anim->Montage_JumpToSection(TEXT("DefaultLoop"), Montage);
		TestEqual(TEXT("测试前置状态位于 DefaultLoop"),
			Anim->Montage_GetCurrentSection(Montage), FName(TEXT("DefaultLoop")));

		// 记录空中实例身份，落地必须复用它，避免只查询最新登记实例而漏掉旧循环。
		const FAnimMontageInstance* AirInstance = Anim->GetActiveInstanceForMontage(Montage);
		const int32 AirInstanceID = AirInstance ? AirInstance->GetInstanceID() : INDEX_NONE;
		TestTrue(TEXT("落地前存在空中实例"), AirInstanceID != INDEX_NONE);
		Player->Landed(FHitResult());
		TestEqual(TEXT("落地必须立即退出 DefaultLoop 并进入 Landing"),
			Anim->Montage_GetCurrentSection(Montage), FName(TEXT("Landing")));
		// 除了当前帧跳段，还要覆盖空中阶段留下的 DefaultLoop 自循环，防止动画线程时序把它重新评估回循环段。
		const int32 DefaultLoopSection = Montage->GetSectionIndex(TEXT("DefaultLoop"));
		const int32 LandingSection = Montage->GetSectionIndex(TEXT("Landing"));
		TestEqual(TEXT("落地覆盖 DefaultLoop 自循环并将下一段指向 Landing"),
			Anim->Montage_GetNextSectionID(Montage, DefaultLoopSection), LandingSection);
		// 枚举全部同资源实例，检查 Landing 不能与遗留的空中自循环同时播放。
		int32 LandingInstanceCount = 0;
		for (const FAnimMontageInstance* Instance : Anim->MontageInstances)
		{
			// 跳过空项和其他动作的蒙太奇，避免干扰无关动画。
			if (!Instance || Instance->Montage != Montage) continue;
			const FName Section = Instance->GetCurrentSection();
			if (Instance->IsPlaying() && Section == FName(TEXT("Landing")))
			{
				++LandingInstanceCount;
				TestEqual(TEXT("Landing 复用已有空中实例"), Instance->GetInstanceID(), AirInstanceID);
			}
			TestFalse(FString::Printf(TEXT("实例 %d 不得残留 DefaultLoop"), Instance->GetInstanceID()),
				Section == FName(TEXT("DefaultLoop"))
					&& (Instance->IsPlaying() || Instance->GetWeight() > KINDA_SMALL_NUMBER));
		}
		TestEqual(TEXT("落地后只有一个正在播放的 Landing 实例"), LandingInstanceCount, 1);
	}

	World->DestroyWorld(false);
	return bReady;
}

#endif
