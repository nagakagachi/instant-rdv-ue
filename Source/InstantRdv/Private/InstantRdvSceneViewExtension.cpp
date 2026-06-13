#include "InstantRdvSceneViewExtension.h"

#include "HAL/IConsoleManager.h"
#include "InstantRdvBbv.h"
#include "PostProcess/PostProcessInputs.h"

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

    const int32 DebugMode = CVarInstantRdvBbvDebugMode.GetValueOnRenderThread();
    const bool bEnableMainViewUpdate = (CVarInstantRdvBbvMainViewUpdate.GetValueOnRenderThread() != 0);
    const bool bEnableMainViewInjection = bEnableMainViewUpdate && (CVarInstantRdvBbvMainViewInjection.GetValueOnRenderThread() != 0);
    const bool bEnableMainViewRemoval = bEnableMainViewUpdate && (CVarInstantRdvBbvMainViewRemoval.GetValueOnRenderThread() != 0);
    BbvSystem->Execute(
        GraphBuilder,
        View,
        SceneDepthTexture,
        SceneTextureParameters->SceneColorTexture,
        DebugMode,
        bEnableMainViewInjection,
        bEnableMainViewRemoval);
}

int32 FInstantRdvSceneViewExtension::GetPriority() const
{
    return -10;
}
