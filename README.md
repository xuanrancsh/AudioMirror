# AudioMirror - 音频多设备监听

**把 OBS 主混音的成品音频，同时、低延迟地输出到多个音频设备。**

OBS 内置的「高级音频属性 → 监听设备」每条混音只能选**一个**设备——想同时用耳机、音箱、采集卡、直播闸刀一起听？OBS 做不到。本插件补上这个能力：直采 OBS 主混音输出，自管 WASAPI 低延迟渲染，勾选几个设备就同时送几个。

## 功能

- **多设备同时监听**：勾选即开始，可任意多选；每设备独立音量（0–200%）
- **低延迟**：直采混音输出 + WASAPI shared mode + 自适应积压控制，每路延迟钉在一个音频块的量级（有线设备约 20–40ms，面板实时显示每路毫秒数），且**不会像 OBS 内置监听那样越跑越延迟**
- **傻瓜式体验**：安装后 OBS 一打开就自动弹出设备列表；OBS 里没有采集系统声音的源时，自动创建「桌面音频」源；上次勾选的设备开机自动恢复
- **防呆设计**：当前默认播放设备自动置灰（声音已经在这里播放，再监听只会加倍音量）
- **音轨可选**：默认音轨 1（最终混音成品），也可选 1–6 中任意一条
- **热键**：一键开始/停止全部监听（可在 OBS 设置里绑定）

## 延迟说明（重要预期管理）

| 路径 | 延迟量级 |
|------|----------|
| 本插件（有线耳机/音箱/USB 设备） | 约 20–40ms |
| 本插件（蓝牙设备） | 额外 +150–300ms（蓝牙协议硬件限制，任何软件都无解） |
| 本插件（HDMI 采集卡/电视） | 视设备音频处理，约 +20–80ms |
| OBS 内置监听 | 缓冲更大，且有时钟漂移导致的「越跑越延迟」问题（[obsproject#3664](https://github.com/obsproject/obs-studio/issues/3664)） |

「零延迟」在数字音频里不存在（必然经过缓冲区），但 20–40ms 已达到人耳基本无感的量级。两个使用注意：

1. **Windows 主音量保持 100%**——本插件采的是主音量之后的声音，主音量调小采集音量会跟着小；
2. 不要同时开启 OBS 内置监听和本插件到同一个设备（会双倍音量/回声）。

## 安装

从 [Releases](https://github.com/xuanrancsh/AudioMirror/releases) 下载 `AudioMirror-<版本>-windows-x64.exe` 安装包，按提示安装到 OBS 插件目录即可（要求 OBS Studio 31.1.1 或更高版本，64 位）。

## 从源码构建

依赖：CMake 3.16+、Qt6、[obs-deps](https://github.com/obsproject/obs-deps)（预编译版）、OBS Studio 源码（或已安装的 OBS SDK）。

```bash
# 本地构建（Windows，需先设置好 obs-deps 与 OBS 源码路径，见 CMakePresets.json）
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

也可以直接推送 tag 触发 GitHub Actions 云端构建（windows-latest，自动出 Release 安装包）：

```bash
git tag v0.1.0 && git push origin v0.1.0
```

## 技术路线

```
OBS 主混音(mix N, 默认 mix 0 = 音轨 1)
   │  obs_add_raw_audio_callback(每设备注册一次, 各自带该设备的 conv)
   ▼
libobs 音频引擎按目标设备格式重采样/声道映射(交错 float32)
   │
   ▼
本插件: float -> 设备位深 + 音量增益
   │  自适应积压控制(目标 = 1 个音频块, 漂移每次最多丢 1ms)
   ▼
WASAPI shared mode 渲染 -> 设备 A / 设备 B / 设备 N (各自独立, 互不阻塞)
```

核心手法与用户的 [obs-capcast](https://github.com/xuanrancsh/obs-capcast) 项目同源（`obs_add_raw_audio_callback` 订阅主混音 + WASAPI 直出 + 积压控制），已在生产环境验证。

## 许可证

[GPL-2.0](LICENSE)（链接 libobs 的 OBS 插件须为 GPL-2.0+）。

> 本项目代码由 AI 辅助生成（依据 OBS 论坛资源发布政策声明）。
