#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

// フレーム単位でBBVとFSP間で受け渡すリソース参照。
// RDGスコープ内で有効な FRDGBufferRef / FRDGTextureRef を保持する。
// NOTE: Public ヘッダなので軽量な前方参照 (RenderGraphFwd) のみを include している。

struct FInstantRdvBbvFrameResources
{
    // ラジアンス蓄積バッファの要素数 (暫定)
    uint32 RadianceAccumBufferSize = 0;

    // FSP用セルデータバッファの要素数 (暫定)
    uint32 FspCellDataBufferSize = 0;

    // フレーム内の RDG 参照 (BeginFrame でクリア、各 Execute で設定される)
    FRDGBufferRef FrameBitmaskBuffer = nullptr;
    FRDGBufferRef FrameBrickDataBuffer = nullptr;
    FRDGBufferRef FrameHiBrickDataBuffer = nullptr;
    FRDGBufferRef FrameOptionalDataBuffer = nullptr;
    FRDGBufferRef FrameRadianceAccumBuffer = nullptr;
    FRDGBufferRef FrameFspCellDataBuffer = nullptr;
    FRDGBufferRef FrameFspVisibleSurfaceListBuffer = nullptr;

    // 将来的に FRDGTextureRef 等も追加される可能性がある。
};
