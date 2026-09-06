/*
 * wx_astro.c -- moon phase from the clock alone.
 *
 * No weather API supplies moon phase and none needs to: it is a function of
 * the date. Open-Meteo has no moon field, and adding a second provider for one
 * number would be absurd.
 *
 * WHY THIS IS CONTINUOUS NOW
 * The first version snapped the cycle into 8 buckets, 3.7 days wide, and both
 * the label and the art came out of that index. On a moon 1.7 days past last
 * quarter -- visibly a crescent, 32% lit -- it displayed "LAST QTR", because
 * 0.808 of the way round the cycle still fell inside the bucket centred on
 * 0.75. The bucket was doing two jobs badly: picking a picture and naming a
 * phase, when those want different resolutions.
 *
 * They are now separate:
 *   wx_moon_illum()  the actual illuminated fraction, continuous
 *   wx_moon_step()   an index into 48 pre-rendered tiles, 0.62 days apart,
 *                    which is finer than a 96x96 icon can show
 *   wx_moon_name()   a name chosen by where in the cycle we are, with the
 *                    four INSTANT phases held to a narrow window
 *
 * NEW MOON, FIRST QUARTER, FULL MOON and LAST QUARTER are instants, not
 * periods. Treating them as periods is what produced the bug. They get +/-0.02
 * of the cycle here -- about +/-0.6 days, so each shows for roughly 1.2 days
 * around the actual event -- and the crescent/gibbous names cover everything
 * between. Those four are kept rather than dropped because a half-lit moon is
 * a distinct thing to see, and "waning crescent" would be a worse description
 * of it than "last quarter".
 *
 * ACCURACY, STATED HONESTLY: this is the MEAN synodic month, which ignores the
 * moon's orbital eccentricity. True new moon wanders about +/-0.5 day either
 * side of the mean, so the illuminated percentage is good to a few points and
 * the name can be a few hours early or late at a boundary. It is the wrong
 * algorithm for an almanac and the right one for a 96x96 icon and a caption.
 * Validated against the real lunations of September 2026 before anything was
 * built on it: new 09-11, first quarter 09-18, full 09-26, last quarter 10-03.
 */
#include <math.h>
#include <time.h>

#include "vaultweather.h"

/* Reference new moon: 2000-01-06 18:14 UTC, unix 947182440. The standard
 * epoch for mean-phase arithmetic. */
#define NEW_MOON_EPOCH   947182440.0

/* Mean synodic month, 29.530588853 days, in seconds. */
#define SYNODIC_SECONDS  2551442.8769

#define TAU              6.283185307179586

/* Half-width of the window in which an INSTANT phase name is used, as a
 * fraction of the cycle. 0.02 * 29.53 days = 0.59 days either side. */
#define INSTANT_HALF_WIDTH  0.02

/* Position in the cycle: 0.0 = new, 0.25 = first quarter, 0.5 = full. */
static double cycle_frac(time_t t)
{
	if (t <= 0)
		return 0.0;

	double age = fmod((double)t - NEW_MOON_EPOCH, SYNODIC_SECONDS);
	if (age < 0.0)
		age += SYNODIC_SECONDS;      /* dates before the epoch */
	return age / SYNODIC_SECONDS;
}

float wx_moon_illum(time_t t)
{
	/* The lit fraction of the visible disc. This is the projection of the
	 * terminator, not a linear ramp -- which is why the moon looks nearly
	 * full for several days but passes through exactly half in hours. */
	return (float)((1.0 - cos(TAU * cycle_frac(t))) / 2.0);
}

int wx_moon_step(time_t t)
{
	/* Nearest of WX_MOON_STEPS tiles. Round rather than truncate: truncating
	 * makes every tile show from its own instant until the next, so the art
	 * lags reality by up to a full step. */
	int idx = (int)(cycle_frac(t) * WX_MOON_STEPS + 0.5);
	return idx % WX_MOON_STEPS;
}

const char *wx_moon_name(time_t t)
{
	double f = cycle_frac(t);

	static const struct { double centre; const char *name; } instant[] = {
		{ 0.00, "NEW MOON"      },
		{ 0.25, "FIRST QUARTER" },
		{ 0.50, "FULL MOON"     },
		{ 0.75, "LAST QUARTER"  },
	};

	for (unsigned i = 0; i < sizeof(instant) / sizeof(instant[0]); i++) {
		double d = fabs(f - instant[i].centre);
		if (d > 0.5)
			d = 1.0 - d;         /* wrap: 0.99 is near 0.00, not far */
		if (d < INSTANT_HALF_WIDTH)
			return instant[i].name;
	}

	/* Crescent below half lit, gibbous above -- that IS the definition, so it
	 * is keyed off the illuminated fraction rather than off the cycle
	 * position, and the two agree by construction. */
	bool waxing = (f < 0.5);
	bool gibbous = ((1.0 - cos(TAU * f)) / 2.0) >= 0.5;

	if (gibbous)
		return waxing ? "WAXING GIBBOUS" : "WANING GIBBOUS";
	return waxing ? "WAXING CRESCENT" : "WANING CRESCENT";
}
