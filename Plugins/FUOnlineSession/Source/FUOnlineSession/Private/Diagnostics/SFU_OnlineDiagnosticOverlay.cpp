#include "Diagnostics/SFU_OnlineDiagnosticOverlay.h"

#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SFU_OnlineDiagnosticOverlay::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	SetCanTick(true);

	// 【输入穿透】该 Viewport 浮层只负责绘制脱敏诊断，不提供任何可交互控件。
	// HitTestInvisible 会同时排除根节点和所有 Border/Text 子节点；不能使用
	// SelfHitTestInvisible，否则子节点仍可能抢走底层 UMG 控件的 click/hover。
	SetVisibility(EVisibility::HitTestInvisible);

	ChildSlot
	[
		SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(16.0f))
		[
			SAssignNew(RowsContainer, SVerticalBox)
		]
	];

	RefreshRows();
}

void SFU_OnlineDiagnosticOverlay::Tick(
	const FGeometry& AllottedGeometry,
	const double InCurrentTime,
	const float InDeltaTime)
{
	// SCompoundWidget 将 Super typedef 保持为 private；这里显式调用基类，兼容 UE 5.8 的 Slate 声明方式。
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// 【轻量刷新】最多 20 行，且只有内容/过期状态改变时才重建 Slate 子项。
	RefreshRows();
}

void SFU_OnlineDiagnosticOverlay::RefreshRows()
{
	if (!Model.IsValid() || !RowsContainer.IsValid())
	{
		return;
	}

	const TArray<FFU_OnlineDiagnosticOverlayRow> VisibleRows = Model->GetVisibleRows(FDateTime::UtcNow());

	// 【视觉生命周期】最后一行过期后折叠内容容器，让快照在时限到达时真正消失；
	// 根 Widget 仍保持 Tick，后续新诊断无需重新附着 Viewport 就能再次出现。
	const EVisibility DesiredRowsVisibility =
		VisibleRows.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
	if (RowsContainer->GetVisibility() != DesiredRowsVisibility)
	{
		RowsContainer->SetVisibility(DesiredRowsVisibility);
	}

	bool bRowsChanged = VisibleRows.Num() != LastRenderedRows.Num();
	if (!bRowsChanged)
	{
		for (int32 Index = 0; Index < VisibleRows.Num(); ++Index)
		{
			if (VisibleRows[Index].Severity != LastRenderedRows[Index].Severity
				|| VisibleRows[Index].Text != LastRenderedRows[Index].Text)
			{
				bRowsChanged = true;
				break;
			}
		}
	}

	if (!bRowsChanged)
	{
		return;
	}

	RowsContainer->ClearChildren();
	for (const FFU_OnlineDiagnosticOverlayRow& Row : VisibleRows)
	{
		RowsContainer->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
		[
			SNew(SBorder)
			.Padding(FMargin(8.0f, 5.0f))
			.BorderBackgroundColor(GetSeverityBackground(Row.Severity))
			[
				SNew(STextBlock)
				.ColorAndOpacity(GetSeverityColor(Row.Severity))
				.Text(FText::FromString(Row.Text))
			]
		];
	}

	LastRenderedRows = VisibleRows;
}

#if WITH_DEV_AUTOMATION_TESTS
EVisibility SFU_OnlineDiagnosticOverlay::GetRowsVisibilityForTesting() const
{
	return RowsContainer.IsValid() ? RowsContainer->GetVisibility() : EVisibility::Collapsed;
}

int32 SFU_OnlineDiagnosticOverlay::GetRenderedRowCountForTesting() const
{
	return RowsContainer.IsValid() ? RowsContainer->NumSlots() : 0;
}
#endif

FLinearColor SFU_OnlineDiagnosticOverlay::GetSeverityColor(const EFU_OnlineDiagnosticSeverity Severity)
{
	switch (Severity)
	{
	case EFU_OnlineDiagnosticSeverity::Verbose:
		return FLinearColor(0.70f, 0.70f, 0.70f);
	case EFU_OnlineDiagnosticSeverity::Info:
		return FLinearColor(0.65f, 0.85f, 1.00f);
	case EFU_OnlineDiagnosticSeverity::Warning:
		return FLinearColor(1.00f, 0.82f, 0.30f);
	case EFU_OnlineDiagnosticSeverity::Error:
		return FLinearColor(1.00f, 0.40f, 0.40f);
	default:
		return FLinearColor::White;
	}
}

FLinearColor SFU_OnlineDiagnosticOverlay::GetSeverityBackground(const EFU_OnlineDiagnosticSeverity Severity)
{
	switch (Severity)
	{
	case EFU_OnlineDiagnosticSeverity::Warning:
		return FLinearColor(0.35f, 0.22f, 0.02f, 0.90f);
	case EFU_OnlineDiagnosticSeverity::Error:
		return FLinearColor(0.36f, 0.04f, 0.04f, 0.90f);
	case EFU_OnlineDiagnosticSeverity::Verbose:
	case EFU_OnlineDiagnosticSeverity::Info:
	default:
		return FLinearColor(0.02f, 0.06f, 0.12f, 0.88f);
	}
}
