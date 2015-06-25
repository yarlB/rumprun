/*-
 * Copyright (c) 2014, 2015 Antti Kantee.  All Rights Reserved.
 * Copyright (c) 2015 Martin Lucina.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <bmk/kernel.h>

#define NSEC_PER_SEC	1000000000ULL
#define TSC_SHIFT	27

/* clock isr trampoline (in locore.S) */
void bmk_cpu_isr_clock(void);

/* TSC multiplier for converting ticks to nsecs, scaled by TSC_SHIFT. */
static uint64_t tsc_mult;

/* Base time values at the last call to bmk_cpu_clock_now(). */
static bmk_time_t time_base;
static uint64_t tsc_base;

/*
 * Read the current i8254 channel 0 tick count.
 */
static unsigned int
i8254_gettick(void)
{
	uint16_t rdval;

	outb(TIMER_MODE, TIMER_SEL0 | TIMER_LATCH);
	rdval = inb(TIMER_CNTR);
	rdval |= (inb(TIMER_CNTR) << 8);
	return rdval;
}

/*
 * Delay for approximately n microseconds using the i8254 channel 0 counter.
 * Timer must be programmed appropriately before calling this function.
 */
static void
i8254_delay(unsigned int n)
{
	unsigned int cur_tick, initial_tick;
	int remaining;
	const unsigned long timer_rval = TIMER_HZ / HZ;

	initial_tick = i8254_gettick();

	remaining = (unsigned long long) n * TIMER_HZ / 1000000;

	while (remaining > 1) {
		cur_tick = i8254_gettick();
		if (cur_tick > initial_tick)
			remaining -= timer_rval - (cur_tick - initial_tick);
		else
			remaining -= initial_tick - cur_tick;
		initial_tick = cur_tick;
	}
}

static uint64_t
rdtsc(void)
{
	uint64_t val;
	unsigned long eax, edx;

	__asm__ __volatile__("rdtsc" : "=a"(eax), "=d"(edx));
	val = ((uint64_t)edx<<32)|(eax);
	return val;
}

void
bmk_x86_initclocks(void)
{
	uint64_t tsc_freq;

	/*
	 * map clock interrupt.
	 * note, it's still disabled in the PIC, we only enable it
	 * during nanohlt
	 */
	bmk_x86_fillgate(32, bmk_cpu_isr_clock, 0);

	/* Initialise i8254 timer channel 0 to mode 2 at 100 Hz */
	outb(TIMER_MODE, TIMER_SEL0 | TIMER_RATEGEN | TIMER_16BIT);
	outb(TIMER_CNTR, (TIMER_HZ / HZ) & 0xff);
	outb(TIMER_CNTR, (TIMER_HZ / HZ) >> 8);

	/*
	 * Calculate TSC frequency by calibrating against an 0.1s delay
	 * using the i8254 timer.
	 */
	tsc_base = rdtsc();
	i8254_delay(100000);
	tsc_freq = (rdtsc() - tsc_base) * 10;

	/*
	 * Calculate TSC scaling multiplier and initialiase time_base.
	 */
	tsc_mult = (NSEC_PER_SEC << TSC_SHIFT) / tsc_freq;
	time_base = (tsc_base * tsc_mult) >> TSC_SHIFT;
}

void
bmk_isr_clock(void)
{

	/*
	 * Nothing to do here, clock interrupt serves only as a way to wake
	 * the CPU from halt state.
	 */
}

bmk_time_t
bmk_cpu_clock_now(void)
{
	uint64_t tsc_now, tsc_delta;

	/*
	 * Update time_base (monotonic time) and tsc_base (TSC time).
	 * Ensure to use a delta between now and the last call to
	 * bmk_cpu_clock_now() to prevent overflow.
	 * XXX: Document when overflow would happen.
	 */
	tsc_now = rdtsc();
	tsc_delta = tsc_now - tsc_base;
	time_base += (tsc_delta * tsc_mult) >> TSC_SHIFT;
	tsc_base = tsc_now;

	return time_base;
}

void
bmk_cpu_block(bmk_time_t until)
{

	bmk_cpu_nanohlt();
}

void
bmk_cpu_nanohlt(void)
{

	/*
	 * Enable clock interrupt and wait for the next whichever interrupt.
	 */
	outb(PIC1_DATA, 0xff & ~(1<<2|1<<0));
	hlt();
	outb(PIC1_DATA, 0xff & ~(1<<2));
}