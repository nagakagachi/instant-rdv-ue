#include "InstantRdvBbv.h"

#include "FXRenderingUtils.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

namespace
{
static constexpr uint32 kBbvElementUpdateSkipCount = 3;
static constexpr uint32 kBbvRadianceAccumComponentCount = 4;
static constexpr uint32 kFspCellDataU32Count = 4;
// 移植時の重要注意（RDG/RHI）:
// - 再生成したバッファは QueueBufferExtraction 前に必ず produced 状態へする（Clear など）。
//   produced でない抽出は RDG validation で落ちる。
// - Indirect dispatch は args バッファ生成だけでなく、消費パス側で IndirectArgs access を明示する。
//   片側だけだと実行時 validation で失敗する。

static TAutoConsoleVariable<int32> CVarInstantRdvBbvReset(
    TEXT("r.InstantRdv.Bbv.Reset"),
    0,
    TEXT("Force BBV buffer reinitialization.\n0: Keep persistent BBV state\n1: Clear BBV state this frame"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarInstantRdvBbvDepthtestInjectionOffsetFineCells(
    TEXT("r.InstantRdv.Bbv.DepthtestInjectionOffsetFineCells"),
    2.0f,
    TEXT("Depthtest Injectionの視線奥オフセット量（fine cell単位）。\n参照実装準拠で、実際の距離は CellSizeCm * (FineCells / BbvPerVoxelResolution) で算出。"),
    ECVF_RenderThreadSafe);
// 移植ミス再発防止:
// - 参照実装は「固定m値」ではなく fine cell 基準でオフセット量を決める。
// - UE側はワールド単位がcmのため、シェーダへ渡す前に必ず CellSizeCm/BbvPerVoxelResolution で換算する。
// - CVarの意味は「ワールド距離」ではなく「fine cell数」を維持すること。

class FInstantRdvBbvBeginUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvBeginUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvBeginUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BitmaskElementCount)
        SHADER_PARAMETER(uint32, BrickDataElementCount)
        SHADER_PARAMETER(uint32, HiBrickDataElementCount)
        SHADER_PARAMETER(uint32, OptionalDataElementCount)
        SHADER_PARAMETER(uint32, DispatchLinearWidth)
        SHADER_PARAMETER(uint32, DispatchThreadCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxel)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBrickData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWHiBrickData)
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
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, BbvPerVoxelResolution)
        SHADER_PARAMETER(FVector3f, ToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, GridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(float, DepthtestInjectionWorldOffsetWs)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxel)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvDepthFrustumCullCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvDepthFrustumCullCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvDepthFrustumCullCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(FVector3f, ToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, GridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxel)
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
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
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
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, BbvPerVoxelResolution)
        SHADER_PARAMETER(FVector3f, ToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, GridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FMatrix44f, ViewMatrix)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FrustumBrickCounter)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FrustumBrickList)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxel)
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
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, OptionalDataU32Count)
        SHADER_PARAMETER(FIntVector4, GridMoveDeltaCells)
        SHADER_PARAMETER(FVector3f, ToroidalOffsetCells)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxel)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxelOptionData)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvBrickCountAggregateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvBrickCountAggregateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvBrickCountAggregateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxel)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBrickData)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvBbvElementUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvBbvElementUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvBbvElementUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BrickCount)
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, OptionalDataU32Count)
        SHADER_PARAMETER(uint32, BbvPerVoxelResolution)
        SHADER_PARAMETER(uint32, FrameCount)
        SHADER_PARAMETER(uint32, UpdateSkipCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxel)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickData)
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
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, BbvPerVoxelResolution)
        SHADER_PARAMETER(FVector3f, ToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, GridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(float, DepthtestInjectionWorldOffsetWs)
        SHADER_PARAMETER(float, SceneColorPreExposure)
        SHADER_PARAMETER(FVector3f, CameraPositionWs)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxel)
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
        SHADER_PARAMETER(uint32, OptionalDataU32Count)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBbvRadianceAccumBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBitmaskBrickVoxelOptionData)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspScreenSpaceCollectCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspScreenSpaceCollectCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspScreenSpaceCollectCS, FGlobalShader);

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
        SHADER_PARAMETER(uint32, FspVisibleSurfaceListCapacity)
        SHADER_PARAMETER(FVector3f, FspGridMinPositionWs)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspCellData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspVisibleSurfaceList)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspRadianceUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspRadianceUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspRadianceUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, FspGridResolutionX)
        SHADER_PARAMETER(uint32, FspGridResolutionY)
        SHADER_PARAMETER(uint32, FspGridResolutionZ)
        SHADER_PARAMETER(uint32, FspCellDataU32Count)
        SHADER_PARAMETER(uint32, FspVisibleSurfaceListCapacity)
        SHADER_PARAMETER(uint32, BbvGridResolutionX)
        SHADER_PARAMETER(uint32, BbvGridResolutionY)
        SHADER_PARAMETER(uint32, BbvGridResolutionZ)
        SHADER_PARAMETER(uint32, BbvOptionalDataU32Count)
        SHADER_PARAMETER(FVector3f, FspGridMinPositionWs)
        SHADER_PARAMETER(float, FspCellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, BbvCellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BbvOptionalData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspCellData)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspVisibleSurfaceList)
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
        SHADER_PARAMETER(uint32, DepthSizeX)
        SHADER_PARAMETER(uint32, DepthSizeY)
        SHADER_PARAMETER(uint32, ViewRectMinX)
        SHADER_PARAMETER(uint32, ViewRectMinY)
        SHADER_PARAMETER(uint32, ViewRectSizeX)
        SHADER_PARAMETER(uint32, ViewRectSizeY)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
        SHADER_PARAMETER(uint32, BitmaskWordsPerBrick)
        SHADER_PARAMETER(uint32, BbvPerVoxelResolution)
        SHADER_PARAMETER(FVector3f, ToroidalOffsetCells)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FVector3f, GridMinPositionWs)
        SHADER_PARAMETER(float, MaxTraceDistanceCm)
        SHADER_PARAMETER(int32, DebugMode)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxel)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickData)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxelOptionData)
        RENDER_TARGET_BINDING_SLOTS()
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvBeginUpdateCS, "/InstantRdvShaders/Private/InstantRdv/bbv_begin_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvBeginViewUpdateCS, "/InstantRdvShaders/Private/InstantRdv/bbv_begin_view_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthInjectionCS, "/InstantRdvShaders/Private/InstantRdv/bbv_depthtest_injection_apply_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthFrustumCullCS, "/InstantRdvShaders/Private/InstantRdv/bbv_depthtest_frustum_cull_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthCarvingIndirectArgBuildCS, "/InstantRdvShaders/Private/InstantRdv/bbv_depthtest_carving_indirect_arg_build_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDepthCarvingCS, "/InstantRdvShaders/Private/InstantRdv/bbv_depthtest_carving_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvToroidalClearCS, "/InstantRdvShaders/Private/InstantRdv/bbv_toroidal_clear_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvBrickCountAggregateCS, "/InstantRdvShaders/Private/InstantRdv/bbv_brick_count_aggregate_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvElementUpdateCS, "/InstantRdvShaders/Private/InstantRdv/bbv_element_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvRadianceInjectionCS, "/InstantRdvShaders/Private/InstantRdv/bbv_radiance_injection_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvRadianceResolveCS, "/InstantRdvShaders/Private/InstantRdv/bbv_radiance_resolve_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspScreenSpaceCollectCS, "/InstantRdvShaders/Private/InstantRdv/fsp_screen_space_collect_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspRadianceUpdateCS, "/InstantRdvShaders/Private/InstantRdv/fsp_radiance_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDebugVisualizePS, "/InstantRdvShaders/Private/InstantRdv/bbv_debug_visualize_ps.usf", "MainPS", SF_Pixel);
} // namespace

