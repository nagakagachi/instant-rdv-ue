/*
    InstantRdvWorldSubsystem.cpp

    World ごとの Persistent Level 設定 Actor を発見し、render-thread 用スナップショットを更新する。
*/
#include "InstantRdvWorldSubsystem.h"

#include "EngineUtils.h"
#include "InstantRdvRuntimeSettingsRegistry.h"
#include "InstantRdvSettingsActor.h"

void UInstantRdvWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    if (UWorld* World = GetWorld())
    {
        for (TActorIterator<AInstantRdvSettingsActor> It(World); It; ++It)
        {
            if (It->GetLevel() == World->PersistentLevel)
            {
                RegisterSettingsActor(*It);
                break;
            }
        }
    }
}

void UInstantRdvWorldSubsystem::Deinitialize()
{
    if (UWorld* World = GetWorld())
    {
        FInstantRdvRuntimeSettingsRegistry::Remove(*World);
    }
    SettingsActor.Reset();
    Super::Deinitialize();
}

UInstantRdvWorldSubsystem* UInstantRdvWorldSubsystem::Get(const UObject* WorldContextObject)
{
    if (WorldContextObject == nullptr)
    {
        return nullptr;
    }

    if (UWorld* World = WorldContextObject->GetWorld())
    {
        return World->GetSubsystem<UInstantRdvWorldSubsystem>();
    }
    return nullptr;
}

void UInstantRdvWorldSubsystem::RegisterSettingsActor(AInstantRdvSettingsActor* InActor)
{
    if (InActor == nullptr || InActor->GetWorld() != GetWorld() || InActor->GetLevel() != GetWorld()->PersistentLevel)
    {
        return;
    }

    SettingsActor = InActor;
    Settings = InActor->Settings;
    Publish();
}

void UInstantRdvWorldSubsystem::UnregisterSettingsActor(AInstantRdvSettingsActor* InActor)
{
    if (SettingsActor.Get() == InActor)
    {
        SettingsActor.Reset();
        Settings.bEnabled = false;
        Publish();
    }
}

void UInstantRdvWorldSubsystem::ApplySettings(const FInstantRdvLevelSettings& InSettings)
{
    Settings = InSettings;
    if (AInstantRdvSettingsActor* Actor = SettingsActor.Get())
    {
        Actor->Settings = InSettings;
    }
    Publish();
}

void UInstantRdvWorldSubsystem::Publish()
{
    if (UWorld* World = GetWorld())
    {
        FInstantRdvRenderSettings Snapshot;
        Snapshot.LevelSettings = Settings;
        Snapshot.Revision = ++Revision;
        FInstantRdvRuntimeSettingsRegistry::Publish(*World, Snapshot);
    }
}
