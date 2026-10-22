/*
 * mirror-core.cpp - AudioMirror 插件核心实现
 *
 * 平台: Windows (WASAPI); 非 Windows 平台编译通过但功能不可用。
 * 依赖: libobs + obs-frontend-api + Qt6 Core
 *
 * 音频手法(v0.2.0 起改为按源采集, 与 obs-capcast 同源的积压控制):
 *   1. 采集: obs_source_add_audio_capture_callback(source, cb, param)
 *      - 只订"桌面音频类"源: wasapi_output_capture(环回) /
 *        wasapi_process_output_capture(应用音频);
 *      - 麦克风源(wasapi_input_capture)从机制上被排除,
 *        用户不需要去关 OBS 的麦克风;
 *      - 每个输出设备注册一次, param = 该设备的渲染器,
 *        增删单个设备互不影响;
 *      - 回调拿到的是源输出格式的平面 float(不随 OBS 混音器的静音/
 *        隐藏/音量变化: 那三者只影响 OBS 自己的混音与监听, 采集数据
 *        仍是真实音频), win-wasapi 采集源的格式 = 所采设备的 mix format。
 *   2. 重采样: 每设备一个 libobs audio_resampler_t
 *      (源格式 -> 该设备 mix format, float), 平面 -> 交错。
 *   3. 输出: WASAPI shared mode, 严格按 GetMixFormat() 的格式;
 *      100ms 缓冲只作抖动余量, 实际延迟由"自适应积压控制"钉在
 *      一个 OBS 音频块的量级(~21ms @48kHz), 设备时钟漂移每次
 *      最多丢 1ms, 听感无感但延迟永不累积。
 *   4. 全程不改 OBS 混音器/音轨/监听设备设置, 不用 audio_monitor。
 *
 * 线程模型与锁纪律(崩溃安全的关键, 改动前必读):
 *   - writeCaptured() 在桌面音频源的音频线程执行: libobs 调用我们的
 *     回调时持有该源的 audio_cb_mutex(见 obs-source.c 的
 *     source_signal_audio_data), 而
 *     obs_source_remove_audio_capture_callback 也持同一把锁。
 *     => "先摘回调再停设备"可保证回调不会再执行, 此后销毁渲染器安全。
 *   - 但因此有 AB-BA 死锁风险: 音频线程持 audio_cb_mutex 等渲染器 mtx,
 *     若 UI 线程持渲染器 mtx 再去 obs_remove_audio_capture_callback(等
 *     audio_cb_mutex), 两线程互锁。
 *     => 纪律: 绝不在持有 WasapiRenderer::mtx_ 时调用
 *        obs_add/remove_audio_capture_callback。调用顺序永远是
 *        "摘回调(不持锁) -> stop()(持锁)"。
 *   - 引擎列表锁 g_engine_mtx 只被 UI 线程持有, 音频线程不碰,
 *     故持该锁调用 add/remove callback 是安全的。
 *   - g_cap_source 持有采集源的引用: 既防止用户在监听中删掉源导致
 *     悬垂指针, 也保证 remove 时源一定还活着。
 */
#include "mirror-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <media-io/audio-io.h>
#include <media-io/audio-resampler.h>

#include <QStringList>
#include <QVector>

#include <cstring>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <propidl.h>
#endif

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#define AM_LOG(level, fmt, ...) blog(level, "[AudioMirror] " fmt, ##__VA_ARGS__)

