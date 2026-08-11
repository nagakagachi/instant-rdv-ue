/*
    InstantRdvModule.h
*/

#pragma once

#include "CoreMinimal.h"
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

    FDelegateHandle PostEngineInitHandle;
    TSharedPtr<FInstantRdvSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
