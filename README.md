# instant-rdv-ue

**Instant-RDV (Instant Raster Derived Voxel scene)** の UE5.7 相当向け CodePlugin 雛形です。  
SceneViewExtension を基盤に、GI 機能を積み上げるための最小構成を提供します。

## 構成

- `InstantRdv.uplugin`: プラグイン定義
- `Source/InstantRdv/`: Runtime モジュール
- `FInstantRdvSceneViewExtension`: レンダリング拡張の基盤クラス

## 現在の実装方針

- SceneViewExtension をエンジン初期化後に安全に登録
- `r.InstantRdv.GI` / `r.InstantRdv.Bbv.Enable` で機能有効化を切り替え可能
- `r.InstantRdv.Bbv.MainViewInjection` / `r.InstantRdv.Bbv.MainViewRemoval` で各BBV更新フェーズを個別に切り替え可能
- `r.InstantRdv.Bbv.MainViewUpdate` で Injection + Removal を同時にON/OFF可能（更新停止確認用）
- Main View の DepthBuffer を入力に BBV更新（Begin / BeginView / Injection / FrustumCull / Carving / BrickCountAggregate）を実行
- `r.InstantRdv.Bbv.DebugMode` で BBVデバッグ可視化（0:OFF, 1:レントゲン, 2:Brickレイトレ, 3:Voxelレイトレ）
- 既存パイプラインへの影響を最小化した初期実装

## 初期パラメータ（native InstantRdv 準拠 + UE単位換算）

- BBV: `64 x 64 x 64`
- BBV voxel size: `300 cm`（native 3m 相当）
- Probe: `32 x 32 x 32`
- Probe cell size: `200 cm`（native 2m 相当）
- Probe cascade count: `5`
