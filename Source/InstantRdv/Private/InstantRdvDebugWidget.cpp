/*
    InstantRdvDebugWidget.cpp

    Instant-RDVのCVarを列挙・分類・ソートし、チェックボックス、
    スライダ、Reset、Tooltip、詳細説明を持つSlateデバッグUIへ構築する。
*/

#include "InstantRdvDebugWidget.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InstantRdvConsoleVariables.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
struct FInstantRdvCVarDefinition
{
    FString CategoryId;
    FString Category;
    FString Label;
    FString CVarName;
    EInstantRdvCVarUiType Type = EInstantRdvCVarUiType::Int;
    float MinValue;
    float MaxValue;
    int32 CategoryOrder = MAX_int32;
    int32 ItemOrder = MAX_int32;
};

static constexpr EConsoleVariableFlags UiSetFlags = ECVF_SetByGameSetting;
static const FNumberFormattingOptions FloatFormattingOptions = FNumberFormattingOptions().SetMaximumFractionalDigits(2);

static FInstantRdvCVarDefinition MakeDefinition(const FInstantRdvCVarUiMetadata& Metadata)
{
    // 生成した定義はループ終了後もSlateのLambdaから参照されるため、
    // リテラルのメタデータを所有文字列へコピーする。
    FInstantRdvCVarDefinition Definition;
    Definition.CategoryId = Metadata.CategoryId;
    Definition.Category = TEXT("Unclassified");
    Definition.Label = Metadata.Label;
    Definition.CVarName = Metadata.CVarName;
    Definition.Type = Metadata.Type;
    Definition.MinValue = Metadata.MinValue;
    Definition.MaxValue = Metadata.MaxValue;
    Definition.ItemOrder = Metadata.ItemOrder;

    if (const FInstantRdvCVarUiCategory* Category = FInstantRdvCVarUiRegistry::FindCategory(Metadata.CategoryId))
    {
        Definition.Category = Category->Label;
        Definition.CategoryOrder = Category->SortOrder;
    }
    else
    {
        // 古いカテゴリIDや誤記でCVarを非表示にしない。
        // 最後のフォールバックカテゴリへ表示したうえで、開発ビルドでは
        // 登録ミスをensureで明示する。
        ensureMsgf(false, TEXT("Instant-RDV CVar metadata refers to an unregistered category: %s"), Metadata.CategoryId);
        Definition.CategoryId = TEXT("Unclassified");
        if (const FInstantRdvCVarUiCategory* FallbackCategory = FInstantRdvCVarUiRegistry::FindCategory(TEXT("Unclassified")))
        {
            Definition.Category = FallbackCategory->Label;
            Definition.CategoryOrder = FallbackCategory->SortOrder;
        }
    }

    return Definition;
}

static IConsoleVariable* FindCVar(const FInstantRdvCVarDefinition& Definition)
{
    return IConsoleManager::Get().FindConsoleVariable(*Definition.CVarName);
}

static float GetValue(const FInstantRdvCVarDefinition& Definition)
{
    IConsoleVariable* CVar = FindCVar(Definition);
    if (CVar == nullptr)
    {
        return 0.0f;
    }

    return Definition.Type == EInstantRdvCVarUiType::Float ? CVar->GetFloat() : static_cast<float>(CVar->GetInt());
}

static FText GetValueText(const FInstantRdvCVarDefinition& Definition)
{
    const float Value = GetValue(Definition);
    if (Definition.Type == EInstantRdvCVarUiType::Float)
    {
        return FText::AsNumber(Value, &FloatFormattingOptions);
    }

    return FText::AsNumber(FMath::RoundToInt(Value));
}

static FText GetHelpText(const FInstantRdvCVarDefinition& Definition)
{
    if (const IConsoleVariable* CVar = FindCVar(Definition))
    {
        const FString Help = CVar->GetHelp();
        return FText::FromString(
            FString::Printf(TEXT("%s\n\n%s"), *Definition.CVarName, *Help));
    }

    return FText::FromString(
        FString::Printf(TEXT("%s\n\nCVar is not registered."), *Definition.CVarName));
}

static FReply CopyCVarName(const FInstantRdvCVarDefinition& Definition)
{
    FPlatformApplicationMisc::ClipboardCopy(*Definition.CVarName);
    return FReply::Handled();
}

static float GetDefaultValue(const FInstantRdvCVarDefinition& Definition)
{
    IConsoleVariable* CVar = FindCVar(Definition);
    if (CVar == nullptr)
    {
        return 0.0f;
    }

    // Reset値はUI側の二重定義ではなくCVar登録から取得する。
    // Config／Console経由の動作とSlateボタンの既定値を一致させる。
    const FString DefaultValue = CVar->GetDefaultValue();
    return Definition.Type == EInstantRdvCVarUiType::Float
        ? FCString::Atof(*DefaultValue)
        : static_cast<float>(FCString::Atoi(*DefaultValue));
}

