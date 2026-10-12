#pragma once

#include "Blueprint/UserWidget.h"
#include "ECPPickupStatusWidget.generated.h"

class UTextBlock;

/** 每个本地玩家只读取自己的扫描和服务器确认的持有状态。 */
UCLASS()
class UECPPickupStatusWidget : public UUserWidget
{
	GENERATED_BODY()
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
private:
	void RefreshStatus();
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HitText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PickupText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EquippedText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LoadText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StoredText;
	FTimerHandle RefreshTimer;
};
