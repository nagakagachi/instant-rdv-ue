#pragma once

#include "SceneViewExtension.h"

class FInstantRdvBbv;
class FRDGBuilder;
struct FPostProcessingInputs;

class FInstantRdvSceneViewExtension final : public FSceneViewExtensionBase
{
public:
    explicit FInstantRdvSceneViewExtension(const FAutoRegister& AutoRegister);
    virtual ~FInstantRdvSceneViewExtension() override;

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;
    virtual int32 GetPriority() const override;

private:
    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

    TUniquePtr<FInstantRdvBbv> BbvSystem;
};
