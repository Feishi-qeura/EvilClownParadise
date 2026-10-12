using UnrealBuildTool;

// 迁移工具只进入 Editor 目标，打包游戏不链接资产重命名或保存接口。
public class ECPFoundationEditor : ModuleRules
{
    public ECPFoundationEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UnrealEd" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetRegistry", "AssetTools", "BlueprintGraph", "GameFeatures", "Json", "Projects"
        });
    }
}
