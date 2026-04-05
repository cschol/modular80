#include "modular80.hpp"

#include <thread>
#include <condition_variable>
#include <chrono>
#include <algorithm>

#include "osdialog.h"

#define DR_WAV_IMPLEMENTATION
#include "dep/dr_libs/dr_wav.h"

#define MAX_BANK_SIZE 2147483648l // 2GB max per bank (in memory!)
#define MAX_NUM_BANKS 16
#define MAX_DIR_DEPTH 1

#define PITCH_MODE_DEFAULT 0.5f
#define NORMAL_MODE_DEFAULT 0.0f


class FileScanner {

public:

FileScanner() :
  scanDepth(0),
  bankCount(0),
  banks()
  {}
~FileScanner() {};

void reset() {
	bankCount = 0;
	scanDepth = 0;
	banks.clear();
}

static bool isSupportedAudioFormat(const std::string& path) {
	// Need at least 4 characters for extension (.wav or .raw)
	static const size_t MIN_EXTENSION_LENGTH = 4;
	if (path.size() < MIN_EXTENSION_LENGTH) {
		return false;
	}

	std::string suffix = path.substr(path.size() - MIN_EXTENSION_LENGTH);
	std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

	return (suffix == ".wav" || suffix == ".raw");
}

void scan(const std::string& root, const bool sort = false, const bool filter = true) {

	std::vector<std::string> files;
	std::vector<std::string> entries;

	entries = system::getEntries(root);

	if (sort) {
        std::sort(entries.begin(), entries.end());
	}

	for (const std::string &entry : entries) {
		if (system::isDirectory(entry)) {
			if (string::startsWith(entry, "SPOTL") ||
			    string::startsWith(entry, "TRASH") ||
				string::startsWith(entry, "__MACOSX")) {
				continue;
			}

			if (bankCount > MAX_NUM_BANKS) {
				WARN("Max number of banks reached. Ignoring subdirectories.");
				return;
			}

			if (scanDepth++ > MAX_DIR_DEPTH) {
				WARN("Directory has too many subdirectories: %s", entry.c_str());
				continue;
			};

			scan(entry, sort, filter);

		} else {
			files.push_back(entry);
		}
	}

	if (filter) {
		files.erase(
			std::remove_if(files.begin(), files.end(),
				[](const std::string& f) { return !isSupportedAudioFormat(f); }),
			files.end()
		);
	}

	if (!files.empty()) {
		bankCount++;
		banks.push_back(files);
	}
	scanDepth--;
}

int scanDepth;
int bankCount;
std::vector<std::vector<std::string>> banks;

};


// Base class
class AudioObject {

public:

AudioObject() :
  filePath(),
  currentPos(0.0f),
  channels(0),
  sampleRate(0),
  bytesPerSample(2),
  totalSamples(0),
  samples(nullptr),
  peak(0.0f)
  {}

virtual ~AudioObject() {}

virtual bool load(const std::string &path) = 0;

std::string filePath;
float currentPos;
unsigned int channels;
unsigned int sampleRate;
unsigned int bytesPerSample;
drwav_uint64 totalSamples;
float *samples;
float peak;

};


class WavAudioObject : public AudioObject {

public:

WavAudioObject() : AudioObject() {
	bytesPerSample = 4;
}
~WavAudioObject() {
	if (samples) {
		drwav_free(samples, nullptr);
	}
}

bool load(const std::string &path) override {
	drwav_uint64 totalFrames(0);

	filePath = path;
	samples = drwav_open_file_and_read_pcm_frames_f32(
		filePath.c_str(), &channels, &sampleRate, &totalFrames, nullptr
	);

	totalSamples = totalFrames * channels;

	if (samples) {
		for (size_t i = 0; i < totalSamples; ++i) {
			float absSample = std::abs(samples[i]);
			if (absSample > peak) peak = absSample;
		}
	}

	return (samples != nullptr);
}
};


class RawAudioObject : public AudioObject {

public:

RawAudioObject() : AudioObject() {
	channels = 1;
	sampleRate = 44100;
	bytesPerSample = 2;
}
~RawAudioObject() {
	if (samples) {
		free(samples);
	}
}

bool load(const std::string &path) override {
	filePath = path;

	FILE *wav = fopen(filePath.c_str(), "rb");

	if (wav) {
		fseek(wav, 0, SEEK_END);
		const long fsize = ftell(wav);

		// Check for ftell errors and unreasonable file sizes
		if (fsize <= 0 || fsize > 2147483647L) { // Max 2GB for safety
			WARN("Invalid file size: %ld bytes for %s", fsize, filePath.c_str());
			fclose(wav);
			return false;
		}

		rewind(wav);

		int16_t *rawSamples = (int16_t*)malloc(sizeof(int16_t) * fsize/bytesPerSample);
		if (rawSamples) {
			const long samplesRead = fread(rawSamples, (size_t)sizeof(int16_t), fsize/bytesPerSample, wav);
			if (samplesRead != fsize/(long)bytesPerSample) { WARN("Failed to read entire file"); }

			// FIX: Allocate samples buffer BEFORE using it
			totalSamples = fsize/bytesPerSample;
			samples = (float*)malloc(sizeof(float) * totalSamples);

			if (samples) {
				for (size_t i = 0; i < totalSamples; ++i) {
					samples[i] = static_cast<float>(rawSamples[i]) / 32768.0f;
					if (std::abs(samples[i]) > peak) peak = std::abs(samples[i]);
				}
				free(rawSamples);
			} else {
				WARN("Failed to allocate memory for samples");
				free(rawSamples); // Free rawSamples before returning
				fclose(wav);
				return false;
			}
		} else {
			WARN("Failed to allocate memory for rawSamples");
			fclose(wav);
			return false;
		}

		fclose(wav);

	} else {
		WARN("Failed to load file: %s", filePath.c_str());
		return false;
	}

    return (samples != nullptr);
}

};


