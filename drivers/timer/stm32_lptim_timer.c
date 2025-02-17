/*
 * Copyright (c) 2018 Foundries.io Ltd
 * Copyright (c) 2019 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <soc.h>
#include <stm32_ll_lptim.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_rcc.h>
#include <stm32_ll_pwr.h>
#include <stm32_ll_system.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/sys_clock.h>

#include <zephyr/spinlock.h>

/*
 * Assumptions and limitations:
 *
 * - system clock based on an LPTIM1 instance, clocked by LSI or LSE
 * - prescaler is set to 1 (LL_LPTIM_PRESCALER_DIV1 in the related register)
 * - using LPTIM1 AutoReload capability to trig the IRQ (timeout irq)
 * - when timeout irq occurs the counter is already reset
 * - the maximum timeout duration is reached with the LPTIM_TIMEBASE value
 * - with prescaler of 1, the max timeout (LPTIM_TIMEBASE) is 2seconds
 */

#define LPTIM_CLOCK CONFIG_STM32_LPTIM_CLOCK
#define LPTIM_TIMEBASE CONFIG_STM32_LPTIM_TIMEBASE

/* nb of LPTIM counter unit per kernel tick  */
#define COUNT_PER_TICK (LPTIM_CLOCK / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

/* minimum nb of clock cycles to have to set autoreload register correctly */
#define LPTIM_GUARD_VALUE 2

/* A 32bit value cannot exceed 0xFFFFFFFF/LPTIM_TIMEBASE counting cycles.
 * This is for example about of 65000 x 2000ms when clocked by LSI
 */
static uint32_t accumulated_lptim_cnt;
/* Next autoreload value to set */
static uint32_t autoreload_next;
/* Indicate if the autoreload register is ready for a write */
static bool autoreload_ready = true;

static struct k_spinlock lock;

static void lptim_irq_handler(const struct device *unused)
{

	ARG_UNUSED(unused);

	uint32_t autoreload = LL_LPTIM_GetAutoReload(LPTIM1);

	if ((LL_LPTIM_IsActiveFlag_ARROK(LPTIM1) != 0)
		&& LL_LPTIM_IsEnabledIT_ARROK(LPTIM1) != 0) {
		LL_LPTIM_ClearFlag_ARROK(LPTIM1);
		if ((autoreload_next > 0) && (autoreload_next != autoreload)) {
			/* the new autoreload value change, we set it */
			autoreload_ready = false;
			LL_LPTIM_SetAutoReload(LPTIM1, autoreload_next);
		} else {
			autoreload_ready = true;
		}
	}

	if ((LL_LPTIM_IsActiveFlag_ARRM(LPTIM1) != 0)
		&& LL_LPTIM_IsEnabledIT_ARRM(LPTIM1) != 0) {

		k_spinlock_key_t key = k_spin_lock(&lock);

		/* do not change ARR yet, sys_clock_announce will do */
		LL_LPTIM_ClearFLAG_ARRM(LPTIM1);

		/* increase the total nb of autoreload count
		 * used in the sys_clock_cycle_get_32() function.
		 */
		autoreload++;

		accumulated_lptim_cnt += autoreload;

		k_spin_unlock(&lock, key);

		/* announce the elapsed time in ms (count register is 16bit) */
		uint32_t dticks = (autoreload
				* CONFIG_SYS_CLOCK_TICKS_PER_SEC)
				/ LPTIM_CLOCK;

		sys_clock_announce(IS_ENABLED(CONFIG_TICKLESS_KERNEL)
				? dticks : (dticks > 0));
	}
}

static void lptim_set_autoreload(uint32_t arr)
{
	/* Update autoreload register */
	autoreload_next = arr;

	if (!autoreload_ready)
		return;

	/* The ARR register ready, we could set it directly */
	if ((arr > 0) && (arr != LL_LPTIM_GetAutoReload(LPTIM1))) {
		/* The new autoreload value change, we set it */
		autoreload_ready = false;
		LL_LPTIM_ClearFlag_ARROK(LPTIM1);
		LL_LPTIM_SetAutoReload(LPTIM1, arr);
	}
}

