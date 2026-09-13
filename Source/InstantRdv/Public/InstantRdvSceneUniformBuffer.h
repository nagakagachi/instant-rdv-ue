/*
    InstantRdvSceneUniformBuffer.h

    RDVのパラメータやバッファをSceneUniformBufferに追加してマテリアル等からアクセス可能にする.

*/
#pragma once
#include "CoreMinimal.h"
#include "ShaderParameterMacros.h"
#include "SceneUniformBuffer.h"

/*
    Material CustomNode内で

        Scene.InstantRdvParam.TestColor;
        Scene.InstantRdvParam.FspIrradianceVolumeSH;

    のようにSceneUniformBufferメンバとしてアクセス可能になる。
*/
BEGIN_SHADER_PARAMETER_STRUCT(FInstantRdvSceneUniformBufferParams, )
    SHADER_PARAMETER(uint32, FspEnabled)
    SHADER_PARAMETER(uint32, FspGridResolutionX)
    SHADER_PARAMETER(uint32, FspGridResolutionY)
    SHADER_PARAMETER(uint32, FspGridResolutionZ)
    SHADER_PARAMETER(uint32, FspCascadeCount)
    SHADER_PARAMETER(uint32, FspIrradianceVolumeCellCount)
    SHADER_PARAMETER(uint32, FspIrradianceVolumeSHFloat4Count)
    SHADER_PARAMETER(float, FspCellSizeCm)
    SHADER_PARAMETER(uint32, FspCascadeDitherInterpolation)
    SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, FspIrradianceVolumeSH)
    SHADER_PARAMETER(uint32, BbvEnabled)
    SHADER_PARAMETER(uint32, BbvGridResolutionX)
    SHADER_PARAMETER(uint32, BbvGridResolutionY)
    SHADER_PARAMETER(uint32, BbvGridResolutionZ)
    SHADER_PARAMETER(uint32, BbvBrickResolution)
    SHADER_PARAMETER(float, BbvBrickSizeCm)
    SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
    SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
    SHADER_PARAMETER(uint32, BbvBrickDataBaseOffset)
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvRadianceAccum)
END_SHADER_PARAMETER_STRUCT()

DECLARE_SCENE_UB_STRUCT(FInstantRdvSceneUniformBufferParams, InstantRdvParam, )
