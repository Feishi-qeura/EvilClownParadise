#pragma once

#include "CoreMinimal.h"
#include "Misc/SecureHash.h"

/** 一次性旧项目配置迁移的可观察结果。 */
enum class EFU_LegacyMigrationResult : uint8
{
	NoManagedBlock,
	Published,
	PublishedWithWarning,
	InvalidMarkers,
	Failed,
	ManualRecoveryRequired
};

/** 迁移过程中创建或保留的文件，供日志和人工恢复准确定位。 */
struct FFU_LegacyMigrationArtifacts
{
	FString SavedBackupPath;
	FString RollbackBackupPath;
	FString TemporaryPath;
	FString Error;
};

/** 纯字节变换的返回值；无效标记时不允许携带可发布内容。 */
struct FFU_LegacyTransformResult
{
	EFU_LegacyMigrationResult Result = EFU_LegacyMigrationResult::Failed;
	TArray<uint8> TransformedBytes;
	FString Error;
};

/**
 * 只在 Editor 私有模块中使用的历史 DefaultEngine.ini 清理器。
 * 它只处理以前由 FU 写入的标记区块，绝不生成新的项目 Engine 配置。
 */
class FFU_LegacyConfigMigration final
{
public:
	static FFU_LegacyTransformResult Transform(const TArray<uint8>& OriginalBytes);
	static EFU_LegacyMigrationResult ClassifyReplaceOutcome(
		const FSHAHash& OriginalHash,
		const FSHAHash& TransformedHash,
		const TOptional<FSHAHash>& TargetHash);
	static EFU_LegacyMigrationResult MigrateProjectDefaultEngine(
		FFU_LegacyMigrationArtifacts& OutArtifacts);
};
