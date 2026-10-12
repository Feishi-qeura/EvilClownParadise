#pragma once
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace ECPPIETestCleanup
{
// A PIE override can initialize a source-map world without giving it a live
// context. Dispose only a known fixture source after all PIE instances end.
// Editor worlds and other live contexts remain owned by the engine.
inline void CleanupSourceWorld(const TCHAR* ObjectPath)
{
    UWorld* World = FindObject<UWorld>(nullptr, ObjectPath);
    if (World && World->IsInitialized() && GEngine && !GEngine->GetWorldContextFromWorld(World))
    {
        if (World->HasBegunPlay()) World->EndPlay(EEndPlayReason::Quit);
        World->CleanupWorld();
    }
}
}