namespace {

constexpr const char *CFG_SECTION = "AudioMirror";

/* 固定 5 个监听位 */
constexpr int kSlotCount = 5;

config_t *user_config()
{
	return obs_frontend_get_user_config();
}

#ifdef _WIN32

/* ---- 本地 GUID 定义(避免依赖 uuid.lib 符号解析, obs-capcast 同款) ---- */
const GUID k_clsid_mmdevice_enumerator = {
	0xBCDE0395, 0xE52F, 0x467C,
	{0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
const GUID k_subtype_ieee_float = {
	0x00000003, 0x0000, 0x0010,
	{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

QString wide_to_qstring(const wchar_t *w)
{
	return w ? QString::fromWCharArray(w) : QString();
}

bool wave_is_float(const WAVEFORMATEX *wf)
{
	if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
		const WAVEFORMATEXTENSIBLE *wfe =
			reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
		return IsEqualGUID(wfe->SubFormat, k_subtype_ieee_float) != FALSE;
	}
	return false;
}

/* 设备声道数 -> OBS speaker 布局 */
enum speaker_layout layout_from_channels(UINT32 n)
{
	switch (n) {
	case 1:
		return SPEAKERS_MONO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_STEREO;
	}
}

/* ================= 单设备 WASAPI 渲染器 ================= */

class WasapiRenderer {
public:
	explicit WasapiRenderer(QString deviceId) : device_id_(std::move(deviceId)) {}
	~WasapiRenderer() { stop(); }

	WasapiRenderer(const WasapiRenderer &) = delete;
	WasapiRenderer &operator=(const WasapiRenderer &) = delete;

	/* 打开设备并 Start; src = 桌面音频源的格式(用于建重采样器)。
	 * 失败时内部已清理, 返回 false。
	 * 注意: 本函数不注册采集回调(调用方在返回成功后注册)。 */
	bool start(const struct resample_info &src);
	void stop();

	/* 源格式变化时重建重采样器(设备必须已打开) */
	bool setSourceFormat(const struct resample_info &src);

	/* 桌面音频源线程调用: 平面 float(源格式) -> 重采样 -> 交错 ->
	 * float -> 设备位深 -> 自适应积压控制写出。 */
	void writeCaptured(const struct audio_data *audio);

	void setVolume(double percent)
	{
		volume_pct_.store(percent, std::memory_order_relaxed);
	}
	double volume() const
	{
		return volume_pct_.load(std::memory_order_relaxed);
	}

	bool active() const { return active_.load(std::memory_order_relaxed); }
	const QString &deviceId() const { return device_id_; }

	/* 无音频输入诊断用时间戳(纳秒); 详见上面两个成员 */
	uint64_t startedNs() const
	{
		return started_ns_.load(std::memory_order_relaxed);
	}
	uint64_t lastPacketNs() const
	{
		return last_packet_ns_.load(std::memory_order_relaxed);
	}

	/* 当前积压(毫秒) = 该路实际监听延迟; 未运行返回 -1 */
	double latencyMs() const;

	/* 形如 "48000 Hz · 24 bit float" */
	QString formatDesc() const;

private:
	void shutdownLocked();
	void convertFloatToDevice(const float *src, uint8_t *dst, uint32_t frames,
				  float gain);
	void writeAudio(const float *src, uint32_t frames);

	mutable std::recursive_mutex mtx_;
	std::atomic<bool> active_{false};
	std::atomic<double> volume_pct_{100.0};

	/* 无音频输入诊断时间戳(os_gettime_ns 单调时钟, 纳秒):
	 * started_ns_ = start() 成功时刻;
	 * last_packet_ns_ = 最近一次真正收到桌面音频包的时刻(0 = 未收到)。 */
	std::atomic<uint64_t> started_ns_{0};
	std::atomic<uint64_t> last_packet_ns_{0};

	QString device_id_;

	IMMDevice *device_ = nullptr;
	IAudioClient *client_ = nullptr;
	IAudioRenderClient *render_ = nullptr;
	UINT32 buffer_frames_ = 0;
	UINT32 channels_ = 2;
	UINT32 sample_rate_ = 48000;
	UINT32 dev_bits_ = 32;
	bool dev_float_ = true;
	UINT32 frame_bytes_ = 8;
	UINT32 obs_block_ = 0;

	/* 重采样器: 源格式 -> 本设备 mix format(float) */
	audio_resampler_t *resampler_ = nullptr;
	struct resample_info dst_info_{};

	/* 重采样输出(平面) -> 交错 的中间缓冲 */
	std::vector<float> interleave_;
	/* float -> 设备原始格式 的转换缓冲 */
	std::vector<uint8_t> scratch_;
};

bool WasapiRenderer::start(const struct resample_info &src)
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);

	/* COM 已在 OBS 的线程上初始化过时忽略错误继续(与 capcast 相同);
	 * 此处刻意不做 CoUninitialize —— 本对象的 COM 接口寿命超过本函数,
	 * 而线程属于 OBS/Qt, 由它们管理 COM 生命周期。 */
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	(void)hrInit;

	IMMDeviceEnumerator *enumerator = nullptr;
	HRESULT hr = CoCreateInstance(k_clsid_mmdevice_enumerator, nullptr,
				      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	if (FAILED(hr) || !enumerator) {
		AM_LOG(LOG_ERROR, "%s: 无法创建设备枚举器",
		       qUtf8Printable(device_id_));
		return false;
	}

	const std::wstring wide_id = device_id_.toStdWString();
	hr = enumerator->GetDevice(wide_id.c_str(), &device_);
	enumerator->Release();
	if (FAILED(hr) || !device_) {
		AM_LOG(LOG_ERROR, "设备不存在或已拔出: %s",
		       qUtf8Printable(device_id_));
		device_ = nullptr;
		return false;
	}

	hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
			       (void **)&client_);
	if (FAILED(hr) || !client_) {
		AM_LOG(LOG_ERROR, "%s: IAudioClient 激活失败",
		       qUtf8Printable(device_id_));
		shutdownLocked();
		return false;
	}

	/* shared mode 必须严格使用 GetMixFormat() 返回的格式 */
	WAVEFORMATEX *wfex = nullptr;
	hr = client_->GetMixFormat(&wfex);
	if (FAILED(hr) || !wfex) {
		AM_LOG(LOG_ERROR, "%s: GetMixFormat 失败",
		       qUtf8Printable(device_id_));
		shutdownLocked();
		return false;
	}

	if (wfex->nChannels < 1 || wfex->nChannels > MAX_AUDIO_CHANNELS) {
		AM_LOG(LOG_ERROR, "%s: 不支持的声道数 %u",
		       qUtf8Printable(device_id_), wfex->nChannels);
		CoTaskMemFree(wfex);
		shutdownLocked();
		return false;
	}

	sample_rate_ = wfex->nSamplesPerSec;
	channels_ = wfex->nChannels;
	dev_float_ = wave_is_float(wfex);
	if (dev_float_) {
		dev_bits_ = 32;
	} else {
		switch (wfex->wBitsPerSample) {
		case 8:
			dev_bits_ = 8;
			break;
		case 24:
			dev_bits_ = 24;
			break;
		case 32:
			dev_bits_ = 32;
			break;
		default:
			dev_bits_ = 16;
			break;
		}
	}
	frame_bytes_ = channels_ * (dev_bits_ / 8);

	/* 100ms 缓冲仅作抖动余量(headroom): 实际延迟由积压控制决定,
	 * 不把缓冲本身开成 1 秒(旧实现那样延迟会爆炸)。 */
	hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /* 100ms */,
				 0, wfex, nullptr);
	CoTaskMemFree(wfex);
	if (FAILED(hr)) {
		AM_LOG(LOG_ERROR, "%s: Initialize 失败 (0x%08lX)",
		       qUtf8Printable(device_id_), hr);
		shutdownLocked();
		return false;
	}

	if (FAILED(client_->GetBufferSize(&buffer_frames_))) {
		shutdownLocked();
		return false;
	}
	if (FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
		shutdownLocked();
		return false;
	}
	if (FAILED(client_->Start())) {
		AM_LOG(LOG_ERROR, "%s: Start 失败", qUtf8Printable(device_id_));
		shutdownLocked();
		return false;
	}

	/* 建重采样器: 桌面音频源格式 -> 本设备 mix format(float)。
	 * audio_resampler_resample 内部自管输出缓冲, 我们只做最后一次
	 * float -> 设备位深。 */
	dst_info_ = {};
	dst_info_.samples_per_sec = sample_rate_;
	dst_info_.format = AUDIO_FORMAT_FLOAT;
	dst_info_.speakers = layout_from_channels(channels_);

	if (resampler_)
		audio_resampler_destroy(resampler_);
	resampler_ = audio_resampler_create(&dst_info_, &src);
	if (!resampler_) {
		AM_LOG(LOG_ERROR, "%s: 重采样器创建失败", qUtf8Printable(device_id_));
		shutdownLocked();
		return false;
	}

	active_.store(true, std::memory_order_relaxed);
	last_packet_ns_.store(0, std::memory_order_relaxed);
	started_ns_.store(os_gettime_ns(), std::memory_order_relaxed);
	AM_LOG(LOG_INFO, "%s: 已开始 (%uHz %uch %ubit%s, 缓冲 %u 帧)",
	       qUtf8Printable(device_id_), sample_rate_, channels_, dev_bits_,
	       dev_float_ ? " float" : "", buffer_frames_);
	return true;
}

bool WasapiRenderer::setSourceFormat(const struct resample_info &src)
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	if (!client_ || sample_rate_ == 0)
		return false;

