/*
    InstantRdvBbv.cpp

    BBVの永続リソース管理、Geometry/Radiance更新、FSP更新、
    および関連するRenderThreadパスの実装を担当する。
*/

#include "InstantRdvBbv.h"
#include "InstantRdvConsoleVariables.h"
#include "InstantRdvSceneUniformBuffer.h"

#include "FXRenderingUtils.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ScreenRendering.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"

namespace
{
static constexpr uint32 kBbvElementUpdateSkipCount = 3;

static constexpr uint32 kFspIrradianceVolumeShFloat4Count = 4;
static constexpr uint32 kFspTraceDistanceCm = 5000;

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvFspWarmStart,
    TEXT("r.InstantRdv.Fsp.WarmStart"),
    1,
    TEXT("新規FSP ActiveProbeのAtlas warm start。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("FSP"),
    TEXT("Warm start"),
    10);

INSTANT_RDV_CVAR_FLOAT(
    CVarInstantRdvFspRelocationOffsetScale,
    TEXT("r.InstantRdv.Fsp.RelocationOffsetScale"),
    0.9f,
    TEXT("ActiveProbe relocationの最大距離をCascade cell sizeに対する比率で指定する。\n")
    TEXT("Native InstantRDV default: 0.9"),
    ECVF_RenderThreadSafe,
    TEXT("FSP"),
    TEXT("Relocation offset scale"),
    0.0f,
    1.5f,
    20);



TGlobalResource<FEmptyVertexDeclaration, FRenderResource::EInitPhase::Pre> GInstantRdvNullVertexDeclaration;

// 移植時の重要注意（RDG/RHI）:
// - 再生成したバッファは QueueBufferExtraction 前に必ず produced 状態へする（Clear など）。
//   produced でない抽出は RDG validation で落ちる。
// - Indirect dispatch は args バッファ生成だけでなく、消費パス側で IndirectArgs access を明示する。
//   片側だけだと実行時 validation で失敗する。

INSTANT_RDV_CVAR_ACTION(
    CVarInstantRdvBbvReset,
    TEXT("r.InstantRdv.Bbv.Reset"),
    0,
    TEXT("Force BBV buffer reinitialization.\n0: Keep persistent BBV state\n1: Clear BBV state this frame"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Reset BBV state"),
    1000);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvRadianceShortRayFallback,
    TEXT("r.InstantRdv.Bbv.RadianceShortRayFallback"),
    1,
    TEXT("Enable Native-equivalent short-ray fallback for BBV Radiance Injection.\n")
    TEXT("0: Discard injection when the start Brick is empty\n")
    TEXT("1: Search up to 1 Brick in 4 steps and inject into the first occupied Brick"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("Radiance short-ray fallback"),
    6);

INSTANT_RDV_CVAR_FLOAT(
    CVarInstantRdvBbvDepthtestInjectionOffsetFineCells,
    TEXT("r.InstantRdv.Bbv.DepthtestInjectionOffsetFineCells"),
    2.0f,
    TEXT("Depthtest Injectionの視線奥オフセット量（fine cell単位）。\n参照実装準拠で、実際の距離は CellSizeCm * (FineCells / k_irdv_bbv_brick_reso) で算出。"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Injection offset (fine cells)"),
    0.0f,
    8.0f,
    220);

INSTANT_RDV_CVAR_INT(
    CVarInstantRdvBbvDepthCullMode,
    TEXT("r.InstantRdv.Bbv.DepthCullMode"),
    1,
    TEXT("BBV Removal Cell Culling candidate mode.\n")
    TEXT("0: Legacy Brick-center XY test\n")
    TEXT("1: Conservative world-space Brick AABB vs six frustum planes"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Removal Cell Culling mode"),
    0.0f,
    1.0f,
    200);

INSTANT_RDV_CVAR_INT(
    CVarInstantRdvBbvDepthInjectionMethod,
    TEXT("r.InstantRdv.Bbv.DepthInjectionMethod"),
    1,
    TEXT("Depth Injectionの表面->Near方向計算方式。\n")
    TEXT("0: UE現行方式（NDC z=0/1の距離比較）\n")
    TEXT("1: Native方式（ProjectionのNear Plane深度を使用）"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Depth Injection method"),
    0.0f,
    1.0f,
    210);

INSTANT_RDV_CVAR_FLOAT(
    CVarInstantRdvBbvDepthRelationRangeFineCells,
    TEXT("r.InstantRdv.Bbv.DepthRelationRangeFineCells"),
    8.0f,
    TEXT("BBV Depth Relation debugの表示範囲（±fine cell数）。"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("BBV depth relation range (fine cells)"),
    0.0f,
    32.0f,
    4);
// 移植ミス再発防止:
// - 参照実装は「固定m値」ではなく fine cell 基準でオフセット量を決める。
// - UE側はワールド単位がcmのため、シェーダへ渡す前に必ず CellSizeCm/k_irdv_bbv_brick_reso で換算する。
// - CVarの意味は「ワールド距離」ではなく「fine cell数」を維持すること。

INSTANT_RDV_CVAR_FLOAT(
    CVarInstantRdvFspDebugProbeRadiusCm,
    TEXT("r.InstantRdv.Fsp.DebugProbeRadiusCm"),
    10.0f,
    TEXT("FSP probe sphere debug radius in centimeters."),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("Probe radius (cm)"),
    0.0f,
    100.0f,
    40);

class FInstantRdvBbvBeginUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvBeginUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvBeginUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BitmaskElementCount)
        SHADER_PARAMETER(uint32, BrickDataElementCount)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
        SHADER_PARAMETER(uint32, OptionalDataElementCount)
        SHADER_PARAMETER(uint32, DispatchLinearWidth)
        SHADER_PARAMETER(uint32, DispatchThreadCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxelOptionData)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvBeginViewUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvBeginViewUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvBeginViewUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFrustumBrickCounter)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDepthInjectionCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDepthInjectionCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDepthInjectionCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, DepthSizeX)
        SHADER_PARAMETER(uint32, DepthSizeY)
        SHADER_PARAMETER(uint32, ViewRectMinX)
        SHADER_PARAMETER(uint32, ViewRectMinY)
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(float, DepthtestInjectionWorldOffsetWs)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(float, NativeNearPlaneDeviceDepth)
        SHADER_PARAMETER(uint32, DepthInjectionMethod)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvReducedSurfaceBufferBuildCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvReducedSurfaceBufferBuildCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvReducedSurfaceBufferBuildCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, SourceDepthSizeX)
        SHADER_PARAMETER(uint32, SourceDepthSizeY)
        SHADER_PARAMETER(uint32, SourceViewRectMinX)
        SHADER_PARAMETER(uint32, SourceViewRectMinY)
        SHADER_PARAMETER(uint32, SourceViewRectSizeX)
        SHADER_PARAMETER(uint32, SourceViewRectSizeY)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, InvProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWReducedSurfaceBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvReducedSurfaceInjectionCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvReducedSurfaceInjectionCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvReducedSurfaceInjectionCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, SourceDepthSizeX)
        SHADER_PARAMETER(uint32, SourceDepthSizeY)
        SHADER_PARAMETER(uint32, SourceViewRectMinX)
        SHADER_PARAMETER(uint32, SourceViewRectMinY)
        SHADER_PARAMETER(uint32, SourceViewRectSizeX)
        SHADER_PARAMETER(uint32, SourceViewRectSizeY)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(float, InjectionWorldOffsetWs)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, InvProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ReducedSurfaceBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvReducedRadianceInjectionCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvReducedRadianceInjectionCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvReducedRadianceInjectionCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, SourceDepthSizeX)
        SHADER_PARAMETER(uint32, SourceDepthSizeY)
        SHADER_PARAMETER(uint32, SourceViewRectMinX)
        SHADER_PARAMETER(uint32, SourceViewRectMinY)
        SHADER_PARAMETER(uint32, SourceViewRectSizeX)
        SHADER_PARAMETER(uint32, SourceViewRectSizeY)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(float, InjectionWorldOffsetWs)
        SHADER_PARAMETER(float, SceneColorPreExposure)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, InvProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
        SHADER_PARAMETER(uint32, bEnableShortRayFallback)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ReducedSurfaceBuffer)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvRadianceAccumBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDepthFrustumCullCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDepthFrustumCullCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDepthFrustumCullCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFrustumBrickCounter)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFrustumBrickList)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDepthFrustumCullAabbCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDepthFrustumCullAabbCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDepthFrustumCullAabbCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(float, NearPlaneDeviceDepth)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFrustumBrickCounter)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFrustumBrickList)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDepthCarvingIndirectArgBuildCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDepthCarvingIndirectArgBuildCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDepthCarvingIndirectArgBuildCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FrustumBrickCounter)
        SHADER_PARAMETER(uint32, ThreadGroupSizeX)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWFrustumBrickIndirectArg)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDepthCarvingCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDepthCarvingCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDepthCarvingCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, DepthSizeX)
        SHADER_PARAMETER(uint32, DepthSizeY)
        SHADER_PARAMETER(uint32, ViewRectMinX)
        SHADER_PARAMETER(uint32, ViewRectMinY)
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FrustumBrickCounter)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FrustumBrickList)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvBuffer)
        // Indirect dispatch では消費側パラメータに IndirectArgs access 宣言が必須。
        RDG_BUFFER_ACCESS(FrustumBrickIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvToroidalClearCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvToroidalClearCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvToroidalClearCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FIntVector4, GridMoveDeltaCells)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxelOptionData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvRadianceAccumBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvBrickCountAggregateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvBrickCountAggregateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvBrickCountAggregateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvBuffer)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvElementUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvElementUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvElementUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, UpdateSkipCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxelOptionData)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvRadianceInjectionCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvRadianceInjectionCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvRadianceInjectionCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, DepthSizeX)
        SHADER_PARAMETER(uint32, DepthSizeY)
        SHADER_PARAMETER(uint32, ViewRectMinX)
        SHADER_PARAMETER(uint32, ViewRectMinY)
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(float, DepthtestInjectionWorldOffsetWs)
        SHADER_PARAMETER(float, SceneColorPreExposure)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
        SHADER_PARAMETER(uint32, bEnableShortRayFallback)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvRadianceAccumBuffer)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvRadianceResolveCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvRadianceResolveCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvRadianceResolveCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvRadianceAccumBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxelOptionData)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspSurfaceMaskInjectCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspSurfaceMaskInjectCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspSurfaceMaskInjectCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, DepthSizeX)
        SHADER_PARAMETER(uint32, DepthSizeY)
        SHADER_PARAMETER(uint32, ViewRectMinX)
        SHADER_PARAMETER(uint32, ViewRectMinY)
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspSurfaceCellMask)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspSurfaceMaskCompactCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspSurfaceMaskCompactCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspSurfaceMaskCompactCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspTotalCellCount)
        SHADER_PARAMETER(uint32, FspVisibleSurfaceListCapacity)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspSurfaceCellMask)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspVisibleSurfaceList)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspSurfaceDetectReducedCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspSurfaceDetectReducedCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspSurfaceDetectReducedCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, SourceDepthSizeX)
        SHADER_PARAMETER(uint32, SourceDepthSizeY)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, InvProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(uint32, VisibleSurfaceListCapacity)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ReducedSurfaceBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspSurfaceCellMask)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspVisibleSurfaceList)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspVisibleSurfaceSourceTexelList)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspInitPoolCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspInitPoolCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspInitPoolCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, ProbePoolElementCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeFreeStack)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspCellProbeIndex)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWFspIrradianceVolumeSH)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspCounterIndirectArgBuildCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspCounterIndirectArgBuildCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspCounterIndirectArgBuildCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, ThreadGroupSizeX)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CounterBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArg)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspBeginUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspBeginUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspBeginUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(FVector3f, FspPrevGridCenterPositionWs)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspActiveProbeListPrev)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspActiveProbeListCurr)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeFreeStack)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspCellProbeIndex)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspVisibleSurfaceList)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeRayRequestBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeRayResultBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWFspIrradianceVolumeSH)
        RDG_BUFFER_ACCESS(FspPrevActiveProbeIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspPreUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspPreUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspPreUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, bEnableWarmStart)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(float, FspRelocationOffsetScale)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(uint32, bUseReducedSurfaceBuffer)
        SHADER_PARAMETER(uint32, SourceViewRectSizeX)
        SHADER_PARAMETER(uint32, SourceViewRectSizeY)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewMatrix)
        SHADER_PARAMETER(FMatrix44f, InvProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
        SHADER_PARAMETER(uint32, BbvGridResolutionX)
        SHADER_PARAMETER(uint32, BbvGridResolutionY)
        SHADER_PARAMETER(uint32, BbvGridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, BbvCellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspVisibleSurfaceList)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspVisibleSurfaceSourceTexelList)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ReducedSurfaceBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspActiveProbeListCurr)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeFreeStack)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspCellProbeIndex)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWFspProbeAtlas)
        RDG_BUFFER_ACCESS(FspPreUpdateIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspProbeRayRequestCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspProbeRayRequestCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspProbeRayRequestCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspActiveProbeListCurr)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeRayRequestBuffer)
        RDG_BUFFER_ACCESS(FspActiveProbeIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspProbeRayTraceCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspProbeRayTraceCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspProbeRayTraceCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, bUseProbeTraceOffset)
        SHADER_PARAMETER(uint32, BbvGridResolutionX)
        SHADER_PARAMETER(uint32, BbvGridResolutionY)
        SHADER_PARAMETER(uint32, BbvGridResolutionZ)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(float, FspRelocationOffsetScale)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, BbvCellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(float, FspTraceDistanceCm)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbeRayRequestBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeRayResultBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
        RDG_BUFFER_ACCESS(FspTraceIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspProbeRayResolveCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspProbeRayResolveCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspProbeRayResolveCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbeRayResultBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvOptionalData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWFspProbeAtlas)
        RDG_BUFFER_ACCESS(FspResolveIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspShUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspShUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspShUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspActiveProbeListCurr)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FspProbeAtlas)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWFspPackedSH)
        RDG_BUFFER_ACCESS(FspActiveProbeIndirectArgBuffer, ERHIAccess::IndirectArgs)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspIrradianceVolumePropagateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspIrradianceVolumePropagateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspIrradianceVolumePropagateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspCellProbeIndex)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWFspPackedSH)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDebugVisualizePS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDebugVisualizePS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDebugVisualizePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(float, SceneColorPreExposure)
        SHADER_PARAMETER(float, SceneColorBlendRate)
        SHADER_PARAMETER(uint32, bDepthTest)
        SHADER_PARAMETER(uint32, DepthSizeX)
        SHADER_PARAMETER(uint32, DepthSizeY)
        SHADER_PARAMETER(uint32, ViewRectMinX)
        SHADER_PARAMETER(uint32, ViewRectMinY)
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, MaxTraceDistanceCm)
        SHADER_PARAMETER(float, DepthRelationRangeFineCells)
        SHADER_PARAMETER(int32, DebugMode)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxelOptionData)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspProbeBillboardVS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspProbeBillboardVS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspProbeBillboardVS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FVector3f, CameraRightWs)
        SHADER_PARAMETER(FVector3f, CameraUpWs)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(float, FspRelocationOffsetScale)
        SHADER_PARAMETER(float, DebugProbeRadiusCm)
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, DebugMode)
        SHADER_PARAMETER(uint32, bUseProbeVisualizationOffset)
        SHADER_PARAMETER(uint32, bUseProbeTraceOffset)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspCellProbeIndex)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FspProbeAtlas)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FspPackedSH)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspProbeBillboardPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspProbeBillboardPS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspProbeBillboardPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RENDER_TARGET_BINDING_SLOTS()
        SHADER_PARAMETER(FVector3f, CameraUpWs)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCascadeCount)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, DebugMode)
        SHADER_PARAMETER(uint32, bUseProbeTraceOffset)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(float, FspRelocationOffsetScale)
        SHADER_PARAMETER(FVector3f, FspGridCenterPositionWs)
        SHADER_PARAMETER(uint32, BbvGridResolutionX)
        SHADER_PARAMETER(uint32, BbvGridResolutionY)
        SHADER_PARAMETER(uint32, BbvGridResolutionZ)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(float, BbvCellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbePool)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FspProbeAtlas)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FspPackedSH)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvBuffer)
        SHADER_PARAMETER(uint32, BrickDataBaseOffset)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspDebugTextPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspDebugTextPS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspDebugTextPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, FspProbePoolElementCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspVisibleSurfaceList)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspActiveProbeListCurr)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbeRayRequestBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbeRayResultBuffer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspProbeFreeStack)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};

