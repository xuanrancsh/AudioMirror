/*
 * mirror-window.hpp - AudioMirror 监听面板(弹窗)
 *
 * v0.1.0 用 QDockWidget, 但 OBS 会把 dock 包进自己的 QDockWidget,
 * 用户关掉一次后工具菜单就再也点不出来了(v0.1.0 实测 bug)。
 * v0.2.0 改为非模态 QDialog: 菜单点击必弹窗, 关闭只是隐藏。
 *
 * 布局: 固定 5 个监听位, 每位 = 启用勾选框 + 设备下拉 + 音量 + 实时延迟;
 *       另有"OBS 运行时自动开启"与"OBS 启动时自动打开本窗口"两个勾选框。
 */
#pragma once

#include "mirror-core.hpp"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

/* 与 mirror-core.cpp 的匿名命名空间常量保持一致 */
constexpr int kSlotCount = 5;

class MirrorWindow : public QDialog {
	Q_OBJECT

public:
	explicit MirrorWindow(QWidget *parent = nullptr);
	~MirrorWindow() override;

private slots:
	void onSlotEnableToggled();
	void onSlotDeviceChanged();
	void onSlotVolumeChanged();
	void onAutoStartToggled(bool checked);
	void onAutoOpenToggled(bool checked);
	void onAddDesktopAudio();
	void refreshAll();

private:
	void applyNow();
	void refreshStatus();
	void rebuildDeviceCombos();
	void loadSlotStates();
	int slotIndexByWidget(QWidget *w) const;

	/* 一个监听位的控件 */
	struct SlotRows {
		QCheckBox *enable = nullptr;
		QComboBox *device = nullptr;
		QSlider *volume = nullptr;
		QLabel *latency = nullptr;
	};

	QVector<SlotRows> slots_;
	QVector<MirrorDevice> last_devs_; /* 上次枚举到的设备(变化才重建下拉) */
	QCheckBox *autoStartCheck_ = nullptr;
	QCheckBox *autoOpenCheck_ = nullptr;
	QLabel *statusLabel_ = nullptr;
	QPushButton *addDesktopBtn_ = nullptr;
	QTimer *statusTimer_ = nullptr;
};