/** 4-point 3rd-order optimal interpolation (Watte tri-linear)
    Provides excellent quality with minimal computational overhead.
    Better frequency response and less aliasing than Hermite.
*/
inline float interpolateOptimal4Point(const float* samples, const unsigned long intPos, const float fracPos, const drwav_uint64 totalSamples) {
    if (samples == nullptr || intPos >= totalSamples) {
        return 0.0f;
    }

    // Ensure we have enough samples for interpolation
    if (intPos + 1 >= totalSamples) {
        return samples[intPos];
    }

    // Get the four samples
    float y0 = (intPos > 0) ? samples[intPos - 1] : samples[intPos];
    float y1 = samples[intPos];
    float y2 = (intPos + 1 < totalSamples) ? samples[intPos + 1] : samples[intPos];
    float y3 = (intPos + 2 < totalSamples) ? samples[intPos + 2] : samples[intPos + 1];

    // Optimal 4-point 3rd-order (Watte tri-linear)
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

    return ((c3 * fracPos + c2) * fracPos + c1) * fracPos + c0;
}

class AudioPlayer {

public:
AudioPlayer() :
  startPos(0.0f),
  playbackSpeed(1.0f)
{}
~AudioPlayer() {}

void load(std::shared_ptr<AudioObject> object) {
	audio = std::move(object);
}

void skipTo(float pos) {
	if (audio) {
		// Clamp position to valid range [0, totalSamples)
		if (pos < 0.0f) {
			audio->currentPos = 0.0f;
		} else if (pos >= audio->totalSamples) {
			audio->currentPos = audio->totalSamples > 0 ? audio->totalSamples - 1.0f : 0.0f;
		} else {
			audio->currentPos = pos;
		}
	}
}

float play(unsigned int channel) const {
	if (!audio) {
		return 0.0f;
	}

	if (channel >= audio->channels) {
		return 0.0f;
	}

	const float pos = audio->currentPos + channel;
	if (pos >= audio->totalSamples) {
		return 0.0f;
	}

	const unsigned long intPos = static_cast<unsigned long>(pos);
	const float fracPos = pos - intPos;

	// Only use interpolation if we have a fractional position (pitch mode)
	// In normal mode (1x speed), fracPos will be 0 and we can skip the expensive interpolation
	if (fracPos < 0.001f) {
		// Fast path: no interpolation needed
		return audio->samples[intPos];
	}

	// Slow path: use interpolation for pitch shifting
	return interpolateOptimal4Point(audio->samples, intPos, fracPos, audio->totalSamples);
}

void advance(bool repeat, bool pitchMode) {
	if (!audio) {
		return;
	}

	// Cache frequently accessed values
	const unsigned int channels = audio->channels;
	const float maxPos = static_cast<float>(audio->totalSamples);

	float nextPos;
	if (pitchMode) {
		nextPos = audio->currentPos + playbackSpeed * static_cast<float>(channels);
	} else {
		nextPos = audio->currentPos + channels;
	}

	if (nextPos >= maxPos) {
		if (repeat) {
			audio->currentPos = startPos;
		} else {
			audio->currentPos = maxPos;
		}
	} else {
		audio->currentPos = nextPos;
	}
}

void resetTo(float pos) {
	if (audio) {
		startPos = pos;
		audio->currentPos = startPos;
	}
}

bool ready() const {
	return audio && audio->totalSamples > 0;
}

void reset() {
	if (audio) {
		audio.reset();
	}
}

void setPlaybackSpeed(const float speed) {
	playbackSpeed = speed;
}

std::shared_ptr<AudioObject> object() const {
	return audio;
}

private:

std::shared_ptr<AudioObject> audio;
float startPos;
float playbackSpeed;

};


struct AudioObjectPool {
	unsigned long memoryUsage = 0;
	std::vector<std::shared_ptr<AudioObject>> objects;

	void clear() {
		objects.clear();
		memoryUsage = 0;
	}
};


struct RadioMusic : Module {
	// Synchronization and timing constants
	static constexpr float XFADE_RATE = 0.005f;      // ~25ms crossfade time
	static constexpr float FADEOUT_RATE = 0.05f;     // ~5ms fadeout time
	static constexpr int RESET_LED_FLASH_TIME_MS = 50; // Reset LED flash duration
	static constexpr float PITCH_RANGE = 8.0f;       // Pitch mode octave range

	enum ParamIds {
		STATION_PARAM,
		START_PARAM,
		RESET_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		STATION_INPUT,
		START_INPUT,
		RESET_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		RESET_LIGHT,
		LED_0_LIGHT,
		LED_1_LIGHT,
		LED_2_LIGHT,
		LED_3_LIGHT,
		NUM_LIGHTS
	};

	RadioMusic();
	~RadioMusic();

	void process(const ProcessArgs &args) override;
	void onReset() override;

	void clearCurrentBank();

