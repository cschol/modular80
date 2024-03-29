#include "modular80.hpp"

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

	Nosering()
	{
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		configParam(INT_RATE_PARAM, 0.0f, 14.0f, 0.0f, "Clock Rate", " Hz");
		configParam(CHANGE_PARAM, -10.0f, 10.0f, -10.0f, "Change");
		configParam(CHANCE_PARAM, -10.0f, 10.0f, -10.0f, "Chance");
		configSwitch(INVERT_OLD_DATA_PARAM, 0.0f, 1.0f, 0.0f, "Invert Old Data", {"Normal", "Inverted"});

		configInput(CHANGE_INPUT, "Change");
		configInput(CHANCE_INPUT, "Chance");
		configInput(EXT_RATE_INPUT, "External Clock Rate");
		configInput(EXT_CHANCE_INPUT, "External Chance");
		configInput(INV_OUT_INPUT, "Invert Old Data");

		configOutput(N_PLUS_1_OUTPUT, "n+1");
		configOutput(TWO_POW_N_OUTPUT, "2^n");
		configOutput(NOISE_OUTPUT, "Noise");
	}

	void process(const ProcessArgs &args) override;
	void onReset() override;

private:

	float phase;

	dsp::SchmittTrigger clkTrigger;

	unsigned int shiftRegister[SR_SIZE] = {0,0,0,0,0,0,0,0};
};

void Nosering::onReset() {
	phase = 0.0f;

	for (unsigned int &val : shiftRegister) {
		val = 0;
	}
}

void Nosering::process(const ProcessArgs &args) {

	bool doStep(false);

	// Inputs
	const float change = clamp((params[CHANGE_PARAM].getValue() + inputs[CHANGE_INPUT].getVoltage()), -10.0f, 10.0f);
	const float chance = clamp((params[CHANCE_PARAM].getValue() + inputs[CHANCE_INPUT].getVoltage()), -10.0f, 10.0f);

	// Generate White noise sample
	const float noiseSample = clamp((random::uniform() * 20.0f - 10.0f), -10.0f, 10.0f);

	// Either use Chance input to sample data for Chance comparator or White Noise.
	float sample(0.0f);
	if (inputs[EXT_CHANCE_INPUT].isConnected()) {
		sample = inputs[EXT_CHANCE_INPUT].getVoltage();
	} else {
		sample = noiseSample;
	}

	// External clock
	if (inputs[EXT_RATE_INPUT].isConnected()) {
		if (clkTrigger.process(inputs[EXT_RATE_INPUT].getVoltage())) {
			phase = 0.0f;
			doStep = true;
		}
	}
	else { // Internal clock
		float freq = powf(2.0f, params[INT_RATE_PARAM].getValue());

		// Limit internal rate.
		if (freq > MAX_FREQ) {
			freq = MAX_FREQ;
		}

		phase += freq * args.sampleTime;
		if (phase >= 1.0f) {
			phase = 0.0f;
			doStep = true;
		}
	}

	if (doStep) {
		bool selectNewData = (noiseSample > change); // Always compare against White Noise

		unsigned int newData = (sample > chance) ? 0 : 1;
		unsigned int oldData = shiftRegister[SR_SIZE - 1];

		const bool invertOldData = (params[INVERT_OLD_DATA_PARAM].getValue() != 0.0f) ||
								   (inputs[INV_OUT_INPUT].getVoltage() != 0.0f);
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
	outputs[N_PLUS_1_OUTPUT].setVoltage(clamp(nPlus1Output, 0.0f, 10.0f));
	outputs[TWO_POW_N_OUTPUT].setVoltage(clamp(TwoPowNOutput, 0.0f, 10.0f));
	outputs[NOISE_OUTPUT].setVoltage(noiseSample);
}

struct NoseringWidget : ModuleWidget {
	NoseringWidget(Nosering *module);
};

NoseringWidget::NoseringWidget(Nosering *module) {
	setModule(module);
	setPanel(createPanel(asset::plugin(pluginInstance, "res/Nosering.svg")));

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