uint32 FInstantRdvBbvConfig::GetBbvBrickCount() const
{
    return static_cast<uint32>(BbvGridResolution.X) * static_cast<uint32>(BbvGridResolution.Y) * static_cast<uint32>(BbvGridResolution.Z);
}

uint32 FInstantRdvBbvConfig::GetBitmaskU32CountPerBrick() const
{
    const uint32 PerBrickBitCount = BbvPerVoxelResolution * BbvPerVoxelResolution * BbvPerVoxelResolution;
    return PerBrickBitCount / 32u;
}

uint32 FInstantRdvBbvConfig::GetBitmaskElementCount() const
{
    return GetBbvBrickCount() * GetBitmaskU32CountPerBrick();
}

uint32 FInstantRdvBbvConfig::GetBrickDataElementCount() const
{
    return GetBbvBrickCount() * BrickDataU32Count;
}

uint32 FInstantRdvBbvConfig::GetHiBrickBrickCount() const
{
    const uint32 HiX = FMath::DivideAndRoundUp(static_cast<uint32>(BbvGridResolution.X), 2u);
    const uint32 HiY = FMath::DivideAndRoundUp(static_cast<uint32>(BbvGridResolution.Y), 2u);
    const uint32 HiZ = FMath::DivideAndRoundUp(static_cast<uint32>(BbvGridResolution.Z), 2u);
    return HiX * HiY * HiZ;
}

uint32 FInstantRdvBbvConfig::GetHiBrickDataElementCount() const
{
    return GetHiBrickBrickCount() * HiBrickDataU32Count;
}

