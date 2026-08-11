/*
    InstantRdvEditorModule.cpp

    Editor専用モジュールとしてInstant-RDVデバッグSlateパネルの
    Nomad Tab登録とLevel Editorへの公開を担当する。
*/

#include "InstantRdvEditorModule.h"

#include "InstantRdvDebugWidget.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

namespace
{
static const FName InstantRdvDebugTabName(TEXT("InstantRdv.Debug"));
static const FName InstantRdvWorkspaceGroupName(TEXT("InstantRdv"));
}

void FInstantRdvEditorModule::StartupModule()
{
    RegisterDebugTab();
}

void FInstantRdvEditorModule::ShutdownModule()
{
    UnregisterDebugTab();
}

void FInstantRdvEditorModule::RegisterDebugTab()
{
    const TSharedRef<FWorkspaceItem> InstantRdvWorkspaceGroup =
        WorkspaceMenu::GetMenuStructure().GetDeveloperToolsDebugCategory()->AddGroup(
            InstantRdvWorkspaceGroupName,
            FText::FromString(TEXT("Instant-RDV")));

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        InstantRdvDebugTabName,
        FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
        {
            return SNew(SDockTab)
                .TabRole(ETabRole::NomadTab)
                [
                    SNew(SBox)
                    .WidthOverride(800.0f)
                    .HeightOverride(1500.0f)
                    [
                        SNew(SInstantRdvDebugPanel)
                    ]
                ];
        }))
        .SetDisplayName(FText::FromString(TEXT("Instant-RDV Debug")))
        .SetTooltipText(FText::FromString(TEXT("Open the Instant-RDV debug controls.")))
        .SetGroup(InstantRdvWorkspaceGroup);
}

void FInstantRdvEditorModule::UnregisterDebugTab()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(InstantRdvDebugTabName);
}

IMPLEMENT_MODULE(FInstantRdvEditorModule, InstantRdvEditor)
