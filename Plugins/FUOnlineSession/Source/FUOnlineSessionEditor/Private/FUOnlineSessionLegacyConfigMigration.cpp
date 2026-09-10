#include "FUOnlineSessionLegacyConfigMigration.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace FUOnlineSessionLegacyMigration
{
	constexpr ANSICHAR BeginMarker[] = "; BEGIN FUONLINESESSION AUTO CONFIG";
	constexpr ANSICHAR EndMarker[] = "; END FUONLINESESSION AUTO CONFIG";

	struct FPhysicalLine
	{
		int32 Start = 0;
		int32 ContentEnd = 0;
		int32 EndWithTerminator = 0;
	};

	/**
	 * 【字节边界】只按 CR、LF 与 CRLF 切分物理行，不将 ini 内容转换为 TCHAR。
	 * 这样 BOM、未知编码和二进制噪声都不会被解析器重写；末行无换行同样是完整物理行。
	 */
	void EnumeratePhysicalLines(const TArray<uint8>& Bytes, TArray<FPhysicalLine>& OutLines)
	{
		int32 Cursor = 0;
		while (Cursor < Bytes.Num())
		{
			FPhysicalLine& Line = OutLines.Emplace_GetRef();
			Line.Start = Cursor;
			while (Cursor < Bytes.Num() && Bytes[Cursor] != '\r' && Bytes[Cursor] != '\n')
			{
				++Cursor;
			}

			Line.ContentEnd = Cursor;
			if (Cursor < Bytes.Num() && Bytes[Cursor] == '\r')
			{
				++Cursor;
				if (Cursor < Bytes.Num() && Bytes[Cursor] == '\n')
				{
					++Cursor;
				}
			}
			else if (Cursor < Bytes.Num())
			{
				++Cursor;
			}

			Line.EndWithTerminator = Cursor;
		}
	}

	/**
	 * 【精确标记】完整物理行才属于插件；前后多出任意字节都必须按人工损坏处理。
	 * 返回 false 时调用者不会发布转换结果，防止删除用户在注释或值中写出的相同文字。
	 */
	bool IsExactMarkerLine(const TArray<uint8>& Bytes, const FPhysicalLine& Line, const ANSICHAR* Marker)
	{
		const int32 MarkerLength = FCStringAnsi::Strlen(Marker);
		return Line.ContentEnd - Line.Start == MarkerLength
			&& FMemory::Memcmp(Bytes.GetData() + Line.Start, Marker, MarkerLength) == 0;
	}

	/**
	 * 【损坏检测】统计所有原始字节出现位置；只要同名标记藏在非完整行中便拒绝迁移。
	 * 该扫描不修改输入，所以误判或截断时最坏结果是要求人工处理而不是触碰项目配置。
	 */
	int32 CountRawOccurrences(const TArray<uint8>& Bytes, const ANSICHAR* Marker)
	{
		const int32 MarkerLength = FCStringAnsi::Strlen(Marker);
		int32 Count = 0;
		for (int32 Index = 0; Index + MarkerLength <= Bytes.Num(); ++Index)
		{
			if (FMemory::Memcmp(Bytes.GetData() + Index, Marker, MarkerLength) == 0)
			{
				++Count;
			}
		}

		return Count;
	}

	/**
	 * 【关闭保证】以平台文件句柄写入并完整 Flush 后销毁句柄。
	 * 写入失败时绝不尝试字符串编码或覆盖回退，调用方只能保留备份并报告失败。
	 */
	bool WriteBytesAndClose(const FString& Path, const TArray<uint8>& Bytes, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Path, false, false));
		if (!Handle.IsValid())
		{
			OutError = FString::Printf(TEXT("无法打开写入文件：%s"), *Path);
			return false;
		}

		const bool bWrote = Bytes.IsEmpty() || Handle->Write(Bytes.GetData(), Bytes.Num());
		const bool bFlushed = bWrote && Handle->Flush(true);
		Handle.Reset();
		if (!bFlushed)
		{
			OutError = FString::Printf(TEXT("写入或刷新失败：%s"), *Path);
		}

		return bFlushed;
	}

	/** 私有文件操作接缝仅留在本 cpp，方便 publisher 结果路径做单元替身而不暴露写入 API。 */
	class IFU_LegacyMigrationFileOperations
	{
	public:
		virtual ~IFU_LegacyMigrationFileOperations() = default;
		virtual bool WriteBytesAndCloseFile(const FString& Path, const TArray<uint8>& Bytes, FString& OutError) = 0;
		virtual bool ReplaceAtomically(const FString& TargetPath, const FString& TemporaryPath, const FString& RollbackPath, FString& OutError) = 0;
	};

	class FFU_NativeLegacyMigrationFileOperations final : public IFU_LegacyMigrationFileOperations
	{
	public:
		virtual bool WriteBytesAndCloseFile(const FString& Path, const TArray<uint8>& Bytes, FString& OutError) override
		{
			return WriteBytesAndClose(Path, Bytes, OutError);
		}

		virtual bool ReplaceAtomically(const FString& TargetPath, const FString& TemporaryPath, const FString& RollbackPath, FString& OutError) override
		{
#if PLATFORM_WINDOWS
			// 【原子发布】只接受 ReplaceFileW 的单步替换语义；失败后必须依赖目标哈希判断，禁止覆盖回退。
			const BOOL bReplaced = ::ReplaceFileW(*TargetPath, *TemporaryPath, *RollbackPath, 0, nullptr, nullptr);
			if (!bReplaced)
			{
				OutError = FString::Printf(TEXT("ReplaceFileW 失败，Win32Error=%lu"), ::GetLastError());
			}

			// WindowsHWrapper 会收紧传统宏定义；用 BOOL 的数值语义避免依赖未导出的 FALSE 宏。
			return bReplaced != 0;
#else
			// 【平台封闭】非 Windows 无法证明 ReplaceFileW 语义，宁可迁移失败也不能采用非原子替代实现。
			OutError = TEXT("Legacy DefaultEngine.ini migration requires Windows ReplaceFileW");
			return false;
#endif
		}
	};

	/**
	 * 【命名隔离】所有临时/回滚文件都带进程和高精度时间标识，避免同时启动的 Editor 互相覆盖。
	 * 临时与回滚文件保持在目标目录，保证 ReplaceFileW 不会跨卷退化；Saved 备份单独用于审计恢复。
	 */
	FString MakeUniquePath(const FString& Directory, const FString& Prefix, const FString& Extension)
	{
		return FPaths::Combine(
			Directory,
			FString::Printf(
				TEXT("%s-%s-%u-%llu%s"),
				*Prefix,
				*FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S-%f")),
				FPlatformProcess::GetCurrentProcessId(),
				FPlatformTime::Cycles64(),
				*Extension));
	}

	/** 加载并哈希目标；读取失败必须与“仍为原始内容”区分，避免错误清理恢复材料。 */
	TOptional<FSHAHash> LoadHash(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return TOptional<FSHAHash>();
		}

		return FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num());
	}

	/** 已知目标安全时只删本次已不需要的过程文件，永远保留 Saved 审计备份。 */
	void RemoveKnownUnneededArtifact(const FString& Path)
	{
		if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
		{
			IFileManager::Get().Delete(*Path, false, true, true);
		}
	}
}