static void SetValue(const FInstantRdvCVarDefinition& Definition, float Value)
{
    if (IConsoleVariable* CVar = FindCVar(Definition))
    {
        if (Definition.Type == EInstantRdvCVarUiType::Float)
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
    SetValue(Definition, GetDefaultValue(Definition));
}

static void TriggerAction(const FInstantRdvCVarDefinition& Definition)
{
    // Action CVarはRender側で1フレームのパルスとして消費されるため、
    // 有効状態を残さず、次のGameThread tickで0へ戻す。
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
    TArray<FInstantRdvCVarDefinition> Definitions;
    TSet<FString> RegisteredNames;

    // 既知のメタデータから表示名、範囲、カテゴリ、順序を得る。
    // 現在値・既定値・ヘルプ文は常にCVar本体から取得する。
    for (const FInstantRdvCVarUiMetadata& Metadata : FInstantRdvCVarUiRegistry::GetAll())
    {
        FInstantRdvCVarDefinition Definition = MakeDefinition(Metadata);
        if (FindCVar(Definition) != nullptr)
        {
            Definitions.Add(MoveTemp(Definition));
            RegisteredNames.Add(Metadata.CVarName);
        }
        else
        {
            ensureMsgf(false, TEXT("Instant-RDV Slate metadata refers to an unregistered CVar: %s"), Metadata.CVarName);
        }
    }

    IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
        FConsoleObjectVisitor::CreateLambda(
            [&Definitions, &RegisteredNames](const TCHAR* Name, IConsoleObject* Object)
            {
                // Prefix列挙により、専用メタデータが未登録の新規CVarも検出する。
                // 未登録項目は保守的な汎用範囲と最後尾の順序で表示する。
                if (Object == nullptr || Object->AsVariable() == nullptr || RegisteredNames.Contains(Name))
                {
                    return;
                }

                FInstantRdvCVarDefinition Definition;
                Definition.CVarName = Name;
                const FString NameString(Name);
                int32 LastDotIndex = INDEX_NONE;
                Definition.Label = NameString;
                if (NameString.FindLastChar(TEXT('.'), LastDotIndex))
                {
                    Definition.Label = NameString.RightChop(LastDotIndex + 1);
                }
                Definition.CategoryId = TEXT("Unclassified");
                Definition.Category = TEXT("Unclassified");
                Definition.CategoryOrder = MAX_int32;
                Definition.MinValue = -100.0f;
                Definition.MaxValue = 100.0f;

                if (Object->IsVariableBool())
                {
                    Definition.Type = EInstantRdvCVarUiType::Bool;
                    Definition.MinValue = 0.0f;
                    Definition.MaxValue = 1.0f;
                }
                else if (Object->IsVariableFloat())
                {
                    Definition.Type = EInstantRdvCVarUiType::Float;
                }
                else if (!Object->IsVariableInt())
                {
                    return;
                }

                Definitions.Add(MoveTemp(Definition));
            }),
        TEXT("r.InstantRdv."));

    // 特にTranslation Unitをまたぐ静的登録順序はUIの契約にできない。
    // 明示したカテゴリ順と項目順で表示順を安定させる。
    Definitions.StableSort(
        [](const FInstantRdvCVarDefinition& A, const FInstantRdvCVarDefinition& B)
        {
            if (A.CategoryOrder != B.CategoryOrder)
            {
                return A.CategoryOrder < B.CategoryOrder;
            }
            if (A.ItemOrder != B.ItemOrder)
            {
                return A.ItemOrder < B.ItemOrder;
            }
            return A.CVarName < B.CVarName;
        });

    TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
    FString LastCategory;

    for (const FInstantRdvCVarDefinition& Definition : Definitions)
    {
        if (LastCategory != Definition.Category)
        {
            LastCategory = Definition.Category;
            Content->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 8.0f, 0.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Definition.Category))
                .ToolTipText_Lambda([Definition]()
                {
                    if (const FInstantRdvCVarUiCategory* Category = FInstantRdvCVarUiRegistry::FindCategory(*Definition.CategoryId))
                    {
                        return FText::FromString(Category->Help);
                    }
                    return FText::FromString(TEXT("Unclassified Instant-RDV settings."));
                })
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
            ];
        }

        if (Definition.Type == EInstantRdvCVarUiType::Action)
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
                SNew(SHorizontalBox)
                .Visibility_Lambda([bShowHelp]()
                {
                    return *bShowHelp ? EVisibility::Visible : EVisibility::Collapsed;
                })
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([Definition]()
                    {
                        return GetHelpText(Definition);
                    })
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Top)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Copy")))
                    .ToolTipText(FText::FromString(TEXT("Copy CVar name")))
                    .OnClicked_Lambda([Definition]()
                    {
                        return CopyCVarName(Definition);
                    })
                ]
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

        if (Definition.Type == EInstantRdvCVarUiType::Bool)
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
            // ドラッグ中の即時フィードバックにはPreventThrottlingが必要。
            // 未指定の場合、Slateがマウスキャプチャ終了まで描画更新を遅延させる場合がある。
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
            SNew(SHorizontalBox)
            .Visibility_Lambda([bShowHelp]()
            {
                return *bShowHelp ? EVisibility::Visible : EVisibility::Collapsed;
            })
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([Definition]()
                {
                    return GetHelpText(Definition);
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Top)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Copy")))
                .ToolTipText(FText::FromString(TEXT("Copy CVar name")))
                .OnClicked_Lambda([Definition]()
                {
                    return CopyCVarName(Definition);
                })
            ]
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
    for (const FInstantRdvCVarUiMetadata& Metadata : FInstantRdvCVarUiRegistry::GetAll())
    {
        if (Metadata.Type != EInstantRdvCVarUiType::Action)
        {
            ResetValue(MakeDefinition(Metadata));
        }
    }

    return FReply::Handled();
}
