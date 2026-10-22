/*
 * plugin-main.cpp - AudioMirror 插件主入口
 *
 * 参考 DistroAV / obs-plugintemplate 的模块组织方式:
 *   - obs_module_load 里注册工具菜单 + 事件回调 + 热键
 *   - 不注册 source/output/filter(纯前端工具型插件)
 *
 * 傻瓜版行为(v0.2.0):
 *   - OBS 启动完成 -> 确保有"桌面音频"采集源 -> 按 5 个监听位的
 *     勾选状态自动开始(自动开启勾选时) -> 面板自动弹出(自动打开勾选时)
 *   - 工具菜单点击 -> 弹出/置顶面板
 *
 * v0.2.2 起:
 *   - 默认固定采集 OBS 默认桌面音频(device_id="default" 的环回源),
 *     采集与场景/激活无关, 不再监听切场景事件;
 *   - "监听其他音频源"是可选高级功能(默认关闭), 勾选后采集指定源。
 */
#include "plugin-main.hpp"
#include "mirror-core.hpp"
#include "mirror-window.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QMainWindow>
#include <QAction>
#include <QApplication>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* 用 QPointer 保存: 面板是 OBS 主窗口的子对象, 退出时可能被 OBS
 * 先一步销毁。QPointer 会在对象被销毁后自动置空, 避免悬垂指针。 */
static QPointer<MirrorWindow> g_window;
static obs_hotkey_id g_hotkey_id = OBS_INVALID_HOTKEY_ID;

/* ================= 面板显隐 ================= */

static void show_window()
{
	if (g_window.isNull()) {
		QMainWindow *main_window =
			static_cast<QMainWindow *>(obs_frontend_get_main_window());
		if (!main_window)
			return;
		obs_frontend_push_ui_translation(obs_module_get_string);
		g_window = new MirrorWindow(main_window);
		obs_frontend_pop_ui_translation();
	}
	g_window->show();
	g_window->raise();
	g_window->activateWindow();
}

/* ================= 一键开始/停止(热键共用) ================= */

/* 收集 5 个监听位里已勾选的设备 */
static QStringList enabledDeviceIds()
{
	QStringList ids;
	for (const auto &s : mirror::cfgSlots()) {
		if (s.enabled && !s.device.isEmpty())
			ids.append(s.device);
	}
	return ids;
}

/* 高级功能"监听其他音频源": 勾选时才传采集源名, 否则走默认路径
 * (固定 OBS 默认桌面音频)。 */
static QString customSourceName()
{
	if (!mirror::cfgUseCustomSource())
		return QString();
	const QString name = mirror::cfgCustomSourceName();
	if (name.isEmpty())
		return QString();
	/* 配置的源可能已被删除(如 OBS 关闭期间删的): 校验存在性,
	 * 不存在则回退默认桌面音频 —— 与面板路径的静默降级行为一致,
	 * 避免重启后自动开启/热键以"找不到音频源"失败且无明确原因。 */
	const auto srcs = mirror::enumAudioSources();
	for (const auto &s : srcs) {
		if (s.name == name)
			return name;
	}
	blog(LOG_WARNING,
	     "[AudioMirror] 配置的音频源「%s」不存在, 回退默认桌面音频",
	     qUtf8Printable(name));
	return QString();
}

static void toggle_all()
{
	if (mirror::isActive()) {
		mirror::stopAll();
		return;
	}
	const QStringList ids = enabledDeviceIds();
	if (ids.isEmpty())
		return;
	const QString err = mirror::applySelection(ids, customSourceName());
	if (!err.isEmpty())
		blog(LOG_WARNING, "[AudioMirror] start failed: %s",
		     qUtf8Printable(err));
}

static void hotkey_callback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		QMetaObject::invokeMethod(qApp, [] { toggle_all(); },
					  Qt::QueuedConnection);
}

/* ================= 事件回调 ================= */

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		/* 傻瓜版: OBS 一开就把该做的自动做完。
		 * 1) 确保有源在采系统声音(没有就自动创建);
		 * 2) 勾了"自动打开" -> 弹出面板;
		 * 3) 勾了"自动开启"且有启用的监听位 -> 直接开始监听
		 *    ("监听其他音频源"勾选时采集配置里指定的源)。 */
		mirror::ensureDesktopAudioSource(true);

		if (mirror::cfgAutoOpen()) {
			QMetaObject::invokeMethod(
				qApp, [] { show_window(); },
				Qt::QueuedConnection);
		}

		if (mirror::cfgAutoStart()) {
			const QStringList ids = enabledDeviceIds();
			if (!ids.isEmpty()) {
				const QString srcName = customSourceName();
				blog(LOG_INFO,
				     "[AudioMirror] auto start: %d device(s)",
				     (int)ids.size());
				QMetaObject::invokeMethod(
					qApp,
					[ids, srcName] {
						const QString err =
							mirror::applySelection(
								ids, srcName);
						if (!err.isEmpty())
							blog(LOG_WARNING,
							     "[AudioMirror] auto start failed: %s",
							     qUtf8Printable(err));
					},
					Qt::QueuedConnection);
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT ||
		   event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		/* OBS 开始关闭: 立刻停止全部监听并释放 WASAPI。
		 * 只停音频, 不碰任何 Qt 控件 —— 此刻 OBS 正在销毁主窗口
		 * 与面板, 对正在析构的 QWidget 操作会崩溃。 */
		mirror::stopAll();
	}
}

/* ================= 模块入口 ================= */

bool obs_module_load(void)
{
	blog(LOG_INFO, "%s %s loaded", PLUGIN_DISPLAY_NAME, PLUGIN_VERSION);

	QMainWindow *main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (main_window) {
		/* 工具菜单: 弹出/置顶面板 */
		auto *menu_action = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(
				obs_module_text("AudioMirror.Menu.ShowHide")));
		menu_action->connect(menu_action, &QAction::triggered,
				     show_window);
	}

	/* 事件回调: 启动自动开始 / 退出清理 */
	obs_frontend_add_event_callback(frontend_event, nullptr);

	/* 全局热键: 一键开始/停止全部 */
	g_hotkey_id = obs_hotkey_register_frontend(
		"audiomirror.toggle_all",
		obs_module_text("AudioMirror.Hotkey.Toggle"), hotkey_callback,
		nullptr);

	blog(LOG_INFO, "[AudioMirror] plugin loaded (version %s)",
	      PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[AudioMirror] unloading...");

	/* 1. 先注销回调, 防止卸载后 OBS 再回调到已失效的代码 */
	obs_frontend_remove_event_callback(frontend_event, nullptr);

	/* 2. 注销热键 */
	if (g_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_hotkey_id);
		g_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}

	/* 3. 只停止音频路由并释放 WASAPI, 绝不触碰 Qt 控件。
	 *    卸载阶段 OBS 正在销毁主窗口与面板, 对正在析构的 QWidget
	 *    操作会把事件派发到已释放对象上导致崩溃。 */
	mirror::stopAll();

	/* 4. 面板不在这里 delete: 它是 OBS 主窗口的子对象, 由 Qt 的
	 *    父子所有权自动销毁; 此处同步 delete 若 OBS 已先销毁过它,
	 *    就是二次析构。g_window 用 QPointer 持有, 清空引用即可。 */
	g_window = nullptr;

	blog(LOG_INFO, "[AudioMirror] plugin unloaded");
}
