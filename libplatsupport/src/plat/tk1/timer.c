/*
 * Copyright 2016, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */


#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <utils/util.h>

#include <platsupport/timer.h>
#include <platsupport/plat/timer.h>

/* enable bit */
#define PVT_E_BIT           31

/* enable auto-reload for periodic mode */
#define PVT_PERIODIC_E_BIT  30

/* 0-28 bits are for value, n + 1 trigger mode */
#define PVT_VAL_MASK        0x1fffffff 
#define PVT_VAL_BITS        29

/* write 1 to clear interruts */
#define PCR_INTR_CLR_BIT    30

/* counter value: decrements from PVT */
#define PCR_VAL_MASK        0x1fffffff
/* counter width is 29 bits */

#define CLK_FREQ_MHZ        12
#define CLK_FREQ_HZ         12000000ull

struct tmr_map {
    uint32_t    pvt;    /* present trigger value */
    uint32_t    pcr;    /* present count value */
};

struct tmr_shared_map {
    uint32_t    intr_status;
    uint32_t    secure_cfg;
};

struct tmrus_map {
    /* A free-running read-only counter changes once very microsecond */
    uint32_t    cntr_1us;
    /* configure this regsiter by telling what fraction of 1 microsecond
     * each clk_m represents. if the clk_m is running at 12 MHz, then
     * each clm_m represent 1/12 of a microsecond.*/
    uint32_t    usec_cfg;
    uint32_t    cntr_freeze;
};

typedef enum {
    PERIODIC,
    ONESHOT
} tmr_mode_t;

typedef struct tmr {
    volatile struct tmr_map         *tmr_map;
    volatile struct tmrus_map       *tmrus_map;
    volatile struct tmr_shared_map  *tmr_shared_map;
    uint64_t                        counter_start;
    int                             mode;
    uint32_t                        irq;
} tmr_t;


static int
tmr_start(const pstimer_t *timer)
{
    tmr_t *tmr = (tmr_t *)timer->data;
    tmr->tmr_map->pvt |= BIT(PVT_E_BIT);

    return 0;
}


static int
tmr_stop(const pstimer_t *timer)
{
    tmr_t *tmr = (tmr_t *)timer->data;
    tmr->tmr_map->pvt &= ~(BIT(PVT_E_BIT));

    return 0;
}

#define INVALID_PVT_VAL 0x80000000

static uint32_t
get_ticks(uint64_t ns)
{
    uint64_t microsecond = ns / 1000ull;
    uint64_t ticks = microsecond * CLK_FREQ_MHZ;
    if (ticks >= BIT(PVT_VAL_BITS)) {
        ZF_LOGE("ns too high %llu\n", ns);
        return INVALID_PVT_VAL;
    }
    return (uint32_t)ticks;
}

static int
tmr_oneshot_absolute(const pstimer_t *timer UNUSED, uint64_t ns UNUSED)
{
    /* epit is a count-down timer, can't set relative timeouts */
    return ENOSYS;
}


static int
tmr_periodic(const pstimer_t *timer, uint64_t ns)
{
    tmr_t *tmr = (tmr_t *)timer->data;
    uint32_t ticks = get_ticks(ns);
    if (ticks == INVALID_PVT_VAL) {
        return EINVAL;
    }
    tmr->mode = PERIODIC;
    tmr->tmr_map->pvt = BIT(PVT_E_BIT) | BIT(PVT_PERIODIC_E_BIT) | ticks;
    return 0;
}

static int
tmr_oneshot_relative(const pstimer_t *timer, uint64_t ns)
{
    tmr_t *tmr = (tmr_t *)timer->data;
    uint32_t ticks = get_ticks(ns);
    if (ticks == INVALID_PVT_VAL) {
        return EINVAL;
    }
    tmr->mode = ONESHOT;
    tmr->tmr_map->pcr |= BIT(PCR_INTR_CLR_BIT);
    tmr->tmr_map->pvt = BIT(PVT_E_BIT) | ticks;
    return 0;
}

static void
tmr_handle_irq(const pstimer_t *timer, uint32_t irq)
{
    tmr_t *tmr = (tmr_t *)timer->data;
    tmr->tmr_map->pcr |= BIT(PCR_INTR_CLR_BIT);
    if (tmr->mode != PERIODIC) {
        tmr->tmr_map->pvt &= ~(BIT(PVT_E_BIT));
    }
}

static uint64_t
tmr_get_time(const pstimer_t *timer)
{
    return 0;
}

static uint32_t
tmr_get_nth_irq(const pstimer_t *timer, uint32_t n)
{
    switch (n) {
        case TMR0:
            return INT_NV_TMR0;
        case TMR1:
            return INT_NV_TMR1;
        case TMR2:
            return INT_NV_TMR2;
        case TMR3:
            return INT_NV_TMR3;
        case TMR4:
            return INT_NV_TMR4;
        case TMR5:
            return INT_NV_TMR5;
        case TMR6:
            return INT_NV_TMR6;
        case TMR7:
            return INT_NV_TMR7;
        case TMR8:
            return INT_NV_TMR8;
        case TMR9:
            return INT_NV_TMR9;
        default:
            ZF_LOGE("invalid timer id %d\n", n);
            return 0;
    }
}

static pstimer_t singleton_timer;
static tmr_t     singleton_tmr;

#define TMRUS_USEC_CFG_DEFAULT   0xb

pstimer_t *
tk1_get_timer(nv_tmr_config_t *config)
{
    switch (config->irq) {

        case INT_NV_TMR0:
        case INT_NV_TMR1:
        case INT_NV_TMR2:
        case INT_NV_TMR3:
        case INT_NV_TMR4:
        case INT_NV_TMR5:
        case INT_NV_TMR6:
        case INT_NV_TMR7:
        case INT_NV_TMR8:
        case INT_NV_TMR9:
             break;

        default:
             fprintf(stderr, "Invalid irq %u for NV timer\n", config->irq);
             return NULL;
    }

    pstimer_t *timer = &singleton_timer;
    tmr_t *tmr = &singleton_tmr;
    
    timer->properties.upcounter = false;
    timer->properties.timeouts = true;
    timer->properties.bit_width = 0;
    timer->properties.irqs = 1;

    timer->data = (void *)tmr;
    timer->start = tmr_start;
    timer->stop = tmr_stop;
    timer->get_time = tmr_get_time;
    timer->oneshot_absolute = tmr_oneshot_absolute;
    timer->oneshot_relative = tmr_oneshot_relative;
    timer->periodic = tmr_periodic;
    timer->handle_irq = tmr_handle_irq;
    timer->get_nth_irq = tmr_get_nth_irq;
    
    tmr->irq = config->irq;
    tmr->tmr_map = (volatile struct tmr_map *)config->vaddr;
    tmr->tmrus_map = (volatile struct tmrus_map *)config->tmrus_vaddr;
    tmr->tmr_shared_map = (volatile struct tmr_shared_map *)config->shared_vaddr;
    
    tmr->tmr_map->pvt = 0;
    tmr->tmr_map->pcr = BIT(PCR_INTR_CLR_BIT);
    if (tmr->tmrus_map->usec_cfg != TMRUS_USEC_CFG_DEFAULT) {
        ZF_LOGE("Check your clock frequency and configure USEC_CFG registers accordingly");
    }

    return timer;
}

