/*
 * plugin-main.cpp - AudioMirror 插件主入口
 *
 * 参考 DistroAV / obs-plugintemplate 的模块组织方式:
 *   - obs_module_load 里注册工具菜单 + dock + 事件回调 + 热键
 *   - 不注册 source/output/filter(纯前端工具型插件)
 *
 * 傻瓜版行为:
 *   - OBS 启动完成 -> 自动确保有"桌面音频"采集源 -> 按上次勾选自动开始
 *   - dock 自动弹出, 勾选即监听
 */
#include "plugin-main.hpp"
#include "mirror-core.hpp"
#include "mirror-dock.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QMainWindow>
#include <QAction>
#include <QApplication>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* 用 QPointer 保存: dock 是 OBS 主窗口的子对象, 退出时可能被 OBS
 * 先一步销毁。QPointer 会在对象被销毁后自动置空, 避免悬垂指针。 */
static QPointer<MirrorDock> g_dock;
static obs_hotkey_id g_hotkey_id = OBS_INVALID_HOTKEY_ID;

/* ================= 工具菜单 ================= */

static void show_dock()
{
	if (!g_dock.isNull()) {
		g_dock->show();
		g_dock->raise();
	}
}

/* ================= 一键开始/停止(热键共用) ================= */

static void toggle_all()
{
	if (mirror::isActive()) {
		mirror::stopAll();
	} else {
		const QString err =
			mirror::applySelection(mirror::cfgDevices(),
					       mirror::cfgTrack());
		if (!err.isEmpty())
			blog(LOG_WARNING, "[AudioMirror] start failed: %s",
			     qUtf8Printable(err));
	}
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
		 * 2) 上次有勾选设备且开了自动开始 -> 直接开始监听。 */
		mirror::ensureDesktopAudioSource(true);

		if (mirror::cfgAutoStart()) {
			const QStringList devs = mirror::cfgDevices();
			if (!devs.isEmpty()) {
				blog(LOG_INFO,
				     "[AudioMirror] auto start: %d device(s)",
				     (int)devs.size());
				QMetaObject::invokeMethod(
					qApp,
					[devs] {
						const QString err =
							mirror::applySelection(
								devs,
								mirror::cfgTrack());
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
		 * 与 dock, 对正在析构的 QWidget 操作会崩溃。 */
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
		/* 工具菜单: 显示/隐藏面板 */
		auto *menu_action = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(
				obs_module_text("AudioMirror.Menu.ShowHide")));
		menu_action->connect(menu_action, &QAction::triggered,
				     show_dock);

		/* dock: 设备勾选面板 */
		obs_frontend_push_ui_translation(obs_module_get_string);
		g_dock = new MirrorDock(main_window);
		obs_frontend_pop_ui_translation();

		obs_frontend_add_dock_by_id(
			"AudioMirrorDock",
			obs_module_text("AudioMirror.Title"),
			static_cast<void *>(g_dock.data()));
		/* 傻瓜版: 首次安装后直接可见, 不需要用户去找菜单 */
		g_dock->show();
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
	 *    卸载阶段 OBS 正在销毁主窗口与 dock, 对正在析构的 QWidget
	 *    操作会把事件派发到已释放对象上导致崩溃。 */
	mirror::stopAll();

	/* 4. dock 不在这里 delete: 它是 OBS 主窗口的子对象, 由 Qt 的
	 *    父子所有权自动销毁; 此处同步 delete 若 OBS 已先销毁过它,
	 *    就是二次析构。g_dock 用 QPointer 持有, 清空引用即可。 */
	g_dock = nullptr;

	blog(LOG_INFO, "[AudioMirror] plugin unloaded");
}
