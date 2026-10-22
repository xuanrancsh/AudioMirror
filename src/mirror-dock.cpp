/*
 * mirror-dock.cpp - AudioMirror 监听面板实现
 */
#include "mirror-dock.hpp"
#include "mirror-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSignalBlocker>

namespace {

/* 面板内边距/间距与 OBS 原生 dock 观感一致 */
constexpr int kMargin = 10;
constexpr int kSpacing = 6;

} // namespace

MirrorDock::MirrorDock(QWidget *parent) : QWidget(parent)
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
	mainLayout->setSpacing(kSpacing);

	/* ---- 标题 ---- */
	auto *header = new QLabel(obs_module_text("AudioMirror.Dock.Header"),
				  this);
	header->setWordWrap(true);
	mainLayout->addWidget(header);

	/* ---- 设备列表(可滚动, 设备多也不挤) ---- */
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *rowsHost = new QWidget(scroll);
	rowsLayout_ = new QVBoxLayout(rowsHost);
	rowsLayout_->setContentsMargins(0, 0, 0, 0);
	rowsLayout_->setSpacing(4);
	rowsLayout_->addStretch(1);
	scroll->setWidget(rowsHost);
	mainLayout->addWidget(scroll, 1);

	/* ---- 全选/全不选 ---- */
	auto *btnRow = new QHBoxLayout;
	btnRow->setSpacing(kSpacing);
	selectAllBtn_ = new QPushButton(
		obs_module_text("AudioMirror.Dock.SelectAll"), this);
	selectNoneBtn_ = new QPushButton(
		obs_module_text("AudioMirror.Dock.SelectNone"), this);
	btnRow->addWidget(selectAllBtn_);
	btnRow->addWidget(selectNoneBtn_);
	btnRow->addStretch(1);
	mainLayout->addLayout(btnRow);

	/* ---- 音轨 + 自动开始 ---- */
	auto *optRow = new QHBoxLayout;
	optRow->setSpacing(kSpacing);
	optRow->addWidget(new QLabel(
		obs_module_text("AudioMirror.Dock.Track"), this));
	trackCombo_ = new QComboBox(this);
	for (int t = 1; t <= 6; ++t)
		trackCombo_->addItem(QString::number(t), t);
	trackCombo_->setToolTip(
		QString::fromUtf8("只送勾选了这条音轨的源。1 = OBS 最终混音成品"));
	trackCombo_->setMaximumWidth(72);
	optRow->addWidget(trackCombo_);
	optRow->addStretch(1);
	mainLayout->addLayout(optRow);

	autoStartCheck_ = new QCheckBox(
		obs_module_text("AudioMirror.Dock.AutoStart"), this);
	autoStartCheck_->setToolTip(QString::fromUtf8(
		"勾选后 OBS 启动时按上次勾选的设备自动开始监听"));
	mainLayout->addWidget(autoStartCheck_);

	/* ---- 桌面音频源缺失时的补救按钮 ---- */
	addDesktopBtn_ = new QPushButton(
		obs_module_text("AudioMirror.Dock.AddSource"), this);
	addDesktopBtn_->setToolTip(QString::fromUtf8(
		"OBS 里没有采集系统声音的源, 点此自动创建一个并加入当前场景"));
	addDesktopBtn_->setVisible(false);
	mainLayout->addWidget(addDesktopBtn_);

	/* ---- 状态 + 提示 ---- */
	statusLabel_ = new QLabel(this);
	statusLabel_->setWordWrap(true);
	mainLayout->addWidget(statusLabel_);

	tipLabel_ = new QLabel(obs_module_text("AudioMirror.Dock.Tip"), this);
	tipLabel_->setWordWrap(true);
	tipLabel_->setEnabled(false); /* 弱化为提示文本 */
	mainLayout->addWidget(tipLabel_);

	mainLayout->addStretch(1);

	/* ---- 信号 ---- */
	connect(selectAllBtn_, &QPushButton::clicked, this,
		&MirrorDock::onSelectAll);
	connect(selectNoneBtn_, &QPushButton::clicked, this,
		&MirrorDock::onSelectNone);
	connect(trackCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &MirrorDock::onTrackChanged);
	connect(autoStartCheck_, &QCheckBox::toggled, this,
		&MirrorDock::onAutoStartToggled);
	connect(addDesktopBtn_, &QPushButton::clicked, this,
		&MirrorDock::onAddDesktopAudio);

	/* 500ms 轮询一次延迟/状态, 开销可忽略 */
	statusTimer_ = new QTimer(this);
	statusTimer_->setInterval(500);
	connect(statusTimer_, &QTimer::timeout, this,
		&MirrorDock::refreshStatus);
	statusTimer_->start();

	/* 首次填充 */
	trackCombo_->blockSignals(true);
	{
		const int track = mirror::cfgTrack();
		const int row = trackCombo_->findData(track);
		trackCombo_->setCurrentIndex(row >= 0 ? row : 0);
	}
	trackCombo_->blockSignals(false);
	autoStartCheck_->blockSignals(true);
	autoStartCheck_->setChecked(mirror::cfgAutoStart());
	autoStartCheck_->blockSignals(false);

	refreshDevices();
	refreshStatus();

	setMinimumWidth(300);
}

MirrorDock::~MirrorDock() = default;

