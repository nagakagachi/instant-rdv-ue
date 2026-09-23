# InstantRDV for Unreal Engine

InstantRDV は、Raster Rendering Pipeline の成果物から GPU voxel scene を逐次構築・維持する技術デモです。本リポジトリには Unreal Engine 5.8 向けプラグインとサンプルプロジェクトを含みます。

InstantRDV は voxel scene を構築する仕組みです。`RdvGI` は InstantRDV を使用する global illumination のデモ実装です。

UE サンプルシーンでの RdvGI の ON/OFF 比較です。

| RdvGI OFF | RdvGI ON |
|---|---|
| ![RdvGI 無効時のサンプルシーン](docs/images/rdvgi_off.png) | ![RdvGI 有効時のサンプルシーン](docs/images/rdvgi_on.png) |

## 用語

- **RDV — Raster Derived Voxel**: Raster Pass の成果物から導出する voxel scene。
- **BBV — Bitmask Brick Voxel**: occupancy を bitmask で保持する brick voxel grid。
- **RdvGI**: InstantRDV の仕組みを利用した real-time global illumination 実装。
- **VSP — Visible Surface Probe**: 可視 surface 上に Probe を配置して GI 計算を行う probe system。

## InstantRDV: Raster 成果物を流用した GPU Voxel Scene 構築

一般的な voxelization では、scene geometry の追加 rasterization または CPU 側 scene data の GPU 転送を行います。InstantRDV は main view rendering で生成済みの DepthBuffer を入力として利用します。Depth から world-space surface position を復元し、Compute Shader で BBV を更新します。追加 geometry pass および CPU-side voxel build は不要です。(でも実装ではMainViewのみです)

入力は MainView の DepthBuffer に限りません。world-space surface position を復元できる Raster 成果物を BBV 更新に使用できます。ShadowMap、GBuffer、独自の surface cache も使用できます。

**図（未作成）: MainView の DepthBuffer を ReducedSurfaceBuffer へ圧縮し、world position / surface attribute を復元して BBV の injection、removal、brick radiance update へ渡すデータフロー図。ShadowMap、GBuffer、surface cache などの代替 Raster 成果物も同じ更新段へ接続できることを示します。**

### BBV: Bitmask Brick Voxel

BBV は自動運転分野の Occupancy Grid Map に類似したVoxel表現です。空間を brick grid に分割し、brick 内の voxel occupancy を bitmask として保持します。1 voxel を scalar texture element として保持する dense voxel volume と比較して、occupancy の容量とメモリ帯域を抑えつつ、brick 内の geometry resolution を維持します。デモ実装では 8x8x8 voxel を 1 brickとして 512bit で表現します。

ジオメトリ情報としての occupancy よりも低周波の情報として、 brick毎の材質等の情報を追加で保持します。デモ実装では scene color 由来の輝度を brick 単位の coarse radiance として格納し、RdvGI のレイトレースのヒット位置の輝度としてサンプリングします。

DepthBuffer から復元した surface は voxel **injection** として occupancy に追加します。視点移動および screen coverage の変化で観測されなくなった領域は **removal** で除去します。BBV はこの逐次更新によってリアルタイムにシーンに追従する voxel scene 表現です。

**図（未作成）: brick grid の一つを拡大し、bitmask による voxel occupancy、brick 単位の coarse radiance、Depth 由来の injection と removal による occupancy の時間変化を示す図です。**

BBV の voxel occupancy と brick radiance のデバッグ表示です。

| Voxel occupancy | Brick radiance |
|---|---|
| ![BBV の voxel occupancy の可視化](docs/images/bbv_visualize_voxel.png) | ![BBV の brick radiance の可視化](docs/images/bbv_visualize_brick_radiance.png) |

### BBV ray tracing

BBV は Compute Shader から voxel ray tracing できます。ray traversal は BBV occupancy を参照して hit surface を検出します。VSP の visibility capture、probe relocation、debug visualization に使用します。hardware ray tracing scene は必要としません。Raster 成果物から構築した scene representation を同一 GPU pipeline 内で query できます。

## RdvGI: Rdv 上で実装される probe based GI

RdvGI は BBV ray tracing を使用する GI 実装です。Enshrouded の GI および SurfelGI と同様に、visible surface を起点に sparse lighting sample を更新します。Visible Surface Probe (VSP) により indirect lighting を推定します。

VSP は DepthBuffer で観測した surface cell から sparse probe を配置・再利用します。各 probe は BBV ray tracing で octahedral map に radiance と sky visibility を capture し、L1 spherical harmonics へ投影します。投影結果を cascade IrradianceVolume へ伝播・格納します。material evaluation は volume を trilinear sample し、indirect diffuse irradiance と sky visibility IBL を取得します。

probe ray origin は、対応する surface に埋まらない位置へ relocation します。capture に使う origin は relocation で確保します。このため GI evaluation 時に DDGI のような probe validity weight および visibility-weighted interpolation を使用しません。評価時には cascade selection、boundary dither、hardware trilinear filtering、SH evaluation を行います。

**図（未作成）: DepthBuffer 上の visible surface cell から VSP を生成し、BBV ray tracing で octahedral radiance / sky visibility を capture、L1 SH を経て cascade IrradianceVolume を更新するパイプライン図。probe relocation が surface 内部から origin を押し出す様子も示します。**

**図（未作成）: DDGI の volume probe と VSP の比較図。DDGI 側は validity / visibility weighted interpolation、VSP 側は visible surface 起点の placement と relocation により評価時の重み付けを不要にする設計を示します。**

ActiveProbe と IrradianceVolume のデバッグ表示です。

