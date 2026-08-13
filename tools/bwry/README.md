# Note4C B/W/R/Y 图片转换工具

把普通照片转成 Note4C 面板需要的 **400×300、2bpp、30000 字节** framebuffer。

设备颜色编码和 SSD2683 刷新逻辑完全不变，本工具只负责「原图 → B/W/R/Y framebuffer」这一段：

```
black=0  white=1  yellow=2  red=3     4 像素 / 字节，MSB 在前
```

输出可以直接喂给 `/upload?format=bwry2bpp`（见 `docs/LAN_PHOTO_PUSH_API.md`）。

## 安装

```bash
python3 -m venv .venv-imgtool
.venv-imgtool/bin/pip install -r tools/bwry/requirements.txt
```

## 快速开始

```bash
.venv-imgtool/bin/python tools/bwry/bwryctl.py convert tmp/Cubes.jpg out.bin --preset photo --preview out.png
```

跑一遍 A/B 矩阵并生成对比页：

```bash
.venv-imgtool/bin/python tools/bwry/bwryctl.py ab tmp/Cubes.jpg tmp/NEXTSTEP头像-s.png --out tmp/ab
```

然后打开 `tmp/ab/index.html`。

自检（含与现有 JS 转换器的逐字节一致性校验）：

```bash
.venv-imgtool/bin/python tools/bwry/selftest.py
```

## 处理链路

顺序是有原因的，不能随便调换：

| 阶段 | 做什么 | 为什么在这个位置 |
| --- | --- | --- |
| 1 解码 / 适配 | EXIF 校正，缩放到 400×300，白底 letterbox | 与固件网页保持一致 |
| 2 sRGB → Lab | 线性化 → XYZ → CIE Lab | 之后所有判断都在感知空间 |
| 3 色调曲线 | autocontrast、曝光、S 曲线、暗部提升、高光压缩 | 单调 LUT，任何参数组合都不会出现色阶断裂 |
| 4 局部对比度 | L\* 上的 unsharp（两级：大半径 + 细节） | 面板 L\* 动态范围只有 sRGB 的一半左右，全局对比度必须让步，细节靠局部对比度找回来 |
| 5 映射到面板 L\* 区间 | 0..100 → profile 的 black..white | 黑点补偿，把源图铺满可用范围 |
| 6 色域压缩 | 投影进四种墨水在 XYZ 中张成的四面体 | **必须在抖色之前**：否则饱和的蓝天把无法表达的残差丢进误差缓冲，最后变成满屏红黄噪点 |
| 7 Chroma gate | 近中性区域彻底压回黑白轴 | 灰墙、阴影、云、肤色暗部不再有资格买彩色墨水 |
| 8 半色调 | 误差扩散或有序蓝噪声，带边缘保护 | |
| 9 打包 | 2bpp / 30000 字节 | 设备编码原样不动 |

### 两个容易踩的坑

**误差扩散必须在线性光里做。** 半色调在视觉上是按*反射率线性*平均的，不是按 L\* 平均。
在 Lab 里扩散误差看起来很「感知正确」，实际结果是每一块平坦区域都墨量不足、整张图发灰发白。
默认 `error_space="linear"`（XYZ）；`"lab"` 保留下来是为了能把这个失败模式摆到面板上并排看
（A/B 矩阵里的 `01-cal-lab-fs` vs `02-linear-error`）。

选色仍然是 Lab 里的 ΔE76 —— 这才是防止偏蓝阴影被判给红墨水的东西。

**中性区的彩色残差要一起掐掉。** 没有一种墨水是绝对中性的（实测黑通常偏蓝一点点），
所以一长串黑白半色调会慢慢攒下一个反方向的彩色残差，攒够了就由某个像素以一颗红点或黄点的形式还清 ——
这就是灰墙上彩噪的来源。光靠 chroma gate 拦住*选色*没用，还必须让 gate 关闭的地方**不再传递彩色残差**
（`gate_error_floor`，默认 0）。实测这一步让中性区彩噪率从 0.30% 降到 0.01%，差 30 倍。

## Palette profile

profile 描述的是四种墨水在**真实面板上**长什么样，用**媒体相对**（media-relative）sRGB 表示：
面板自己的纸白按通道归一到中性 255。

这么做是有意的 —— 它把环境光的色偏除掉了（手机拍照最大的误差来源），
而丢掉的只是面板本来也用不上的信息：纸白之上没有更白的参考色可以用来还原纸张色偏。

```bash
.venv-imgtool/bin/python tools/bwry/bwryctl.py profiles
```

内置：

