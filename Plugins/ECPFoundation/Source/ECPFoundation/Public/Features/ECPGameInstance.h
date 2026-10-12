#pragma once

#include "Engine/GameInstance.h"
#include "ECPGameInstance.generated.h"

/** 只负责功能就绪门控；FU 的 GameInstanceSubsystem 仍由引擎自动创建和管理。 */
UCLASS()
class ECPFOUNDATION_API UECPGameInstance : public UGameInstance
{
	GENERATED_BODY()
public:
	virtual void StartGameInstance() override;
	virtual void Shutdown() override;
#if WITH_EDITOR
	virtual FGameInstancePIEResult InitializeForPlayInEditor(int32 PIEInstanceIndex,
		const FGameInstancePIEParameters& Params) override;
#endif
private:
	bool CheckRequiredFeatures(FString& OutReason) const;
	void UnbindFeatureStartup();
	/** 等待的注册完成后只启动一次；不对其他 GameInstance 发起加载或卸载。 */
	UFUNCTION() void HandleFeaturesReady();
	/** 启动失败撤销当前实例的监听并保留具体原因，不能进入缺少功能的地图。 */
	UFUNCTION() void HandleFeatureStartupFailed(FName PluginName, const FString& Reason);
	bool bStartRequested = false;
	bool bGameStarted = false;
};
