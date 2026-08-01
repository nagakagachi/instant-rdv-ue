/*
    InstantRdvSceneUniformBuffer.cpp

    RDVのパラメータやバッファをSceneUniformBufferに追加してマテリアル等からアクセス可能にする.
*/

#include "InstantRdvSceneUniformBuffer.h"

// SceneUniformBufferの拡張部のデフォルト値定義用関数.
static void GetDefaultResourceParameters_FInstantRdvSceneUniformBufferParams(FInstantRdvSceneUniformBufferParams& ShaderParams, FRDGBuilder& GraphBuilder)
{
    ShaderParams.TestColor = FVector3f(0.0f, 0.0f, 1.0f);
}

// SceneUniformBufferのメンバ登録.
IMPLEMENT_SCENE_UB_STRUCT(FInstantRdvSceneUniformBufferParams, InstantRdvParam, GetDefaultResourceParameters_FInstantRdvSceneUniformBufferParams);