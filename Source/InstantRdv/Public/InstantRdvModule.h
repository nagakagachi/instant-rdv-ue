#pragma once

#include "Delegates/Delegate.h"
#include "Modules/ModuleManager.h"

class FInstantRdvSceneViewExtension;

class FInstantRdvModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterSceneViewExtension();

    bool bShaderDirectoryMapped = false;
    FDelegateHandle PostEngineInitHandle;
    TSharedPtr<FInstantRdvSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
