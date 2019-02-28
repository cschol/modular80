#include "modular80.hpp"

#include <random>

#include "dsp/digital.hpp"

//#define DEBUG_MODE

#define SR_SIZE 8
#define MAX_FREQ 10000.0f

// Resistor ladder values for Digital-to-Analog conversion
const float DAC_MULT1[SR_SIZE] = {1.28f, 1.28f, 1.28f, 1.28f, 1.28f, 1.28f, 1.28f, 1.28f};
const float DAC_MULT2[SR_SIZE] = {5.0f, 2.5f, 1.25f, 0.625f, 0.3125f, 0.1525f, 0.078125f, 0.0390625f};

struct Nosering : Module {
	enum ParamIds {
		CHANGE_PARAM,
		CHANCE_PARAM,
		INT_RATE_PARAM,
		INVERT_OLD_DATA_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		CHANGE_INPUT,
		CHANCE_INPUT,
		EXT_RATE_INPUT,
		EXT_CHANCE_INPUT,
		INV_OUT_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		N_PLUS_1_OUTPUT,
		TWO_POW_N_OUTPUT,
		NOISE_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	Nosering() : _seed_gen(_rd()),
		_generator(_seed_gen()),
		_uniform(-10.0, 10.0)
	{
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		//addParam(createParam<Davies1900hBlackKnob>(Vec(49, 52), module, Nosering::INT_RATE_PARAM, 0, 14.0f, 0.0f));
		params[INT_RATE_PARAM].config(0.0f, 14.0f, 0.0f, "Rate", " Hz");
		//addParam(createParam<Davies1900hBlackKnob>(Vec(49, 109), module, Nosering::CHANGE_PARAM, -10.0f, 10.0f, -10.0f));
		params[CHANGE_PARAM].config(-10.0f, 10.0f, -10.0f, "Change");
		//addParam(createParam<Davies1900hBlackKnob>(Vec(49, 166), module, Nosering::CHANCE_PARAM, -10.0f, 10.0f, -10.0f));
		params[CHANCE_PARAM].config(-10.0f, 10.0f, -10.0f, "Chance");
		//addParam(createParam<CKSS>(Vec(60, 224), module, Nosering::INVERT_OLD_DATA_PARAM, 0.0f, 1.0f, 0.0f));
		params[INVERT_OLD_DATA_PARAM].config(0.0f, 1.0f, 0.0f, "Invert Old Data");
	}

	void process(const ProcessArgs &args) override;
	void reset();
	void onReset() override;

private:

	float phase;

	dsp::SchmittTrigger clkTrigger;

	unsigned int shiftRegister[SR_SIZE] = {0,0,0,0,0,0,0,0};

