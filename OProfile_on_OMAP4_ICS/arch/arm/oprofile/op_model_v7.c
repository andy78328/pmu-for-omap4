/**
 * op_model_v7.c
 * ARM V7 (Cortex A8) Event Monitor Driver
 *
 * Copyright 2008 Jean Pihet <jpihet@mvista.com>
 * Copyright 2004 ARM SMP Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/oprofile.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/smp.h>

#include "op_counter.h"
#include "op_arm_model.h"
#include "op_model_v7.h"

#define DEBUG
#include <linux/io.h>
#include <mach/hardware.h>

#define OMAP44XX_IRQ_GIC_START			32
#define OMAP44XX_IRQ_CTI0			(1 + OMAP44XX_IRQ_GIC_START)	/* TRIGOUT[6] of Cross Trigger Interface 0 (CTI0) */
#define OMAP44XX_IRQ_CTI1			(2 + OMAP44XX_IRQ_GIC_START)	/* TRIGOUT[6] of Cross Trigger Interface 1 (CTI1) */

#define CM_L3INSTR_L3_3_CLKCTRL         0x4A008E20
#define CM_L3INSTR_L3_INSTR_CLKCTRL     0x4A008E28
#define CM_EMU_CLKSTCTRL		0x4a307a00

static unsigned long CTI[2] = {0x54148000, 0x54149000};

/*
 * ARM V7 PMNC support
 */

static u32 cnt_en[CNTMAX];

static inline void armv7_pmnc_write(u32 val)
{
	val &= PMNC_MASK;
	asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r" (val));
}

static inline u32 armv7_pmnc_read(void)
{
	u32 val;

	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r" (val));
	return val;
}

static inline u32 armv7_pmnc_enable_counter(unsigned int cnt)
{
	u32 val;

	if (cnt >= CNTMAX) {
		printk(KERN_ERR "oprofile: CPU%u enabling wrong PMNC counter"
			" %d\n", smp_processor_id(), cnt);
		return -1;
	}

	if (cnt == CCNT)
		val = CNTENS_C;
	else
		val = (1 << (cnt - CNT0));

	val &= CNTENS_MASK;
	asm volatile("mcr p15, 0, %0, c9, c12, 1" : : "r" (val));

	return cnt;
}

static inline u32 armv7_pmnc_disable_counter(unsigned int cnt)
{
	u32 val;

	if (cnt >= CNTMAX) {
		printk(KERN_ERR "oprofile: CPU%u disabling wrong PMNC counter"
			" %d\n", smp_processor_id(), cnt);
		return -1;
	}

	if (cnt == CCNT)
		val = CNTENC_C;
	else
		val = (1 << (cnt - CNT0));

	val &= CNTENC_MASK;
	asm volatile("mcr p15, 0, %0, c9, c12, 2" : : "r" (val));

	return cnt;
}

static inline u32 armv7_pmnc_enable_intens(unsigned int cnt)
{
	u32 val;

	if (cnt >= CNTMAX) {
		printk(KERN_ERR "oprofile: CPU%u enabling wrong PMNC counter"
			" interrupt enable %d\n", smp_processor_id(), cnt);
		return -1;
	}

	if (cnt == CCNT)
		val = INTENS_C;
	else
		val = (1 << (cnt - CNT0));

	val &= INTENS_MASK;
	asm volatile("mcr p15, 0, %0, c9, c14, 1" : : "r" (val));

	return cnt;
}

static inline u32 armv7_pmnc_getreset_flags(void)
{
	u32 val;

	/* Read */
	asm volatile("mrc p15, 0, %0, c9, c12, 3" : "=r" (val));

	/* Write to clear flags */
	val &= FLAG_MASK;
	asm volatile("mcr p15, 0, %0, c9, c12, 3" : : "r" (val));

	return val;
}

static inline int armv7_pmnc_select_counter(unsigned int cnt)
{
	u32 val;

	if ((cnt == CCNT) || (cnt >= CNTMAX)) {
		printk(KERN_ERR "oprofile: CPU%u selecting wrong PMNC counteri"
			" %d\n", smp_processor_id(), cnt);
		return -1;
	}

	val = (cnt - CNT0) & SELECT_MASK;
	asm volatile("mcr p15, 0, %0, c9, c12, 5" : : "r" (val));

	return cnt;
}

static inline void armv7_pmnc_write_evtsel(unsigned int cnt, u32 val)
{
	if (armv7_pmnc_select_counter(cnt) == cnt) {
		val &= EVTSEL_MASK;
		asm volatile("mcr p15, 0, %0, c9, c13, 1" : : "r" (val));
	}
}