	// Context menu
	std::atomic<bool> loadFiles{false};
	std::atomic<bool> scanFiles{false};
	std::atomic<bool> selectBank{false};

	// Settings
	bool stereoOutputMode;
	bool pitchMode;
	bool loopingEnabled;
	bool enableCrossfade;
	bool sortFiles;
	bool allowAllFiles;
	std::string rootDir;
	int currentBank;

	json_t *dataToJson() override;
	void dataFromJson(json_t *rootJ) override;

private:

	void init();
	void workerThread();
	void threadedScan();
	void threadedLoad();
	void resetCurrentPlayer(const float start);
	void updateResetLedState();
	void updateLoadingIndicator();
	void processAudioFrame(int frameIndex, dsp::Frame<2>& frame, const float start);

	AudioPlayer audioPlayer1;
	AudioPlayer audioPlayer2;

	AudioPlayer *currentPlayer;
	AudioPlayer *previousPlayer;

	AudioObjectPool audioContainer1;
	AudioObjectPool audioContainer2;
	AudioObjectPool* currentObjectPool;
	AudioObjectPool* tmpObjectPool;

	dsp::SchmittTrigger rstButtonTrigger;
	dsp::SchmittTrigger rstInputTrigger;

	int prevIndex;
	unsigned long tick;
	unsigned long elapsedMs;
	bool crossfade;
	bool fadeout;
	float fadeOutGain;
	float xfadeGain1;
	float xfadeGain2;
	bool flashResetLed;
	unsigned long ledTimerMs;

	// Reset LED flash state
	bool resetLedInitTimer = true;
	unsigned long resetLedTimerStart = 0;

	// Loading indicator state
	bool loadingIndicatorInitTimer = true;
	unsigned long loadingIndicatorTimerStart = 0;
	bool loadingIndicatorToggle = false;
	int loadingIndicatorBlinks = 0;

	// Cached start parameter for frame processing
	float currentStartParam = 0.0f;

	dsp::VuMeter2 vumeter;

	dsp::SampleRateConverter<2> outputSrc;
	dsp::DoubleRingBuffer<dsp::Frame<2>, 256> outputBuffer;

	FileScanner scanner;

	const int BLOCK_SIZE = 16;

	std::mutex mutex;
	std::condition_variable cond;
	std::shared_ptr<std::thread> worker;
	std::atomic<bool> stopWorker;
	std::atomic<bool> workerDoWork;

	std::atomic<bool> loadingFiles;
	std::atomic<bool> filesLoaded;
	std::atomic<bool> loadError;
	std::atomic<bool> abortLoad;
	std::atomic<bool> scanAudioFiles;
	std::atomic<bool> loadAudioFiles;
};

// Custom ParamQuantity to handle modal behavior of Start parameter
struct StartParamQuantity : ParamQuantity {

	float getDefaultValue() override {
		if (module) {
			rm = dynamic_cast<RadioMusic*>(module);
			if (rm) {
				return (rm->pitchMode) ? PITCH_MODE_DEFAULT : NORMAL_MODE_DEFAULT;
			}
		}
		return getValue();
	}

	std::string getLabel() override {
		if (module) {
			rm = dynamic_cast<RadioMusic*>(module);
			if (rm) {
				return (rm->pitchMode) ? "Pitch" : "Start";
			}
		}
		return "";
	}

	RadioMusic* rm = nullptr;
};


json_t *RadioMusic::dataToJson() {
	json_t *rootJ = json_object();

	// Option: Stereo Output Mode
	json_object_set_new(rootJ, "stereoOutputMode", json_boolean(stereoOutputMode));

	// Option: Pitch Mode
	json_object_set_new(rootJ, "pitchMode", json_boolean(pitchMode));

	// Option: Loop Samples
	json_object_set_new(rootJ, "loopingEnabled", json_boolean(loopingEnabled));

	// Option: Enable Crossfade
	json_object_set_new(rootJ, "enableCrossfade", json_boolean(enableCrossfade));

	// Option: Sort Files
	json_object_set_new(rootJ, "sortFiles", json_boolean(sortFiles));

	// Option: Allow All Files
	json_object_set_new(rootJ, "allowAllFiles", json_boolean(allowAllFiles));

	// Internal state: rootDir
	json_object_set_new(rootJ, "rootDir", json_string(rootDir.c_str()));

	// Internal state: currentBank
	json_object_set_new(rootJ, "currentBank", json_integer(currentBank));

	return rootJ;
}

void RadioMusic::dataFromJson(json_t *rootJ) {
	// Option: Stereo Output Mode
	json_t *stereoOutputModeJ = json_object_get(rootJ, "stereoOutputMode");
	if (stereoOutputModeJ) stereoOutputMode = json_boolean_value(stereoOutputModeJ);

	// Option: Pitch Mode
	json_t *pitchModeJ = json_object_get(rootJ, "pitchMode");
	if (pitchModeJ) pitchMode = json_boolean_value(pitchModeJ);

	// Option: Loop Samples
	json_t *loopingJ = json_object_get(rootJ, "loopingEnabled");
	if (loopingJ) loopingEnabled = json_boolean_value(loopingJ);

	// Option: Enable Crossfade
	json_t *crossfadeJ = json_object_get(rootJ, "enableCrossfade");
	if (crossfadeJ) enableCrossfade = json_boolean_value(crossfadeJ);

	// Option: Sort Files
	json_t *sortJ = json_object_get(rootJ, "sortFiles");
	if (sortJ) sortFiles = json_boolean_value(sortJ);

	// Option: Allow All Files
	json_t *filesJ = json_object_get(rootJ, "allowAllFiles");
	if (filesJ) allowAllFiles = json_boolean_value(filesJ);

	// Internal state: rootDir
	json_t *rootDirJ = json_object_get(rootJ, "rootDir");
	if (rootDirJ) rootDir = json_string_value(rootDirJ);

	// Internal state: currentBank
	json_t *bankJ = json_object_get(rootJ, "currentBank");
	if (bankJ) currentBank = json_integer_value(bankJ);

	scanFiles.store(true);
}