uint32 FInstantRdvBbvConfig::GetOptionalDataElementCount() const
{
    return GetBbvBrickCount() * OptionalDataU32Count;
}

void FInstantRdvBbv::BeginFrame_RenderThread()
{
    ResourceCache.FrameBitmaskBuffer = nullptr;
    ResourceCache.FrameBrickDataBuffer = nullptr;
    ResourceCache.FrameHiBrickDataBuffer = nullptr;
    ResourceCache.FrameOptionalDataBuffer = nullptr;
    ResourceCache.FrameRadianceAccumBuffer = nullptr;
    ResourceCache.FrameFspCellDataBuffer = nullptr;
    ResourceCache.FrameFspVisibleSurfaceListBuffer = nullptr;
}

void FInstantRdvBbv::ExecuteGeometryUpdate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    bool bEnableMainViewGeometryInjection,
    bool bEnableMainViewGeometryRemoval)
{
    if (SceneDepthTexture == nullptr)
    {
        return;
    }

    const uint32 BitmaskElementCount = Config.GetBitmaskElementCount();
    const uint32 BrickDataElementCount = Config.GetBrickDataElementCount();
    const uint32 HiBrickDataElementCount = Config.GetHiBrickDataElementCount();
    const uint32 OptionalDataElementCount = Config.GetOptionalDataElementCount();
    const uint32 RadianceAccumElementCount = Config.GetBbvBrickCount() * kBbvRadianceAccumComponentCount;
    const uint32 BrickCount = Config.GetBbvBrickCount();
    const uint32 BitmaskWordsPerBrick = Config.GetBitmaskU32CountPerBrick();

    const bool bNeedRecreateBuffers =
        !ResourceCache.BitmaskBuffer.IsValid() ||
        !ResourceCache.BrickDataBuffer.IsValid() ||
        !ResourceCache.HiBrickDataBuffer.IsValid() ||
        !ResourceCache.OptionalDataBuffer.IsValid() ||
        !ResourceCache.RadianceAccumBuffer.IsValid() ||
        ResourceCache.CachedBitmaskElements != BitmaskElementCount ||
        ResourceCache.CachedBrickDataElements != BrickDataElementCount ||
        ResourceCache.CachedHiBrickDataElements != HiBrickDataElementCount ||
        ResourceCache.CachedOptionalDataElements != OptionalDataElementCount ||
        ResourceCache.CachedRadianceAccumElements != RadianceAccumElementCount;

    FRDGBufferRef BitmaskBuffer = nullptr;
    FRDGBufferRef BrickDataBuffer = nullptr;
    FRDGBufferRef HiBrickDataBuffer = nullptr;
    FRDGBufferRef OptionalDataBuffer = nullptr;
    FRDGBufferRef RadianceAccumBuffer = nullptr;

    if (bNeedRecreateBuffers)
    {
        BitmaskBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BitmaskElementCount), TEXT("InstantRdv.BbvBitmaskBuffer"));
        BrickDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BrickDataElementCount), TEXT("InstantRdv.BbvBrickDataBuffer"));
        HiBrickDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), HiBrickDataElementCount), TEXT("InstantRdv.BbvHiBrickDataBuffer"));
        OptionalDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), OptionalDataElementCount), TEXT("InstantRdv.BbvOptionalDataBuffer"));
        RadianceAccumBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RadianceAccumElementCount), TEXT("InstantRdv.BbvRadianceAccumBuffer"));

        // 再生成直後の pooled buffer は produced 扱いではないため、
        // extraction 前に明示的に書き込み（clear）して RDG validation を満たす。
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BitmaskBuffer), 0u);
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BrickDataBuffer), 0u);
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HiBrickDataBuffer), 0u);
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OptionalDataBuffer), 0u);
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RadianceAccumBuffer), 0u);

        GraphBuilder.QueueBufferExtraction(BitmaskBuffer, &ResourceCache.BitmaskBuffer);
        GraphBuilder.QueueBufferExtraction(BrickDataBuffer, &ResourceCache.BrickDataBuffer);
        GraphBuilder.QueueBufferExtraction(HiBrickDataBuffer, &ResourceCache.HiBrickDataBuffer);
        GraphBuilder.QueueBufferExtraction(OptionalDataBuffer, &ResourceCache.OptionalDataBuffer);
        GraphBuilder.QueueBufferExtraction(RadianceAccumBuffer, &ResourceCache.RadianceAccumBuffer);
        ResourceCache.CachedBitmaskElements = BitmaskElementCount;
        ResourceCache.CachedBrickDataElements = BrickDataElementCount;
        ResourceCache.CachedHiBrickDataElements = HiBrickDataElementCount;
        ResourceCache.CachedOptionalDataElements = OptionalDataElementCount;
        ResourceCache.CachedRadianceAccumElements = RadianceAccumElementCount;
    }
    else
    {
        BitmaskBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.BitmaskBuffer, TEXT("InstantRdv.BbvBitmaskBuffer"));
        BrickDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.BrickDataBuffer, TEXT("InstantRdv.BbvBrickDataBuffer"));
        HiBrickDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.HiBrickDataBuffer, TEXT("InstantRdv.BbvHiBrickDataBuffer"));
        OptionalDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.OptionalDataBuffer, TEXT("InstantRdv.BbvOptionalDataBuffer"));
        RadianceAccumBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.RadianceAccumBuffer, TEXT("InstantRdv.BbvRadianceAccumBuffer"));
    }

    const FVector GridExtentCm = FVector(Config.BbvGridResolution) * Config.BbvBrickSizeCm;
    const FVector GridHalfExtentCm = GridExtentCm * 0.5f;
    const float Cell = FMath::Max(Config.BbvBrickSizeCm, 0.001f);
    const FVector DesiredGridMin = View.ViewLocation - GridHalfExtentCm;
    const FIntVector DesiredMinCell(FMath::FloorToInt(DesiredGridMin.X / Cell), FMath::FloorToInt(DesiredGridMin.Y / Cell), FMath::FloorToInt(DesiredGridMin.Z / Cell));
    if (!ResourceCache.bGridOriginInitialized)
    {
        ResourceCache.GridMinCell = DesiredMinCell;
        ResourceCache.GridMinPositionWs = FVector(ResourceCache.GridMinCell) * Cell;
        ResourceCache.ToroidalOffsetCells = FIntVector::ZeroValue;
        ResourceCache.bGridOriginInitialized = true;
    }

    const FIntVector CurrentMinCell = ResourceCache.GridMinCell;
    const FIntVector GridMoveCellDelta = DesiredMinCell - CurrentMinCell;
    if (!GridMoveCellDelta.IsZero())
    {
        ResourceCache.GridMinCell = DesiredMinCell;
        ResourceCache.GridMinPositionWs = FVector(ResourceCache.GridMinCell) * Cell;
        auto WrapOffset = [](int32 Base, int32 Delta, int32 Mod)->int32
        {
            const int32 Raw = (Base + Delta) % Mod;
            return (Raw < 0) ? (Raw + Mod) : Raw;
        };
        ResourceCache.ToroidalOffsetCells = FIntVector(
            WrapOffset(ResourceCache.ToroidalOffsetCells.X, GridMoveCellDelta.X, Config.BbvGridResolution.X),
            WrapOffset(ResourceCache.ToroidalOffsetCells.Y, GridMoveCellDelta.Y, Config.BbvGridResolution.Y),
            WrapOffset(ResourceCache.ToroidalOffsetCells.Z, GridMoveCellDelta.Z, Config.BbvGridResolution.Z));
    }

    FRDGBufferRef FrustumBrickCounterBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("InstantRdv.BbvFrustumBrickCounter"));
    FRDGBufferRef FrustumBrickListBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BrickCount), TEXT("InstantRdv.BbvFrustumBrickList"));
    FRDGBufferRef FrustumBrickIndirectArgBuffer = nullptr;

    const bool bForceReset = (CVarInstantRdvBbvReset.GetValueOnRenderThread() != 0);
    const bool bNeedInitialize = bNeedRecreateBuffers || !ResourceCache.bInitialized || bForceReset;
    if (bNeedInitialize)
    {
        const uint32 TotalElementCount = FMath::Max(FMath::Max(BitmaskElementCount, BrickDataElementCount), FMath::Max(HiBrickDataElementCount, OptionalDataElementCount));
        const uint32 ThreadGroupCount = FMath::DivideAndRoundUp(TotalElementCount, 64u);
        const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(static_cast<int32>(ThreadGroupCount));

        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RadianceAccumBuffer), 0u);

        FInstantRdvBbvBeginUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvBeginUpdateCS::FParameters>();
        {
            Parameters->BitmaskElementCount = BitmaskElementCount;
            Parameters->BrickDataElementCount = BrickDataElementCount;
            Parameters->HiBrickDataElementCount = HiBrickDataElementCount;
            Parameters->OptionalDataElementCount = OptionalDataElementCount;
            Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(BitmaskBuffer);
            Parameters->RWBrickData = GraphBuilder.CreateUAV(BrickDataBuffer);
            Parameters->RWHiBrickData = GraphBuilder.CreateUAV(HiBrickDataBuffer);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(OptionalDataBuffer);
            Parameters->DispatchLinearWidth = FComputeShaderUtils::WrappedGroupStride * 64u;
            Parameters->DispatchThreadCount = Parameters->DispatchLinearWidth * GroupCount.Y * GroupCount.Z;
        }
        TShaderMapRef<FInstantRdvBbvBeginUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvBeginUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, GroupCount);
    }

    {
        FInstantRdvBbvBeginViewUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvBeginViewUpdateCS::FParameters>();
        {
            Parameters->RWFrustumBrickCounter = GraphBuilder.CreateUAV(FrustumBrickCounterBuffer);
        }
        TShaderMapRef<FInstantRdvBbvBeginViewUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvBeginViewUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(1, 1, 1));
    }

    if (bEnableMainViewGeometryInjection)
    {
        FInstantRdvBbvDepthInjectionCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthInjectionCS::FParameters>();
        {
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->GridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
            Parameters->GridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->BbvPerVoxelResolution = Config.BbvPerVoxelResolution;
            Parameters->ToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
            Parameters->GridMinPositionWs = FVector3f(ResourceCache.GridMinPositionWs);
            Parameters->CellSizeCm = Config.BbvBrickSizeCm;
            // 参照実装と同じく「fine cell 数」からワールド距離を算出する。
            const float InjectionOffsetFineCells = CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            const float FineCellSizeCm = Config.BbvBrickSizeCm / FMath::Max(static_cast<float>(Config.BbvPerVoxelResolution), 1.0f);
            Parameters->DepthtestInjectionWorldOffsetWs = FineCellSizeCm * InjectionOffsetFineCells;
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(BitmaskBuffer);
        }
        TShaderMapRef<FInstantRdvBbvDepthInjectionCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvDepthInjectionMainView"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
    }

    if (!GridMoveCellDelta.IsZero())
    {
        FInstantRdvBbvToroidalClearCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvToroidalClearCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->GridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
            Parameters->GridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->OptionalDataU32Count = Config.OptionalDataU32Count;
            Parameters->GridMoveDeltaCells = FIntVector4(GridMoveCellDelta.X, GridMoveCellDelta.Y, GridMoveCellDelta.Z, 0);
            Parameters->ToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
            Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(BitmaskBuffer);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(OptionalDataBuffer);
        }
        TShaderMapRef<FInstantRdvBbvToroidalClearCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvToroidalClear"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    if (bEnableMainViewGeometryRemoval)
    {
        {
            FInstantRdvBbvDepthFrustumCullCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthFrustumCullCS::FParameters>();
            {
                Parameters->BrickCount = BrickCount;
                Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
                Parameters->GridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
                Parameters->GridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
                Parameters->GridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
                Parameters->ToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
                Parameters->GridMinPositionWs = FVector3f(ResourceCache.GridMinPositionWs);
                Parameters->CellSizeCm = Config.BbvBrickSizeCm;
                Parameters->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
                Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(BitmaskBuffer);
                Parameters->RWFrustumBrickCounter = GraphBuilder.CreateUAV(FrustumBrickCounterBuffer);
                Parameters->RWFrustumBrickList = GraphBuilder.CreateUAV(FrustumBrickListBuffer);
            }
            TShaderMapRef<FInstantRdvBbvDepthFrustumCullCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvDepthFrustumCull"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
        }

        // Frustum候補数から、Carving(1thread=1u32 job)向けのDispatchIndirect引数を生成する。
        FrustumBrickIndirectArgBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
            TEXT("InstantRdv.BbvFrustumBrickIndirectArg"));
        {
            FInstantRdvBbvDepthCarvingIndirectArgBuildCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDepthCarvingIndirectArgBuildCS::FParameters>();
            {
                Parameters->FrustumBrickCounter = GraphBuilder.CreateSRV(FrustumBrickCounterBuffer);
                Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
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
                Parameters->GridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
                Parameters->GridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
                Parameters->GridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
                Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
                Parameters->BbvPerVoxelResolution = Config.BbvPerVoxelResolution;
                Parameters->ToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
                Parameters->GridMinPositionWs = FVector3f(ResourceCache.GridMinPositionWs);
                Parameters->CellSizeCm = Config.BbvBrickSizeCm;
                Parameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());
                Parameters->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
                Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
                Parameters->SceneDepthTexture = SceneDepthTexture;
                Parameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                Parameters->FrustumBrickCounter = GraphBuilder.CreateSRV(FrustumBrickCounterBuffer);
                Parameters->FrustumBrickList = GraphBuilder.CreateSRV(FrustumBrickListBuffer);
                Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(BitmaskBuffer);
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
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(BitmaskBuffer);
            Parameters->RWBrickData = GraphBuilder.CreateUAV(BrickDataBuffer);
        }
        TShaderMapRef<FInstantRdvBbvBrickCountAggregateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvBrickCountAggregate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    {
        FInstantRdvBbvElementUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvElementUpdateCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->OptionalDataU32Count = Config.OptionalDataU32Count;
            Parameters->BbvPerVoxelResolution = Config.BbvPerVoxelResolution;
            Parameters->FrameCount = ResourceCache.FrameCount++;
            Parameters->UpdateSkipCount = kBbvElementUpdateSkipCount;
            Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(BitmaskBuffer);
            Parameters->BrickData = GraphBuilder.CreateSRV(BrickDataBuffer);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(OptionalDataBuffer);
        }
        TShaderMapRef<FInstantRdvBbvElementUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 PerFrameCount = FMath::DivideAndRoundUp(BrickCount, kBbvElementUpdateSkipCount + 1u);
        const uint32 GroupX = FMath::DivideAndRoundUp(PerFrameCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvElementUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    ResourceCache.FrameBitmaskBuffer = BitmaskBuffer;
    ResourceCache.FrameBrickDataBuffer = BrickDataBuffer;
    ResourceCache.FrameHiBrickDataBuffer = HiBrickDataBuffer;
    ResourceCache.FrameOptionalDataBuffer = OptionalDataBuffer;
    ResourceCache.FrameRadianceAccumBuffer = RadianceAccumBuffer;
    ResourceCache.bInitialized = true;
}

