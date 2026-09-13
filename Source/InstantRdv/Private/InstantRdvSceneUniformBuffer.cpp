/*
    InstantRdvSceneUniformBuffer.cpp

    RDVのパラメータやバッファをSceneUniformBufferに追加してマテリアル等からアクセス可能にする.
*/

#include "InstantRdvSceneUniformBuffer.h"
#include "RenderGraphUtils.h"

// SceneUniformBufferの拡張部のデフォルト値定義用関数.
static void GetDefaultResourceParameters_FInstantRdvSceneUniformBufferParams(FInstantRdvSceneUniformBufferParams& ShaderParams, FRDGBuilder& GraphBuilder)
{
    const FRDGBufferRef DummyPackedHalf4Buffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2u, 1u),
        TEXT("InstantRdv.SceneUniformBuffer.DummyPackedHalf4"));
    const FRDGBufferRef DummyUintBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1u),
        TEXT("InstantRdv.SceneUniformBuffer.DummyUint"));
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyPackedHalf4Buffer), 0u);
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyUintBuffer), 0u);

    ShaderParams.FspEnabled = 0u;
    ShaderParams.FspGridResolutionX = 0u;
    ShaderParams.FspGridResolutionY = 0u;
    ShaderParams.FspGridResolutionZ = 0u;
    ShaderParams.FspCascadeCount = 0u;
    ShaderParams.FspIrradianceVolumeCellCount = 0u;
    ShaderParams.FspIrradianceVolumeSHFloat4Count = 0u;
    ShaderParams.FspCellSizeCm = 0.0f;
    ShaderParams.FspCascadeDitherInterpolation = 1u;
    ShaderParams.FspGridCenterPositionWs = FVector3f::ZeroVector;
    ShaderParams.FspIrradianceVolumeSH = GraphBuilder.CreateSRV(DummyPackedHalf4Buffer);
    ShaderParams.BbvEnabled = 0u;
    ShaderParams.BbvGridResolutionX = 0u;
    ShaderParams.BbvGridResolutionY = 0u;
    ShaderParams.BbvGridResolutionZ = 0u;
    ShaderParams.BbvBrickResolution = 0u;
    ShaderParams.BbvBrickSizeCm = 0.0f;
    ShaderParams.BbvGridMinPositionWs = FVector3f::ZeroVector;
    ShaderParams.BbvToroidalOffsetCells = FVector3f::ZeroVector;
    ShaderParams.BbvBuffer = GraphBuilder.CreateSRV(DummyUintBuffer);
    ShaderParams.BbvBrickDataBaseOffset = 0u;
    ShaderParams.BbvRadianceAccum = GraphBuilder.CreateSRV(DummyUintBuffer);
}

// SceneUniformBufferのメンバ登録.
IMPLEMENT_SCENE_UB_STRUCT(FInstantRdvSceneUniformBufferParams, InstantRdvParam, GetDefaultResourceParameters_FInstantRdvSceneUniformBufferParams);