BEGIN_SHADER_PARAMETER_STRUCT(FInstantRdvFspProbeBillboard_Parameters, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstantRdvFspProbeBillboardVS::FParameters, VS)
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstantRdvFspProbeBillboardPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()




IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvBeginUpdateCS, "/InstantRdvShaders/Private/Bbv/bbv_begin_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvBeginViewUpdateCS, "/InstantRdvShaders/Private/Bbv/bbv_begin_view_update_cs.usf", "MainCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthInjectionCS, "/InstantRdvShaders/Private/Bbv/bbv_depthtest_injection_apply_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvReducedSurfaceBufferBuildCS, "/InstantRdvShaders/Private/Bbv/reduced_surface_buffer_build_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvReducedSurfaceInjectionCS, "/InstantRdvShaders/Private/Bbv/bbv_reduced_surface_injection_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthFrustumCullCS, "/InstantRdvShaders/Private/Bbv/bbv_depthtest_frustum_cull_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthFrustumCullAabbCS, "/InstantRdvShaders/Private/Bbv/bbv_depthtest_frustum_cull_aabb_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthCarvingIndirectArgBuildCS, "/InstantRdvShaders/Private/Bbv/bbv_depthtest_carving_indirect_arg_build_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthCarvingCS, "/InstantRdvShaders/Private/Bbv/bbv_depthtest_carving_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvToroidalClearCS, "/InstantRdvShaders/Private/Bbv/bbv_toroidal_clear_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvBrickCountAggregateCS, "/InstantRdvShaders/Private/Bbv/bbv_brick_count_aggregate_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvElementUpdateCS, "/InstantRdvShaders/Private/Bbv/bbv_element_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvRadianceInjectionCS, "/InstantRdvShaders/Private/Bbv/bbv_radiance_injection_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvReducedRadianceInjectionCS, "/InstantRdvShaders/Private/Bbv/bbv_reduced_radiance_injection_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvRadianceResolveCS, "/InstantRdvShaders/Private/Bbv/bbv_radiance_resolve_cs.usf", "MainCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspSurfaceMaskInjectCS, "/InstantRdvShaders/Private/Fsp/fsp_surface_mask_inject_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspSurfaceMaskCompactCS, "/InstantRdvShaders/Private/Fsp/fsp_surface_mask_compact_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspSurfaceDetectReducedCS, "/InstantRdvShaders/Private/Fsp/fsp_surface_detect_reduced_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspInitPoolCS, "/InstantRdvShaders/Private/Fsp/fsp_init_pool_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspCounterIndirectArgBuildCS, "/InstantRdvShaders/Private/Fsp/fsp_counter_indirect_arg_build_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspBeginUpdateCS, "/InstantRdvShaders/Private/Fsp/fsp_begin_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspPreUpdateCS, "/InstantRdvShaders/Private/Fsp/fsp_pre_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeRayRequestCS, "/InstantRdvShaders/Private/Fsp/fsp_probe_ray_request_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeRayTraceCS, "/InstantRdvShaders/Private/Fsp/fsp_probe_ray_trace_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeRayResolveCS, "/InstantRdvShaders/Private/Fsp/fsp_probe_ray_resolve_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspShUpdateCS, "/InstantRdvShaders/Private/Fsp/fsp_sh_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspIrradianceVolumePropagateCS, "/InstantRdvShaders/Private/Fsp/fsp_irradiance_volume_propagate_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDebugVisualizePS, "/InstantRdvShaders/Private/Bbv/bbv_debug_visualize_ps.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeBillboardVS, "/InstantRdvShaders/Private/Fsp/fsp_probe_billboard.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeBillboardPS, "/InstantRdvShaders/Private/Fsp/fsp_probe_billboard.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspDebugTextPS, "/InstantRdvShaders/Private/Fsp/fsp_debug_text_ps.usf", "MainPS", SF_Pixel);
} // namespace



uint32 FInstantRdvBbvConfig::GetBbvBrickCount() const
{
    return static_cast<uint32>(BbvGridResolution.X) * static_cast<uint32>(BbvGridResolution.Y) * static_cast<uint32>(BbvGridResolution.Z);
}

uint32 FInstantRdvBbvConfig::GetBitmaskElementCount() const
{
    return GetBbvBrickCount() * k_irdv_bbv_bitmask_u32_count_per_brick;
}

uint32 FInstantRdvBbvConfig::GetBrickDataElementCount() const
{
    return GetBbvBrickCount() * k_irdv_bbv_brick_data_u32_count;
}

uint32 FInstantRdvBbvConfig::GetBbvBufferElementCount() const
{
    return GetBitmaskElementCount() + GetBrickDataElementCount();
}

uint32 FInstantRdvBbvConfig::GetOptionalDataElementCount() const
{
    return GetBbvBrickCount() * k_irdv_bbv_brick_optional_data_u32_count;
}
uint32 FInstantRdvBbvConfig::GetRadianceAccumDataElementCount() const
{
    return GetBbvBrickCount() * k_irdv_bbv_radiance_accum_component_count;
}



uint32 FInstantRdvFspConfig::GetFspCellCount() const
{
    return static_cast<uint32>(ProbeGridResolution.X * ProbeGridResolution.Y * ProbeGridResolution.Z);
}

uint32 FInstantRdvFspConfig::GetFspTotalCellCount() const
{
    return GetFspCellCount() * FMath::Max(ProbeCascadeCount, 1u);
}


void FInstantRdvBbv::Initialize()
{
    {
        SystemState = {};
    }
}

