/*
    InstantRdvSceneUniformBuffer.cpp

    RDVのパラメータやバッファをSceneUniformBufferに追加してマテリアル等からアクセス可能にする.
*/

#include "InstantRdvSceneUniformBuffer.h"
#include "RHIStaticStates.h"
#include "RenderGraphUtils.h"

// SceneUniformBufferの拡張部のデフォルト値定義用関数.
void InitializeInstantRdvSceneUniformBufferDefaults(FInstantRdvSceneUniformBufferParams& ShaderParams, FRDGBuilder& GraphBuilder)
{
    const FRDGTextureRef DummyFloat4Texture = GraphBuilder.CreateTexture(
        FRDGTextureDesc::Create3D(
            FIntVector(1, 1, 1),
            PF_FloatRGBA,
            FClearValueBinding::Black,
            TexCreate_ShaderResource | TexCreate_UAV),
        TEXT("InstantRdv.SceneUniformBuffer.DummyFloat4Texture"));
    const FRDGBufferRef DummyUintBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1u),
        TEXT("InstantRdv.SceneUniformBuffer.DummyUint"));
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyFloat4Texture), FLinearColor::Transparent);
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyUintBuffer), 0u);

    ShaderParams.VspEnabled = 0u;
    ShaderParams.VspGridResolutionX = 0u;
    ShaderParams.VspGridResolutionY = 0u;
    ShaderParams.VspGridResolutionZ = 0u;
    ShaderParams.VspCascadeCount = 0u;
    ShaderParams.VspIrradianceVolumeCellCount = 0u;
    ShaderParams.VspCellSizeCm = 0.0f;
    ShaderParams.VspCascadeDitherInterpolation = 1u;
    ShaderParams.VspTrilinearInterpolation = 1u;
    ShaderParams.VspGridCenterPositionWs = FVector3f::ZeroVector;
    ShaderParams.VspIrradianceVolumeSH = GraphBuilder.CreateSRV(DummyFloat4Texture);
    ShaderParams.VspIrradianceVolumeSampler =
        TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
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
IMPLEMENT_SCENE_UB_STRUCT(FInstantRdvSceneUniformBufferParams, InstantRdvParam, InitializeInstantRdvSceneUniformBufferDefaults);
