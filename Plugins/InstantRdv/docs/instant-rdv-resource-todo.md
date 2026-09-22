# InstantRDV リソース TODO

根拠となる現状構成は [リソース監査](instant-rdv-resource-audit-2026-09-12.md) に記載する。

## 参照実装との差分

- [ ] UEの`BbvOptionalData`縮小を意図的な仕様として文書化する。
  - D3D12の現行FSP relocationは`to_surface_vector`と`resolved_sample_count`を読まない。
  - `to_surface_vector`はSDF的なBBV情報を保持する検証用であり、現行ReducedSurface relocationの入力ではない。
  - ReducedSurface relocationはVisibleSurface cell/source-texelリストとReducedSurface Textureに依存する。

- [x] UEのReducedSurface relocation失敗時の扱いとsurface clearanceを参照実装へ合わせる。

- [x] UEのReducedSurface normal構築へゼロ長vectorのGuardを追加する。

- [ ] D3D12のwave単位VisibleSurface cell appendをUEへ移植する価値を測定する。
  - 現在のatomicによるdeduplicationは機能上正しいが、candidateごとにglobal atomicを試行する。

- [ ] FSP ProbePoolのstride差を解消または文書化する。
  - D3D12は7個の`uint` field、UEは`Reserved3`を含む8 wordである。

## RDGとresource lifetime

- [ ] FSP traceとIrradianceVolume更新の実行phaseをPostProcessからBBV更新phaseへ移す可否を検証する。
  - SceneDepth、BBV、Material Radiance、ReducedSurface、FSP永続resource、SceneColor依存を明示する。
  - 多View、初回抽出、debug、material samplingを含めてRDG read/write順序を検証する。

- [ ] graph内だけで完結するFSP作業resourceをtransient RDG resource化できるか評価する。
  - 対象候補はVisibleSurfaceリスト、source texelリスト、ProbeRayRequest、ProbeRayResult。
  - CellProbeIndex、ProbePool、FreeStack、Active Probeリスト、ProbeAtlas、IrradianceVolumeは永続resourceとして維持する。

- [ ] 永続resourceのready状態を堅牢化する。
  - `bRenderInitialized`だけで抽出完了を判断せず、pooled texture/bufferの有効性を確認する。
  - 不足するresourceがあれば依存set全体を再構築または無効化する。

- [ ] 解像度・容量変更時のresource set再確保を同期部分で確定する。
  - 対象はFSP grid解像度、Cascade数、Probe容量、VisibleSurface容量、viewport依存resource。
  - RenderThreadでresource構成を変更しない。

## 計測と検証

- [ ] 永続resourceの論理メモリ量とSparse Probeのactive/capacity比を表示するtelemetryを追加する。

- [ ] 1080p、1440p、4KでReducedSurface resize時のメモリpeakを計測する。

- [ ] IrradianceVolume 3D Textureの品質・性能を計測する。
  - hardware Trilinear samplingと旧buffer samplingのGPU時間を比較する。
  - 高輝度Emissive、負の方向SH、長時間propagation、トロイダル移動、Cascade境界、dither切り替えを検証する。
  - Guard更新とCascade/信号境界でのfilter漏れを確認する。

## 完了した改善

- [x] ReducedSurface relocationの参照実装との機能差を修正した。
- [x] BBV Material Radianceのresolved historyをpacked FP16へ変更した。
- [x] IrradianceVolumeをRGBA16F 3D Textureへ変更し、SkyVisibilityとDiffuse Irradiance RGBの4信号を格納した。
- [x] Material Custom NodeとScene Uniform Bufferを3D Texture/Sampler契約へ更新した。
- [x] hardware Trilinear sampling、正側Guard、Voxel Debugの3D Texture表示に対応した。
