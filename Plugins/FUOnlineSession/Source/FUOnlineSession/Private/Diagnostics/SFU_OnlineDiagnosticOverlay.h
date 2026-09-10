#pragma once

#include "CoreMinimal.h"
#include "Diagnostics/FU_OnlineSessionDiagnostics.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

/**
 * 不依赖 UMG 资产的运行时诊断浮层。
 *
 * 它只读取脱敏后的 OverlayModel，并由 GameViewportClient 精确添加/移除，
 * 因而不会使用仅开发用途的 AddOnScreenDebugMessage，也不会污染玩家现有 Widget 树。
 */
class SFU_OnlineDiagnosticOverlay final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFU_OnlineDiagnosticOverlay) {}
		SLATE_ARGUMENT(TSharedPtr<FFU_OnlineDiagnosticOverlayModel>, Model)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;

private:
	void RefreshRows();
	static FLinearColor GetSeverityColor(EFU_OnlineDiagnosticSeverity Severity);
	static FLinearColor GetSeverityBackground(EFU_OnlineDiagnosticSeverity Severity);

	TSharedPtr<FFU_OnlineDiagnosticOverlayModel> Model;
	TSharedPtr<SVerticalBox> RowsContainer;
	TArray<FFU_OnlineDiagnosticOverlayRow> LastRenderedRows;
};
