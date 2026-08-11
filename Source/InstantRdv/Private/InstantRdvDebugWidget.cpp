/*
    InstantRdvDebugWidget.cpp
*/

#include "InstantRdvDebugWidget.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
enum class EInstantRdvCVarType : uint8
{
    Bool,
    Int,
    Float,
    Action
};

struct FInstantRdvCVarDefinition
{
    const TCHAR* Category;
    const TCHAR* Label;
    const TCHAR* CVarName;
    EInstantRdvCVarType Type;
    float DefaultValue;
    float MinValue;
    float MaxValue;
};

static constexpr EConsoleVariableFlags UiSetFlags = ECVF_SetByGameSetting;
static const FNumberFormattingOptions FloatFormattingOptions = FNumberFormattingOptions().SetMaximumFractionalDigits(2);

static const FInstantRdvCVarDefinition CVarDefinitions[] =
{
    { TEXT("Runtime"), TEXT("Instant-RDV enabled"), TEXT("r.InstantRdv.GI"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("Runtime"), TEXT("BBV enabled"), TEXT("r.InstantRdv.Bbv.Enable"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },

    { TEXT("BBV"), TEXT("Main view update"), TEXT("r.InstantRdv.Bbv.MainViewUpdate"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Main view injection"), TEXT("r.InstantRdv.Bbv.MainViewInjection"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Main view removal"), TEXT("r.InstantRdv.Bbv.MainViewRemoval"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Radiance update"), TEXT("r.InstantRdv.Bbv.RadianceUpdate"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Radiance injection"), TEXT("r.InstantRdv.Bbv.RadianceInjection"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Radiance resolve"), TEXT("r.InstantRdv.Bbv.RadianceResolve"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Depth Cull mode"), TEXT("r.InstantRdv.Bbv.DepthCullMode"), EInstantRdvCVarType::Int, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Depth Injection method"), TEXT("r.InstantRdv.Bbv.DepthInjectionMethod"), EInstantRdvCVarType::Int, 1.0f, 0.0f, 1.0f },
    { TEXT("BBV"), TEXT("Injection offset (fine cells)"), TEXT("r.InstantRdv.Bbv.DepthtestInjectionOffsetFineCells"), EInstantRdvCVarType::Float, 2.0f, 0.0f, 8.0f },
    { TEXT("BBV"), TEXT("Depth relation range (fine cells)"), TEXT("r.InstantRdv.Bbv.DepthRelationRangeFineCells"), EInstantRdvCVarType::Float, 8.0f, 0.0f, 32.0f },
    { TEXT("BBV"), TEXT("Reset BBV state"), TEXT("r.InstantRdv.Bbv.Reset"), EInstantRdvCVarType::Action, 0.0f, 0.0f, 1.0f },

    { TEXT("FSP"), TEXT("FSP update"), TEXT("r.InstantRdv.Fsp.Update"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("FSP"), TEXT("Warm start"), TEXT("r.InstantRdv.Fsp.WarmStart"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("FSP"), TEXT("Relocation offset scale"), TEXT("r.InstantRdv.Fsp.RelocationOffsetScale"), EInstantRdvCVarType::Float, 0.9f, 0.0f, 1.5f },
    { TEXT("FSP"), TEXT("Trace uses probe offset"), TEXT("r.InstantRdv.Fsp.TraceUseProbeOffset"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("FSP"), TEXT("Probe visualization uses offset"), TEXT("r.InstantRdv.Fsp.VisProbeUseOffset"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },

    { TEXT("Debug"), TEXT("BBV visualization mode"), TEXT("r.InstantRdv.Bbv.VisDebug"), EInstantRdvCVarType::Int, 0.0f, 0.0f, 5.0f },
    { TEXT("Debug"), TEXT("ActiveProbe visualization mode"), TEXT("r.InstantRdv.Fsp.VisProbe"), EInstantRdvCVarType::Int, 0.0f, 0.0f, 10.0f },
    { TEXT("Debug"), TEXT("IrradianceVolume visualization mode"), TEXT("r.InstantRdv.Fsp.VisIvProbe"), EInstantRdvCVarType::Int, 0.0f, 0.0f, 2.0f },
    { TEXT("Debug"), TEXT("Debug depth test"), TEXT("r.InstantRdv.Fsp.DebugDepthTest"), EInstantRdvCVarType::Bool, 1.0f, 0.0f, 1.0f },
    { TEXT("Debug"), TEXT("Probe radius (cm)"), TEXT("r.InstantRdv.Fsp.DebugProbeRadiusCm"), EInstantRdvCVarType::Float, 10.0f, 0.0f, 100.0f },
};

static IConsoleVariable* FindCVar(const FInstantRdvCVarDefinition& Definition)
{
    return IConsoleManager::Get().FindConsoleVariable(Definition.CVarName);
}

static float GetValue(const FInstantRdvCVarDefinition& Definition)
{
    const IConsoleVariable* CVar = FindCVar(Definition);
    if (CVar == nullptr)
    {
        return Definition.DefaultValue;
    }

    return Definition.Type == EInstantRdvCVarType::Float ? CVar->GetFloat() : static_cast<float>(CVar->GetInt());
}

static FText GetValueText(const FInstantRdvCVarDefinition& Definition)
{
    const float Value = GetValue(Definition);
    if (Definition.Type == EInstantRdvCVarType::Float)
    {
        return FText::AsNumber(Value, &FloatFormattingOptions);
    }

    return FText::AsNumber(FMath::RoundToInt(Value));
}

static FText GetHelpText(const FInstantRdvCVarDefinition& Definition)
{
    if (const IConsoleVariable* CVar = FindCVar(Definition))
    {
        return FText::FromString(CVar->GetHelp());
    }

    return FText::FromString(TEXT("CVar is not registered."));
}

static void SetValue(const FInstantRdvCVarDefinition& Definition, float Value)
{
    if (IConsoleVariable* CVar = FindCVar(Definition))
    {
        if (Definition.Type == EInstantRdvCVarType::Float)
        {
            CVar->Set(Value, UiSetFlags);
        }
        else
        {
            CVar->Set(FMath::RoundToInt(Value), UiSetFlags);
        }
    }
}

static void ResetValue(const FInstantRdvCVarDefinition& Definition)
{
    SetValue(Definition, Definition.DefaultValue);
}

static void TriggerAction(const FInstantRdvCVarDefinition& Definition)
{
    SetValue(Definition, 1.0f);
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Definition](float)
        {
            SetValue(Definition, 0.0f);
            return false;
        }));
}
}

void SInstantRdvDebugPanel::Construct(const FArguments& InArgs)
{
    TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
    const TCHAR* LastCategory = nullptr;

    for (const FInstantRdvCVarDefinition& Definition : CVarDefinitions)
    {
        if (LastCategory == nullptr || FCString::Strcmp(LastCategory, Definition.Category) != 0)
        {
            LastCategory = Definition.Category;
            Content->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 8.0f, 0.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Definition.Category))
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
            ];
        }

        if (Definition.Type == EInstantRdvCVarType::Action)
        {
            TSharedPtr<bool> bShowHelp = MakeShared<bool>(false);
            TSharedRef<SHorizontalBox> ActionRow = SNew(SHorizontalBox);
            ActionRow->AddSlot()
            .FillWidth(1.0f)
            [
                SNew(SButton)
                .Text(FText::FromString(Definition.Label))
                .OnClicked_Lambda([Definition]()
                {
                    TriggerAction(Definition);
                    return FReply::Handled();
                })
            ];
            ActionRow->AddSlot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("?")))
                .ToolTipText_Lambda([Definition]()
                {
                    return GetHelpText(Definition);
                })
                .OnClicked_Lambda([bShowHelp]()
                {
                    *bShowHelp = !*bShowHelp;
                    return FReply::Handled();
                })
            ];

            TSharedRef<SVerticalBox> ActionItem = SNew(SVerticalBox);
            ActionItem->AddSlot()
            .AutoHeight()
            [
                ActionRow
            ];
            ActionItem->AddSlot()
            .AutoHeight()
            .Padding(4.0f, 2.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([Definition]()
                {
                    return GetHelpText(Definition);
                })
                .Visibility_Lambda([bShowHelp]()
                {
                    return *bShowHelp ? EVisibility::Visible : EVisibility::Collapsed;
                })
            ];

            Content->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 2.0f)
            [
                ActionItem
            ];
            continue;
        }

        TSharedPtr<bool> bShowHelp = MakeShared<bool>(false);
        TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
        Row->AddSlot()
        .FillWidth(0.35f)
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Definition.Label))
        ];
        Row->SetToolTipText(GetHelpText(Definition));

        if (Definition.Type == EInstantRdvCVarType::Bool)
        {
            Row->AddSlot()
            .FillWidth(0.45f)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([Definition]()
                {
                    return GetValue(Definition) != 0.0f ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([Definition](ECheckBoxState State)
                {
                    SetValue(Definition, State == ECheckBoxState::Checked ? 1.0f : 0.0f);
                })
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Enabled")))
                ]
            ];
        }
        else
        {
            Row->AddSlot()
            .FillWidth(0.4f)
            .VAlign(VAlign_Center)
            [
                SNew(SSlider)
                .PreventThrottling(true)
                .Value_Lambda([Definition]()
                {
                    return FMath::GetRangePct(Definition.MinValue, Definition.MaxValue, GetValue(Definition));
                })
                .OnValueChanged_Lambda([Definition](float NormalizedValue)
                {
                    SetValue(Definition, FMath::Lerp(Definition.MinValue, Definition.MaxValue, NormalizedValue));
                })
            ];
            Row->AddSlot()
            .AutoWidth()
            .Padding(6.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([Definition]()
                {
                    return GetValueText(Definition);
                })
            ];
        }

        Row->AddSlot()
        .AutoWidth()
        [
            SNew(SButton)
            .Text(FText::FromString(TEXT("?")))
            .ToolTipText_Lambda([Definition]()
            {
                return GetHelpText(Definition);
            })
            .OnClicked_Lambda([bShowHelp]()
            {
                *bShowHelp = !*bShowHelp;
                return FReply::Handled();
            })
        ];

        Row->AddSlot()
        .AutoWidth()
        [
            SNew(SButton)
            .Text(FText::FromString(TEXT("Reset")))
            .OnClicked_Lambda([Definition]()
            {
                ResetValue(Definition);
                return FReply::Handled();
            })
        ];

        TSharedRef<SVerticalBox> Item = SNew(SVerticalBox);
        Item->AddSlot()
        .AutoHeight()
        [
            Row
        ];
        Item->AddSlot()
        .AutoHeight()
        .Padding(4.0f, 2.0f, 0.0f, 0.0f)
        [
            SNew(STextBlock)
            .Text_Lambda([Definition]()
            {
                return GetHelpText(Definition);
            })
            .Visibility_Lambda([bShowHelp]()
            {
                return *bShowHelp ? EVisibility::Visible : EVisibility::Collapsed;
            })
        ];

        Content->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 2.0f)
        [
            Item
        ];
    }

    ChildSlot
    [
        SNew(SBorder)
        .Padding(8.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Instant-RDV Debug")))
                    .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Reset All")))
                    .OnClicked(this, &SInstantRdvDebugPanel::ResetAllSettings)
                ]
            ]
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    Content
                ]
            ]
        ]
    ];
}

FReply SInstantRdvDebugPanel::ResetAllSettings()
{
    for (const FInstantRdvCVarDefinition& Definition : CVarDefinitions)
    {
        if (Definition.Type != EInstantRdvCVarType::Action)
        {
            ResetValue(Definition);
        }
    }

    return FReply::Handled();
}
