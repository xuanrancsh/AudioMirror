# AudioMirror - 音频多设备监听

**把电脑输出的所有声音（不含麦克风），同时、低延迟地监听到多个音频设备。**

OBS 内置的「高级音频属性 → 监听设备」只能选**一个**设备——想同时用耳机、音箱、采集卡、直播闸刀一起听？OBS 做不到。本插件补上这个能力：按源采集 OBS 的桌面音频，自管 WASAPI 低延迟渲染，勾选几个设备就同时送几个。

## 功能（v0.2.0）

- **5 个固定监听位**：每位的下拉里选一个输出设备即开始监听该设备（取消勾选「启用」即停止，保留选择）；每设备独立音量（0–200%）
- **只采桌面音频，不采麦克风**：从机制上排除麦克风源，**不需要**去关 OBS 的麦克风
- **低延迟**：按源采集 + 每设备 libobs 重采样 + WASAPI shared mode + 自适应积压控制，每路延迟钉在一个音频块的量级（有线设备约 20–40ms，面板实时显示每路毫秒数），且**不会像 OBS 内置监听那样越跑越延迟**
- **傻瓜式体验**：OBS 一打开就自动弹出面板；没有采集系统声音的源时自动创建「桌面音频」源；切场景自动保障采集不断；上次的 5 个监听位开机自动恢复
- **两个自动勾选框**：「OBS 运行时自动开启监听」（勾选则每次开 OBS 按各位状态自动开始，不勾选则完全手动）、「OBS 启动时自动打开本窗口」
- **防呆设计**：当前默认播放设备自动置灰（声音已经在这里播放，再监听只会加倍音量）；同一设备不能被两个监听位同时选
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

## 使用方法

1. 安装插件，打开 OBS（要求 OBS Studio 31.1.1 或更高版本，64 位）
2. 「音频多设备监听」面板自动弹出（也可随时从 工具 → 音频多设备监听 打开）
3. 在任意监听位的下拉里选择目标设备——自动开始监听；最多 5 台
4. 「OBS 运行时自动开启监听」保持勾选，则下次开 OBS 自动恢复

## 安装

从 [Releases](https://github.com/xuanrancsh/AudioMirror/releases) 下载 `AudioMirror-<版本>-windows-x64-Installer.exe` 安装包，按提示安装到 OBS 插件目录即可。

## 从源码构建

依赖：CMake 3.16+、Qt6、[obs-deps](https://github.com/obsproject/obs-deps)（预编译版）、OBS Studio 源码（或已安装的 OBS SDK）。

```bash
# 本地构建（Windows，需先设置好 obs-deps 与 OBS 源码路径，见 CMakePresets.json）
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

也可以直接推送 tag 触发 GitHub Actions 云端构建（windows-latest，自动出 Release 安装包）：

```bash
git tag v0.2.0 && git push origin v0.2.0
```

## 技术路线（v0.2.0）

```
OBS 桌面音频源(wasapi_output_capture 环回采集 / wasapi_process_output_capture 应用音频)
   │  obs_source_add_audio_capture_callback(每设备注册一次, param = 该设备渲染器)
   │  ※ 麦克风源(wasapi_input_capture)从机制上被排除, 无需关 OBS 麦克风
   ▼
libobs audio_resampler_t: 源格式 -> 该设备 mix format(平面 float)
   │
   ▼
本插件: 平面 -> 交错 -> float -> 设备位深 + 音量增益
   │  自适应积压控制(目标 = 1 个音频块, 漂移每次最多丢 1ms)
   ▼
WASAPI shared mode 渲染 -> 设备 A / 设备 B / ... / 设备 E (各自独立, 互不阻塞)
```

核心手法与用户的 [obs-capcast](https://github.com/xuanrancsh/obs-capcast) 项目同源（WASAPI 直出 + 自适应积压控制，已在生产环境验证）。v0.1.0 及以前用 `obs_add_raw_audio_callback` 订阅主混音；v0.2.0 起改为按源采集，以排除麦克风。

## 已知限制

- 设备热插拔后不会自动恢复（要重新选一次；下次启动按配置恢复）
- DRM 保护内容（如部分 Netflix/Disney+ 页面）采不到
- 切换场景时若桌面音频源不在新场景，插件会自动把它加进去（可能需要一两秒）
- Windows 主音量需保持 100%（调小采集音量会跟着小）

## 许可证

[GPL-2.0](LICENSE)（链接 libobs 的 OBS 插件须为 GPL-2.0+）。

> 本项目代码由 AI 辅助生成（依据 OBS 论坛资源发布政策声明）。
