/*
    InstantRdvConsoleVariables.cpp

    CVar登録マクロから収集したカテゴリとSlate UIメタデータを保持し、
    重複・空定義・不正範囲を検証するRuntime側レジストリを実装する。
*/

#include "InstantRdvConsoleVariables.h"

namespace
{
TArray<FInstantRdvCVarUiCategory>& GetMutableCategories()
{
    // CVar宣言とレジストリのTranslation Unit間で静的初期化順序に依存しないよう、
    // 関数ローカルの静的領域として保持する。
    static TArray<FInstantRdvCVarUiCategory> Categories;
    return Categories;
}

TArray<FInstantRdvCVarUiMetadata>& GetMutableMetadata()
{
    // 登録はCVar所有側のTranslation Unitにある静的Registrarから行うため、
    // レジストリ本体は遅延初期化する。
    static TArray<FInstantRdvCVarUiMetadata> Metadata;
    return Metadata;
}
}

INSTANT_RDV_CATEGORY(
    Runtime,
    TEXT("Runtime"),
    TEXT("Runtime"),
    TEXT("Instant-RDV global settings."),
    0);

INSTANT_RDV_CATEGORY(
    Bbv,
    TEXT("BBV"),
    TEXT("BBV"),
    TEXT("BBV geometry and radiance settings."),
    100);

INSTANT_RDV_CATEGORY(
    Fsp,
    TEXT("FSP"),
    TEXT("FSP"),
    TEXT("FSP and ActiveProbe settings."),
    200);

INSTANT_RDV_CATEGORY(
    Debug,
    TEXT("Debug"),
    TEXT("Debug"),
    TEXT("Debug visualization settings."),
    300);

INSTANT_RDV_CATEGORY(
    Unclassified,
    TEXT("Unclassified"),
    TEXT("Unclassified"),
    TEXT("Instant-RDV CVars without a valid category assignment."),
    MAX_int32);

void FInstantRdvCVarUiRegistry::RegisterCategory(const FInstantRdvCVarUiCategory& Category)
{
    ensureMsgf(Category.Id != nullptr && Category.Id[0] != TEXT('\0'), TEXT("Instant-RDV category has an empty ID."));
    ensureMsgf(Category.Label != nullptr && Category.Label[0] != TEXT('\0'), TEXT("Instant-RDV category has an empty label."));

    TArray<FInstantRdvCVarUiCategory>& RegisteredCategories = GetMutableCategories();
    for (const FInstantRdvCVarUiCategory& Existing : RegisteredCategories)
    {
        if (FCString::Strcmp(Existing.Id, Category.Id) == 0)
        {
            ensureMsgf(false, TEXT("Duplicate Instant-RDV category: %s"), Category.Id);
            return;
        }
    }

    RegisteredCategories.Add(Category);
}

void FInstantRdvCVarUiRegistry::Register(const FInstantRdvCVarUiMetadata& Metadata)
{
    ensureMsgf(Metadata.CVarName != nullptr && Metadata.CVarName[0] != TEXT('\0'), TEXT("Instant-RDV CVar UI metadata has an empty CVar name."));
    ensureMsgf(Metadata.CategoryId != nullptr && Metadata.CategoryId[0] != TEXT('\0'), TEXT("Instant-RDV CVar UI metadata has an empty category."));
    ensureMsgf(Metadata.Label != nullptr && Metadata.Label[0] != TEXT('\0'), TEXT("Instant-RDV CVar UI metadata has an empty label."));
    ensureMsgf(Metadata.MinValue <= Metadata.MaxValue, TEXT("Invalid Instant-RDV CVar UI range for %s."), Metadata.CVarName);

    TArray<FInstantRdvCVarUiMetadata>& RegisteredMetadata = GetMutableMetadata();
    for (const FInstantRdvCVarUiMetadata& Existing : RegisteredMetadata)
    {
        if (FCString::Strcmp(Existing.CVarName, Metadata.CVarName) == 0)
        {
            ensureMsgf(false, TEXT("Duplicate Instant-RDV CVar UI metadata: %s"), Metadata.CVarName);
            return;
        }
    }

    RegisteredMetadata.Add(Metadata);
}

const TArray<FInstantRdvCVarUiCategory>& FInstantRdvCVarUiRegistry::GetCategories()
{
    return GetMutableCategories();
}

const TArray<FInstantRdvCVarUiMetadata>& FInstantRdvCVarUiRegistry::GetAll()
{
    return GetMutableMetadata();
}

const FInstantRdvCVarUiCategory* FInstantRdvCVarUiRegistry::FindCategory(const TCHAR* CategoryId)
{
    if (CategoryId == nullptr)
    {
        return nullptr;
    }

    for (const FInstantRdvCVarUiCategory& Category : GetMutableCategories())
    {
        if (FCString::Strcmp(Category.Id, CategoryId) == 0)
        {
            return &Category;
        }
    }

    return nullptr;
}
