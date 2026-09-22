/*
    InstantRdvWorldSubsystem.h

    World ごとの InstantRDV 設定 Actor を管理し、Blueprint からの設定変更窓口を提供する。
*/
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "InstantRdvRuntimeSettings.h"
#include "InstantRdvWorldSubsystem.generated.h"

class AInstantRdvSettingsActor;

UCLASS()
class INSTANTRDV_API UInstantRdvWorldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "InstantRDV", meta = (WorldContext = "WorldContextObject"))
    static UInstantRdvWorldSubsystem* Get(const UObject* WorldContextObject);

    UFUNCTION(BlueprintCallable, Category = "InstantRDV")
    void ApplySettings(const FInstantRdvLevelSettings& InSettings);

    UFUNCTION(BlueprintPure, Category = "InstantRDV")
    FInstantRdvLevelSettings GetSettings() const { return Settings; }

    void RegisterSettingsActor(AInstantRdvSettingsActor* InActor);
    void UnregisterSettingsActor(AInstantRdvSettingsActor* InActor);

private:
    void Publish();

    TWeakObjectPtr<AInstantRdvSettingsActor> SettingsActor;
    FInstantRdvLevelSettings Settings;
    uint64 Revision = 0;
};
