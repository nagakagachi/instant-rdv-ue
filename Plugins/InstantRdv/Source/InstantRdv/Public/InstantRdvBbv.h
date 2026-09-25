/*
    InstantRdvBbv.h
    RDV: Raster Derived Voxel.
    BBV: Bitmask Brick Voxel.
    VSP: Visible Surface Probe.
*/

#pragma once

#include "CoreMinimal.h"
#include "InstantRdvDefaultSettings.h"
#include "RenderGraphFwd.h"

#include "../../../Shaders/Private/instant_rdv_common.ush"


class FRDGBuilder;
class FRDGTexture;
class FSceneView;
class FSceneUniformBuffer;
class FRDGPooledTexture;
class FInstantRdvVsp;
class FInstantRdvSceneUniformBufferParams;

struct FInstantRdvBbvConfig
{
    FIntVector BbvGridResolution = InstantRdvDefaults::BbvGridResolution;
    float BbvBrickSizeCm = InstantRdvDefaults::BbvBrickSizeCm;// NxNxNのVoxelクラスタをBrickと称し, そのサイズを指定するパラメータ.

    uint32 GetBbvBrickCount() const;
    uint32 GetBitmaskElementCount() const;
    uint32 GetBrickDataElementCount() const;
    uint32 GetBbvBufferElementCount() const;
    uint32 GetOptionalDataElementCount() const;
    uint32 GetRadianceAccumDataElementCount() const;
};
struct FInstantRdvVspConfig
{
    FIntVector ProbeGridResolution = InstantRdvDefaults::ProbeGridResolution;
    float ProbeCellSizeCm = InstantRdvDefaults::ProbeCellSizeCm;
    uint32 ProbeCascadeCount = InstantRdvDefaults::ProbeCascadeCount;
    // Denseセル数とは独立した全カスケード共通のSparse容量。
    uint32 ProbeCapacity = InstantRdvDefaults::ProbePoolCapacity;
    uint32 VisibleSurfaceCapacity = InstantRdvDefaults::VisibleSurfaceCapacity;
    uint32 GetRayCapacity() const { return ProbeCapacity * k_irdv_vsp_probe_octmap_width * k_irdv_vsp_probe_octmap_width; }
    uint32 GetAtlasTileWidth() const { return FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::CeilToInt(FMath::Sqrt(static_cast<float>(ProbeCapacity))))); }

    uint32 GetVspCellCount() const;
    uint32 GetVspTotalCellCount() const;
};


struct FToroidalGrid
{
    FIntVector  GridReso = FIntVector::ZeroValue;// 解像度
    FIntVector  GridMinCell = FIntVector::ZeroValue;// セル絶対座標におけるグリッドMinセル座標
    FVector     MinPositionWs = FVector::ZeroVector;// Bbvのグリッド範囲のMin座標
    FIntVector  FrameCellDelta = FIntVector::ZeroValue;// フレームでの移動Cell量.

    FIntVector  ToroidalOffsetCells = FIntVector::ZeroValue;// Toroidal Mapping Offset


    static FIntVector CalcToroidalOffsetCells(const FIntVector& current_offset, const FIntVector& delta, const FIntVector& resolution)
    {
        auto ToroidalMappingWrap1D = [](int Base, int Delta, int Mod)
            {
                const int Raw = (Base + Delta) % Mod;
                return (Raw < 0) ? (Raw + Mod) : Raw;// 負の場合は反対側へWrap.
            };

        return FIntVector(
            ToroidalMappingWrap1D(current_offset.X, delta.X, resolution.X),
            ToroidalMappingWrap1D(current_offset.Y, delta.Y, resolution.Y),
            ToroidalMappingWrap1D(current_offset.Z, delta.Z, resolution.Z));
    }

    void UpdateDelta(const FVector& new_important_position, float cell_size)
    {
        const FVector GridHalfExtent = FVector(GridReso) * cell_size * 0.5f;
        const FVector DesiredGridMin = new_important_position - GridHalfExtent;
        const FIntVector DesiredMinCell(floorf(DesiredGridMin.X / cell_size), floorf(DesiredGridMin.Y / cell_size), floorf(DesiredGridMin.Z / cell_size) );
        // 移動セル量.
        FrameCellDelta = DesiredMinCell - GridMinCell;
        GridMinCell = DesiredMinCell;
        if (!FrameCellDelta.IsZero())
        {
            MinPositionWs = FVector(GridMinCell) * cell_size;
            ToroidalOffsetCells = CalcToroidalOffsetCells(ToroidalOffsetCells, FrameCellDelta, GridReso);
        }
    }

    int GetCellCount() const
    {
        return GridReso.X * GridReso.Y * GridReso.Z;
    }
};

class FInstantRdvBbv final
{
public:

    FInstantRdvBbv(const FInstantRdvBbvConfig& InBbvConfig, const FInstantRdvVspConfig& InVspConfig);

    // 初期化/解放 (Editor/Device 初期化タイミングで呼ぶ)
    void Initialize();