RadioMusic::RadioMusic() {
	config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

	configParam(STATION_PARAM, 0.0f, 1.0f, 0.0f, "Station");
	configParam<StartParamQuantity>(START_PARAM, 0.0f, 1.0f, 0.0f, "Start");
	configButton(RESET_PARAM, "Reset");

	configInput(STATION_INPUT, "Station");
	configInput(START_INPUT, "Start");
	configInput(RESET_INPUT, "Reset");

	configOutput(OUT_OUTPUT, "Output");

	configLight(RESET_LIGHT, "Reset");

	currentPlayer = &audioPlayer1;
	previousPlayer = &audioPlayer2;
	currentObjectPool = &audioContainer1;
	tmpObjectPool = &audioContainer2;

	stopWorker.store(false);

	worker = std::make_shared<std::thread>(&RadioMusic::workerThread, this);

	init();
}

RadioMusic::~RadioMusic() {
	abortLoad.store(true);
	stopWorker.store(true);
	workerDoWork.store(true);
	cond.notify_all(); // Wake up worker thread
	worker->join();
}

void RadioMusic::onReset() {
	init();
}

void RadioMusic::init() {
	prevIndex = -1;
	tick = 0;
	elapsedMs = 0;
	crossfade = false;
	fadeout = false;
	fadeOutGain = 1.0f;
	xfadeGain1 = 0.0f;
	xfadeGain2 = 1.0f;
	flashResetLed = false;
	ledTimerMs = 0;

	// Reset LED flash state
	resetLedInitTimer = true;
	resetLedTimerStart = 0;

	// Loading indicator state
	loadingIndicatorInitTimer = true;
	loadingIndicatorTimerStart = 0;
	loadingIndicatorToggle = false;
	loadingIndicatorBlinks = 0;

	// Initialize cached start parameter
	currentStartParam = 0.0f;

	selectBank.store(false);
	loadFiles.store(false);
	scanFiles.store(false);

	filesLoaded.store(false);
	loadingFiles.store(false);
	loadError.store(false);
	abortLoad.store(false);
	scanAudioFiles.store(false);
	loadAudioFiles.store(false);

	// Settings
	stereoOutputMode = false;
	pitchMode = false;
	loopingEnabled = true;
	enableCrossfade = true;
	sortFiles = false;
	allowAllFiles = false;
	rootDir = "";
	currentBank = 0;

	// Internal state
	{
		std::lock_guard<std::mutex> lock(mutex);
		scanner.banks.clear();
	}

	if (currentPlayer->object()) {
		currentPlayer->reset();
	}
	if (previousPlayer->object()) {
		previousPlayer->reset();
	}

	for (size_t i = 0; i < NUM_LIGHTS; i++) {
		lights[RESET_LIGHT + i].value = 0.0f;
	}
}

void RadioMusic::threadedScan() {
	if (rootDir.empty()) {
		WARN("No root directory defined. Scan failed.");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		scanner.reset();
		scanner.scan(rootDir, sortFiles, !allowAllFiles);
		if (scanner.banks.size() == 0) {
			return;
		}
		currentBank = clamp(currentBank, 0, (int)scanner.banks.size()-1);
	}

	loadFiles.store(true);
}

void RadioMusic::workerThread() {
	while (true) {
		std::unique_lock<std::mutex> lock(mutex);

		// Wait on condition variable instead of spin-waiting
		cond.wait(lock, [this] {
			return stopWorker.load() ||
			       scanAudioFiles.load() ||
			       loadAudioFiles.load();
		});

		if (stopWorker.load()) {
			return;
		}

		if (scanAudioFiles.load()) {
			scanAudioFiles.store(false);
			lock.unlock();
			threadedScan();
			lock.lock();
		}
		if (loadAudioFiles.load()) {
			loadAudioFiles.store(false);
			lock.unlock();
			threadedLoad();
			lock.lock();
		}

		workerDoWork.store(false);
	}
}