static inline uint32_t z_clock_lptim_getcounter(void)
{
	uint32_t lp_time;
	uint32_t lp_time_prev_read;

	/* It should be noted that to read reliably the content
	 * of the LPTIM_CNT register, two successive read accesses
	 * must be performed and compared
	 */
	lp_time = LL_LPTIM_GetCounter(LPTIM1);
	do {
		lp_time_prev_read = lp_time;
		lp_time = LL_LPTIM_GetCounter(LPTIM1);
	} while (lp_time != lp_time_prev_read);
	return lp_time;
}

void sys_clock_set_timeout(int32_t ticks, bool idle)
{
	/* new LPTIM1 AutoReload value to set (aligned on Kernel ticks) */
	uint32_t next_arr = 0;

	ARG_UNUSED(idle);

	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		return;
	}

	if (ticks == K_TICKS_FOREVER) {
		/* disable LPTIM clock to avoid counting */
#if defined(LL_APB1_GRP1_PERIPH_LPTIM1)
		LL_APB1_GRP1_DisableClock(LL_APB1_GRP1_PERIPH_LPTIM1);
#elif defined(LL_APB3_GRP1_PERIPH_LPTIM1)
		LL_APB3_GRP1_DisableClock(LL_APB3_GRP1_PERIPH_LPTIM1);
#endif
		return;
	}

	/* if LPTIM clock was previously stopped, it must now be restored */
#if defined(LL_APB1_GRP1_PERIPH_LPTIM1)
	if (!LL_APB1_GRP1_IsEnabledClock(LL_APB1_GRP1_PERIPH_LPTIM1)) {
		LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_LPTIM1);
	}
#elif defined(LL_APB3_GRP1_PERIPH_LPTIM1)
	if (!LL_APB3_GRP1_IsEnabledClock(LL_APB3_GRP1_PERIPH_LPTIM1)) {
		LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM1);
	}
#endif

	/* passing ticks==1 means "announce the next tick",
	 * ticks value of zero (or even negative) is legal and
	 * treated identically: it simply indicates the kernel would like the
	 * next tick announcement as soon as possible.
	 */
	ticks = CLAMP(ticks - 1, 1, (int32_t)LPTIM_TIMEBASE);

	k_spinlock_key_t key = k_spin_lock(&lock);

	/* read current counter value (cannot exceed 16bit) */

	uint32_t lp_time = z_clock_lptim_getcounter();

	uint32_t autoreload = LL_LPTIM_GetAutoReload(LPTIM1);

	if (LL_LPTIM_IsActiveFlag_ARRM(LPTIM1)
	    || ((autoreload - lp_time) < LPTIM_GUARD_VALUE)) {
		/* interrupt happens or happens soon.
		 * It's impossible to set autoreload value.
		 */
		k_spin_unlock(&lock, key);
		return;
	}

	/* calculate the next arr value (cannot exceed 16bit)
	 * adjust the next ARR match value to align on Ticks
	 * from the current counter value to first next Tick
	 */
	next_arr = (((lp_time * CONFIG_SYS_CLOCK_TICKS_PER_SEC)
			/ LPTIM_CLOCK) + 1) * LPTIM_CLOCK
			/ (CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	/* add count unit from the expected nb of Ticks */
	next_arr = next_arr + ((uint32_t)(ticks) * LPTIM_CLOCK)
			/ CONFIG_SYS_CLOCK_TICKS_PER_SEC - 1;

	/* maximise to TIMEBASE */
	if (next_arr > LPTIM_TIMEBASE) {
		next_arr = LPTIM_TIMEBASE;
	}
	/* The new autoreload value must be LPTIM_GUARD_VALUE clock cycles
	 * after current lptim to make sure we don't miss
	 * an autoreload interrupt
	 */
	else if (next_arr < (lp_time + LPTIM_GUARD_VALUE)) {
		next_arr = lp_time + LPTIM_GUARD_VALUE;
	}

	/* Update autoreload register */
	lptim_set_autoreload(next_arr);

	k_spin_unlock(&lock, key);
}

