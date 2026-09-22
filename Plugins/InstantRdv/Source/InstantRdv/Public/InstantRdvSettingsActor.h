/*
    InstantRdvSettingsActor.h

    Persistent Level に配置し、InstantRDV の通常運用設定を保存する Actor を定義する。
*/
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InstantRdvRuntimeSettings.h"
#include "InstantRdvSettingsActor.generated.h"

UCLASS(BlueprintType, Blueprintable, meta = (DisplayName = "Instant RDV Settings"))
class INSTANTRDV_API AInstantRdvSettingsActor : public AActor
{
    GENERATED_BODY()

public:
    AInstantRdvSettingsActor();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV", meta = (ShowOnlyInnerProperties))
    FInstantRdvLevelSettings Settings;

    UFUNCTION(BlueprintCallable, Category = "InstantRDV")
    void ApplySettings(const FInstantRdvLevelSettings& InSettings);

    UFUNCTION(BlueprintCallable, Category = "InstantRDV")
    FInstantRdvLevelSettings GetSettings() const { return Settings; }

protected:
    virtual void PostRegisterAllComponents() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void PublishSettings();
};