void FInstantRdvBbv::ExecuteRadianceUpdate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    FRDGTexture* SceneColorTexture,
    float SceneColorPreExposure,
    bool bEnableRadianceInjection,
    bool bEnableRadianceResolve)
{
    if (SceneDepthTexture == nullptr || SceneColorTexture == nullptr || !ResourceCache.bInitialized)
    {
        return;
    }

    FRDGBufferRef BitmaskBuffer = ResourceCache.FrameBitmaskBuffer;
    FRDGBufferRef OptionalDataBuffer = ResourceCache.FrameOptionalDataBuffer;
    FRDGBufferRef RadianceAccumBuffer = ResourceCache.FrameRadianceAccumBuffer;

    if (BitmaskBuffer == nullptr && ResourceCache.BitmaskBuffer.IsValid())
    {
        BitmaskBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.BitmaskBuffer, TEXT("InstantRdv.BbvBitmaskBuffer"));
    }
    if (OptionalDataBuffer == nullptr && ResourceCache.OptionalDataBuffer.IsValid())
    {
        OptionalDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.OptionalDataBuffer, TEXT("InstantRdv.BbvOptionalDataBuffer"));
    }
    if (RadianceAccumBuffer == nullptr && ResourceCache.RadianceAccumBuffer.IsValid())
    {
        RadianceAccumBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.RadianceAccumBuffer, TEXT("InstantRdv.BbvRadianceAccumBuffer"));
    }
    if (BitmaskBuffer == nullptr || OptionalDataBuffer == nullptr || RadianceAccumBuffer == nullptr)
    {
        return;
    }

    const uint32 BrickCount = Config.GetBbvBrickCount();
    const uint32 BitmaskWordsPerBrick = Config.GetBitmaskU32CountPerBrick();
    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);

    if (bEnableRadianceInjection)
    {
        FInstantRdvBbvRadianceInjectionCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvRadianceInjectionCS::FParameters>();
        {
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->GridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
            Parameters->GridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->BbvPerVoxelResolution = Config.BbvPerVoxelResolution;
            Parameters->ToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
            Parameters->GridMinPositionWs = FVector3f(ResourceCache.GridMinPositionWs);
            Parameters->CellSizeCm = Config.BbvBrickSizeCm;
            const float InjectionOffsetFineCells = CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            const float FineCellSizeCm = Config.BbvBrickSizeCm / FMath::Max(static_cast<float>(Config.BbvPerVoxelResolution), 1.0f);
            Parameters->DepthtestInjectionWorldOffsetWs = FineCellSizeCm * InjectionOffsetFineCells;
            Parameters->SceneColorPreExposure = FMath::Max(SceneColorPreExposure, 1.0e-6f);
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->SceneColorTexture = SceneColorTexture;
            Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(BitmaskBuffer);
            Parameters->RWBbvRadianceAccumBuffer = GraphBuilder.CreateUAV(RadianceAccumBuffer);
        }
        TShaderMapRef<FInstantRdvBbvRadianceInjectionCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvRadianceInjection"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
    }

    if (bEnableRadianceResolve)
    {
        FInstantRdvBbvRadianceResolveCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvRadianceResolveCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->OptionalDataU32Count = Config.OptionalDataU32Count;
            Parameters->RWBbvRadianceAccumBuffer = GraphBuilder.CreateUAV(RadianceAccumBuffer);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(OptionalDataBuffer);
        }
        TShaderMapRef<FInstantRdvBbvRadianceResolveCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(BrickCount, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvRadianceResolve"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    ResourceCache.FrameOptionalDataBuffer = OptionalDataBuffer;
    ResourceCache.FrameRadianceAccumBuffer = RadianceAccumBuffer;
}