uint32_t sys_clock_elapsed(void)
{
	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	uint32_t lp_time = z_clock_lptim_getcounter();

	/* In case of counter roll-over, add this value,
	 * even if the irq has not yet been handled
	 */
	if ((LL_LPTIM_IsActiveFlag_ARRM(LPTIM1) != 0)
	  && LL_LPTIM_IsEnabledIT_ARRM(LPTIM1) != 0) {
		lp_time += LL_LPTIM_GetAutoReload(LPTIM1) + 1;
	}

	k_spin_unlock(&lock, key);

	/* gives the value of LPTIM1 counter (ms)
	 * since the previous 'announce'
	 */
	uint64_t ret = ((uint64_t)lp_time * CONFIG_SYS_CLOCK_TICKS_PER_SEC) / LPTIM_CLOCK;

	return (uint32_t)(ret);
}

uint32_t sys_clock_cycle_get_32(void)
{
	/* just gives the accumulated count in a number of hw cycles */

	k_spinlock_key_t key = k_spin_lock(&lock);

	uint32_t lp_time = z_clock_lptim_getcounter();

	/* In case of counter roll-over, add this value,
	 * even if the irq has not yet been handled
	 */
	if ((LL_LPTIM_IsActiveFlag_ARRM(LPTIM1) != 0)
	  && LL_LPTIM_IsEnabledIT_ARRM(LPTIM1) != 0) {
		lp_time += LL_LPTIM_GetAutoReload(LPTIM1) + 1;
	}

	lp_time += accumulated_lptim_cnt;

	/* convert lptim count in a nb of hw cycles with precision */
	uint64_t ret = ((uint64_t)lp_time * sys_clock_hw_cycles_per_sec()) / LPTIM_CLOCK;

	k_spin_unlock(&lock, key);

	/* convert in hw cycles (keeping 32bit value) */
	return (uint32_t)(ret);
}

static int sys_clock_driver_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	/* enable LPTIM clock source */
#if defined(LL_APB1_GRP1_PERIPH_LPTIM1)
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_LPTIM1);
	LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_LPTIM1);
#elif defined(LL_APB3_GRP1_PERIPH_LPTIM1)
	LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM1);
	LL_SRDAMR_GRP1_EnableAutonomousClock(LL_SRDAMR_GRP1_PERIPH_LPTIM1AMEN);
#endif

#if defined(CONFIG_STM32_LPTIM_CLOCK_LSI)
	/* enable LSI clock */
#ifdef CONFIG_SOC_SERIES_STM32WBX
	LL_RCC_LSI1_Enable();
	while (!LL_RCC_LSI1_IsReady()) {
#else
	LL_RCC_LSI_Enable();
	while (!LL_RCC_LSI_IsReady()) {
#endif /* CONFIG_SOC_SERIES_STM32WBX */
		/* Wait for LSI ready */
	}

	LL_RCC_SetLPTIMClockSource(LL_RCC_LPTIM1_CLKSOURCE_LSI);

#else /* CONFIG_STM32_LPTIM_CLOCK_LSI */
#if defined(LL_APB1_GRP1_PERIPH_PWR)
	/* Enable the power interface clock */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
#endif /* LL_APB1_GRP1_PERIPH_PWR */

	/* enable backup domain */
	LL_PWR_EnableBkUpAccess();

	/* enable LSE clock */
	LL_RCC_LSE_DisableBypass();
#ifdef RCC_BDCR_LSEDRV_Pos
	LL_RCC_LSE_SetDriveCapability(STM32_LSE_DRIVING << RCC_BDCR_LSEDRV_Pos);
#endif
	LL_RCC_LSE_Enable();
	while (!LL_RCC_LSE_IsReady()) {
		/* Wait for LSE ready */
	}
#ifdef RCC_BDCR_LSESYSEN
	LL_RCC_LSE_EnablePropagation();
#endif /* RCC_BDCR_LSESYSEN */
	LL_RCC_SetLPTIMClockSource(LL_RCC_LPTIM1_CLKSOURCE_LSE);

#endif /* CONFIG_STM32_LPTIM_CLOCK_LSI */

	/* Clear the event flag and possible pending interrupt */
	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(lptim1)),
		    DT_IRQ(DT_NODELABEL(lptim1), priority),
		    lptim_irq_handler, 0, 0);
	irq_enable(DT_IRQN(DT_NODELABEL(lptim1)));