	audio_resampler_t *rs = audio_resampler_create(&dst_info_, &src);
	if (!rs)
		return false;
	if (resampler_)
		audio_resampler_destroy(resampler_);
	resampler_ = rs;
	return true;
}

void WasapiRenderer::stop()
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	if (resampler_) {
		audio_resampler_destroy(resampler_);
		resampler_ = nullptr;
	}
	shutdownLocked();
}

void WasapiRenderer::shutdownLocked()
{
	if (render_) {
		render_->Release();
		render_ = nullptr;
	}
	if (client_) {
		client_->Release();
		client_ = nullptr;
	}
	if (device_) {
		device_->Release();
		device_ = nullptr;
	}
	buffer_frames_ = 0;
	obs_block_ = 0;
	interleave_.clear();
	interleave_.shrink_to_fit();
	scratch_.clear();
	scratch_.shrink_to_fit();
	active_.store(false, std::memory_order_relaxed);
	started_ns_.store(0, std::memory_order_relaxed);
	last_packet_ns_.store(0, std::memory_order_relaxed);
}

/* 交错 float32 -> 设备原始格式, 同时应用本设备音量增益 */
void WasapiRenderer::convertFloatToDevice(const float *src, uint8_t *dst,
					  uint32_t frames, float gain)
{
	const UINT32 total = frames * channels_;

	if (dev_float_) {
		float *p = reinterpret_cast<float *>(dst);
		if (gain == 1.f) { /* 快速路径 */
			memcpy(dst, src, (size_t)total * sizeof(float));
			return;
		}
		for (UINT32 i = 0; i < total; ++i) {
			const float v = src[i] * gain;
			p[i] = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
		}
		return;
	}

	switch (dev_bits_) {
	case 8: { /* unsigned 8bit */
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			dst[i] = (uint8_t)(int)((v + 1.f) * 127.5f);
		}
		break;
	}
	case 16: {
		int16_t *p = reinterpret_cast<int16_t *>(dst);
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			p[i] = (int16_t)(v * 32767.f);
		}
		break;
	}
	case 24: {
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			const int32_t s = (int32_t)(v * 8388607.f);
			uint8_t *p = dst + (size_t)i * 3;
			p[0] = (uint8_t)(s & 0xFF);
			p[1] = (uint8_t)((s >> 8) & 0xFF);
			p[2] = (uint8_t)((s >> 16) & 0xFF);
		}
		break;
	}
	default: { /* 32bit int */
		int32_t *p = reinterpret_cast<int32_t *>(dst);
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			p[i] = (int32_t)(v * 2147483647.f);
		}
		break;
	}
	}
}