| ActiveProbe | IrradianceVolume |
|---|---|
| ![VSP の ActiveProbe の可視化](docs/images/vsp_visualize_active_probe.png) | ![カスケード IrradianceVolume の可視化](docs/images/vsp_visualize_irradiancevolume.png) |

## GPU 更新フロー

InstantRDV は選択した MainView を基準にし、同一 ViewFamily 内で BBV geometry、brick radiance、VSP を各 1 回更新します。主な更新順は次のとおりです。

1. **Toroidal BBV の更新**: camera position に合わせて BBV grid を移動し、新たに入った領域を clear します。geometry update では MainView の DepthBuffer から geometry injection と removal を実行します。injection は Wave Intrinsics で同一 bitmask word を更新する lane を集約し、`WaveActiveBitOr` で統合した bitmask を代表 lane の `InterlockedOr` で書き込みます。pixel ごとの atomic 操作による競合を抑えます。
2. **ReducedSurfaceBuffer の構築**: DepthBuffer を 4×4 タイル単位で時間分散 sampling します。各 sample は view-space depth、octahedral encoding した world normal、normal confidence を保持します。BBV geometry/radiance 更新と VSP は、同一 ViewFamily 内で ReducedSurfaceBuffer または legacy full-resolution Depth path のいずれか一方を共有します。
3. **Brick radiance の更新**: lighting 後、tonemap 前の SceneColor を入力とします。surface sample を brick へ injection し、brick 単位の radiance accumulator を optional data に resolve します。
4. **Visible Surface Probe の更新**: visible surface を VSP cell mask に注入して compact し、probe を持たない cell には sparse probe pool から probe を割り当てます。既存 probe を含め、可視 cell の probe position は surface anchor と BBV による relocation で更新します。active probe 数、ray request 数、ray result 数は counter buffer から indirect dispatch argument を生成します。
5. **Probe capture と volume 更新**: active probe ごとに 6×6 octahedral directions の ray request を生成し、BBV を trace します。hit は brick optional radiance、miss は sky visibility `1` として probe atlas へ temporal blend します。atlas を L1 SH へ積分して owner cell の IrradianceVolume を更新し、probe を持たない cell へは checkerboard propagation を行います。

このフローでは、VSP の probe/ray 処理量は grid の全 cell や全 ray 数ではなく、可視 surface から生成した active probe と ray request の数で決まります。

## Unreal Engine integration

### Debug menu

`Tools > Debug > Instant-RDV > Instant-RDV Debug` からデバッグメニューを開きます。メニューは Runtime、BBV、VSP、Debug の4区分です。Runtime では InstantRDV、RdvGI、ReducedSurfaceBuffer の有効・無効を切り替えます。BBV と VSP では各更新処理と評価方式を確認できます。Debug では BBV、ActiveProbe、IrradianceVolume を可視化できます。

![Instant-RDV Debug メニューの場所](docs/images/debugmenu_location.png)

![Instant-RDV Debug の項目レイアウト](docs/images/debugmenu_layout.png)

### Level Settings Actor

`InstantRdvSettingsActor` は `Plugins > Instant-RDV > InstantRdv > Public` にある Actor class です。この Actor をレベルへ配置し、Details パネルでレベルごとの設定を変更します。`Enabled` は BBV を含む InstantRDV の有効・無効を切り替えます。`Gi Enabled` は VSP 更新とマテリアルからの GI 出力を切り替えます。BBV の更新は `Gi Enabled` の影響を受けません。BBV、VSP、Rendering、LOD、Relocation にはレベルごとのパラメータがあります。

![InstantRdvSettingsActor の配置元](docs/images/settingsactor_location.png)

![InstantRdvSettingsActor の主要パラメータ](docs/images/settingsactor_param.png)

CVar では各機能を実行時に無効化できます。

- `r.InstantRdv.Enable`: InstantRDV 基幹機能。
- `r.InstantRdv.Gi.Enable`: RdvGI。

両 CVar の既定値は有効です。Settings Actor の `Enabled` と `Gi Enabled` の既定値は無効です。レベルに Actor を配置して必要な機能を有効化するまで処理は実行されません。

### Material Function

`MF_Irdv_SampleGi` はプラグインの Content に含まれる Material Function です。Material Graph から呼び出し、`InSamplePosition` に Absolute World Position、`InSampleNormal` に VertexNormalWS を接続します。内部の Custom Node は `instant_rdv_material.ush` を include し、`InstantRdvMaterialTryEvaluateIndirectLighting` を呼び出します。

出力 `Irradiance` は入射照度 `E` です。`Irradiance/PI` は `E / PI` です。Lambert diffuse として Emissive へ加算する場合は、Base Color と `Irradiance/PI` を乗算します。`SkyVisibility` は SkyLight / IBL 用の遮蔽係数です。

![MF_Irdv_SampleGi の配置場所](docs/images/customnode_content_mf.png)

![MF_Irdv_SampleGi を使用したマテリアルグラフ](docs/images/customnode_sampling_gi.png)

## 実装上の注記

- BBV の main-view injection / removal、radiance update、VSP update、reduced-surface path は CVar で個別に観察できます。
- VSP の probe pool、active probe list、ray request/result、probe atlas、IrradianceVolume は GPU resource として管理します。
- VSP は GI を無効にしても GPU resource を確保したままです。
- MainView 外の BrickRadiance は更新しません。高輝度 brick が画面外へ移動した後も残ります。

## 初期パラメータ

| 項目 | 既定値 |
|---|---:|
| BBV grid | `64 x 64 x 64` bricks |
| BBV brick size | `300 cm` |
| VSP grid | `16 x 16 x 16` cells per cascade |
| VSP cell size | `200 cm` |
| VSP cascade count | `5` |
| Sparse probe pool | `8192` probes shared across cascades |
