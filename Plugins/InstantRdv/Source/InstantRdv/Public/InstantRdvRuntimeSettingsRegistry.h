/*
    InstantRdvRuntimeSettingsRegistry.h

    InstantRDV のゲームスレッド設定を Scene 単位で render thread に安全に公開するレジストリを定義する。
*/
#pragma once

#include "CoreMinimal.h"
#include "InstantRdvRuntimeSettings.h"

class UWorld;
class FSceneInterface;

class INSTANTRDV_API FInstantRdvRuntimeSettingsRegistry
{
public:
    static void Publish(UWorld& World, const FInstantRdvRenderSettings& Settings);
    static void Remove(UWorld& World);
    static TSharedPtr<const FInstantRdvRenderSettings, ESPMode::ThreadSafe> Find(const FSceneInterface* Scene);
};