void FInstantRdvBbv::BeginFrame_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView)
{
    // TODO.
    // 各種システムのRenderでのBeginUpdate処理を集約する.
    // リソースの確保/再確保等も可能な限りここへ.


    if (!SystemState.bRenderInitialized)
    {
        // 初期化済み.
        SystemState.bRenderInitialized = true;
        // フレームクリア.
        SystemState.FrameCount = 0;

        {
            auto SetupToroidalGrid = [](FToroidalGrid& out_tr_grid, const FIntVector& resolution)
            {
                    out_tr_grid = {};
                    out_tr_grid.GridReso = resolution;
            };

            SetupToroidalGrid(SystemState.bbv.TrGrid, Config.bbv.BbvGridResolution);
            SetupToroidalGrid(SystemState.fsp.TrGrid, Config.fsp.ProbeGridResolution);
        }

        // Bbv
        {
            // 必要なリソースをRDGから生成. 寿命はフレームを超えるので, Extractionして管理責任を受け取る.
            const uint32 OptionalDataElementCount = Config.bbv.GetOptionalDataElementCount();
            const uint32 RadianceAccumElementCount = Config.bbv.GetRadianceAccumDataElementCount();


            // RDGPool上に確保. この時点ではまだフレーム(RDG)寿命のリソース.
            SystemState.bbv.BbvBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Config.bbv.GetBbvBufferElementCount()), TEXT("InstantRdv.BbvBuffer"));
            SystemState.bbv.OptionalDataBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), OptionalDataElementCount), TEXT("InstantRdv.BbvOptionalDataBuffer"));
            SystemState.bbv.RadianceAccumBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RadianceAccumElementCount), TEXT("InstantRdv.BbvRadianceAccumBuffer"));

            // 再生成直後の pooled buffer は produced 扱いではないため、
            // extraction 前に明示的に書き込み（clear）して RDG validation をパスさせる  本当に必要か?要確認.
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.OptionalDataBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.RadianceAccumBuffer.Handle), 0u);

            // Extractionして永続化.
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.BbvBuffer.Handle, &SystemState.bbv.BbvBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.OptionalDataBuffer.Handle, &SystemState.bbv.OptionalDataBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.RadianceAccumBuffer.Handle, &SystemState.bbv.RadianceAccumBuffer.PooledBuffer);
        }

        // Fsp
        {
            const uint32 FspCellCount = Config.fsp.GetFspTotalCellCount();
            const uint32 FspRayWorkCount = FspCellCount * (k_irdv_fsp_probe_octmap_width * k_irdv_fsp_probe_octmap_width);


            SystemState.fsp.FspCellProbeIndexBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount), TEXT("InstantRdv.fsp.FspCellProbeIndexBuffer"));
            SystemState.fsp.FspVisibleSurfaceListBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1), TEXT("InstantRdv.fsp.FspVisibleSurfaceListBuffer"));
            SystemState.fsp.FspProbePoolBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount * k_irdv_fsp_probe_pool_data_u32_count), TEXT("InstantRdv.fsp.FspProbePoolBuffer"));
            SystemState.fsp.FspProbeFreeStackBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.fsp.FspProbeFreeStackBuffer"));
            // 参照InstantRDVと同じくActiveProbeListは2本だけ確保し、毎フレームcurr/prev indexを入れ替える。
            // 固定Prev/Currへ末尾コピーすると不要な全ActiveProbeコピーpassが発生するため避ける。
            SystemState.fsp.FspActiveProbeListBuffers[0].Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.FspActiveProbeList0"));
            SystemState.fsp.FspActiveProbeListBuffers[1].Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.FspActiveProbeList1"));
            SystemState.fsp.FspProbeAtlasBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 4, FspRayWorkCount), TEXT("InstantRdv.fsp.FspProbeAtlasBuffer"));
            SystemState.fsp.FspProbeRayRequestBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspRayWorkCount + 1u), TEXT("InstantRdv.fsp.FspProbeRayRequestBuffer"));
            SystemState.fsp.FspProbeRayResultBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspRayWorkCount * k_irdv_fsp_ray_result_data_stride + 1u), TEXT("InstantRdv.fsp.FspProbeRayResultBuffer"));
            SystemState.fsp.FspPackedSHBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float) * 4, FspCellCount * kFspIrradianceVolumeShFloat4Count), TEXT("InstantRdv.fsp.FspPackedSHBuffer"));
            SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.fsp.FspVisibleSurfaceSourceTexelListBuffer"));

            const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(InView);
            const FIntPoint ReducedExtent(
                FMath::Max(FMath::DivideAndRoundUp(ViewRect.Width(), 4), 1),
                FMath::Max(FMath::DivideAndRoundUp(ViewRect.Height(), 4), 1));
            SystemState.fsp.ReducedSurfaceBuffer.Extent = ReducedExtent;
            SystemState.fsp.ReducedSurfaceBuffer.Handle = GraphBuilder.CreateTexture(
                FRDGTextureDesc::Create2D(
                    ReducedExtent,
                    PF_A32B32G32R32F,
                    FClearValueBinding::None,
                    TexCreate_ShaderResource | TexCreate_UAV),
                TEXT("InstantRdv.fsp.ReducedSurfaceBuffer"));
            // Legacy経路でも初回フレームの永続化対象になるため、未生成リソースにならないよう明示的に初期化する。
            AddClearUAVPass(
                GraphBuilder,
                GraphBuilder.CreateUAV(SystemState.fsp.ReducedSurfaceBuffer.Handle),
                FLinearColor::Transparent);


            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspCellProbeIndexBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbePoolBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeFreeStackBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspActiveProbeListBuffers[0].Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspActiveProbeListBuffers[1].Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeAtlasBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayRequestBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayResultBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle), 0u);


            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspCellProbeIndexBuffer.Handle, &SystemState.fsp.FspCellProbeIndexBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle, &SystemState.fsp.FspVisibleSurfaceListBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.Handle, &SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbePoolBuffer.Handle, &SystemState.fsp.FspProbePoolBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbeFreeStackBuffer.Handle, &SystemState.fsp.FspProbeFreeStackBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspActiveProbeListBuffers[0].Handle, &SystemState.fsp.FspActiveProbeListBuffers[0].PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspActiveProbeListBuffers[1].Handle, &SystemState.fsp.FspActiveProbeListBuffers[1].PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbeAtlasBuffer.Handle, &SystemState.fsp.FspProbeAtlasBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbeRayRequestBuffer.Handle, &SystemState.fsp.FspProbeRayRequestBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbeRayResultBuffer.Handle, &SystemState.fsp.FspProbeRayResultBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspPackedSHBuffer.Handle, &SystemState.fsp.FspPackedSHBuffer.PooledBuffer);
            GraphBuilder.QueueTextureExtraction(
                SystemState.fsp.ReducedSurfaceBuffer.Handle,
                &SystemState.fsp.ReducedSurfaceBuffer.PooledTexture);
        }
    }
    else
    {
        // フレーム加算.
        ++SystemState.FrameCount;

        // Pool されているリソースをこのフレーム用にRDGにRegisterしてハンドル更新.
        
        // Bbv
        {
            SystemState.bbv.BbvBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.BbvBuffer.PooledBuffer, TEXT("InstantRdv.BbvBuffer"));
            SystemState.bbv.OptionalDataBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.OptionalDataBuffer.PooledBuffer, TEXT("InstantRdv.BbvOptionalDataBuffer"));
            SystemState.bbv.RadianceAccumBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.RadianceAccumBuffer.PooledBuffer, TEXT("InstantRdv.BbvRadianceAccumBuffer"));
        }

        // Fsp
        {
            SystemState.fsp.FspCellProbeIndexBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspCellProbeIndexBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspCellProbeIndexBuffer"));
            SystemState.fsp.FspVisibleSurfaceListBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspVisibleSurfaceListBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspVisibleSurfaceListBuffer"));
            SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspVisibleSurfaceSourceTexelListBuffer"));
            SystemState.fsp.FspProbePoolBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbePoolBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbePoolBuffer"));
            SystemState.fsp.FspProbeFreeStackBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbeFreeStackBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbeFreeStackBuffer"));
            SystemState.fsp.FspActiveProbeListBuffers[0].Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspActiveProbeListBuffers[0].PooledBuffer, TEXT("InstantRdv.FspActiveProbeList0"));
            SystemState.fsp.FspActiveProbeListBuffers[1].Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspActiveProbeListBuffers[1].PooledBuffer, TEXT("InstantRdv.FspActiveProbeList1"));
            SystemState.fsp.FspProbeAtlasBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbeAtlasBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbeAtlasBuffer"));
            SystemState.fsp.FspProbeRayRequestBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbeRayRequestBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbeRayRequestBuffer"));
            SystemState.fsp.FspProbeRayResultBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbeRayResultBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbeRayResultBuffer"));
            SystemState.fsp.FspPackedSHBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspPackedSHBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspPackedSHBuffer"));
            const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(InView);
            const FIntPoint ReducedExtent(
                FMath::Max(FMath::DivideAndRoundUp(ViewRect.Width(), 4), 1),
                FMath::Max(FMath::DivideAndRoundUp(ViewRect.Height(), 4), 1));
            if (SystemState.fsp.ReducedSurfaceBuffer.PooledTexture.IsValid() &&
                SystemState.fsp.ReducedSurfaceBuffer.Extent == ReducedExtent)
            {
                SystemState.fsp.ReducedSurfaceBuffer.Handle = GraphBuilder.RegisterExternalTexture(
                    SystemState.fsp.ReducedSurfaceBuffer.PooledTexture,
                    TEXT("InstantRdv.fsp.ReducedSurfaceBuffer"));
            }
        }
    }


    // グリッド更新.
    {
        SystemState.bbv.TrGrid.UpdateDelta(InView.ViewLocation, Config.bbv.BbvBrickSizeCm);
        SystemState.fsp.TrGrid.UpdateDelta(InView.ViewLocation, Config.fsp.ProbeCellSizeCm);
        // FSP shader/debugのgrid centerは、RDV更新ownerとして受理されたViewの位置に固定する。
        // Debugを別View callbackから読む場合も同じcenterを使い、表示だけがViewごとにずれないようにする。
        SystemState.fsp.CurrentGridCenterPositionWs = InView.ViewLocation;
    }


}

