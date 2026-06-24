#include "InstantRdvFsp.h"
#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"

FInstantRdvFsp::FInstantRdvFsp()
{
}

FInstantRdvFsp::~FInstantRdvFsp()
{
    Release();
}

void FInstantRdvFsp::Initialize()
{
    // 将来的な ProbePool / FreeStack 等の準備をここで行う。
}

void FInstantRdvFsp::Release()
{
    // RDG外の永続リソースがあればここで解放する。
}

void FInstantRdvFsp::Tick(FRDGBuilder& GraphBuilder)
{
    // 現状は移行中のため、InstantRdvBbv 側で実装済みのパスを利用している。
    // 将来的にはここに SurfaceCollect -> ProbeAssign -> RadianceUpdate を統合する予定。
    (void)GraphBuilder;
}
