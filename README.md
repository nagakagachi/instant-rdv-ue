# InstantRDV for Unreal Engine

InstantRDV は、Raster Rendering Pipeline の成果物から GPU voxel scene を逐次構築・維持する技術デモである。本リポジトリには Unreal Engine 5.8 向けプラグインとサンプルプロジェクトを含む。

InstantRDV は voxel scene 構築の枠組みである。`RdvGI` はその応用として実装した global illumination デモである。

## 用語

- **RDV — Raster Derived Voxel**: Raster Pass の成果物から導出する voxel scene。
- **BBV — Bitmask Brick Voxel**: occupancy を bitmask で保持する brick voxel grid。
- **RdvGI**: InstantRDV の仕組みを利用した real-time global illumination 実装。
- **VSP — Visible Surface Probe**: 可視 surface 上にProbe配置をしてGI計算をする probe system。

## InstantRDV: Raster 成果物を流用した GPU Voxel Scene 構築

一般的な voxelization では、scene geometry の追加 rasterization または CPU 側 scene data の GPU 転送を行う。InstantRDV は main view rendering で生成済みの DepthBuffer を入力として利用する。Depth から world-space surface position を復元し、Compute Shader で BBV を更新する。追加 geometry pass および CPU-side voxel build は不要である。

入力は DepthBuffer に限らない。world-space surface position を復元可能な Raster 成果物は BBV 更新に利用できる。ShadowMap、GBuffer、独自 surface cache は入力候補である。

**図（未作成）: main view の DepthBuffer を ReducedSurfaceBuffer へ圧縮し、world position / surface attribute を復元して BBV の injection、removal、brick radiance update へ渡すデータフロー図。ShadowMap、GBuffer、surface cache などの代替 Raster 成果物も同じ更新段へ接続できることを示す。**

### BBV: Bitmask Brick Voxel

BBV は自動運転分野の Occupancy Grid Map に着想を得た occupancy representation である。空間を brick grid に分割し、brick 内の voxel occupancy を bitmask として保持する。1 voxel を scalar texture element として保持する dense voxel volume と比較して、occupancy の容量とメモリ帯域を抑えつつ、brick 内の geometry resolution を維持する。

UE 実装では、BBV occupancy と brick ごとの補助情報を buffer として保持する。brick 単位の coarse radiance も保持する。RdvGI はこれを scene radiance source として使用する。GBuffer 由来の albedo、normal、material class などを coarse representation へ統合する拡張余地がある。

DepthBuffer から復元した surface は voxel **injection** として occupancy に追加する。視点移動および screen coverage の変化で観測されなくなった領域は **removal** で更新する。BBV は static voxelization ではなく、raster 可視性に追従する real-time voxel scene である。

**図（未作成）: brick grid の一つを拡大し、bitmask による voxel occupancy、brick 単位の coarse radiance、Depth 由来の injection と removal による occupancy の時間変化を示す図。**

### BBV ray tracing

BBV は Compute Shader から voxel ray tracing できる。ray traversal は BBV occupancy を参照して hit surface を検出する。VSP の visibility capture、probe relocation、debug visualization に使用する。hardware ray tracing scene は必要としない。Raster 成果物から構築した scene representation を同一 GPU pipeline 内で query できる。

## RdvGI: Rdv 上で実装される probe based GI

RdvGI は BBV ray tracing の利用例である。Enshrouded の GI および SurfelGI と類似し、visible surface を起点に sparse lighting sample を更新する。Visible Surface Probe (VSP) により indirect lighting を推定する。

VSP は DepthBuffer で観測した surface cell から sparse probe を配置・再利用する。各 probe は BBV ray tracing で octahedral map に radiance と sky visibility を capture し、L1 spherical harmonics へ投影する。投影結果を cascade IrradianceVolume へ伝播・格納する。material evaluation は volume を trilinear sample し、indirect diffuse irradiance と sky visibility IBL を取得する。

probe ray origin は owner surface に埋まらない位置へ relocation する。capture origin の validity は relocation で確保する。このため GI evaluation 時に DDGI のような probe validity weight および visibility-weighted interpolation を使用しない。評価処理は cascade selection、boundary dither、hardware trilinear filtering、SH evaluation で構成される。

**図（未作成）: DepthBuffer 上の visible surface cell から VSP を生成し、BBV ray tracing で octahedral radiance / sky visibility を capture、L1 SH を経て cascade IrradianceVolume を更新するパイプライン図。probe relocation が surface 内部から origin を押し出す様子も示す。**

**図（未作成）: DDGI の volume probe と VSP の比較図。DDGI 側は validity / visibility weighted interpolation、VSP 側は visible surface 起点の placement と relocation により評価時の重み付けを不要にする設計を示す。**

## Unreal Engine integration

**図（未作成）: Unreal Engine の CVar 操作 Slate。`r.InstantRdv.Enable`、`r.InstantRdv.Gi.Enable`、各デバッグ CVar を操作する画面のスクリーンショット。**

**図（未作成）: `AInstantRdvSettingsActor` の Details パネル。InstantRDV 基幹機能と RdvGI のレベル単位パラメータを設定する画面のスクリーンショット。**

`AInstantRdvSettingsActor` はレベルごとの設定を保持する。

- `bEnabled`: InstantRDV 基幹機能を制御する。BBV の構築・更新を含む。
- `bGiEnabled`: RdvGI を制御する。VSP 更新と material GI output を停止する。BBV 更新は継続する。

CVar は対応する最終ゲートである。

- `r.InstantRdv.Enable`: InstantRDV 基幹機能。
- `r.InstantRdv.Gi.Enable`: RdvGI。

両 CVar の既定値は有効である。Setting Actor の両フラグの既定値は無効である。レベルに Actor を配置し、必要な層を明示的に有効化するまで処理は実行されない。

RdvGI デモでは Material Custom Node から `instant_rdv_material.ush` を include し、`InstantRdvMaterialTryEvaluateIndirectLighting` を呼び出す。出力 irradiance `E` は Lambert reflection として `BaseColor * E / PI` を Emissive に加算できる。Sky visibility IBL は SkyLight / IBL contribution 用の無次元係数である。

## 実装上の注記

- BBV の main-view injection / removal、radiance update、VSP update、reduced-surface path は CVar で個別に観察できる。
- VSP の probe pool、active probe list、ray request/result、probe atlas、IrradianceVolume は GPU resource として管理する。
- VSP は GI 無効時も resource allocation を維持する。GI resource の lazy allocation / release は lifecycle synchronization と合わせて最適化対象である。
- MainView 外の BrickRadiance は更新しない。高輝度 brick が画面外へ移動した後も残る。
- `Plugins/InstantRdv/docs/` に resource audit と実装 TODO を記録する。

## 初期パラメータ

| 項目 | 既定値 |
|---|---:|
| BBV grid | `64 x 64 x 64` bricks |
| BBV brick size | `300 cm` |
| VSP grid | `16 x 16 x 16` cells per cascade |
| VSP cell size | `200 cm` |
| VSP cascade count | `5` |
| Sparse probe pool | `8192` probes shared across cascades |
