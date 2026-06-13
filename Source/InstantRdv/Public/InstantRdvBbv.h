#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FRDGBuilder;
class FRDGTexture;
class FSceneView;

struct FInstantRdvBbvConfig
{
    FIntVector BbvGridResolution = FIntVector(64, 64, 64);
    float BbvVoxelSizeCm = 300.0f;
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
};

class FInstantRdvBbv final
{
public:
    void Execute(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        FRDGTexture* SceneDepthTexture,
        FRDGTexture* SceneColorTexture,
        int32 DebugMode,
        bool bEnableMainViewInjection,
        bool bEnableMainViewRemoval);

private:
    struct FResourceCache
    {
        TRefCountPtr<class FRDGPooledBuffer> BitmaskBuffer;
        TRefCountPtr<class FRDGPooledBuffer> BrickDataBuffer;
        TRefCountPtr<class FRDGPooledBuffer> HiBrickDataBuffer;
        TRefCountPtr<class FRDGPooledBuffer> OptionalDataBuffer;
        uint32 CachedBitmaskElements = 0;
        uint32 CachedBrickDataElements = 0;
        uint32 CachedHiBrickDataElements = 0;
        uint32 CachedOptionalDataElements = 0;
        bool bInitialized = false;
        bool bGridOriginInitialized = false;
        uint32 FrameCount = 0;
        FVector GridMinPositionWs = FVector::ZeroVector;
        FIntVector ToroidalOffsetCells = FIntVector::ZeroValue;
    };

    FInstantRdvBbvConfig Config;
    FResourceCache ResourceCache;
};
