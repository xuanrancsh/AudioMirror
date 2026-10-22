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

/* 一个可监听的音频采集源(OBS 源, 桌面音频类) */
struct AudioSourceInfo {
	QString name;                  /* OBS 源名, 如 "桌面音频" */
	QString typeId;                /* 源类型 id: wasapi_output_capture /
					wasapi_process_output_capture */
	QString deviceId;              /* settings 的 device_id, 空则 "(默认)" */
	QString typeDesc;              /* 类型描述: "桌面音频 (环回)" / "应用程序音频" */
	bool isDefaultDesktop = false; /* 默认桌面音频(环回 + 默认设备) */
};

namespace mirror {

/* ---------- 设备枚举(UI 线程调用) ---------- */
QVector<MirrorDevice> enumDevices();

/* ---------- 运行控制(均在 UI 线程调用) ---------- */

/* 是否有设备正在监听 */
bool isActive();

/* 应用选择: 立即生效(新增的开路/取消的断路), 并持久化启用状态。
 * deviceIds: 需要监听的设备 ID 列表(<=5, 内部去重/剔除默认播放设备)。
 * sourceName: 为空 = 固定采集 OBS 默认桌面音频(默认路径);
 *             非空 = 采集指定名字的桌面音频类源("监听其他音频源"
 *             高级功能勾选时由 UI 传入)。
 * 返回错误描述; 成功返回空串。 */
QString applySelection(const QStringList &deviceIds,
		       const QString &sourceName = QString());

/* 枚举 OBS 里所有桌面音频类源(环回 + 应用程序音频), 供
 * "监听其他音频源(高级)"下拉选择。UI 线程调用。
 * 排序: 默认桌面音频优先, 其余按源名。 */
QVector<AudioSourceInfo> enumAudioSources();

/* 当前采集源的描述串, 如 "桌面音频 · 桌面音频 (环回) · 默认设备";
 * 未在监听(没有采集源)时返回空串。UI 用于诊断当前实际挂了哪个源。 */
QString captureSourceDesc();

/* 运行时改音量(0-200, 100 = 原始音量), 立即生效不落盘 */
void setDeviceVolume(const QString &deviceId, double percent);

/* 每设备当前积压(毫秒)= 该路实际监听延迟; 未运行返回 -1。
 * UI 定时轮询用于实时显示。 */
double deviceLatencyMs(const QString &deviceId);

/* 无音频输入诊断: 渲染器在运行, 但超过 2 秒没收到桌面音频包
 * (采集源被删除/采集失败/目标应用未运行等)。UI 用于在延迟位置给出
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

/* 高级功能"监听其他音频源"开关: 默认 false(固定采集默认桌面音频)。
 * 勾选时 applySelection 的 sourceName 传 cfgCustomSourceName()。 */
bool cfgUseCustomSource();
void cfgSetUseCustomSource(bool v);
/* 高级功能选中的音频源名(默认空; 仅 cfgUseCustomSource() 为 true 时生效) */
QString cfgCustomSourceName();
void cfgSetCustomSourceName(const QString &name);

/* ---------- 傻瓜版: 桌面音频保障 ---------- */

/* 确保 OBS 里有源在采集"默认桌面音频(系统默认输出设备的环回)":
 *   已有默认桌面音频源              -> true, 什么都不做;
 *   没有 且 createIfMissing = true  -> 自建一个(常驻持有引用,
 *                                      不加入任何场景);
 *   没有 且 createIfMissing = false -> false(UI 应提示手动添加)。
 * 说明: 采集与场景/激活无关 —— win-wasapi 采集源在构造函数末尾即
 * Start() 采集线程(obs-source.c 的 source_signal_audio_data 音频回调
 * 也无激活/静音门控), 因此本函数不把源加进任何场景, 不污染用户场景;
 * 自建源的生命周期 = 插件生命周期(stopAll / 模块卸载时释放)。 */
bool ensureDesktopAudioSource(bool createIfMissing);

} // namespace mirror
