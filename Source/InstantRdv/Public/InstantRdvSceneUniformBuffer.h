/*
    InstantRdvSceneUniformBuffer.h

    RDVのパラメータやバッファをSceneUniformBufferに追加してマテリアル等からアクセス可能にする.

*/
#pragma once
#include "CoreMinimal.h"
#include "ShaderParameterMacros.h"
#include "SceneUniformBuffer.h"

/*
    Material CustomNode内で

        Scene.InstantRdvParam.TestColor;

    のような形でSceneUniformBufferメンバとしてアクセス可能になる.
*/
BEGIN_SHADER_PARAMETER_STRUCT(FInstantRdvSceneUniformBufferParams, )
    SHADER_PARAMETER(FVector3f, TestColor)
END_SHADER_PARAMETER_STRUCT()

DECLARE_SCENE_UB_STRUCT(FInstantRdvSceneUniformBufferParams, InstantRdvParam, )