- `note4c-ideal` —— 纯 `#000/#FFF/#FF0000/#FFFF00`。这是现在固件网页和 `docs/inkscreen_image_converter.js` 的假设，
  留作 A/B 基线。**这不是面板的样子。**
- `note4c-estimate-v1` —— 按 E Ink Spectra BWRY 常见反射率推出来的起始估计值。
  **这是估计，不是实测**，请用下面的流程换掉它。

## 实测标定

```bash
# 1. 生成色卡并推到设备
.venv-imgtool/bin/python tools/bwry/bwryctl.py chart --out chart.bin --push http://<设备IP>

# 2. 在网页上点这张图的「展示」，让它全屏显示，然后拍照（要求见下）

# 3. 提取
.venv-imgtool/bin/python tools/bwry/bwryctl.py calibrate chart.jpg --out my_profile.json
```

### 必须走网页「展示」全屏

设备上有两条显示路径，只有一条能用来标定：

| 路径 | 行为 |
| --- | --- |
| 网页「展示」全屏（`photo_gallery.cc`） | 400×300 逐像素 1:1 拷贝，**标定用这条** |
| 设备上按键翻看详情（`photo_detail_renderer.cc`） | 缩放到 372×252，最近邻重采样 |

详情页那条会用最近邻把图缩放，半色调块的实际覆盖率会被改掉，灰阶就不再是准确的
25/50/75%，相机响应也就没法测了。纯色块不受缩放影响，但整套流程依赖灰阶。

### 拍照要求

**必须做到的（做不到标定结果不可用）：**

- **关掉所有画质增强**：HDR / Smart HDR、鲜艳模式、场景优化、夜景、人像。
  手机默认的成片是「好看」不是「准确」，它会给饱和色加饱和度，把黄色的蓝通道压到 0 ——
  信息一旦被压没就再也补不回来了。iPhone 用 ProRAW 或 Halide / ProCam 之类的手动 App；
  Android 用「专业模式」并存 RAW。
- **不要过曝**：白色块不能顶到 255，压到 220~240 比较安全。
- **不要有反光**：光源不能在屏幕的镜面反射路径上。反光会抬高黑块，把对比度压低。

**尽量做到的：**

- 光尽量均匀（工具会用色卡四周白边拟合并校正光照不均，但差异越小越好）
- 正对屏幕，整屏入画
- 锁定白平衡和曝光
- 就在设备平时所处的房间和光线下拍

### 工具会自己检查

标定不会闷头产出一个看着像模像样的 profile。它会先验证这张照片：

- **对比度是否落在物理区间**。BWRY 大约 6:1~13:1。超出就是照片有问题，不是面板有问题。
- **有没有墨水比白块还亮**。白态是面板反射率最高的状态，任何墨水在任何通道上都不可能超过它。
  超过了就说明相机做了饱和度处理 —— 这条检查不依赖任何关于面板的假设。
- **有没有通道被压到 0 或顶到 255**。
- **光照均匀性**。

任何一条硬性检查不过，它会拒绝写 profile 并说明原因。这是有意的：
基于坏测量的 profile 比它要取代的估计值更糟。确实要写就加 `--force`。

### 相机曲线自校准

手机 JPEG 带反差曲线，sRGB 解码之后仍然在。实测这台设备：直接读是 **33:1**，
而 BWRY 物理上到不了 —— 纯色块没有半色调，面板和固件都变不出不存在的对比度。

色卡里的黑白灰阶能把它测出来：半色调按反射率**线性**混合是物理事实，所以真实反射率
对墨水覆盖率必须是一条直线。**让灰阶变直的那个 γ，就是相机多加的曲线。** 实测 γ=1.43，
修正后对比度落到 11.6:1，正好在物理区间 —— 灰阶的*形状*完全不知道绝对对比度，
却独立地把纯色块修到了合理范围，两个证据互相印证。

灰阶用的是 4 行粗条带而不是细棋盘：覆盖率一样精确，但边缘少得多，光学扩张的影响更小，
测到的更接近纯粹的相机响应。

诚实的局限：真实存在的光学扩张会被一并吸收进 γ，所以会略微过冲。对比度是否落回
物理区间是对结果是否可信的独立检验。`--camera-gamma 1.0` 可以关掉这个修正。

角点自动识别失败时手动给：

```bash
... calibrate chart.jpg --out my_profile.json --corners x_tl,y_tl,x_tr,y_tr,x_br,y_br,x_bl,y_bl
```

有色度计或色卡 App 的话可以完全跳过拍照：

```bash
... calibrate --out my_profile.json --swatches '#5f6165,#fffdf8,#c8b055,#94413a'
```

