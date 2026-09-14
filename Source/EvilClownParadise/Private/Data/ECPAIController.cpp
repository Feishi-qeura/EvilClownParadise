#include "Data/ECPAIController.h"

#include "Characters/ECPMonsterBase.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/StateTreeAIComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionTypes.h"
#include "Perception/AISenseConfig_Sight.h"


AECPAIController::AECPAIController()
{
	// 决策：StateTreeAI组件跑在AIC上，附身后自动开始
	StateTreeAIComp = CreateDefaultSubobject<UStateTreeAIComponent>(TEXT("StateTreeAIComp"));
	
	// 眼睛：视觉感知配置
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig -> SightRadius = 1000.f; // 视野范围10m
	SightConfig -> LoseSightRadius = 1200.f; // 视野丢失范围 > 视野范围，避免在边界抽风
	SightConfig -> PeripheralVisionAngleDegrees = 60.f; // 视野角度
	SightConfig -> SetMaxAge(3.f); // 丢失视野后会继续追击3秒
	SightConfig -> DetectionByAffiliation.bDetectEnemies = true;
	SightConfig -> DetectionByAffiliation.bDetectFriendlies = true;
	SightConfig -> DetectionByAffiliation.bDetectNeutrals = true;
	
	AIPerceptionComp = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerceptionComp"));
	AIPerceptionComp -> ConfigureSense(*SightConfig);
	AIPerceptionComp -> SetDominantSense(SightConfig -> GetSenseImplementation());
}

void AECPAIController::BeginPlay()
{
	Super::BeginPlay();
}

void AECPAIController::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (DefaultStateTree)
	{
		StateTreeAIComp -> SetStateTree(DefaultStateTree);
	}
	
	// 事件绑定
	AIPerceptionComp -> OnTargetPerceptionUpdated.AddDynamic(this, &AECPAIController::OnTargetPerceptionUpdated);
}

void AECPAIController::OnTargetPerceptionUpdated(AActor* Actor, struct FAIStimulus Stimulus)
{
	AECPMonsterBase* Monster = GetPawn<AECPMonsterBase>();
	AECPPlayerBase* Player = Cast<AECPPlayerBase>(Actor);
	
	if (Monster && Player && !Player -> IsDead())
	{
		Monster -> SetTarget(Player);
		GEngine -> AddOnScreenDebugMessage(-1, 2.0f, FColor::Cyan, FString::Printf(TEXT("%s看见了玩家：%s"), *Monster -> GetName(), *Player -> GetName()));
	}
}
