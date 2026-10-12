#pragma once

#include "Commandlets/Commandlet.h"
#include "ECPMigrateAssetsCommandlet.generated.h"

/**
 * @brief 按审定清单检查或迁移项目资产，并可独立创建 Game Feature 装配资源。
 * @note 无参数仅检查；-ApplyAssets 明确写资产，-CreateFeatureData 仅创建功能数据及 Cook 标签。
 *        所有保存必须匹配本次模式生成的精确白名单，受保护 BMMCSystem 永远不允许保存。
 */
UCLASS()
class ECPFOUNDATIONEDITOR_API UECPMigrateAssetsCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UECPMigrateAssetsCommandlet();

    /**
     * @brief 执行一个互斥模式，并通过退出码区分完整成功与需要人工处理的失败。
     * @param Params 命令行参数；不接受自定义清单或任意源目标路径。
     * @return 0 表示所选模式成功；非零表示校验、加载、重命名或保存失败。
     * @note 这是编辑器离线工具，不参与联网玩法；写入失败可能已留下部分结果，绝不承诺事务回滚。
     */
    virtual int32 Main(const FString& Params) override;
};
