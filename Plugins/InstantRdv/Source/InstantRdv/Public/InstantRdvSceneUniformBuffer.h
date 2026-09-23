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
        Scene.InstantRdvParam.VspIrradianceVolumeSH;

    のようにSceneUniformBufferメンバとしてアクセス可能になる。
*/
BEGIN_SHADER_PARAMETER_STRUCT(FInstantRdvSceneUniformBufferParams, )
    SHADER_PARAMETER(uint32, VspEnabled)
    SHADER_PARAMETER(uint32, VspGridResolutionX)
    SHADER_PARAMETER(uint32, VspGridResolutionY)
    SHADER_PARAMETER(uint32, VspGridResolutionZ)
    SHADER_PARAMETER(uint32, VspCascadeCount)
    SHADER_PARAMETER(uint32, VspIrradianceVolumeCellCount)
    SHADER_PARAMETER(float, VspCellSizeCm)
    SHADER_PARAMETER(uint32, VspCascadeDitherInterpolation)
    SHADER_PARAMETER(uint32, VspTrilinearInterpolation)
    SHADER_PARAMETER(FVector3f, VspGridCenterPositionWs)
    SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float4>, VspIrradianceVolumeSH)
    SHADER_PARAMETER_SAMPLER(SamplerState, VspIrradianceVolumeSampler)
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

class FRDGBuilder;
INSTANTRDV_API void InitializeInstantRdvSceneUniformBufferDefaults(FInstantRdvSceneUniformBufferParams& ShaderParams, FRDGBuilder& GraphBuilder);