    // RDV永続リソースをそのフレームのRDGへ接続し、BBVの全体フレーム状態を1つ進める。
    // VSPのActiveProbe世代は実際にVSP更新を実行したときだけ進むため、SceneViewExtension側で選んだ
    // 代表ViewFamily/Viewにつき1回だけ呼ぶこと。View単位callbackから複数回呼ぶとライフサイクルが破綻する。
    void BeginFrame_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView);

    // Material CustomNodeから利用するSceneUniformBuffer拡張値へ、当フレームのRDGリソースを設定する。
    void FillSceneUniformBufferParams_RenderThread(
        FRDGBuilder& GraphBuilder,
        FInstantRdvSceneUniformBufferParams& OutParams,
        bool bUseLiveResources, bool bUseVspResources);

    // BBV Geometry 更新（Injection / Removal）本体。
    FRDGTexture* ExecuteGeometryUpdate(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        bool bEnableMainViewGeometryInjection,
        bool bEnableMainViewGeometryRemoval,
        bool bUseReducedSurfaceBuffer);

    // BBV Debug 可視化。Geometry 更新とは切り離して呼べるようにする。
    void ExecuteDebugVisualize(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        FRDGTexture* SceneColorTexture,
        float SceneColorPreExposure,
        int32 BbvDebugMode,
        int32 VspProbeDebugMode,
        int32 VspIvProbeDebugMode,
        bool bProbeDepthTest,
        float BbvDebugSceneColorBlend,
        bool bBbvDebugDepthTest,
        bool bUseProbeVisualizationOffset,
        bool bUseProbeTraceOffset);

    // BBV Radiance 更新。BeforeDOF の SceneColor は PreExposure 済みなので、
    // シェーダ内で PreExposure を打ち消して絶対輝度として蓄積する。
    void ExecuteRadianceUpdate(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        FRDGTexture* SceneColorTexture,
        float SceneColorPreExposure,
        bool bEnableRadianceInjection,
        bool bEnableRadianceResolve,
        bool bUseReducedSurfaceBuffer);

    // Visible Surface Probe(VSP) 初期更新。
    // 参照実装のScreenSpacePass相当で可視Surface Cellを収集し、BBV resolved radianceをVSP cellへ写す。
    void ExecuteVspUpdate(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        bool bEnableVspUpdate,
        bool bUseProbeTraceOffset,
        bool bUseReducedSurfaceBuffer,
        FRDGTexture* GeometryReducedSurfaceTexture);

private:

    struct FPersistentRdgPooledBufferSet
    {
        TRefCountPtr<class FRDGPooledBuffer>    PooledBuffer{};// プールに確保したBuffer本体. 最初にExtraction指定して永続化する.

        FRDGBufferRef                           Handle{};// フレーム先頭で上記BufferをRDGにRegisterした際のハンドルを保持.
    };

    struct FPersistentRdgPooledTextureSet
    {
        // フレームをまたいで保持できるのはpooled texture本体だけ。
        // Handleは現在のFRDGBuilder内だけで使用し、BeginFrameごとに生成または再登録する。
        TRefCountPtr<IPooledRenderTarget> PooledTexture{};
        FRDGTextureRef Handle{};
        FIntPoint Extent = FIntPoint::ZeroValue;
    };

    struct FSystemState
    {

        // Inner.
        struct FBbvState
        {
            FToroidalGrid TrGrid{};

            // システムがフレームをまたいで管理するリソース群.
            FPersistentRdgPooledBufferSet BbvBuffer;
            FPersistentRdgPooledBufferSet OptionalDataBuffer;
            FPersistentRdgPooledBufferSet RadianceAccumBuffer;
        };
        // Inner.
        struct FVspState
        {
            FToroidalGrid TrGrid{};
            // VSPは参照InstantRDVと同じくtoroidal grid移動量で古いphysical slotを無効化する。
            // 前フレームのgrid centerを保持し、BeginUpdate shaderへ渡してslotの押し出し判定に使う。
            FVector CurrentGridCenterPositionWs = FVector::ZeroVector;
            FVector PreviousGridCenterPositionWs = FVector::ZeroVector;

            // VspCellProbeIndexBuffer:
            //   global cell index -> owning active probe index。未所有はinvalid probe index。
            // VspVisibleSurfaceListBuffer:
            //   [0]=count, [1..]=depth surfaceから選ばれたowner cell index。
            FPersistentRdgPooledBufferSet VspCellProbeIndexBuffer;
            FPersistentRdgPooledBufferSet VspVisibleSurfaceListBuffer;
            FPersistentRdgPooledBufferSet VspVisibleSurfaceSourceTexelListBuffer;
            // ProbePool/FreeStack/ActiveListは参照VSPのActiveProbe lifecycleをGPU上で回すための永続buffer。
            // ActiveProbeList固有のレイアウト。各物理Bufferのword 0/1を世代交代counter、
            // word 2以降をProbe index listとして使用する。他のcounter bufferはword 0のみを
            // counterとして使用するため、ActiveProbeListを単一counter前提で扱わないこと。
            // これによりGPUフレーム重複時もreset中のcounterを別世代のappendが上書きしない。
            FPersistentRdgPooledBufferSet VspProbePoolBuffer;
            FPersistentRdgPooledBufferSet VspProbeFreeStackBuffer;
            FPersistentRdgPooledBufferSet VspActiveProbeListBuffers[2];
            // ProbeAtlasはActiveProbeごとの6x6 OctMap、IrradianceVolumeは全CascadeをZ方向へ連結した3D Texture。
            FPersistentRdgPooledTextureSet VspProbeAtlas;
            FPersistentRdgPooledBufferSet VspProbeRayRequestBuffer;
            FPersistentRdgPooledBufferSet VspProbeRayResultBuffer;
            FPersistentRdgPooledTextureSet VspIrradianceVolumeSHTexture;
            FPersistentRdgPooledTextureSet ReducedSurfaceBuffer;
            uint32 VspUpdateFrameCount = 0;
        };


        bool    bRenderInitialized = false;
        uint32  FrameCount = 0;

        FBbvState bbv{};
        FVspState vsp{};
    };

    struct FConfig
    {
        FInstantRdvBbvConfig    bbv{};
        FInstantRdvVspConfig    vsp{};
    };
    FConfig                 Config;
    FSystemState            SystemState;
};
