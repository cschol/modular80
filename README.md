# A collection of modules for VCV Rack

This repository contains a collection of modules for [VCV Rack](https://vcvrack.com/),
the open-source virtual modular synthesizer.

The minimum supported VCV Rack version is **0.6**.

# Overview of modules

![modular80](/modular80.png)

## Logistiker

The `Logistiker` module is based on the [Logistic Map](https://en.wikipedia.org/wiki/Logistic_map),
a non-linear dynamic equation, which for certain input parameters exhibits [chaotic behavior](https://en.wikipedia.org/wiki/Chaos_theory).

The **RATE** knob controls the update rate of the internal clock. It has no function, if an
external clock signal is connected to the **EXT CLOCK** input.

The **R** knob, and the corresponding input, controls the **R** variable of the equation.
The [Wikipedia page](https://en.wikipedia.org/wiki/Logistic_map) has a good overview of the effect
of different **R** values. The default value corresponds to the onset of *Chaos* in the system.

The **X0** knob sets the initial starting value for the **X** variable of the equation.

If the **RESET** button is pressed, or a positive edge arrives at the **RESET** input,
the model starts over from the value set by the **X0** knob. The reset takes effect at the
next rising edge of the (internal or external) clock signal.

[YouTube Module Demo](https://youtu.be/xGSvLBChjzk)

## Nosering

The `Nosering` module is inspired by Grant Richter's [Noisering](https://malekkoheavyindustry.com/product/richter-noisering/) module. It does not implement all of
the Noisering functionality, but enough to be useful for my purposes.

See [this page](https://www.infinitesimal.eu/modules/index.php?title=Malekko_Noisering) for more
information on theory of operation of the original *Noisering* module.

The **RATE** knob controls the update rate of the internal clock. It has no function, if an
external clock signal is connected to the **EXT CLOCK** input.

The **CHANGE** knob controls the probability that **new data** is introduced into the system.
All the way CCW means only new data is feed into the shift register. All the way CW means only
old data is recycled in the shift register, i.e. the shift register content is looping.

The **CHANCE** knob controls the probability of introducing either a **0** (all the way CCW) or
a **1** (all the way CW) into the system.

The **INV OLD** switch will cause the last bit pushed out of the shift register to be **inverted**
before feeding it back into the shift register input.

The **NOISE OUT** output carries the internal noise signal of the module.

The **n+1** output produces **n+1** or 9 levels, the **2^n** output produces **2^n** or 256 levels.
This is comparable to the [Buchla 266 Source Of Uncertainty](https://modularsynthesis.com/roman/buchla_266/266sou.htm).


# License

Source code licensed under BSD-3-Clause by Christoph Scholtes.
