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
#include "ScreenRendering.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"

namespace
{
static constexpr uint32 kBbvElementUpdateSkipCount = 3;

// Minimal vertex declaration that represents "no vertex attributes".
// Used for SV_VertexID-driven shaders that compute positions from VertexID and external buffers.
class FInstantRdvNullVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		FVertexDeclarationElementList Elements; // empty list
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

//TGlobalResource<FInstantRdvNullVertexDeclaration> GInstantRdvNullVertexDeclaration;
TGlobalResource<FEmptyVertexDeclaration, FRenderResource::EInitPhase::Pre> GInstantRdvNullVertexDeclaration;


static constexpr uint32 kBbvRadianceAccumComponentCount = 4;
static constexpr uint32 kFspProbeDataU32Count = 4;// Fsp Radiance Bufferの1Probeあたりのu32数
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
    TEXT("Depthtest Injectionの視線奥オフセット量（fine cell単位）。\n参照実装準拠で、実際の距離は CellSizeCm * (FineCells / BbvPerBrickResolution) で算出。"),
    ECVF_RenderThreadSafe);
// 移植ミス再発防止:
// - 参照実装は「固定m値」ではなく fine cell 基準でオフセット量を決める。
// - UE側はワールド単位がcmのため、シェーダへ渡す前に必ず CellSizeCm/BbvPerBrickResolution で換算する。
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
        SHADER_PARAMETER(uint32, BbvPerBrickResolution)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
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
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
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
        SHADER_PARAMETER(uint32, BbvPerBrickResolution)
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
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
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
        SHADER_PARAMETER(uint32, BbvPerBrickResolution)
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
        SHADER_PARAMETER(uint32, BbvPerBrickResolution)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
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

class FInstantRdvFspInitPoolCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspInitPoolCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspInitPoolCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, ProbePoolElementCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWFspProbeFreeStack)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspRayCaptureCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspRayCaptureCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspRayCaptureCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FspVisibleSurfaceList)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWFspProbeRadiance)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspShUpdateCS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspShUpdateCS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspShUpdateCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FspProbeRadiance)
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
        SHADER_PARAMETER(uint32, BbvPerBrickResolution)
        SHADER_PARAMETER(FVector3f, BbvToroidalOffsetCells)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(FVector3f, BbvGridMinPositionWs)
        SHADER_PARAMETER(float, MaxTraceDistanceCm)
        SHADER_PARAMETER(int32, DebugMode)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BitmaskBrickVoxel)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickData)
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
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
        SHADER_PARAMETER(float, ViewAcpectRatio)
        SHADER_PARAMETER(FVector3f, FspGridMinPositionWs)
        SHADER_PARAMETER(float, CellSizeCm)
        SHADER_PARAMETER(uint32, GridResolutionX)
        SHADER_PARAMETER(uint32, GridResolutionY)
        SHADER_PARAMETER(uint32, GridResolutionZ)
    END_SHADER_PARAMETER_STRUCT()
};

class FInstantRdvFspProbeBillboardPS final : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FInstantRdvFspProbeBillboardPS);
    SHADER_USE_PARAMETER_STRUCT(FInstantRdvFspProbeBillboardPS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RENDER_TARGET_BINDING_SLOTS()
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FspProbeRadiance)
    END_SHADER_PARAMETER_STRUCT()
};

BEGIN_SHADER_PARAMETER_STRUCT(FInstantRdvFspProbeBillboard_Parameters, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstantRdvFspProbeBillboardVS::FParameters, VS)
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstantRdvFspProbeBillboardPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()




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
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspInitPoolCS, "/InstantRdvShaders/Private/InstantRdv/fsp_init_pool_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspRayCaptureCS, "/InstantRdvShaders/Private/InstantRdv/fsp_ray_capture_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspShUpdateCS, "/InstantRdvShaders/Private/InstantRdv/fsp_sh_update_cs.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvBbvDebugVisualizePS, "/InstantRdvShaders/Private/InstantRdv/bbv_debug_visualize_ps.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeBillboardVS, "/InstantRdvShaders/Private/InstantRdv/fsp_probe_billboard.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FInstantRdvFspProbeBillboardPS, "/InstantRdvShaders/Private/InstantRdv/fsp_probe_billboard.usf", "MainPS", SF_Pixel);
} // namespace