void MirrorDock::refreshDevices()
{
	/* 记住当前勾选, 重建后恢复(热插拔后设备可能已变) */
	const QStringList keep = checkedIds();

	/* 清掉旧行(保留末尾 stretch) */
	while (rowsLayout_->count() > 1) {
		QLayoutItem *item = rowsLayout_->takeAt(0);
		if (item) {
			delete item->widget();
			delete item;
		}
	}
	rows_.clear();

	const auto devs = mirror::enumDevices();
	for (const auto &d : devs) {
		DeviceRow row;
		row.id = d.id;

		auto *host = new QWidget(this);
		auto *grid = new QGridLayout(host);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setHorizontalSpacing(6);
		grid->setVerticalSpacing(2);

		row.check = new QCheckBox(d.name, host);
		if (d.isDefault) {
			/* 防呆: 默认播放设备不可勾选 —— 声音已经在这里播放,
			 * 再监听一份只会音量加倍。 */
			row.check->setEnabled(false);
			row.check->setToolTip(QString::fromUtf8(
				"这是电脑当前的默认播放设备, 声音已经在这里播放, 不需要再监听"));
		}
		grid->addWidget(row.check, 0, 0, 1, 2);

		row.info = new QLabel(host);
		row.info->setEnabled(false);
		grid->addWidget(row.info, 1, 0);

		row.latency = new QLabel(host);
		row.latency->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		grid->addWidget(row.latency, 1, 1);

		row.volume = new QSlider(Qt::Horizontal, host);
		row.volume->setRange(0, 200);
		row.volume->setValue((int)(mirror::cfgDeviceVolume(d.id) + 0.5));
		row.volume->setToolTip(QString::fromUtf8(
			"只影响送到这个设备的音量, 不改 OBS 混音器"));
		grid->addWidget(row.volume, 2, 0, 1, 2);

		host->setLayout(grid);
		rowsLayout_->insertWidget(rowsLayout_->count() - 1, host);

		connect(row.check, &QCheckBox::toggled, this,
			&MirrorDock::onDeviceToggled);
		connect(row.volume, &QSlider::valueChanged, this,
			&MirrorDock::onVolumeChanged);

		/* 恢复勾选: 配置里有且设备可选。
		 * 用 QSignalBlocker 屏蔽 toggled 信号 —— 恢复的是"上次的
		 * 选择状态", 不是用户本次操作; 是否真的开始路由由
		 * 「自动开始」开关或用户手动勾选决定。 */
		if (!d.isDefault && keep.contains(d.id)) {
			const QSignalBlocker blocker(row.check);
			row.check->setChecked(true);
		}

		rows_.append(row);
	}
}

void MirrorDock::refreshStatus()
{
	/* 桌面音频源缺失 -> 显示补救按钮 */
	const bool hasSource = mirror::ensureDesktopAudioSource(false);
	addDesktopBtn_->setVisible(!hasSource);

	/* 状态摘要 */
	if (mirror::isActive()) {
		const QStringList ids = checkedIds();
		statusLabel_->setText(
			QString(obs_module_text("AudioMirror.Dock.Status.Running"))
				.arg(ids.size()));
	} else {
		statusLabel_->setText(
			rows_.isEmpty()
				? obs_module_text("AudioMirror.Dock.Status.NoDevice")
				: obs_module_text("AudioMirror.Dock.Status.Idle"));
	}

	/* 每设备: 格式 + 实时延迟 */
	for (const auto &row : rows_) {
		if (row.check->isChecked() && mirror::isActive()) {
			const double ms = mirror::deviceLatencyMs(row.id);
			if (ms >= 0.0)
				row.latency->setText(
					QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
			else
				row.latency->clear();
			const QString fmt = mirror::deviceFormat(row.id);
			if (!fmt.isEmpty())
				row.info->setText(fmt);
		} else {
			row.latency->clear();
			row.info->clear();
			if (!row.check->isEnabled())
				row.info->setText(
					obs_module_text("AudioMirror.Dock.Default"));
		}
	}
}

QStringList MirrorDock::checkedIds() const
{
	QStringList ids;
	for (const auto &row : rows_) {
		if (row.check && row.check->isChecked())
			ids.append(row.id);
	}
	return ids;
}

void MirrorDock::applyNow()
{
	const int track = trackCombo_->currentData().toInt();
	const QString err = mirror::applySelection(checkedIds(), track);
	if (!err.isEmpty())
		statusLabel_->setText(
			obs_module_text("AudioMirror.Dock.Status.Error") +
			QStringLiteral(": ") + err);
	refreshStatus();
}

void MirrorDock::onDeviceToggled()
{
	applyNow();
}

void MirrorDock::onVolumeChanged()
{
	auto *slider = qobject_cast<QSlider *>(sender());
	if (!slider)
		return;
	for (const auto &row : rows_) {
		if (row.volume == slider) {
			const double v = slider->value();
			mirror::setDeviceVolume(row.id, v);
			mirror::cfgSetDeviceVolume(row.id, v);
			return;
		}
	}
}

void MirrorDock::onSelectAll()
{
	for (const auto &row : rows_) {
		if (row.check->isEnabled()) {
			const QSignalBlocker blocker(row.check);
			row.check->setChecked(true);
		}
	}
	applyNow();
}

void MirrorDock::onSelectNone()
{
	for (const auto &row : rows_) {
		if (row.check->isEnabled()) {
			const QSignalBlocker blocker(row.check);
			row.check->setChecked(false);
		}
	}
	applyNow();
}

void MirrorDock::onTrackChanged(int index)
{
	Q_UNUSED(index)
	applyNow();
}

void MirrorDock::onAutoStartToggled(bool checked)
{
	mirror::cfgSetAutoStart(checked);
}

void MirrorDock::onAddDesktopAudio()
{
	if (mirror::ensureDesktopAudioSource(true)) {
		addDesktopBtn_->setVisible(false);
		statusLabel_->setText(
			obs_module_text("AudioMirror.Dock.Status.SourceAdded"));
	} else {
		statusLabel_->setText(
			obs_module_text("AudioMirror.Dock.Status.SourceFailed"));
	}
}
