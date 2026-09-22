/*
    InstantRdvSettingsActor.cpp

    Persistent Level の設定を World Subsystem へ通知する Actor 実装。
*/
#include "InstantRdvSettingsActor.h"

#include "InstantRdvWorldSubsystem.h"

AInstantRdvSettingsActor::AInstantRdvSettingsActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bIsSpatiallyLoaded = false;
}

void AInstantRdvSettingsActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // BeginPlay前のEditor/Mapロード時にも設定をWorldSubsystemへ公開する。
    PublishSettings();
}

void AInstantRdvSettingsActor::BeginPlay()
{
    Super::BeginPlay();
    PublishSettings();
}

void AInstantRdvSettingsActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UInstantRdvWorldSubsystem* Subsystem = GetWorld()->GetSubsystem<UInstantRdvWorldSubsystem>())
    {
        Subsystem->UnregisterSettingsActor(this);
    }
    Super::EndPlay(EndPlayReason);
}

void AInstantRdvSettingsActor::ApplySettings(const FInstantRdvLevelSettings& InSettings)
{
    Settings = InSettings;
    PublishSettings();
}

void AInstantRdvSettingsActor::PublishSettings()
{
    if (UWorld* World = GetWorld())
    {
        if (UInstantRdvWorldSubsystem* Subsystem = World->GetSubsystem<UInstantRdvWorldSubsystem>())
        {
            Subsystem->RegisterSettingsActor(this);
        }
    }
}

#if WITH_EDITOR
void AInstantRdvSettingsActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    PublishSettings();
}
#endif