void WasapiRenderer::writeAudio(const float *src, uint32_t frames)
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	if (!active_.load(std::memory_order_relaxed) || !render_ || !src ||
	    frames == 0)
		return;

	/* 1) float -> 设备原始格式(同时应用本设备音量) */
	const float gain =
		(float)(volume_pct_.load(std::memory_order_relaxed) / 100.0);
	const size_t need = (size_t)frames * channels_ * (dev_bits_ / 8);
	if (scratch_.size() < need)
		scratch_.resize(need);
	convertFloatToDevice(src, scratch_.data(), frames, gain);
	const uint8_t *data = scratch_.data();

	/* 2) 目标积压 = 一个 OBS 音频块(~1024 帧)。
	 *    把实际延迟钉在"一块"的量级, 而不是整套缓冲。 */
	if (obs_block_ == 0)
		obs_block_ = frames;
	const UINT32 target = obs_block_ < buffer_frames_ ? obs_block_
							 : buffer_frames_ / 2;

	UINT32 pad = 0;
	if (FAILED(client_->GetCurrentPadding(&pad)))
		return;

	/* 3) 自适应: 设备时钟与 OBS 时钟有漂移时积压会缓慢上涨,
	 *    每次最多丢 1ms —— 听感无感, 但延迟不会越积越多。
	 *    (OBS 内置监听正是缺这个控制, 才有"越跑越延迟"的顽疾) */
	if (pad > target) {
		UINT32 excess = pad - target;
		const UINT32 max_adj = sample_rate_ / 1000; /* 1ms */
		if (excess > max_adj)
			excess = max_adj;
		if (excess >= frames)
			return;
		data += (size_t)excess * frame_bytes_;
		frames -= excess;
	}

	/* 4) 写入设备环形缓冲(不阻塞音频线程) */
	UINT32 remaining = frames;
	while (remaining > 0) {
		if (FAILED(client_->GetCurrentPadding(&pad)))
			return;
		const UINT32 avail = buffer_frames_ - pad;
		if (avail == 0)
			break; /* 缓冲满: 丢弃剩余, 绝不阻塞音频线程 */
		const UINT32 chunk = remaining < avail ? remaining : avail;

		BYTE *dst = nullptr;
		/* 取缓冲失败: 放弃本帧剩余部分。不能对未成功 GetBuffer 的
		 * 句柄调用 ReleaseBuffer。 */
		if (FAILED(render_->GetBuffer(chunk, &dst)))
			return;
		memcpy(dst, data, (size_t)chunk * frame_bytes_);
		data += (size_t)chunk * frame_bytes_;
		remaining -= chunk;
		render_->ReleaseBuffer(chunk, 0);
	}
}

void WasapiRenderer::writeCaptured(const struct audio_data *audio)
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	if (!active_.load(std::memory_order_relaxed) || !resampler_ ||
	    !render_ || !audio || audio->frames == 0 || !audio->data[0])
		return;

	/* 诊断: 记下最近一次真正收到包的时刻(无音频输入判定用) */
	last_packet_ns_.store(os_gettime_ns(), std::memory_order_relaxed);

	/* 1) 源格式(平面 float) -> 本设备格式(平面 float)。
	 *    audio_resampler_resample 内部自管输出缓冲, 会在 output[]
	 *    里填好各声道的指针; ts_offset 是它的出参, 忽略即可。 */
	uint8_t *planes[MAX_AUDIO_CHANNELS] = {};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	if (!audio_resampler_resample(resampler_, planes, &out_frames,
				      &ts_offset, audio->data, audio->frames))
		return;
	if (out_frames == 0 || !planes[0])
		return;

	/* 2) 平面 -> 交错 */
	const size_t need = (size_t)out_frames * channels_;
	if (interleave_.size() < need)
		interleave_.resize(need);
	float *dst = interleave_.data();
	for (uint32_t f = 0; f < out_frames; ++f) {
		float *row = dst + (size_t)f * channels_;
		for (UINT32 c = 0; c < channels_; ++c)
			row[c] = reinterpret_cast<const float *>(planes[c])[f];
	}

	/* 3) 交给既有的设备格式转换 + 自适应积压控制 */
	writeAudio(dst, out_frames);
}

double WasapiRenderer::latencyMs() const
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	if (!active_.load(std::memory_order_relaxed) || !client_ ||
	    sample_rate_ == 0)
		return -1.0;
	UINT32 pad = 0;
	if (FAILED(client_->GetCurrentPadding(&pad)))
		return -1.0;
	return (double)pad * 1000.0 / (double)sample_rate_;
}

QString WasapiRenderer::formatDesc() const
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	if (!active_.load(std::memory_order_relaxed))
		return {};
	return QStringLiteral("%1 Hz · %2 bit%3")
		.arg(sample_rate_)
		.arg(dev_bits_)
		.arg(dev_float_ ? QStringLiteral(" float")
			       : QString());
}

/* ================= 引擎状态 ================= */

/* 只保护 renderers 列表本身; 音频线程只经 param 指针访问自己的
 * 渲染器, 不碰本列表。 */
std::mutex g_engine_mtx;
std::vector<std::unique_ptr<WasapiRenderer>> g_renderers;

/* 采集回调的目标源。持有引用: 防止监听中源被删导致悬垂指针,
 * 也保证 remove 时源一定还活着。 */
obs_source_t *g_cap_source = nullptr;
/* 采集源的音频格式(所有渲染器共用)与"身份戳"(源名+设备),
 * 变了就要按新格式重建全部重采样器。 */
struct resample_info g_src_info{};
bool g_has_src = false;
QString g_src_token;

/* 采集回调: param = WasapiRenderer*。
 * 在桌面音频源的音频线程执行, libobs 此时持有源的 audio_cb_mutex。 */
