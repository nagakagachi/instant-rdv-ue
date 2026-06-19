#include "InstantRdvModule.h"

#include "Engine/Engine.h"
#include "InstantRdvSceneViewExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

void FInstantRdvModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InstantRdv"));
    if (Plugin.IsValid())
    {
        const FString PluginShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/InstantRdvShaders"), PluginShaderDir);
        bShaderDirectoryMapped = true;
    }

    if (!FApp::CanEverRender())
    {
        return;
    }

    if (GEngine)
    {
        RegisterSceneViewExtension();
    }
    else
    {
        PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FInstantRdvModule::RegisterSceneViewExtension);//PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FInstantRdvModule::RegisterSceneViewExtension);
    }
}

void FInstantRdvModule::ShutdownModule()
{
    if (PostEngineInitHandle.IsValid())
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);//FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
        PostEngineInitHandle.Reset();
    }

    SceneViewExtension.Reset();
    bShaderDirectoryMapped = false;
}

void FInstantRdvModule::RegisterSceneViewExtension()
{
    if (!SceneViewExtension.IsValid() && GEngine)
    {
        SceneViewExtension = FSceneViewExtensions::NewExtension<FInstantRdvSceneViewExtension>();
    }
}

IMPLEMENT_MODULE(FInstantRdvModule, InstantRdv)
