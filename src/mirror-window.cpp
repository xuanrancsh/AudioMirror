/*
 * mirror-window.cpp - AudioMirror 监听面板(弹窗)实现
 */
#include "mirror-window.hpp"
#include "mirror-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>

namespace {

/* 面板内边距/间距 */
constexpr int kMargin = 12;
constexpr int kSpacing = 8;

} // namespace

MirrorWindow::MirrorWindow(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(
		QString::fromUtf8(obs_module_text("AudioMirror.Title")));
	setMinimumWidth(500);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
	layout->setSpacing(kSpacing);

	/* ---- 标题说明 ---- */
	auto *header = new QLabel(
		QString::fromUtf8(obs_module_text("AudioMirror.Window.Header")),
		this);
	header->setWordWrap(true);
	layout->addWidget(header);

	/* ---- 5 个固定监听位 ---- */
	for (int i = 0; i < kSlotCount; ++i) {
		auto *row = new QHBoxLayout;
		row->setSpacing(6);

		SlotRows sr;
		sr.enable = new QCheckBox(
			QString::fromUtf8(
				obs_module_text("AudioMirror.Window.SlotEnable")),
			this);
		sr.enable->setToolTip(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.SlotEnableTip")));

		sr.device = new QComboBox(this);
		sr.device->setMinimumWidth(220);
		sr.device->setToolTip(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.SlotDeviceTip")));

		sr.volume = new QSlider(Qt::Horizontal, this);
		sr.volume->setRange(0, 200);
		sr.volume->setFixedWidth(90);
		sr.volume->setToolTip(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.SlotVolumeTip")));

		sr.latency = new QLabel(this);
		sr.latency->setFixedWidth(64);
		sr.latency->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		row->addWidget(sr.enable);
		row->addWidget(sr.device, 1);
		row->addWidget(sr.volume);
		row->addWidget(sr.latency);
		layout->addLayout(row);

		slots_.append(sr);

		connect(sr.enable, &QCheckBox::toggled, this,
			&MirrorWindow::onSlotEnableToggled);
		connect(sr.device,
			QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &MirrorWindow::onSlotDeviceChanged);
		connect(sr.volume, &QSlider::valueChanged, this,
			&MirrorWindow::onSlotVolumeChanged);
	}

	/* ---- 自动开启 / 自动打开 ---- */
	autoStartCheck_ = new QCheckBox(
		QString::fromUtf8(
			obs_module_text("AudioMirror.Window.AutoStart")),
		this);
	autoStartCheck_->setToolTip(QString::fromUtf8(
		obs_module_text("AudioMirror.Window.AutoStartTip")));
	layout->addWidget(autoStartCheck_);

	autoOpenCheck_ = new QCheckBox(
		QString::fromUtf8(
			obs_module_text("AudioMirror.Window.AutoOpen")),
		this);
	autoOpenCheck_->setToolTip(QString::fromUtf8(
		obs_module_text("AudioMirror.Window.AutoOpenTip")));
	layout->addWidget(autoOpenCheck_);

	/* ---- 桌面音频源缺失时的补救按钮 ---- */
	addDesktopBtn_ = new QPushButton(
		QString::fromUtf8(
			obs_module_text("AudioMirror.Window.AddSource")),
		this);
	addDesktopBtn_->setToolTip(QString::fromUtf8(
		"OBS 里没有采集系统声音的源, 点此自动创建一个并加入当前场景"));
	addDesktopBtn_->setVisible(false);
	layout->addWidget(addDesktopBtn_);

	/* ---- 状态 + 提示 ---- */
	statusLabel_ = new QLabel(this);
	statusLabel_->setWordWrap(true);
	layout->addWidget(statusLabel_);

	auto *tipLabel = new QLabel(
		QString::fromUtf8(obs_module_text("AudioMirror.Window.Tip")),
		this);
	tipLabel->setWordWrap(true);
	tipLabel->setEnabled(false); /* 弱化为提示文本 */
	layout->addWidget(tipLabel);

	layout->addStretch(1);

	/* ---- 信号 ---- */
	connect(autoStartCheck_, &QCheckBox::toggled, this,
		&MirrorWindow::onAutoStartToggled);
	connect(autoOpenCheck_, &QCheckBox::toggled, this,
		&MirrorWindow::onAutoOpenToggled);
	connect(addDesktopBtn_, &QPushButton::clicked, this,
		&MirrorWindow::onAddDesktopAudio);

	/* 500ms 轮询一次设备列表/延迟/状态, 开销可忽略 */
	statusTimer_ = new QTimer(this);
	statusTimer_->setInterval(500);
	connect(statusTimer_, &QTimer::timeout, this,
		&MirrorWindow::refreshAll);
	statusTimer_->start();

	/* ---- 初始状态 ---- */
	rebuildDeviceCombos(); /* 先填好下拉 */
	loadSlotStates();     /* 再恢复上次的选择/启用/音量 */
	rebuildDeviceCombos(); /* 选择就位后重算"已被选用"置灰 */

	{
		const QSignalBlocker b1(autoStartCheck_);
		autoStartCheck_->setChecked(mirror::cfgAutoStart());
		const QSignalBlocker b2(autoOpenCheck_);
		autoOpenCheck_->setChecked(mirror::cfgAutoOpen());
	}

	refreshStatus();
}

MirrorWindow::~MirrorWindow() = default;

int MirrorWindow::slotIndexByWidget(QWidget *w) const
{
	for (int i = 0; i < slots_.size(); ++i) {
		const SlotRows &s = slots_[i];
		if (s.enable == w || s.device == w || s.volume == w)
			return i;
	}
	return -1;
}

void MirrorWindow::loadSlotStates()
{
	const QVector<MirrorSlot> cfg = mirror::cfgSlots();
	QStringList seen; /* 配置异常时去重: 同一设备只允许出现在一个位 */

	for (int i = 0; i < slots_.size(); ++i) {
		const MirrorSlot s = (i < cfg.size()) ? cfg[i] : MirrorSlot{};
		QString dev = s.device;
		if (!dev.isEmpty() && seen.contains(dev))
			dev.clear();
		if (!dev.isEmpty())
			seen.append(dev);

		{
			const QSignalBlocker b(slots_[i].enable);
			slots_[i].enable->setChecked(s.enabled && !dev.isEmpty());
		}
		{
			const QSignalBlocker b(slots_[i].device);
			const int idx = slots_[i].device->findData(dev);
			slots_[i].device->setCurrentIndex(idx >= 0 ? idx : 0);
		}
		{
			const QSignalBlocker b(slots_[i].volume);
			slots_[i].volume->setValue(
				(int)(mirror::cfgDeviceVolume(dev) + 0.5));
		}
	}
}

void MirrorWindow::rebuildDeviceCombos()
{
	const auto devs = mirror::enumDevices();
	const QString noDevice =
		QString::fromUtf8(obs_module_text("AudioMirror.Window.NoDevice"));
	const QString suffixDefault = QString::fromUtf8(
		obs_module_text("AudioMirror.Window.DefaultDevice"));
	const QString suffixUsed = QString::fromUtf8(
		obs_module_text("AudioMirror.Window.UsedDevice"));

	for (int i = 0; i < slots_.size(); ++i) {
		/* 其它位已选中的设备(本位自己的不算) */
		QStringList usedElsewhere;
		for (int j = 0; j < slots_.size(); ++j) {
			if (j == i)
				continue;
			const QString d =
				slots_[j].device->currentData().toString();
			if (!d.isEmpty())
				usedElsewhere.append(d);
		}

		const QString keep =
			slots_[i].device->currentData().toString();

		QSignalBlocker b(slots_[i].device);
		slots_[i].device->clear();
		slots_[i].device->addItem(noDevice, QString());

		for (const auto &d : devs) {
			QString text = d.name;
			if (d.isDefault)
				text += suffixDefault;
			else if (usedElsewhere.contains(d.id))
				text += suffixUsed;

			slots_[i].device->addItem(text, d.id);

			/* 默认播放设备 / 已被别的位选用: 置灰不可选 */
			if (d.isDefault || usedElsewhere.contains(d.id)) {
				if (auto *m = qobject_cast<QStandardItemModel *>(
					    slots_[i].device->model()))
					m->item(slots_[i].device->count() - 1)
						->setEnabled(false);
			}
		}

		/* 恢复本位选择; 设备已不存在则回到占位项并停用该位 */
		int sel = 0;
		if (!keep.isEmpty()) {
			sel = slots_[i].device->findData(keep);
			if (sel < 0) {
				sel = 0;
				if (slots_[i].enable->isChecked()) {
					const QSignalBlocker be(slots_[i].enable);
					slots_[i].enable->setChecked(false);
				}
			}
		}
		slots_[i].device->setCurrentIndex(sel);
	}
}

void MirrorWindow::applyNow()
{
	/* 收集启用的位 */
	QStringList ids;
	for (const auto &s : slots_) {
		if (!s.enable->isChecked())
			continue;
		const QString dev = s.device->currentData().toString();
		if (!dev.isEmpty())
			ids.append(dev);
	}

	const QString err = mirror::applySelection(ids);
	if (!err.isEmpty()) {
		statusLabel_->setText(
			QString::fromUtf8(
				obs_module_text("AudioMirror.Window.Status.Error")) +
			QStringLiteral(": ") + err);
	} else {
		refreshStatus();
	}

	/* 持久化: 启用状态 + 设备选择(音量在拖动时已单独落盘) */
	QVector<MirrorSlot> cfg;
	cfg.reserve(slots_.size());
	for (const auto &s : slots_) {
		MirrorSlot ms;
		ms.enabled = s.enable->isChecked();
		ms.device = s.device->currentData().toString();
		ms.volume = 100.0;
		cfg.append(ms);
	}
	mirror::cfgSetSlots(cfg);
}

void MirrorWindow::refreshStatus()
{
	/* 桌面音频源缺失 -> 显示补救按钮 */
	const bool hasSource = mirror::ensureDesktopAudioSource(false);
	addDesktopBtn_->setVisible(!hasSource);

	/* 状态摘要 */
	int enabledCount = 0;
	bool anyDevice = false;
	for (const auto &s : slots_) {
		const QString dev = s.device->currentData().toString();
		if (!dev.isEmpty())
			anyDevice = true;
		if (s.enable->isChecked() && !dev.isEmpty())
			++enabledCount;
	}

	if (mirror::isActive()) {
		statusLabel_->setText(QString::fromUtf8(obs_module_text(
					 "AudioMirror.Window.Status.Running"))
					      .arg(enabledCount));
	} else if (!anyDevice) {
		statusLabel_->setText(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.Status.NoDevice")));
	} else {
		statusLabel_->setText(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.Status.Idle")));
	}

	/* 每设备实时延迟 */
	const bool running = mirror::isActive();
	for (const auto &s : slots_) {
		const QString dev = s.device->currentData().toString();
		const bool on = running && s.enable->isChecked() && !dev.isEmpty();
		if (on) {
			const double ms = mirror::deviceLatencyMs(dev);
			s.latency->setText(ms >= 0.0
						   ? QStringLiteral("%1 ms")
							     .arg(ms, 0, 'f', 1)
						   : QString());
		} else {
			s.latency->clear();
		}
	}
}

void MirrorWindow::refreshAll()
{
	/* 设备热插拔: 列表变了才重建下拉(保留各位选择) */
	const auto devs = mirror::enumDevices();
	bool same = devs.size() == last_devs_.size();
	if (same) {
		for (int i = 0; i < devs.size(); ++i) {
			if (devs[i].id != last_devs_[i].id ||
			    devs[i].name != last_devs_[i].name ||
			    devs[i].isDefault != last_devs_[i].isDefault) {
				same = false;
				break;
			}
		}
	}
	if (!same) {
		last_devs_ = devs;
		rebuildDeviceCombos();
	}

	refreshStatus();
}

/* ================= 槽函数 ================= */

void MirrorWindow::onSlotEnableToggled()
{
	auto *cb = qobject_cast<QCheckBox *>(sender());
	if (!cb || slotIndexByWidget(cb) < 0)
		return;
	applyNow();
}

void MirrorWindow::onSlotDeviceChanged()
{
	auto *cmb = qobject_cast<QComboBox *>(sender());
	const int i = cmb ? slotIndexByWidget(cmb) : -1;
	if (i < 0)
		return;

	const QString dev = cmb->currentData().toString();

	/* 选了设备 = 自动启用该位(傻瓜版语义: 选一个就监听一个);
	 * 选回"未选择" = 停用该位。 */
	const bool shouldEnable = !dev.isEmpty();
	if (slots_[i].enable->isChecked() != shouldEnable) {
		const QSignalBlocker b(slots_[i].enable);
		slots_[i].enable->setChecked(shouldEnable);
	}

	/* 音量滑块恢复该设备的存档值 */
	{
		const QSignalBlocker b(slots_[i].volume);
		slots_[i].volume->setValue(
			(int)(mirror::cfgDeviceVolume(dev) + 0.5));
	}

	/* 重算各位的"已被选用"置灰, 然后立即生效 */
	rebuildDeviceCombos();
	applyNow();
}

void MirrorWindow::onSlotVolumeChanged()
{
	auto *sl = qobject_cast<QSlider *>(sender());
	const int i = sl ? slotIndexByWidget(sl) : -1;
	if (i < 0)
		return;
	const QString dev = slots_[i].device->currentData().toString();
	if (dev.isEmpty())
		return;
	const double v = sl->value();
	mirror::setDeviceVolume(dev, v);
	mirror::cfgSetDeviceVolume(dev, v);
}

void MirrorWindow::onAutoStartToggled(bool checked)
{
	mirror::cfgSetAutoStart(checked);
}

void MirrorWindow::onAutoOpenToggled(bool checked)
{
	mirror::cfgSetAutoOpen(checked);
}

void MirrorWindow::onAddDesktopAudio()
{
	if (mirror::ensureDesktopAudioSource(true)) {
		addDesktopBtn_->setVisible(false);
		rebuildDeviceCombos();
		statusLabel_->setText(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.Status.SourceAdded")));
	} else {
		statusLabel_->setText(QString::fromUtf8(
			obs_module_text("AudioMirror.Window.Status.SourceFailed")));
	}
}