static void armv7_pmnc_reset_counter(unsigned int cnt)
{
	u32 cpu_cnt = CPU_COUNTER(smp_processor_id(), cnt);
	u32 val = -(u32)counter_config[cpu_cnt].count;

	switch (cnt) {
	case CCNT:
		armv7_pmnc_disable_counter(cnt);

		asm volatile("mcr p15, 0, %0, c9, c13, 0" : : "r" (val));

		if (cnt_en[cnt] != 0)
		    armv7_pmnc_enable_counter(cnt);

		break;

	case CNT0:
	case CNT1:
	case CNT2:
	case CNT3:
	case CNT4:
	case CNT5:
		armv7_pmnc_disable_counter(cnt);

		if (armv7_pmnc_select_counter(cnt) == cnt)
		    asm volatile("mcr p15, 0, %0, c9, c13, 2" : : "r" (val));

		if (cnt_en[cnt] != 0)
		    armv7_pmnc_enable_counter(cnt);

		break;

	default:
		printk(KERN_ERR "oprofile: CPU%u resetting wrong PMNC counter"
			" %d\n", smp_processor_id(), cnt);
		break;
	}
}

int armv7_setup_pmnc(void)
{
	unsigned int cnt;

	if (armv7_pmnc_read() & PMNC_E) {
		printk(KERN_ERR "oprofile: CPU%u PMNC still enabled when setup"
			" new event counter.\n", smp_processor_id());
		return -EBUSY;
	}

	/* Initialize & Reset PMNC: C bit and P bit */
	armv7_pmnc_write(PMNC_P | PMNC_C);


	for (cnt = CCNT; cnt < CNTMAX; cnt++) {
		unsigned long event;
		u32 cpu_cnt = CPU_COUNTER(smp_processor_id(), cnt);

		/*
		 * Disable counter
		 */
		armv7_pmnc_disable_counter(cnt);
		cnt_en[cnt] = 0;

		if (!counter_config[cpu_cnt].enabled)
			continue;

		event = counter_config[cpu_cnt].event & 255;

		/*
		 * Set event (if destined for PMNx counters)
		 * We don't need to set the event if it's a cycle count
		 */
		if (cnt != CCNT)
			armv7_pmnc_write_evtsel(cnt, event);

		/*
		 * Enable interrupt for this counter
		 */
		armv7_pmnc_enable_intens(cnt);

		/*
		 * Reset counter
		 */
		armv7_pmnc_reset_counter(cnt);

		/*
		 * Enable counter
		 */
		armv7_pmnc_enable_counter(cnt);
		cnt_en[cnt] = 1;
	}

	return 0;
}

static inline void armv7_start_pmnc(void)
{
	armv7_pmnc_write(armv7_pmnc_read() | PMNC_E);
}

static inline void armv7_stop_pmnc(void)
{
	armv7_pmnc_write(armv7_pmnc_read() & ~PMNC_E);
}

/*
 * CPU counters' IRQ handler (one IRQ per CPU)
 */
static irqreturn_t armv7_pmnc_interrupt(int irq, void *arg)
{
	struct pt_regs *regs = get_irq_regs();
	unsigned int cnt, p = smp_processor_id();
	u32 flags;


	/*
	 * Stop IRQ generation
	 */
	armv7_stop_pmnc();
	__raw_writel(1 << 6,  OMAP2_EMU_IO_ADDRESS(CTI[p]+0x010));

	/*
	 * Get and reset overflow status flags
	 */
	flags = armv7_pmnc_getreset_flags();

	/*
	 * Cycle counter
	 */
	if (flags & FLAG_C) {
		u32 cpu_cnt = CPU_COUNTER(smp_processor_id(), CCNT);
		armv7_pmnc_reset_counter(CCNT);
		oprofile_add_sample(regs, cpu_cnt);
	}

	/*
	 * PMNC counters 0:3
	 */
	for (cnt = CNT0; cnt < CNTMAX; cnt++) {
		if (flags & (1 << (cnt - CNT0))) {
			u32 cpu_cnt = CPU_COUNTER(smp_processor_id(), cnt);
			armv7_pmnc_reset_counter(cnt);
			oprofile_add_sample(regs, cpu_cnt);
		}
	}

	/*
	 * Allow IRQ generation
	 */
	armv7_start_pmnc();

	return IRQ_HANDLED;
}

