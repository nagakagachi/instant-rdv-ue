/*
    InstantRdvMaterialExpression.cpp

*/
#include "InstantRdvMaterialExpression.h"

#define LOCTEXT_NAMESPACE "InstantRdvMaterialExpression"

UMaterialExpressionInstantRdvTestParam::UMaterialExpressionInstantRdvTestParam(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ExternalCodeIdentifiers = { TEXT("InstantRdvTestColor") };

#if WITH_EDITORONLY_DATA
    MenuCategories.Add(LOCTEXT("MenuCategory", "Instant RDV"));
    bCollapsed = false;
#endif
}

#if WITH_EDITOR
void UMaterialExpressionInstantRdvTestParam::GetCaption(TArray<FString>& OutCaptions) const
{
    OutCaptions.Add(TEXT("Instant RDV Test Param"));
}

FText UMaterialExpressionInstantRdvTestParam::GetCreationName() const
{
    return LOCTEXT("CreationName", "Instant RDV Test Param");
}

void UMaterialExpressionInstantRdvTestParam::GetIncludeFilePaths(TSet<FString>& OutIncludeFilePaths) const
{
    // ExternalCode declaration is currently inlined from MaterialExpressions.ini because that path is the
    // most reliable way to prove SceneUB access from a plugin node. Keep the helper include registered so
    // future RDV material nodes can move shared sampling logic into .ush without changing the node API.
    OutIncludeFilePaths.Add(TEXT("/InstantRdvShaders/Private/Expression/InstantRdvMaterialExpression.ush"));
}
#endif

#undef LOCTEXT_NAMESPACE