	std::random_device _rd;
	std::mt19937 _seed_gen;
	std::minstd_rand _generator;
	std::uniform_real_distribution<float> _uniform;
};

void Nosering::reset() {
	onReset();
}

void Nosering::onReset() {
	phase = 0.0f;

	for (unsigned int &val : shiftRegister) {
		val = 0;
	}
}

void Nosering::process(const ProcessArgs &args) {

	bool doStep(false);

	// Inputs
	const float change = clamp((params[CHANGE_PARAM].value + inputs[CHANGE_INPUT].value), -10.0f, 10.0f);
	const float chance = clamp((params[CHANCE_PARAM].value + inputs[CHANCE_INPUT].value), -10.0f, 10.0f);

	// Generate White noise sample
	const float noiseSample = clamp(_uniform(_generator), -10.0f, 10.0f);

	// Either use Chance input to sample data for Chance comparator or White Noise.
	float sample(0.0f);
	if (inputs[EXT_CHANCE_INPUT].active) {
		sample = inputs[EXT_CHANCE_INPUT].value;
	} else {
		sample = noiseSample;
	}

	// External clock
	if (inputs[EXT_RATE_INPUT].active) {
		if (clkTrigger.process(inputs[EXT_RATE_INPUT].value)) {
			phase = 0.0f;
			doStep = true;
		}
	}
	else { // Internal clock
		float freq = powf(2.0f, params[INT_RATE_PARAM].value);

		// Limit internal rate.
		if (freq > MAX_FREQ) {
			freq = MAX_FREQ;
		}

		phase += freq / APP->engine->getSampleRate();
		if (phase >= 1.0f) {
			phase = 0.0f;
			doStep = true;
		}
	}

	if (doStep) {
		bool selectNewData = (noiseSample > change); // Always compare against White Noise

		unsigned int newData = (sample > chance) ? 0 : 1;
		unsigned int oldData = shiftRegister[SR_SIZE - 1];

		const bool invertOldData = (params[INVERT_OLD_DATA_PARAM].value != 0.0f) ||
								   (inputs[INV_OUT_INPUT].value != 0.0f);
		if (invertOldData) {
			oldData = (oldData == 1) ? 0 : 1;
		}

		unsigned int sum(0);

		// Advance shift register
		for (size_t i = SR_SIZE-1; i > 0; i--) {
			sum += shiftRegister[i];
			shiftRegister[i] = shiftRegister[i - 1];
		}

		sum += shiftRegister[0];

		// Only do stale data detection if we are not inverting old data.invertOldData
		// If we are, the shift register should never be stale.
		if (!invertOldData) {
			// Stale data detection (either all 0s or all 1s in the shift register).
			if (sum == 0) {
				selectNewData = true;
				newData = 1;
			} else if (sum == 8) {
				selectNewData = true;
				newData = 0;
			}
		}

		// Move data into shift register
		shiftRegister[0] = (selectNewData) ? newData : oldData;

#ifdef DEBUG_MODE
		for (size_t i = 0; i < SR_SIZE; ++i) {
			debug("%d %d", i, shiftRegister[i]);
		}
#endif
	}

	// DAC
	float TwoPowNOutput(0.0f);
	float nPlus1Output(0.0f);
	for (size_t i = 0; i < SR_SIZE; ++i) {
		nPlus1Output += (static_cast<float>(shiftRegister[i]) * DAC_MULT1[i]);
		TwoPowNOutput += (static_cast<float>(shiftRegister[i]) * DAC_MULT2[i]);
	}

	// Outputs
	outputs[N_PLUS_1_OUTPUT].value = clamp(nPlus1Output, 0.0f, 10.0f);
	outputs[TWO_POW_N_OUTPUT].value = clamp(TwoPowNOutput, 0.0f, 10.0f);
	outputs[NOISE_OUTPUT].value = noiseSample;
}

struct NoseringWidget : ModuleWidget {
	NoseringWidget(Nosering *module);
};

NoseringWidget::NoseringWidget(Nosering *module) {
	setModule(module);
	setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Nosering.svg")));

	addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
	addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));

	addParam(createParam<Davies1900hBlackKnob>(Vec(49, 52), module, Nosering::INT_RATE_PARAM));
	addParam(createParam<Davies1900hBlackKnob>(Vec(49, 109), module, Nosering::CHANGE_PARAM));
	addParam(createParam<Davies1900hBlackKnob>(Vec(49, 166), module, Nosering::CHANCE_PARAM));
	addParam(createParam<CKSS>(Vec(60, 224), module, Nosering::INVERT_OLD_DATA_PARAM));

	addInput(createInput<PJ301MPort>(Vec(11, 58), module, Nosering::EXT_RATE_INPUT));
	addInput(createInput<PJ301MPort>(Vec(11, 115), module, Nosering::CHANGE_INPUT));
	addInput(createInput<PJ301MPort>(Vec(11, 172), module, Nosering::CHANCE_INPUT));
	addInput(createInput<PJ301MPort>(Vec(11, 221), module, Nosering::INV_OUT_INPUT));
	addInput(createInput<PJ301MPort>(Vec(11, 275), module, Nosering::EXT_CHANCE_INPUT));

	addOutput(createOutput<PJ301MPort>(Vec(56, 275), module, Nosering::NOISE_OUTPUT));
	addOutput(createOutput<PJ301MPort>(Vec(11, 319), module, Nosering::N_PLUS_1_OUTPUT));
	addOutput(createOutput<PJ301MPort>(Vec(56, 319), module, Nosering::TWO_POW_N_OUTPUT));

	addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
	addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
}

Model *modelNosering = createModel<Nosering, NoseringWidget>("Nosering");
