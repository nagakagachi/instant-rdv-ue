/*
    InstantRdvConsoleVariables.h

    Instant-RDVのCVar登録とSlateデバッグUIメタデータを結び付ける公開定義。
    CVarの既定値・ヘルプ・実値はCVar本体を正本とし、カテゴリ、表示順、
    Widget種別、スライダ範囲だけをUIメタデータとして管理する。

    CVar/UIメタデータのルール:
    - CVar登録を名前、既定値、ヘルプ、フラグ、実値の正本とする。
    - カテゴリは独立登録する。CVarメタデータにはカテゴリID、表示名、
      カテゴリ内順序、Widget種別、スライダ範囲だけを保持する。
    - 登録マクロのCVar名はシェーダ／C++側で使用する名前と完全に一致させる。
      UI範囲などをCVar名へ埋め込まない。
    - カテゴリIDは安定した識別子とし、表示名と全体順序はカテゴリ登録側で管理する。
    - BoolとActionの範囲は0..1とする。Intは範囲内を整数へ丸めて書き込み、
      Floatは整数化せず連続値として書き込む。
    - Action CVarは一度だけ発火する操作項目とする。UIは1を書き込み、
      次のGameThread tickerで0へ戻す。

    CVar登録例:
      INSTANT_RDV_CVAR_FLOAT(
          CVarInstantRdvFspRelocationOffsetScale,
          TEXT("r.InstantRdv.Fsp.RelocationOffsetScale"),
          0.9f,
          TEXT("ActiveProbe relocation scale."),
          ECVF_RenderThreadSafe,
          TEXT("FSP"),
          TEXT("Relocation offset scale"),
          0.0f,
          1.5f,
          20);

    カテゴリ登録例:
      INSTANT_RDV_CATEGORY(
          Fsp,
          TEXT("FSP"),
          TEXT("FSP"),
          TEXT("FSP and ActiveProbe settings."),
          200);

    CVar追加手順:
    1. 所有するRuntimeソースで、下記の型別登録マクロを使用する。
    2. 既存のカテゴリIDを指定し、カテゴリ内で一意な項目順を設定する。
    3. UI操作で安全に扱える保守的な範囲を設定する。
    4. Slate側へ既定値やヘルプ文を重複記述しない。
    5. カテゴリが存在しない場合、パネルはUnclassifiedへ配置し、
       開発ビルドでensureを発生させて登録漏れを知らせる。
*/

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

enum class EInstantRdvCVarUiType : uint8
{
    Bool,
    Int,
    Float,
    Action
};

struct INSTANTRDV_API FInstantRdvCVarUiMetadata
{
    const TCHAR* CVarName = TEXT("");
    const TCHAR* CategoryId = TEXT("");
    const TCHAR* Label = TEXT("");
    EInstantRdvCVarUiType Type = EInstantRdvCVarUiType::Int;
    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    int32 ItemOrder = 0;
};

struct INSTANTRDV_API FInstantRdvCVarUiCategory
{
    const TCHAR* Id = TEXT("");
    const TCHAR* Label = TEXT("");
    const TCHAR* Help = TEXT("");
    int32 SortOrder = 0;
};

class INSTANTRDV_API FInstantRdvCVarUiRegistry final
{
public:
    static void RegisterCategory(const FInstantRdvCVarUiCategory& Category);
    static void Register(const FInstantRdvCVarUiMetadata& Metadata);
    static const TArray<FInstantRdvCVarUiCategory>& GetCategories();
    static const TArray<FInstantRdvCVarUiMetadata>& GetAll();
    static const FInstantRdvCVarUiCategory* FindCategory(const TCHAR* CategoryId);
};

class FInstantRdvCVarUiCategoryRegistrar final
{
public:
    explicit FInstantRdvCVarUiCategoryRegistrar(const FInstantRdvCVarUiCategory& InCategory)
    {
        FInstantRdvCVarUiRegistry::RegisterCategory(InCategory);
    }
};

class FInstantRdvCVarUiRegistrar final
{
public:
    explicit FInstantRdvCVarUiRegistrar(const FInstantRdvCVarUiMetadata& InMetadata)
    {
        FInstantRdvCVarUiRegistry::Register(InMetadata);
    }
};

#define INSTANT_RDV_CATEGORY(Identifier, Id, Label, Help, SortOrder) \
    static FInstantRdvCVarUiCategoryRegistrar Identifier##CategoryRegistrar( \
        { Id, Label, Help, SortOrder })

#define INSTANT_RDV_CVAR_BOOL(Identifier, Name, DefaultValue, Help, Flags, CategoryId, Label, ItemOrder) \
    static TAutoConsoleVariable<int32> Identifier(Name, DefaultValue, Help, Flags); \
    static FInstantRdvCVarUiRegistrar Identifier##UiRegistrar( \
        { Name, CategoryId, Label, EInstantRdvCVarUiType::Bool, 0.0f, 1.0f, ItemOrder })

#define INSTANT_RDV_CVAR_INT(Identifier, Name, DefaultValue, Help, Flags, CategoryId, Label, MinValue, MaxValue, ItemOrder) \
    static TAutoConsoleVariable<int32> Identifier(Name, DefaultValue, Help, Flags); \
    static FInstantRdvCVarUiRegistrar Identifier##UiRegistrar( \
        { Name, CategoryId, Label, EInstantRdvCVarUiType::Int, MinValue, MaxValue, ItemOrder })

#define INSTANT_RDV_CVAR_FLOAT(Identifier, Name, DefaultValue, Help, Flags, CategoryId, Label, MinValue, MaxValue, ItemOrder) \
    static TAutoConsoleVariable<float> Identifier(Name, DefaultValue, Help, Flags); \
    static FInstantRdvCVarUiRegistrar Identifier##UiRegistrar( \
        { Name, CategoryId, Label, EInstantRdvCVarUiType::Float, MinValue, MaxValue, ItemOrder })

#define INSTANT_RDV_CVAR_ACTION(Identifier, Name, DefaultValue, Help, Flags, CategoryId, Label, ItemOrder) \
    static TAutoConsoleVariable<int32> Identifier(Name, DefaultValue, Help, Flags); \
    static FInstantRdvCVarUiRegistrar Identifier##UiRegistrar( \
        { Name, CategoryId, Label, EInstantRdvCVarUiType::Action, 0.0f, 1.0f, ItemOrder })
