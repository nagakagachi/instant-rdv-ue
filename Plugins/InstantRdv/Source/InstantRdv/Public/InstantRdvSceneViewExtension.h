/*
    InstantRdvSceneViewExtension.h
*/

#pragma once

#include "SceneViewExtension.h"
#include "InstantRdvRuntimeSettings.h"

class FInstantRdvBbv;
class FRDGBuilder;
class FRDGTexture;
class FSceneView;
struct FPostProcessMaterialInputs;
struct FPostProcessingInputs;
struct FScreenPassTexture;

class FInstantRdvSceneViewExtension final : public FSceneViewExtensionBase
{
public:
    explicit FInstantRdvSceneViewExtension(const FAutoRegister& AutoRegister);
    virtual ~FInstantRdvSceneViewExtension() override;

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
    virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
    virtual void PreRenderBasePass_RenderThread(FRDGBuilder& GraphBuilder, bool bDepthBufferIsPopulated) override;
    virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;
    virtual int32 GetPriority() const override;

private:
    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
    void ResetAcceptedViewFamilyState_RenderThread();
    bool TryAcceptViewFamilyForRdv_RenderThread(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily);
    const FSceneView* FindRdvUpdateView_RenderThread(const FSceneViewFamily& ViewFamily) const;
    bool IsAcceptedViewFamily_RenderThread(const FSceneViewFamily* ViewFamily) const;
    bool IsRdvUpdateView_RenderThread(const FSceneView& View) const;
    bool CanRunDebugVisualize_RenderThread(const FSceneView& View) const;
    bool IsRdvFamilyAlreadyUpdated_RenderThread(const FSceneViewFamily& ViewFamily) const;
    void ExecuteBbvGeometryUpdate_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTexture* SceneDepthTexture);
    FScreenPassTexture BbvBeforeDof_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

    TUniquePtr<FInstantRdvBbv> BbvSystem;
    const FSceneInterface* ActiveScene_RenderThread = nullptr;
    uint64 ActiveSettingsRevision_RenderThread = MAX_uint64;
    FInstantRdvLevelSettings ActiveLevelSettings_RenderThread;
    TArray<const FSceneView*> FrameViews_RenderThread;
    const FSceneViewFamily* AcceptedViewFamily_RenderThread = nullptr;
    const FSceneView* UpdateOwnerView_RenderThread = nullptr;
    uint64 LastRdvUpdateFrameCounter_RenderThread = MAX_uint64;
    uint32 LastRdvUpdateFrameNumber_RenderThread = MAX_uint32;
    // Geometry(BasePass前)とRadiance/VSP(BeforeDOF)は別callbackで実行される。
    // callback途中のCVar変更で経路が混在しないよう、owner ViewFamily受理時の値を固定して使う。
    bool bUseReducedSurfaceBufferForAcceptedFamily_RenderThread = true;
    bool bAcceptedFamilyPostProcessUpdated_RenderThread = false;
};
