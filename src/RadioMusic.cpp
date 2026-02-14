#include "modular80.hpp"

#include <thread>
#include <condition_variable>
#include <chrono>

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
  bankCount(0)
  {}
~FileScanner() {};

void reset() {
	bankCount = 0;
	scanDepth = 0;
	banks.clear();
}

static bool isSupportedAudioFormat(const std::string& path) {
	const std::string tmpF = string::lowercase(path);
	return (string::endsWith(tmpF, ".wav") ||
			string::endsWith(tmpF, ".raw"));
}

void scan(const std::string& root, const bool sort = false, const bool filter = true) {

	std::vector<std::string> files;
	std::vector<std::string> entries;

	entries = system::getEntries(root);

	if (sort) {
        std::sort(entries.begin(), entries.end());
	}

	for (std::string &entry : entries) {
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
		for (std::vector<std::string>::iterator it = files.begin(); it != files.end(); /* */) {
			if (!isSupportedAudioFormat(*it)) it = files.erase(it);
			else ++it;
		}
	}

	if (!files.empty()) {
		bankCount++;
		banks.push_back(files);
	}
	scanDepth--;
}

int scanDepth;
int bankCount;
std::vector< std::vector<std::string> > banks;

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
  peak(0.0f) {};

virtual ~AudioObject() {};

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
};
~WavAudioObject() {
	if (samples) {
		drwav_free(samples, nullptr);
	}
};

bool load(const std::string &path) override {
	drwav_uint64 totalFrames(0);

	filePath = path;
	samples = drwav_open_file_and_read_pcm_frames_f32(
		filePath.c_str(), &channels, &sampleRate, &totalFrames, nullptr
	);

	totalSamples = totalFrames * channels;

	if (samples) {
		for (size_t i = 0; i < totalSamples; ++i) {
			if (samples[i] > peak) peak = samples[i];
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
};
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
		rewind(wav);

		int16_t *rawSamples = (int16_t*)malloc(sizeof(int16_t) * fsize/bytesPerSample);
		if (rawSamples) {
			const long samplesRead = fread(rawSamples, (size_t)sizeof(int16_t), fsize/bytesPerSample, wav);
		if (samplesRead != fsize/(long)bytesPerSample) { WARN("Failed to read entire file"); }
			if (samples) {
				for (size_t i = 0; i < totalSamples; ++i) {
					samples[i] = static_cast<float>(rawSamples[i]);
					if (samples[i] > peak) peak = samples[i];
				}
			} else {
				WARN("Failed to allocate memory for samples");
				fclose(wav);
				free(rawSamples);
				return false;
			}
		} else {
			WARN("Failed to allocate memory for rawSamples");
			fclose(wav);
			return false;
		}

		fclose(wav);
		free(rawSamples);

	} else {
		WARN("Failed to load file: %s", filePath.c_str());
		return false;
	}

    return (samples != nullptr);
}

};


