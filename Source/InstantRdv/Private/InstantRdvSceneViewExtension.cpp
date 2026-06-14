#include "InstantRdvSceneViewExtension.h"

#include "HAL/IConsoleManager.h"
#include "InstantRdvBbv.h"
#include "PostProcess/PostProcessInputs.h"
#include "SceneRendering.h"

namespace
{
static TAutoConsoleVariable<int32> CVarInstantRdvEnable(
    TEXT("r.InstantRdv.GI"),
    1,
    TEXT("Enable Instant-RDV runtime hooks.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvEnable(
    TEXT("r.InstantRdv.Bbv.Enable"),
    1,
    TEXT("Enable Instant-RDV BBV update passes.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvDebugMode(
    TEXT("r.InstantRdv.Bbv.DebugMode"),
    0,
    TEXT("Instant-RDV BBV debug mode selector.\n")
    TEXT("0: Off\n")
    TEXT("1: XY fine-voxel Z-count map (no raytrace)\n")
    TEXT("2: Brick raytrace debug\n")
    TEXT("3: Voxel raytrace debug"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvMainViewInjection(
    TEXT("r.InstantRdv.Bbv.MainViewInjection"),
    1,
    TEXT("Enable BBV main view injection pass.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvMainViewRemoval(
    TEXT("r.InstantRdv.Bbv.MainViewRemoval"),
    1,
    TEXT("Enable BBV main view removal pass.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvMainViewUpdate(
    TEXT("r.InstantRdv.Bbv.MainViewUpdate"),
    1,
    TEXT("Master toggle for BBV main view update passes (Injection + Removal).\n")
    TEXT("0: Disable both Injection and Removal\n")
    TEXT("1: Enable by individual pass toggles"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvGeometryUpdateTiming(
    TEXT("r.InstantRdv.Bbv.GeometryUpdateTiming"),
    0,
    TEXT("BBV Geometry更新（Injection/Removal）の実行タイミング。\n")
    TEXT("0: PreRenderBasePass_RenderThread（既定）\n")
    TEXT("1: PrePostProcessPass_RenderThread（従来互換）"),
    ECVF_RenderThreadSafe);

static FRDGTextureRef GetViewSceneDepthTexture_RenderThread(const FSceneView& View)
{
    // SceneViewExtension のコールバック実体は FViewInfo なので、Renderer内部情報から Depth RDG を取得する。
    const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
    return ViewInfo.GetSceneTextures().Depth.Target;
}
}

FInstantRdvSceneViewExtension::FInstantRdvSceneViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
    , BbvSystem(MakeUnique<FInstantRdvBbv>())
{
}

FInstantRdvSceneViewExtension::~FInstantRdvSceneViewExtension() = default;

bool FInstantRdvSceneViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    return CVarInstantRdvEnable.GetValueOnAnyThread() != 0;
}

void FInstantRdvSceneViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FInstantRdvSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
}

void FInstantRdvSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FInstantRdvSceneViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
    (void)GraphBuilder;
    (void)InViewFamily;
    FrameViews_RenderThread.Reset();
    if (BbvSystem.IsValid())
    {
        BbvSystem->BeginFrame_RenderThread();
    }
}

void FInstantRdvSceneViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
    (void)GraphBuilder;
    FrameViews_RenderThread.Add(&InView);
}

void FInstantRdvSceneViewExtension::ExecuteBbvGeometryUpdate_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTexture* SceneDepthTexture)
{
    if (CVarInstantRdvBbvEnable.GetValueOnRenderThread() == 0 || BbvSystem.IsValid() == false)
    {
        return;
    }

    if (SceneDepthTexture == nullptr)
    {
        return;
    }

    // MainViewUpdate は Geometry 更新（Injection/Removal）のみを止める。
    // Grid/Toroidal の更新は BBV内部で常時維持する。
    const bool bEnableMainViewUpdate = (CVarInstantRdvBbvMainViewUpdate.GetValueOnRenderThread() != 0);
    const bool bEnableMainViewGeometryInjection = bEnableMainViewUpdate && (CVarInstantRdvBbvMainViewInjection.GetValueOnRenderThread() != 0);
    const bool bEnableMainViewGeometryRemoval = bEnableMainViewUpdate && (CVarInstantRdvBbvMainViewRemoval.GetValueOnRenderThread() != 0);
    BbvSystem->ExecuteGeometryUpdate(
        GraphBuilder,
        View,
        SceneDepthTexture,
        bEnableMainViewGeometryInjection,
        bEnableMainViewGeometryRemoval);
}

void FInstantRdvSceneViewExtension::PreRenderBasePass_RenderThread(FRDGBuilder& GraphBuilder, bool bDepthBufferIsPopulated)
{
    if (CVarInstantRdvBbvGeometryUpdateTiming.GetValueOnRenderThread() != 0)
    {
        return;
    }
    if (!bDepthBufferIsPopulated)
    {
        return;
    }

    for (const FSceneView* View : FrameViews_RenderThread)
    {
        if (View == nullptr)
        {
            continue;
        }

        FRDGTextureRef SceneDepthTexture = GetViewSceneDepthTexture_RenderThread(*View);
        ExecuteBbvGeometryUpdate_RenderThread(GraphBuilder, *View, SceneDepthTexture);
    }
}

void FInstantRdvSceneViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
    if (CVarInstantRdvBbvEnable.GetValueOnRenderThread() == 0 || BbvSystem.IsValid() == false)
    {
        return;
    }

    Inputs.Validate();
    if (!Inputs.SceneTextures)
    {
        return;
    }

    const auto& SceneTextureParameters = Inputs.SceneTextures->GetParameters();
    FRDGTextureRef SceneDepthTexture = SceneTextureParameters->SceneDepthTexture;
    if (SceneDepthTexture == nullptr)
    {
        return;
    }

    if (CVarInstantRdvBbvGeometryUpdateTiming.GetValueOnRenderThread() == 1)
    {
        ExecuteBbvGeometryUpdate_RenderThread(GraphBuilder, View, SceneDepthTexture);
    }

    const int32 DebugMode = CVarInstantRdvBbvDebugMode.GetValueOnRenderThread();
    BbvSystem->ExecuteDebugVisualize(
        GraphBuilder,
        View,
        SceneDepthTexture,
        SceneTextureParameters->SceneColorTexture,
        DebugMode);
}

int32 FInstantRdvSceneViewExtension::GetPriority() const
{
    return -10;
}