#ifdef CONFIG_SOC_SERIES_STM32WLX
	/* Enable the LPTIM1 wakeup EXTI line */
	LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_29);
#endif

	/* configure the LPTIM1 counter */
	LL_LPTIM_SetClockSource(LPTIM1, LL_LPTIM_CLK_SOURCE_INTERNAL);
	/* configure the LPTIM1 prescaler with 1 */
	LL_LPTIM_SetPrescaler(LPTIM1, LL_LPTIM_PRESCALER_DIV1);
#ifdef CONFIG_SOC_SERIES_STM32U5X
	LL_LPTIM_OC_SetPolarity(LPTIM1, LL_LPTIM_CHANNEL_CH1,
				LL_LPTIM_OUTPUT_POLARITY_REGULAR);
#else
	LL_LPTIM_SetPolarity(LPTIM1, LL_LPTIM_OUTPUT_POLARITY_REGULAR);
#endif
	LL_LPTIM_SetUpdateMode(LPTIM1, LL_LPTIM_UPDATE_MODE_IMMEDIATE);
	LL_LPTIM_SetCounterMode(LPTIM1, LL_LPTIM_COUNTER_MODE_INTERNAL);
	LL_LPTIM_DisableTimeout(LPTIM1);
	/* counting start is initiated by software */
	LL_LPTIM_TrigSw(LPTIM1);

#ifdef CONFIG_SOC_SERIES_STM32U5X
	/* Enable the LPTIM1 before proceeding with configuration */
	LL_LPTIM_Enable(LPTIM1);

	LL_LPTIM_DisableIT_CC1(LPTIM1);
	while (LL_LPTIM_IsActiveFlag_DIEROK(LPTIM1) == 0) {
	}
	LL_LPTIM_ClearFlag_DIEROK(LPTIM1);
	LL_LPTIM_ClearFLAG_CC1(LPTIM1);
#else
	/* LPTIM1 interrupt set-up before enabling */
	/* no Compare match Interrupt */
	LL_LPTIM_DisableIT_CMPM(LPTIM1);
	LL_LPTIM_ClearFLAG_CMPM(LPTIM1);
#endif

	/* Autoreload match Interrupt */
	LL_LPTIM_EnableIT_ARRM(LPTIM1);
#ifdef CONFIG_SOC_SERIES_STM32U5X
	while (LL_LPTIM_IsActiveFlag_DIEROK(LPTIM1) == 0) {
	}
	LL_LPTIM_ClearFlag_DIEROK(LPTIM1);
#endif
	LL_LPTIM_ClearFLAG_ARRM(LPTIM1);
	/* ARROK bit validates the write operation to ARR register */
	LL_LPTIM_EnableIT_ARROK(LPTIM1);
	LL_LPTIM_ClearFlag_ARROK(LPTIM1);

	accumulated_lptim_cnt = 0;

#ifndef CONFIG_SOC_SERIES_STM32U5X
	/* Enable the LPTIM1 counter */
	LL_LPTIM_Enable(LPTIM1);
#endif

	/* Set the Autoreload value once the timer is enabled */
	if (IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		/* LPTIM1 is triggered on a LPTIM_TIMEBASE period */
		lptim_set_autoreload(LPTIM_TIMEBASE);
	} else {
		/* LPTIM1 is triggered on a Tick period */
		lptim_set_autoreload(COUNT_PER_TICK - 1);
	}

	/* Start the LPTIM counter in continuous mode */
	LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_CONTINUOUS);

#ifdef CONFIG_DEBUG
	/* stop LPTIM1 during DEBUG */
#if defined(LL_DBGMCU_APB1_GRP1_LPTIM1_STOP)
	LL_DBGMCU_APB1_GRP1_FreezePeriph(LL_DBGMCU_APB1_GRP1_LPTIM1_STOP);
#elif defined(LL_DBGMCU_APB3_GRP1_LPTIM1_STOP)
	LL_DBGMCU_APB3_GRP1_FreezePeriph(LL_DBGMCU_APB3_GRP1_LPTIM1_STOP);
#endif

#endif
	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2,
	 CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