色卡里除了四个纯色块，还有 25/50/75% 的半色调混合块。
`calibrate` 会检查实测混合色和「两种墨水线性平均」的预测值差多少 ——
如果 ΔE 偏大，说明存在明显的光学扩张（dot gain），profile 只能当近似值用。

然后用它：

```bash
... convert photo.jpg out.bin --preset photo --profile my_profile.json
... ab tmp/*.jpg --out tmp/ab --profile my_profile.json
```

## Preset

| preset | 适用 | 特点 |
| --- | --- | --- |
| `photo` | 照片 | 适度 S 曲线 + 较强局部对比度，chroma gate 收紧，Sierra-2 serpentine + 边缘保护 |
| `illustration` | 插画、海报、漫画、UI | 保饱和度，gate 放宽，Atkinson 让平涂区域干净、线条锐利 |
| `text` | 截图、文档 | 硬色调曲线，chroma 几乎全关，误差扩散削弱以保住字形边缘 |
| `legacy` | —— | 当前线上算法，A/B 基线 |

`convert` 会顺带打印它对内容类型的判断（`content looks : photo`），可以拿来对照 preset 选得对不对。

## A/B 流程

内置矩阵是一条阶梯，每一级只改一个变量，这样面板上的对比能回答「这一步值不值」，
而不是只能得出「新的看着好一点」：

```
legacy                       现在线上的算法（与 JS 逐字节一致）
01-cal-lab-fs                + 实测 palette + Lab ΔE76 选色
02-linear-error              + 误差改在线性光里扩散
03-tone-gamut                + 色调曲线 / 局部对比度 / 色域压缩
04-chroma-gate               + chroma gate
05-fs-serpentine             + serpentine 扫描
06-sierra2                   Sierra-2（计划里的主选）
07-stucki                    Stucki
08-atkinson                  Atkinson
09-sierra2-edge              + 边缘感知误差衰减
10-bluenoise                 有序 void-and-cluster 蓝噪声（第二条路线）
11-sierra2-bluenoise-hybrid  Sierra-2 + 蓝噪声调制
12-illustration / 13-text    两个 preset
```

每个 (图片 × 配方) 会写出 `.bin`、逐像素预览 PNG、人眼模拟 PNG，
以及一份 `.json`：完整参数 + 使用的 palette + 客观指标。
任何一个 `.bin` 都能追溯回产生它的确切配方。

只跑其中几个：

```bash
... ab tmp/Cubes.jpg --out tmp/ab --only 06-sierra2,09-sierra2-edge,10-bluenoise
```

**直接推到设备逐张对比**（这一步会写入设备相册，所以是显式 opt-in）：

```bash
... ab tmp/Cubes.jpg --out tmp/ab --only 06-sierra2,09-sierra2-edge --push http://<设备IP>
```

每张的标题会写成 `图片名 · 配方名`，在设备上翻页就能对上号。

### 指标怎么读

指标只用来缩小范围，**最终以真机显示效果为准**。

- **dE self** —— 半色调离它自己的目标有多远，衡量抖色器本身；只在共用同一条色调曲线的配方之间可比。
- **dE ref** —— 离「朴素比色渲染」有多远。刻意的色调选择会把它抬高，所以它是描述而不是评分。
- **confetti** —— 源图中性区域里拿到红/黄墨水的像素比例。**越低越好，永远**。这就是「灰墙彩噪」那个数。
- **anisotropy** —— 半色调纹理的方向性。蛇形扫描和蓝噪声会把它压向 0，raster 扫描的 worm 会把它抬高。

dE 都是在 HVS 滤波之后算的：半色调逐像素比对没有意义，两边都要先过一个近似人眼空间响应的高斯。

## 目录

```
bwry/color.py       sRGB / linear / XYZ / Lab / LCh，ΔE76、ΔE94
bwry/palette.py     PaletteProfile：墨水实测值 + 设备编码 + 序列化
bwry/gamut.py       四面体色域 + 保色相的软压缩
bwry/tone.py        单调色调 LUT、局部对比度、L* 区间映射、chroma gate
bwry/edges.py       梯度 / 平坦度 / 内容分类
bwry/bluenoise.py   void-and-cluster 蓝噪声（增量能量场）
bwry/dither.py      误差扩散核 + 蓝噪声有序抖色
bwry/legacy.py      当前线上算法的忠实复刻（A/B 基线）
bwry/metrics.py     HVS ΔE、墨水用量、彩噪率、纹理各向异性
bwry/pipeline.py    Recipe / convert / 产物写出
bwry/presets.py     preset 与 A/B 矩阵
bwry/calibrate.py   色卡生成与实测提取
bwry/abtest.py      矩阵执行、对比页、设备推送
bwry/pack.py        2bpp 打包 / 解包 / 预览渲染
```