void on_source_audio(void *param, obs_source_t *source, const struct audio_data *audio,
		     bool muted)
{
	(void)source;
	/* libobs 的 muted 只表示该源在 OBS 混音器里被静音/隐藏
	 * (obs-source.c source_muted: !enabled || user_muted / push_to_mute),
	 * 音频数据本身仍是真实音频 —— 静音只影响 OBS 自己的混音与监听。
	 * 本插件的勾选 + 音量滑条才是路由意图, 因此一律转发。 */
	if (muted) {
		static std::atomic<bool> warned{false};
		if (!warned.exchange(true))
			AM_LOG(LOG_WARNING,
			       "桌面音频源在 OBS 中被静音/隐藏, 仍继续转发");
	}
	auto *r = static_cast<WasapiRenderer *>(param);
	if (!r || !r->active())
		return;
	r->writeCaptured(audio);
}

/* 桌面音频类源? 环回 + 应用音频(麦克风/线路输入不在内) */
bool is_desktop_audio_id(const char *id)
{
	return id && (strcmp(id, "wasapi_output_capture") == 0 ||
		      strcmp(id, "wasapi_process_output_capture") == 0);
}

bool is_loopback_id(const char *id)
{
	return id && strcmp(id, "wasapi_output_capture") == 0;
}

/* 找优先级最高的桌面音频源。
 * 返回持有引用的 obs_source_t*(调用方负责 release), 找不到返回 null。
 * 优先级: ① 默认设备的环回 ② 任意环回 ③ 应用音频。
 * 注意: obs_enum_all_sources 枚举期间只持 sources_mutex 且不持引用,
 * 因此枚举回调里只取引用, 读源设置(持源自己的锁)放到枚举结束后做,
 * 避免任何锁序风险。 */
obs_source_t *find_desktop_audio_source()
{
	struct Ctx {
		std::vector<obs_source_t *> candidates; /* 持有引用 */
	} ctx;

	obs_enum_all_sources(
		[](void *param, obs_source_t *src) {
			auto *c = static_cast<Ctx *>(param);
			if (is_desktop_audio_id(obs_source_get_id(src)))
				c->candidates.push_back(obs_source_get_ref(src));
			return true;
		},
		&ctx);

	obs_source_t *best = nullptr;
	int best_rank = 99;
	for (obs_source_t *src : ctx.candidates) {
		int rank = is_loopback_id(obs_source_get_id(src)) ? 2 : 3;
		if (rank == 2) {
			obs_data_t *s = obs_source_get_settings(src);
			const char *dev =
				s ? obs_data_get_string(s, "device_id") : nullptr;
			if (dev && _stricmp(dev, "default") == 0)
				rank = 1;
			if (s)
				obs_data_release(s);
		}
		if (rank < best_rank) {
			if (best)
				obs_source_release(best);
			best = src; /* 移交引用 */
		} else {
			obs_source_release(src);
		}
	}
	return best; /* 调用方负责 release */
}

/* 取桌面音频源的音频格式。
 * win-wasapi 采集源的格式 = 所采设备的 mix format(float);
 * 源设置的 device_id 为 "default"/空 时取系统默认渲染端点。
 * 查询失败回退到 OBS 全局音频设置。
 * resolved_id(可选): 实际解析到的端点 ID —— 默认设备被更换时,
 * 用它做"身份戳"判断重采样器是否需要按新格式重建。 */
bool query_source_format(obs_source_t *source, struct resample_info &out,
			 QString *resolved_id = nullptr)
{
	bool ok = false;
	std::wstring device_id;
	bool use_default = true;

	obs_data_t *s = obs_source_get_settings(source);
	if (s) {
		const char *dev = obs_data_get_string(s, "device_id");
		if (dev && *dev && _stricmp(dev, "default") != 0) {
			device_id = QString::fromUtf8(dev).toStdWString();
			use_default = false;
		}
		obs_data_release(s);
	}

	/* COM 可能已在调用线程初始化过; 失败(已初始化)时继续用。 */
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool com_owned = SUCCEEDED(hrInit);

	IMMDeviceEnumerator *enumerator = nullptr;
	HRESULT hr = CoCreateInstance(k_clsid_mmdevice_enumerator, nullptr,
				      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	if (SUCCEEDED(hr) && enumerator) {
		IMMDevice *dev = nullptr;
		bool got_dev = false;
		if (use_default) {
			got_dev = SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
					    eRender, eConsole, &dev)) &&
				  dev;
		} else {
			got_dev = SUCCEEDED(enumerator->GetDevice(
					    device_id.c_str(), &dev)) &&
				  dev;
		}
		if (got_dev) {
			if (resolved_id) {
				LPWSTR wId = nullptr;
				if (SUCCEEDED(dev->GetId(&wId)) && wId) {
					*resolved_id = wide_to_qstring(wId);
					CoTaskMemFree(wId);
				}
			}
			IAudioClient *client = nullptr;
			if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient),
						    CLSCTX_ALL, nullptr,
						    (void **)&client)) &&
			    client) {
				WAVEFORMATEX *wfex = nullptr;
				if (SUCCEEDED(client->GetMixFormat(&wfex)) &&
				    wfex) {
					out.samples_per_sec = wfex->nSamplesPerSec;
					out.speakers = layout_from_channels(
						wfex->nChannels);
					out.format = AUDIO_FORMAT_FLOAT;
					CoTaskMemFree(wfex);
					ok = true;
				}
				client->Release();
			}
			dev->Release();
		}
		enumerator->Release();
	}

	if (com_owned)
		CoUninitialize();

	if (!ok) {
		/* 回退: OBS 全局音频设置(默认 48kHz 立体声 float) */
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai)) {
			out.samples_per_sec = oai.samples_per_sec;
			out.speakers = oai.speakers;
			out.format = AUDIO_FORMAT_FLOAT;
			ok = true;
		}
	}
	return ok;
}

