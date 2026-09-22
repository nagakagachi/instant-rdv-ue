/*
    InstantRdvSceneViewExtension.cpp

    UEのSceneViewExtensionへ接続し、BBV/FSPのRenderThread更新、
    ポストプロセス処理、デバッグ可視化のタイミングを管理する。
*/

#include "InstantRdvSceneViewExtension.h"

#include "HAL/IConsoleManager.h"
#include "InstantRdvBbv.h"
#include "InstantRdvConsoleVariables.h"
#include "InstantRdvRuntimeSettingsRegistry.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PostProcess/PostProcessInputs.h"
#include "SceneRendering.h"

#include "InstantRdvSceneUniformBuffer.h"

namespace
{
INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvEnable,
    TEXT("r.InstantRdv.GI"),
    1,
    TEXT("Enable Instant-RDV runtime hooks.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("Runtime"),
    TEXT("Instant-RDV enabled"),
    0);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvEnable,
    TEXT("r.InstantRdv.Bbv.Enable"),
    1,
    TEXT("Enable Instant-RDV BBV update passes.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("Runtime"),
    TEXT("BBV enabled"),
    10);

INSTANT_RDV_CVAR_INT(
    CVarInstantRdvBbvVisDebug,
    TEXT("r.InstantRdv.Bbv.VisDebug"),
    0,
    TEXT("Instant-RDV BBV voxel debug mode selector.\n")
    TEXT("0: Off\n")
    TEXT("1: XY fine-voxel Z-count map (no raytrace)\n")
    TEXT("2: Brick raytrace debug\n")
    TEXT("3: Voxel raytrace debug\n")
    TEXT("4: Voxel radiance debug"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("BBV visualization mode"),
    0.0f,
    4.0f,
    0);

INSTANT_RDV_CVAR_FLOAT(
    CVarInstantRdvBbvDebugSceneColorBlend,
    TEXT("r.InstantRdv.Bbv.DebugSceneColorBlend"),
    0.5f,
    TEXT("SceneColor contribution for BBV debug visualization.\n")
    TEXT("0: Fully overwrite SceneColor where debug output exists\n")
    TEXT("1: SceneColor only"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("BBV debug SceneColor blend"),
    0.0f,
    1.0f,
    2);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvDebugDepthTest,
    TEXT("r.InstantRdv.Bbv.DebugDepthTest"),
    0,
    TEXT("Enable SceneDepth testing for BBV raytrace debug visualization.\n")
    TEXT("0: Draw BBV hits regardless of SceneDepth\n")
    TEXT("1: Draw only BBV hits in front of the SceneDepth surface"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("BBV debug depth test"),
    3);

INSTANT_RDV_CVAR_INT(
    CVarInstantRdvFspVisProbe,
    TEXT("r.InstantRdv.Fsp.VisProbe"),
    0,
    TEXT("Instant-RDV FSP active probe debug mode selector.\n")
    TEXT("0: Off\n")
    TEXT("1: Active probe liveness\n")
    TEXT("2: Active probe index hash\n")
    TEXT("3: Active probe age\n")
    TEXT("4: Active probe cascade\n")
    TEXT("5: Active probe OctMap radiance\n")
    TEXT("6: Active probe OctMap sky visibility\n")
    TEXT("7: Active probe SH radiance\n")
    TEXT("8: Active probe SH sky visibility\n")
    TEXT("9: Probe sample position embedded in BBV occupancy\n")
    TEXT("10: Camera-to-relocated-probe BBV reachability (blue=outside, red=blocked, green=reached)\n")
    TEXT("Active probe modes also draw counters at top-left: cyan=visible cells, green=active probes, yellow=ray requests, orange=ray results, purple=free probes"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("ActiveProbe visualization mode"),
    0.0f,
    10.0f,
    10);

INSTANT_RDV_CVAR_INT(
    CVarInstantRdvFspVisIvProbe,
    TEXT("r.InstantRdv.Fsp.VisIvProbe"),
    0,
    TEXT("Instant-RDV FSP irradiance volume probe debug mode selector.\n")
    TEXT("0: Off\n")
    TEXT("1: IrradianceVolume SH radiance\n")
    TEXT("2: IrradianceVolume SH sky visibility"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("IrradianceVolume visualization mode"),
    0.0f,
    2.0f,
    20);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvFspProbeDebugDepthTest,
    TEXT("r.InstantRdv.Fsp.ProbeDebugDepthTest"),
    1,
    TEXT("Enable SceneDepth testing for FSP probe debug spheres.\n")
    TEXT("0: Draw without depth test\n")
    TEXT("1: Respect SceneDepth occlusion"),
    ECVF_RenderThreadSafe,
    TEXT("Debug"),
    TEXT("FSP probe debug depth test"),
    30);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvMainViewInjection,
    TEXT("r.InstantRdv.Bbv.MainViewInjection"),
    1,
    TEXT("Enable BBV main view injection pass.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Main view injection"),
    10);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvMainViewRemoval,
    TEXT("r.InstantRdv.Bbv.MainViewRemoval"),
    1,
    TEXT("Enable BBV main view removal pass.\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Main view removal"),
    20);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvMainViewUpdate,
    TEXT("r.InstantRdv.Bbv.MainViewUpdate"),
    1,
    TEXT("Master toggle for BBV main view update passes (Injection + Removal).\n")
    TEXT("0: Disable both Injection and Removal\n")
    TEXT("1: Enable by individual pass toggles"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Main view update"),
    0);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvUseReducedSurfaceBuffer,
    TEXT("r.InstantRdv.UseReducedSurfaceBuffer"),
    1,
    TEXT("Select the MainView ReducedSurfaceBuffer path for BBV and FSP.\n")
    TEXT("0: Legacy full-resolution Depth path\n")
    TEXT("1: ReducedSurfaceBuffer path"),
    ECVF_RenderThreadSafe,
    TEXT("Runtime"),
    TEXT("Use ReducedSurfaceBuffer"),
    20);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvRadianceUpdate,
    TEXT("r.InstantRdv.Bbv.RadianceUpdate"),
    1,
    TEXT("BBV Radiance更新（Injection + Resolve）の有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled at SubscribeToPostProcessingPass BeforeDOF"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Radiance update"),
    100);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvRadianceInjection,
    TEXT("r.InstantRdv.Bbv.RadianceInjection"),
    1,
    TEXT("BBV Radiance Injection passの有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Radiance injection"),
    110);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvBbvRadianceResolve,
    TEXT("r.InstantRdv.Bbv.RadianceResolve"),
    1,
    TEXT("BBV Radiance Resolve passの有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled"),
    ECVF_RenderThreadSafe,
    TEXT("BBV"),
    TEXT("Radiance resolve"),
    120);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvFspUpdate,
    TEXT("r.InstantRdv.Fsp.Update"),
    1,
    TEXT("Frustum Space Probe(FSP) lifecycle / ray trace / SH update の有効化。\n")
    TEXT("0: Disabled\n")
    TEXT("1: Enabled after BBV Radiance Resolve"),
    ECVF_RenderThreadSafe,
    TEXT("FSP"),
    TEXT("FSP update"),
    0);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvFspTraceUseProbeOffset,
    TEXT("r.InstantRdv.Fsp.TraceUseProbeOffset"),
    1,
    TEXT("Apply active probe offset to FSP ray trace origins.\n")
    TEXT("0: Trace from owner cell center\n")
    TEXT("1: Trace from offset probe sample position"),
    ECVF_RenderThreadSafe,
    TEXT("FSP"),
    TEXT("Trace uses probe offset"),
    30);

