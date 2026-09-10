#include "FU_OnlineSessionSettings.h"

FName UFU_OnlineSessionSettings::GetCategoryName() const
{
    //UDeveloperSettings 会自动注册到 Project Settings。
    return TEXT("Plugins");
}