FFU_LegacyTransformResult FFU_LegacyConfigMigration::Transform(const TArray<uint8>& OriginalBytes)
{
	using namespace FUOnlineSessionLegacyMigration;

	TArray<FPhysicalLine> Lines;
	EnumeratePhysicalLines(OriginalBytes, Lines);

	int32 WholeBeginCount = 0;
	int32 WholeEndCount = 0;
	FPhysicalLine BeginLine;
	FPhysicalLine EndLine;
	for (const FPhysicalLine& Line : Lines)
	{
		if (IsExactMarkerLine(OriginalBytes, Line, BeginMarker))
		{
			++WholeBeginCount;
			BeginLine = Line;
		}
		if (IsExactMarkerLine(OriginalBytes, Line, EndMarker))
		{
			++WholeEndCount;
			EndLine = Line;
		}
	}

	const int32 RawBeginCount = CountRawOccurrences(OriginalBytes, BeginMarker);
	const int32 RawEndCount = CountRawOccurrences(OriginalBytes, EndMarker);
	if (RawBeginCount == 0 && RawEndCount == 0)
	{
		// 【无遗留配置】没有任何标记时保持原始字节，调用方据此保证不创建备份或临时文件。
		FFU_LegacyTransformResult Result;
		Result.Result = EFU_LegacyMigrationResult::NoManagedBlock;
		Result.TransformedBytes = OriginalBytes;
		return Result;
	}

	if (RawBeginCount != 1 || RawEndCount != 1
		|| WholeBeginCount != 1 || WholeEndCount != 1
		|| BeginLine.Start >= EndLine.Start)
	{
		// 【失败封闭】重复、缺失、倒序或藏在非完整行中的标记都可能是人工内容，不能猜测删除范围。
		FFU_LegacyTransformResult Result;
		Result.Result = EFU_LegacyMigrationResult::InvalidMarkers;
		Result.Error = TEXT("FUOnlineSession legacy config markers are missing, malformed, duplicated, or reversed");
		return Result;
	}

	FFU_LegacyTransformResult Result;
	Result.Result = EFU_LegacyMigrationResult::Published;
	Result.TransformedBytes.Reserve(OriginalBytes.Num() - (EndLine.EndWithTerminator - BeginLine.Start));
	Result.TransformedBytes.Append(OriginalBytes.GetData(), BeginLine.Start);
	Result.TransformedBytes.Append(OriginalBytes.GetData() + EndLine.EndWithTerminator, OriginalBytes.Num() - EndLine.EndWithTerminator);
	return Result;
}