void FInstantRdvBbv::ExecuteFspUpdate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    bool bEnableFspUpdate)
{
    if (!bEnableFspUpdate || SceneDepthTexture == nullptr || !ResourceCache.bInitialized)
    {
        return;
    }

    FRDGBufferRef OptionalDataBuffer = ResourceCache.FrameOptionalDataBuffer;
    if (OptionalDataBuffer == nullptr && ResourceCache.OptionalDataBuffer.IsValid())
    {
        OptionalDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.OptionalDataBuffer, TEXT("InstantRdv.BbvOptionalDataBuffer"));
    }
    if (OptionalDataBuffer == nullptr)
    {
        return;
    }

    const uint32 FspCellCount =
        static_cast<uint32>(Config.ProbeGridResolution.X) *
        static_cast<uint32>(Config.ProbeGridResolution.Y) *
        static_cast<uint32>(Config.ProbeGridResolution.Z);
    const uint32 FspCellDataElementCount = FspCellCount * kFspCellDataU32Count;
    const uint32 FspVisibleSurfaceListElementCount = FspCellCount + 1u;

    const bool bNeedRecreateFspBuffers =
        !ResourceCache.FspCellDataBuffer.IsValid() ||
        !ResourceCache.FspVisibleSurfaceListBuffer.IsValid() ||
        ResourceCache.CachedFspCellDataElements != FspCellDataElementCount ||
        ResourceCache.CachedFspVisibleSurfaceListElements != FspVisibleSurfaceListElementCount;

    FRDGBufferRef FspCellDataBuffer = nullptr;
    FRDGBufferRef FspVisibleSurfaceListBuffer = nullptr;
    if (bNeedRecreateFspBuffers)
    {
        FspCellDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellDataElementCount), TEXT("InstantRdv.FspCellDataBuffer"));
        FspVisibleSurfaceListBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspVisibleSurfaceListElementCount), TEXT("InstantRdv.FspVisibleSurfaceListBuffer"));
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FspCellDataBuffer), 0u);
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FspVisibleSurfaceListBuffer), 0u);
        GraphBuilder.QueueBufferExtraction(FspCellDataBuffer, &ResourceCache.FspCellDataBuffer);
        GraphBuilder.QueueBufferExtraction(FspVisibleSurfaceListBuffer, &ResourceCache.FspVisibleSurfaceListBuffer);
        ResourceCache.CachedFspCellDataElements = FspCellDataElementCount;
        ResourceCache.CachedFspVisibleSurfaceListElements = FspVisibleSurfaceListElementCount;
    }
    else
    {
        FspCellDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.FspCellDataBuffer, TEXT("InstantRdv.FspCellDataBuffer"));
        FspVisibleSurfaceListBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.FspVisibleSurfaceListBuffer, TEXT("InstantRdv.FspVisibleSurfaceListBuffer"));
    }

    // 参照実装の FspBeginUpdate に相当する最小初期化。
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FspVisibleSurfaceListBuffer), 0u);

    const FVector FspGridExtentCm = FVector(Config.ProbeGridResolution) * Config.ProbeCellSizeCm;
    const FVector FspGridMinPositionWs = View.ViewLocation - FspGridExtentCm * 0.5f;
    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);

    {
        FInstantRdvFspScreenSpaceCollectCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspScreenSpaceCollectCS::FParameters>();
        Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
        Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
        Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
        Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
        Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
        Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
        Parameters->FspGridResolutionX = static_cast<uint32>(Config.ProbeGridResolution.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(Config.ProbeGridResolution.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(Config.ProbeGridResolution.Z);
        Parameters->FspVisibleSurfaceListCapacity = FspVisibleSurfaceListElementCount - 1u;
        Parameters->FspGridMinPositionWs = FVector3f(FspGridMinPositionWs);
        Parameters->FspCellSizeCm = Config.ProbeCellSizeCm;
        Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
        Parameters->SceneDepthTexture = SceneDepthTexture;
        Parameters->RWFspCellData = GraphBuilder.CreateUAV(FspCellDataBuffer);
        Parameters->RWFspVisibleSurfaceList = GraphBuilder.CreateUAV(FspVisibleSurfaceListBuffer);
        TShaderMapRef<FInstantRdvFspScreenSpaceCollectCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspScreenSpaceCollect"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
    }

    {
        FInstantRdvFspRadianceUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspRadianceUpdateCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(Config.ProbeGridResolution.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(Config.ProbeGridResolution.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(Config.ProbeGridResolution.Z);
        Parameters->FspCellDataU32Count = kFspCellDataU32Count;
        Parameters->FspVisibleSurfaceListCapacity = FspVisibleSurfaceListElementCount - 1u;
        Parameters->BbvGridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
        Parameters->BbvGridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
        Parameters->BbvGridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
        Parameters->BbvOptionalDataU32Count = Config.OptionalDataU32Count;
        Parameters->FspGridMinPositionWs = FVector3f(FspGridMinPositionWs);
        Parameters->FspCellSizeCm = Config.ProbeCellSizeCm;
        Parameters->BbvGridMinPositionWs = FVector3f(ResourceCache.GridMinPositionWs);
        Parameters->BbvCellSizeCm = Config.BbvBrickSizeCm;
        Parameters->BbvToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
        Parameters->BbvOptionalData = GraphBuilder.CreateSRV(OptionalDataBuffer);
        Parameters->RWFspCellData = GraphBuilder.CreateUAV(FspCellDataBuffer);
        Parameters->FspVisibleSurfaceList = GraphBuilder.CreateSRV(FspVisibleSurfaceListBuffer);
        TShaderMapRef<FInstantRdvFspRadianceUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(FspVisibleSurfaceListElementCount - 1u, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspRadianceUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }

    ResourceCache.FrameFspCellDataBuffer = FspCellDataBuffer;
    ResourceCache.FrameFspVisibleSurfaceListBuffer = FspVisibleSurfaceListBuffer;
}

void FInstantRdvBbv::ExecuteDebugVisualize(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    FRDGTexture* SceneDepthTexture,
    FRDGTexture* SceneColorTexture,
    float SceneColorPreExposure,
    int32 DebugMode)
{
    if (DebugMode <= 0 || SceneDepthTexture == nullptr || SceneColorTexture == nullptr)
    {
        return;
    }

    FRDGBufferRef BitmaskBuffer = ResourceCache.FrameBitmaskBuffer;
    FRDGBufferRef BrickDataBuffer = ResourceCache.FrameBrickDataBuffer;
    FRDGBufferRef OptionalDataBuffer = ResourceCache.FrameOptionalDataBuffer;

    if (BitmaskBuffer == nullptr && ResourceCache.BitmaskBuffer.IsValid())
    {
        BitmaskBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.BitmaskBuffer, TEXT("InstantRdv.BbvBitmaskBuffer"));
    }
    if (BrickDataBuffer == nullptr && ResourceCache.BrickDataBuffer.IsValid())
    {
        BrickDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.BrickDataBuffer, TEXT("InstantRdv.BbvBrickDataBuffer"));
    }
    if (OptionalDataBuffer == nullptr && ResourceCache.OptionalDataBuffer.IsValid())
    {
        OptionalDataBuffer = GraphBuilder.RegisterExternalBuffer(ResourceCache.OptionalDataBuffer, TEXT("InstantRdv.BbvOptionalDataBuffer"));
    }
    if (BitmaskBuffer == nullptr || BrickDataBuffer == nullptr || OptionalDataBuffer == nullptr)
    {
        return;
    }

    FRDGTextureRef SceneColorInputTexture = GraphBuilder.CreateTexture(SceneColorTexture->Desc, TEXT("InstantRdv.BbvDebugSceneColorInput"));
    AddCopyTexturePass(GraphBuilder, SceneColorTexture, SceneColorInputTexture);

    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
    FInstantRdvBbvDebugVisualizePS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvDebugVisualizePS::FParameters>();
    {
        Parameters->SceneDepthTexture = SceneDepthTexture;
        Parameters->SceneColorTexture = SceneColorInputTexture;
        Parameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
        Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
        Parameters->SceneColorPreExposure = FMath::Max(SceneColorPreExposure, 1.0e-6f);
        Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
        Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
        Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
        Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
        Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
        Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
        Parameters->GridResolutionX = static_cast<uint32>(Config.BbvGridResolution.X);
        Parameters->GridResolutionY = static_cast<uint32>(Config.BbvGridResolution.Y);
        Parameters->GridResolutionZ = static_cast<uint32>(Config.BbvGridResolution.Z);
        Parameters->BitmaskWordsPerBrick = Config.GetBitmaskU32CountPerBrick();
        Parameters->BbvPerVoxelResolution = Config.BbvPerVoxelResolution;
        Parameters->ToroidalOffsetCells = FVector3f(ResourceCache.ToroidalOffsetCells);
        Parameters->CellSizeCm = Config.BbvBrickSizeCm;
        Parameters->GridMinPositionWs = FVector3f(ResourceCache.GridMinPositionWs);
        Parameters->MaxTraceDistanceCm = Config.BbvBrickSizeCm * static_cast<float>(Config.BbvGridResolution.GetMax());
        Parameters->DebugMode = DebugMode;
        Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(BitmaskBuffer);
        Parameters->BrickData = GraphBuilder.CreateSRV(BrickDataBuffer);
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
