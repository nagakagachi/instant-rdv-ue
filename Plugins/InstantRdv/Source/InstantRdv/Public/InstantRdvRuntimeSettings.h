/*
    InstantRdvRuntimeSettings.h

    InstantRDV のレベル保存設定と、ゲームスレッドから描画スレッドへ渡す
    不変スナップショットを定義する。
*/
#pragma once

#include "CoreMinimal.h"
#include "InstantRdvRuntimeSettings.generated.h"

USTRUCT(BlueprintType)
struct INSTANTRDV_API FInstantRdvLevelSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV")
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|BBV", meta = (ClampMin = "1"))
    FIntVector BbvGridResolution = FIntVector(64, 64, 64);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|BBV", meta = (ClampMin = "1.0", Units = "cm"))
    float BbvBrickSizeCm = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|FSP", meta = (ClampMin = "1"))
    FIntVector ProbeGridResolution = FIntVector(32, 32, 32);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|FSP", meta = (ClampMin = "1.0", Units = "cm"))
    float ProbeCellSizeCm = 200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|FSP", meta = (ClampMin = "1", ClampMax = "16"))
    int32 ProbeCascadeCount = 5;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|FSP", meta = (ClampMin = "1"))
    int32 ProbePoolCapacity = 8192;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|FSP", meta = (ClampMin = "1"))
    int32 VisibleSurfaceCapacity = 4096;


};

struct FInstantRdvRenderSettings
{
    FInstantRdvLevelSettings LevelSettings;
    uint64 Revision = 0;
};