/* 保障桌面音频采集源(详见头文件注释) */
bool ensure_desktop_audio_source(bool create_if_missing)
{
	struct Ctx {
		bool has_active = false;
		obs_source_t *any = nullptr; /* 持有引用的第一个环回源 */
	} ctx;

	obs_enum_all_sources(
		[](void *param, obs_source_t *src) {
			auto *c = static_cast<Ctx *>(param);
			if (!is_loopback_id(obs_source_get_id(src)))
				return true;
			if (obs_source_active(src)) {
				c->has_active = true;
				return false; /* 找到即止 */
			}
			if (!c->any)
				c->any = obs_source_get_ref(src);
			return true;
		},
		&ctx);

	if (ctx.has_active) {
		if (ctx.any)
			obs_source_release(ctx.any);
		return true;
	}

	obs_source_t *to_add = ctx.any;
	if (!to_add) {
		if (!create_if_missing)
			return false;
		to_add = obs_source_create("wasapi_output_capture",
					   "AudioMirror 桌面音频", nullptr,
					   nullptr);
		if (!to_add) {
			AM_LOG(LOG_ERROR, "桌面音频保障: 源创建失败");
			return false;
		}
	}

	/* 加进当前场景(OBS 的音频源只在所在场景激活时才产出音频)。
	 * 先用 obs_sceneitem_get_source 精确判断是否已在场景里。 */
	bool added = false;
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (scene_source) {
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		if (scene) {
			struct PresentCtx {
				obs_source_t *target;
				bool present = false;
			} pctx{to_add};

			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item,
				   void *p) {
					auto *pc = static_cast<PresentCtx *>(p);
					if (obs_sceneitem_get_source(item) ==
					    pc->target)
						pc->present = true;
					return true;
				},
				&pctx);

			if (!pctx.present) {
				/* obs_scene_add 内部自取引用 */
				obs_scene_add(scene, to_add);
				AM_LOG(LOG_INFO,
				       "桌面音频保障: 已加入当前场景");
			}
			added = true;
		} else {
			AM_LOG(LOG_WARNING, "桌面音频保障: 当前源不是场景");
		}
		obs_source_release(scene_source);
	} else {
		AM_LOG(LOG_WARNING, "桌面音频保障: 拿不到当前场景");
	}

	obs_source_release(to_add);
	return added;
}

/* 按 id 找运行中的渲染器; 调用方需持有 g_engine_mtx */
WasapiRenderer *findRenderer(const QString &id)
{
	for (auto &r : g_renderers)
		if (r->deviceId() == id)
			return r.get();
	return nullptr;
}

#endif /* _WIN32 */

} // namespace

/* ================= 设备枚举 ================= */

QVector<MirrorDevice> mirror::enumDevices()
{
	QVector<MirrorDevice> out;
#ifdef _WIN32
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
		return out;
	const bool comOwned = SUCCEEDED(hrInit);

	IMMDeviceEnumerator *enumerator = nullptr;
	HRESULT hr = CoCreateInstance(k_clsid_mmdevice_enumerator, nullptr,
				      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	if (SUCCEEDED(hr) && enumerator) {
		/* 默认播放设备: 声音已经在这里播放, 不需要(也不应该)
		 * 再监听一份, 否则音量加倍。 */
		QString defaultId;
		IMMDevice *def = nullptr;
		if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
			    eRender, eConsole, &def)) &&
		    def) {
			LPWSTR wId = nullptr;
			if (SUCCEEDED(def->GetId(&wId)) && wId) {
				defaultId = wide_to_qstring(wId);
				CoTaskMemFree(wId);
			}
			def->Release();
		}

		IMMDeviceCollection *coll = nullptr;
		if (SUCCEEDED(enumerator->EnumAudioEndpoints(
			    eRender, DEVICE_STATE_ACTIVE, &coll)) &&
		    coll) {
			UINT count = 0;
			coll->GetCount(&count);
			for (UINT i = 0; i < count; ++i) {
				IMMDevice *dev = nullptr;
				if (FAILED(coll->Item(i, &dev)) || !dev)
					continue;

				MirrorDevice md;
				LPWSTR wId = nullptr;
				if (SUCCEEDED(dev->GetId(&wId)) && wId) {
					md.id = wide_to_qstring(wId);
					CoTaskMemFree(wId);
				}

				IPropertyStore *props = nullptr;
				if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ,
								    &props)) &&
				    props) {
					PROPVARIANT varName;
					PropVariantInit(&varName);
					if (SUCCEEDED(props->GetValue(
						    PKEY_Device_FriendlyName,
						    &varName)) &&
					    varName.vt == VT_LPWSTR) {
						md.name = wide_to_qstring(
							varName.pwszVal);
					}
					PropVariantClear(&varName);
					props->Release();
				}
				dev->Release();

				if (md.name.isEmpty() || md.id.isEmpty())
					continue;
				md.isDefault = (md.id == defaultId);
				out.append(md);
			}
			coll->Release();
		}
		enumerator->Release();
	}

	if (comOwned)
		CoUninitialize();
#endif
	return out;
}

/* ================= 运行控制 ================= */

bool mirror::isActive()
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_engine_mtx);
	return !g_renderers.empty();
