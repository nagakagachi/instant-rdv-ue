# InstantRDV UE リソース監査（2026-09-18）

対応予定は [リソース TODO](instant-rdv-resource-todo.md) に記載する。

## 対象と算定方法

本書はUEプラグインのBBV、FSP、ReducedSurfaceを対象とし、現時点のD3D12参照実装と比較する。数値はリソース記述子から求めた論理ペイロード量である。D3D12の配置アラインメント、RDGプールの保持、アロケータ粒度、解像度変更時に旧リソースと新リソースが同時に生存する期間は含まない。

既定値は以下の通り。

- BBVグリッド: `64 x 64 x 64` brick、brick内解像度 `8 x 8 x 8`。
- FSPグリッド: Cascadeごとに `16 x 16 x 16` cell、5 Cascade。
- Sparse Probe容量: 全Cascade共有で8,192。
- 可視Surfaceリスト容量: 4,096。
- Probe Atlas: `6 x 6` octahedral map。
- IrradianceVolume: SkyVisibility SHとDiffuse Irradiance RGB SHを、4個のRGBA16F信号として格納する。

## 永続リソース

### BBV

| リソース | 構成 | 論理サイズ | 用途 |
|---|---|---:|---|
| `BbvBuffer` | `StructuredBuffer<uint>`、brickごと20 word | 20.000 MiB | occupancy bitmask、count、局所AABB。 |
| `BbvOptionalDataBuffer` | `StructuredBuffer<uint>`、brickごと4 word | 4.000 MiB | occupancy関連情報と解決済みRadiance RGB。 |
| `BbvRadianceAccumBuffer` | `StructuredBuffer<uint>`、brickごと4 word | 4.000 MiB | 固定小数点のRadiance累積値とサンプル数。 |

BBV永続リソース合計は28.000 MiBである。`to_surface_vector`はSDF的なBBV情報の検証用であり、現行ReducedSurface Probe Relocationでは使用しない。

### FSP

| リソース | 構成 | 論理サイズ | 用途 |
|---|---|---:|---|
| `FspCellProbeIndexBuffer` | 20,480 × `uint` | 0.078 MiB | denseなcellからSparse Probeへの対応表。 |
| `FspVisibleSurfaceListBuffer` | 4,097 × `uint` | 0.016 MiB | 可視Surface cellの作業リスト。 |
| `FspVisibleSurfaceSourceTexelListBuffer` | 4,097 × `uint` | 0.016 MiB | 可視Surfaceリストと対になるsource texel。 |
| `FspProbePoolBuffer` | 8,192 × 8 × `uint` | 0.250 MiB | Sparse Probeの状態。 |
| `FspProbeFreeStackBuffer` | 8,193 × `uint` | 0.031 MiB | Sparse Probe allocator。 |
| `FspActiveProbeList0/1` | 2 × 8,194 × `uint` | 0.063 MiB | 世代を交互に使うActive Probeリスト。 |
| `FspProbeAtlas` | `768 x 384`、RGBA16F | 2.250 MiB | ProbeごとのRadiance RGBとSkyVisibility A。 |
| `FspProbeRayRequestBuffer` | 294,913 × `uint` | 1.125 MiB | ray requestの作業領域。 |
| `FspProbeRayResultBuffer` | 589,825 × `uint` | 2.250 MiB | ray resultの作業領域。 |
| `FspIrradianceVolumeSHTexture` | `17 x 17 x 340`、RGBA16F | 0.750 MiB | 5 Cascade × 4信号をZ方向に連結したGI用3D Texture。 |

IrradianceVolumeは、各Cascadeで`16 x 16 x 16`の物理cellと正側1 texelのGuardを持つ。信号ごとの33 sliceを連結し、信号順はSkyVisibility、Irradiance R、G、Bである。Guardはトロイダル物理座標0を複製し、Linear Clampフィルタが物理境界やCascade境界を越えて混合しないようにする。

FSP永続リソース合計は約6.828 MiB、BBVとFSPの合計は約34.828 MiBである。作業用リストとray bufferは現在はプール保持されるが、グラフ内だけで完結できるかをTODOで検討する。

### ReducedSurface

`ReducedSurfaceBuffer`は`PF_A32B32G32R32F`のquarter-resolution Textureである。Geometry RDG graphで生成して抽出し、後続graphへ外部Textureとして登録する。View解像度の変更時に再確保する。

| View解像度 | ReducedSurface解像度 | 論理サイズ | BBV + FSP + ReducedSurface |
|---:|---:|---:|---:|
| 1920 × 1080 | 480 × 270 | 1.978 MiB | 36.806 MiB |
| 2560 × 1440 | 640 × 360 | 3.516 MiB | 38.344 MiB |
| 3840 × 2160 | 960 × 540 | 7.910 MiB | 42.738 MiB |

## RDGと寿命

永続リソースは次の手順で管理する。

1. 初回graphでRDGリソースを生成し、clearまたは更新後に抽出を予約する。
2. graph完了後もpooled resourceを保持する。
3. 後続graphではpooled resourceを外部リソースとして登録する。
4. 各passのSRV/UAV用途をRDG parameterで宣言し、状態遷移とbarrierはRDGに任せる。

`ReducedSurfaceBuffer`とIrradianceVolumeはTextureとしてRDG用途を記録する。前者は複数phaseで利用されるためRDGが状態遷移を管理する。後者はRDGへ外部Textureとして登録し、FSP passおよびMaterial Uniform Bufferで使用する。

## 実装上の確認事項

- IrradianceVolumeのmaterial samplingは、従来の8 corner × 4信号の明示loadと補間を、4回のhardware Trilinear sampleへ置き換えた。
- 書き込み時にRadiance RGB SHへLambertのclamped-cosine畳み込みを適用する。通常のGI評価は格納済みDiffuse Irradianceを直接評価する。
- デバッグのRadiance表示だけは逆畳み込みして、従来のRadiance SH表示契約を維持する。
- `PF_FloatRGBA`はRGBA16Fであり、永続IrradianceVolumeの論理サイズは旧FP32 `float4` bufferの10 MiBより小さい。一方、guardを含むため密なFP16 bufferよりは増える。
- Resizeや設定変更の判定・resource setの再確保は、RenderThreadより前の同期部分で確定する方針とする。RenderThreadではRDG handleの生成、temporary constant buffer、pass parameterの設定だけを行う。