class AudioPlayer {

public:
AudioPlayer() :
  startPos(0.0f),
  playbackSpeed(1.0f)
{};
~AudioPlayer() {};

void load(std::shared_ptr<AudioObject> object) {
	audio = std::move(object);
}

void skipTo(float pos) {
	if (audio) {
		audio->currentPos = pos;
	}
}

float play(unsigned int channel) {
	float sample(0.0f);

	if (audio) {
		if (channel < audio->channels) {
			if ((audio->currentPos + channel) < audio->totalSamples) {
				const unsigned int pos = static_cast<int>(audio->currentPos + channel);
				const float delta = (audio->currentPos + channel) - pos;
				sample = crossfade(audio->samples[pos],
								   audio->samples[std::min(pos+1, (unsigned int)audio->totalSamples-1)],
								   delta);
			}
		}
	}

	return sample;
}

void advance(bool repeat, bool pitchMode) {
	if (audio) {

		float nextPos;
		if (pitchMode) {
			const float speed = playbackSpeed;
			nextPos = audio->currentPos + speed * static_cast<float>(audio->channels);
		} else {
			nextPos = audio->currentPos + audio->channels;
		}

		const float maxPos = static_cast<float>(audio->totalSamples);
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
}

void resetTo(float pos) {
	if (audio) {
		startPos = pos;
		audio->currentPos = startPos;
	}
}

bool ready() {
	if (audio) {
		return audio->totalSamples > 0;
	} else {
		return false;
	}
}

void reset() {
	if (audio) {
		audio.reset();
	}
}

void setPlaybackSpeed(const float speed) {
	playbackSpeed = speed;
}

std::shared_ptr<AudioObject> object() {
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
	bool loadFiles;
	bool scanFiles;
	bool selectBank;

	// Settings
	bool stereoOutputMode;
	bool pitchMode;
	bool loopingEnabled;
	bool enableCrossfade;
	bool sortFiles;
	bool allowAllFiles;
	std::string rootDir;
	int currentBank;

	json_t *dataToJson() override {
		json_t *rootJ = json_object();

		// Option: Stereo Output Mode
		json_t *stereoOutputModeJ = json_boolean(stereoOutputMode);
		json_object_set_new(rootJ, "stereoOutputMode", stereoOutputModeJ);

		// Option: Pitch Mode
		json_t *pitchModeJ = json_boolean(pitchMode);
		json_object_set_new(rootJ, "pitchMode", pitchModeJ);

		// Option: Loop Samples
		json_t *loopingJ = json_boolean(loopingEnabled);
		json_object_set_new(rootJ, "loopingEnabled", loopingJ);

		// Option: Enable Crossfade
		json_t *crossfadeJ = json_boolean(enableCrossfade);
		json_object_set_new(rootJ, "enableCrossfade", crossfadeJ);

		// Option: Sort Files
		json_t *sortJ = json_boolean(sortFiles);
		json_object_set_new(rootJ, "sortFiles", sortJ);

		// Option: Allow All Files
		json_t *filesJ = json_boolean(allowAllFiles);
		json_object_set_new(rootJ, "allowAllFiles", filesJ);

		// Internal state: rootDir
		json_t *rootDirJ = json_string(rootDir.c_str());
		json_object_set_new(rootJ, "rootDir", rootDirJ);

		// Internal state: currentBank
		json_t *bankJ = json_integer(currentBank);
		json_object_set_new(rootJ, "currentBank", bankJ);

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
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

		scanFiles = true;
	}

private:

	void init();
	void workerThread();
	void threadedScan();
	void threadedLoad();
	void resetCurrentPlayer(float start);
	void updateResetLedState();
	void updateLoadingIndicator();
	void processAudioFrame(int frameIndex, dsp::Frame<2>& frame, float start);

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
// It changes default value and label when in Pitch mode
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

	worker = std::make_shared<std::thread>(&RadioMusic::workerThread, this);

	init();
}

RadioMusic::~RadioMusic() {
	abortLoad.store(true);
	stopWorker.store(true);
	workerDoWork.store(true);
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

	selectBank = false;
	loadFiles = false;
	scanFiles = false;

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

	loadFiles = true;
}

// Worker thread main loop - uses atomic flags with spin-wait
void RadioMusic::workerThread() {
	while (true) {
		// Check if we should stop
		if (stopWorker.load()) {
			return;
		}

		// Wait for work to be signaled via atomic flag
		// Use spin-wait with sleep to avoid busy-waiting
		if (!workerDoWork.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		// Do the work
		if (scanAudioFiles.load()) {
			threadedScan();
			scanAudioFiles.store(false);
		}
		if (loadAudioFiles.load()) {
			threadedLoad();
			loadAudioFiles.store(false);
		}

		// Signal that work is complete by clearing the flag
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
	for (unsigned int i = 0; i < files.size(); ++i) {
		std::shared_ptr<AudioObject> object;

		// Quickly determine if file is WAV file
		if (drwav_init_file(&wav, files[i].c_str(), nullptr)) {
			object = std::make_shared<WavAudioObject>();
			if (drwav_uninit(&wav) != DRWAV_SUCCESS) {
				FATAL("Failed to uninitialize object %d %s", i, files[i].c_str());
			}
		} else { // if load fails, interpret as raw audio
			object = std::make_shared<RawAudioObject>();
		}

		// Actually load files
		if (object->load(files[i])) {
			// Abort the current load process and release the memory.
			if (abortLoad) {
				tmpObjectPool->clear();
				loadingFiles = false;
				return;
			}

			const unsigned long memory = object->totalSamples*sizeof(float);
			if ((tmpObjectPool->memoryUsage + memory) < MAX_BANK_SIZE) {
				tmpObjectPool->objects.push_back(std::move(object));
				tmpObjectPool->memoryUsage += memory;
			} else {
				WARN("Bank memory limit of %ld Bytes exceeded. Aborting loading of audio objects.", MAX_BANK_SIZE);
				loadError = true;
				break;
			}
		} else {
			WARN("Failed to load object %d %s", i, files[i].c_str());
			loadError.store(true);
		}
	}

	filesLoaded.store(true);

	// Wait for object audio pool pointers to be swapped (in main thread).
	// Use simple spin-wait with brief sleeps to avoid busy-waiting
	int waitCount = 0;
	while (filesLoaded.load() && !stopWorker.load() && waitCount < 1000) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		waitCount++;
	}

	// After swap, release memory of previous audio object pool.
	tmpObjectPool->clear();

	loadingFiles.store(false);
}

void RadioMusic::resetCurrentPlayer(float start) {
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

void RadioMusic::processAudioFrame(int frameIndex, dsp::Frame<2>& frame, float start) {
	if (!currentPlayer->object() || currentPlayer->object()->channels == 0) {
		return;
	}

	const unsigned int channels = currentPlayer->object()->channels;
	const float peak = std::max(currentPlayer->object()->peak, 0.001f); // Prevent division by zero

	// Crossfade?
	if (crossfade) {
		xfadeGain1 = rack::crossfade(xfadeGain1, 1.0f, XFADE_RATE);
		xfadeGain2 = rack::crossfade(xfadeGain2, 0.0f, XFADE_RATE);

		for (unsigned int channel = 0; channel < channels; channel++) {
			const float currSample = currentPlayer->play(channel);
			const float prevSample = previousPlayer->play(channel);
			const float out = currSample * xfadeGain1 + prevSample * xfadeGain2;

			frame.samples[channel] = 5.0f * out / peak;
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

			frame.samples[channel] = 5.0f * out / peak;
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
			frame.samples[channel] = 5.0f * out / peak;
		}

		currentPlayer->advance(loopingEnabled, pitchMode);
	}
}

void RadioMusic::process(const ProcessArgs &args) {

	if (rootDir.empty()) {
		// No files loaded yet. Idle.
		return;
	}

	if (scanFiles) {
		scanAudioFiles.store(true);
		workerDoWork.store(true);

		scanFiles = false;
	}

	if (loadFiles) {
		// If we are already loading, tell the thread to abort the
		// current loading process.
		if (loadingFiles.load() && !abortLoad.load()) {
			abortLoad.store(true);
		}
		if (!loadingFiles.load()) {
			abortLoad.store(false);

			loadAudioFiles.store(true);
			workerDoWork.store(true);

			loadFiles = false;
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
	}

	// Bank selection mode
	if (selectBank) {
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
			lights[LED_0_LIGHT].value = (1 && (currentBank & 1));
			lights[LED_1_LIGHT].value = (1 && (currentBank & 2));
			lights[LED_2_LIGHT].value = (1 && (currentBank & 4));
			lights[LED_3_LIGHT].value = (1 && (currentBank & 8));
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

		if (index < (int)currentObjectPool->objects.size()) {
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

	if (!flashResetLed && !selectBank) {
		lights[RESET_LIGHT].value = 0.0f;
	}

	// Audio processing
	if (outputBuffer.empty()) {
		// Nothing to play if no audio objects are loaded into players.
		if (!currentPlayer->object() || !currentPlayer->object()->channels) {
			return;
		}

		dsp::Frame<2> frame[BLOCK_SIZE];

		for (int i = 0; i < BLOCK_SIZE; i++) {
			processAudioFrame(i, frame[i], currentStartParam);
		}

		// Sample rate conversion to match Rack engine sample rate.
		if (currentPlayer->object() && currentPlayer->object()->sampleRate > 0) {
			outputSrc.setRates(currentPlayer->object()->sampleRate, args.sampleRate);
			int inLen = BLOCK_SIZE;
			int outLen = outputBuffer.capacity();

			outputSrc.process(frame, &inLen, outputBuffer.endData(), &outLen);
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
		if (!selectBank) {
			const float sampleTime = args.sampleTime;
			vumeter.process(sampleTime, frame.samples[0]/5.0f);

			if (tick % 512 == 0) {
				for (int i = 0; i < 4; i++){
					float b = vumeter.getBrightness(-6.0f * (i+1), 0.0f * i);
					lights[LED_3_LIGHT - i].setBrightness(b);
				}
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
		char *path = osdialog_file(OSDIALOG_OPEN_DIR, dir.c_str(), NULL, NULL);
		if (path) {
			rm->rootDir = std::string(path);
			rm->scanFiles = true;
			free(path);
		}
	}
};

struct RadioMusicSelectBankItem : MenuItem {
	RadioMusic *rm;
	void onAction(const event::Action &e) override {
		rm->selectBank = !rm->selectBank;

		if (rm->selectBank == false) {
			rm->loadFiles = true;
		}
	}
	void step() override {
		text = (rm->selectBank != true) ? "Enter Bank Select Mode" : "Exit Bank Select Mode";
		rightText = CHECKMARK(rm->selectBank);
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
