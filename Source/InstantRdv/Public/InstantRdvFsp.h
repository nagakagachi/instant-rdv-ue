#pragma once

#include "CoreMinimal.h"

// FSP (Frustum Space Probe) 管理クラスのスケルトン
// 日本語コメント: このクラスはFSPのリソース管理、サーフェス収集、Probeライフサイクル
// およびラジアンス転写の高レベル制御を担う。詳細実装はInstantRdvFsp.cppに記述する。

class FRDGBuilder; // 前方宣言: 実装側で正しいヘッダをincludeする

class FInstantRdvFsp
{
public:
    FInstantRdvFsp();
    ~FInstantRdvFsp();

    // 初期化/解放 (Editor/Device 初期化タイミングで呼ぶ)
    void Initialize();
    void Release();

    // フレーム毎の更新: RDG スコープ内から呼ぶことを想定
    void Tick(FRDGBuilder& GraphBuilder);

private:
    // TODO: Probe pool, free stack, visible lists, atlas などのメンバを追加
};
