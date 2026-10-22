/*
 * mirror-core.cpp - AudioMirror 插件核心实现
 *
 * 平台: Windows (WASAPI); 非 Windows 平台编译通过但功能不可用。
 * 依赖: libobs + obs-frontend-api + Qt6 Core
 *
 * 音频手法(与 obs-capcast 同源, 已在生产验证):
 *   1. 订阅: obs_add_raw_audio_callback(mix_idx, &conv, cb, param)
 *      - 每个输出设备注册一次, param = 该设备的渲染器;
 *      - conv 填目标设备的采样率/声道/float, 重采样与声道映射
 *        全部由 libobs 音频引擎完成(它内部按 mix 维护回调列表,
 *        同 mix 可挂多个回调, 见 media-io/audio-io.c);
 *      - mix_idx = 音轨 - 1, 0 = 音轨 1 = 最终混音成品。
 *   2. 输出: WASAPI shared mode, 严格按 GetMixFormat() 的格式;
 *      100ms 缓冲只作抖动余量, 实际延迟由"自适应积压控制"钉在
 *      一个 OBS 音频块的量级(~21ms @48kHz), 设备时钟漂移每次
 *      最多丢 1ms, 听感无感但延迟永不累积。
 *   3. 全程不改 OBS 混音器/监听设备设置, 不用 audio_monitor。
 *
 * 线程模型与锁纪律(崩溃安全的关键, 改动前必读):
 *   - writeAudio() 在 OBS 音频线程执行: libobs 的音频线程在调用
 *     我们的回调时持有 audio_output 的 input_mutex(见 audio-io.c
 *     的 do_audio_output), 而 audio_output_disconnect 也持同一把锁。
 *     => "先摘回调再停设备"可保证回调不会再执行, 此后销毁渲染器安全。
 *   - 但因此有 AB-BA 死锁风险: 音频线程持 input_mutex 等渲染器 mtx,
 *     若 UI 线程持渲染器 mtx 再去 obs_remove_raw_audio_callback(等
 *     input_mutex), 两线程互锁。
 *     => 纪律: 绝不在持有 WasapiRenderer::mtx_ 时调用
 *        obs_add/remove_raw_audio_callback。调用顺序永远是
 *        "摘回调(不持锁) -> stop()(持锁)"。
 *   - 引擎列表锁 g_engine_mtx 只被 UI 线程持有, 音频线程不碰,
 *     故持该锁调用 connect/disconnect 是安全的。
 */
#include "mirror-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>
#include <media-io/audio-io.h>

#include <QStringList>
#include <QVector>

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

	/* 打开设备并 Start。失败时内部已清理, 返回 false。
	 * 注意: 本函数不注册 raw audio callback(调用方在返回成功后注册)。 */
	bool start(size_t mixIdx);
	void stop();

	/* OBS 音频线程调用: 写入一栏"已是本设备格式"的交错 float32 */
	void writeAudio(const float *src, uint32_t frames);

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
	size_t mixIdx() const { return mix_idx_; }
	void setMixIdx(size_t idx) { mix_idx_ = idx; }

	/* 注册回调时传给 libobs 的转换参数(libobs 内部会拷贝) */
	const struct audio_convert_info *conv() const { return &conv_; }

	/* 当前积压(毫秒) = 该路实际监听延迟; 未运行返回 -1 */
	double latencyMs() const;

	/* 形如 "48000 Hz · 24 bit float" */
	QString formatDesc() const;

private:
	void shutdownLocked();
	void convertFloatToDevice(const float *src, uint8_t *dst, uint32_t frames,
				  float gain);

	mutable std::recursive_mutex mtx_;
	std::atomic<bool> active_{false};
	std::atomic<double> volume_pct_{100.0};

	QString device_id_;
	size_t mix_idx_ = 0;

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
	struct audio_convert_info conv_{};

	std::vector<uint8_t> scratch_;
};

