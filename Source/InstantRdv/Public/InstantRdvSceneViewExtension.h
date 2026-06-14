#pragma once

#include "SceneViewExtension.h"

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
    void ExecuteBbvGeometryUpdate_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTexture* SceneDepthTexture);
    FScreenPassTexture BbvBeforeDof_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

    TUniquePtr<FInstantRdvBbv> BbvSystem;
    TArray<const FSceneView*> FrameViews_RenderThread;
};