void FInstantRdvBbv::FillSceneUniformBufferParams_RenderThread(
    FRDGBuilder& GraphBuilder,
    FInstantRdvSceneUniformBufferParams& OutParams,
    bool bUseLiveResources)
{
    const FRDGBufferRef DummyFloat4Buffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float) * 4u, 1u),
        TEXT("InstantRdv.SceneUniformBuffer.DummyFloat4"));
    const FRDGBufferRef DummyUintBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1u),
        TEXT("InstantRdv.SceneUniformBuffer.DummyUint"));
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyFloat4Buffer), 0u);
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyUintBuffer), 0u);

    OutParams.FspIrradianceVolumeSH = GraphBuilder.CreateSRV(DummyFloat4Buffer);
    OutParams.BbvBuffer = GraphBuilder.CreateSRV(DummyUintBuffer);
    OutParams.BbvBrickDataBaseOffset = 0u;
    OutParams.BbvRadianceAccum = GraphBuilder.CreateSRV(DummyUintBuffer);

    const FIntVector& Resolution = SystemState.fsp.TrGrid.GridReso;
    const uint32 CellCount = Config.fsp.GetFspTotalCellCount();

    OutParams.FspEnabled = bUseLiveResources && SystemState.bRenderInitialized ? 1u : 0u;
    OutParams.FspGridResolutionX = static_cast<uint32>(FMath::Max(Resolution.X, 0));
    OutParams.FspGridResolutionY = static_cast<uint32>(FMath::Max(Resolution.Y, 0));
    OutParams.FspGridResolutionZ = static_cast<uint32>(FMath::Max(Resolution.Z, 0));
    OutParams.FspCascadeCount = Config.fsp.ProbeCascadeCount;
    OutParams.FspIrradianceVolumeCellCount = CellCount;
    OutParams.FspIrradianceVolumeSHFloat4Count = kFspIrradianceVolumeShFloat4Count;
    OutParams.FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
    OutParams.FspGridCenterPositionWs = FVector3f(SystemState.fsp.CurrentGridCenterPositionWs);
    OutParams.FspIrradianceVolumeSH = bUseLiveResources && SystemState.fsp.FspPackedSHBuffer.Handle != nullptr
        ? GraphBuilder.CreateSRV(SystemState.fsp.FspPackedSHBuffer.Handle)
        : OutParams.FspIrradianceVolumeSH;

    const FIntVector& BbvResolution = SystemState.bbv.TrGrid.GridReso;
    OutParams.BbvEnabled = bUseLiveResources && SystemState.bRenderInitialized ? 1u : 0u;
    OutParams.BbvGridResolutionX = static_cast<uint32>(FMath::Max(BbvResolution.X, 0));
    OutParams.BbvGridResolutionY = static_cast<uint32>(FMath::Max(BbvResolution.Y, 0));
    OutParams.BbvGridResolutionZ = static_cast<uint32>(FMath::Max(BbvResolution.Z, 0));
    OutParams.BbvBrickResolution = k_irdv_bbv_brick_reso;
    OutParams.BbvBrickSizeCm = Config.bbv.BbvBrickSizeCm;
    OutParams.BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
    OutParams.BbvToroidalOffsetCells = FVector3f(
        static_cast<float>(SystemState.bbv.TrGrid.ToroidalOffsetCells.X),
        static_cast<float>(SystemState.bbv.TrGrid.ToroidalOffsetCells.Y),
        static_cast<float>(SystemState.bbv.TrGrid.ToroidalOffsetCells.Z));
    OutParams.BbvBuffer = bUseLiveResources && SystemState.bbv.BbvBuffer.Handle != nullptr
        ? GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle)
        : OutParams.BbvBuffer;
    OutParams.BbvBrickDataBaseOffset = Config.bbv.GetBitmaskElementCount();
    OutParams.BbvRadianceAccum = bUseLiveResources && SystemState.bbv.RadianceAccumBuffer.Handle != nullptr
        ? GraphBuilder.CreateSRV(SystemState.bbv.RadianceAccumBuffer.Handle)
        : OutParams.BbvRadianceAccum;
}

void FInstantRdvBbv::ExecuteGeometryUpdate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    bool bEnableMainViewGeometryInjection,
    bool bEnableMainViewGeometryRemoval,
    bool bUseReducedSurfaceBuffer)
{
    if (SceneDepthTexture == nullptr)
    {
        return;
    }

    const uint32 BrickCount = SystemState.bbv.TrGrid.GetCellCount();
    const uint32 BitmaskElementCount = Config.bbv.GetBitmaskElementCount();
    const uint32 BrickDataElementCount = Config.bbv.GetBrickDataElementCount();
    const uint32 OptionalDataElementCount = Config.bbv.GetOptionalDataElementCount();
    const uint32 RadianceAccumElementCount = Config.bbv.GetRadianceAccumDataElementCount();


    FRDGBufferRef FrustumBrickCounterBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("InstantRdv.BbvFrustumBrickCounter"));
    FRDGBufferRef FrustumBrickListBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BrickCount), TEXT("InstantRdv.BbvFrustumBrickList"));
    FRDGBufferRef FrustumBrickIndirectArgBuffer = nullptr;

    const bool bForceReset = (CVarInstantRdvBbvReset.GetValueOnRenderThread() != 0);
    // 初回フレームか, 強制された場合.
    const bool bNeedInitialize = (0 == SystemState.FrameCount) || bForceReset;
    if (bNeedInitialize)
    {
        {
            const uint32 TotalElementCount = FMath::Max(FMath::Max(BitmaskElementCount, BrickDataElementCount), OptionalDataElementCount);
            const uint32 ThreadGroupCount = FMath::DivideAndRoundUp(TotalElementCount, 64u);
            const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(static_cast<int32>(ThreadGroupCount));

            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.RadianceAccumBuffer.Handle), 0u);

            FInstantRdvBbvBeginUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvBeginUpdateCS::FParameters>();
            {
                Parameters->BitmaskElementCount = BitmaskElementCount;
                Parameters->BrickDataElementCount = BrickDataElementCount;
                Parameters->BrickDataBaseOffset = BitmaskElementCount;
                Parameters->OptionalDataElementCount = OptionalDataElementCount;
                Parameters->RWBbvBuffer = GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle);
                Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(SystemState.bbv.OptionalDataBuffer.Handle);
                Parameters->DispatchLinearWidth = FComputeShaderUtils::WrappedGroupStride * 64u;
                Parameters->DispatchThreadCount = Parameters->DispatchLinearWidth * GroupCount.Y * GroupCount.Z;
            }
            TShaderMapRef<FInstantRdvBbvBeginUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvBeginUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, GroupCount);
        }
    }

    {
        FInstantRdvBbvBeginViewUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvBeginViewUpdateCS::FParameters>();
        {
            Parameters->RWFrustumBrickCounter = GraphBuilder.CreateUAV(FrustumBrickCounterBuffer);
        }
        TShaderMapRef<FInstantRdvBbvBeginViewUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvBeginViewUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(1, 1, 1));
    }

    if (!SystemState.bbv.TrGrid.FrameCellDelta.IsZero())
    {
        FInstantRdvBbvToroidalClearCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvToroidalClearCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->GridMoveDeltaCells = FIntVector4(SystemState.bbv.TrGrid.FrameCellDelta, 0);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->RWBbvBuffer = GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(SystemState.bbv.OptionalDataBuffer.Handle);
            Parameters->RWBbvRadianceAccumBuffer = GraphBuilder.CreateUAV(SystemState.bbv.RadianceAccumBuffer.Handle);
        }
        TShaderMapRef<FInstantRdvBbvToroidalClearCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvToroidalClear"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    const bool bUseReducedPath =
        bUseReducedSurfaceBuffer &&
        SystemState.fsp.ReducedSurfaceBuffer.Handle != nullptr;
    const FIntRect MainViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
    if (bUseReducedPath)
    {
        FInstantRdvReducedSurfaceBufferBuildCS::FParameters* Parameters =
            GraphBuilder.AllocParameters<FInstantRdvReducedSurfaceBufferBuildCS::FParameters>();
        Parameters->SourceDepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
        Parameters->SourceDepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
        Parameters->SourceViewRectMinX = static_cast<uint32>(FMath::Max(MainViewRect.Min.X, 0));
        Parameters->SourceViewRectMinY = static_cast<uint32>(FMath::Max(MainViewRect.Min.Y, 0));
        Parameters->SourceViewRectSizeX = static_cast<uint32>(FMath::Max(MainViewRect.Width(), 1));
        Parameters->SourceViewRectSizeY = static_cast<uint32>(FMath::Max(MainViewRect.Height(), 1));
        Parameters->FrameCount = SystemState.FrameCount;
        Parameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetTranslatedViewMatrix());
        Parameters->InvViewMatrix = FMatrix44f(View.ViewMatrices.GetWorldToView().InverseFast());
        Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
        Parameters->InvProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewToClip().InverseFast());
        Parameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewToClip());
        Parameters->SceneDepthTexture = SceneDepthTexture;
        Parameters->RWReducedSurfaceBuffer = GraphBuilder.CreateUAV(SystemState.fsp.ReducedSurfaceBuffer.Handle);

        TShaderMapRef<FInstantRdvReducedSurfaceBufferBuildCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.X), 8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.Y), 8u);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("InstantRdv.ReducedSurfaceBufferBuild"),
            ERDGPassFlags::Compute,
            ComputeShader,
            Parameters,
            FIntVector(GroupX, GroupY, 1));
    }

    if (bEnableMainViewGeometryInjection)
    {
        const FIntRect ViewRect = MainViewRect;
        const uint32 ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
        const uint32 ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
        if (bUseReducedPath)
        {
            FInstantRdvBbvReducedSurfaceInjectionCS::FParameters* Parameters =
                GraphBuilder.AllocParameters<FInstantRdvBbvReducedSurfaceInjectionCS::FParameters>();
            Parameters->SourceDepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->SourceDepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->SourceViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->SourceViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->SourceViewRectSizeX = ViewRectSizeX;
            Parameters->SourceViewRectSizeY = ViewRectSizeY;
            Parameters->FrameCount = SystemState.FrameCount;
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            const float InjectionOffsetFineCells =
                CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            Parameters->InjectionWorldOffsetWs =
                Config.bbv.BbvBrickSizeCm /
                FMath::Max(static_cast<float>(k_irdv_bbv_brick_reso), 1.0f) *
                InjectionOffsetFineCells;
            Parameters->ViewMatrix =
                FMatrix44f(View.ViewMatrices.GetTranslatedViewMatrix());
            Parameters->InvViewMatrix =
                FMatrix44f(View.ViewMatrices.GetWorldToView().InverseFast());
            Parameters->InvViewProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetClipToWorld());
            Parameters->InvProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetViewToClip().InverseFast());
            Parameters->ProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetViewToClip());
            Parameters->ReducedSurfaceBuffer =
                SystemState.fsp.ReducedSurfaceBuffer.Handle;
            Parameters->RWBbvBuffer =
                GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle);
            TShaderMapRef<FInstantRdvBbvReducedSurfaceInjectionCS> ComputeShader(
                GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(
                static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.X),
                8u);
            const uint32 GroupY = FMath::DivideAndRoundUp(
                static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.Y),
                8u);
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("InstantRdv.BbvReducedSurfaceInjection"),
                ERDGPassFlags::Compute,
                ComputeShader,
                Parameters,
                FIntVector(GroupX, GroupY, 1));
        }
        else
        {
            FInstantRdvBbvDepthInjectionCS::FParameters* Parameters =
                GraphBuilder.AllocParameters<FInstantRdvBbvDepthInjectionCS::FParameters>();
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = ViewRectSizeX;
            Parameters->ViewRectSizeY = ViewRectSizeY;
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            const float InjectionOffsetFineCells =
                CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            const float FineCellSizeCm =
                Config.bbv.BbvBrickSizeCm /
                FMath::Max(static_cast<float>(k_irdv_bbv_brick_reso), 1.0f);
            Parameters->DepthtestInjectionWorldOffsetWs =
                FineCellSizeCm * InjectionOffsetFineCells;
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->InvViewProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetClipToWorld());
            const FMatrix ProjectionMatrix = View.ViewMatrices.GetViewToClip();
            Parameters->NativeNearPlaneDeviceDepth =
                (ProjectionMatrix.M[2][3] > 0.0f) ? 1.0f : 0.0f;
            Parameters->DepthInjectionMethod = static_cast<uint32>(
                FMath::Clamp(CVarInstantRdvBbvDepthInjectionMethod.GetValueOnRenderThread(), 0, 1));
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->SceneDepthSampler =
                TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            Parameters->RWBbvBuffer =
                GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle);
            TShaderMapRef<FInstantRdvBbvDepthInjectionCS> ComputeShader(
                GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(ViewRectSizeX, 8u);
            const uint32 GroupY = FMath::DivideAndRoundUp(ViewRectSizeY, 8u);
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("InstantRdv.BbvDepthInjectionMainView"),
                ERDGPassFlags::Compute,
                ComputeShader,
                Parameters,
                FIntVector(GroupX, GroupY, 1));
        }
    }

    if (bEnableMainViewGeometryRemoval)
    {
        {
            const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
            const int32 DepthCullMode = FMath::Clamp(CVarInstantRdvBbvDepthCullMode.GetValueOnRenderThread(), 0, 1);
            if (DepthCullMode == 1)
            {
                FInstantRdvBbvDepthFrustumCullAabbCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthFrustumCullAabbCS::FParameters>();
                {
                    Parameters->BrickCount = BrickCount;
                    Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
                    Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
                    Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
                    Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
                    Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
                    Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
                    Parameters->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetWorldToClip());
                    const FMatrix ProjectionMatrix = View.ViewMatrices.GetViewToClip();
                    Parameters->NearPlaneDeviceDepth = (ProjectionMatrix.M[2][3] > 0.0f) ? 1.0f : 0.0f;
                    Parameters->BbvBuffer = GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle);
                    Parameters->RWFrustumBrickCounter = GraphBuilder.CreateUAV(FrustumBrickCounterBuffer);
                    Parameters->RWFrustumBrickList = GraphBuilder.CreateUAV(FrustumBrickListBuffer);
                }
                TShaderMapRef<FInstantRdvBbvDepthFrustumCullAabbCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvDepthFrustumCull[AABB]"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
            }
            else
            {
                FInstantRdvBbvDepthFrustumCullCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthFrustumCullCS::FParameters>();
                {
                    Parameters->BrickCount = BrickCount;
                    Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
                    Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
                    Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
                    Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
                    Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
                    Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
                    Parameters->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetWorldToClip());
                    Parameters->BbvBuffer = GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle);
                    Parameters->RWFrustumBrickCounter = GraphBuilder.CreateUAV(FrustumBrickCounterBuffer);
                    Parameters->RWFrustumBrickList = GraphBuilder.CreateUAV(FrustumBrickListBuffer);
                }
                TShaderMapRef<FInstantRdvBbvDepthFrustumCullCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvDepthFrustumCull[Legacy]"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
            }
        }

        // Frustum候補数から、Carving(1thread=1u32 job)向けのDispatchIndirect引数を生成する。
        FrustumBrickIndirectArgBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
            TEXT("InstantRdv.BbvFrustumBrickIndirectArg"));
        {
            FInstantRdvBbvDepthCarvingIndirectArgBuildCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthCarvingIndirectArgBuildCS::FParameters>();
            {
                Parameters->FrustumBrickCounter = GraphBuilder.CreateSRV(FrustumBrickCounterBuffer);
                Parameters->ThreadGroupSizeX = 64u;
                Parameters->RWFrustumBrickIndirectArg = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(FrustumBrickIndirectArgBuffer, PF_R32_UINT));
            }
            TShaderMapRef<FInstantRdvBbvDepthCarvingIndirectArgBuildCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvDepthCarvingIndirectArgBuild"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(1, 1, 1));
        }

        {
            const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
            FInstantRdvBbvDepthCarvingCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthCarvingCS::FParameters>();
            {
                Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
                Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
                Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
                Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
                Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
                Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
                Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
                Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
                Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
                Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
                Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
                Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
                Parameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetWorldToView());
                Parameters->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetWorldToClip());
                Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
                Parameters->SceneDepthTexture = SceneDepthTexture;
                Parameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                Parameters->FrustumBrickCounter = GraphBuilder.CreateSRV(FrustumBrickCounterBuffer);
                Parameters->FrustumBrickList = GraphBuilder.CreateSRV(FrustumBrickListBuffer);
                Parameters->RWBbvBuffer = GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle);
                Parameters->FrustumBrickIndirectArgBuffer = FrustumBrickIndirectArgBuffer;
            }
            TShaderMapRef<FInstantRdvBbvDepthCarvingCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("InstantRdv.BbvDepthCarving"),
                ERDGPassFlags::Compute,
                ComputeShader,
                Parameters,
                FrustumBrickIndirectArgBuffer,
                0);
        }
    }

    {
        FInstantRdvBbvBrickCountAggregateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvBrickCountAggregateCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->RWBbvBuffer = GraphBuilder.CreateUAV(SystemState.bbv.BbvBuffer.Handle);
            Parameters->BrickDataBaseOffset = BitmaskElementCount;
        }
        TShaderMapRef<FInstantRdvBbvBrickCountAggregateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvBrickCountAggregate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    {
        FInstantRdvBbvElementUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvElementUpdateCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->FrameCount = SystemState.FrameCount;
            Parameters->UpdateSkipCount = kBbvElementUpdateSkipCount;
            Parameters->BbvBuffer = GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle);
            Parameters->BrickDataBaseOffset = BitmaskElementCount;
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(SystemState.bbv.OptionalDataBuffer.Handle);
        }
        TShaderMapRef<FInstantRdvBbvElementUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 PerFrameCount = FMath::DivideAndRoundUp(BrickCount, kBbvElementUpdateSkipCount + 1u);
        const uint32 GroupX = FMath::DivideAndRoundUp(PerFrameCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvElementUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }
}