void RadioMusic::threadedLoad() {
	std::vector<std::string> files;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (scanner.banks.empty()) {
			WARN("No banks available. Failed to load audio files.");
			return;
		}

		currentBank = clamp(currentBank, 0, (int)scanner.banks.size()-1);
		files = scanner.banks[currentBank];
	}

	loadingFiles.store(true);

	drwav wav;
	for (size_t i = 0; i < files.size(); ++i) {
		// Check for abort signal before loading each file
		if (abortLoad.load()) {
			tmpObjectPool->clear();
			loadingFiles.store(false);
			return;
		}

		std::shared_ptr<AudioObject> object;

		if (drwav_init_file(&wav, files[i].c_str(), nullptr)) {
			object = std::make_shared<WavAudioObject>();
			if (drwav_uninit(&wav) != DRWAV_SUCCESS) {
				FATAL("Failed to uninitialize object %d %s", (int)i, files[i].c_str());
			}
		} else { // if load fails, interpret as raw audio
			object = std::make_shared<RawAudioObject>();
		}

		// Actually load files
		if (object->load(files[i])) {

			const unsigned long memory = object->totalSamples*sizeof(float);
			if ((tmpObjectPool->memoryUsage + memory) < MAX_BANK_SIZE) {
				tmpObjectPool->objects.push_back(std::move(object));
				tmpObjectPool->memoryUsage += memory;
			} else {
				WARN("Bank memory limit of %lld Bytes exceeded. Aborting loading of audio objects.", (long long)MAX_BANK_SIZE);
				loadError = true;
				break;
			}
		} else {
			WARN("Failed to load object %d %s", (int)i, files[i].c_str());
			loadError.store(true);
		}
	}

	filesLoaded.store(true);

	// Wait for object audio pool pointers to be swapped (in main thread).
	// Wait for pool swap using condition variable
	std::unique_lock<std::mutex> lock(mutex);
	cond.wait_for(lock, std::chrono::seconds(5), [this] {
		return !filesLoaded.load() || stopWorker.load();
	});

	// After swap, release memory of previous audio object pool.
	tmpObjectPool->clear();

	loadingFiles.store(false);
}

void RadioMusic::resetCurrentPlayer(const float start) {
	if (!currentPlayer->object() || currentPlayer->object()->channels == 0) {
		return;
	}

	const unsigned int channels = currentPlayer->object()->channels;
	const drwav_uint64 frameSamples = currentPlayer->object()->totalSamples / channels;

	// Protect against overflow and invalid values
	if (frameSamples == 0) return;

	unsigned long pos = static_cast<unsigned long>(start * frameSamples);
	if (pos >= frameSamples) { pos = frameSamples - 1; }
	pos = pos % frameSamples;
	currentPlayer->resetTo(pos * channels);
}

void RadioMusic::clearCurrentBank() {
	currentObjectPool->clear();
	previousPlayer->reset();
	currentPlayer->reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (currentBank < (int)scanner.banks.size()) {
			scanner.banks[currentBank].clear();
		}
	}

	for (int i = 0; i < 4; i++) {
		lights[LED_0_LIGHT+i].value = 0.0f;
	}
}

void RadioMusic::updateResetLedState() {
	if (resetLedInitTimer) {
		resetLedTimerStart = ledTimerMs;
		resetLedInitTimer = false;
	}

	lights[RESET_LIGHT].value = 1.0f;

	if ((ledTimerMs - resetLedTimerStart) > RESET_LED_FLASH_TIME_MS) {
		resetLedInitTimer = true;
		ledTimerMs = 0;
		flashResetLed = false;
	}
}

void RadioMusic::updateLoadingIndicator() {
	unsigned int blinkTime = 0;

	if (loadingFiles) {
		blinkTime = 1000u;
	}
	if (loadError) {
		blinkTime = 200u;
	}

	if (loadingIndicatorInitTimer) {
		loadingIndicatorTimerStart = ledTimerMs;
		loadingIndicatorInitTimer = false;
	}

	for (int i = 0; i < 4; i++) {
		lights[LED_0_LIGHT+i].value = loadingIndicatorToggle ? 1.0f : 0.0f;
	}

	if ((ledTimerMs - loadingIndicatorTimerStart) > blinkTime) {
		loadingIndicatorInitTimer = true;
		ledTimerMs = 0;
		loadingIndicatorToggle = !loadingIndicatorToggle;

		if (loadError && ++loadingIndicatorBlinks > 10) {
			loadingIndicatorBlinks = 0;
			loadingIndicatorToggle = false;
			loadError = false;
		}
	}
}

void RadioMusic::processAudioFrame(int frameIndex, dsp::Frame<2>& frame, const float start) {
	if (!currentPlayer->object() || currentPlayer->object()->channels == 0) {
		return;
	}

	const unsigned int channels = currentPlayer->object()->channels;
	const float peak = std::max(currentPlayer->object()->peak, 0.001f); // Prevent division by zero
	const float gain = 5.0f / peak; // Pre-calculate gain

	// Crossfade?
	if (crossfade) {
		xfadeGain1 = rack::crossfade(xfadeGain1, 1.0f, XFADE_RATE);
		xfadeGain2 = rack::crossfade(xfadeGain2, 0.0f, XFADE_RATE);

		for (unsigned int channel = 0; channel < channels; channel++) {
			const float currSample = currentPlayer->play(channel);
			const float prevSample = previousPlayer->play(channel);
			const float out = currSample * xfadeGain1 + prevSample * xfadeGain2;

			frame.samples[channel] = gain * out;
		}

		currentPlayer->advance(loopingEnabled, pitchMode);
		previousPlayer->advance(loopingEnabled, pitchMode);

		if (isNear(xfadeGain1, 1.0f) || isNear(xfadeGain2, 0.0f)) {
			crossfade = false;
		}
	}
	// Fade out (before resetting)?
	else if (fadeout) {
		fadeOutGain = rack::crossfade(fadeOutGain, 0.0f, FADEOUT_RATE);

		for (unsigned int channel = 0; channel < channels; channel++) {
			const float sample = currentPlayer->play(channel);
			const float out = sample * fadeOutGain;

			frame.samples[channel] = gain * out;
		}

		currentPlayer->advance(loopingEnabled, pitchMode);

		if (isNear(fadeOutGain, 0.0f)) {
			resetCurrentPlayer(start);
			fadeout = false;
		}
	}
	// Normal playback
	else {
		for (unsigned int channel = 0; channel < channels; channel++) {
			const float out = currentPlayer->play(channel);
			frame.samples[channel] = gain * out;
		}

		currentPlayer->advance(loopingEnabled, pitchMode);
	}
}

