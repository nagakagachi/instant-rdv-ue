# InstantRDV リソース TODO

根拠となる現状構成は [リソース監査](instant-rdv-resource-audit-2026-09-12.md) に記載する。

## 参照実装との差分

- [x] UEのReducedSurface relocation失敗時の扱いとsurface clearanceを参照実装へ合わせる。

- [x] UEのReducedSurface normal構築へゼロ長vectorのGuardを追加する。

- [ ] D3D12のwave単位VisibleSurface cell appendをUEへ移植する価値を測定する。
  - 通常Depth経路は32-bit surface mask wordごとに1回の`InterlockedAdd`でcompactしているが、wave単位appendではない。
  - ReducedSurface経路は現在もcandidate texelごとにglobal atomic appendを試行する。
  - D3D12参照実装とのGPU時間、atomic競合、可視cell数を比較して移植可否を決める。

- [ ] FSP ProbePoolのstride差を解消または文書化する。
  - UEは`Reserved3`を含む8 word、D3D12は7個の`uint` fieldである。
  - `Reserved3`の将来用途を確定するか削除し、GPU buffer stride・リソース監査・参照実装との差分を一致させる。

## 画面外BBV BrickのRadiance寿命

- [ ] 画面外となったBBV BrickのMaterial Radianceが、ライト・ジオメトリ・露出などのシーン変化後も残留してGIへ過剰に寄与する問題を対処する。
  - 最小対処として、画面外 Brick のRadianceを時間に応じてゼロへ減衰させる。
  - 次段階として、可視 Brick のRadiance統計を参照し、残留 Brick をもっともらしい輝度スケールまで弱める。
  - 発展案として、参照実装RDVと同様にCascade ShadowまたはVirtual Shadow Mapを参照し、Brickを擬似LambertシェーディングしたRadianceへ遷移させる。
  - ライト・遮蔽・emissive変化、カメラ移動、長時間画面外、再可視化を含む比較シーンで残留量とGPUコストを計測する。

## RDGとresource lifetime

- [ ] FSP traceとIrradianceVolume更新の実行phaseをPostProcessからBBV更新phaseへ移す可否を検証する。
  - SceneDepth、BBV、Material Radiance、ReducedSurface、FSP永続resource、SceneColor依存を明示する。
  - 多View、初回抽出、debug、material samplingを含めてRDG read/write順序を検証する。

- [ ] graph内だけで完結するFSP作業resourceをtransient RDG resource化できるか評価する。
  - 対象候補はVisibleSurfaceリスト、source texelリスト、ProbeRayRequest、ProbeRayResult。
  - CellProbeIndex、ProbePool、FreeStack、Active Probeリスト、ProbeAtlas、IrradianceVolumeは永続resourceとして維持する。

- [ ] 永続resourceのready状態を堅牢化する。
  - ReducedSurfaceBufferはpooled textureの有効性とextentを確認してから再登録している。
  - BBV/FSPのbuffer・ProbeAtlas・IrradianceVolumeは、現在も`bRenderInitialized`を前提に直接`RegisterExternal*`している。
  - 各pooled texture/bufferの有効性を確認し、不足時は依存resource set全体を再構築または無効化する。

- [ ] 解像度・容量変更時のresource set再確保を同期部分で確定する。
  - Setting Actorのrevision検知により、FSP grid解像度、Cascade数、Probe容量、VisibleSurface容量を変更してBBV/FSPを再生成できる。
  - 現在の再生成は`TryAcceptViewFamilyForRdv_RenderThread`で行われる。同期部分で構成変更を確定し、RenderThreadでは確定済みresource setを安全に切り替える構造へ移す。
  - viewport依存のReducedSurface再確保、旧resource setの解放、複数Sceneの切替も同じ寿命契約で検証する。

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
