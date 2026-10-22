/*
 * mirror-dock.hpp - AudioMirror 监听面板(OBS dock)
 *
 * 傻瓜版交互: 打开就是一张设备勾选列表 —— 勾一个就开始监听一个,
 * 每设备可单独调音量, 实时显示该路实际延迟(毫秒)。
 */
#pragma once

#include <QWidget>

#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QTimer;
class QVBoxLayout;

class MirrorDock : public QWidget {
	Q_OBJECT

public:
	explicit MirrorDock(QWidget *parent = nullptr);
	~MirrorDock() override;

	/* 重新枚举设备并重建列表(保留当前勾选) */
	void refreshDevices();
	/* 刷新状态: 运行/停止、每设备延迟、桌面音频源提示 */
	void refreshStatus();

private slots:
	void onDeviceToggled();
	void onVolumeChanged();
	void onSelectAll();
	void onSelectNone();
	void onTrackChanged(int index);
	void onAutoStartToggled(bool checked);
	void onAddDesktopAudio();

private:
	QStringList checkedIds() const;
	void applyNow();

	struct DeviceRow {
		QString id;
		QCheckBox *check = nullptr;
		QSlider *volume = nullptr;
		QLabel *info = nullptr;
		QLabel *latency = nullptr;
	};

	QVBoxLayout *rowsLayout_ = nullptr;
	QVector<DeviceRow> rows_;

	QPushButton *selectAllBtn_ = nullptr;
	QPushButton *selectNoneBtn_ = nullptr;
	QComboBox *trackCombo_ = nullptr;
	QCheckBox *autoStartCheck_ = nullptr;
	QLabel *statusLabel_ = nullptr;
	QLabel *tipLabel_ = nullptr;
	QPushButton *addDesktopBtn_ = nullptr;
	QTimer *statusTimer_ = nullptr;
};