INSTANT_RDV_CVAR_BOOL(
    CVarInstantRdvFspVisProbeUseOffset,
    TEXT("r.InstantRdv.Fsp.VisProbeUseOffset"),
    1,
    TEXT("Apply active probe offset to ActiveProbe debug billboard positions.\n")
    TEXT("0: Draw at owner cell center\n")
    TEXT("1: Draw at offset probe sample position"),
    ECVF_RenderThreadSafe,
    TEXT("FSP"),
    TEXT("Probe visualization uses offset"),
    40);

static FRDGTextureRef GetViewSceneDepthTexture_RenderThread(const FSceneView& View)
{
    // SceneViewExtension のコールバック実体は FViewInfo なので、Renderer内部情報から Depth RDG を取得する。
    const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
    return ViewInfo.GetSceneTextures().Depth.Target;
}

static bool IsRdvEligibleViewFamily_RenderThread(const FSceneViewFamily& ViewFamily)
{
    // ViewFamilyはレンダリング要求ごとの一時オブジェクトで、Editorでは同一engine frameに複数生成される。
    // HitProxyや追加ViewFamilyはRDVのscene lighting lifecycleを進める対象ではないため、ここで入口から除外する。
    return
        ViewFamily.Scene != nullptr &&
        ViewFamily.Views.Num() > 0 &&
        !ViewFamily.EngineShowFlags.HitProxies &&
        !ViewFamily.bAdditionalViewFamily;
}

