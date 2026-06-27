#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "InstantRdvFsp.h"

class FRDGBuilder;
class FRDGTexture;
class FSceneView;
class FInstantRdvFsp;

struct FInstantRdvBbvConfig
{
    FIntVector BbvGridResolution = FIntVector(64, 64, 64);
    float BbvBrickSizeCm = 300.0f;// NxNxNのVoxelクラスタをBrickと称し, そのサイズを指定するパラメータ.

    uint32 BbvPerBrickResolution = 8;
    // BrickData は 1 Brick あたり 4 uint を前提に各シェーダがアクセスする。
    // ここを 1 などに変更すると、occupied count や各種属性の参照先が壊れる。
    uint32 BrickDataU32Count = 4;
    uint32 HiBrickDataU32Count = 1;
    uint32 OptionalDataU32Count = 4;


    uint32 GetBbvBrickCount() const;
    uint32 GetBitmaskU32CountPerBrick() const;
    uint32 GetBitmaskElementCount() const;
    uint32 GetBrickDataElementCount() const;
    uint32 GetHiBrickBrickCount() const;
    uint32 GetHiBrickDataElementCount() const;
    uint32 GetOptionalDataElementCount() const;
    uint32 GetRadianceAccumDataElementCount() const;
};
struct FInstantRdvFspConfig
{
    FIntVector ProbeGridResolution = FIntVector(32, 32, 32);
    float ProbeCellSizeCm = 200.0f;
    uint32 ProbeCascadeCount = 5;

    uint32 GetFspCellCount() const;
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

    // 初期化/解放 (Editor/Device 初期化タイミングで呼ぶ)
    void Initialize();

    // フレーム内Renderの最初の更新. SceneViewExtensionのPreRenderView_RenderThread等で呼ぶことを想定.
    void BeginFrame_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InViewFamily);

    // BBV Geometry 更新（Injection / Removal）本体。
    void ExecuteGeometryUpdate(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        bool bEnableMainViewGeometryInjection,
        bool bEnableMainViewGeometryRemoval);

    // BBV Debug 可視化。Geometry 更新とは切り離して呼べるようにする。
    void ExecuteDebugVisualize(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        FRDGTexture* SceneColorTexture,
        float SceneColorPreExposure,
        int32 DebugMode);

    // BBV Radiance 更新。BeforeDOF の SceneColor は PreExposure 済みなので、
    // シェーダ内で PreExposure を打ち消して絶対輝度として蓄積する。
    void ExecuteRadianceUpdate(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        FRDGTexture* SceneColorTexture,
        float SceneColorPreExposure,
        bool bEnableRadianceInjection,
        bool bEnableRadianceResolve);

    // Frustum Space Probe(FSP) 初期更新。
    // 参照実装のScreenSpacePass相当で可視Surface Cellを収集し、BBV resolved radianceをFSP cellへ写す。
    void ExecuteFspUpdate(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        bool bEnableFspUpdate);

private:

    struct FPersistentRdgPooledBufferSet
    {
        TRefCountPtr<class FRDGPooledBuffer>    PooledBuffer{};// プールに確保したBuffer本体. 最初にExtraction指定して永続化する.

        FRDGBufferRef                           Handle{};// フレーム先頭で上記BufferをRDGにRegisterした際のハンドルを保持.
    };

    struct FSystemState
    {

        // Inner.
        struct FBbvState
        {
            FToroidalGrid TrGrid{};

            // システムがフレームをまたいで管理するリソース群.
            FPersistentRdgPooledBufferSet BitmaskBuffer;
            FPersistentRdgPooledBufferSet BrickDataBuffer;
            FPersistentRdgPooledBufferSet HiBrickDataBuffer;
            FPersistentRdgPooledBufferSet OptionalDataBuffer;
            FPersistentRdgPooledBufferSet RadianceAccumBuffer;
        };
        // Inner.
        struct FFspState
        {
            FToroidalGrid TrGrid{};

            FPersistentRdgPooledBufferSet FspCellDataBuffer;
            FPersistentRdgPooledBufferSet FspVisibleSurfaceListBuffer;
            FPersistentRdgPooledBufferSet FspProbePoolBuffer;
            FPersistentRdgPooledBufferSet FspProbeFreeStackBuffer;
            FPersistentRdgPooledBufferSet FspActiveProbeListPrevBuffer;
            FPersistentRdgPooledBufferSet FspActiveProbeListCurrBuffer;
            FPersistentRdgPooledBufferSet FspProbeRadianceBuffer;
            FPersistentRdgPooledBufferSet FspPackedSHBuffer;
        };


        bool    bRenderInitialized = false;
        uint32  FrameCount = 0;

        FBbvState bbv{};
        FFspState fsp{};
    };

    struct FConfig
    {
        FInstantRdvBbvConfig    bbv{};
        FInstantRdvFspConfig    fsp{};
    };
    FConfig                 Config;
    FSystemState            SystemState;
};
