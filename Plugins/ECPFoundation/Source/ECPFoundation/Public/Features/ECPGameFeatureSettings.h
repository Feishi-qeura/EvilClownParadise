#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ECPGameFeatureSettings.generated.h"

/** 清单只保存插件身份与依赖，不硬引用功能中的类或资源。 */
USTRUCT(BlueprintType)
struct ECPFOUNDATION_API FECPGameFeatureDefinition
{
	GENERATED_BODY()

	/** 必须与 Plugins/GameFeatures 中的 .uplugin 文件名一致。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|Features")
	FName PluginName;

	/** 功能依赖必须先激活；公共普通 Runtime 插件不在这份列表内。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|Features")
	TArray<FName> Dependencies;

	/** 该功能在项目 /Game/Project 下的资源根；用于归属和 Cook 配置核对。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|Features")
	FString ProjectContentRoot;
};

/** 唯一功能清单在 DefaultGame.ini 配置；修改后重启，运行中的网络世界不热换协议。 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ECP Game Features"))
class ECPFOUNDATION_API UECPGameFeatureSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UPROPERTY(Config, EditAnywhere, Category = "Startup", meta = (ConfigRestartRequired = true))
	TArray<FECPGameFeatureDefinition> Features;
};

/** 纯清单校验与拓扑排序不触碰引擎状态，失败不返回部分可用的加载顺序。 */
namespace ECPFeatureManifest
{
	ECPFOUNDATION_API bool BuildActivationOrder(const TArray<FECPGameFeatureDefinition>& Definitions,
		TArray<FName>& OutOrder, FString& OutError);
}
