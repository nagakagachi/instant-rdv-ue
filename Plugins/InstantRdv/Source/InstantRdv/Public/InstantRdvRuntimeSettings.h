/*
    InstantRdvRuntimeSettings.h

    InstantRDV のレベル保存設定と、ゲームスレッドから描画スレッドへ渡す
    不変スナップショットを定義する。
*/
#pragma once

#include "CoreMinimal.h"
#include "InstantRdvDefaultSettings.h"
#include "InstantRdvRuntimeSettings.generated.h"

USTRUCT(BlueprintType)
struct INSTANTRDV_API FInstantRdvLevelSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV")
    // InstantRDV基幹機能をレベル単位で有効にする。BBV更新を含む。
    bool bEnabled = false;

    // InstantRDV GI機能のVSP更新とマテリアル公開をレベル単位で制御する。BBV基幹更新には影響しない。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|GI")
    bool bGiEnabled = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|BBV", meta = (ClampMin = "1"))
    FIntVector BbvGridResolution = InstantRdvDefaults::BbvGridResolution;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|BBV", meta = (ClampMin = "1.0", Units = "cm"))
    float BbvBrickSizeCm = InstantRdvDefaults::BbvBrickSizeCm;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|VSP", meta = (ClampMin = "1"))
    FIntVector ProbeGridResolution = InstantRdvDefaults::ProbeGridResolution;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|VSP", meta = (ClampMin = "1.0", Units = "cm"))
    float ProbeCellSizeCm = InstantRdvDefaults::ProbeCellSizeCm;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|VSP", meta = (ClampMin = "1", ClampMax = "16"))
    int32 ProbeCascadeCount = static_cast<int32>(InstantRdvDefaults::ProbeCascadeCount);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|VSP", meta = (ClampMin = "1"))
    int32 ProbePoolCapacity = static_cast<int32>(InstantRdvDefaults::ProbePoolCapacity);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InstantRDV|VSP", meta = (ClampMin = "1"))
    int32 VisibleSurfaceCapacity = static_cast<int32>(InstantRdvDefaults::VisibleSurfaceCapacity);


};

struct FInstantRdvRenderSettings
{
    FInstantRdvLevelSettings LevelSettings;
    uint64 Revision = 0;
};