static bool IsRdvEligibleView_RenderThread(const FSceneView& View)
{
    // RDV/FSPはLumenに近いscene/view-family更新なので、永続的なViewStateを持つ通常Viewだけをowner候補にする。
    // SceneCapture/Reflection/Planar/RVT/Offline系は同じSceneViewExtensionへ到達しても、別用途の副次ViewFamilyとして扱う。
    return
        View.bIsViewInfo &&
        View.State != nullptr &&
        !View.bIsSceneCapture &&
        !View.bIsReflectionCapture &&
        !View.bIsPlanarReflection &&
        !View.bIsVirtualTexture &&
        !View.bIsOfflineRender &&
        View.IsPerspectiveProjection();
}
}

FInstantRdvSceneViewExtension::FInstantRdvSceneViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
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

void FInstantRdvSceneViewExtension::ResetAcceptedViewFamilyState_RenderThread()
{
    FrameViews_RenderThread.Reset();
    AcceptedViewFamily_RenderThread = nullptr;
    UpdateOwnerView_RenderThread = nullptr;
    bUseReducedSurfaceBufferForAcceptedFamily_RenderThread = true;
    bAcceptedFamilyPostProcessUpdated_RenderThread = false;
}

bool FInstantRdvSceneViewExtension::TryAcceptViewFamilyForRdv_RenderThread(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily)
{
    const FSceneView* OwnerView = FindRdvUpdateView_RenderThread(ViewFamily);
    const TSharedPtr<const FInstantRdvRenderSettings, ESPMode::ThreadSafe> RenderSettings =
        FInstantRdvRuntimeSettingsRegistry::Find(ViewFamily.Scene);
    if (OwnerView == nullptr || IsRdvFamilyAlreadyUpdated_RenderThread(ViewFamily) ||
        RenderSettings == nullptr || !RenderSettings->LevelSettings.bEnabled)
    {
        return false;
    }

    if (ActiveScene_RenderThread != ViewFamily.Scene || ActiveSettingsRevision_RenderThread != RenderSettings->Revision)
    {
        const FInstantRdvLevelSettings& Settings = RenderSettings->LevelSettings;
        FInstantRdvBbvConfig BbvConfig;
        BbvConfig.BbvGridResolution = FIntVector(FMath::Max(Settings.BbvGridResolution.X, 1), FMath::Max(Settings.BbvGridResolution.Y, 1), FMath::Max(Settings.BbvGridResolution.Z, 1));
        BbvConfig.BbvBrickSizeCm = FMath::Max(Settings.BbvBrickSizeCm, 1.0f);
        FInstantRdvFspConfig FspConfig;
        FspConfig.ProbeGridResolution = FIntVector(FMath::Max(Settings.ProbeGridResolution.X, 1), FMath::Max(Settings.ProbeGridResolution.Y, 1), FMath::Max(Settings.ProbeGridResolution.Z, 1));
        FspConfig.ProbeCellSizeCm = FMath::Max(Settings.ProbeCellSizeCm, 1.0f);
        FspConfig.ProbeCascadeCount = FMath::Max(Settings.ProbeCascadeCount, 1);
        FspConfig.ProbeCapacity = FMath::Max(Settings.ProbePoolCapacity, 1);
        FspConfig.VisibleSurfaceCapacity = FMath::Max(Settings.VisibleSurfaceCapacity, 1);
        BbvSystem = MakeUnique<FInstantRdvBbv>(BbvConfig, FspConfig);
        BbvSystem->Initialize();
        ActiveScene_RenderThread = ViewFamily.Scene;
        ActiveSettingsRevision_RenderThread = RenderSettings->Revision;
        ActiveLevelSettings_RenderThread = Settings;
    }

    // ここがRDV lifecycleの唯一の入口。
    // BeginFrameはFrameCountを進め、ActiveProbeListのCurr/Prev世代を決めるため、
    // View単位callbackやsecondary ViewFamilyから呼ぶと参照InstantRDVのdouble bufferingが壊れる。
    //
    // Reduced/Legacyの選択もこの時点でViewFamily単位に固定する。
    // BasePass前とBeforeDOFの間でRenderThreadSafe CVarが変更されても、同じ更新世代の
    // BBV Geometry、Radiance、FSPが異なる入力方式を使わないことが重要。
    bUseReducedSurfaceBufferForAcceptedFamily_RenderThread =
        CVarInstantRdvUseReducedSurfaceBuffer.GetValueOnRenderThread() != 0;
    BbvSystem->BeginFrame_RenderThread(GraphBuilder, *OwnerView);
    AcceptedViewFamily_RenderThread = &ViewFamily;
    UpdateOwnerView_RenderThread = OwnerView;
    LastRdvUpdateFrameCounter_RenderThread = ViewFamily.FrameCounter;
    LastRdvUpdateFrameNumber_RenderThread = ViewFamily.FrameNumber;
    return true;
}

