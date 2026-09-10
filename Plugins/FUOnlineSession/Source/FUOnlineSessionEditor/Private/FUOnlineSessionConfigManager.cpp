#include "FUOnlineSessionConfigManager.h"

#include "FUOnlineSessionLegacyConfigMigration.h"

DEFINE_LOG_CATEGORY_STATIC(LogFUOnlineSessionConfig, Log, All);

EFU_OnlineConfigResult FFUOnlineSessionConfigManager::EnsureProjectConfiguration()
{
	// 【兼容入口】保留旧私有入口给既有 Editor 调用者；新语义只执行一次遗留块清理，不读取 Settings 生成新配置。
	FFU_LegacyMigrationArtifacts Artifacts;
	const EFU_LegacyMigrationResult MigrationResult =
		FFU_LegacyConfigMigration::MigrateProjectDefaultEngine(Artifacts);

	switch (MigrationResult)
	{
	case EFU_LegacyMigrationResult::NoManagedBlock:
		UE_LOG(LogFUOnlineSessionConfig, Log, TEXT("未发现 FU Online Session 历史 DefaultEngine.ini 受管区块"));
		return EFU_OnlineConfigResult::Unchanged;

	case EFU_LegacyMigrationResult::Published:
		UE_LOG(LogFUOnlineSessionConfig, Log, TEXT("已安全迁移 FU Online Session 历史配置；审计备份：%s"), *Artifacts.SavedBackupPath);
		return EFU_OnlineConfigResult::Updated;

	case EFU_LegacyMigrationResult::PublishedWithWarning:
		// 【结果可证】ReplaceFileW 返回异常但目标哈希已确认发布，提醒用户重启并保留 Saved 备份供审计。
		UE_LOG(LogFUOnlineSessionConfig, Warning, TEXT("历史配置已发布但 ReplaceFileW 返回警告：%s；备份：%s"), *Artifacts.Error, *Artifacts.SavedBackupPath);
		return EFU_OnlineConfigResult::Updated;

	case EFU_LegacyMigrationResult::ManualRecoveryRequired:
		// 【人工恢复】未知目标状态时列出全部保留材料，绝不由插件猜测覆盖方向。
		UE_LOG(LogFUOnlineSessionConfig, Error, TEXT("历史配置迁移需要人工恢复：%s；Saved=%s；Rollback=%s；Temp=%s"), *Artifacts.Error, *Artifacts.SavedBackupPath, *Artifacts.RollbackBackupPath, *Artifacts.TemporaryPath);
		return EFU_OnlineConfigResult::Failed;

	case EFU_LegacyMigrationResult::InvalidMarkers:
	case EFU_LegacyMigrationResult::Failed:
	default:
		// 【失败封闭】标记或 IO 不可信时不生成任何项目配置，要求用户从日志和备份继续处理。
		UE_LOG(LogFUOnlineSessionConfig, Error, TEXT("历史 DefaultEngine.ini 迁移失败：%s"), *Artifacts.Error);
		return EFU_OnlineConfigResult::Failed;
	}
}
