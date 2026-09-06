/*
 * wx_astro.c -- moon phase from the clock alone.
 *
 * No weather API supplies moon phase and none needs to: it is a function of
 * the date. Open-Meteo has no moon field, and adding a second provider for one
 * integer would be absurd.
 *
 * ACCURACY, STATED HONESTLY: this is the MEAN synodic month, which ignores the
 * moon's orbital eccentricity. True new moon wanders about +/-0.5 day either
 * side of the mean. Against eight phase buckets that are 3.7 days wide, a
 * half-day error can only misclassify a reading taken within half a day of a
 * bucket boundary -- and on those days the moon genuinely looks like both. It
 * is the wrong algorithm for an almanac and the right one for a 96x96 icon.
 */
#include <math.h>
#include <time.h>

#include "vaultweather.h"

/* Reference new moon: 2000-01-06 18:14 UTC.
 * Unix 947182440. This is the standard epoch used for mean-phase arithmetic. */
#define NEW_MOON_EPOCH   947182440.0

/* Mean synodic month, 29.530588853 days, in seconds. */
#define SYNODIC_SECONDS  2551442.8769

int wx_moon_phase(time_t t)
{
	if (t <= 0)
		return 0;

	double age = fmod((double)t - NEW_MOON_EPOCH, SYNODIC_SECONDS);
	if (age < 0.0)
		age += SYNODIC_SECONDS;          /* dates before the epoch */

	double frac = age / SYNODIC_SECONDS; /* 0.0 = new, 0.5 = full */

	/* Round to the nearest bucket rather than truncating: with 8 buckets,
	 * truncation would label everything from new moon until 3.7 days later
	 * "NEW", so the icon would lag reality by up to a full bucket. */
	int idx = (int)(frac * WX_MOON_PHASES + 0.5);
	return idx % WX_MOON_PHASES;
}

const char *wx_moon_phase_name(int phase)
{
	/* All <= 12 characters. At 16px per glyph that is 192px, which fits the
	 * condition-text slot; see the width rule in wx_ui.c. */
	static const char *names[WX_MOON_PHASES] = {
		"NEW MOON",      /* 0 */
		"WAX CRESCENT",  /* 1 */
		"FIRST QTR",     /* 2 */
		"WAX GIBBOUS",   /* 3 */
		"FULL MOON",     /* 4 */
		"WAN GIBBOUS",   /* 5 */
		"LAST QTR",      /* 6 */
		"WAN CRESCENT",  /* 7 */
	};
	if (phase < 0 || phase >= WX_MOON_PHASES)
		return "MOON";
	return names[phase];
}
