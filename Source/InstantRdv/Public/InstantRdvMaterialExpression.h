/*
    InstantRdvMaterialExpression.h


*/
#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpressionExternalCodeBase.h"
#include "InstantRdvMaterialExpression.generated.h"

// RDV専用MaterialExpressionの最小テスト実装。
// まずは SceneUniformBuffer 拡張 InstantRdvParam へ到達できることだけを確認するため、
// Material から TestColor を返す専用nodeを用意する。
// 将来的に IrradianceVolume / ProbeAtlas / その他RDV resource参照へ拡張する場合も、
// 「MaterialExpression側は用途別node、実データ参照は .ush helper」という分担を維持する。
UCLASS(collapsecategories, hidecategories = Object)
class INSTANTRDV_API UMaterialExpressionInstantRdvTestParam : public UMaterialExpressionExternalCodeBase
{
    GENERATED_BODY()

public:
    UMaterialExpressionInstantRdvTestParam(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITOR
    virtual void GetCaption(TArray<FString>& OutCaptions) const override;
    virtual FText GetCreationName() const override;
    virtual void GetIncludeFilePaths(TSet<FString>& OutIncludeFilePaths) const override;
#endif
};
