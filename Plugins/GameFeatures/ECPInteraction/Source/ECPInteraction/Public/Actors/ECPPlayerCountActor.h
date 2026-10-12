#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ECPPlayerCountActor.generated.h"

/**
 * 关卡内玩家人数查询与同步跳转基类，供 BP_Actor 的 BOX 到齐逻辑调用。
 * 不保存人数快照；每次调用均读取当前服务器世界。
 */
UCLASS(Blueprintable)
class ECPINTERACTION_API AECPPlayerCountActor : public AActor
{
	GENERATED_BODY()

public:
	/**
	 * @brief 获取当前世界中已控制非旁观角色的玩家数量。
	 * @return 服务器上的人数；客户端或无有效世界时返回 0。
	 * @note 只遍历 PlayerController，AI 不计入；适合在服务器蓝图逻辑中调用。
	 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Interaction|Level", meta=(DisplayName="获取当前场景玩家人数"))
	int32 GetCurrentWorldPlayerCount();

	/** 每次在服务器实时检查本 Actor 的 Box 内是否包含所有已控制角色的玩家。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Interaction|Level", meta=(DisplayName="所有玩家是否在 Box 内"))
	bool AreAllCurrentPlayersInsideBox();

	/**
	 * @brief 在服务器切换关卡，让所有已连接客户端跟随。
	 * @param DestinationLevel 目标地图资产；在蓝图节点中选择 Demo 等地图。
	 * @return 是否接受本次跳转请求；客户端、无效地图或正在跳转时返回 false。
	 * @note 在服务器的“所有玩家是否在 Box 内”条件为真后调用一次。
	 *       返回 true 表示请求已接受，关卡加载完成由引擎异步处理。
	 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Interaction|Level", meta=(DisplayName="同步切换关卡（服务器）"))
	bool TravelAllPlayersToLevel(TSoftObjectPtr<UWorld> DestinationLevel);
};