void RadioMusic::process(const ProcessArgs &args) {

	if (rootDir.empty()) {
		// No files loaded yet. Idle.
		return;
	}

	if (scanFiles.load()) {
		scanAudioFiles.store(true);
		workerDoWork.store(true);
		cond.notify_one();

		scanFiles.store(false);
	}

	if (loadFiles.load()) {
		// If we are already loading, tell the thread to abort the
		// current loading process.
		if (loadingFiles.load() && !abortLoad.load()) {
			abortLoad.store(true);
		}
		if (!loadingFiles.load()) {
			abortLoad.store(false);

			loadAudioFiles.store(true);
			workerDoWork.store(true);
			cond.notify_one();

			loadFiles.store(false);
		}
	}

	if (filesLoaded.load()) {
		// Swap out Audio Object Pool with newly loaded files
		{
			std::lock_guard<std::mutex> lock(mutex);
			AudioObjectPool* tmp;
			tmp = currentObjectPool;
			currentObjectPool = tmpObjectPool;
			tmpObjectPool = tmp;
		}

		currentPlayer->reset(); // Reset current player to use new audio
		outputBuffer.clear();   // Clear output buffer to start fresh
		prevIndex = -1; // Force channel change detection upon loading files
		elapsedMs = 0;  // Reset station to beginning

		filesLoaded.store(false);  // Signal worker thread that swap is complete
		cond.notify_one(); // Signal worker that swap is complete
	}

	// Bank selection mode
	if (selectBank.load()) {
		// Bank is selected via Reset button
		if (rstButtonTrigger.process(params[RESET_PARAM].getValue())) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (scanner.banks.size() > 0) {
					currentBank++;
					currentBank %= scanner.banks.size();
				}
			}
		}

			// Show bank selection in LED bar
		{
			std::lock_guard<std::mutex> lock(mutex);
			lights[LED_0_LIGHT].value = ((currentBank & 1) != 0) ? 1.0f : 0.0f;
			lights[LED_1_LIGHT].value = ((currentBank & 2) != 0) ? 1.0f : 0.0f;
			lights[LED_2_LIGHT].value = ((currentBank & 4) != 0) ? 1.0f : 0.0f;
			lights[LED_3_LIGHT].value = ((currentBank & 8) != 0) ? 1.0f : 0.0f;
		}
		lights[RESET_LIGHT].value = 1.0f;
	}

	// Keep track of milliseconds of elapsed time
	if (tick++ % (static_cast<int>(args.sampleRate)/1000) == 0) {
		elapsedMs++;
		ledTimerMs++;
	}

	// Normal mode: Start knob & input
	float start(0.0f);
	if (!pitchMode) {
		start = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage()/5.0f, 0.0f, 1.0f);
	} else {
		// Pitch mode: Start knob sets sample root pitch (via playback speed). Start input follows 1V/Oct.
		const float speed = clamp(params[START_PARAM].getValue() + inputs[START_INPUT].getVoltage()/5.0f, 0.0f, 1.0f);
		const float scaledSpeed = pow(2.0f, PITCH_RANGE*speed - PITCH_RANGE*0.5f);
		currentPlayer->setPlaybackSpeed(scaledSpeed);
	}
	currentStartParam = start;

	if (currentObjectPool->objects.size() > 0 && (rstButtonTrigger.process(params[RESET_PARAM].getValue()) ||
		(inputs[RESET_INPUT].isConnected() && rstInputTrigger.process(inputs[RESET_INPUT].getVoltage())))) {

		fadeOutGain = 1.0f;

		if (enableCrossfade) {
			fadeout = true;
		} else {
			resetCurrentPlayer(start);
		}

		flashResetLed = true;
	}

	// Channel knob & input
	const float channel = clamp(params[STATION_PARAM].getValue() + inputs[STATION_INPUT].getVoltage()/5.0f, 0.0f, 1.0f);
	const int index = (currentObjectPool->objects.size() > 0) ? \
		clamp(static_cast<int>(rescale(channel, 0.0f, 1.0f, 0.0f, static_cast<float>(currentObjectPool->objects.size()))),
			0, static_cast<int>(currentObjectPool->objects.size()) - 1) : 0;

	// Channel switch detection
	if (currentObjectPool->objects.size() > 0 && index != prevIndex) {

		AudioPlayer *tmp;
		tmp = previousPlayer;
		previousPlayer = currentPlayer;
		currentPlayer = tmp;

		if (index < static_cast<int>(currentObjectPool->objects.size())) {
			currentPlayer->load(currentObjectPool->objects[index]);

			if (!pitchMode) {
				const auto& audioObj = currentObjectPool->objects[index];
				if (audioObj && audioObj->channels > 0 && audioObj->sampleRate > 0) {
					const uint64_t frameSamples = audioObj->totalSamples / audioObj->channels;
					const uint64_t elapsedSamples = (elapsedMs * static_cast<uint64_t>(audioObj->sampleRate)) / 1000u;
					uint64_t pos = audioObj->currentPos + (static_cast<uint64_t>(currentPlayer->object()->channels) * elapsedSamples);
					if (frameSamples > 0) {
						pos = pos % (frameSamples);
					}
					currentPlayer->skipTo(pos);
				}
			} else {
				currentPlayer->skipTo(0);
			}

			elapsedMs = 0;
		}

		xfadeGain1 = 0.0f;
		xfadeGain2 = 1.0f;

		crossfade = enableCrossfade;

		// Different number of channels while crossfading leads to audible artifacts.
		if (previousPlayer->object()) {
			if (currentPlayer->object()->channels != previousPlayer->object()->channels) {
				crossfade = false;
			}
		}

		flashResetLed = true;
	}

	prevIndex = index;

	// Reset LED
	if (flashResetLed) {
		updateResetLedState();
	}

	if (!flashResetLed && !selectBank.load()) {
		lights[RESET_LIGHT].value = 0.0f;
	}

	// Audio processing
	if (outputBuffer.empty()) {
		// Nothing to play if no audio objects are loaded into players.
		if (!currentPlayer->object() || !currentPlayer->object()->channels) {
			return;
		}

		dsp::Frame<2> audioFrames[BLOCK_SIZE];

		for (int i = 0; i < BLOCK_SIZE; i++) {
			processAudioFrame(i, audioFrames[i], currentStartParam);
		}

		// Sample rate conversion to match Rack engine sample rate.
		if (currentPlayer->object() && currentPlayer->object()->sampleRate > 0) {
			outputSrc.setRates(currentPlayer->object()->sampleRate, args.sampleRate);
			int inLen = BLOCK_SIZE;
			int outLen = outputBuffer.capacity();

			outputSrc.process(audioFrames, &inLen, outputBuffer.endData(), &outLen);
			outputBuffer.endIncr(outLen);
		}
	}

	// Output processing & metering
	if (!outputBuffer.empty()) {
		outputs[OUT_OUTPUT].setChannels(stereoOutputMode ? 2 : 1);
		dsp::Frame<2> frame = outputBuffer.shift();

		// Validate channel count before using
		if (!currentPlayer->object() || currentPlayer->object()->channels == 0 || currentPlayer->object()->peak <= 0.0f) {
			return;
		}

		const unsigned int channels = currentPlayer->object()->channels;

		// Stereo mode
		if (stereoOutputMode) {
			if (channels == 2) {
				for (unsigned int c = 0; c < 2; c++) {
					outputs[OUT_OUTPUT].setVoltage(frame.samples[c], c);
				}
			} else if (channels == 1) {
				// For mono audio files, duplicate mono audio across both channels.
				outputs[OUT_OUTPUT].setVoltage(frame.samples[0], 0);
				outputs[OUT_OUTPUT].setVoltage(frame.samples[0], 1);
			}
		// Mono mode
		} else {
			if (channels == 2) {
				// L/R channels summed to mono.
				outputs[OUT_OUTPUT].setVoltage((frame.samples[0] + frame.samples[1])/channels);
			} else if (channels == 1) {
				outputs[OUT_OUTPUT].setVoltage(frame.samples[0]);
			}
		}

		// Disable VU Meter in Bank Selection mode.
		if (!selectBank.load()) {
			const float sampleTime = args.sampleTime;
			vumeter.process(sampleTime, frame.samples[0]/5.0f);

			// Only update LED brightness every 512 samples to reduce overhead
			if (tick % 512 == 0) {
				lights[LED_3_LIGHT].setBrightness(vumeter.getBrightness(-6.0f, 0.0f));
				lights[LED_2_LIGHT].setBrightness(vumeter.getBrightness(-12.0f, -6.0f));
				lights[LED_1_LIGHT].setBrightness(vumeter.getBrightness(-18.0f, -12.0f));
				lights[LED_0_LIGHT].setBrightness(vumeter.getBrightness(-24.0f, -18.0f));
			}
		}
	}

	// Indicator for loading audio files and errors during load.
	if (loadingFiles || loadError) {
		updateLoadingIndicator();
	}
}