bool WasapiRenderer::start(size_t mixIdx)
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
	mix_idx_ = mixIdx;

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

	/* 100ms 缓冲仅作抖动余量(headroom): 实际延迟由下面的积压控制
	 * 决定, 不把缓冲本身开成 1 秒(旧实现那样延迟会爆炸)。 */
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

	/* 让 libobs 按本设备的采样率/声道把混音转成交错 float32,
	 * 我们只做最后一次 float -> 设备位深。 */
	conv_ = {};
	conv_.samples_per_sec = sample_rate_;
	conv_.format = AUDIO_FORMAT_FLOAT;
	conv_.speakers = layout_from_channels(channels_);
	conv_.allow_clipping = true;

	active_.store(true, std::memory_order_relaxed);
	AM_LOG(LOG_INFO, "%s: 已开始 (%uHz %uch %ubit%s, 缓冲 %u 帧)",
	       qUtf8Printable(device_id_), sample_rate_, channels_, dev_bits_,
	       dev_float_ ? " float" : "", buffer_frames_);
	return true;
}

void WasapiRenderer::stop()
{
	std::lock_guard<std::recursive_mutex> lk(mtx_);
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
	scratch_.clear();
	scratch_.shrink_to_fit();
	active_.store(false, std::memory_order_relaxed);
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

	/* 4) 写入设备环形缓冲(不阻塞 OBS 音频线程) */
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
std::atomic<int> g_track{1};

/* raw audio callback: param = WasapiRenderer*。
 * 在 OBS 音频线程执行, libobs 此时持有 audio_output 的 input_mutex。 */
void on_mix_audio(void *param, size_t mix_idx, struct audio_data *audio)
{
	(void)mix_idx;
	auto *r = static_cast<WasapiRenderer *>(param);
	if (!r || !audio || audio->frames == 0 || !audio->data[0])
		return;
	/* conv 已让 libobs 输出"本设备采样率 + 本设备声道 + 交错 float32" */
	r->writeAudio(reinterpret_cast<const float *>(audio->data[0]),
		      audio->frames);
}

QString deviceNameById(const QVector<MirrorDevice> &devs, const QString &id)
{
	for (const auto &d : devs)
		if (d.id == id)
			return d.name;
	return id;
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

QString mirror::applySelection(const QStringList &deviceIds, int track)
{
#ifdef _WIN32
	if (track < 1)
		track = 1;
	if ((size_t)track > MAX_AUDIO_MIXES)
		track = (int)MAX_AUDIO_MIXES;

	/* 1. 归一化: 去空、去重、剔除默认播放设备(加倍音量无意义) */
	const auto devs = enumDevices();
	QStringList ids;
	for (const QString &id : deviceIds) {
		if (id.isEmpty() || ids.contains(id))
			continue;
		for (const auto &d : devs) {
			if (d.id == id) {
				if (d.isDefault)
					AM_LOG(LOG_WARNING,
					       "跳过默认播放设备(声音已在此播放): %s",
					       qUtf8Printable(d.name));
				else
					ids.append(id);
				break;
			}
		}
	}

	std::lock_guard<std::mutex> lk(g_engine_mtx);

	/* 2. 移除取消勾选的设备。
	 *    锁纪律见文件头: 先摘回调(不持渲染器 mtx), 再停设备。 */
	for (auto it = g_renderers.begin(); it != g_renderers.end();) {
		if (ids.contains((*it)->deviceId())) {
			++it;
			continue;
		}
		obs_remove_raw_audio_callback((*it)->mixIdx(), on_mix_audio,
					      it->get());
		(*it)->stop();
		AM_LOG(LOG_INFO, "设备已移除: %s",
		       qUtf8Printable((*it)->deviceId()));
		it = g_renderers.erase(it);
	}

	/* 3. 音轨变化: 保留中的设备也要按新 mix 索引重新注册
	 *    (obs_remove 必须用注册时相同的 mix_idx 才能摘掉)。 */
	const bool trackChanged = (g_track.load(std::memory_order_relaxed) != track);
	if (trackChanged) {
		for (auto &r : g_renderers) {
			obs_remove_raw_audio_callback(r->mixIdx(), on_mix_audio,
						      r.get());
			r->setMixIdx((size_t)(track - 1));
			obs_add_raw_audio_callback(r->mixIdx(), r->conv(),
						   on_mix_audio, r.get());
		}
		g_track.store(track, std::memory_order_relaxed);
		AM_LOG(LOG_INFO, "订阅音轨切换到 %d", track);
	}

	/* 4. 新增勾选的设备 */
	for (const QString &id : ids) {
		if (findRenderer(id))
			continue;

		auto r = std::make_unique<WasapiRenderer>(id);
		if (!r->start((size_t)(track - 1)))
			return QStringLiteral("无法打开音频设备：%1")
				.arg(deviceNameById(devs, id));

		r->setVolume(cfgDeviceVolume(id));
		obs_add_raw_audio_callback(r->mixIdx(), r->conv(), on_mix_audio,
					   r.get());
		AM_LOG(LOG_INFO, "设备已添加: %s (音轨 %d)",
		       qUtf8Printable(id), track);
		g_renderers.push_back(std::move(r));
	}

	cfgSetDevices(ids);
	cfgSetTrack(track);
	return {};
#else
	Q_UNUSED(deviceIds)
	Q_UNUSED(track)
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
	for (auto &r : g_renderers)
		obs_remove_raw_audio_callback(r->mixIdx(), on_mix_audio, r.get());
	for (auto &r : g_renderers)
		r->stop();
	g_renderers.clear();
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
	config_set_default_string(c, CFG_SECTION, "Devices", "");
	config_set_default_int(c, CFG_SECTION, "Track", 1);
	config_set_default_bool(c, CFG_SECTION, "AutoStart", true);
}

QStringList mirror::cfgDevices()
{
	config_t *c = user_config();
	if (!c)
		return {};
	cfg_init_defaults();
	const QString raw = QString::fromUtf8(
		config_get_string(c, CFG_SECTION, "Devices"));
	if (raw.isEmpty())
		return {};
	return raw.split(QChar('\n'), Qt::SkipEmptyParts);
}

void mirror::cfgSetDevices(const QStringList &ids)
{
	config_t *c = user_config();
	if (!c)
		return;
	config_set_string(c, CFG_SECTION, "Devices",
			  ids.join(QChar('\n')).toUtf8().constData());
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

int mirror::cfgTrack()
{
	config_t *c = user_config();
	if (!c)
		return 1;
	cfg_init_defaults();
	int t = (int)config_get_int(c, CFG_SECTION, "Track");
	if (t < 1)
		t = 1;
	if ((size_t)t > MAX_AUDIO_MIXES)
		t = (int)MAX_AUDIO_MIXES;
	return t;
}

void mirror::cfgSetTrack(int track)
{
	config_t *c = user_config();
	if (!c)
		return;
	config_set_int(c, CFG_SECTION, "Track", track);
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

/* ================= 傻瓜版: 桌面音频保障 ================= */

bool mirror::ensureDesktopAudioSource(bool createIfMissing)
{
#ifdef _WIN32
	/* 1. 已有一个在采"桌面音频(系统声音)"的源? */
	struct FindCtx {
		bool found = false;
	};
	FindCtx ctx;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *c = static_cast<FindCtx *>(param);
			const char *id = obs_source_get_id(source);
			if (id && strcmp(id, "wasapi_output_capture") == 0 &&
			    obs_source_active(source)) {
				c->found = true;
				return false; /* 找到即止 */
			}
			return true;
		},
		&ctx);

	if (ctx.found)
		return true;

	if (!createIfMissing)
		return false;

	/* 2. 没有 -> 创建一个加进当前场景(加入场景即激活采集) */
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource) {
		AM_LOG(LOG_WARNING, "桌面音频保障: 拿不到当前场景");
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		AM_LOG(LOG_WARNING, "桌面音频保障: 当前源不是场景");
		return false;
	}

	/* 场景里已有同名 item(用户手动加过又禁用) -> 直接认为就绪 */
	if (obs_scene_find_source(scene, "AudioMirror 桌面音频")) {
		obs_source_release(sceneSource);
		return true;
	}

	obs_source_t *src = obs_source_create("wasapi_output_capture",
					      "AudioMirror 桌面音频",
					      nullptr, nullptr);
	if (!src) {
		obs_source_release(sceneSource);
		AM_LOG(LOG_ERROR, "桌面音频保障: 源创建失败");
		return false;
	}

	/* obs_scene_add 内部自取引用, 这里释放我们自己的 */
	obs_scene_add(scene, src);
	obs_source_release(src);
	obs_source_release(sceneSource);
	AM_LOG(LOG_INFO, "桌面音频保障: 已自动创建并加入当前场景");
	return true;
#else
	Q_UNUSED(createIfMissing)
	return false;
#endif
}
