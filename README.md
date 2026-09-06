# instant-rdv-ue

**Instant-RDV (Instant Raster Derived Voxel scene)** の UE5.8 相当向け CodePlugin 雛形です。  
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
- `r.InstantRdv.UseReducedSurfaceBuffer`（既定値1）でReducedSurfaceBuffer経路とLegacy full-resolution Depth経路を切り替え可能
- Main View の DepthBuffer を入力に BBV更新（Begin / BeginView / Injection / FrustumCull / Carving / BrickCountAggregate）を実行
- `r.InstantRdv.Bbv.VisDebug` で BBVボクセル系デバッグ可視化（0:OFF, 1:レントゲン, 2:Brickレイトレ, 3:Voxelレイトレ, 4:Voxel radiance）
- `r.InstantRdv.Fsp.VisProbe` で ActiveProbe デバッグ可視化（0:OFF, 1:生存状態, 2:インデックスhash, 3:age, 4:cascade, 5:OctMap radiance, 6:OctMap sky visibility, 7:SH radiance, 8:SH sky visibility, 9:BBV埋まり判定）
- `r.InstantRdv.Fsp.VisIvProbe` で IrradianceVolume プローブ可視化（0:OFF, 1:SH radiance, 2:SH sky visibility）
- `r.InstantRdv.Fsp.TraceUseProbeOffset` で FSP ray trace の始点に Probe offset を適用するか切り替え可能
- `r.InstantRdv.Fsp.VisProbeUseOffset` で ActiveProbe デバッグ球の表示位置に Probe offset を適用するか切り替え可能
- ActiveProbeListは他のFSP counter bufferと異なり、word 0/1を世代交代counter、word 2以降をProbe indexとして使用する。これはGPUフレーム重複時のreset/append競合を避けるための専用レイアウトである
- FSP は InstantRDV 参照実装の ActiveProbe lifecycle（visible surface収集、probe pool/free stack、ray request/trace/resolve、OctMap、SH更新、IrradianceVolume伝播）をUE RDG passへ移植中
- 既存パイプラインへの影響を最小化した初期実装

## 初期パラメータ（native InstantRdv 準拠 + UE単位換算）

- BBV: `64 x 64 x 64`
- BBV voxel size: `300 cm`（native 3m 相当）
- Probe: `32 x 32 x 32`
- Probe cell size: `200 cm`（native 2m 相当）
- Probe cascade count: `5`
