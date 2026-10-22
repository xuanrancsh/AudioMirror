/*
 * mirror-core.hpp - AudioMirror 插件核心
 *
 * 职责: 把 OBS 的"桌面音频"(电脑输出的所有声音, 不含麦克风)
 *       同时、低延迟地输出到多个 WASAPI 渲染设备。
 *
 * 数据通路(v0.2.0 起改为按源采集, 不再订阅主混音):
 *   OBS 桌面音频源(wasapi_output_capture 环回采集)
 *     --obs_source_add_audio_capture_callback(每设备注册一次)-->
 *     每设备独立 libobs 重采样(源格式 -> 该设备 mix format, float)
 *     -> 平面转交错 -> 本插件做最后一次 float -> 设备位深转换
 *     -> WASAPI shared mode + 自适应积压控制写出
 *
 * 为什么按源采集而不是订阅主混音:
 *   - 主混音里混着麦克风; 按源采集只订桌面音频类源
 *     (wasapi_output_capture / wasapi_process_output_capture),
 *     麦克风源(wasapi_input_capture)从机制上被排除,
 *     用户不需要去关 OBS 的麦克风。
 *   - 不碰 OBS 混音器/音轨/监听设备设置。
 *
 * 与 OBS 内置监听的差异(为什么要这个插件):
 *   - OBS 的"高级音频属性 -> 监听设备"只能选一个设备;
 *   - OBS 监听走它自己的 audio_output 管道, 缓冲大且有时钟漂移,
 *     论坛有大量"越跑越延迟"的投诉(obsproject#3664);
 *   - 本插件自管 WASAPI 渲染 + 自适应积压控制,
 *     把每路延迟钉在一个音频块的量级, 且不会随时间累积。
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstddef>
#include <cstdint>

/* 一个可监听的输出设备 */
struct MirrorDevice {
	QString id;             /* WASAPI 设备 ID(配置持久化用) */
	QString name;           /* 友好名, 如 "Elgato HD60 X" */
	bool isDefault = false; /* 当前系统默认播放设备: 声音已在此播放, 选它只会加倍音量 */
};

/* 一个监听位(固定 5 个): 启用开关 + 目标设备 + 音量 */
struct MirrorSlot {
	bool enabled = false;
	QString device;
	double volume = 100.0;
};

namespace mirror {

/* ---------- 设备枚举(UI 线程调用) ---------- */
QVector<MirrorDevice> enumDevices();

/* ---------- 运行控制(均在 UI 线程调用) ---------- */

/* 是否有设备正在监听 */
bool isActive();

/* 应用选择: 立即生效(新增的开路/取消的断路), 并持久化启用状态。
 * deviceIds: 需要监听的设备 ID 列表(<=5, 内部去重/剔除默认播放设备)。
 * 返回错误描述; 成功返回空串。 */
QString applySelection(const QStringList &deviceIds);

/* 运行时改音量(0-200, 100 = 原始音量), 立即生效不落盘 */
void setDeviceVolume(const QString &deviceId, double percent);

/* 每设备当前积压(毫秒)= 该路实际监听延迟; 未运行返回 -1。
 * UI 定时轮询用于实时显示。 */
double deviceLatencyMs(const QString &deviceId);

/* 无音频输入诊断: 渲染器在运行, 但超过 2 秒没收到桌面音频包
 * (源不在当前场景/被移除/采集失败)。UI 用于在延迟位置给出
 * "无音频输入"提示, 避免把 0.0 ms 误显示成正常低延迟。
 * 注意: OBS 混音器里的静音/隐藏不算 —— 那只是显示状态, 采集
 * 数据仍照常转发。 */
bool deviceAudioStalled(const QString &deviceId);

/* 运行中设备的格式描述(如 "48000 Hz · 24 bit"); 未运行返回空串 */
QString deviceFormat(const QString &deviceId);

/* 全部停止: 只停音频/释放 WASAPI, 绝不碰 Qt 控件
 * (OBS 退出与模块卸载时调用是安全的)。 */
void stopAll();

/* ---------- 配置持久化(OBS user config, 段 "AudioMirror") ---------- */
QVector<MirrorSlot> cfgSlots();                 /* 固定 5 个, 默认全关 */
/* 注意: 形参名不能用 slots —— Qt 把 slots 定义为空宏(signals/slots
 * 机制), 会被预处理掉导致语法错误。 */
void cfgSetSlots(const QVector<MirrorSlot> &slotList);
double cfgDeviceVolume(const QString &id);      /* 按设备 ID, 默认 100 */
void cfgSetDeviceVolume(const QString &id, double percent);
bool cfgAutoStart();                            /* 默认 true: OBS 运行时自动开启 */
void cfgSetAutoStart(bool v);
bool cfgAutoOpen();                             /* 默认 true: OBS 启动时自动打开面板 */
void cfgSetAutoOpen(bool v);

/* ---------- 傻瓜版: 桌面音频保障 ---------- */

/* 确保 OBS 里有源在采集"桌面音频(系统输出声音)":
 *   已有活跃的桌面音频源                -> true, 什么都不做;
 *   有但不活跃(在别的场景里)            -> 把它加进当前场景;
 *   没有 且 createIfMissing = true      -> 自动创建并加入当前场景;
 *   创建/添加失败                       -> false(UI 应提示手动添加)。
 * 说明: 本插件按源采集桌面音频; OBS 的音频源只在所在场景激活时才
 * 产出音频, 所以切场景后要重新保障(plugin-main 监听 SCENE_CHANGED)。
 * 此函数让"什么都不会"的用户装上就能响。 */
bool ensureDesktopAudioSource(bool createIfMissing);

} // namespace mirror