#else
	return false;
#endif
}

QString mirror::applySelection(const QStringList &deviceIds)
{
#ifdef _WIN32
	/* 1. 保障桌面音频源(没有就自动创建) */
	if (!ensure_desktop_audio_source(true))
		return QStringLiteral(
			"没有可用的桌面音频采集源，请在 OBS 里添加「桌面音频」");

	/* 2. 找采集源并确定它的音频格式 */
	obs_source_t *cap = find_desktop_audio_source();
	if (!cap)
		return QStringLiteral("找不到桌面音频采集源");

	struct resample_info src_info{};
	QString resolved_id;
	if (!query_source_format(cap, src_info, &resolved_id)) {
		obs_source_release(cap);
		return QStringLiteral("无法确定桌面音频的音频格式");
	}

	/* 身份戳: 源名 + 设置里的设备 + 实际解析到的端点。
	 * 用户换源/改设备/默认设备更换后要按新格式重建。 */
	QString token = QString::fromUtf8(obs_source_get_name(cap));
	{
		obs_data_t *s = obs_source_get_settings(cap);
		if (s) {
			token += QLatin1Char('|');
			token += QString::fromUtf8(
				obs_data_get_string(s, "device_id"));
			obs_data_release(s);
		}
	}
	token += QLatin1Char('|') + resolved_id;

	/* 3. 归一化目标设备: 去空/去重/剔除默认播放设备/只认已知设备 */
	const auto devs = enumDevices();
	QStringList ids;
	for (const QString &id : deviceIds) {
		if (id.isEmpty() || ids.contains(id))
			continue;
		bool known = false;
		bool is_def = false;
		for (const auto &d : devs) {
			if (d.id == id) {
				known = true;
				is_def = d.isDefault;
				break;
			}
		}
		if (!known)
			continue;
		if (is_def) {
			AM_LOG(LOG_WARNING,
			       "跳过默认播放设备(声音已在此播放): %s",
			       qUtf8Printable(id));
			continue;
		}
		ids.append(id);
	}
	if (ids.size() > kSlotCount)
		ids = ids.mid(0, kSlotCount);

	std::lock_guard<std::mutex> lk(g_engine_mtx);

	/* 4. 采集源变化: 摘旧源上的全部回调 -> 停全部设备 -> 换源。
	 *    锁纪律见文件头: 摘回调时不持渲染器 mtx。 */
	const bool src_changed =
		!g_has_src || (g_src_token != token) || (g_cap_source != cap);
	if (src_changed) {
		for (auto &r : g_renderers) {
			if (g_cap_source)
				obs_source_remove_audio_capture_callback(
					g_cap_source, on_source_audio, r.get());
		}
		for (auto &r : g_renderers)
			r->stop();
		g_renderers.clear();
		if (g_cap_source == cap) {
			obs_source_release(cap); /* 同一源: 释放多余引用 */
		} else {
			if (g_cap_source)
				obs_source_release(g_cap_source);
			g_cap_source = cap; /* 移交 cap 的引用 */
		}
		g_src_info = src_info;
		g_has_src = true;
		g_src_token = token;
		AM_LOG(LOG_INFO, "采集源: %s", qUtf8Printable(token));
	} else {
		obs_source_release(cap); /* 已有同一源的引用 */
	}
	cap = nullptr;

	/* 5. 移除取消勾选的设备 */
	for (auto it = g_renderers.begin(); it != g_renderers.end();) {
		if (ids.contains((*it)->deviceId())) {
			++it;
			continue;
		}
		obs_source_remove_audio_capture_callback(g_cap_source,
							on_source_audio,
							it->get());
		(*it)->stop();
		AM_LOG(LOG_INFO, "设备已移除: %s",
		       qUtf8Printable((*it)->deviceId()));
		it = g_renderers.erase(it);
	}

	/* 6. 新增勾选的设备 */
	for (const QString &id : ids) {
		if (findRenderer(id))
			continue;

		auto r = std::make_unique<WasapiRenderer>(id);
		r->setVolume(cfgDeviceVolume(id));
		if (!r->start(g_src_info)) {
			AM_LOG(LOG_ERROR, "无法打开音频设备: %s",
			       qUtf8Printable(id));
			continue; /* 单设备失败不拖累其它设备 */
		}
		obs_source_add_audio_capture_callback(g_cap_source,
						     on_source_audio, r.get());
		AM_LOG(LOG_INFO, "设备已添加: %s", qUtf8Printable(id));
		g_renderers.push_back(std::move(r));
	}

	return {};
#else
	Q_UNUSED(deviceIds)
	return QStringLiteral("当前仅支持 Windows");
#endif
}

void mirror::setDeviceVolume(const QString &deviceId, double percent)
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_engine_mtx);
	if (WasapiRenderer *r = findRenderer(deviceId))
		r->setVolume(percent);
#else
	Q_UNUSED(deviceId)
	Q_UNUSED(percent)
#endif
}

double mirror::deviceLatencyMs(const QString &deviceId)
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_engine_mtx);
	if (WasapiRenderer *r = findRenderer(deviceId))
		return r->latencyMs();
#else
	Q_UNUSED(deviceId)
#endif
	return -1.0;
}

