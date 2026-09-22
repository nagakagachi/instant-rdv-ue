/*
    InstantRdvEditorModule.h
*/

#pragma once

#include "Modules/ModuleManager.h"

class FInstantRdvEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterDebugTab();
    void UnregisterDebugTab();
};