void FInstantRdvBbv::ExecuteRadianceUpdate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    FRDGTexture* SceneColorTexture,
    float SceneColorPreExposure,
    bool bEnableRadianceInjection,
    bool bEnableRadianceResolve,
    bool bUseReducedSurfaceBuffer)
{
    if (SceneDepthTexture == nullptr || SceneColorTexture == nullptr || !SystemState.bRenderInitialized)
    {
        return;
    }

    FRDGBufferRef BbvBuffer = SystemState.bbv.BbvBuffer.Handle;
    FRDGBufferRef OptionalDataBuffer = SystemState.bbv.OptionalDataBuffer.Handle;
    FRDGBufferRef RadianceAccumBuffer = SystemState.bbv.RadianceAccumBuffer.Handle;

    const uint32 BrickCount = SystemState.bbv.TrGrid.GetCellCount();
    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);

    if (bEnableRadianceInjection)
    {
        const bool bUseReducedPath =
            bUseReducedSurfaceBuffer &&
            SystemState.fsp.ReducedSurfaceBuffer.Handle != nullptr;
        if (bUseReducedPath)
        {
            FInstantRdvBbvReducedRadianceInjectionCS::FParameters* Parameters =
                GraphBuilder.AllocParameters<FInstantRdvBbvReducedRadianceInjectionCS::FParameters>();
            Parameters->SourceDepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->SourceDepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->SourceViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->SourceViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->SourceViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->SourceViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->FrameCount = SystemState.FrameCount;
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            const float InjectionOffsetFineCells =
                CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            Parameters->InjectionWorldOffsetWs =
                Config.bbv.BbvBrickSizeCm /
                FMath::Max(static_cast<float>(k_irdv_bbv_brick_reso), 1.0f) *
                InjectionOffsetFineCells;
            Parameters->SceneColorPreExposure =
                FMath::Max(SceneColorPreExposure, 1.0e-6f);
            Parameters->ViewMatrix =
                FMatrix44f(View.ViewMatrices.GetTranslatedViewMatrix());
            Parameters->InvViewMatrix =
                FMatrix44f(View.ViewMatrices.GetWorldToView().InverseFast());
            Parameters->InvViewProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetClipToWorld());
            Parameters->InvProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetViewToClip().InverseFast());
            Parameters->ProjectionMatrix =
                FMatrix44f(View.ViewMatrices.GetViewToClip());
            Parameters->BrickDataBaseOffset = Config.bbv.GetBitmaskElementCount();
            Parameters->bEnableShortRayFallback =
                CVarInstantRdvBbvRadianceShortRayFallback.GetValueOnRenderThread() != 0
                    ? 1u
                    : 0u;
            Parameters->ReducedSurfaceBuffer =
                SystemState.fsp.ReducedSurfaceBuffer.Handle;
            Parameters->SceneColorTexture = SceneColorTexture;
            Parameters->BbvBuffer = GraphBuilder.CreateSRV(BbvBuffer);
            Parameters->RWBbvRadianceAccumBuffer =
                GraphBuilder.CreateUAV(RadianceAccumBuffer);
            TShaderMapRef<FInstantRdvBbvReducedRadianceInjectionCS> ComputeShader(
                GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(
                static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.X),
                8u);
            const uint32 GroupY = FMath::DivideAndRoundUp(
                static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.Y),
                8u);
            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("InstantRdv.BbvReducedRadianceInjection"),
                ERDGPassFlags::Compute,
                ComputeShader,
                Parameters,
                FIntVector(GroupX, GroupY, 1));
        }
        else
        {
            FInstantRdvBbvRadianceInjectionCS::FParameters* Parameters =
                GraphBuilder.AllocParameters<FInstantRdvBbvRadianceInjectionCS::FParameters>();
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            const float InjectionOffsetFineCells = CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            const float FineCellSizeCm = Config.bbv.BbvBrickSizeCm / FMath::Max(static_cast<float>(k_irdv_bbv_brick_reso), 1.0f);
            Parameters->DepthtestInjectionWorldOffsetWs = FineCellSizeCm * InjectionOffsetFineCells;
            Parameters->SceneColorPreExposure = FMath::Max(SceneColorPreExposure, 1.0e-6f);
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
            Parameters->BrickDataBaseOffset = Config.bbv.GetBitmaskElementCount();
            Parameters->bEnableShortRayFallback = CVarInstantRdvBbvRadianceShortRayFallback.GetValueOnRenderThread() != 0 ? 1u : 0u;
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->SceneColorTexture = SceneColorTexture;
            Parameters->BbvBuffer = GraphBuilder.CreateSRV(BbvBuffer);
            Parameters->RWBbvRadianceAccumBuffer = GraphBuilder.CreateUAV(RadianceAccumBuffer);
            TShaderMapRef<FInstantRdvBbvRadianceInjectionCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
            const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvRadianceInjection"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
        }
    }

    if (bEnableRadianceResolve)
    {
        FInstantRdvBbvRadianceResolveCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvRadianceResolveCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->RWBbvRadianceAccumBuffer = GraphBuilder.CreateUAV(RadianceAccumBuffer);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(OptionalDataBuffer);
        }
        TShaderMapRef<FInstantRdvBbvRadianceResolveCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvRadianceResolve"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }
}

