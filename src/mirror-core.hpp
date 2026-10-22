/*
 * mirror-core.hpp - AudioMirror 插件核心
 *
 * 职责: 把 OBS 主混音(音轨 1-6)的成品音频同时、低延迟地输出到
 *       多个 WASAPI 渲染设备。
 *
 * 数据通路:
 *   OBS 主混音(mix N) --obs_add_raw_audio_callback(每设备一次注册)-->
 *     每设备独立重采样成该设备的 mix format(libobs 负责) -->
 *     本插件做最后一次 float -> 设备位深转换 + 自适应积压控制写出
 *
 * 与 OBS 内置监听的差异(为什么要这个插件):
 *   - OBS 的"高级音频属性 -> 监听设备"每条混音只能选一个设备;
 *   - OBS 监听走它自己的 audio_output 管道, 缓冲大且有时钟漂移,
 *     论坛有大量"越跑越延迟"的投诉(obsproject#3664);
 *   - 本插件直采混音输出, 自管 WASAPI 渲染 + 自适应积压控制,
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

namespace mirror {

/* ---------- 设备枚举(UI 线程调用) ---------- */
QVector<MirrorDevice> enumDevices();

/* ---------- 运行控制(均在 UI 线程调用) ---------- */

/* 是否有设备正在监听 */
bool isActive();

/* 应用勾选: 立即生效(新增的开路/取消的断路), 并持久化。
 * track: 订阅的音轨 1-6(1 = 主混音成品)。
 * 返回错误描述; 成功返回空串。 */
QString applySelection(const QStringList &deviceIds, int track);

/* 运行时改音量(0-200, 100 = 与 OBS 主混音一致), 立即生效不落盘 */
void setDeviceVolume(const QString &deviceId, double percent);

/* 每设备当前积压(毫秒)= 该路实际监听延迟; 未运行返回 -1。
 * UI 定时轮询用于实时显示。 */
double deviceLatencyMs(const QString &deviceId);

/* 运行中设备的格式描述(如 "48000 Hz · 24 bit"); 未运行返回空串 */
QString deviceFormat(const QString &deviceId);

/* 全部停止: 只停音频/释放 WASAPI, 绝不碰 Qt 控件
 * (OBS 退出与模块卸载时调用是安全的)。 */
void stopAll();

/* ---------- 配置持久化(OBS user config, 段 "AudioMirror") ---------- */
QStringList cfgDevices();                            /* 默认空 */
void cfgSetDevices(const QStringList &ids);
double cfgDeviceVolume(const QString &id);           /* 默认 100 */
void cfgSetDeviceVolume(const QString &id, double percent);
int cfgTrack();                                      /* 默认 1 */
void cfgSetTrack(int track);
bool cfgAutoStart();                                 /* 默认 true */
void cfgSetAutoStart(bool v);

/* ---------- 傻瓜版: 桌面音频保障 ---------- */

/* 确保 OBS 里有源在采集"系统输出声音"(桌面音频):
 *   已有在采的桌面音频源           -> true, 什么都不做;
 *   没有 且 createIfMissing = true -> 自动创建并加入当前场景;
 *   创建失败                       -> false(UI 应提示手动添加)。
 * 说明: 本插件订阅的是 OBS 混音输出; 若用户的 OBS 里没有任何
 * 音频源, 混音就是静音, 监听不到任何声音。此函数让"什么都不会"
 * 的用户装上就能响。 */
bool ensureDesktopAudioSource(bool createIfMissing);

} // namespace mirror