int armv7_request_interrupts(int *irqs, int nr)
{
	unsigned int i, p = smp_processor_id();
	int ret = 0;

	for (i = 0; i < nr; i++) {
		ret = request_irq(irqs[i], armv7_pmnc_interrupt,
				IRQF_DISABLED, "CP15 PMNC", NULL);
		if (ret != 0) {
			printk(KERN_ERR "oprofile: unable to request IRQ%u"
				" for ARMv7\n",
			       irqs[i]);
			break;
		}
		irq_set_affinity(irqs[i], cpumask_of(p));
	}

	if (i != nr)
		while (i-- != 0)
			free_irq(irqs[i], NULL);

	if (i == nr) {
		omap_writel(1, CM_L3INSTR_L3_3_CLKCTRL);
		omap_writel(1, CM_L3INSTR_L3_INSTR_CLKCTRL);
		omap_writel(2, CM_EMU_CLKSTCTRL);
		while ((omap_readl(CM_EMU_CLKSTCTRL) & 0x300) != 0x300);

		__raw_writel(0xc5acce55, OMAP2_EMU_IO_ADDRESS(CTI[p]+0xFB0));
		__raw_writel(1 << (2 + p),  OMAP2_EMU_IO_ADDRESS(CTI[p]+0x024));
		__raw_writel(1 << (2 + p),  OMAP2_EMU_IO_ADDRESS(CTI[p]+0x0B8));
		__raw_writel(0x1, OMAP2_EMU_IO_ADDRESS(CTI[p]));
	}

	return ret;
}

void armv7_release_interrupts(int *irqs, int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++)
		free_irq(irqs[i], NULL);
}

#ifdef DEBUG
static void armv7_pmnc_dump_regs(void)
{
	u32 val;
	unsigned int cnt;

	printk(KERN_INFO "PMNC registers dump CPU %d:\n", smp_processor_id());

	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r" (val));
	printk(KERN_INFO "PMNC  =0x%08x\n", val);

	asm volatile("mrc p15, 0, %0, c9, c12, 1" : "=r" (val));
	printk(KERN_INFO "CNTENS=0x%08x\n", val);

	asm volatile("mrc p15, 0, %0, c9, c14, 1" : "=r" (val));
	printk(KERN_INFO "INTENS=0x%08x\n", val);

	asm volatile("mrc p15, 0, %0, c9, c12, 3" : "=r" (val));
	printk(KERN_INFO "FLAGS =0x%08x\n", val);

	asm volatile("mrc p15, 0, %0, c9, c12, 5" : "=r" (val));
	printk(KERN_INFO "SELECT=0x%08x\n", val);

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r" (val));
	printk(KERN_INFO "CCNT  =0x%08x\n", val);

	for (cnt = CNT0; cnt < CNTMAX; cnt++) {
		armv7_pmnc_select_counter(cnt);
		asm volatile("mrc p15, 0, %0, c9, c13, 2" : "=r" (val));
		printk(KERN_INFO "CNT[%d] count =0x%08x\n", cnt-CNT0, val);
		asm volatile("mrc p15, 0, %0, c9, c13, 1" : "=r" (val));
		printk(KERN_INFO "CNT[%d] evtsel=0x%08x\n", cnt-CNT0, val);
	}
}
#endif

struct em_function_data {
	int (*fn)(void);
	int ret;
};

static void em_func(void *data)
{
	struct em_function_data *d = data;
	int ret = d->fn();
	if (ret)
		d->ret = ret;
}

static int em_call_function(int (*fn)(void))
{
	struct em_function_data data;

	data.fn = fn;
	data.ret = 0;

	preempt_disable();
	smp_call_function(em_func, &data, 1);
	em_func(&data);
	preempt_enable();

	return data.ret;
}

static int irqs[] = {
#ifdef CONFIG_ARCH_OMAP3
	INT_34XX_BENCH_MPU_EMUL,
#endif
#ifdef CONFIG_ARCH_OMAP4
	OMAP44XX_IRQ_CTI0,
	OMAP44XX_IRQ_CTI1
#endif
};

static void armv7_pmnc_stop(void)
{
#ifdef DEBUG
	armv7_pmnc_dump_regs();
#endif
	armv7_stop_pmnc();
	armv7_release_interrupts(&irqs[smp_processor_id()], 1);
}

static int armv7_pmnc_start(void)
{
	int ret=0;

#ifdef DEBUG
	armv7_pmnc_dump_regs();
#endif
	ret = armv7_request_interrupts(&irqs[smp_processor_id()], 1);
	if (ret >= 0)
		armv7_start_pmnc();

	return ret;
}

static int armv7_detect_pmnc(void)
{
	return 0;
}

static void em_armv7_pmnc_stop(void)
{
	em_call_function((int (*)(void))armv7_pmnc_stop);
}

static int em_armv7_pmnc_start(void)
{
	return em_call_function(armv7_pmnc_start);
}

static int em_armv7_setup_pmnc(void)
{
	return em_call_function(armv7_setup_pmnc);
}

struct op_arm_model_spec op_armv7_spec = {
	.init		= armv7_detect_pmnc,
	.num_counters	= CNTMAX,
	.setup_ctrs	= em_armv7_setup_pmnc,
	.start		= em_armv7_pmnc_start,
	.stop		= em_armv7_pmnc_stop,
	.name		= "arm/armv7",
};