const FSceneView* FInstantRdvSceneViewExtension::FindRdvUpdateView_RenderThread(const FSceneViewFamily& ViewFamily) const
{
    if (!IsRdvEligibleViewFamily_RenderThread(ViewFamily))
    {
        return nullptr;
    }

    const FSceneView* FirstEligibleView = nullptr;
    for (const FSceneView* View : ViewFamily.Views)
    {
        if (View == nullptr || !IsRdvEligibleView_RenderThread(*View))
        {
            continue;
        }

        // Game/PIE viewが含まれるViewFamilyではそれを最優先にする。
        // Editor viewportだけで動作確認するケースもあるため、GameViewが無ければ最初の通常Viewをfallback ownerにする。
        if (View->bIsGameView || ViewFamily.EngineShowFlags.Game)
        {
            return View;
        }

        if (FirstEligibleView == nullptr)
        {
            FirstEligibleView = View;
        }
    }

    return FirstEligibleView;
}

bool FInstantRdvSceneViewExtension::IsAcceptedViewFamily_RenderThread(const FSceneViewFamily* ViewFamily) const
{
    return ViewFamily != nullptr && AcceptedViewFamily_RenderThread == ViewFamily;
}

bool FInstantRdvSceneViewExtension::IsRdvUpdateView_RenderThread(const FSceneView& View) const
{
    return IsAcceptedViewFamily_RenderThread(View.Family) && UpdateOwnerView_RenderThread == &View;
}

bool FInstantRdvSceneViewExtension::CanRunDebugVisualize_RenderThread(const FSceneView& View) const
{
    // Debug描画はRDV lifecycleを書き換えない読み取り専用。
    // owner ViewのBeforeDOF更新が完了していない場合は、古い/未接続リソースを読むので抑止する。
    return IsAcceptedViewFamily_RenderThread(View.Family) && bAcceptedFamilyPostProcessUpdated_RenderThread;
}

bool FInstantRdvSceneViewExtension::IsRdvFamilyAlreadyUpdated_RenderThread(const FSceneViewFamily& ViewFamily) const
{
    // FSceneViewFamily*は一時オブジェクトなので永続キーにしない。
    // GFrameCounter由来のFrameCounterを主キーにし、古い経路で0の場合のみFrameNumberをfallbackにする。
    if (ViewFamily.FrameCounter != 0)
    {
        return LastRdvUpdateFrameCounter_RenderThread == ViewFamily.FrameCounter;
    }

    return LastRdvUpdateFrameCounter_RenderThread == 0 &&
        LastRdvUpdateFrameNumber_RenderThread == ViewFamily.FrameNumber;
}

void FInstantRdvSceneViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
    ResetAcceptedViewFamilyState_RenderThread();
    const bool bAcceptedForRdv = TryAcceptViewFamilyForRdv_RenderThread(GraphBuilder, InViewFamily);


    ISceneRenderer* sceneRenderer = InViewFamily.GetSceneRenderer();
    if (sceneRenderer)
    {
        FSceneUniformBuffer& sceneUniformBuffer = sceneRenderer->GetSceneUniforms();
        FInstantRdvSceneUniformBufferParams params{};
        InitializeInstantRdvSceneUniformBufferDefaults(params, GraphBuilder);
        if (BbvSystem.IsValid())
        {
            BbvSystem->FillSceneUniformBufferParams_RenderThread(
                GraphBuilder, params, bAcceptedForRdv);
        }
        sceneUniformBuffer.Set(SceneUB::InstantRdvParam, params);
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
        bEnableMainViewGeometryRemoval,
        bUseReducedSurfaceBufferForAcceptedFamily_RenderThread);
}

