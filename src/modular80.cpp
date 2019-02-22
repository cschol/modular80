#include "modular80.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
	pluginInstance = p;

	p->addModel(modelLogistiker);
	p->addModel(modelNosering);
	p->addModel(modelRadioMusic);
}