EFU_LegacyMigrationResult FFU_LegacyConfigMigration::ClassifyReplaceOutcome(
	const FSHAHash& OriginalHash,
	const FSHAHash& TransformedHash,
	const TOptional<FSHAHash>& TargetHash)
{
	// 【结果判定】ReplaceFileW 的返回值不足以说明落盘状态；只信任重新读取后的精确哈希。
	if (!TargetHash.IsSet())
	{
		return EFU_LegacyMigrationResult::ManualRecoveryRequired;
	}
	if (TargetHash.GetValue() == OriginalHash)
	{
		return EFU_LegacyMigrationResult::Failed;
	}
	if (TargetHash.GetValue() == TransformedHash)
	{
		return EFU_LegacyMigrationResult::PublishedWithWarning;
	}

	return EFU_LegacyMigrationResult::ManualRecoveryRequired;
}

EFU_LegacyMigrationResult FFU_LegacyConfigMigration::MigrateProjectDefaultEngine(
	FFU_LegacyMigrationArtifacts& OutArtifacts)
{
	using namespace FUOnlineSessionLegacyMigration;
	OutArtifacts = FFU_LegacyMigrationArtifacts();

#if !PLATFORM_WINDOWS
	// 【平台封闭】不支持的平台没有等价的发布证明，故不读取/修改项目配置。
	OutArtifacts.Error = TEXT("Legacy DefaultEngine.ini migration is supported only on Windows");
	return EFU_LegacyMigrationResult::Failed;
#else
	const FString TargetPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini")));
	TArray<uint8> OriginalBytes;
	if (!FFileHelper::LoadFileToArray(OriginalBytes, *TargetPath))
	{
		OutArtifacts.Error = FString::Printf(TEXT("无法读取项目 DefaultEngine.ini：%s"), *TargetPath);
		return EFU_LegacyMigrationResult::Failed;
	}

	const FFU_LegacyTransformResult TransformResult = Transform(OriginalBytes);
	if (TransformResult.Result != EFU_LegacyMigrationResult::Published)
	{
		// 【不可发布转换】无标记不需要写入；标记损坏必须完整保留现场并交给人工判断。
		OutArtifacts.Error = TransformResult.Error;
		return TransformResult.Result;
	}

	const FString SavedBackupDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FUOnlineSession/ConfigBackups"));
	if (!IFileManager::Get().MakeDirectory(*SavedBackupDirectory, true))
	{
		OutArtifacts.Error = FString::Printf(TEXT("无法创建迁移备份目录：%s"), *SavedBackupDirectory);
		return EFU_LegacyMigrationResult::Failed;
	}

	const FString TargetDirectory = FPaths::GetPath(TargetPath);
	OutArtifacts.SavedBackupPath = MakeUniquePath(SavedBackupDirectory, TEXT("DefaultEngine.ini.fu-legacy-backup"), TEXT(".ini"));
	OutArtifacts.TemporaryPath = MakeUniquePath(TargetDirectory, TEXT("DefaultEngine.ini.fu-legacy-temp"), TEXT(".tmp"));
	OutArtifacts.RollbackBackupPath = MakeUniquePath(TargetDirectory, TEXT("DefaultEngine.ini.fu-legacy-rollback"), TEXT(".bak"));

	FFU_NativeLegacyMigrationFileOperations FileOperations;
	if (!FileOperations.WriteBytesAndCloseFile(OutArtifacts.SavedBackupPath, OriginalBytes, OutArtifacts.Error))
	{
		return EFU_LegacyMigrationResult::Failed;
	}
	if (!FileOperations.WriteBytesAndCloseFile(OutArtifacts.TemporaryPath, TransformResult.TransformedBytes, OutArtifacts.Error))
	{
		// 【已知未发布】目标尚未进入替换调用，临时残片无恢复价值，可安全清理；Saved 备份保留。
		RemoveKnownUnneededArtifact(OutArtifacts.TemporaryPath);
		return EFU_LegacyMigrationResult::Failed;
	}

	TArray<uint8> VerifiedTemporaryBytes;
	if (!FFileHelper::LoadFileToArray(VerifiedTemporaryBytes, *OutArtifacts.TemporaryPath)
		|| VerifiedTemporaryBytes != TransformResult.TransformedBytes)
	{
		// 【发布前校验】临时文件字节不一致时拒绝 ReplaceFileW，防止存储层异常扩大到目标配置。
		OutArtifacts.Error = FString::Printf(TEXT("临时迁移文件校验失败：%s"), *OutArtifacts.TemporaryPath);
		RemoveKnownUnneededArtifact(OutArtifacts.TemporaryPath);
		return EFU_LegacyMigrationResult::Failed;
	}

	const FSHAHash OriginalHash = FSHA1::HashBuffer(OriginalBytes.GetData(), OriginalBytes.Num());
	const FSHAHash TransformedHash = FSHA1::HashBuffer(TransformResult.TransformedBytes.GetData(), TransformResult.TransformedBytes.Num());
	const bool bReplaceSucceeded = FileOperations.ReplaceAtomically(
		TargetPath,
		OutArtifacts.TemporaryPath,
		OutArtifacts.RollbackBackupPath,
		OutArtifacts.Error);
	const TOptional<FSHAHash> TargetHash = LoadHash(TargetPath);
	const EFU_LegacyMigrationResult ClassifiedResult = ClassifyReplaceOutcome(OriginalHash, TransformedHash, TargetHash);

	if (ClassifiedResult == EFU_LegacyMigrationResult::PublishedWithWarning)
	{
		// 【哈希优先】API 报失败但目标已是转换字节时，发布事实成立；仅保留可诊断的警告结果。
		RemoveKnownUnneededArtifact(OutArtifacts.TemporaryPath);
		RemoveKnownUnneededArtifact(OutArtifacts.RollbackBackupPath);
		return bReplaceSucceeded ? EFU_LegacyMigrationResult::Published : EFU_LegacyMigrationResult::PublishedWithWarning;
	}
	if (ClassifiedResult == EFU_LegacyMigrationResult::Failed)
	{
		// 【已知原状】目标哈希仍为原始内容时未发生发布，可清除过程文件但保留 Saved 备份做审计。
		RemoveKnownUnneededArtifact(OutArtifacts.TemporaryPath);
		RemoveKnownUnneededArtifact(OutArtifacts.RollbackBackupPath);
		return EFU_LegacyMigrationResult::Failed;
	}

	// 【人工恢复】目标缺失或未知哈希时绝不删除 temp/rollback/Saved，路径已写入 Artifacts 供人工选择恢复源。
	if (OutArtifacts.Error.IsEmpty())
	{
		OutArtifacts.Error = FString::Printf(TEXT("ReplaceFileW 后目标状态未知：%s"), *TargetPath);
	}
	return EFU_LegacyMigrationResult::ManualRecoveryRequired;
#endif
}
