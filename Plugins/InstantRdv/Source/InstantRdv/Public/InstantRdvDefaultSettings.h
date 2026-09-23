/*
    InstantRdvDefaultSettings.h

    InstantRDV のレベル設定と render config が共有する新規インスタンス用既定値を定義する。
    シェーダー ABI や GPU リソースレイアウトの固定値は instant_rdv_common.ush に置く。
*/
#pragma once

#include "CoreMinimal.h"

namespace InstantRdvDefaults
{
    inline const FIntVector BbvGridResolution(64, 64, 64);
    constexpr float BbvBrickSizeCm = 300.0f;

    inline const FIntVector ProbeGridResolution(16, 16, 16);
    constexpr float ProbeCellSizeCm = 200.0f;
    constexpr uint32 ProbeCascadeCount = 5u;
    constexpr uint32 ProbePoolCapacity = 8192u;
    constexpr uint32 VisibleSurfaceCapacity = 4096u;
}
