#include "InstantRdvSceneViewExtension.h"

#include "HAL/IConsoleManager.h"
#include "InstantRdvBbv.h"
#include "PostProcess/PostProcessMaterialInputs.h"
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
    TEXT("3: Voxel raytrace debug\n")
    TEXT("4: Voxel radiance debug\n")
    TEXT("5-14: FSP probe debug modes 0-9\n")
    TEXT("       FSP 5: active/liveness, 9-10: active OctMap, 11-14: SH/IrradianceVolume\n")
    TEXT("       FSP modes also draw counters at top-left: cyan=visible cells, green=active probes, yellow=ray requests, orange=ray results, purple=free probes"),
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

static TAutoConsoleVariable<int32> CVarInstantRdvBbvRadianceUpdate(
    TEXT("r.InstantRdv.Bbv.RadianceUpdate"),
    1,
    TEXT("BBV Radiance更新（Injection + Resolve）の有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled at SubscribeToPostProcessingPass BeforeDOF"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvRadianceInjection(
    TEXT("r.InstantRdv.Bbv.RadianceInjection"),
    1,
    TEXT("BBV Radiance Injection passの有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvBbvRadianceResolve(
    TEXT("r.InstantRdv.Bbv.RadianceResolve"),
    1,
    TEXT("BBV Radiance Resolve passの有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstantRdvFspUpdate(
    TEXT("r.InstantRdv.Fsp.Update"),
    1,
    TEXT("Frustum Space Probe(FSP) lifecycle / ray trace / SH update の有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled after BBV Radiance Resolve"),
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
{
    BbvSystem = MakeUnique<FInstantRdvBbv>();
    {
        BbvSystem->Initialize();
    }

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

    /*
    if (BbvSystem.IsValid())
    {
        BbvSystem->BeginFrame_RenderThread(GraphBuilder, InViewFamily);
    }
    */
}

void FInstantRdvSceneViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
    (void)GraphBuilder;
    FrameViews_RenderThread.Add(&InView);

    if (BbvSystem.IsValid())
    {
        BbvSystem->BeginFrame_RenderThread(GraphBuilder, InView);
    }
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
// PostProcess先頭.
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

    // 現状は特になし.
}

// PostProcess内の各種タイミング.
void FInstantRdvSceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    (void)InView;
    if (Pass == EPostProcessingPass::BeforeDOF && bIsPassEnabled && CVarInstantRdvBbvRadianceUpdate.GetValueOnAnyThread() != 0)
    {
        // Radiance はLighting後かつTonemap前のSceneColorが必要なため、BeforeDOFで購読する。
        InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FInstantRdvSceneViewExtension::BbvBeforeDof_RenderThread));
    }
}

FScreenPassTexture FInstantRdvSceneViewExtension::BbvBeforeDof_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
    if (CVarInstantRdvBbvEnable.GetValueOnRenderThread() == 0 || BbvSystem.IsValid() == false)
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
    if (!SceneColorSlice.IsValid())
    {
        return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
    }

    FScreenPassTexture SceneColor(SceneColorSlice);
    FRDGTextureRef SceneDepthTexture = GetViewSceneDepthTexture_RenderThread(View);
    if (SceneDepthTexture != nullptr && SceneColor.Texture != nullptr)
    {
        const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
        const bool bEnableRadianceInjection =
            CVarInstantRdvBbvRadianceUpdate.GetValueOnRenderThread() != 0 &&
            CVarInstantRdvBbvRadianceInjection.GetValueOnRenderThread() != 0;
        const bool bEnableRadianceResolve =
            CVarInstantRdvBbvRadianceUpdate.GetValueOnRenderThread() != 0 &&
            CVarInstantRdvBbvRadianceResolve.GetValueOnRenderThread() != 0;
        // BbvMaterial Radialce 更新.
        BbvSystem->ExecuteRadianceUpdate(
            GraphBuilder,
            View,
            SceneDepthTexture,
            SceneColor.Texture,
            ViewInfo.PreExposure,
            bEnableRadianceInjection,
            bEnableRadianceResolve);
        // Fsp更新.
        BbvSystem->ExecuteFspUpdate(
            GraphBuilder,
            View,
            SceneDepthTexture,
            CVarInstantRdvFspUpdate.GetValueOnRenderThread() != 0);




        // デバッグ表示.
        const int32 DebugMode = CVarInstantRdvBbvDebugMode.GetValueOnRenderThread();
        BbvSystem->ExecuteDebugVisualize(
            GraphBuilder,
            View,
            SceneDepthTexture,
            SceneColor.Texture,
            ViewInfo.PreExposure,
            DebugMode);
    }

    return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}

int32 FInstantRdvSceneViewExtension::GetPriority() const
{
    return -10;
}