bool mirror::deviceAudioStalled(const QString &deviceId)
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_engine_mtx);
	const WasapiRenderer *r = findRenderer(deviceId);
	if (!r || !r->active())
		return false;
	const uint64_t now = os_gettime_ns();
	/* 已收到过包: 看最近一包; 还没收到: 看启动时刻(2s 宽容) */
	const uint64_t last =
		r->lastPacketNs() ? r->lastPacketNs() : r->startedNs();
	if (last == 0 || now <= last)
		return false;
	return now - last > 2000000000ull; /* 2 秒 */
#else
	Q_UNUSED(deviceId)
	return false;
#endif
}

QString mirror::deviceFormat(const QString &deviceId)
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_engine_mtx);
	if (WasapiRenderer *r = findRenderer(deviceId))
		return r->formatDesc();
#else
	Q_UNUSED(deviceId)
#endif
	return {};
}

void mirror::stopAll()
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_engine_mtx);
	/* 先摘全部回调, 再停设备(锁纪律同 applySelection) */
	for (auto &r : g_renderers) {
		if (g_cap_source)
			obs_source_remove_audio_capture_callback(
				g_cap_source, on_source_audio, r.get());
	}
	for (auto &r : g_renderers)
		r->stop();
	g_renderers.clear();
	if (g_cap_source) {
		obs_source_release(g_cap_source);
		g_cap_source = nullptr;
	}
	g_has_src = false;
	g_src_token.clear();
	AM_LOG(LOG_INFO, "已停止全部监听");
#endif
}

/* ================= 配置持久化 ================= */

/* OBS 31 起 config_get_* 不再接受默认值参数, 改用
 * config_set_default_* 注册默认值(重复注册无害)。
 * 每次读配置前先把默认值补上, 保证首次运行有合理默认。 */
static void cfg_init_defaults()
{
	config_t *c = user_config();
	if (!c)
		return;
	for (int i = 1; i <= kSlotCount; ++i) {
		const QByteArray p =
			QStringLiteral("Slot%1.").arg(i).toUtf8();
		config_set_default_bool(c, CFG_SECTION,
					(p + "Enabled").constData(), false);
		config_set_default_string(c, CFG_SECTION,
					  (p + "Device").constData(), "");
	}
	config_set_default_bool(c, CFG_SECTION, "AutoStart", true);
	config_set_default_bool(c, CFG_SECTION, "AutoOpen", true);
}

QVector<MirrorSlot> mirror::cfgSlots()
{
	QVector<MirrorSlot> out;
	config_t *c = user_config();
	if (!c) {
		out.resize(kSlotCount);
		return out;
	}
	cfg_init_defaults();
	for (int i = 1; i <= kSlotCount; ++i) {
		const QByteArray p = QStringLiteral("Slot%1.").arg(i).toUtf8();
		MirrorSlot s;
		s.enabled = config_get_bool(c, CFG_SECTION,
					    (p + "Enabled").constData());
		s.device = QString::fromUtf8(
			config_get_string(c, CFG_SECTION,
					  (p + "Device").constData()));
		s.volume = 100.0;
		out.append(s);
	}
	return out;
}

void mirror::cfgSetSlots(const QVector<MirrorSlot> &slotList)
{
	config_t *c = user_config();
	if (!c)
		return;
	cfg_init_defaults();
	for (int i = 0; i < kSlotCount && i < slotList.size(); ++i) {
		const QByteArray p =
			QStringLiteral("Slot%1.").arg(i + 1).toUtf8();
		config_set_bool(c, CFG_SECTION, (p + "Enabled").constData(),
				slotList[i].enabled);
		config_set_string(c, CFG_SECTION, (p + "Device").constData(),
				  slotList[i].device.toUtf8().constData());
	}
	config_save_safe(c, "tmp", "bak");
}

double mirror::cfgDeviceVolume(const QString &id)
{
	config_t *c = user_config();
	if (!c)
		return 100.0;
	const QByteArray key = ("Vol." + id).toUtf8();
	config_set_default_double(c, CFG_SECTION, key.constData(), 100.0);
	return config_get_double(c, CFG_SECTION, key.constData());
}

void mirror::cfgSetDeviceVolume(const QString &id, double percent)
{
	config_t *c = user_config();
	if (!c)
		return;
	const QByteArray key = ("Vol." + id).toUtf8();
	config_set_double(c, CFG_SECTION, key.constData(), percent);
	config_save_safe(c, "tmp", "bak");
}

bool mirror::cfgAutoStart()
{
	config_t *c = user_config();
	if (!c)
		return true;
	cfg_init_defaults();
	return config_get_bool(c, CFG_SECTION, "AutoStart");
}

void mirror::cfgSetAutoStart(bool v)
{
	config_t *c = user_config();
	if (!c)
		return;
	config_set_bool(c, CFG_SECTION, "AutoStart", v);
	config_save_safe(c, "tmp", "bak");
}

bool mirror::cfgAutoOpen()
{
	config_t *c = user_config();
	if (!c)
		return true;
	cfg_init_defaults();
	return config_get_bool(c, CFG_SECTION, "AutoOpen");
}

void mirror::cfgSetAutoOpen(bool v)
{
	config_t *c = user_config();
	if (!c)
		return;
	config_set_bool(c, CFG_SECTION, "AutoOpen", v);
	config_save_safe(c, "tmp", "bak");
}

/* ================= 傻瓜版: 桌面音频保障 ================= */

bool mirror::ensureDesktopAudioSource(bool createIfMissing)
{
#ifdef _WIN32
	return ensure_desktop_audio_source(createIfMissing);
#else
	Q_UNUSED(createIfMissing)
	return false;
#endif
}