void FInstantRdvBbv::ExecuteFspUpdate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    bool bEnableFspUpdate,
    bool bUseProbeTraceOffset,
    bool bUseReducedSurfaceBuffer)
{
    if (!bEnableFspUpdate || SceneDepthTexture == nullptr || !SystemState.bRenderInitialized)
    {
        return;
    }

    const uint32 FspCellCount = Config.fsp.GetFspTotalCellCount();
    const uint32 FspCascadeCount = FMath::Max(Config.fsp.ProbeCascadeCount, 1u);
    const FVector3f FspGridCenterPositionWs(SystemState.fsp.CurrentGridCenterPositionWs);
    const FVector3f FspPrevGridCenterPositionWs(SystemState.fsp.FspUpdateFrameCount == 0 ? SystemState.fsp.CurrentGridCenterPositionWs : SystemState.fsp.PreviousGridCenterPositionWs);
    // 参照InstantRDVと同じActiveProbeList double buffering。
    // frame_count & 1 をCurr、反対側をPrevにして、フレーム末尾のCurr->Prev全コピーを不要にする。
    const uint32 FspActiveProbeCurrListIndex = SystemState.fsp.FspUpdateFrameCount & 1u;
    const uint32 FspActiveProbePrevListIndex = 1u - FspActiveProbeCurrListIndex;
    FRDGBufferRef FspActiveProbeListCurrBuffer = SystemState.fsp.FspActiveProbeListBuffers[FspActiveProbeCurrListIndex].Handle;
    FRDGBufferRef FspActiveProbeListPrevBuffer = SystemState.fsp.FspActiveProbeListBuffers[FspActiveProbePrevListIndex].Handle;
    // FSPの主要passは [0]=count のlist bufferを入力にする。
    // countからDispatchIndirect引数を毎回GPU上で作り、全cell/全rayをdispatchしない参照設計を保つ。
    auto AddFspCounterIndirectArgBuildPass =
        [&](const TCHAR* PassName, FRDGBufferRef CounterBuffer, uint32 ThreadGroupSizeX) -> FRDGBufferRef
        {
            FRDGBufferRef IndirectArgBuffer = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
                PassName);

            FInstantRdvFspCounterIndirectArgBuildCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspCounterIndirectArgBuildCS::FParameters>();
            Parameters->ThreadGroupSizeX = ThreadGroupSizeX;
            Parameters->CounterBuffer = GraphBuilder.CreateSRV(CounterBuffer);
            Parameters->RWIndirectArg = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgBuffer, PF_R32_UINT));
            TShaderMapRef<FInstantRdvFspCounterIndirectArgBuildCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspCounterIndirectArgBuild"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(1, 1, 1));
            return IndirectArgBuffer;
        };

    const bool bNeedInitialize = (0 == SystemState.fsp.FspUpdateFrameCount);
    // 初回フレームか, 強制された場合.
    if (bNeedInitialize)
    {
        {
            // Probe free stack を GPU 側で初期化するパスを追加.
            FInstantRdvFspInitPoolCS::FParameters* InitParams = GraphBuilder.AllocParameters<FInstantRdvFspInitPoolCS::FParameters>();
            InitParams->ProbePoolElementCount = FspCellCount;
            InitParams->RWFspProbeFreeStack = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeFreeStackBuffer.Handle);
            InitParams->RWFspCellProbeIndex = GraphBuilder.CreateUAV(SystemState.fsp.FspCellProbeIndexBuffer.Handle);
            InitParams->RWFspProbePool = GraphBuilder.CreateUAV(SystemState.fsp.FspProbePoolBuffer.Handle);
            InitParams->RWFspIrradianceVolumeSH = GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle);
            TShaderMapRef<FInstantRdvFspInitPoolCS> InitShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 InitGroupX = FMath::DivideAndRoundUp(FspCellCount, 64u);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspInitPool"), ERDGPassFlags::Compute, InitShader, InitParams, FIntVector(InitGroupX, 1, 1));
        }
    }

    // フレーム一時counter類を初期化する。
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle), 0u);
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FspActiveProbeListCurrBuffer), 0u);
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayRequestBuffer.Handle), 0u);
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayResultBuffer.Handle), 0u);

    FRDGBufferRef FspPrevActiveProbeIndirectArgBuffer = AddFspCounterIndirectArgBuildPass(
        TEXT("InstantRdv.FspPrevActiveProbeIndirectArg"),
        FspActiveProbeListPrevBuffer,
        k_irdv_fsp_probe_update_thread_group_size);

    {
        // Prev ActiveProbe listを検査し、生存probeだけCurrへ引き継ぐ。
        // staleまたはtoroidal移動で押し出されたprobeはownership/SHを消してfree stackへ戻す。
        FInstantRdvFspBeginUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspBeginUpdateCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FrameCount = SystemState.fsp.FspUpdateFrameCount;
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->FspPrevGridCenterPositionWs = FspPrevGridCenterPositionWs;
        Parameters->FspActiveProbeListPrev = GraphBuilder.CreateSRV(FspActiveProbeListPrevBuffer);
        Parameters->RWFspActiveProbeListCurr = GraphBuilder.CreateUAV(FspActiveProbeListCurrBuffer);
        Parameters->RWFspProbeFreeStack = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeFreeStackBuffer.Handle);
        Parameters->RWFspCellProbeIndex = GraphBuilder.CreateUAV(SystemState.fsp.FspCellProbeIndexBuffer.Handle);
        Parameters->RWFspProbePool = GraphBuilder.CreateUAV(SystemState.fsp.FspProbePoolBuffer.Handle);
        Parameters->RWFspVisibleSurfaceList = GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
        Parameters->RWFspProbeRayRequestBuffer = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayRequestBuffer.Handle);
        Parameters->RWFspProbeRayResultBuffer = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayResultBuffer.Handle);
        Parameters->RWFspIrradianceVolumeSH = GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle);
        Parameters->FspPrevActiveProbeIndirectArgBuffer = FspPrevActiveProbeIndirectArgBuffer;
        TShaderMapRef<FInstantRdvFspBeginUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspBeginUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FspPrevActiveProbeIndirectArgBuffer, 0);
    }

    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
    const uint32 FspSurfaceMaskWordCount = FMath::DivideAndRoundUp(FspCellCount, 32u);
    FRDGBufferRef FspSurfaceCellMaskBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspSurfaceMaskWordCount),
        TEXT("InstantRdv.FspSurfaceCellMask"));
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FspSurfaceCellMaskBuffer), 0u);

    const bool bUseReducedPath =
        bUseReducedSurfaceBuffer &&
        SystemState.fsp.ReducedSurfaceBuffer.Handle != nullptr;
    if (bUseReducedPath)
    {
        FInstantRdvFspSurfaceDetectReducedCS::FParameters* Parameters =
            GraphBuilder.AllocParameters<FInstantRdvFspSurfaceDetectReducedCS::FParameters>();
        Parameters->SourceDepthSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
        Parameters->SourceDepthSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
        Parameters->FrameCount = SystemState.FrameCount;
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->ViewMatrix =
            FMatrix44f(View.ViewMatrices.GetTranslatedViewMatrix());
        Parameters->InvViewMatrix =
            FMatrix44f(View.ViewMatrices.GetWorldToView().InverseFast());
        Parameters->InvViewProjectionMatrix =
            FMatrix44f(View.ViewMatrices.GetClipToWorld());
        Parameters->InvProjectionMatrix =
            FMatrix44f(View.ViewMatrices.GetViewToClip().InverseFast());
        Parameters->ProjectionMatrix =
            FMatrix44f(View.ViewMatrices.GetViewToClip());
        Parameters->VisibleSurfaceListCapacity = FspCellCount;
        Parameters->ReducedSurfaceBuffer =
            SystemState.fsp.ReducedSurfaceBuffer.Handle;
        Parameters->RWFspSurfaceCellMask =
            GraphBuilder.CreateUAV(FspSurfaceCellMaskBuffer);
        Parameters->RWFspVisibleSurfaceList =
            GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
        Parameters->RWFspVisibleSurfaceSourceTexelList =
            GraphBuilder.CreateUAV(
                SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.Handle);
        TShaderMapRef<FInstantRdvFspSurfaceDetectReducedCS> ComputeShader(
            GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(
            static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.X),
            8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(
            static_cast<uint32>(SystemState.fsp.ReducedSurfaceBuffer.Extent.Y),
            8u);
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("InstantRdv.FspSurfaceDetectReduced"),
            ERDGPassFlags::Compute,
            ComputeShader,
            Parameters,
            FIntVector(GroupX, GroupY, 1));
    }
    else
    {
        {
            // Nativeと同じくDepth pixelを直接appendせず、Wave集約した1bit/cell maskへ注入する。
            FInstantRdvFspSurfaceMaskInjectCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspSurfaceMaskInjectCS::FParameters>();
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
            Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
            Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
            Parameters->FspCascadeCount = FspCascadeCount;
            Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
            Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->RWFspSurfaceCellMask = GraphBuilder.CreateUAV(FspSurfaceCellMaskBuffer);
            TShaderMapRef<FInstantRdvFspSurfaceMaskInjectCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
            const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspSurfaceMaskInject"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
        }

        {
            FInstantRdvFspSurfaceMaskCompactCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspSurfaceMaskCompactCS::FParameters>();
            Parameters->FspTotalCellCount = FspCellCount;
            Parameters->FspVisibleSurfaceListCapacity = FspCellCount;
            Parameters->FspSurfaceCellMask = GraphBuilder.CreateSRV(FspSurfaceCellMaskBuffer);
            Parameters->RWFspVisibleSurfaceList = GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
            TShaderMapRef<FInstantRdvFspSurfaceMaskCompactCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(FspSurfaceMaskWordCount, 128u);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspSurfaceMaskCompact"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
        }
    }

    FRDGBufferRef FspPreUpdateIndirectArgBuffer = AddFspCounterIndirectArgBuildPass(
        TEXT("InstantRdv.FspPreUpdateIndirectArg"),
        SystemState.fsp.FspVisibleSurfaceListBuffer.Handle,
        k_irdv_fsp_probe_update_thread_group_size);

    {
        // Visible surface cellへprobeを割り当て、既存probeはLastSeenFrameを更新する。
        // Dispatch数はVisibleSurfaceList[0]に比例する。
        FInstantRdvFspPreUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspPreUpdateCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FrameCount = SystemState.fsp.FspUpdateFrameCount;
        Parameters->bEnableWarmStart = CVarInstantRdvFspWarmStart.GetValueOnRenderThread() != 0 ? 1u : 0u;
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->FspRelocationOffsetScale = FMath::Max(CVarInstantRdvFspRelocationOffsetScale.GetValueOnRenderThread(), 0.01f);
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
        Parameters->bUseReducedSurfaceBuffer = bUseReducedPath ? 1u : 0u;
        Parameters->SourceViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
        Parameters->SourceViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
        Parameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetTranslatedViewMatrix());
        Parameters->InvViewMatrix = FMatrix44f(View.ViewMatrices.GetWorldToView().InverseFast());
        Parameters->InvProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewToClip().InverseFast());
        Parameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewToClip());
        Parameters->BbvGridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
        Parameters->BbvGridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
        Parameters->BbvGridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
        Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
        Parameters->BbvCellSizeCm = Config.bbv.BbvBrickSizeCm;
        Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
        Parameters->FspVisibleSurfaceList = GraphBuilder.CreateSRV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
        Parameters->FspVisibleSurfaceSourceTexelList =
            GraphBuilder.CreateSRV(
                SystemState.fsp.FspVisibleSurfaceSourceTexelListBuffer.Handle);
        Parameters->ReducedSurfaceBuffer =
            SystemState.fsp.ReducedSurfaceBuffer.Handle;
        Parameters->BbvBuffer = GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle);
        Parameters->RWFspActiveProbeListCurr = GraphBuilder.CreateUAV(FspActiveProbeListCurrBuffer);
        Parameters->RWFspProbeFreeStack = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeFreeStackBuffer.Handle);
        Parameters->RWFspCellProbeIndex = GraphBuilder.CreateUAV(SystemState.fsp.FspCellProbeIndexBuffer.Handle);
        Parameters->RWFspProbePool = GraphBuilder.CreateUAV(SystemState.fsp.FspProbePoolBuffer.Handle);
        Parameters->RWFspProbeAtlas = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeAtlasBuffer.Handle);
        Parameters->FspPreUpdateIndirectArgBuffer = FspPreUpdateIndirectArgBuffer;
        TShaderMapRef<FInstantRdvFspPreUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspPreUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FspPreUpdateIndirectArgBuffer, 0);
    }

    FRDGBufferRef FspActiveProbeIndirectArgBuffer = AddFspCounterIndirectArgBuildPass(
        TEXT("InstantRdv.FspActiveProbeIndirectArg"),
        FspActiveProbeListCurrBuffer,
        k_irdv_fsp_probe_update_thread_group_size);

    {
        // ActiveProbeごとに6x6 OctMap方向のray requestを生成する。
        // 以降のtrace/resolveはrequest/result counterで間接dispatchする。
        FInstantRdvFspProbeRayRequestCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspProbeRayRequestCS::FParameters>();
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FspActiveProbeListCurr = GraphBuilder.CreateSRV(FspActiveProbeListCurrBuffer);
        Parameters->RWFspProbeRayRequestBuffer = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayRequestBuffer.Handle);
        Parameters->FspActiveProbeIndirectArgBuffer = FspActiveProbeIndirectArgBuffer;
        TShaderMapRef<FInstantRdvFspProbeRayRequestCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspProbeRayRequest"), ERDGPassFlags::Compute, ComputeShader, Parameters, FspActiveProbeIndirectArgBuffer, 0);
    }

    FRDGBufferRef FspTraceIndirectArgBuffer = AddFspCounterIndirectArgBuildPass(
        TEXT("InstantRdv.FspTraceIndirectArg"),
        SystemState.fsp.FspProbeRayRequestBuffer.Handle,
        k_irdv_fsp_ray_linear_thread_group_size);

    {
        // Probe rayをBBVへtraceする。ここで走るthread数はactive probe数 * 36方向に限定される。
        FInstantRdvFspProbeRayTraceCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspProbeRayTraceCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FrameCount = SystemState.fsp.FspUpdateFrameCount;
        Parameters->bUseProbeTraceOffset = bUseProbeTraceOffset ? 1u : 0u;
        Parameters->BbvGridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
        Parameters->BbvGridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
        Parameters->BbvGridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->FspRelocationOffsetScale = FMath::Max(CVarInstantRdvFspRelocationOffsetScale.GetValueOnRenderThread(), 0.01f);
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
        Parameters->BbvCellSizeCm = Config.bbv.BbvBrickSizeCm;
        Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
        Parameters->FspTraceDistanceCm = static_cast<float>(kFspTraceDistanceCm);
        Parameters->FspProbeRayRequestBuffer = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeRayRequestBuffer.Handle);
        Parameters->RWFspProbeRayResultBuffer = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRayResultBuffer.Handle);
        Parameters->FspProbePool = GraphBuilder.CreateSRV(SystemState.fsp.FspProbePoolBuffer.Handle);
        Parameters->BbvBuffer = GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle);
        Parameters->BrickDataBaseOffset = Config.bbv.GetBitmaskElementCount();
        Parameters->FspTraceIndirectArgBuffer = FspTraceIndirectArgBuffer;
        TShaderMapRef<FInstantRdvFspProbeRayTraceCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspProbeRayTrace"), ERDGPassFlags::Compute, ComputeShader, Parameters, FspTraceIndirectArgBuffer, 0);
    }

    FRDGBufferRef FspResolveIndirectArgBuffer = AddFspCounterIndirectArgBuildPass(
        TEXT("InstantRdv.FspResolveIndirectArg"),
        SystemState.fsp.FspProbeRayResultBuffer.Handle,
        k_irdv_fsp_ray_linear_thread_group_size);

    {
        // Trace結果をOctMap atlasへresolveし、BBV radianceまたはsky visibilityを履歴blendする。
        FInstantRdvFspProbeRayResolveCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspProbeRayResolveCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FrameCount = SystemState.fsp.FspUpdateFrameCount;
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->FspProbeRayResultBuffer = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeRayResultBuffer.Handle);
        Parameters->BbvOptionalData = GraphBuilder.CreateSRV(SystemState.bbv.OptionalDataBuffer.Handle);
        Parameters->RWFspProbePool = GraphBuilder.CreateUAV(SystemState.fsp.FspProbePoolBuffer.Handle);
        Parameters->RWFspProbeAtlas = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeAtlasBuffer.Handle);
        Parameters->FspResolveIndirectArgBuffer = FspResolveIndirectArgBuffer;
        TShaderMapRef<FInstantRdvFspProbeRayResolveCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspProbeRayResolve"), ERDGPassFlags::Compute, ComputeShader, Parameters, FspResolveIndirectArgBuffer, 0);
    }

    {
        // ActiveProbeのOctMapをL1 SHへ積分し、owner cellのdense IrradianceVolume SHを更新する。
        FInstantRdvFspShUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspShUpdateCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->FspActiveProbeListCurr = GraphBuilder.CreateSRV(FspActiveProbeListCurrBuffer);
        Parameters->FspProbePool = GraphBuilder.CreateSRV(SystemState.fsp.FspProbePoolBuffer.Handle);
        Parameters->FspProbeAtlas = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeAtlasBuffer.Handle);
        Parameters->RWFspPackedSH = GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle);
        Parameters->FspActiveProbeIndirectArgBuffer = FspActiveProbeIndirectArgBuffer;
        TShaderMapRef<FInstantRdvFspShUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspProbeShUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FspActiveProbeIndirectArgBuffer, 0);
    }

    {
        // ActiveProbeが無いcellにも近傍SHを伝播し、IrradianceVolumeとして参照できるdense fieldを維持する。
        FInstantRdvFspIrradianceVolumePropagateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspIrradianceVolumePropagateCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCascadeCount = FspCascadeCount;
        Parameters->FspProbePoolElementCount = FspCellCount;
        Parameters->FrameCount = SystemState.fsp.FspUpdateFrameCount;
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->FspGridCenterPositionWs = FspGridCenterPositionWs;
        Parameters->FspCellProbeIndex = GraphBuilder.CreateSRV(SystemState.fsp.FspCellProbeIndexBuffer.Handle);
        Parameters->FspProbePool = GraphBuilder.CreateSRV(SystemState.fsp.FspProbePoolBuffer.Handle);
        Parameters->RWFspPackedSH = GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle);
        TShaderMapRef<FInstantRdvFspIrradianceVolumePropagateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspIrradianceVolumePropagate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(FMath::DivideAndRoundUp(FspCellCount, 64u), 1, 1));
    }

    // 次フレームのtoroidal invalidationで「前回のgrid center」として使う。
    SystemState.fsp.PreviousGridCenterPositionWs = View.ViewLocation;
    ++SystemState.fsp.FspUpdateFrameCount;
}