void FInstantRdvSceneViewExtension::PreRenderBasePass_RenderThread(FRDGBuilder& GraphBuilder, bool bDepthBufferIsPopulated)
{
    if (!bDepthBufferIsPopulated)
    {
        return;
    }

    for (const FSceneView* View : FrameViews_RenderThread)
    {
        if (View == nullptr || !IsRdvUpdateView_RenderThread(*View))
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
    if (Pass == EPostProcessingPass::BeforeDOF && bIsPassEnabled && IsRdvEligibleView_RenderThread(InView))
    {
        // Radiance はLighting後かつTonemap前のSceneColorが必要なため、BeforeDOFで購読する。
        // 実際のowner判定はBbvBeforeDof側で行う。
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
        if (IsRdvUpdateView_RenderThread(View))
        {
            const bool bEnableRadianceUpdate = (CVarInstantRdvBbvRadianceUpdate.GetValueOnRenderThread() != 0);
            const bool bEnableRadianceInjection =
                bEnableRadianceUpdate &&
                (CVarInstantRdvBbvRadianceInjection.GetValueOnRenderThread() != 0);
            const bool bEnableRadianceResolve =
                bEnableRadianceUpdate &&
                (CVarInstantRdvBbvRadianceResolve.GetValueOnRenderThread() != 0);
            // BBV RadianceとFSPは同じowner ViewのDepth/SceneColorを入力にして、ViewFamily内で1回だけ更新する。
            // FSP ActiveProbeListはここで生成されたCurr世代を、その後のdebug表示が読み取るだけにする。
            BbvSystem->ExecuteRadianceUpdate(
                GraphBuilder,
                View,
                SceneDepthTexture,
                SceneColor.Texture,
                ViewInfo.PreExposure,
                bEnableRadianceInjection,
                bEnableRadianceResolve,
                bUseReducedSurfaceBufferForAcceptedFamily_RenderThread);
            BbvSystem->ExecuteFspUpdate(
                GraphBuilder,
                View,
                SceneDepthTexture,
                CVarInstantRdvFspUpdate.GetValueOnRenderThread() != 0,
                CVarInstantRdvFspTraceUseProbeOffset.GetValueOnRenderThread() != 0,
                bUseReducedSurfaceBufferForAcceptedFamily_RenderThread);
            bAcceptedFamilyPostProcessUpdated_RenderThread = true;
        }

        const int32 BbvDebugMode = CVarInstantRdvBbvVisDebug.GetValueOnRenderThread();
        const int32 FspProbeDebugMode = CVarInstantRdvFspVisProbe.GetValueOnRenderThread();
        const int32 FspIvProbeDebugMode = CVarInstantRdvFspVisIvProbe.GetValueOnRenderThread();
        const bool bProbeDepthTest = (CVarInstantRdvFspProbeDebugDepthTest.GetValueOnRenderThread() != 0);
        const float BbvDebugSceneColorBlend = CVarInstantRdvBbvDebugSceneColorBlend.GetValueOnRenderThread();
        const bool bBbvDebugDepthTest = (CVarInstantRdvBbvDebugDepthTest.GetValueOnRenderThread() != 0);
        const bool bUseProbeTraceOffset = (CVarInstantRdvFspTraceUseProbeOffset.GetValueOnRenderThread() != 0);
        const bool bUseProbeVisualizationOffset = (CVarInstantRdvFspVisProbeUseOffset.GetValueOnRenderThread() != 0);
        if (CanRunDebugVisualize_RenderThread(View))
        {
            BbvSystem->ExecuteDebugVisualize(
                GraphBuilder,
                View,
                SceneDepthTexture,
                SceneColor.Texture,
                ViewInfo.PreExposure,
                BbvDebugMode,
                FspProbeDebugMode,
                FspIvProbeDebugMode,
                bProbeDepthTest,
                BbvDebugSceneColorBlend,
                bBbvDebugDepthTest,
                bUseProbeVisualizationOffset,
                bUseProbeTraceOffset);
        }
    }

    return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}

int32 FInstantRdvSceneViewExtension::GetPriority() const
{
    return -10;
}