struct RadioMusicDirDialogItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {

		const std::string dir = \
			rm->rootDir.empty() ? asset::user("") : rm->rootDir;
		char *path = osdialog_file(OSDIALOG_OPEN_DIR, dir.c_str(), nullptr, nullptr);
		if (path) {
			rm->rootDir = std::string(path);
			rm->scanFiles.store(true);
			free(path);
		}
	}
};

struct RadioMusicSelectBankItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->selectBank.store(!rm->selectBank.load());

		if (rm->selectBank.load() == false) {
			rm->loadFiles.store(true);
		}
	}
	void step() override {
		text = (rm->selectBank.load() != true) ? "Enter Bank Select Mode" : "Exit Bank Select Mode";
		rightText = CHECKMARK(rm->selectBank.load());
	}
};

struct RadioMusicClearCurrentBankItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->clearCurrentBank();
	}
};

struct RadioMusicStereoOutputModeItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->stereoOutputMode = !rm->stereoOutputMode;
	}
	void step() override {
		rightText = CHECKMARK(rm->stereoOutputMode);
	}
};

struct RadioMusicPitchModeItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->pitchMode = !rm->pitchMode;
	}
	void step() override {
		rightText = CHECKMARK(rm->pitchMode);
	}
};

struct RadioMusicLoopingEnabledItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->loopingEnabled = !rm->loopingEnabled;
	}
	void step() override {
		rightText = CHECKMARK(rm->loopingEnabled);
	}
};

struct RadioMusicCrossfadeItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->enableCrossfade = !rm->enableCrossfade;
	}
	void step() override {
		rightText = CHECKMARK(rm->enableCrossfade);
	}
};

struct RadioMusicFileSortItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->sortFiles = !rm->sortFiles;
	}
	void step() override {
		rightText = CHECKMARK(rm->sortFiles);
	}
};

struct RadioMusicFilesAllowedItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->allowAllFiles = !rm->allowAllFiles;
	}
	void step() override {
		rightText = CHECKMARK(rm->allowAllFiles);
	}
};

struct RadioMusicWidget : ModuleWidget {
	RadioMusicWidget(RadioMusic *module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Radio.svg")));

		addChild(createWidget<ScrewSilver>(Vec(14, 0)));

		addChild(createLight<MediumLight<RedLight>>(Vec(6, 33), module, RadioMusic::LED_0_LIGHT));
		addChild(createLight<MediumLight<RedLight>>(Vec(19, 33), module, RadioMusic::LED_1_LIGHT));
		addChild(createLight<MediumLight<RedLight>>(Vec(32, 33), module, RadioMusic::LED_2_LIGHT));
		addChild(createLight<MediumLight<RedLight>>(Vec(45, 33), module, RadioMusic::LED_3_LIGHT));

		addParam(createParam<Davies1900hBlackKnob>(Vec(12, 49), module, RadioMusic::STATION_PARAM));
		addParam(createParam<Davies1900hBlackKnob>(Vec(12, 131), module, RadioMusic::START_PARAM));

		addChild(createLight<MediumLight<RedLight>>(Vec(44, 188), module, RadioMusic::RESET_LIGHT));
		addParam(createParam<PB61303>(Vec(25, 202), module, RadioMusic::RESET_PARAM));

		addInput(createInput<PJ301MPort>(Vec(3, 274), module, RadioMusic::STATION_INPUT));
		addInput(createInput<PJ301MPort>(Vec(32, 274), module, RadioMusic::START_INPUT));

		addInput(createInput<PJ301MPort>(Vec(3, 318), module, RadioMusic::RESET_INPUT));
		addOutput(createOutput<PJ301MPort>(Vec(32, 318), module, RadioMusic::OUT_OUTPUT));

		addChild(createWidget<ScrewSilver>(Vec(14, 365)));
	};

	void appendContextMenu(Menu *menu) override {
		RadioMusic *module = dynamic_cast<RadioMusic*>(this->module);
		if (!module) return;

		menu->addChild(new MenuEntry);

		RadioMusicDirDialogItem *rootDirItem = new RadioMusicDirDialogItem;
		std::stringstream rootDirText, rootDir;
		if (module->rootDir.empty()) {
			rootDir << "<No root directory selected. Click to select.>";
		} else {
			rootDir << module->rootDir;
		}
		rootDirText << "Root Directory: " << rootDir.str();
		rootDirItem->text = rootDirText.str();
		rootDirItem->rm = module;
		menu->addChild(rootDirItem);

		RadioMusicSelectBankItem *selectBankItem = new RadioMusicSelectBankItem;
		selectBankItem->text = "";
		selectBankItem->rm = module;
		menu->addChild(selectBankItem);

		RadioMusicClearCurrentBankItem *clearCurrentBankItem = new RadioMusicClearCurrentBankItem();
		clearCurrentBankItem->text = "Clear Current Bank";
		clearCurrentBankItem->rm = module;
		menu->addChild(clearCurrentBankItem);

		menu->addChild(new MenuEntry);

		RadioMusicStereoOutputModeItem *stereoOutputModeItem = new RadioMusicStereoOutputModeItem;
		stereoOutputModeItem->text = "Stereo Output enabled";
		stereoOutputModeItem->rm = module;
		menu->addChild(stereoOutputModeItem);

		RadioMusicPitchModeItem *pitchModeItem = new RadioMusicPitchModeItem;
		pitchModeItem->text = "Pitch Mode enabled";
		pitchModeItem->rm = module;
		menu->addChild(pitchModeItem);

		RadioMusicLoopingEnabledItem *loopingEnabledItem = new RadioMusicLoopingEnabledItem;
		loopingEnabledItem->text = "Looping enabled";
		loopingEnabledItem->rm = module;
		menu->addChild(loopingEnabledItem);

		RadioMusicCrossfadeItem *crossfadeItem = new RadioMusicCrossfadeItem;
		crossfadeItem->text = "Crossfade enabled";
		crossfadeItem->rm = module;
		menu->addChild(crossfadeItem);

		RadioMusicFileSortItem *fileSortItem = new RadioMusicFileSortItem;
		fileSortItem->text = "Files sorted";
		fileSortItem->rm = module;
		menu->addChild(fileSortItem);

		RadioMusicFilesAllowedItem *filesAllowedItem = new RadioMusicFilesAllowedItem;
		filesAllowedItem->text = "All files allowed";
		filesAllowedItem->rm = module;
		menu->addChild(filesAllowedItem);
	}
};

Model *modelRadioMusic = createModel<RadioMusic, RadioMusicWidget>("RadioMusic");