void FInstantRdvBbv::ExecuteDebugVisualize(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    FRDGTexture* SceneColorTexture,
    float SceneColorPreExposure,
    int32 BbvDebugMode,
    int32 FspProbeDebugMode,
    int32 FspIvProbeDebugMode,
    bool bProbeDepthTest,
    float BbvDebugSceneColorBlend,
    bool bBbvDebugDepthTest,
    bool bUseProbeVisualizationOffset,
    bool bUseProbeTraceOffset)
{
    if ((BbvDebugMode <= 0 && FspProbeDebugMode <= 0 && FspIvProbeDebugMode <= 0) || SceneDepthTexture == nullptr || SceneColorTexture == nullptr)
    {
        return;
    }

    FRDGBufferRef BbvBuffer = SystemState.bbv.BbvBuffer.Handle;
    FRDGBufferRef OptionalDataBuffer = SystemState.bbv.OptionalDataBuffer.Handle;

    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);

    // 全画面描画系デバッグ表示.
    if (
        BbvDebugMode == 1 ||
        BbvDebugMode == 2 ||
        BbvDebugMode == 3 ||
        BbvDebugMode == 4 ||
        BbvDebugMode == 5 ||

        false
        )
    {
        FRDGTextureRef SceneColorInputTexture = GraphBuilder.CreateTexture(SceneColorTexture->Desc, TEXT("InstantRdv.BbvDebugSceneColorInput"));
        AddCopyTexturePass(GraphBuilder, SceneColorTexture, SceneColorInputTexture);

        FInstantRdvBbvDebugVisualizePS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDebugVisualizePS::FParameters>();
        {
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->SceneColorTexture = SceneColorInputTexture;
            Parameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->SceneColorPreExposure = FMath::Max(SceneColorPreExposure, 1.0e-6f);
            Parameters->SceneColorBlendRate = FMath::Clamp(BbvDebugSceneColorBlend, 0.0f, 1.0f);
            Parameters->bDepthTest = bBbvDebugDepthTest ? 1u : 0u;
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->MaxTraceDistanceCm = Config.bbv.BbvBrickSizeCm * static_cast<float>(SystemState.bbv.TrGrid.GridReso.GetMax());
            Parameters->DepthRelationRangeFineCells = FMath::Max(CVarInstantRdvBbvDepthRelationRangeFineCells.GetValueOnRenderThread(), 0.001f);
            Parameters->DebugMode = BbvDebugMode;
            Parameters->BbvBuffer = GraphBuilder.CreateSRV(BbvBuffer);
            Parameters->BrickDataBaseOffset = Config.bbv.GetBitmaskElementCount();
            Parameters->BitmaskBrickVoxelOptionData = GraphBuilder.CreateSRV(OptionalDataBuffer);
            Parameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
        }

        const FScreenPassTextureViewport Viewport(SceneColorTexture, ViewRect);
        TShaderMapRef<FScreenPassVS> VertexShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        TShaderMapRef<FInstantRdvBbvDebugVisualizePS> PixelShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        AddDrawScreenPass(
            GraphBuilder,
            RDG_EVENT_NAME("InstantRdv.BbvDebugVisualize"),
            View,
            Viewport,
            Viewport,
            VertexShader,
            PixelShader,
            Parameters);
    }
    
    const auto AddFspProbeBillboardPass = [&](uint32 InternalDebugMode, const TCHAR* PassName, bool bDrawCounters)
    {
        const uint32 ProbeCount = Config.fsp.GetFspTotalCellCount();
        const uint32 FspCascadeCount = FMath::Max(Config.fsp.ProbeCascadeCount, 1u);
        const FVector3f FspGridCenterPositionWs(SystemState.fsp.CurrentGridCenterPositionWs);
        const FMatrix InvViewMatrix = View.ViewMatrices.GetWorldToView().InverseFast();
        const FVector3f CameraRightWs(FVector(InvViewMatrix.M[0][0], InvViewMatrix.M[0][1], InvViewMatrix.M[0][2]));
        const FVector3f CameraUpWs(FVector(InvViewMatrix.M[1][0], InvViewMatrix.M[1][1], InvViewMatrix.M[1][2]));
        {
            FInstantRdvFspProbeBillboard_Parameters* Params = GraphBuilder.AllocParameters<FInstantRdvFspProbeBillboard_Parameters>();

            FInstantRdvFspProbeBillboardVS::FParameters* VSParams = &Params->VS;
            {
                VSParams->ViewMatrix = FMatrix44f(View.ViewMatrices.GetWorldToView());
                VSParams->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetWorldToClip());
                VSParams->CameraRightWs = CameraRightWs;
                VSParams->CameraUpWs = CameraUpWs;
                VSParams->CameraPositionWs = FVector3f(View.ViewLocation);
                VSParams->FspGridCenterPositionWs = FspGridCenterPositionWs;
                VSParams->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
                VSParams->FspRelocationOffsetScale = FMath::Max(CVarInstantRdvFspRelocationOffsetScale.GetValueOnRenderThread(), 0.01f);
                VSParams->DebugProbeRadiusCm = CVarInstantRdvFspDebugProbeRadiusCm.GetValueOnRenderThread();
                VSParams->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
                VSParams->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
                VSParams->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
                VSParams->FspCascadeCount = FspCascadeCount;
                VSParams->FspProbePoolElementCount = ProbeCount;
                VSParams->FrameCount = SystemState.FrameCount;
                VSParams->DebugMode = InternalDebugMode;
                VSParams->bUseProbeVisualizationOffset = bUseProbeVisualizationOffset ? 1u : 0u;
                VSParams->bUseProbeTraceOffset = bUseProbeTraceOffset ? 1u : 0u;
                VSParams->FspCellProbeIndex = GraphBuilder.CreateSRV(SystemState.fsp.FspCellProbeIndexBuffer.Handle);
                VSParams->FspProbePool = GraphBuilder.CreateSRV(SystemState.fsp.FspProbePoolBuffer.Handle);
                VSParams->FspProbeAtlas = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeAtlasBuffer.Handle);
                VSParams->FspPackedSH = GraphBuilder.CreateSRV(SystemState.fsp.FspPackedSHBuffer.Handle);
            }

            FInstantRdvFspProbeBillboardPS::FParameters* PSParams = &Params->PS;
            {
                PSParams->CameraUpWs = CameraUpWs;
                PSParams->CameraPositionWs = FVector3f(View.ViewLocation);
                PSParams->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
                PSParams->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
                PSParams->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
                PSParams->FspCascadeCount = FspCascadeCount;
                PSParams->FspProbePoolElementCount = ProbeCount;
                PSParams->FrameCount = SystemState.FrameCount;
                PSParams->DebugMode = InternalDebugMode;
                PSParams->bUseProbeTraceOffset = bUseProbeTraceOffset ? 1u : 0u;
                PSParams->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
                PSParams->FspRelocationOffsetScale = FMath::Max(CVarInstantRdvFspRelocationOffsetScale.GetValueOnRenderThread(), 0.01f);
                PSParams->FspGridCenterPositionWs = FspGridCenterPositionWs;
                PSParams->BbvGridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
                PSParams->BbvGridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
                PSParams->BbvGridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
                PSParams->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
                PSParams->BbvCellSizeCm = Config.bbv.BbvBrickSizeCm;
                PSParams->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
                PSParams->FspProbePool = GraphBuilder.CreateSRV(SystemState.fsp.FspProbePoolBuffer.Handle);
                PSParams->FspProbeAtlas = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeAtlasBuffer.Handle);
                PSParams->FspPackedSH = GraphBuilder.CreateSRV(SystemState.fsp.FspPackedSHBuffer.Handle);
                PSParams->BbvBuffer = GraphBuilder.CreateSRV(SystemState.bbv.BbvBuffer.Handle);
                PSParams->BrickDataBaseOffset = Config.bbv.GetBitmaskElementCount();

                // GlobalShaderのGraphicsPipelineでRasterをする場合はShaderParameterにRENDER_TARGET_BINDING_SLOTSでRenderTargetSlotを定義しておいてターゲットを設定し, GraphBuilder.AddPass() のShaderParameter引数有バージョンに引き渡すことでRenderTarget設定される.
                {
                    PSParams->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
                    PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilNop);
                }
            }

            TShaderMapRef<FInstantRdvFspProbeBillboardVS> ProbeVS(GetGlobalShaderMap(View.GetFeatureLevel()));
            TShaderMapRef<FInstantRdvFspProbeBillboardPS> ProbePS(GetGlobalShaderMap(View.GetFeatureLevel()));

            GraphBuilder.AddPass(RDG_EVENT_NAME("%s", PassName), Params, ERDGPassFlags::Raster, [ViewRect, ProbeVS, ProbePS, VSParams, PSParams, ProbeCount, bProbeDepthTest](FRHICommandListImmediate& RHICmdList)
            {
                FGraphicsPipelineStateInitializer GraphicsPSOInit;
                RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

                RHICmdList.SetViewport((float)ViewRect.Min.X, (float)ViewRect.Min.Y, 0.0f, (float)ViewRect.Max.X, (float)ViewRect.Max.Y, 1.0f);

                // InputLayout無し.
                GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GInstantRdvNullVertexDeclaration.VertexDeclarationRHI;

                // PipelineStageのShaderを設定.
                GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ProbeVS.GetVertexShader();
                GraphicsPSOInit.BoundShaderState.PixelShaderRHI = ProbePS.GetPixelShader();

                GraphicsPSOInit.PrimitiveType = PT_TriangleList;
                GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
                GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
                GraphicsPSOInit.DepthStencilState = bProbeDepthTest
                    ? TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()
                    : TStaticDepthStencilState<false, CF_Always>::GetRHI();

                SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                
                // Set shader parameters. -> AddPassのShaderParameter指定版を使う場合, RenderTargetの設定などはなされるが, シェーダステージのUniformパラメータは自動セットされない模様.
                SetShaderParameters(RHICmdList, ProbeVS, ProbeVS.GetVertexShader(), *VSParams);
                SetShaderParameters(RHICmdList, ProbePS, ProbePS.GetPixelShader(), *PSParams);

                const int k_per_instance_prim_count = 2;
                const int instance_count = static_cast<int>(ProbeCount);
                RHICmdList.DrawPrimitive(0, instance_count * k_per_instance_prim_count, 1);
            });
        }

        if (bDrawCounters)
        {
            FInstantRdvFspDebugTextPS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspDebugTextPS::FParameters>();
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->FspProbePoolElementCount = ProbeCount;
            const uint32 FspActiveProbeCurrListIndex = (SystemState.fsp.FspUpdateFrameCount == 0u)
                ? 0u
                : (1u - (SystemState.fsp.FspUpdateFrameCount & 1u));
            Parameters->FspVisibleSurfaceList = GraphBuilder.CreateSRV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
            Parameters->FspActiveProbeListCurr = GraphBuilder.CreateSRV(SystemState.fsp.FspActiveProbeListBuffers[FspActiveProbeCurrListIndex].Handle);
            Parameters->FspProbeRayRequestBuffer = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeRayRequestBuffer.Handle);
            Parameters->FspProbeRayResultBuffer = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeRayResultBuffer.Handle);
            Parameters->FspProbeFreeStack = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeFreeStackBuffer.Handle);
            Parameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);

            const FScreenPassTextureViewport Viewport(SceneColorTexture, ViewRect);
            TShaderMapRef<FScreenPassVS> VertexShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            TShaderMapRef<FInstantRdvFspDebugTextPS> PixelShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            AddDrawScreenPass(
                GraphBuilder,
                RDG_EVENT_NAME("InstantRdv.FspDebugText"),
                View,
                Viewport,
                Viewport,
                VertexShader,
                PixelShader,
                Parameters);

            AddDrawCanvasPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspDebugTextLabels"), View, FScreenPassRenderTarget(SceneColorTexture, ERenderTargetLoadAction::ELoad), [](FCanvas& Canvas)
            {
                const float X = 28.0f;
                float Y = 10.0f;
                constexpr float RowHeight = 26.0f;
                Canvas.DrawShadowedString(X, Y, TEXT("VisibleCells"), GetStatsFont(), FLinearColor(0.2f, 0.8f, 1.0f));
                Canvas.DrawShadowedString(X, Y += RowHeight, TEXT("ActiveProbes"), GetStatsFont(), FLinearColor(0.2f, 1.0f, 0.3f));
                Canvas.DrawShadowedString(X, Y += RowHeight, TEXT("RayRequests"), GetStatsFont(), FLinearColor(1.0f, 0.8f, 0.2f));
                Canvas.DrawShadowedString(X, Y += RowHeight, TEXT("RayResults"), GetStatsFont(), FLinearColor(1.0f, 0.4f, 0.2f));
                Canvas.DrawShadowedString(X, Y += RowHeight, TEXT("FreeProbes"), GetStatsFont(), FLinearColor(0.8f, 0.5f, 1.0f));
            });
        }
    };

    // ActiveProbeとIrradianceVolumeは見たい対象が異なるため、別CVarから個別に描画できるようにする。
    // シェーダ内部の既存debug mode体系はそのまま使い、ここで外部CVar値を内部modeへ写像する。
    if (FspProbeDebugMode > 0)
    {
        const uint32 InternalDebugMode = (FspProbeDebugMode == 10)
            ? 11u
            : static_cast<uint32>(FspProbeDebugMode - 1);
        AddFspProbeBillboardPass(InternalDebugMode, TEXT("InstantRdv.FspProbeBillboard"), true);
    }

    if (FspIvProbeDebugMode > 0)
    {
        AddFspProbeBillboardPass(static_cast<uint32>(FspIvProbeDebugMode + 8), TEXT("InstantRdv.FspIvProbeBillboard"), false);
    }
}