uint32 FInstantRdvBbvConfig::GetBbvBrickCount() const
{
    return static_cast<uint32>(BbvGridResolution.X) * static_cast<uint32>(BbvGridResolution.Y) * static_cast<uint32>(BbvGridResolution.Z);
}

uint32 FInstantRdvBbvConfig::GetBitmaskU32CountPerBrick() const
{
    const uint32 PerBrickBitCount = BbvPerBrickResolution * BbvPerBrickResolution * BbvPerBrickResolution;
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
uint32 FInstantRdvBbvConfig::GetRadianceAccumDataElementCount() const
{
    return GetBbvBrickCount() * kBbvRadianceAccumComponentCount;
}



uint32 FInstantRdvFspConfig::GetFspCellCount() const
{
    return static_cast<uint32>(ProbeGridResolution.X * ProbeGridResolution.Y * ProbeGridResolution.Z);
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
            const uint32 BitmaskElementCount = Config.bbv.GetBitmaskElementCount();
            const uint32 BrickDataElementCount = Config.bbv.GetBrickDataElementCount();
            const uint32 HiBrickDataElementCount = Config.bbv.GetHiBrickDataElementCount();
            const uint32 OptionalDataElementCount = Config.bbv.GetOptionalDataElementCount();
            const uint32 RadianceAccumElementCount = Config.bbv.GetRadianceAccumDataElementCount();
            const uint32 BrickCount = Config.bbv.GetBbvBrickCount();
            const uint32 BitmaskWordsPerBrick = Config.bbv.GetBitmaskU32CountPerBrick();


            // RDGPool上に確保. この時点ではまだフレーム(RDG)寿命のリソース.
            SystemState.bbv.BitmaskBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BitmaskElementCount), TEXT("InstantRdv.BbvBitmaskBuffer"));
            SystemState.bbv.BrickDataBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BrickDataElementCount), TEXT("InstantRdv.BbvBrickDataBuffer"));
            SystemState.bbv.HiBrickDataBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), HiBrickDataElementCount), TEXT("InstantRdv.BbvHiBrickDataBuffer"));
            SystemState.bbv.OptionalDataBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), OptionalDataElementCount), TEXT("InstantRdv.BbvOptionalDataBuffer"));
            SystemState.bbv.RadianceAccumBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RadianceAccumElementCount), TEXT("InstantRdv.BbvRadianceAccumBuffer"));

            // 再生成直後の pooled buffer は produced 扱いではないため、
            // extraction 前に明示的に書き込み（clear）して RDG validation をパスさせる  本当に必要か?要確認.
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.BitmaskBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.BrickDataBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.HiBrickDataBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.OptionalDataBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.RadianceAccumBuffer.Handle), 0u);

            // Extractionして永続化.
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.BitmaskBuffer.Handle, &SystemState.bbv.BitmaskBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.BrickDataBuffer.Handle, &SystemState.bbv.BrickDataBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.HiBrickDataBuffer.Handle, &SystemState.bbv.HiBrickDataBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.OptionalDataBuffer.Handle, &SystemState.bbv.OptionalDataBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.bbv.RadianceAccumBuffer.Handle, &SystemState.bbv.RadianceAccumBuffer.PooledBuffer);
        }

        // Fsp
        {
            const uint32 FspCellCount =  SystemState.fsp.TrGrid.GetCellCount();


            SystemState.fsp.FspCellDataBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount * kFspProbeDataU32Count), TEXT("InstantRdv.fsp.FspCellDataBuffer"));
            SystemState.fsp.FspVisibleSurfaceListBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1), TEXT("InstantRdv.fsp.FspVisibleSurfaceListBuffer"));
            SystemState.fsp.FspProbePoolBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount), TEXT("InstantRdv.fsp.FspProbePoolBuffer"));
            SystemState.fsp.FspProbeFreeStackBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.fsp.FspProbeFreeStackBuffer"));
            SystemState.fsp.FspActiveProbeListPrevBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.FspActiveProbeListPrev"));
            SystemState.fsp.FspActiveProbeListCurrBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FspCellCount + 1u), TEXT("InstantRdv.FspActiveProbeListCurr"));
            SystemState.fsp.FspProbeRadianceBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * kFspProbeDataU32Count, SystemState.fsp.TrGrid.GetCellCount()), TEXT("InstantRdv.fsp.FspProbeRadianceBuffer"));
            SystemState.fsp.FspPackedSHBuffer.Handle = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float) * 4, SystemState.fsp.TrGrid.GetCellCount()), TEXT("InstantRdv.fsp.FspPackedSHBuffer"));


            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspCellDataBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbePoolBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeFreeStackBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspActiveProbeListPrevBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspActiveProbeListCurrBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRadianceBuffer.Handle), 0u);
            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle), 0u);


            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspCellDataBuffer.Handle, &SystemState.fsp.FspCellDataBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle, &SystemState.fsp.FspVisibleSurfaceListBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbePoolBuffer.Handle, &SystemState.fsp.FspProbePoolBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbeFreeStackBuffer.Handle, &SystemState.fsp.FspProbeFreeStackBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspActiveProbeListPrevBuffer.Handle, &SystemState.fsp.FspActiveProbeListPrevBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspActiveProbeListCurrBuffer.Handle, &SystemState.fsp.FspActiveProbeListCurrBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspProbeRadianceBuffer.Handle, &SystemState.fsp.FspProbeRadianceBuffer.PooledBuffer);
            GraphBuilder.QueueBufferExtraction(SystemState.fsp.FspPackedSHBuffer.Handle, &SystemState.fsp.FspPackedSHBuffer.PooledBuffer);
        }
    }
    else
    {
        // フレーム加算.
        ++SystemState.FrameCount;

        // Pool されているリソースをこのフレーム用にRDGにRegisterしてハンドル更新.
        
        // Bbv
        {
            SystemState.bbv.BitmaskBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.BitmaskBuffer.PooledBuffer, TEXT("InstantRdv.BbvBitmaskBuffer"));
            SystemState.bbv.BrickDataBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.BrickDataBuffer.PooledBuffer, TEXT("InstantRdv.BbvBrickDataBuffer"));
            SystemState.bbv.HiBrickDataBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.HiBrickDataBuffer.PooledBuffer, TEXT("InstantRdv.BbvHiBrickDataBuffer"));
            SystemState.bbv.OptionalDataBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.OptionalDataBuffer.PooledBuffer, TEXT("InstantRdv.BbvOptionalDataBuffer"));
            SystemState.bbv.RadianceAccumBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.bbv.RadianceAccumBuffer.PooledBuffer, TEXT("InstantRdv.BbvRadianceAccumBuffer"));
        }

        // Fsp
        {
            SystemState.fsp.FspCellDataBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspCellDataBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspCellDataBuffer"));
            SystemState.fsp.FspVisibleSurfaceListBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspVisibleSurfaceListBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspVisibleSurfaceListBuffer"));
            SystemState.fsp.FspProbePoolBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbePoolBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbePoolBuffer"));
            SystemState.fsp.FspProbeFreeStackBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbeFreeStackBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbeFreeStackBuffer"));
            SystemState.fsp.FspActiveProbeListPrevBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspActiveProbeListPrevBuffer.PooledBuffer, TEXT("InstantRdv.FspActiveProbeListPrev"));
            SystemState.fsp.FspActiveProbeListCurrBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspActiveProbeListCurrBuffer.PooledBuffer, TEXT("InstantRdv.FspActiveProbeListCurr"));
            SystemState.fsp.FspProbeRadianceBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspProbeRadianceBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspProbeRadianceBuffer"));
            SystemState.fsp.FspPackedSHBuffer.Handle = GraphBuilder.RegisterExternalBuffer(SystemState.fsp.FspPackedSHBuffer.PooledBuffer, TEXT("InstantRdv.fsp.FspPackedSHBuffer"));
        }
    }


    // グリッド更新.
    {
        SystemState.bbv.TrGrid.UpdateDelta(InView.ViewLocation, Config.bbv.BbvBrickSizeCm);
        SystemState.fsp.TrGrid.UpdateDelta(InView.ViewLocation, Config.fsp.ProbeCellSizeCm);
    }


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

    const uint32 BrickCount = SystemState.bbv.TrGrid.GetCellCount();
    const uint32 BitmaskElementCount = Config.bbv.GetBitmaskElementCount();
    const uint32 BrickDataElementCount = Config.bbv.GetBrickDataElementCount();
    const uint32 HiBrickDataElementCount = Config.bbv.GetHiBrickDataElementCount();
    const uint32 OptionalDataElementCount = Config.bbv.GetOptionalDataElementCount();
    const uint32 RadianceAccumElementCount = Config.bbv.GetRadianceAccumDataElementCount();
    const uint32 BitmaskWordsPerBrick = Config.bbv.GetBitmaskU32CountPerBrick();


    FRDGBufferRef FrustumBrickCounterBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("InstantRdv.BbvFrustumBrickCounter"));
    FRDGBufferRef FrustumBrickListBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BrickCount), TEXT("InstantRdv.BbvFrustumBrickList"));
    FRDGBufferRef FrustumBrickIndirectArgBuffer = nullptr;

    const bool bForceReset = (CVarInstantRdvBbvReset.GetValueOnRenderThread() != 0);
    // 初回フレームか, 強制された場合.
    const bool bNeedInitialize = (0 == SystemState.FrameCount) || bForceReset;
    if (bNeedInitialize)
    {
        {
            const uint32 TotalElementCount = FMath::Max(FMath::Max(BitmaskElementCount, BrickDataElementCount), FMath::Max(HiBrickDataElementCount, OptionalDataElementCount));
            const uint32 ThreadGroupCount = FMath::DivideAndRoundUp(TotalElementCount, 64u);
            const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(static_cast<int32>(ThreadGroupCount));

            AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.bbv.RadianceAccumBuffer.Handle), 0u);

            FInstantRdvBbvBeginUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvBeginUpdateCS::FParameters>();
            {
                Parameters->BitmaskElementCount = BitmaskElementCount;
                Parameters->BrickDataElementCount = BrickDataElementCount;
                Parameters->HiBrickDataElementCount = HiBrickDataElementCount;
                Parameters->OptionalDataElementCount = OptionalDataElementCount;
                Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(SystemState.bbv.BitmaskBuffer.Handle);
                Parameters->RWBrickData = GraphBuilder.CreateUAV(SystemState.bbv.BrickDataBuffer.Handle);
                Parameters->RWHiBrickData = GraphBuilder.CreateUAV(SystemState.bbv.HiBrickDataBuffer.Handle);
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
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->BbvPerBrickResolution = Config.bbv.BbvPerBrickResolution;
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            // 参照実装と同じく「fine cell 数」からワールド距離を算出する。
            const float InjectionOffsetFineCells = CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            const float FineCellSizeCm = Config.bbv.BbvBrickSizeCm / FMath::Max(static_cast<float>(Config.bbv.BbvPerBrickResolution), 1.0f);
            Parameters->DepthtestInjectionWorldOffsetWs = FineCellSizeCm * InjectionOffsetFineCells;
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
            Parameters->SceneDepthTexture = SceneDepthTexture;
            Parameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(SystemState.bbv.BitmaskBuffer.Handle);
        }
        TShaderMapRef<FInstantRdvBbvDepthInjectionCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.BbvDepthInjectionMainView"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
    }

    if (!SystemState.bbv.TrGrid.FrameCellDelta.IsZero())
    {
        FInstantRdvBbvToroidalClearCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvBbvToroidalClearCS::FParameters>();
        {
            Parameters->BrickCount = BrickCount;
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->OptionalDataU32Count = Config.bbv.OptionalDataU32Count;
            Parameters->GridMoveDeltaCells = FIntVector4(SystemState.bbv.TrGrid.FrameCellDelta, 0);
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(SystemState.bbv.BitmaskBuffer.Handle);
            Parameters->RWBitmaskBrickVoxelOptionData = GraphBuilder.CreateUAV(SystemState.bbv.OptionalDataBuffer.Handle);
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
                Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
                Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
                Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
                Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
                Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
                Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
                Parameters->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetWorldToClip());
                Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(SystemState.bbv.BitmaskBuffer.Handle);
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
                Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
                Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
                Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
                Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
                Parameters->BbvPerBrickResolution = Config.bbv.BbvPerBrickResolution;
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
                Parameters->RWBitmaskBrickVoxel = GraphBuilder.CreateUAV(SystemState.bbv.BitmaskBuffer.Handle);
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
            Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(SystemState.bbv.BitmaskBuffer.Handle);
            Parameters->RWBrickData = GraphBuilder.CreateUAV(SystemState.bbv.BrickDataBuffer.Handle);
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
            Parameters->OptionalDataU32Count = Config.bbv.OptionalDataU32Count;
            Parameters->BbvPerBrickResolution = Config.bbv.BbvPerBrickResolution;
            Parameters->FrameCount = SystemState.FrameCount;
            Parameters->UpdateSkipCount = kBbvElementUpdateSkipCount;
            Parameters->BitmaskBrickVoxel = GraphBuilder.CreateSRV(SystemState.bbv.BitmaskBuffer.Handle);
            Parameters->BrickData = GraphBuilder.CreateSRV(SystemState.bbv.BrickDataBuffer.Handle);
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
    bool bEnableRadianceResolve)
{
    if (SceneDepthTexture == nullptr || SceneColorTexture == nullptr || !SystemState.bRenderInitialized)
    {
        return;
    }

    FRDGBufferRef BitmaskBuffer = SystemState.bbv.BitmaskBuffer.Handle;
    FRDGBufferRef OptionalDataBuffer = SystemState.bbv.OptionalDataBuffer.Handle;
    FRDGBufferRef RadianceAccumBuffer = SystemState.bbv.RadianceAccumBuffer.Handle;

    const uint32 BrickCount = SystemState.bbv.TrGrid.GetCellCount();
    const uint32 BitmaskWordsPerBrick = Config.bbv.GetBitmaskU32CountPerBrick();
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
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BitmaskWordsPerBrick = BitmaskWordsPerBrick;
            Parameters->BbvPerBrickResolution = Config.bbv.BbvPerBrickResolution;
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            const float InjectionOffsetFineCells = CVarInstantRdvBbvDepthtestInjectionOffsetFineCells.GetValueOnRenderThread();
            const float FineCellSizeCm = Config.bbv.BbvBrickSizeCm / FMath::Max(static_cast<float>(Config.bbv.BbvPerBrickResolution), 1.0f);
            Parameters->DepthtestInjectionWorldOffsetWs = FineCellSizeCm * InjectionOffsetFineCells;
            Parameters->SceneColorPreExposure = FMath::Max(SceneColorPreExposure, 1.0e-6f);
            Parameters->CameraPositionWs = FVector3f(View.ViewLocation);
            Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld()); //Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
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
            Parameters->OptionalDataU32Count = Config.bbv.OptionalDataU32Count;
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
    bool bEnableFspUpdate)
{
    if (!bEnableFspUpdate || SceneDepthTexture == nullptr || !SystemState.bRenderInitialized)
    {
        return;
    }

    const bool bNeedInitialize = (0 == SystemState.FrameCount);
    // 初回フレームか, 強制された場合.
    if (bNeedInitialize)
    {
        {
            // Probe free stack を GPU 側で初期化するパスを追加.
            FInstantRdvFspInitPoolCS::FParameters* InitParams = GraphBuilder.AllocParameters<FInstantRdvFspInitPoolCS::FParameters>();
            InitParams->ProbePoolElementCount = SystemState.fsp.TrGrid.GetCellCount();
            InitParams->RWFspProbeFreeStack = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeFreeStackBuffer.Handle);
            TShaderMapRef<FInstantRdvFspInitPoolCS> InitShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 InitGroupX = FMath::DivideAndRoundUp(SystemState.fsp.TrGrid.GetCellCount(), 64);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspInitPool"), ERDGPassFlags::Compute, InitShader, InitParams, FIntVector(InitGroupX, 1, 1));
        }
    }


    FRDGBufferRef OptionalDataBuffer = SystemState.bbv.OptionalDataBuffer.Handle;



    // 参照実装の FspBeginUpdate に相当する最小初期化。
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle), 0u);

    const FVector FspGridExtentCm = FVector(SystemState.fsp.TrGrid.GridReso) * Config.fsp.ProbeCellSizeCm;
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
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspVisibleSurfaceListCapacity = SystemState.fsp.TrGrid.GetCellCount();
        Parameters->FspGridMinPositionWs = FVector3f(FspGridMinPositionWs);
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetClipToWorld());
        Parameters->SceneDepthTexture = SceneDepthTexture;
        Parameters->RWFspCellData = GraphBuilder.CreateUAV(SystemState.fsp.FspCellDataBuffer.Handle);
        Parameters->RWFspVisibleSurfaceList = GraphBuilder.CreateUAV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
        TShaderMapRef<FInstantRdvFspScreenSpaceCollectCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(Parameters->ViewRectSizeX, 8u);
        const uint32 GroupY = FMath::DivideAndRoundUp(Parameters->ViewRectSizeY, 8u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspScreenSpaceCollect"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, GroupY, 1));
    }

    {
        FInstantRdvFspRadianceUpdateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FInstantRdvFspRadianceUpdateCS::FParameters>();
        Parameters->FspGridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
        Parameters->FspGridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
        Parameters->FspGridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
        Parameters->FspCellDataU32Count = kFspProbeDataU32Count;
        Parameters->FspVisibleSurfaceListCapacity = SystemState.fsp.TrGrid.GetCellCount();
        Parameters->BbvGridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
        Parameters->BbvGridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
        Parameters->BbvGridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
        Parameters->BbvOptionalDataU32Count = Config.bbv.OptionalDataU32Count;
        Parameters->FspGridMinPositionWs = FVector3f(FspGridMinPositionWs);
        Parameters->FspCellSizeCm = Config.fsp.ProbeCellSizeCm;
        Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
        Parameters->BbvCellSizeCm = Config.bbv.BbvBrickSizeCm;
        Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
        Parameters->BbvOptionalData = GraphBuilder.CreateSRV(OptionalDataBuffer);
        Parameters->RWFspCellData = GraphBuilder.CreateUAV(SystemState.fsp.FspCellDataBuffer.Handle);
        Parameters->FspVisibleSurfaceList = GraphBuilder.CreateSRV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
        TShaderMapRef<FInstantRdvFspRadianceUpdateCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 GroupX = FMath::DivideAndRoundUp(SystemState.fsp.TrGrid.GetCellCount(), 64);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspRadianceUpdate"), ERDGPassFlags::Compute, ComputeShader, Parameters, FIntVector(GroupX, 1, 1));
    }


    // Ray capture (簡易版): 可視 surface list に基づき各セルの ProbeRadiance を書き込む
    {
        // Dispatch ray-capture compute
        FInstantRdvFspRayCaptureCS::FParameters* RayParams = GraphBuilder.AllocParameters<FInstantRdvFspRayCaptureCS::FParameters>();
        RayParams->FspVisibleSurfaceList = GraphBuilder.CreateSRV(SystemState.fsp.FspVisibleSurfaceListBuffer.Handle);
        RayParams->RWFspProbeRadiance = GraphBuilder.CreateUAV(SystemState.fsp.FspProbeRadianceBuffer.Handle);
        TShaderMapRef<FInstantRdvFspRayCaptureCS> RayShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        const uint32 VisibleCapacity = SystemState.fsp.TrGrid.GetCellCount();
        const uint32 GroupX = FMath::DivideAndRoundUp(VisibleCapacity, 64u);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspRayCapture"), ERDGPassFlags::Compute, RayShader, RayParams, FIntVector(GroupX, 1, 1));

        // Packed SH update (簡易): ProbeRadiance -> PackedSH (4 float4 per probe)
        {
            FInstantRdvFspShUpdateCS::FParameters* ShParams = GraphBuilder.AllocParameters<FInstantRdvFspShUpdateCS::FParameters>();
            ShParams->FspProbeRadiance = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeRadianceBuffer.Handle);
            ShParams->RWFspPackedSH = GraphBuilder.CreateUAV(SystemState.fsp.FspPackedSHBuffer.Handle);
            TShaderMapRef<FInstantRdvFspShUpdateCS> ShShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            const uint32 ShGroupX = FMath::DivideAndRoundUp(SystemState.fsp.TrGrid.GetCellCount(), 64);
            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstantRdv.FspShUpdate"), ERDGPassFlags::Compute, ShShader, ShParams, FIntVector(ShGroupX, 1, 1));
        }
    }
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

    FRDGBufferRef BitmaskBuffer = SystemState.bbv.BitmaskBuffer.Handle;
    FRDGBufferRef BrickDataBuffer = SystemState.bbv.BrickDataBuffer.Handle;
    FRDGBufferRef OptionalDataBuffer = SystemState.bbv.OptionalDataBuffer.Handle;

    const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);

    // 全画面描画系デバッグ表示.
    if (
        DebugMode == 0 ||
        DebugMode == 1 ||
        DebugMode == 2 ||
        DebugMode == 3 ||
        DebugMode == 4 ||

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
            Parameters->DepthSizeX = static_cast<uint32>(SceneDepthTexture->Desc.Extent.X);
            Parameters->DepthSizeY = static_cast<uint32>(SceneDepthTexture->Desc.Extent.Y);
            Parameters->ViewRectMinX = static_cast<uint32>(FMath::Max(ViewRect.Min.X, 0));
            Parameters->ViewRectMinY = static_cast<uint32>(FMath::Max(ViewRect.Min.Y, 0));
            Parameters->ViewRectSizeX = static_cast<uint32>(FMath::Max(ViewRect.Width(), 1));
            Parameters->ViewRectSizeY = static_cast<uint32>(FMath::Max(ViewRect.Height(), 1));
            Parameters->GridResolutionX = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.X);
            Parameters->GridResolutionY = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Y);
            Parameters->GridResolutionZ = static_cast<uint32>(SystemState.bbv.TrGrid.GridReso.Z);
            Parameters->BitmaskWordsPerBrick = Config.bbv.GetBitmaskU32CountPerBrick();
            Parameters->BbvPerBrickResolution = Config.bbv.BbvPerBrickResolution;
            Parameters->BbvToroidalOffsetCells = FVector3f(SystemState.bbv.TrGrid.ToroidalOffsetCells);
            Parameters->CellSizeCm = Config.bbv.BbvBrickSizeCm;
            Parameters->BbvGridMinPositionWs = FVector3f(SystemState.bbv.TrGrid.MinPositionWs);
            Parameters->MaxTraceDistanceCm = Config.bbv.BbvBrickSizeCm * static_cast<float>(SystemState.bbv.TrGrid.GridReso.GetMax());
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
    
    // スプライト描画系デバッグ表示.
    if (DebugMode == 5)
    {
        const uint32 ProbeCount = SystemState.fsp.TrGrid.GridReso.X * SystemState.fsp.TrGrid.GridReso.Y * SystemState.fsp.TrGrid.GridReso.Z;
        {
            FInstantRdvFspProbeBillboard_Parameters* Params = GraphBuilder.AllocParameters<FInstantRdvFspProbeBillboard_Parameters>();

            FInstantRdvFspProbeBillboardVS::FParameters* VSParams = &Params->VS;
            {
                VSParams->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetWorldToClip());
                VSParams->ViewAcpectRatio = View.ViewMatrices.GetProjectionMatrix().GetColumn(1)[1] / View.ViewMatrices.GetProjectionMatrix().GetColumn(0)[0];
                VSParams->FspGridMinPositionWs = FVector3f(SystemState.fsp.TrGrid.MinPositionWs);
                VSParams->CellSizeCm = Config.fsp.ProbeCellSizeCm;
                VSParams->GridResolutionX = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.X);
                VSParams->GridResolutionY = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Y);
                VSParams->GridResolutionZ = static_cast<uint32>(SystemState.fsp.TrGrid.GridReso.Z);
            }

            FInstantRdvFspProbeBillboardPS::FParameters* PSParams = &Params->PS;
            {
                PSParams->FspProbeRadiance = GraphBuilder.CreateSRV(SystemState.fsp.FspProbeRadianceBuffer.Handle);

                // GlobalShaderのGraphicsPipelineでRasterをする場合はShaderParameterにRENDER_TARGET_BINDING_SLOTSでRenderTargetSlotを定義しておいてターゲットを設定し, GraphBuilder.AddPass() のShaderParameter引数有バージョンに引き渡すことでRenderTarget設定される.
                {
                    PSParams->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
                    PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilNop);
                }
            }

            TShaderMapRef<FInstantRdvFspProbeBillboardVS> ProbeVS(GetGlobalShaderMap(View.GetFeatureLevel()));
            TShaderMapRef<FInstantRdvFspProbeBillboardPS> ProbePS(GetGlobalShaderMap(View.GetFeatureLevel()));

            GraphBuilder.AddPass(RDG_EVENT_NAME("InstantRdv.FspProbeBillboard"), Params, ERDGPassFlags::Raster, [ViewRect, ProbeVS, ProbePS, VSParams, PSParams, ProbeCount](FRHICommandListImmediate& RHICmdList)
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
                GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();

                SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                
                // Set shader parameters. -> AddPassのShaderParameter指定版を使う場合, RenderTargetの設定などはなされるが, シェーダステージのUniformパラメータは自動セットされない模様.
                SetShaderParameters(RHICmdList, ProbeVS, ProbeVS.GetVertexShader(), *VSParams);
                SetShaderParameters(RHICmdList, ProbePS, ProbePS.GetPixelShader(), *PSParams);

                const int k_per_instance_prim_count = 2;
                const int instance_count = ProbeCount;
                RHICmdList.DrawPrimitive(0, instance_count * k_per_instance_prim_count, 1);
            });
        }
    }
    
}
