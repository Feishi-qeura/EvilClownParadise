#include "Features/ECPGameFeatureSettings.h"

bool ECPFeatureManifest::BuildActivationOrder(const TArray<FECPGameFeatureDefinition>& Definitions,
	TArray<FName>& OutOrder, FString& OutError)
{
	OutOrder.Reset();
	OutError.Reset();
	// 清单缺失属于装配失败，不能把零功能误报成全部已就绪。
	if (Definitions.IsEmpty())
	{
		OutError = TEXT("ECP feature manifest is empty. Configure ECPGameFeatureSettings in DefaultGame.ini.");
		return false;
	}
	TMap<FName, int32> Indices;
	for (int32 Index = 0; Index < Definitions.Num(); ++Index)
	{
		const FECPGameFeatureDefinition& Definition = Definitions[Index];
		if (Definition.PluginName.IsNone() || Indices.Contains(Definition.PluginName))
		{
			OutError = FString::Printf(TEXT("Unnamed or duplicate feature: %s"), *Definition.PluginName.ToString());
			return false;
		}
		// 明确限定项目资产归属，模板目录不会被误加入功能扫描/迁移范围。
		if (!Definition.ProjectContentRoot.StartsWith(TEXT("/Game/Project/"))
			|| Definition.ProjectContentRoot.Contains(TEXT(".."))
			|| Definition.ProjectContentRoot.Contains(TEXT("\\"))
			|| Definition.ProjectContentRoot.Len() <= FString(TEXT("/Game/Project/")).Len())
		{
			OutError = FString::Printf(TEXT("Invalid Project content root for %s: %s"),
				*Definition.PluginName.ToString(), *Definition.ProjectContentRoot);
			return false;
		}
		Indices.Add(Definition.PluginName, Index);
	}
	// 三色 DFS 区分正在访问与已完成，菱形依赖不重复注册，环立即失败。
	TArray<uint8> Marks;
	Marks.Init(0, Definitions.Num());
	TArray<FName> Sorted;
	TFunction<bool(int32)> Visit = [&](int32 Index)
	{
		if (Marks[Index] == 2) return true;
		const FECPGameFeatureDefinition& Definition = Definitions[Index];
		if (Marks[Index] == 1)
		{
			OutError = FString::Printf(TEXT("Feature dependency cycle at %s"), *Definition.PluginName.ToString());
			return false;
		}
		Marks[Index] = 1;
		for (FName Dependency : Definition.Dependencies)
		{
			const int32* DependencyIndex = Indices.Find(Dependency);
			if (!DependencyIndex)
			{
				OutError = FString::Printf(TEXT("Feature %s requires unregistered feature %s"),
					*Definition.PluginName.ToString(), *Dependency.ToString());
				return false;
			}
			if (!Visit(*DependencyIndex)) return false;
		}
		Marks[Index] = 2;
		Sorted.Add(Definition.PluginName);
		return true;
	};
	for (int32 Index = 0; Index < Definitions.Num(); ++Index)
	{
		if (!Visit(Index)) return false;
	}
	OutOrder = MoveTemp(Sorted);
	return true;
}
