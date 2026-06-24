#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "InstantRdvFsp.h"
#include "InstantRdvBbvFrameResources.h"

class FRDGBuilder;
class FRDGTexture;
class FSceneView;
class FInstantRdvFsp;

struct FInstantRdvBbvConfig
{
    FIntVector BbvGridResolution = FIntVector(64, 64, 64);
    float BbvBrickSizeCm = 300.0f;// NxNxNのVoxelクラスタをBrickと称し, そのサイズを指定するパラメータ.
    FIntVector ProbeGridResolution = FIntVector(32, 32, 32);
    float ProbeCellSizeCm = 200.0f;
    uint32 ProbeCascadeCount = 5;

    uint32 BbvPerVoxelResolution = 8;
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

class FInstantRdvBbv final
{
public:

    // 初期化/解放 (Editor/Device 初期化タイミングで呼ぶ)
    void Initialize();

    // フレーム内Renderの最初の更新. SceneViewExtensionのPreRenderViewFamily_RenderThread等で呼ぶことを想定.
    void BeginFrame_RenderThread(FRDGBuilder& GraphBuilder, const FSceneViewFamily& InViewFamily);

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

    struct PersistentRdgPooledBufferSet
    {
        TRefCountPtr<class FRDGPooledBuffer>    PooledBuffer{};// プールに確保したBuffer本体. 最初にExtraction指定して永続化する.

        FRDGBufferRef                           Handle{};// フレーム先頭で上記BufferをRDGにRegisterした際のハンドルを保持.
    };

    struct FSystemState
    {
        bool    bRenderInitialized = false;
        uint32  FrameCount = 0;

        FIntVector  BbvGridMinCell = FIntVector::ZeroValue;// Bbvのセル絶対座標におけるグリッドMinセル座標
        FVector     BbvGridMinPositionWs = FVector::ZeroVector;// Bbvのグリッド範囲のMin座標
        FIntVector  BbvToroidalOffsetCells = FIntVector::ZeroValue;


        // システムがフレームをまたいで管理するリソース群.
        PersistentRdgPooledBufferSet BitmaskBuffer;
        PersistentRdgPooledBufferSet BrickDataBuffer;
        PersistentRdgPooledBufferSet HiBrickDataBuffer;
        PersistentRdgPooledBufferSet OptionalDataBuffer;
        PersistentRdgPooledBufferSet RadianceAccumBuffer;

        PersistentRdgPooledBufferSet FspCellDataBuffer;
        PersistentRdgPooledBufferSet FspVisibleSurfaceListBuffer;
        PersistentRdgPooledBufferSet FspProbePoolBuffer;
        PersistentRdgPooledBufferSet FspProbeFreeStackBuffer;
        PersistentRdgPooledBufferSet FspActiveProbeListPrevBuffer;
        PersistentRdgPooledBufferSet FspActiveProbeListCurrBuffer;
        PersistentRdgPooledBufferSet FspProbeRadianceBuffer;
        PersistentRdgPooledBufferSet FspPackedSHBuffer;
    };

    FInstantRdvBbvConfig    Config;
    FSystemState          SystemState;

    // FSP (Frustum Space Probe) 管理オブジェクト。実装詳細は InstantRdvFsp.* に分離。
    TUniquePtr<class FInstantRdvFsp> Fsp;
};
