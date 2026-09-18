#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FU_CheckSessionStatusTestReceiver.generated.h"

// 仅放在 Editor 模块：记录真实蓝图动态委托的输出，测试辅助对象不会进入游戏包。
UCLASS()
class UFU_CheckSessionStatusTestReceiver : public UObject
{
	GENERATED_BODY()

public:
	int32 ServerCount = 0;
	int32 ClientCount = 0;
	int32 TimeoutCount = 0;
	float LastPing = -1.0f;

	// 各输出独立计数，避免错误分支也让回归测试通过。
	UFUNCTION()
	void ReceiveServer(float Ping) { ++ServerCount; LastPing = Ping; }
	UFUNCTION()
	void ReceiveClient(float Ping) { ++ClientCount; LastPing = Ping; }
	UFUNCTION()
	void ReceiveTimeout(float Ping) { ++TimeoutCount; LastPing = Ping; }
};
