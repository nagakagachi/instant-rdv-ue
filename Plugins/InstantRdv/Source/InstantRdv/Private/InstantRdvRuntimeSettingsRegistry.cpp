/*
    InstantRdvRuntimeSettingsRegistry.cpp

    Scene 単位の設定スナップショットをゲームスレッドと描画スレッドの間で同期する。
*/
#include "InstantRdvRuntimeSettingsRegistry.h"

#include "Engine/World.h"

namespace
{
FRWLock GInstantRdvSettingsLock;
TMap<const FSceneInterface*, TSharedPtr<const FInstantRdvRenderSettings, ESPMode::ThreadSafe>> GInstantRdvSettingsByScene;
}

void FInstantRdvRuntimeSettingsRegistry::Publish(UWorld& World, const FInstantRdvRenderSettings& Settings)
{
    if (World.Scene == nullptr)
    {
        return;
    }

    FWriteScopeLock Lock(GInstantRdvSettingsLock);
    GInstantRdvSettingsByScene.Add(World.Scene, MakeShared<FInstantRdvRenderSettings, ESPMode::ThreadSafe>(Settings));
}

void FInstantRdvRuntimeSettingsRegistry::Remove(UWorld& World)
{
    if (World.Scene == nullptr)
    {
        return;
    }

    FWriteScopeLock Lock(GInstantRdvSettingsLock);
    GInstantRdvSettingsByScene.Remove(World.Scene);
}

TSharedPtr<const FInstantRdvRenderSettings, ESPMode::ThreadSafe> FInstantRdvRuntimeSettingsRegistry::Find(const FSceneInterface* Scene)
{
    FReadScopeLock Lock(GInstantRdvSettingsLock);
    if (const TSharedPtr<const FInstantRdvRenderSettings, ESPMode::ThreadSafe>* Found = GInstantRdvSettingsByScene.Find(Scene))
    {
        return *Found;
    }
    return nullptr;
}
