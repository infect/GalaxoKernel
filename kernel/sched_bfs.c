/*
 *  kernel/sched_bfs.c, was sched.c
 *
 *  Kernel scheduler and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *
 *  1996-12-23  Modified by Dave Grothe to fix bugs in semaphores and
 *		make semaphores SMP safe
 *  1998-11-19	Implemented schedule_timeout() and related stuff
 *		by Andrea Arcangeli
 *  2002-01-04	New ultra-scalable O(1) scheduler by Ingo Molnar:
 *		hybrid priority-list and round-robin design with
 *		an array-switch method of distributing timeslices
 *		and per-CPU runqueues.  Cleanups and useful suggestions
 *		by Davide Libenzi, preemptible kernel bits by Robert Love.
 *  2003-09-03	Interactivity tuning by Con Kolivas.
 *  2004-04-02	Scheduler domains code by Nick Piggin
 *  2007-04-15  Work begun on replacing all interactivity tuning with a
 *              fair scheduling design by Con Kolivas.
 *  2007-05-05  Load balancing (smp-nice) and other improvements
 *              by Peter Williams
 *  2007-05-06  Interactivity improvements to CFS by Mike Galbraith
 *  2007-07-01  Group scheduling enhancements by Srivatsa Vaddagiri
 *  2007-11-29  RT balancing improvements by Steven Rostedt, Gregory Haskins,
 *              Thomas Gleixner, Mike Kravetz
 *  now		Brainfuck deadline scheduling policy by Con Kolivas deletes
 *              a whole lot of those previous things.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <linux/highmem.h>
#include <linux/smp_lock.h>
#include <asm/mmu_context.h>
#include <linux/interrupt.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/kernel_stat.h>
#include <linux/debug_locks.h>
#include <linux/security.h>
#include <linux/notifier.h>
#include <linux/profile.h>
#include <linux/freezer.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/timer.h>
#include <linux/rcupdate.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/times.h>
#include <linux/tsacct_kern.h>
#include <linux/kprobes.h>
#include <linux/delayacct.h>
#include <linux/reciprocal_div.h>
#include <linux/log2.h>
#include <linux/bootmem.h>
#include <linux/ftrace.h>

#include <asm/tlb.h>
#include <asm/unistd.h>

#include <asm/irq_regs.h>


#define rt_prio(prio)		unlikely((prio) < MAX_RT_PRIO)
#define rt_task(p)		rt_prio((p)->prio)
#define rt_queue(rq)		rt_prio((rq)->rq_prio)
#define batch_task(p)		(unlikely((p)->policy == SCHED_BATCH))
#define is_rt_policy(policy)	((policy) == SCHED_FIFO || \
					(policy) == SCHED_RR)
#define has_rt_policy(p)	unlikely(is_rt_policy((p)->policy))
#define idleprio_task(p)	unlikely((p)->policy == SCHED_IDLEPRIO)
#define iso_task(p)		unlikely((p)->policy == SCHED_ISO)
#define iso_queue(rq)		unlikely((rq)->rq_policy == SCHED_ISO)
#define ISO_PERIOD		((5 * HZ * num_online_cpus()) + 1)

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static priority [ MAX_RT_PRIO..MAX_PRIO-1 ],
 * and back.
 */
#define NICE_TO_PRIO(nice)	(MAX_RT_PRIO + (nice) + 20)
#define PRIO_TO_NICE(prio)	((prio) - MAX_RT_PRIO - 20)
#define TASK_NICE(p)		PRIO_TO_NICE((p)->static_prio)

/*
 * 'User priority' is the nice value converted to something we
 * can work with better when scaling various scheduler parameters,
 * it's a [ 0 ... 39 ] range.
 */
#define USER_PRIO(p)		((p)-MAX_RT_PRIO)
#define TASK_USER_PRIO(p)	USER_PRIO((p)->static_prio)
#define MAX_USER_PRIO		(USER_PRIO(MAX_PRIO))
#define SCHED_PRIO(p)		((p)+MAX_RT_PRIO)

/* Some helpers for converting to/from various scales.*/
#define JIFFIES_TO_NS(TIME)	((TIME) * (1000000000 / HZ))
#define MS_TO_NS(TIME)		((TIME) * 1000000)
#define MS_TO_US(TIME)		((TIME) * 1000)

#ifdef CONFIG_SMP
/*
 * Divide a load by a sched group cpu_power : (load / sg->__cpu_power)
 * Since cpu_power is a 'constant', we can use a reciprocal divide.
 */
static inline u32 sg_div_cpu_power(const struct sched_group *sg, u32 load)
{
	return reciprocal_divide(load, sg->reciprocal_cpu_power);
}

/*
 * Each time a sched group cpu_power is changed,
 * we must compute its reciprocal value
 */
static inline void sg_inc_cpu_power(struct sched_group *sg, u32 val)
{
	sg->__cpu_power += val;
	sg->reciprocal_cpu_power = reciprocal_value(sg->__cpu_power);
}
#endif

/*
 * This is the time all tasks within the same priority round robin.
 * Value is in ms and set to a minimum of 6ms. Scales with number of cpus.
 * Tunable via /proc interface.
 */
int rr_interval __read_mostly = 6;

/*
 * sched_iso_cpu - sysctl which determines the cpu percentage SCHED_ISO tasks
 * are allowed to run five seconds as real time tasks. This is the total over
 * all online cpus.
 */
int sched_iso_cpu __read_mostly = 70;

/*
 * The quota handed out to tasks of all priority levels when refilling their
 * time_slice.
 */
static inline unsigned long timeslice(void)
{
	return MS_TO_US(rr_interval);
}

/*
 * The global runqueue data that all CPUs work off. All data is protected
 * by grq.lock.
 */
struct global_rq {
	spinlock_t lock;
	unsigned long nr_running;
	unsigned long nr_uninterruptible;
	unsigned long long nr_switches;
	struct list_head queue[PRIO_LIMIT];
	DECLARE_BITMAP(prio_bitmap, PRIO_LIMIT + 1);
	unsigned long iso_ticks;
	unsigned short iso_refractory;
#ifdef CONFIG_SMP
	unsigned long qnr; /* queued not running */
	cpumask_t cpu_idle_map;
#endif
};

/* There can be only one */
static struct global_rq grq;

/*
 * This is the main, per-CPU runqueue data structure.
 * This data should only be modified by the local cpu.
 */
struct rq {
#ifdef CONFIG_SMP
#ifdef CONFIG_NO_HZ
	unsigned char in_nohz_recently;
#endif
#endif

	struct task_struct *curr, *idle;
	struct mm_struct *prev_mm;

	/* Stored data about rq->curr to work outside grq lock */
	unsigned long rq_deadline;
	unsigned int rq_policy;
	int rq_time_slice;
	u64 rq_last_ran;
	int rq_prio;

	/* Accurate timekeeping data */
	u64 timekeep_clock;
	unsigned long user_pc, nice_pc, irq_pc, softirq_pc, system_pc,
		iowait_pc, idle_pc;
	atomic_t nr_iowait;

	int cpu;		/* cpu of this runqueue */

#ifdef CONFIG_SMP
	int online;

	struct root_domain *rd;
	struct sched_domain *sd;
	unsigned long *cpu_locality; /* CPU relative cache distance */
#endif

	u64 clock;
#ifdef CONFIG_SCHEDSTATS

	/* latency stats */
	struct sched_info rq_sched_info;

	/* sys_sched_yield() stats */
	unsigned int yld_exp_empty;
	unsigned int yld_act_empty;
	unsigned int yld_both_empty;
	unsigned int yld_count;

	/* schedule() stats */
	unsigned int sched_switch;
	unsigned int sched_count;
	unsigned int sched_goidle;

	/* try_to_wake_up() stats */
	unsigned int ttwu_count;
	unsigned int ttwu_local;

	/* BKL stats */
	unsigned int bkl_count;
#endif
};

static DEFINE_PER_CPU(struct rq, runqueues) ____cacheline_aligned_in_smp;
static DEFINE_MUTEX(sched_hotcpu_mutex);

#ifdef CONFIG_SMP

/*
 * We add the notion of a root-domain which will be used to define per-domain
 * variables. Each exclusive cpuset essentially defines an island domain by
 * fully partitioning the member cpus from any other cpuset. Whenever a new
 * exclusive cpuset is created, we also create and attach a new root-domain
 * object.
 *
 */
struct root_domain {
	atomic_t refcount;
	cpumask_t span;
	cpumask_t online;

	/*
	 * The "RT overload" flag: it gets set if a CPU has more than
	 * one runnable RT task.
	 */
	cpumask_t rto_mask;
	atomic_t rto_count;
};

/*
 * By default the system creates a single root-domain with all cpus as
 * members (mimicking the global state we have today).
 */
static struct root_domain def_root_domain;
#endif

static inline int cpu_of(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->cpu;
#else
	return 0;
#endif
}

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See detach_destroy_domains: synchronize_sched for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference(cpu_rq(cpu)->sd); __sd; __sd = __sd->parent)

#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		(&__get_cpu_var(runqueues))
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)

#include "sched_stats.h"

#ifndef prepare_arch_switch
# define prepare_arch_switch(next)	do { } while (0)
#endif
#ifndef finish_arch_switch
# define finish_arch_switch(prev)	do { } while (0)
#endif

/*
 * All common locking functions performed on grq.lock. rq->clock is local to
 * the cpu accessing it so it can be modified just with interrupts disabled,
 * but looking up task_rq must be done under grq.lock to be safe.
 */
static inline void update_rq_clock(struct rq *rq)
{
	rq->clock = sched_clock_cpu(cpu_of(rq));
}

static inline int task_running(struct task_struct *p)
{
	return (!!p->oncpu);
}

static inline void grq_lock(void)
	__acquires(grq.lock)
{
	spin_lock(&grq.lock);
}

static inline void grq_unlock(void)
	__releases(grq.lock)
{
	spin_unlock(&grq.lock);
}

static inline void grq_lock_irq(void)
	__acquires(grq.lock)
{
	spin_lock_irq(&grq.lock);
}

static inline void time_lock_grq(struct rq *rq)
	__acquires(grq.lock)
{
	update_rq_clock(rq);
	grq_lock();
}

static inline void grq_unlock_irq(void)
	__releases(grq.lock)
{
	spin_unlock_irq(&grq.lock);
}

static inline void grq_lock_irqsave(unsigned long *flags)
	__acquires(grq.lock)
{
	spin_lock_irqsave(&grq.lock, *flags);
}

static inline void grq_unlock_irqrestore(unsigned long *flags)
	__releases(grq.lock)
{
	spin_unlock_irqrestore(&grq.lock, *flags);
}

static inline struct rq
*task_grq_lock(struct task_struct *p, unsigned long *flags)
	__acquires(grq.lock)
{
	grq_lock_irqsave(flags);
	return task_rq(p);
}

static inline struct rq
*time_task_grq_lock(struct task_struct *p, unsigned long *flags)
	__acquires(grq.lock)
{
	struct rq *rq = task_grq_lock(p, flags);
	update_rq_clock(rq);
	return rq;
}

static inline struct rq *task_grq_lock_irq(struct task_struct *p)
	__acquires(grq.lock)
{
	grq_lock_irq();
	return task_rq(p);
}

static inline void time_task_grq_lock_irq(struct task_struct *p)
	__acquires(grq.lock)
{
	struct rq *rq = task_grq_lock_irq(p);
	update_rq_clock(rq);
}

static inline void task_grq_unlock_irq(void)
	__releases(grq.lock)
{
	grq_unlock_irq();
}

static inline void task_grq_unlock(unsigned long *flags)
	__releases(grq.lock)
{
	grq_unlock_irqrestore(flags);
}

/**
 * grunqueue_is_locked
 *
 * Returns true if the global runqueue is locked.
 * This interface allows printk to be called with the runqueue lock
 * held and know whether or not it is OK to wake up the klogd.
 */
inline int grunqueue_is_locked(void)
{
	return spin_is_locked(&grq.lock);
}

static inline void time_grq_lock(struct rq *rq, unsigned long *flags)
	__acquires(grq.lock)
{
	local_irq_save(*flags);
	time_lock_grq(rq);
}

static inline struct rq *__task_grq_lock(struct task_struct *p)
	__acquires(grq.lock)
{
	grq_lock();
	return task_rq(p);
}

static inline void __task_grq_unlock(void)
	__releases(grq.lock)
{
	grq_unlock();
}

#ifndef __ARCH_WANT_UNLOCKED_CTXSW
static inline void prepare_lock_switch(struct rq *rq, struct task_struct *next)
{
}

static inline void finish_lock_switch(struct rq *rq, struct task_struct *prev)
{
#ifdef CONFIG_DEBUG_SPINLOCK
	/* this is a valid case when another task releases the spinlock */
	grq.lock.owner = current;
#endif
	/*
	 * If we are tracking spinlock dependencies then we have to
	 * fix up the runqueue lock - which gets 'carried over' from
	 * prev into current:
	 */
	spin_acquire(&grq.lock.dep_map, 0, 0, _THIS_IP_);

	grq_unlock_irq();
}

#else /* __ARCH_WANT_UNLOCKED_CTXSW */

static inline void prepare_lock_switch(struct rq *rq, struct task_struct *next)
{
#ifdef __ARCH_WANT_INTERRUPTS_ON_CTXSW
	grq_unlock_irq();
#else
	grq_unlock();
#endif
}

static inline void finish_lock_switch(struct rq *rq, struct task_struct *prev)
{
	smp_wmb();
#ifndef __ARCH_WANT_INTERRUPTS_ON_CTXSW
	local_irq_enable();
#endif
}
#endif /* __ARCH_WANT_UNLOCKED_CTXSW */

/*
 * A task that is queued but not running will be on the grq run list.
 * A task that is not running or queued will not be on the grq run list.
 * A task that is currently running will have ->oncpu set but not on the
 * grq run list.
 */
static inline int task_queued(struct task_struct *p)
{
	return (!list_empty(&p->run_list));
}

/*
 * Removing from the global runqueue. Enter with grq locked.
 */
static void dequeue_task(struct task_struct *p)
{
	list_del_init(&p->run_list);
	if (list_empty(grq.queue + p->prio))
		__clear_bit(p->prio, grq.prio_bitmap);
}

static inline void reset_first_time_slice(struct task_struct *p)
{
	if (unlikely(p->first_time_slice))
		p->first_time_slice = 0;
}

static int idleprio_suitable(struct task_struct *p)
{
	return (!freezing(p) && !signal_pending(p) &&
		!(task_contributes_to_load(p)) && !(p->flags & (PF_EXITING)));
}

static int isoprio_suitable(void)
{
	return !grq.iso_refractory;
}

/*
 * Adding to the global runqueue. Enter with grq locked.
 */
static void enqueue_task(struct task_struct *p)
{
	if (!rt_task(p)) {
		/* Check it hasn't gotten rt from PI */
		if ((idleprio_task(p) && idleprio_suitable(p)) ||
		   (iso_task(p) && isoprio_suitable()))
			p->prio = p->normal_prio;
		else
			p->prio = NORMAL_PRIO;
	}
	__set_bit(p->prio, grq.prio_bitmap);
	list_add_tail(&p->run_list, grq.queue + p->prio);
	sched_info_queued(p);
}

/* Only idle task does this as a real time task*/
static inline void enqueue_task_head(struct task_struct *p)
{
	__set_bit(p->prio, grq.prio_bitmap);
	list_add(&p->run_list, grq.queue + p->prio);
	sched_info_queued(p);
}

static inline void requeue_task(struct task_struct *p)
{
	sched_info_queued(p);
}

/*
 * task_timeslice - all tasks of all priorities get the exact same timeslice
 * length. CPU distribution is handled by giving different deadlines to
 * tasks of different priorities.
 */
static inline int task_timeslice(struct task_struct *p)
{
	return (TASK_USER_PRIO(p) + 1) * rr_interval;
}

#ifdef CONFIG_SMP
static inline void inc_qnr(void)
{
	grq.qnr++;
}

static inline void dec_qnr(void)
{
	grq.qnr--;
}

static inline int queued_notrunning(void)
{
	return grq.qnr;
}

static inline void set_cpuidle_map(unsigned long cpu)
{
	cpu_set(cpu, grq.cpu_idle_map);
}

static inline void clear_cpuidle_map(unsigned long cpu)
{
	cpu_clear(cpu, grq.cpu_idle_map);
}

static int suitable_idle_cpus(struct task_struct *p)
{
	return (cpus_intersects(p->cpus_allowed, grq.cpu_idle_map));
}

static inline void resched_suitable_idle(struct task_struct *p)
{
	cpumask_t tmp;

	cpus_and(tmp, p->cpus_allowed, grq.cpu_idle_map);

	if (!cpus_empty(tmp))
		wake_up_idle_cpu(first_cpu(tmp));
}

/*
 * The cpu cache locality difference between CPUs is used to determine how far
 * to offset the virtual deadline. "One" difference in locality means that one
 * timeslice difference is allowed longer for the cpu local tasks. This is
 * enough in the common case when tasks are up to 2* number of CPUs to keep
 * tasks within their shared cache CPUs only. CPUs on different nodes or not
 * even in this domain (NUMA) have "3" difference, allowing 4 times longer
 * deadlines before being taken onto another cpu, allowing for 2* the double
 * seen by separate CPUs above. See sched_init_smp for how locality is
 * determined.
 */
static inline int
cache_distance(struct rq *task_rq, struct rq *rq, struct task_struct *p)
{
	return rq->cpu_locality[task_rq->cpu] * task_timeslice(p);
}
#else /* CONFIG_SMP */
static inline void inc_qnr(void)
{
}

static inline void dec_qnr(void)
{
}

static inline int queued_notrunning(void)
{
	return grq.nr_running;
}

static inline void set_cpuidle_map(unsigned long cpu)
{
}

static inline void clear_cpuidle_map(unsigned long cpu)
{
}

/* Always called from a busy cpu on UP */
static inline int suitable_idle_cpus(struct task_struct *p)
{
	return 0;
}

static inline void resched_suitable_idle(struct task_struct *p)
{
}

static inline int
cache_distance(struct rq *task_rq, struct rq *rq, struct task_struct *p)
{
	return 0;
}
#endif /* CONFIG_SMP */

/*
 * activate_idle_task - move idle task to the _front_ of runqueue.
 */
static inline void activate_idle_task(struct task_struct *p)
{
	enqueue_task_head(p);
	grq.nr_running++;
	inc_qnr();
}

static inline int normal_prio(struct task_struct *p)
{
	if (has_rt_policy(p))
		return MAX_RT_PRIO - 1 - p->rt_priority;
	if (idleprio_task(p))
		return IDLE_PRIO;
	if (iso_task(p))
		return ISO_PRIO;
	return NORMAL_PRIO;
}

/*
 * Calculate the current priority, i.e. the priority
 * taken into account by the scheduler. This value might
 * be boosted by RT tasks as it will be RT if the task got
 * RT-boosted. If not then it returns p->normal_prio.
 */
static int effective_prio(struct task_struct *p)
{
	p->normal_prio = normal_prio(p);
	/*
	 * If we are RT tasks or we were boosted to RT priority,
	 * keep the priority unchanged. Otherwise, update priority
	 * to the normal priority:
	 */
	if (!rt_prio(p->prio))
		return p->normal_prio;
	return p->prio;
}

/*
 * activate_task - move a task to the runqueue. Enter with grq locked. The rq
 * doesn't really matter but gives us the local clock.
 */
static void activate_task(struct task_struct *p, struct rq *rq)
{
	u64 now;

	update_rq_clock(rq);
	now = rq->clock;

	/*
	 * Sleep time is in units of nanosecs, so shift by 20 to get a
	 * milliseconds-range estimation of the amount of time that the task
	 * spent sleeping:
	 */
	if (unlikely(prof_on == SLEEP_PROFILING)) {
		if (p->state == TASK_UNINTERRUPTIBLE)
			profile_hits(SLEEP_PROFILING, (void *)get_wchan(p),
				     (now - p->last_ran) >> 20);
	}

	p->prio = effective_prio(p);
	if (task_contributes_to_load(p))
		grq.nr_uninterruptible--;
	enqueue_task(p);
	grq.nr_running++;
	inc_qnr();
}

/*
 * deactivate_task - If it's running, it's not on the grq and we can just
 * decrement the nr_running.
 */
static inline void deactivate_task(struct task_struct *p)
{
	if (task_contributes_to_load(p))
		grq.nr_uninterruptible++;
	grq.nr_running--;
}

#ifdef CONFIG_SMP
void set_task_cpu(struct task_struct *p, unsigned int cpu)
{
	/*
	 * After ->cpu is set up to a new value, task_grq_lock(p, ...) can be
	 * successfuly executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	smp_wmb();
	task_thread_info(p)->cpu = cpu;
}
#endif

/*
 * Move a task off the global queue and take it to a cpu for it will
 * become the running task.
 */
static inline void take_task(struct rq *rq, struct task_struct *p)
{
	set_task_cpu(p, rq->cpu);
	dequeue_task(p);
	dec_qnr();
}

/*
 * Returns a descheduling task to the grq runqueue unless it is being
 * deactivated.
 */
static inline void return_task(struct task_struct *p, int deactivate)
{
	if (deactivate)
		deactivate_task(p);
	else {
		inc_qnr();
		enqueue_task(p);
	}
}

/*
 * resched_task - mark a task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
#ifdef CONFIG_SMP

#ifndef tsk_is_polling
#define tsk_is_polling(t) test_tsk_thread_flag(t, TIF_POLLING_NRFLAG)
#endif

static void resched_task(struct task_struct *p)
{
	int cpu;

	assert_spin_locked(&grq.lock);

	if (unlikely(test_tsk_thread_flag(p, TIF_NEED_RESCHED)))
		return;

	set_tsk_thread_flag(p, TIF_NEED_RESCHED);

	cpu = task_cpu(p);
	if (cpu == smp_processor_id())
		return;

	/* NEED_RESCHED must be visible before we test polling */
	smp_mb();
	if (!tsk_is_polling(p))
		smp_send_reschedule(cpu);
}

#else
static inline void resched_task(struct task_struct *p)
{
	assert_spin_locked(&grq.lock);
	set_tsk_need_resched(p);
}
#endif

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 */
inline int task_curr(const struct task_struct *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

#ifdef CONFIG_SMP
struct migration_req {
	struct list_head list;

	struct task_struct *task;
	int dest_cpu;

	struct completion done;
};

/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * If @match_state is nonzero, it's the @p->state value just checked and
 * not expected to change.  If it changes, i.e. @p might have woken up,
 * then return zero.  When we succeed in waiting for @p to be off its CPU,
 * we return a positive number (its total switch count).  If a second call
 * a short while later returns the same number, the caller can be sure that
 * @p has remained unscheduled the whole time.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time. This function can't
 * be called with interrupts off, or it may introduce deadlock with
 * smp_call_function() if an IPI is sent by the same process we are
 * waiting to become inactive.
 */
unsigned long wait_task_inactive(struct task_struct *p, long match_state)
{
	unsigned long flags;
	int running, on_rq;
	unsigned long ncsw;
	struct rq *rq;

	for (;;) {
		/*
		 * We do the initial early heuristics without holding
		 * any task-queue locks at all. We'll only try to get
		 * the runqueue lock when things look like they will
		 * work out! In the unlikely event rq is dereferenced
		 * since we're lockless, grab it again.
		 */
retry_rq:
		rq = task_rq(p);
		if (unlikely(!rq))
			goto retry_rq;

		/*
		 * If the task is actively running on another CPU
		 * still, just relax and busy-wait without holding
		 * any locks.
		 *
		 * NOTE! Since we don't hold any locks, it's not
		 * even sure that "rq" stays as the right runqueue!
		 * But we don't care, since this will return false
		 * if the runqueue has changed and p is actually now
		 * running somewhere else!
		 */
		while (task_running(p) && p == rq->curr) {
			if (match_state && unlikely(p->state != match_state))
				return 0;
			cpu_relax();
		}

		/*
		 * Ok, time to look more closely! We need the grq
		 * lock now, to be *sure*. If we're wrong, we'll
		 * just go back and repeat.
		 */
		rq = task_grq_lock(p, &flags);
		running = task_running(p);
		on_rq = task_queued(p);
		ncsw = 0;
		if (!match_state || p->state == match_state) {
			ncsw = p->nivcsw + p->nvcsw;
			if (unlikely(!ncsw))
				ncsw = 1;
		}
		task_grq_unlock(&flags);

		/*
		 * If it changed from the expected state, bail out now.
		 */
		if (unlikely(!ncsw))
			break;

		/*
		 * Was it really running after all now that we
		 * checked with the proper locks actually held?
		 *
		 * Oops. Go back and try again..
		 */
		if (unlikely(running)) {
			cpu_relax();
			continue;
		}

		/*
		 * It's not enough that it's not actively running,
		 * it must be off the runqueue _entirely_, and not
		 * preempted!
		 *
		 * So if it wa still runnable (but just not actively
		 * running right now), it's preempted, and we should
		 * yield - it could be a while.
		 */
		if (unlikely(on_rq)) {
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/*
		 * Ahh, all good. It wasn't running, and it wasn't
		 * runnable, which means that it will never become
		 * running in the future either. We're all done!
		 */
		break;
	}

	return ncsw;
}

/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesnt have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
void kick_process(struct task_struct *p)
{
	int cpu;

	preempt_disable();
	cpu = task_cpu(p);
	if ((cpu != smp_processor_id()) && task_curr(p))
		smp_send_reschedule(cpu);
	preempt_enable();
}
#endif

#define rq_idle(rq)	((rq)->rq_prio == PRIO_LIMIT)
#define task_idle(p)	((p)->prio == PRIO_LIMIT)

/*
 * RT tasks preempt purely on priority. SCHED_NORMAL tasks preempt on the
 * basis of earlier deadlines. SCHED_BATCH, ISO and IDLEPRIO don't preempt
 * between themselves, they cooperatively multitask. An idle rq scores as
 * prio PRIO_LIMIT so it is always preempted. The offset_deadline will choose
 * an idle runqueue that is closer cache-wise in preference. latest_deadline
 * and highest_prio_rq are initialised only to silence the compiler. When
 * all else is equal, still prefer this_rq.
 */
static void try_preempt(struct task_struct *p, struct rq *this_rq)
{
	unsigned long latest_deadline = 0, cpu;
	struct rq *highest_prio_rq = this_rq;
	int highest_prio = -1;
	cpumask_t tmp;

	cpus_and(tmp, cpu_online_map, p->cpus_allowed);

	for_each_cpu_mask_nr(cpu, tmp) {
		unsigned long offset_deadline;
		struct rq *rq;
		int rq_prio;

		rq = cpu_rq(cpu);
		rq_prio = rq->rq_prio;
		if (rq_prio < highest_prio)
			continue;

		offset_deadline = -cache_distance(this_rq, rq, p);
		if (rq_prio != PRIO_LIMIT)
			offset_deadline += rq->rq_deadline;

		if (rq_prio > highest_prio || (rq_prio == highest_prio &&
		    (time_after(offset_deadline, latest_deadline) ||
		    (this_rq == rq && offset_deadline == latest_deadline)))) {
			latest_deadline = offset_deadline;
			highest_prio = rq_prio;
			highest_prio_rq = rq;
		}
	}

	if (p->prio > highest_prio || (p->policy == SCHED_NORMAL &&
	    p->prio == highest_prio && !time_before(p->deadline, latest_deadline)))
	    	return;

	/* p gets to preempt highest_prio_rq->curr */
	resched_task(highest_prio_rq->curr);
	return;
}

/***
 * try_to_wake_up - wake up a thread
 * @p: the to-be-woken-up thread
 * @state: the mask of task states that can be woken
 * @sync: do a synchronous wakeup?
 *
 * Put it on the run-queue if it's not already there. The "current"
 * thread is always on the run-queue (except when the actual
 * re-schedule is in progress), and as such you're allowed to do
 * the simpler "current->state = TASK_RUNNING" to mark yourself
 * runnable without the overhead of this.
 *
 * returns failure only if the task is already active.
 */
static int try_to_wake_up(struct task_struct *p, unsigned int state, int sync)
{
	unsigned long flags;
	int success = 0;
	struct rq *rq;

	/* This barrier is undocumented, probably for p->state? くそ */
	smp_wmb();

	/*
	 * No need to do time_lock_grq as we only need to update the rq clock
	 * if we activate the task
	 */
	rq = task_grq_lock(p, &flags);

	/* state is a volatile long, どうして、分からない */
	if (!((unsigned int)p->state & state))
		goto out_unlock;

	if (task_queued(p) || task_running(p))
		goto out_running;

	activate_task(p, rq);
	/*
	 * Sync wakeups (i.e. those types of wakeups where the waker
	 * has indicated that it will leave the CPU in short order)
	 * don't trigger a preemption if there are no idle cpus,
	 * instead waiting for current to deschedule.
	 */
	if (!sync || suitable_idle_cpus(p))
		try_preempt(p, rq);
	success = 1;

out_running:
	trace_mark(kernel_sched_wakeup,
		"pid %d state %ld ## rq %p task %p rq->curr %p",
		p->pid, p->state, rq, p, rq->curr);
	p->state = TASK_RUNNING;
out_unlock:
	task_grq_unlock(&flags);
	return success;
}

/**
 * wake_up_process - Wake up a specific process
 * @p: The process to be woken up.
 *
 * Attempt to wake up the nominated process and move it to the set of runnable
 * processes.  Returns 1 if the process was woken up, 0 if it was already
 * running.
 *
 * It may be assumed that this function implies a write memory barrier before
 * changing the task state if and only if any tasks are woken up.
 */
int wake_up_process(struct task_struct *p)
{
	return try_to_wake_up(p, TASK_ALL, 0);
}
EXPORT_SYMBOL(wake_up_process);

int wake_up_state(struct task_struct *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

/*
 * Perform scheduler related setup for a newly forked process p.
 * p is forked by current.
 */
void sched_fork(struct task_struct *p, int clone_flags)
{
	int cpu = get_cpu();
	struct rq *rq;

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&p->preempt_notifiers);
#endif
	/*
	 * We mark the process as running here, but have not actually
	 * inserted it onto the runqueue yet. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->state = TASK_RUNNING;
	set_task_cpu(p, cpu);

	/* Should be reset in fork.c but done here for ease of bfs patching */
	p->sched_time = p->stime_pc = p->utime_pc = 0;

	/*
	 * Make sure we do not leak PI boosting priority to the child:
	 */
	p->prio = current->normal_prio;

	INIT_LIST_HEAD(&p->run_list);
#if defined(CONFIG_SCHEDSTATS) || defined(CONFIG_TASK_DELAY_ACCT)
	if (unlikely(sched_info_on()))
		memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif

	p->oncpu = 0;

#ifdef CONFIG_PREEMPT
	/* Want to start with kernel preemption disabled. */
	task_thread_info(p)->preempt_count = 1;
#endif
	if (unlikely(p->policy == SCHED_FIFO))
		goto out;
	/*
	 * Share the timeslice between parent and child, thus the
	 * total amount of pending timeslices in the system doesn't change,
	 * resulting in more scheduling fairness. If it's negative, it won't
	 * matter since that's the same as being 0. current's time_slice is
	 * actually in rq_time_slice when it's running.
	 */
	rq = task_grq_lock_irq(current);
	if (likely(rq->rq_time_slice > 0)) {
		rq->rq_time_slice /= 2;
		/*
		 * The remainder of the first timeslice might be recovered by
		 * the parent if the child exits early enough.
		 */
		p->first_time_slice = 1;
	}
	p->time_slice = rq->rq_time_slice;
	task_grq_unlock_irq();
out:
	put_cpu();
}

/*
 * wake_up_new_task - wake up a newly created task for the first time.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created context, then puts the task
 * on the runqueue and wakes it.
 */
void wake_up_new_task(struct task_struct *p, unsigned long clone_flags)
{
	struct task_struct *parent;
	unsigned long flags;
	struct rq *rq;

	rq = task_grq_lock(p, &flags); ;
	parent = p->parent;
	BUG_ON(p->state != TASK_RUNNING);
	set_task_cpu(p, task_cpu(parent));
	activate_task(p, rq);
	trace_mark(kernel_sched_wakeup_new,
		"pid %d state %ld ## rq %p task %p rq->curr %p",
		p->pid, p->state, rq, p, rq->curr);
	if (!(clone_flags & CLONE_VM) && rq->curr == parent &&
		!suitable_idle_cpus(p)) {
		/*
		 * The VM isn't cloned, so we're in a good position to
		 * do child-runs-first in anticipation of an exec. This
		 * usually avoids a lot of COW overhead.
		 */
			resched_task(parent);
	} else
		try_preempt(p, rq);
	task_grq_unlock(&flags);
}

/*
 * Potentially available exiting-child timeslices are
 * retrieved here - this way the parent does not get
 * penalised for creating too many threads.
 *
 * (this cannot be used to 'generate' timeslices
 * artificially, because any timeslice recovered here
 * was given away by the parent in the first place.)
 */
void sched_exit(struct task_struct *p)
{
	struct task_struct *parent;
	unsigned long flags;
	struct rq *rq;

	if (unlikely(p->first_time_slice)) {
		int *par_tslice, *p_tslice;

		parent = p->parent;
		rq = task_grq_lock(parent, &flags);
		par_tslice = &parent->time_slice;
		p_tslice = &p->time_slice;

		/* The real time_slice of the "curr" task is on the rq var.*/
		if (p == rq->curr)
			p_tslice = &rq->rq_time_slice;
		else if (parent == task_rq(parent)->curr)
			par_tslice = &rq->rq_time_slice;

		*par_tslice += *p_tslice;
		if (unlikely(*par_tslice > timeslice()))
			*par_tslice = timeslice();
		task_grq_unlock(&flags);
	}
}

#ifdef CONFIG_PREEMPT_NOTIFIERS

/**
 * preempt_notifier_register - tell me when current is being being preempted & rescheduled
 * @notifier: notifier struct to register
 */
void preempt_notifier_register(struct preempt_notifier *notifier)
{
	hlist_add_head(&notifier->link, &current->preempt_notifiers);
}
EXPORT_SYMBOL_GPL(preempt_notifier_register);

/**
 * preempt_notifier_unregister - no longer interested in preemption notifications
 * @notifier: notifier struct to unregister
 *
 * This is safe to call from within a preemption notifier.
 */
void preempt_notifier_unregister(struct preempt_notifier *notifier)
{
	hlist_del(&notifier->link);
}
EXPORT_SYMBOL_GPL(preempt_notifier_unregister);

static void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	struct preempt_notifier *notifier;
	struct hlist_node *node;

	hlist_for_each_entry(notifier, node, &curr->preempt_notifiers, link)
		notifier->ops->sched_in(notifier, raw_smp_processor_id());
}

static void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
	struct preempt_notifier *notifier;
	struct hlist_node *node;

	hlist_for_each_entry(notifier, node, &curr->preempt_notifiers, link)
		notifier->ops->sched_out(notifier, next);
}

#else /* !CONFIG_PREEMPT_NOTIFIERS */

static void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
}

static void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
}

#endif /* CONFIG_PREEMPT_NOTIFIERS */

/**
 * prepare_task_switch - prepare to switch tasks
 * @rq: the runqueue preparing to switch
 * @next: the task we are going to switch to.
 *
 * This is called with the rq lock held and interrupts off. It must
 * be paired with a subsequent finish_task_switch after the context
 * switch.
 *
 * prepare_task_switch sets up locking and calls architecture specific
 * hooks.
 */
static inline void
prepare_task_switch(struct rq *rq, struct task_struct *prev,
		    struct task_struct *next)
{
	fire_sched_out_preempt_notifiers(prev, next);
	prepare_lock_switch(rq, next);
	prepare_arch_switch(next);
}

/**
 * finish_task_switch - clean up after a task-switch
 * @rq: runqueue associated with task-switch
 * @prev: the thread we just switched away from.
 *
 * finish_task_switch must be called after the context switch, paired
 * with a prepare_task_switch call before the context switch.
 * finish_task_switch will reconcile locking set up by prepare_task_switch,
 * and do any other architecture-specific cleanup actions.
 *
 * Note that we may have delayed dropping an mm in context_switch(). If
 * so, we finish that here outside of the runqueue lock.  (Doing it
 * with the lock held can cause deadlocks; see schedule() for
 * details.)
 */
static inline void finish_task_switch(struct rq *rq, struct task_struct *prev)
	__releases(grq.lock)
{
	struct mm_struct *mm = rq->prev_mm;
	long prev_state;

	rq->prev_mm = NULL;

	/*
	 * A task struct has one reference for the use as "current".
	 * If a task dies, then it sets TASK_DEAD in tsk->state and calls
	 * schedule one last time. The schedule call will never return, and
	 * the scheduled task must drop that reference.
	 * The test for TASK_DEAD must occur while the runqueue locks are
	 * still held, otherwise prev could be scheduled on another cpu, die
	 * there before we look at prev->state, and then the reference would
	 * be dropped twice.
	 *		Manfred Spraul <manfred@colorfullife.com>
	 */
	prev_state = prev->state;
	finish_arch_switch(prev);
	finish_lock_switch(rq, prev);

	fire_sched_in_preempt_notifiers(current);
	if (mm)
		mmdrop(mm);
	if (unlikely(prev_state == TASK_DEAD)) {
		/*
		 * Remove function-return probe instances associated with this
		 * task and put them back on the free list.
	 	 */
		kprobe_flush_task(prev);
		put_task_struct(prev);
	}
}

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
asmlinkage void schedule_tail(struct task_struct *prev)
	__releases(grq.lock)
{
	struct rq *rq = this_rq();

	finish_task_switch(rq, prev);
#ifdef __ARCH_WANT_UNLOCKED_CTXSW
	/* In this case, finish_task_switch does not reenable preemption */
	preempt_enable();
#endif
	if (current->set_child_tid)
		put_user(current->pid, current->set_child_tid);
}

/*
 * context_switch - switch to the new MM and the new
 * thread's register state.
 */
static inline void
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next)
{
	struct mm_struct *mm, *oldmm;

	prepare_task_switch(rq, prev, next);
	trace_mark(kernel_sched_schedule,
		"prev_pid %d next_pid %d prev_state %ld "
		"## rq %p prev %p next %p",
		prev->pid, next->pid, prev->state,
		rq, prev, next);
	mm = next->mm;
	oldmm = prev->active_mm;
	/*
	 * For paravirt, this is coupled with an exit in switch_to to
	 * combine the page table reload and the switch backend into
	 * one hypercall.
	 */
	arch_enter_lazy_cpu_mode();

	if (unlikely(!mm)) {
		next->active_mm = oldmm;
		atomic_inc(&oldmm->mm_count);
		enter_lazy_tlb(oldmm, next);
	} else
		switch_mm(oldmm, mm, next);

	if (unlikely(!prev->mm)) {
		prev->active_mm = NULL;
		rq->prev_mm = oldmm;
	}
	/*
	 * Since the runqueue lock will be released by the next
	 * task (which is an invalid locking op but in the case
	 * of the scheduler it's an obvious special-case), so we
	 * do an early lockdep release here:
	 */
#ifndef __ARCH_WANT_UNLOCKED_CTXSW
	spin_release(&grq.lock.dep_map, 1, _THIS_IP_);
#endif

	/* Here we just switch the register state and the stack. */
	switch_to(prev, next, prev);

	barrier();
	/*
	 * this_rq must be evaluated again because prev may have moved
	 * CPUs since it called schedule(), thus the 'rq' on its stack
	 * frame will be invalid.
	 */
	finish_task_switch(this_rq(), prev);
}

/*
 * nr_running, nr_uninterruptible and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, current number of uninterruptible-sleeping threads, total
 * number of context switches performed since bootup. All are measured
 * without grabbing the grq lock but the occasional inaccurate result
 * doesn't matter so long as it's positive.
 */
unsigned long nr_running(void)
{
	long nr = grq.nr_running;

	if (unlikely(nr < 0))
		nr = 0;
	return (unsigned long)nr;
}

unsigned long nr_uninterruptible(void)
{
	unsigned long nu = grq.nr_uninterruptible;

	if (unlikely(nu < 0))
		nu = 0;
	return nu;
}

unsigned long long nr_context_switches(void)
{
	long long ns = grq.nr_switches;

	/* This is of course impossible */
	if (unlikely(ns < 0))
		ns = 1;
	return (long long)ns;
}

unsigned long nr_iowait(void)
{
	unsigned long i, sum = 0;

	for_each_possible_cpu(i)
		sum += atomic_read(&cpu_rq(i)->nr_iowait);

	return sum;
}

unsigned long nr_active(void)
{
	return nr_running() + nr_uninterruptible();
}

DEFINE_PER_CPU(struct kernel_stat, kstat);

EXPORT_PER_CPU_SYMBOL(kstat);

/*
 * On each tick, see what percentage of that tick was attributed to each
 * component and add the percentage to the _pc values. Once a _pc value has
 * accumulated one tick's worth, account for that. This means the total
 * percentage of load components will always be 100 per tick.
 */
static void pc_idle_time(struct rq *rq, unsigned long pc)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime64_t tmp = cputime_to_cputime64(jiffies_to_cputime(1));

	if (atomic_read(&rq->nr_iowait) > 0) {
		rq->iowait_pc += pc;
		if (rq->iowait_pc >= 100) {
			rq->iowait_pc %= 100;
			cpustat->iowait = cputime64_add(cpustat->iowait, tmp);
		}
	} else {
		rq->idle_pc += pc;
		if (rq->idle_pc >= 100) {
			rq->idle_pc %= 100;
			cpustat->idle = cputime64_add(cpustat->idle, tmp);
		}
	}
}

static void
pc_system_time(struct rq *rq, struct task_struct *p, int hardirq_offset,
	       unsigned long pc, unsigned long ns)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime_t one_jiffy = jiffies_to_cputime(1);
	cputime_t one_jiffy_scaled = cputime_to_scaled(one_jiffy);
	cputime64_t tmp = cputime_to_cputime64(one_jiffy);

	p->stime_pc += pc;
	if (p->stime_pc >= 100) {
		p->stime_pc -= 100;
		p->stime = cputime_add(p->stime, one_jiffy);
		p->stimescaled = cputime_add(p->stimescaled, one_jiffy_scaled);
		acct_update_integrals(p);
	}
	p->sched_time += ns;

	if (hardirq_count() - hardirq_offset)
		rq->irq_pc += pc;
	else if (softirq_count()) {
		rq->softirq_pc += pc;
		if (rq->softirq_pc >= 100) {
			rq->softirq_pc %= 100;
			cpustat->softirq = cputime64_add(cpustat->softirq, tmp);
		}
	} else {
		rq->system_pc += pc;
		if (rq->system_pc >= 100) {
			rq->system_pc %= 100;
			cpustat->system = cputime64_add(cpustat->system, tmp);
		}
	}
}

static void pc_user_time(struct rq *rq, struct task_struct *p,
			 unsigned long pc, unsigned long ns)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime_t one_jiffy = jiffies_to_cputime(1);
	cputime_t one_jiffy_scaled = cputime_to_scaled(one_jiffy);
	cputime64_t tmp = cputime_to_cputime64(one_jiffy);

	p->utime_pc += pc;
	if (p->utime_pc >= 100) {
		p->utime_pc -= 100;
		p->utime = cputime_add(p->utime, one_jiffy);
		p->utimescaled = cputime_add(p->utimescaled, one_jiffy_scaled);
		acct_update_integrals(p);
	}
	p->sched_time += ns;

	if (TASK_NICE(p) > 0 || idleprio_task(p)) {
		rq->nice_pc += pc;
		if (rq->nice_pc >= 100) {
			rq->nice_pc %= 100;
			cpustat->nice = cputime64_add(cpustat->nice, tmp);
		}
	} else {
		rq->user_pc += pc;
		if (rq->user_pc >= 100) {
			rq->user_pc %= 100;
			cpustat->user = cputime64_add(cpustat->user, tmp);
		}
	}
}

/* Convert nanoseconds to percentage of one tick. */
#define NS_TO_PC(NS)	(NS * 100 / JIFFIES_TO_NS(1))

/*
 * This is called on clock ticks and on context switches.
 * Bank in p->sched_time the ns elapsed since the last tick or switch.
 * CPU scheduler quota accounting is also performed here in microseconds.
 * The value returned from sched_clock() occasionally gives bogus values so
 * some sanity checking is required. Time is supposed to be banked all the
 * time so default to half a tick to make up for when sched_clock reverts
 * to just returning jiffies, and for hardware that can't do tsc.
 */
static void
update_cpu_clock(struct rq *rq, struct task_struct *p, int tick)
{
	long account_ns = rq->clock - rq->timekeep_clock;
	struct task_struct *idle = rq->idle;
	unsigned long account_pc;

	if (unlikely(account_ns < 0))
		account_ns = 0;

	account_pc = NS_TO_PC(account_ns);

	if (tick) {
		int user_tick = user_mode(get_irq_regs());

		/* Accurate tick timekeeping */
		if (user_tick)
			pc_user_time(rq, p, account_pc, account_ns);
		else if (p != idle || (irq_count() != HARDIRQ_OFFSET))
			pc_system_time(rq, p, HARDIRQ_OFFSET,
				       account_pc, account_ns);
		else
			pc_idle_time(rq, account_pc);
	} else {
		/* Accurate subtick timekeeping */
		if (p == idle)
			pc_idle_time(rq, account_pc);
		else
			pc_user_time(rq, p, account_pc, account_ns);
	}

	/* time_slice accounting is done in usecs to avoid overflow on 32bit */
	if (rq->rq_policy != SCHED_FIFO && p != idle) {
		long time_diff = rq->clock - rq->rq_last_ran;

		/*
		 * There should be less than or equal to one jiffy worth, and not
		 * negative/overflow. time_diff is only used for internal scheduler
		 * time_slice accounting.
		 */
		if (time_diff <= 0)
			time_diff = JIFFIES_TO_NS(1) / 2;
		else if (time_diff > JIFFIES_TO_NS(1))
			time_diff = JIFFIES_TO_NS(1);

		rq->rq_time_slice -= time_diff / 1000;
	}
	rq->rq_last_ran = rq->timekeep_clock = rq->clock;
}

/*
 * Return accounted runtime for the task.
 * In case the task is currently running, return the runtime plus current's
 * pending runtime that have not been accounted yet.
 */
unsigned long long task_sched_runtime(struct task_struct *p)
{
	unsigned long flags;
	u64 ns, delta_exec;
	struct rq *rq;

	rq = task_grq_lock(p, &flags);
	ns = p->sched_time;
	if (p == rq->curr) {
		update_rq_clock(rq);
		delta_exec = rq->clock - rq->rq_last_ran;
		if ((s64)delta_exec > 0)
			ns += delta_exec;
	}
	task_grq_unlock(&flags);

	return ns;
}

/* Compatibility crap for removal */
void account_user_time(struct task_struct *p, cputime_t cputime,
		       cputime_t cputime_scaled)
{
}

void account_idle_time(cputime_t cputime)
{
}

/*
 * Account guest cpu time to a process.
 * @p: the process that the cpu time gets accounted to
 * @cputime: the cpu time spent in virtual machine since the last update
 * @cputime_scaled: cputime scaled by cpu frequency
 */
static void account_guest_time(struct task_struct *p, cputime_t cputime,
			       cputime_t cputime_scaled)
{
	cputime64_t tmp;
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;

	tmp = cputime_to_cputime64(cputime);

	/* Add guest time to process. */
	p->utime = cputime_add(p->utime, cputime);
	p->utimescaled = cputime_add(p->utimescaled, cputime_scaled);
	p->gtime = cputime_add(p->gtime, cputime);

	/* Add guest time to cpustat. */
	cpustat->user = cputime64_add(cpustat->user, tmp);
	cpustat->guest = cputime64_add(cpustat->guest, tmp);
}

/*
 * Account system cpu time to a process.
 * @p: the process that the cpu time gets accounted to
 * @hardirq_offset: the offset to subtract from hardirq_count()
 * @cputime: the cpu time spent in kernel space since the last update
 * @cputime_scaled: cputime scaled by cpu frequency
 * This is for guest only now.
 */
void account_system_time(struct task_struct *p, int hardirq_offset,
			 cputime_t cputime, cputime_t cputime_scaled)
{

	if ((p->flags & PF_VCPU) && (irq_count() - hardirq_offset == 0))
		account_guest_time(p, cputime, cputime_scaled);
}

/*
 * Account for involuntary wait time.
 * @steal: the cpu time spent in involuntary wait
 */
void account_steal_time(cputime_t cputime)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime64_t cputime64 = cputime_to_cputime64(cputime);

	cpustat->steal = cputime64_add(cpustat->steal, cputime64);
}

/*
 * Account for idle time.
 * @cputime: the cpu time spent in idle wait
 */
static void account_idle_times(cputime_t cputime)
{
	struct cpu_usage_stat *cpustat = &kstat_this_cpu.cpustat;
	cputime64_t cputime64 = cputime_to_cputime64(cputime);
	struct rq *rq = this_rq();

	if (atomic_read(&rq->nr_iowait) > 0)
		cpustat->iowait = cputime64_add(cpustat->iowait, cputime64);
	else
		cpustat->idle = cputime64_add(cpustat->idle, cputime64);
}

#ifndef CONFIG_VIRT_CPU_ACCOUNTING

void account_process_tick(struct task_struct *p, int user_tick)
{
}

/*
 * Account multiple ticks of steal time.
 * @p: the process from which the cpu time has been stolen
 * @ticks: number of stolen ticks
 */
void account_steal_ticks(unsigned long ticks)
{
	account_steal_time(jiffies_to_cputime(ticks));
}

/*
 * Account multiple ticks of idle time.
 * @ticks: number of stolen ticks
 */
void account_idle_ticks(unsigned long ticks)
{
	account_idle_times(jiffies_to_cputime(ticks));
}
#endif

/*
 * Functions to test for when SCHED_ISO tasks have used their allocated
 * quota as real time scheduling and convert them back to SCHED_NORMAL.
 * Where possible, the data is tested lockless, to avoid grabbing grq_lock
 * because the occasional inaccurate result won't matter. However the
 * tick data is only ever modified under lock. iso_refractory is only simply
 * set to 0 or 1 so it's not worth grabbing the lock yet again for that.
 */
static void set_iso_refractory(void)
{
	grq.iso_refractory = 1;
}

static void clear_iso_refractory(void)
{
	grq.iso_refractory = 0;
}

/*
 * Test if SCHED_ISO tasks have run longer than their alloted period as RT
 * tasks and set the refractory flag if necessary. There is 10% hysteresis
 * for unsetting the flag.
 */
static unsigned int test_ret_isorefractory(struct rq *rq)
{
	if (likely(!grq.iso_refractory)) {
		if (grq.iso_ticks / ISO_PERIOD > sched_iso_cpu)
			set_iso_refractory();
	} else {
		if (grq.iso_ticks / ISO_PERIOD < (sched_iso_cpu * 90 / 100))
			clear_iso_refractory();
	}
	return grq.iso_refractory;
}

static void iso_tick(void)
{
	grq_lock();
	grq.iso_ticks += 100;
	grq_unlock();
}

/* No SCHED_ISO task was running so decrease rq->iso_ticks */
static inline void no_iso_tick(void)
{
	if (grq.iso_ticks) {
		grq_lock();
		grq.iso_ticks = grq.iso_ticks * (ISO_PERIOD - 1) / ISO_PERIOD;
		if (unlikely(grq.iso_refractory && grq.iso_ticks /
		    ISO_PERIOD < (sched_iso_cpu * 90 / 100)))
			clear_iso_refractory();
		grq_unlock();
	}
}

static int rq_running_iso(struct rq *rq)
{
	return rq->rq_prio == ISO_PRIO;
}

/* This manages tasks that have run out of timeslice during a scheduler_tick */
static void task_running_tick(struct rq *rq)
{
	struct task_struct *p;

	/*
	 * If a SCHED_ISO task is running we increment the iso_ticks. In
	 * order to prevent SCHED_ISO tasks from causing starvation in the
	 * presence of true RT tasks we account those as iso_ticks as well.
	 */
	if ((rt_queue(rq) || (iso_queue(rq) && !grq.iso_refractory))) {
		if (grq.iso_ticks <= (ISO_PERIOD * 100) - 100)
			iso_tick();
	} else
		no_iso_tick();

	if (iso_queue(rq)) {
		if (unlikely(test_ret_isorefractory(rq))) {
			if (rq_running_iso(rq)) {
				/*
				 * SCHED_ISO task is running as RT and limit
				 * has been hit. Force it to reschedule as
				 * SCHED_NORMAL by zeroing its time_slice
				 */
				rq->rq_time_slice = 0;
			}
		}
	}

	/* SCHED_FIFO tasks never run out of timeslice. */
	if (rq_idle(rq) || rq->rq_time_slice > 0 || rq->rq_policy == SCHED_FIFO)
		return;

	/* p->time_slice <= 0. We only modify task_struct under grq lock */
	p = rq->curr;
	requeue_task(p);
	grq_lock();
	set_tsk_need_resched(p);
	grq_unlock();
}

void wake_up_idle_cpu(int cpu);

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled. The data modified is all
 * local to struct rq so we don't need to grab grq lock.
 */
void scheduler_tick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	sched_clock_tick();
	update_rq_clock(rq);
	update_cpu_clock(rq, rq->curr, 1);
	if (!rq_idle(rq))
		task_running_tick(rq);
	else
		no_iso_tick();
}

#if defined(CONFIG_PREEMPT) && (defined(CONFIG_DEBUG_PREEMPT) || \
				defined(CONFIG_PREEMPT_TRACER))

static inline unsigned long get_parent_ip(unsigned long addr)
{
	if (in_lock_functions(addr)) {
		addr = CALLER_ADDR2;
		if (in_lock_functions(addr))
			addr = CALLER_ADDR3;
	}
	return addr;
}

void __kprobes add_preempt_count(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON((preempt_count() < 0)))
		return;
#endif
	preempt_count() += val;
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Spinlock count overflowing soon?
	 */
	DEBUG_LOCKS_WARN_ON((preempt_count() & PREEMPT_MASK) >=
				PREEMPT_MASK - 10);
#endif
	if (preempt_count() == val)
		trace_preempt_off(CALLER_ADDR0, get_parent_ip(CALLER_ADDR1));
}
EXPORT_SYMBOL(add_preempt_count);

void __kprobes sub_preempt_count(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON(val > preempt_count()))
		return;
	/*
	 * Is the spinlock portion underflowing?
	 */
	if (DEBUG_LOCKS_WARN_ON((val < PREEMPT_MASK) &&
			!(preempt_count() & PREEMPT_MASK)))
		return;
#endif

	if (preempt_count() == val)
		trace_preempt_on(CALLER_ADDR0, get_parent_ip(CALLER_ADDR1));
	preempt_count() -= val;
}
EXPORT_SYMBOL(sub_preempt_count);
#endif

/*
 * Deadline is "now" in jiffies + (offset by priority). Setting the deadline
 * is the key to everything. It distributes cpu fairly amongst tasks of the
 * same nice value, it proportions cpu according to nice level, it means the
 * task that last woke up the longest ago has the earliest deadline, thus
 * ensuring that interactive tasks get low latency on wake up. The CPU
 * proportion works out to the square of the difference, so this equation will
 * give nice 19 3% CPU compared to nice 0 and nice 0 3% compared to nice -20.
 */
static inline int prio_deadline_diff(int user_prio)
{
	return (user_prio + 1) * rr_interval * HZ / 500;
}

static inline int task_deadline_diff(struct task_struct *p)
{
	return prio_deadline_diff(TASK_USER_PRIO(p));
}

static inline int static_deadline_diff(int static_prio)
{
	return prio_deadline_diff(USER_PRIO(static_prio));
}

static inline int longest_deadline_diff(void)
{
	return prio_deadline_diff(39);
}

/*
 * SCHED_IDLEPRIO tasks still have a deadline set, but offset by nice +19.
 * This allows nice levels to work between IDLEPRIO tasks and gives a
 * deadline longer than nice +19 for when they're scheduled as SCHED_NORMAL
 * tasks.
 */
static inline void time_slice_expired(struct task_struct *p)
{
	reset_first_time_slice(p);
	p->time_slice = timeslice();
	p->deadline = jiffies + task_deadline_diff(p);
	if (idleprio_task(p))
		p->deadline += longest_deadline_diff();
}

static inline void check_deadline(struct task_struct *p)
{
	if (p->time_slice <= 0)
		time_slice_expired(p);
}

/*
 * O(n) lookup of all tasks in the global runqueue. The real brainfuck
 * of lock contention and O(n). It's not really O(n) as only the queued,
 * but not running tasks are scanned, and is O(n) queued in the worst case
 * scenario only because the right task can be found before scanning all of
 * them.
 * Tasks are selected in this order:
 * Real time tasks are selected purely by their static priority and in the
 * order they were queued, so the lowest value idx, and the first queued task
 * of that priority value is chosen.
 * If no real time tasks are found, the SCHED_ISO priority is checked, and
 * all SCHED_ISO tasks have the same priority value, so they're selected by
 * the earliest deadline value.
 * If no SCHED_ISO tasks are found, SCHED_NORMAL tasks are selected by the
 * earliest deadline.
 * Finally if no SCHED_NORMAL tasks are found, SCHED_IDLEPRIO tasks are
 * selected by the earliest deadline.
 */
static inline struct
task_struct *earliest_deadline_task(struct rq *rq, struct task_struct *idle)
{
	unsigned long dl, earliest_deadline = 0; /* Initialise to silence compiler */
	struct task_struct *p, *edt;
	unsigned int cpu = rq->cpu;
	struct list_head *queue;
	int idx = 0;

	edt = idle;
retry:
	idx = find_next_bit(grq.prio_bitmap, PRIO_LIMIT, idx);
	if (idx >= PRIO_LIMIT)
		goto out;
	queue = grq.queue + idx;
	list_for_each_entry(p, queue, run_list) {
		/* Make sure cpu affinity is ok */
		if (!cpu_isset(cpu, p->cpus_allowed))
			continue;
		if (idx < MAX_RT_PRIO) {
			/* We found an rt task */
			edt = p;
			goto out_take;
		}

		dl = p->deadline + cache_distance(task_rq(p), rq, p);

		/*
		 * No rt tasks. Find the earliest deadline task. Now we're in
		 * O(n) territory. This is what we silenced the compiler for:
		 * edt will always start as idle.
		 */
		if (edt == idle ||
		    time_before(dl, earliest_deadline)) {
			earliest_deadline = dl;
			edt = p;
		}
	}
	if (edt == idle) {
		if (++idx < PRIO_LIMIT)
			goto retry;
		goto out;
	}
out_take:
	take_task(rq, edt);
out:
	return edt;
}

/*
 * Print scheduling while atomic bug:
 */
static noinline void __schedule_bug(struct task_struct *prev)
{
	struct pt_regs *regs = get_irq_regs();

	printk(KERN_ERR "BUG: scheduling while atomic: %s/%d/0x%08x\n",
		prev->comm, prev->pid, preempt_count());

	debug_show_held_locks(prev);
	print_modules();
	if (irqs_disabled())
		print_irqtrace_events(prev);

	if (regs)
		show_regs(regs);
	else
		dump_stack();
}

/*
 * Various schedule()-time debugging checks and statistics:
 */
static inline void schedule_debug(struct task_struct *prev)
{
	/*
	 * Test if we are atomic. Since do_exit() needs to call into
	 * schedule() atomically, we ignore that path for now.
	 * Otherwise, whine if we are scheduling when we should not be.
	 */
	if (unlikely(in_atomic_preempt_off() && !prev->exit_state))
		__schedule_bug(prev);

	profile_hit(SCHED_PROFILING, __builtin_return_address(0));

	schedstat_inc(this_rq(), sched_count);
#ifdef CONFIG_SCHEDSTATS
	if (unlikely(prev->lock_depth >= 0)) {
		schedstat_inc(this_rq(), bkl_count);
		schedstat_inc(prev, sched_info.bkl_count);
	}
#endif
}

/*
 * The currently running task's information is all stored in rq local data
 * which is only modified by the local CPU, thereby allowing the data to be
 * changed without grabbing the grq lock.
 */
static inline void set_rq_task(struct rq *rq, struct task_struct *p)
{
	rq->rq_time_slice = p->time_slice;
	rq->rq_deadline = p->deadline;
	rq->rq_last_ran = p->last_ran;
	rq->rq_policy = p->policy;
	rq->rq_prio = p->prio;
}

static void reset_rq_task(struct rq *rq, struct task_struct *p)
{
	rq->rq_policy = p->policy;
	rq->rq_prio = p->prio;
}

/*
 * schedule() is the main scheduler function.
 */
asmlinkage void __sched schedule(void)
{
	struct task_struct *prev, *next, *idle;
	unsigned long *switch_count;
	int deactivate, cpu;
	struct rq *rq;
	u64 now;

need_resched:
	preempt_disable();

	cpu = smp_processor_id();
	rq = cpu_rq(cpu);
	idle = rq->idle;
	rcu_qsctr_inc(cpu);
	prev = rq->curr;
	switch_count = &prev->nivcsw;

	release_kernel_lock(prev);
need_resched_nonpreemptible:

	deactivate = 0;
	schedule_debug(prev);

	local_irq_disable();
	update_rq_clock(rq);
	now = rq->clock;
	update_cpu_clock(rq, prev, 0);

	grq_lock();
	clear_tsk_need_resched(prev);

	if (prev->state && !(preempt_count() & PREEMPT_ACTIVE)) {
		if (unlikely(signal_pending_state(prev->state, prev)))
			prev->state = TASK_RUNNING;
		else
			deactivate = 1;
		switch_count = &prev->nvcsw;
	}

	if (prev != idle) {
		/* Update all the information stored on struct rq */
		prev->time_slice = rq->rq_time_slice;
		prev->deadline = rq->rq_deadline;
		check_deadline(prev);
		return_task(prev, deactivate);
		/* Task changed affinity off this cpu */
		if (unlikely(!cpus_intersects(prev->cpus_allowed,
		    cpumask_of_cpu(cpu))))
		    	resched_suitable_idle(prev);
	}

	if (likely(queued_notrunning())) {
		next = earliest_deadline_task(rq, idle);
	} else {
		next = idle;
		schedstat_inc(rq, sched_goidle);
	}

	prefetch(next);
	prefetch_stack(next);

	if (task_idle(next))
		set_cpuidle_map(cpu);
	else
		clear_cpuidle_map(cpu);

	prev->last_ran = now;

	if (likely(prev != next)) {
		sched_info_switch(prev, next);

		set_rq_task(rq, next);
		grq.nr_switches++;
		prev->oncpu = 0;
		next->oncpu = 1;
		rq->curr = next;
		++*switch_count;

		context_switch(rq, prev, next); /* unlocks the grq */
		/*
		 * the context switch might have flipped the stack from under
		 * us, hence refresh the local variables.
		 */
		cpu = smp_processor_id();
		rq = cpu_rq(cpu);
		idle = rq->idle;
	} else
		grq_unlock_irq();

	if (unlikely(reacquire_kernel_lock(current) < 0))
		goto need_resched_nonpreemptible;
	preempt_enable_no_resched();
	if (unlikely(test_thread_flag(TIF_NEED_RESCHED)))
 		goto need_resched;
}
EXPORT_SYMBOL(schedule);

#ifdef CONFIG_PREEMPT
/*
 * this is the entry point to schedule() from in-kernel preemption
 * off of preempt_enable. Kernel preemptions off return from interrupt
 * occur there and call schedule directly.
 */
asmlinkage void __sched preempt_schedule(void)
{
	struct thread_info *ti = current_thread_info();

	/*
	 * If there is a non-zero preempt_count or interrupts are disabled,
	 * we do not want to preempt the current task. Just return..
	 */
	if (likely(ti->preempt_count || irqs_disabled()))
		return;

	do {
		add_preempt_count(PREEMPT_ACTIVE);
		schedule();
		sub_preempt_count(PREEMPT_ACTIVE);

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
		barrier();
	} while (unlikely(test_thread_flag(TIF_NEED_RESCHED)));
}
EXPORT_SYMBOL(preempt_schedule);

/*
 * this is the entry point to schedule() from kernel preemption
 * off of irq context.
 * Note, that this is called and return with irqs disabled. This will
 * protect us against recursive calling from irq.
 */
asmlinkage void __sched preempt_schedule_irq(void)
{
	struct thread_info *ti = current_thread_info();

	/* Catch callers which need to be fixed */
	BUG_ON(ti->preempt_count || !irqs_disabled());

	do {
		add_preempt_count(PREEMPT_ACTIVE);
		local_irq_enable();
		schedule();
		local_irq_disable();
		sub_preempt_count(PREEMPT_ACTIVE);

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
		barrier();
	} while (unlikely(test_thread_flag(TIF_NEED_RESCHED)));
}

#endif /* CONFIG_PREEMPT */

int default_wake_function(wait_queue_t *curr, unsigned mode, int sync,
			  void *key)
{
	return try_to_wake_up(curr->private, mode, sync);
}
EXPORT_SYMBOL(default_wake_function);

/*
 * The core wakeup function.  Non-exclusive wakeups (nr_exclusive == 0) just
 * wake everything up.  If it's an exclusive wakeup (nr_exclusive == small +ve
 * number) then we wake all the non-exclusive tasks and one exclusive task.
 *
 * There are circumstances in which we can try to wake a task which has already
 * started to run but is not in state TASK_RUNNING.  try_to_wake_up() returns
 * zero in this (rare) case, and we handle it by continuing to scan the queue.
 */
void __wake_up_common(wait_queue_head_t *q, unsigned int mode,
			     int nr_exclusive, int sync, void *key)
{
	struct list_head *tmp, *next;

	list_for_each_safe(tmp, next, &q->task_list) {
		wait_queue_t *curr = list_entry(tmp, wait_queue_t, task_list);
		unsigned flags = curr->flags;

		if (curr->func(curr, mode, sync, key) &&
				(flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}
}

/**
 * __wake_up - wake up threads blocked on a waitqueue.
 * @q: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 * @key: is directly passed to the wakeup function
 *
 * It may be assumed that this function implies a write memory barrier before
 * changing the task state if and only if any tasks are woken up.
 */
void __wake_up(wait_queue_head_t *q, unsigned int mode,
			int nr_exclusive, void *key)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_common(q, mode, nr_exclusive, 0, key);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(__wake_up);

/*
 * Same as __wake_up but called with the spinlock in wait_queue_head_t held.
 */
//drakaz : void __wake_up_locked(wait_queue_head_t *q, unsigned int mode)
void __wake_up_locked(wait_queue_head_t *q, unsigned int mode,int nr, void *key)
{
	__wake_up_common(q, mode, 1, 0, NULL);
}

/**
 * __wake_up_sync - wake up threads blocked on a waitqueue.
 * @q: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 *
 * The sync wakeup differs that the waker knows that it will schedule
 * away soon, so while the target thread will be woken up, it will not
 * be migrated to another CPU - ie. the two threads are 'synchronised'
 * with each other. This can prevent needless bouncing between CPUs.
 *
 * On UP it can prevent extra preemption.
 */
void __wake_up_sync(wait_queue_head_t *q, unsigned int mode, int nr_exclusive)
{
	unsigned long flags;
	int sync = 1;

	if (unlikely(!q))
		return;

	if (unlikely(!nr_exclusive))
		sync = 0;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_common(q, mode, nr_exclusive, sync, NULL);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL_GPL(__wake_up_sync);	/* For internal use only */

void complete(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	x->done++;
	__wake_up_common(&x->wait, TASK_NORMAL, 1, 0, NULL);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete);

void complete_all(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	x->done += UINT_MAX/2;
	__wake_up_common(&x->wait, TASK_NORMAL, 0, 0, NULL);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete_all);

static inline long __sched
do_wait_for_common(struct completion *x, long timeout, int state)
{
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);

		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_tail(&x->wait, &wait);
		do {
			if ((state == TASK_INTERRUPTIBLE &&
			     signal_pending(current)) ||
			    (state == TASK_KILLABLE &&
			     fatal_signal_pending(current))) {
				timeout = -ERESTARTSYS;
				break;
			}
			__set_current_state(state);
			spin_unlock_irq(&x->wait.lock);
			timeout = schedule_timeout(timeout);
			spin_lock_irq(&x->wait.lock);
		} while (!x->done && timeout);
		__remove_wait_queue(&x->wait, &wait);
		if (!x->done)
			return timeout;
	}
	x->done--;
	return timeout ?: 1;
}

static long __sched
wait_for_common(struct completion *x, long timeout, int state)
{
	might_sleep();

	spin_lock_irq(&x->wait.lock);
	timeout = do_wait_for_common(x, timeout, state);
	spin_unlock_irq(&x->wait.lock);
	return timeout;
}

void __sched wait_for_completion(struct completion *x)
{
	wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion);

unsigned long __sched
wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion_timeout);

int __sched wait_for_completion_interruptible(struct completion *x)
{
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
	if (t == -ERESTARTSYS)
		return t;
	return 0;
}
EXPORT_SYMBOL(wait_for_completion_interruptible);

unsigned long __sched
wait_for_completion_interruptible_timeout(struct completion *x,
					  unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_INTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion_interruptible_timeout);

int __sched wait_for_completion_killable(struct completion *x)
{
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
	if (t == -ERESTARTSYS)
		return t;
	return 0;
}
EXPORT_SYMBOL(wait_for_completion_killable);

/**
 *	try_wait_for_completion - try to decrement a completion without blocking
 *	@x:	completion structure
 *
 *	Returns: 0 if a decrement cannot be done without blocking
 *		 1 if a decrement succeeded.
 *
 *	If a completion is being used as a counting completion,
 *	attempt to decrement the counter without blocking. This
 *	enables us to avoid waiting if the resource the completion
 *	is protecting is not available.
 */
bool try_wait_for_completion(struct completion *x)
{
	int ret = 1;

	spin_lock_irq(&x->wait.lock);
	if (!x->done)
		ret = 0;
	else
		x->done--;
	spin_unlock_irq(&x->wait.lock);
	return ret;
}
EXPORT_SYMBOL(try_wait_for_completion);

/**
 *	completion_done - Test to see if a completion has any waiters
 *	@x:	completion structure
 *
 *	Returns: 0 if there are waiters (wait_for_completion() in progress)
 *		 1 if there are no waiters.
 *
 */
bool completion_done(struct completion *x)
{
	int ret = 1;

	spin_lock_irq(&x->wait.lock);
	if (!x->done)
		ret = 0;
	spin_unlock_irq(&x->wait.lock);
	return ret;
}
EXPORT_SYMBOL(completion_done);

static long __sched
sleep_on_common(wait_queue_head_t *q, int state, long timeout)
{
	unsigned long flags;
	wait_queue_t wait;

	init_waitqueue_entry(&wait, current);

	__set_current_state(state);

	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue(q, &wait);
	spin_unlock(&q->lock);
	timeout = schedule_timeout(timeout);
	spin_lock_irq(&q->lock);
	__remove_wait_queue(q, &wait);
	spin_unlock_irqrestore(&q->lock, flags);

	return timeout;
}

void __sched interruptible_sleep_on(wait_queue_head_t *q)
{
	sleep_on_common(q, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}
EXPORT_SYMBOL(interruptible_sleep_on);

long __sched
interruptible_sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	return sleep_on_common(q, TASK_INTERRUPTIBLE, timeout);
}
EXPORT_SYMBOL(interruptible_sleep_on_timeout);

void __sched sleep_on(wait_queue_head_t *q)
{
	sleep_on_common(q, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}
EXPORT_SYMBOL(sleep_on);

long __sched sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	return sleep_on_common(q, TASK_UNINTERRUPTIBLE, timeout);
}
EXPORT_SYMBOL(sleep_on_timeout);

#ifdef CONFIG_RT_MUTEXES

/*
 * rt_mutex_setprio - set the current priority of a task
 * @p: task
 * @prio: prio value (kernel-internal form)
 *
 * This function changes the 'effective' priority of a task. It does
 * not touch ->normal_prio like __setscheduler().
 *
 * Used by the rt_mutex code to implement priority inheritance logic.
 */
void rt_mutex_setprio(struct task_struct *p, int prio)
{
	unsigned long flags;
	int queued, oldprio;
	struct rq *rq;

	BUG_ON(prio < 0 || prio > MAX_PRIO);

	rq = time_task_grq_lock(p, &flags);

	oldprio = p->prio;
	queued = task_queued(p);
	if (queued)
		dequeue_task(p);
	p->prio = prio;
	if (task_running(p) && prio > oldprio)
		resched_task(p);
	if (queued) {
		enqueue_task(p);
		try_preempt(p, rq);
	}

	task_grq_unlock(&flags);
}

#endif

/*
 * Adjust the deadline for when the priority is to change, before it's
 * changed.
 */
static inline void adjust_deadline(struct task_struct *p, int new_prio)
{
	p->deadline += static_deadline_diff(new_prio) - task_deadline_diff(p);
}

void set_user_nice(struct task_struct *p, long nice)
{
	int queued, new_static, old_static;
	unsigned long flags;
	struct rq *rq;

	if (TASK_NICE(p) == nice || nice < -20 || nice > 19)
		return;
	new_static = NICE_TO_PRIO(nice);
	/*
	 * We have to be careful, if called from sys_setpriority(),
	 * the task might be in the middle of scheduling on another CPU.
	 */
	rq = time_task_grq_lock(p, &flags);
	/*
	 * The RT priorities are set via sched_setscheduler(), but we still
	 * allow the 'normal' nice value to be set - but as expected
	 * it wont have any effect on scheduling until the task is
	 * not SCHED_NORMAL/SCHED_BATCH:
	 */
	if (has_rt_policy(p)) {
		p->static_prio = new_static;
		goto out_unlock;
	}
	queued = task_queued(p);
	if (queued)
		dequeue_task(p);

	adjust_deadline(p, new_static);
	old_static = p->static_prio;
	p->static_prio = new_static;
	p->prio = effective_prio(p);

	if (queued) {
		enqueue_task(p);
		if (new_static < old_static)
			try_preempt(p, rq);
	} else if (task_running(p)) {
		reset_rq_task(rq, p);
		if (old_static < new_static)
			resched_task(p);
	}
out_unlock:
	task_grq_unlock(&flags);
}
EXPORT_SYMBOL(set_user_nice);

/*
 * can_nice - check if a task can reduce its nice value
 * @p: task
 * @nice: nice value
 */
int can_nice(const struct task_struct *p, const int nice)
{
	/* convert nice value [19,-20] to rlimit style value [1,40] */
	int nice_rlim = 20 - nice;

	return (nice_rlim <= p->signal->rlim[RLIMIT_NICE].rlim_cur ||
		capable(CAP_SYS_NICE));
}

#ifdef __ARCH_WANT_SYS_NICE

/*
 * sys_nice - change the priority of the current process.
 * @increment: priority increment
 *
 * sys_setpriority is a more generic, but much slower function that
 * does similar things.
 */
SYSCALL_DEFINE1(nice, int, increment)
{
	long nice, retval;

	/*
	 * Setpriority might change our priority at the same moment.
	 * We don't have to worry. Conceptually one call occurs first
	 * and we have a single winner.
	 */
	if (increment < -40)
		increment = -40;
	if (increment > 40)
		increment = 40;

	nice = PRIO_TO_NICE(current->static_prio) + increment;
	if (nice < -20)
		nice = -20;
	if (nice > 19)
		nice = 19;

	if (increment < 0 && !can_nice(current, nice))
		return -EPERM;

	retval = security_task_setnice(current, nice);
	if (retval)
		return retval;

	set_user_nice(current, nice);
	return 0;
}

#endif

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * This is the priority value as seen by users in /proc.
 * RT tasks are offset by -100. Normal tasks are centered
 * around 1, value goes from 0 (SCHED_ISO) up to 82 (nice +19
 * SCHED_IDLEPRIO).
 */
int task_prio(const struct task_struct *p)
{
	int delta, prio = p->prio - MAX_RT_PRIO;

	/* rt tasks and iso tasks */
	if (prio <= 0)
		goto out;

	delta = (p->deadline - jiffies) * 40 / longest_deadline_diff();
	if (delta > 0 && delta <= 80)
		prio += delta;
out:
	return prio;
}

/**
 * task_nice - return the nice value of a given task.
 * @p: the task in question.
 */
int task_nice(const struct task_struct *p)
{
	return TASK_NICE(p);
}
EXPORT_SYMBOL_GPL(task_nice);

/**
 * idle_cpu - is a given cpu idle currently?
 * @cpu: the processor in question.
 */
int idle_cpu(int cpu)
{
	return cpu_curr(cpu) == cpu_rq(cpu)->idle;
}

/**
 * idle_task - return the idle task for a given cpu.
 * @cpu: the processor in question.
 */
struct task_struct *idle_task(int cpu)
{
	return cpu_rq(cpu)->idle;
}

/**
 * find_process_by_pid - find a process with a matching PID value.
 * @pid: the pid in question.
 */
static inline struct task_struct *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_vpid(pid) : current;
}

/* Actually do priority change: must hold grq lock. */
static void
__setscheduler(struct task_struct *p, struct rq *rq, int policy, int prio)
{
	BUG_ON(task_queued(p));

	p->policy = policy;
	p->rt_priority = prio;
	p->normal_prio = normal_prio(p);
	/* we are holding p->pi_lock already */
	p->prio = rt_mutex_getprio(p);
	/*
	 * Reschedule if running. schedule() will know if it can continue
	 * running or not.
	 */
	if (task_running(p)) {
		resched_task(p);
		reset_rq_task(rq, p);
	}
}

static int __sched_setscheduler(struct task_struct *p, int policy,
		       struct sched_param *param, bool user)
{
	struct sched_param zero_param = { .sched_priority = 0 };
	int queued, retval, oldpolicy = -1;
	unsigned long flags, rlim_rtprio = 0;
	struct rq *rq;

	/* may grab non-irq protected spin_locks */
	BUG_ON(in_interrupt());

	if (is_rt_policy(policy) && !capable(CAP_SYS_NICE)) {
		unsigned long lflags;

		if (!lock_task_sighand(p, &lflags))
			return -ESRCH;
		rlim_rtprio = p->signal->rlim[RLIMIT_RTPRIO].rlim_cur;
		unlock_task_sighand(p, &lflags);
		if (rlim_rtprio)
			goto recheck;
		/*
		 * If the caller requested an RT policy without having the
		 * necessary rights, we downgrade the policy to SCHED_ISO.
		 * We also set the parameter to zero to pass the checks.
		 */
		policy = SCHED_ISO;
		param = &zero_param;
	}
recheck:
	/* double check policy once rq lock held */
	if (policy < 0)
		policy = oldpolicy = p->policy;
	else if (!SCHED_RANGE(policy))
		return -EINVAL;
	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are
	 * 1..MAX_USER_RT_PRIO-1, valid priority for SCHED_NORMAL and
	 * SCHED_BATCH is 0.
	 */
	if (param->sched_priority < 0 ||
	    (p->mm && param->sched_priority > MAX_USER_RT_PRIO-1) ||
	    (!p->mm && param->sched_priority > MAX_RT_PRIO-1))
		return -EINVAL;
	if (is_rt_policy(policy) != (param->sched_priority != 0))
		return -EINVAL;

	/*
	 * Allow unprivileged RT tasks to decrease priority:
	 */
	if (user && !capable(CAP_SYS_NICE)) {
		if (is_rt_policy(policy)) {
			/* can't set/change the rt policy */
			if (policy != p->policy && !rlim_rtprio)
				return -EPERM;

			/* can't increase priority */
			if (param->sched_priority > p->rt_priority &&
			    param->sched_priority > rlim_rtprio)
				return -EPERM;
		} else {
			switch (p->policy) {
				/*
				 * Can only downgrade policies but not back to
				 * SCHED_NORMAL
				 */
				case SCHED_ISO:
					if (policy == SCHED_ISO)
						goto out;
					if (policy == SCHED_NORMAL)
						return -EPERM;
					break;
				case SCHED_BATCH:
					if (policy == SCHED_BATCH)
						goto out;
					if (policy != SCHED_IDLEPRIO)
					    	return -EPERM;
					break;
				case SCHED_IDLEPRIO:
					if (policy == SCHED_IDLEPRIO)
						goto out;
					return -EPERM;
				default:
					break;
			}
		}

		/* can't change other user's priorities */
		if ((current->euid != p->euid) &&
		    (current->euid != p->uid))
			return -EPERM;
	}

	retval = security_task_setscheduler(p, policy, param);
	if (retval)
		return retval;
	/*
	 * make sure no PI-waiters arrive (or leave) while we are
	 * changing the priority of the task:
	 */
	spin_lock_irqsave(&p->pi_lock, flags);
	/*
	 * To be able to change p->policy safely, the apropriate
	 * runqueue lock must be held.
	 */
	rq = __task_grq_lock(p);
	/* recheck policy now with rq lock held */
	if (unlikely(oldpolicy != -1 && oldpolicy != p->policy)) {
		__task_grq_unlock();
		spin_unlock_irqrestore(&p->pi_lock, flags);
		policy = oldpolicy = -1;
		goto recheck;
	}
	update_rq_clock(rq);
	queued = task_queued(p);
	if (queued)
		dequeue_task(p);
	__setscheduler(p, rq, policy, param->sched_priority);
	if (queued) {
		enqueue_task(p);
		try_preempt(p, rq);
	}
	__task_grq_unlock();
	spin_unlock_irqrestore(&p->pi_lock, flags);

	rt_mutex_adjust_pi(p);
out:
	return 0;
}

/**
 * sched_setscheduler - change the scheduling policy and/or RT priority of a thread.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * NOTE that the task may be already dead.
 */
int sched_setscheduler(struct task_struct *p, int policy,
		       struct sched_param *param)
{
	return __sched_setscheduler(p, policy, param, true);
}

EXPORT_SYMBOL_GPL(sched_setscheduler);

/**
 * sched_setscheduler_nocheck - change the scheduling policy and/or RT priority of a thread from kernelspace.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Just like sched_setscheduler, only don't bother checking if the
 * current context has permission.  For example, this is needed in
 * stop_machine(): we create temporary high priority worker threads,
 * but our caller might not have that capability.
 */
int sched_setscheduler_nocheck(struct task_struct *p, int policy,
			       struct sched_param *param)
{
	return __sched_setscheduler(p, policy, param, false);
}

static int
do_sched_setscheduler(pid_t pid, int policy, struct sched_param __user *param)
{
	struct sched_param lparam;
	struct task_struct *p;
	int retval;

	if (!param || pid < 0)
		return -EINVAL;
	if (copy_from_user(&lparam, param, sizeof(struct sched_param)))
		return -EFAULT;

	rcu_read_lock();
	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (p != NULL)
		retval = sched_setscheduler(p, policy, &lparam);
	rcu_read_unlock();

	return retval;
}

/**
 * sys_sched_setscheduler - set/change the scheduler policy and RT priority
 * @pid: the pid in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 */
asmlinkage long sys_sched_setscheduler(pid_t pid, int policy,
				       struct sched_param __user *param)
{
	/* negative values for policy are not valid */
	if (policy < 0)
		return -EINVAL;

	return do_sched_setscheduler(pid, policy, param);
}

/**
 * sys_sched_setparam - set/change the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the new RT priority.
 */
SYSCALL_DEFINE2(sched_setparam, pid_t, pid, struct sched_param __user *, param)
{
	return do_sched_setscheduler(pid, -1, param);
}

/**
 * sys_sched_getscheduler - get the policy (scheduling class) of a thread
 * @pid: the pid in question.
 */
SYSCALL_DEFINE1(sched_getscheduler, pid_t, pid)
{
	struct task_struct *p;
	int retval = -EINVAL;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (p) {
		retval = security_task_getscheduler(p);
		if (!retval)
			retval = p->policy;
	}
	read_unlock(&tasklist_lock);

out_nounlock:
	return retval;
}

/**
 * sys_sched_getscheduler - get the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the RT priority.
 */
SYSCALL_DEFINE2(sched_getparam, pid_t, pid, struct sched_param __user *, param)
{
	struct sched_param lp;
	struct task_struct *p;
	int retval = -EINVAL;

	if (!param || pid < 0)
		goto out_nounlock;

	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	lp.sched_priority = p->rt_priority;
	read_unlock(&tasklist_lock);

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	retval = copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;

out_nounlock:
	return retval;

out_unlock:
	read_unlock(&tasklist_lock);
	return retval;
}

long sched_setaffinity(pid_t pid, const cpumask_t *in_mask)
{
	cpumask_t cpus_allowed;
	cpumask_t new_mask = *in_mask;
	struct task_struct *p;
	int retval;

	get_online_cpus();
	read_lock(&tasklist_lock);

	p = find_process_by_pid(pid);
	if (!p) {
		read_unlock(&tasklist_lock);
		put_online_cpus();
		return -ESRCH;
	}

	/*
	 * It is not safe to call set_cpus_allowed with the
	 * tasklist_lock held. We will bump the task_struct's
	 * usage count and then drop tasklist_lock.
	 */
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	retval = -EPERM;
	if ((current->euid != p->euid) && (current->euid != p->uid) &&
			!capable(CAP_SYS_NICE))
		goto out_unlock;

	retval = security_task_setscheduler(p, 0, NULL);
	if (retval)
		goto out_unlock;

	cpuset_cpus_allowed(p, &cpus_allowed);
	cpus_and(new_mask, new_mask, cpus_allowed);
 again:
	retval = set_cpus_allowed_ptr(p, &new_mask);

	if (!retval) {
		cpuset_cpus_allowed(p, &cpus_allowed);
		if (!cpus_subset(new_mask, cpus_allowed)) {
			/*
			 * We must have raced with a concurrent cpuset
			 * update. Just reset the cpus_allowed to the
			 * cpuset's cpus_allowed
			 */
			new_mask = cpus_allowed;
			goto again;
		}
	}
out_unlock:
	put_task_struct(p);
	put_online_cpus();
	return retval;
}

static int get_user_cpu_mask(unsigned long __user *user_mask_ptr, unsigned len,
			     cpumask_t *new_mask)
{
	if (len < sizeof(cpumask_t)) {
		memset(new_mask, 0, sizeof(cpumask_t));
	} else if (len > sizeof(cpumask_t)) {
		len = sizeof(cpumask_t);
	}
	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}

/**
 * sys_sched_setaffinity - set the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to the new cpu mask
 */
SYSCALL_DEFINE3(sched_setaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	cpumask_t new_mask;
	int retval;

	retval = get_user_cpu_mask(user_mask_ptr, len, &new_mask);
	if (retval)
		return retval;

	return sched_setaffinity(pid, &new_mask);
}

long sched_getaffinity(pid_t pid, cpumask_t *mask)
{
	struct task_struct *p;
	int retval;

	mutex_lock(&sched_hotcpu_mutex);
	read_lock(&tasklist_lock);

	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	cpus_and(*mask, p->cpus_allowed, cpu_online_map);

out_unlock:
	read_unlock(&tasklist_lock);
	mutex_unlock(&sched_hotcpu_mutex);
	if (retval)
		return retval;

	return 0;
}

/**
 * sys_sched_getaffinity - get the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to hold the current cpu mask
 */
SYSCALL_DEFINE3(sched_getaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	int ret;
	cpumask_t mask;

	if (len < sizeof(cpumask_t))
		return -EINVAL;

	ret = sched_getaffinity(pid, &mask);
	if (ret < 0)
		return ret;

	if (copy_to_user(user_mask_ptr, &mask, sizeof(cpumask_t)))
		return -EFAULT;

	return sizeof(cpumask_t);
}

/**
 * sys_sched_yield - yield the current processor to other threads.
 *
 * This function yields the current CPU to other tasks. It does this by
 * refilling the timeslice, resetting the deadline and scheduling away.
 */
SYSCALL_DEFINE0(sched_yield)
{
	struct task_struct *p;

	p = current;
	time_task_grq_lock_irq(p);
	schedstat_inc(this_rq(), yld_count);
	time_slice_expired(p);
	requeue_task(p);

	/*
	 * Since we are going to call schedule() anyway, there's
	 * no need to preempt or enable interrupts:
	 */
	__release(grq.lock);
	spin_release(&grq.lock.dep_map, 1, _THIS_IP_);
	_raw_spin_unlock(&grq.lock);
	preempt_enable_no_resched();

	schedule();

	return 0;
}

static void __cond_resched(void)
{
	/* NOT a real fix but will make voluntary preempt work. 馬鹿な事 */
	if (unlikely(system_state != SYSTEM_RUNNING))
		return;
#ifdef CONFIG_DEBUG_SPINLOCK_SLEEP
	__might_sleep(__FILE__, __LINE__);
#endif
	/*
	 * The BKS might be reacquired before we have dropped
	 * PREEMPT_ACTIVE, which could trigger a second
	 * cond_resched() call.
	 */
	do {
		add_preempt_count(PREEMPT_ACTIVE);
		schedule();
		sub_preempt_count(PREEMPT_ACTIVE);
	} while (need_resched());
}

int __sched _cond_resched(void)
{
	if (need_resched() && !(preempt_count() & PREEMPT_ACTIVE) &&
					system_state == SYSTEM_RUNNING) {
		__cond_resched();
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(_cond_resched);

/*
 * cond_resched_lock() - if a reschedule is pending, drop the given lock,
 * call schedule, and on return reacquire the lock.
 *
 * This works OK both with and without CONFIG_PREEMPT.  We do strange low-level
 * operations here to prevent schedule() from being called twice (once via
 * spin_unlock(), once by hand).
 */
int cond_resched_lock(spinlock_t *lock)
{
	int resched = need_resched() && system_state == SYSTEM_RUNNING;
	int ret = 0;

	if (spin_needbreak(lock) || resched) {
		spin_unlock(lock);
		if (resched && need_resched())
			__cond_resched();
		else
			cpu_relax();
		ret = 1;
		spin_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(cond_resched_lock);

int __sched cond_resched_softirq(void)
{
	BUG_ON(!in_softirq());

	if (need_resched() && system_state == SYSTEM_RUNNING) {
		local_bh_enable();
		__cond_resched();
		local_bh_disable();
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(cond_resched_softirq);

/**
 * yield - yield the current processor to other threads.
 *
 * This is a shortcut for kernel-space yielding - it marks the
 * thread runnable and calls sys_sched_yield().
 */
void __sched yield(void)
{
	set_current_state(TASK_RUNNING);
	sys_sched_yield();
}
EXPORT_SYMBOL(yield);

/*
 * This task is about to go to sleep on IO.  Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 *
 * But don't do that if it is a deliberate, throttling IO wait (this task
 * has set its backing_dev_info: the queue against which it should throttle)
 */
void __sched io_schedule(void)
{
	struct rq *rq = &__raw_get_cpu_var(runqueues);

	delayacct_blkio_start();
	atomic_inc(&rq->nr_iowait);
	schedule();
	atomic_dec(&rq->nr_iowait);
	delayacct_blkio_end();
}
EXPORT_SYMBOL(io_schedule);

long __sched io_schedule_timeout(long timeout)
{
	struct rq *rq = &__raw_get_cpu_var(runqueues);
	long ret;

	delayacct_blkio_start();
	atomic_inc(&rq->nr_iowait);
	ret = schedule_timeout(timeout);
	atomic_dec(&rq->nr_iowait);
	delayacct_blkio_end();
	return ret;
}

/**
 * sys_sched_get_priority_max - return maximum RT priority.
 * @policy: scheduling class.
 *
 * this syscall returns the maximum rt_priority that can be used
 * by a given scheduling class.
 */
SYSCALL_DEFINE1(sched_get_priority_max, int, policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = MAX_USER_RT_PRIO-1;
		break;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_ISO:
	case SCHED_IDLEPRIO:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_get_priority_min - return minimum RT priority.
 * @policy: scheduling class.
 *
 * this syscall returns the minimum rt_priority that can be used
 * by a given scheduling class.
 */
SYSCALL_DEFINE1(sched_get_priority_min, int, policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_ISO:
	case SCHED_IDLEPRIO:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_rr_get_interval - return the default timeslice of a process.
 * @pid: pid of the process.
 * @interval: userspace pointer to the timeslice value.
 *
 * this syscall writes the default timeslice value of a given process
 * into the user-space timespec buffer. A value of '0' means infinity.
 */
SYSCALL_DEFINE2(sched_rr_get_interval, pid_t, pid,
		struct timespec __user *, interval)
{
	struct task_struct *p;
	int retval = -EINVAL;
	struct timespec t;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	t = ns_to_timespec(p->policy == SCHED_FIFO ? 0 :
			   MS_TO_NS(task_timeslice(p)));
	read_unlock(&tasklist_lock);
	retval = copy_to_user(interval, &t, sizeof(t)) ? -EFAULT : 0;
out_nounlock:
	return retval;
out_unlock:
	read_unlock(&tasklist_lock);
	return retval;
}

static const char stat_nam[] = TASK_STATE_TO_CHAR_STR;

void sched_show_task(struct task_struct *p)
{
	unsigned long free = 0;
	unsigned state;

	state = p->state ? __ffs(p->state) + 1 : 0;
	printk(KERN_INFO "%-13.13s %c", p->comm,
		state < sizeof(stat_nam) - 1 ? stat_nam[state] : '?');
#if BITS_PER_LONG == 32
	if (state == TASK_RUNNING)
		printk(KERN_CONT " running  ");
	else
		printk(KERN_CONT " %08lx ", thread_saved_pc(p));
#else
	if (state == TASK_RUNNING)
		printk(KERN_CONT "  running task    ");
	else
		printk(KERN_CONT " %016lx ", thread_saved_pc(p));
#endif
#ifdef CONFIG_DEBUG_STACK_USAGE
	{
		unsigned long *n = end_of_stack(p);
		while (!*n)
			n++;
		free = (unsigned long)n - (unsigned long)end_of_stack(p);
	}
#endif
	printk(KERN_CONT "%5lu %5d %6d\n", free,
		task_pid_nr(p), task_pid_nr(p->real_parent));

	show_stack(p, NULL);
}

void show_state_filter(unsigned long state_filter)
{
	struct task_struct *g, *p;

#if BITS_PER_LONG == 32
	printk(KERN_INFO
		"  task                PC stack   pid father\n");
#else
	printk(KERN_INFO
		"  task                        PC stack   pid father\n");
#endif
	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take alot of time:
		 */
		touch_nmi_watchdog();
		if (!state_filter || (p->state & state_filter))
			sched_show_task(p);
	} while_each_thread(g, p);

	touch_all_softlockup_watchdogs();

	read_unlock(&tasklist_lock);
	/*
	 * Only show locks if all tasks are dumped:
	 */
	if (state_filter == -1)
		debug_show_all_locks();
}

/**
 * init_idle - set up an idle thread for a given CPU
 * @idle: task in question
 * @cpu: cpu the idle task belongs to
 *
 * NOTE: this function does not set the idle thread's NEED_RESCHED
 * flag, to make booting more robust.
 */
void init_idle(struct task_struct *idle, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	time_grq_lock(rq, &flags);
	idle->last_ran = rq->clock;
	idle->state = TASK_RUNNING;
	/* Setting prio to illegal value shouldn't matter when never queued */
	idle->prio = PRIO_LIMIT;
	set_rq_task(rq, idle);
	idle->cpus_allowed = cpumask_of_cpu(cpu);
	set_task_cpu(idle, cpu);
	rq->curr = rq->idle = idle;
	idle->oncpu = 1;
	set_cpuidle_map(cpu);
#ifdef CONFIG_HOTPLUG_CPU
	idle->unplugged_mask = CPU_MASK_NONE;
#endif
	grq_unlock_irqrestore(&flags);

	/* Set the preempt count _outside_ the spinlocks! */
#if defined(CONFIG_PREEMPT) && !defined(CONFIG_PREEMPT_BKL)
	task_thread_info(idle)->preempt_count = (idle->lock_depth >= 0);
#else
	task_thread_info(idle)->preempt_count = 0;
#endif
}

/*
 * In a system that switches off the HZ timer nohz_cpu_mask
 * indicates which cpus entered this state. This is used
 * in the rcu update to wait only for active cpus. For system
 * which do not switch off the HZ timer nohz_cpu_mask should
 * always be CPU_MASK_NONE.
 */
cpumask_t nohz_cpu_mask = CPU_MASK_NONE;

#ifdef CONFIG_SMP
#ifdef CONFIG_NO_HZ
static struct {
	atomic_t load_balancer;
	cpumask_t cpu_mask;
} nohz ____cacheline_aligned = {
	.load_balancer = ATOMIC_INIT(-1),
	.cpu_mask = CPU_MASK_NONE,
};

/*
 * This routine will try to nominate the ilb (idle load balancing)
 * owner among the cpus whose ticks are stopped. ilb owner will do the idle
 * load balancing on behalf of all those cpus. If all the cpus in the system
 * go into this tickless mode, then there will be no ilb owner (as there is
 * no need for one) and all the cpus will sleep till the next wakeup event
 * arrives...
 *
 * For the ilb owner, tick is not stopped. And this tick will be used
 * for idle load balancing. ilb owner will still be part of
 * nohz.cpu_mask..
 *
 * While stopping the tick, this cpu will become the ilb owner if there
 * is no other owner. And will be the owner till that cpu becomes busy
 * or if all cpus in the system stop their ticks at which point
 * there is no need for ilb owner.
 *
 * When the ilb owner becomes busy, it nominates another owner, during the
 * next busy scheduler_tick()
 */
int select_nohz_load_balancer(int stop_tick)
{
	int cpu = smp_processor_id();

	if (stop_tick) {
		cpu_set(cpu, nohz.cpu_mask);
		cpu_rq(cpu)->in_nohz_recently = 1;

		/*
		 * If we are going offline and still the leader, give up!
		 */
		if (!cpu_active(cpu) &&
		    atomic_read(&nohz.load_balancer) == cpu) {
			if (atomic_cmpxchg(&nohz.load_balancer, cpu, -1) != cpu)
				BUG();
			return 0;
		}

		/* time for ilb owner also to sleep */
		if (cpus_weight(nohz.cpu_mask) == num_online_cpus()) {
			if (atomic_read(&nohz.load_balancer) == cpu)
				atomic_set(&nohz.load_balancer, -1);
			return 0;
		}

		if (atomic_read(&nohz.load_balancer) == -1) {
			/* make me the ilb owner */
			if (atomic_cmpxchg(&nohz.load_balancer, -1, cpu) == -1)
				return 1;
		} else if (atomic_read(&nohz.load_balancer) == cpu)
			return 1;
	} else {
		if (!cpu_isset(cpu, nohz.cpu_mask))
			return 0;

		cpu_clear(cpu, nohz.cpu_mask);

		if (atomic_read(&nohz.load_balancer) == cpu)
			if (atomic_cmpxchg(&nohz.load_balancer, cpu, -1) != cpu)
				BUG();
	}
	return 0;
}

/*
 * When add_timer_on() enqueues a timer into the timer wheel of an
 * idle CPU then this timer might expire before the next timer event
 * which is scheduled to wake up that CPU. In case of a completely
 * idle system the next event might even be infinite time into the
 * future. wake_up_idle_cpu() ensures that the CPU is woken up and
 * leaves the inner idle loop so the newly added timer is taken into
 * account when the CPU goes back to idle and evaluates the timer
 * wheel for the next timer event.
 */
void wake_up_idle_cpu(int cpu)
{
	struct task_struct *idle;
	struct rq *rq;

	if (cpu == smp_processor_id())
		return;

	rq = cpu_rq(cpu);
	idle = rq->idle;

	/*
	 * This is safe, as this function is called with the timer
	 * wheel base lock of (cpu) held. When the CPU is on the way
	 * to idle and has not yet set rq->curr to idle then it will
	 * be serialised on the timer wheel base lock and take the new
	 * timer into account automatically.
	 */
	if (unlikely(rq->curr != idle))
		return;

	/*
	 * We can set TIF_RESCHED on the idle task of the other CPU
	 * lockless. The worst case is that the other CPU runs the
	 * idle task through an additional NOOP schedule()
	 */
	set_tsk_thread_flag(idle, TIF_NEED_RESCHED);

	/* NEED_RESCHED must be visible before we test polling */
	smp_mb();
	if (!tsk_is_polling(idle))
		smp_send_reschedule(cpu);
}

#endif /* CONFIG_NO_HZ */

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * proper CPU and schedule it away if the CPU it's executing on
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely. The
 * call is not atomic; no spinlocks may be held.
 */
int set_cpus_allowed_ptr(struct task_struct *p, const cpumask_t *new_mask)
{
	unsigned long flags;
	int running_wrong = 0;
	int queued = 0;
	struct rq *rq;
	int ret = 0;

	rq = task_grq_lock(p, &flags);
	if (!cpus_intersects(*new_mask, cpu_online_map)) {
		ret = -EINVAL;
		goto out;
	}

	if (unlikely((p->flags & PF_THREAD_BOUND) && p != current &&
		     !cpus_equal(p->cpus_allowed, *new_mask))) {
		ret = -EINVAL;
		goto out;
	}

	queued = task_queued(p);

	p->cpus_allowed = *new_mask;

	/* Can the task run on the task's current CPU? If so, we're done */
	if (cpu_isset(task_cpu(p), *new_mask))
		goto out;

	if (task_running(p)) {
		/* Task is running on the wrong cpu now, reschedule it. */
		set_tsk_need_resched(p);
		running_wrong = 1;
	} else
		set_task_cpu(p, any_online_cpu(*new_mask));

out:
	if (queued)
		try_preempt(p, rq);
	task_grq_unlock(&flags);

	if (running_wrong)
		_cond_resched();

	return ret;
}
EXPORT_SYMBOL_GPL(set_cpus_allowed_ptr);

#ifdef CONFIG_HOTPLUG_CPU
/* Schedules idle task to be the next runnable task on current CPU.
 * It does so by boosting its priority to highest possible.
 * Used by CPU offline code.
 */
void sched_idle_next(void)
{
	int this_cpu = smp_processor_id();
	struct rq *rq = cpu_rq(this_cpu);
	struct task_struct *idle = rq->idle;
	unsigned long flags;

	/* cpu has to be offline */
	BUG_ON(cpu_online(this_cpu));

	/*
	 * Strictly not necessary since rest of the CPUs are stopped by now
	 * and interrupts disabled on the current cpu.
	 */
	time_grq_lock(rq, &flags);

	__setscheduler(idle, rq, SCHED_FIFO, MAX_RT_PRIO - 1);

	activate_idle_task(idle);
	set_tsk_need_resched(rq->curr);

	grq_unlock_irqrestore(&flags);
}

/*
 * Ensures that the idle task is using init_mm right before its cpu goes
 * offline.
 */
void idle_task_exit(void)
{
	struct mm_struct *mm = current->active_mm;

	BUG_ON(cpu_online(smp_processor_id()));

	if (mm != &init_mm)
		switch_mm(mm, &init_mm, current);
	mmdrop(mm);
}

#endif /* CONFIG_HOTPLUG_CPU */

#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_SYSCTL)

static struct ctl_table sd_ctl_dir[] = {
	{
		.procname	= "sched_domain",
		.mode		= 0555,
	},
	{0, },
};

static struct ctl_table sd_ctl_root[] = {
	{
		.ctl_name	= CTL_KERN,
		.procname	= "kernel",
		.mode		= 0555,
		.child		= sd_ctl_dir,
	},
	{0, },
};

static struct ctl_table *sd_alloc_ctl_entry(int n)
{
	struct ctl_table *entry =
		kcalloc(n, sizeof(struct ctl_table), GFP_KERNEL);

	return entry;
}

static void sd_free_ctl_entry(struct ctl_table **tablep)
{
	struct ctl_table *entry;

	/*
	 * In the intermediate directories, both the child directory and
	 * procname are dynamically allocated and could fail but the mode
	 * will always be set. In the lowest directory the names are
	 * static strings and all have proc handlers.
	 */
	for (entry = *tablep; entry->mode; entry++) {
		if (entry->child)
			sd_free_ctl_entry(&entry->child);
		if (entry->proc_handler == NULL)
			kfree(entry->procname);
	}

	kfree(*tablep);
	*tablep = NULL;
}

static void
set_table_entry(struct ctl_table *entry,
		const char *procname, void *data, int maxlen,
		mode_t mode, proc_handler *proc_handler)
{
	entry->procname = procname;
	entry->data = data;
	entry->maxlen = maxlen;
	entry->mode = mode;
	entry->proc_handler = proc_handler;
}

static struct ctl_table *
sd_alloc_ctl_domain_table(struct sched_domain *sd)
{
	struct ctl_table *table = sd_alloc_ctl_entry(12);

	if (table == NULL)
		return NULL;

	set_table_entry(&table[0], "min_interval", &sd->min_interval,
		sizeof(long), 0644, proc_doulongvec_minmax);
	set_table_entry(&table[1], "max_interval", &sd->max_interval,
		sizeof(long), 0644, proc_doulongvec_minmax);
	set_table_entry(&table[2], "busy_idx", &sd->busy_idx,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[3], "idle_idx", &sd->idle_idx,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[4], "newidle_idx", &sd->newidle_idx,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[5], "wake_idx", &sd->wake_idx,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[6], "forkexec_idx", &sd->forkexec_idx,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[7], "busy_factor", &sd->busy_factor,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[8], "imbalance_pct", &sd->imbalance_pct,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[9], "cache_nice_tries",
		&sd->cache_nice_tries,
		sizeof(int), 0644, proc_dointvec_minmax);
	set_table_entry(&table[10], "flags", &sd->flags,
		sizeof(int), 0644, proc_dointvec_minmax);
	/* &table[11] is terminator */

	return table;
}

static ctl_table *sd_alloc_ctl_cpu_table(int cpu)
{
	struct ctl_table *entry, *table;
	struct sched_domain *sd;
	int domain_num = 0, i;
	char buf[32];

	for_each_domain(cpu, sd)
		domain_num++;
	entry = table = sd_alloc_ctl_entry(domain_num + 1);
	if (table == NULL)
		return NULL;

	i = 0;
	for_each_domain(cpu, sd) {
		snprintf(buf, 32, "domain%d", i);
		entry->procname = kstrdup(buf, GFP_KERNEL);
		entry->mode = 0555;
		entry->child = sd_alloc_ctl_domain_table(sd);
		entry++;
		i++;
	}
	return table;
}

static struct ctl_table_header *sd_sysctl_header;
static void register_sched_domain_sysctl(void)
{
	int i, cpu_num = num_online_cpus();
	struct ctl_table *entry = sd_alloc_ctl_entry(cpu_num + 1);
	char buf[32];

	WARN_ON(sd_ctl_dir[0].child);
	sd_ctl_dir[0].child = entry;

	if (entry == NULL)
		return;

	for_each_online_cpu(i) {
		snprintf(buf, 32, "cpu%d", i);
		entry->procname = kstrdup(buf, GFP_KERNEL);
		entry->mode = 0555;
		entry->child = sd_alloc_ctl_cpu_table(i);
		entry++;
	}

	WARN_ON(sd_sysctl_header);
	sd_sysctl_header = register_sysctl_table(sd_ctl_root);
}

/* may be called multiple times per register */
static void unregister_sched_domain_sysctl(void)
{
	if (sd_sysctl_header)
		unregister_sysctl_table(sd_sysctl_header);
	sd_sysctl_header = NULL;
	if (sd_ctl_dir[0].child)
		sd_free_ctl_entry(&sd_ctl_dir[0].child);
}
#else
static void register_sched_domain_sysctl(void)
{
}
static void unregister_sched_domain_sysctl(void)
{
}
#endif

static void set_rq_online(struct rq *rq)
{
	if (!rq->online) {
		cpu_set(rq->cpu, rq->rd->online);
		rq->online = 1;
	}
}

static void set_rq_offline(struct rq *rq)
{
	if (rq->online) {
		cpu_clear(rq->cpu, rq->rd->online);
		rq->online = 0;
	}
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * This cpu is going down, so walk over the tasklist and find tasks that can
 * only run on this cpu and remove their affinity. Store their value in
 * unplugged_mask so it can be restored once their correct cpu is online. No
 * need to do anything special since they'll just move on next reschedule if
 * they're running.
 */
static void remove_cpu(unsigned long cpu)
{
	struct task_struct *p, *t;

	read_lock(&tasklist_lock);

	do_each_thread(t, p) {
		cpumask_t cpus_remaining;

		cpus_and(cpus_remaining, p->cpus_allowed, cpu_online_map);
		cpu_clear(cpu, cpus_remaining);
		if (cpus_empty(cpus_remaining)) {
			p->unplugged_mask = p->cpus_allowed;
			p->cpus_allowed = cpu_possible_map;
		}
	} while_each_thread(t, p);

	read_unlock(&tasklist_lock);
}

/*
 * This cpu is coming up so add it to the cpus_allowed.
 */
static void add_cpu(unsigned long cpu)
{
	struct task_struct *p, *t;

	read_lock(&tasklist_lock);

	do_each_thread(t, p) {
		/* Have we taken all the cpus from the unplugged_mask back */
		if (cpus_empty(p->unplugged_mask))
			continue;

		/* Was this cpu in the unplugged_mask mask */
		if (cpu_isset(cpu, p->unplugged_mask)) {
			cpu_set(cpu, p->cpus_allowed);
			if (cpus_subset(p->unplugged_mask, p->cpus_allowed)) {
				/*
				 * Have we set more than the unplugged_mask?
				 * If so, that means we have remnants set from
				 * the unplug/plug cycle and need to remove
				 * them. Then clear the unplugged_mask as we've
				 * set all the cpus back.
				 */
				p->cpus_allowed = p->unplugged_mask;
				cpus_clear(p->unplugged_mask);
			}
		}
	} while_each_thread(t, p);

	read_unlock(&tasklist_lock);
}
#else
static void add_cpu(unsigned long cpu)
{
}
#endif

/*
 * migration_call - callback that gets triggered when a CPU is added.
 */
static int __cpuinit
migration_call(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	struct task_struct *idle;
	int cpu = (long)hcpu;
	unsigned long flags;
	struct rq *rq;

	switch (action) {

	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		break;

	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		/* Update our root-domain */
		rq = cpu_rq(cpu);
		grq_lock_irqsave(&flags);
		if (rq->rd) {
			BUG_ON(!cpu_isset(cpu, rq->rd->span));

			set_rq_online(rq);
		}
		add_cpu(cpu);
		grq_unlock_irqrestore(&flags);
		break;

#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		break;

	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		cpuset_lock(); /* around calls to cpuset_cpus_allowed_lock() */
		rq = cpu_rq(cpu);
		idle = rq->idle;
		/* Idle task back to normal (off runqueue, low prio) */
		grq_lock_irq();
		remove_cpu(cpu);
		return_task(idle, 1);
		idle->static_prio = MAX_PRIO;
		__setscheduler(idle, rq, SCHED_NORMAL, 0);
		idle->prio = PRIO_LIMIT;
		set_rq_task(rq, idle);
		update_rq_clock(rq);
		grq_unlock_irq();
		cpuset_unlock();
		break;

	case CPU_DYING:
	case CPU_DYING_FROZEN:
		rq = cpu_rq(cpu);
		grq_lock_irqsave(&flags);
		if (rq->rd) {
			BUG_ON(!cpu_isset(cpu, rq->rd->span));
			set_rq_offline(rq);
		}
		grq_unlock_irqrestore(&flags);
		break;
#endif
	}
	return NOTIFY_OK;
}

/* Register at highest priority so that task migration (migrate_all_tasks)
 * happens before everything else.
 */
static struct notifier_block __cpuinitdata migration_notifier = {
	.notifier_call = migration_call,
	.priority = 10
};

int __init migration_init(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	int err;

	/* Start one for the boot CPU: */
	err = migration_call(&migration_notifier, CPU_UP_PREPARE, cpu);
	BUG_ON(err == NOTIFY_BAD);
	migration_call(&migration_notifier, CPU_ONLINE, cpu);
	register_cpu_notifier(&migration_notifier);

	return 0;
}
early_initcall(migration_init);
#endif

/*
 * sched_domains_mutex serialises calls to arch_init_sched_domains,
 * detach_destroy_domains and partition_sched_domains.
 */
static DEFINE_MUTEX(sched_domains_mutex);

#ifdef CONFIG_SMP

#ifdef CONFIG_SCHED_DEBUG

static inline const char *sd_level_to_string(enum sched_domain_level lvl)
{
	switch (lvl) {
	case SD_LV_NONE:
			return "NONE";
	case SD_LV_SIBLING:
			return "SIBLING";
	case SD_LV_MC:
			return "MC";
	case SD_LV_CPU:
			return "CPU";
	case SD_LV_NODE:
			return "NODE";
	case SD_LV_ALLNODES:
			return "ALLNODES";
	case SD_LV_MAX:
			return "MAX";

	}
	return "MAX";
}

static int sched_domain_debug_one(struct sched_domain *sd, int cpu, int level,
				  cpumask_t *groupmask)
{
	struct sched_group *group = sd->groups;
	char str[256];

	cpulist_scnprintf(str, sizeof(str), sd->span);
	cpus_clear(*groupmask);

	printk(KERN_DEBUG "%*s domain %d: ", level, "", level);

	if (!(sd->flags & SD_LOAD_BALANCE)) {
		printk("does not load-balance\n");
		if (sd->parent)
			printk(KERN_ERR "ERROR: !SD_LOAD_BALANCE domain"
					" has parent");
		return -1;
	}

	printk(KERN_CONT "span %s level %s\n",
		str, sd_level_to_string(sd->level));

	if (!cpu_isset(cpu, sd->span)) {
		printk(KERN_ERR "ERROR: domain->span does not contain "
				"CPU%d\n", cpu);
	}
	if (!cpu_isset(cpu, group->cpumask)) {
		printk(KERN_ERR "ERROR: domain->groups does not contain"
				" CPU%d\n", cpu);
	}

	printk(KERN_DEBUG "%*s groups:", level + 1, "");
	do {
		if (!group) {
			printk("\n");
			printk(KERN_ERR "ERROR: group is NULL\n");
			break;
		}

		if (!group->__cpu_power) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: domain->cpu_power not "
					"set\n");
			break;
		}

		if (!cpus_weight(group->cpumask)) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: empty group\n");
			break;
		}

		if (cpus_intersects(*groupmask, group->cpumask)) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: repeated CPUs\n");
			break;
		}

		cpus_or(*groupmask, *groupmask, group->cpumask);

		cpulist_scnprintf(str, sizeof(str), group->cpumask);
		printk(KERN_CONT " %s", str);

		group = group->next;
	} while (group != sd->groups);
	printk(KERN_CONT "\n");

	if (!cpus_equal(sd->span, *groupmask))
		printk(KERN_ERR "ERROR: groups don't span domain->span\n");

	if (sd->parent && !cpus_subset(*groupmask, sd->parent->span))
		printk(KERN_ERR "ERROR: parent span is not a superset "
			"of domain->span\n");
	return 0;
}

static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
	cpumask_t *groupmask;
	int level = 0;

	if (!sd) {
		printk(KERN_DEBUG "CPU%d attaching NULL sched-domain.\n", cpu);
		return;
	}

	printk(KERN_DEBUG "CPU%d attaching sched-domain:\n", cpu);

	groupmask = kmalloc(sizeof(cpumask_t), GFP_KERNEL);
	if (!groupmask) {
		printk(KERN_DEBUG "Cannot load-balance (out of memory)\n");
		return;
	}

	for (;;) {
		if (sched_domain_debug_one(sd, cpu, level, groupmask))
			break;
		level++;
		sd = sd->parent;
		if (!sd)
			break;
	}
	kfree(groupmask);
}
#else /* !CONFIG_SCHED_DEBUG */
# define sched_domain_debug(sd, cpu) do { } while (0)
#endif /* CONFIG_SCHED_DEBUG */

static int sd_degenerate(struct sched_domain *sd)
{
	if (cpus_weight(sd->span) == 1)
		return 1;

	/* Following flags need at least 2 groups */
	if (sd->flags & (SD_LOAD_BALANCE |
			 SD_BALANCE_NEWIDLE |
			 SD_BALANCE_FORK |
			 SD_BALANCE_EXEC |
			 SD_SHARE_CPUPOWER |
			 SD_SHARE_PKG_RESOURCES)) {
		if (sd->groups != sd->groups->next)
			return 0;
	}

	/* Following flags don't use groups */
	if (sd->flags & (SD_WAKE_IDLE |
			 SD_WAKE_AFFINE |
			 SD_WAKE_BALANCE))
		return 0;

	return 1;
}

static int
sd_parent_degenerate(struct sched_domain *sd, struct sched_domain *parent)
{
	unsigned long cflags = sd->flags, pflags = parent->flags;

	if (sd_degenerate(parent))
		return 1;

	if (!cpus_equal(sd->span, parent->span))
		return 0;

	/* Does parent contain flags not in child? */
	/* WAKE_BALANCE is a subset of WAKE_AFFINE */
	if (cflags & SD_WAKE_AFFINE)
		pflags &= ~SD_WAKE_BALANCE;
	/* Flags needing groups don't count if only 1 group in parent */
	if (parent->groups == parent->groups->next) {
		pflags &= ~(SD_LOAD_BALANCE |
				SD_BALANCE_NEWIDLE |
				SD_BALANCE_FORK |
				SD_BALANCE_EXEC |
				SD_SHARE_CPUPOWER |
				SD_SHARE_PKG_RESOURCES);
	}
	if (~cflags & pflags)
		return 0;

	return 1;
}

static void rq_attach_root(struct rq *rq, struct root_domain *rd)
{
	unsigned long flags;

	grq_lock_irqsave(&flags);

	if (rq->rd) {
		struct root_domain *old_rd = rq->rd;

		if (cpu_isset(rq->cpu, old_rd->online))
			set_rq_offline(rq);

		cpu_clear(rq->cpu, old_rd->span);

		if (atomic_dec_and_test(&old_rd->refcount))
			kfree(old_rd);
	}

	atomic_inc(&rd->refcount);
	rq->rd = rd;

	cpu_set(rq->cpu, rd->span);
	if (cpu_isset(rq->cpu, cpu_online_map))
		set_rq_online(rq);

	grq_unlock_irqrestore(&flags);
}

static void init_rootdomain(struct root_domain *rd)
{
	memset(rd, 0, sizeof(*rd));

	cpus_clear(rd->span);
	cpus_clear(rd->online);
}

static void init_defrootdomain(void)
{
	init_rootdomain(&def_root_domain);

	atomic_set(&def_root_domain.refcount, 1);
}

static struct root_domain *alloc_rootdomain(void)
{
	struct root_domain *rd;

	rd = kmalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	init_rootdomain(rd);

	return rd;
}

/*
 * Attach the domain 'sd' to 'cpu' as its base domain. Callers must
 * hold the hotplug lock.
 */
static void
cpu_attach_domain(struct sched_domain *sd, struct root_domain *rd, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct sched_domain *tmp;

	/* Remove the sched domains which do not contribute to scheduling. */
	for (tmp = sd; tmp; tmp = tmp->parent) {
		struct sched_domain *parent = tmp->parent;
		if (!parent)
			break;
		if (sd_parent_degenerate(tmp, parent)) {
			tmp->parent = parent->parent;
			if (parent->parent)
				parent->parent->child = tmp;
		}
	}

	if (sd && sd_degenerate(sd)) {
		sd = sd->parent;
		if (sd)
			sd->child = NULL;
	}

	sched_domain_debug(sd, cpu);

	rq_attach_root(rq, rd);
	rcu_assign_pointer(rq->sd, sd);
}

/* cpus with isolated domains */
static cpumask_t cpu_isolated_map = CPU_MASK_NONE;

/* Setup the mask of cpus configured for isolated domains */
static int __init isolated_cpu_setup(char *str)
{
	static int __initdata ints[NR_CPUS];
	int i;

	str = get_options(str, ARRAY_SIZE(ints), ints);
	cpus_clear(cpu_isolated_map);
	for (i = 1; i <= ints[0]; i++)
		if (ints[i] < NR_CPUS)
			cpu_set(ints[i], cpu_isolated_map);
	return 1;
}

__setup("isolcpus=", isolated_cpu_setup);

/*
 * init_sched_build_groups takes the cpumask we wish to span, and a pointer
 * to a function which identifies what group(along with sched group) a CPU
 * belongs to. The return value of group_fn must be a >= 0 and < NR_CPUS
 * (due to the fact that we keep track of groups covered with a cpumask_t).
 *
 * init_sched_build_groups will build a circular linked list of the groups
 * covered by the given span, and will set each group's ->cpumask correctly,
 * and ->cpu_power to 0.
 */
static void
init_sched_build_groups(const cpumask_t *span, const cpumask_t *cpu_map,
			int (*group_fn)(int cpu, const cpumask_t *cpu_map,
					struct sched_group **sg,
					cpumask_t *tmpmask),
			cpumask_t *covered, cpumask_t *tmpmask)
{
	struct sched_group *first = NULL, *last = NULL;
	int i;

	cpus_clear(*covered);

	for_each_cpu_mask_nr(i, *span) {
		struct sched_group *sg;
		int group = group_fn(i, cpu_map, &sg, tmpmask);
		int j;

		if (cpu_isset(i, *covered))
			continue;

		cpus_clear(sg->cpumask);
		sg->__cpu_power = 0;

		for_each_cpu_mask_nr(j, *span) {
			if (group_fn(j, cpu_map, NULL, tmpmask) != group)
				continue;

			cpu_set(j, *covered);
			cpu_set(j, sg->cpumask);
		}
		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
	}
	last->next = first;
}

#define SD_NODES_PER_DOMAIN 16

#ifdef CONFIG_NUMA

/**
 * find_next_best_node - find the next node to include in a sched_domain
 * @node: node whose sched_domain we're building
 * @used_nodes: nodes already in the sched_domain
 *
 * Find the next node to include in a given scheduling domain. Simply
 * finds the closest node not already in the @used_nodes map.
 *
 * Should use nodemask_t.
 */
static int find_next_best_node(int node, nodemask_t *used_nodes)
{
	int i, n, val, min_val, best_node = 0;

	min_val = INT_MAX;

	for (i = 0; i < nr_node_ids; i++) {
		/* Start at @node */
		n = (node + i) % nr_node_ids;

		if (!nr_cpus_node(n))
			continue;

		/* Skip already used nodes */
		if (node_isset(n, *used_nodes))
			continue;

		/* Simple min distance search */
		val = node_distance(node, n);

		if (val < min_val) {
			min_val = val;
			best_node = n;
		}
	}

	node_set(best_node, *used_nodes);
	return best_node;
}

/**
 * sched_domain_node_span - get a cpumask for a node's sched_domain
 * @node: node whose cpumask we're constructing
 * @span: resulting cpumask
 *
 * Given a node, construct a good cpumask for its sched_domain to span. It
 * should be one that prevents unnecessary balancing, but also spreads tasks
 * out optimally.
 */
static void sched_domain_node_span(int node, cpumask_t *span)
{
	nodemask_t used_nodes;
	node_to_cpumask_ptr(nodemask, node);
	int i;

	cpus_clear(*span);
	nodes_clear(used_nodes);

	cpus_or(*span, *span, *nodemask);
	node_set(node, used_nodes);

	for (i = 1; i < SD_NODES_PER_DOMAIN; i++) {
		int next_node = find_next_best_node(node, &used_nodes);

		node_to_cpumask_ptr_next(nodemask, next_node);
		cpus_or(*span, *span, *nodemask);
	}
}
#endif /* CONFIG_NUMA */

int sched_smt_power_savings = 0, sched_mc_power_savings = 0;

/*
 * SMT sched-domains:
 */
#ifdef CONFIG_SCHED_SMT
static DEFINE_PER_CPU(struct sched_domain, cpu_domains);
static DEFINE_PER_CPU(struct sched_group, sched_group_cpus);

static int
cpu_to_cpu_group(int cpu, const cpumask_t *cpu_map, struct sched_group **sg,
		 cpumask_t *unused)
{
	if (sg)
		*sg = &per_cpu(sched_group_cpus, cpu);
	return cpu;
}
#endif /* CONFIG_SCHED_SMT */

/*
 * multi-core sched-domains:
 */
#ifdef CONFIG_SCHED_MC
static DEFINE_PER_CPU(struct sched_domain, core_domains);
static DEFINE_PER_CPU(struct sched_group, sched_group_core);
#endif /* CONFIG_SCHED_MC */

#if defined(CONFIG_SCHED_MC) && defined(CONFIG_SCHED_SMT)
static int
cpu_to_core_group(int cpu, const cpumask_t *cpu_map, struct sched_group **sg,
		  cpumask_t *mask)
{
	int group;

	*mask = per_cpu(cpu_sibling_map, cpu);
	cpus_and(*mask, *mask, *cpu_map);
	group = first_cpu(*mask);
	if (sg)
		*sg = &per_cpu(sched_group_core, group);
	return group;
}
#elif defined(CONFIG_SCHED_MC)
static int
cpu_to_core_group(int cpu, const cpumask_t *cpu_map, struct sched_group **sg,
		  cpumask_t *unused)
{
	if (sg)
		*sg = &per_cpu(sched_group_core, cpu);
	return cpu;
}
#endif

static DEFINE_PER_CPU(struct sched_domain, phys_domains);
static DEFINE_PER_CPU(struct sched_group, sched_group_phys);

static int
cpu_to_phys_group(int cpu, const cpumask_t *cpu_map, struct sched_group **sg,
		  cpumask_t *mask)
{
	int group;
#ifdef CONFIG_SCHED_MC
	*mask = cpu_coregroup_map(cpu);
	cpus_and(*mask, *mask, *cpu_map);
	group = first_cpu(*mask);
#elif defined(CONFIG_SCHED_SMT)
	*mask = per_cpu(cpu_sibling_map, cpu);
	cpus_and(*mask, *mask, *cpu_map);
	group = first_cpu(*mask);
#else
	group = cpu;
#endif
	if (sg)
		*sg = &per_cpu(sched_group_phys, group);
	return group;
}

#ifdef CONFIG_NUMA
/*
 * The init_sched_build_groups can't handle what we want to do with node
 * groups, so roll our own. Now each node has its own list of groups which
 * gets dynamically allocated.
 */
static DEFINE_PER_CPU(struct sched_domain, node_domains);
static struct sched_group ***sched_group_nodes_bycpu;

static DEFINE_PER_CPU(struct sched_domain, allnodes_domains);
static DEFINE_PER_CPU(struct sched_group, sched_group_allnodes);

static int cpu_to_allnodes_group(int cpu, const cpumask_t *cpu_map,
				 struct sched_group **sg, cpumask_t *nodemask)
{
	int group;

	*nodemask = node_to_cpumask(cpu_to_node(cpu));
	cpus_and(*nodemask, *nodemask, *cpu_map);
	group = first_cpu(*nodemask);

	if (sg)
		*sg = &per_cpu(sched_group_allnodes, group);
	return group;
}

static void init_numa_sched_groups_power(struct sched_group *group_head)
{
	struct sched_group *sg = group_head;
	int j;

	if (!sg)
		return;
	do {
		for_each_cpu_mask_nr(j, sg->cpumask) {
			struct sched_domain *sd;

			sd = &per_cpu(phys_domains, j);
			if (j != first_cpu(sd->groups->cpumask)) {
				/*
				 * Only add "power" once for each
				 * physical package.
				 */
				continue;
			}

			sg_inc_cpu_power(sg, sd->groups->__cpu_power);
		}
		sg = sg->next;
	} while (sg != group_head);
}
#endif /* CONFIG_NUMA */

#ifdef CONFIG_NUMA
/* Free memory allocated for various sched_group structures */
static void free_sched_groups(const cpumask_t *cpu_map, cpumask_t *nodemask)
{
	int cpu, i;

	for_each_cpu_mask_nr(cpu, *cpu_map) {
		struct sched_group **sched_group_nodes
			= sched_group_nodes_bycpu[cpu];

		if (!sched_group_nodes)
			continue;

		for (i = 0; i < nr_node_ids; i++) {
			struct sched_group *oldsg, *sg = sched_group_nodes[i];

			*nodemask = node_to_cpumask(i);
			cpus_and(*nodemask, *nodemask, *cpu_map);
			if (cpus_empty(*nodemask))
				continue;

			if (sg == NULL)
				continue;
			sg = sg->next;
next_sg:
			oldsg = sg;
			sg = sg->next;
			kfree(oldsg);
			if (oldsg != sched_group_nodes[i])
				goto next_sg;
		}
		kfree(sched_group_nodes);
		sched_group_nodes_bycpu[cpu] = NULL;
	}
}
#else /* !CONFIG_NUMA */
static void free_sched_groups(const cpumask_t *cpu_map, cpumask_t *nodemask)
{
}
#endif /* CONFIG_NUMA */

/*
 * Initialise sched groups cpu_power.
 *
 * cpu_power indicates the capacity of sched group, which is used while
 * distributing the load between different sched groups in a sched domain.
 * Typically cpu_power for all the groups in a sched domain will be same unless
 * there are asymmetries in the topology. If there are asymmetries, group
 * having more cpu_power will pickup more load compared to the group having
 * less cpu_power.
 *
 * cpu_power will be a multiple of SCHED_LOAD_SCALE. This multiple represents
 * the maximum number of tasks a group can handle in the presence of other idle
 * or lightly loaded groups in the same sched domain.
 */
static void init_sched_groups_power(int cpu, struct sched_domain *sd)
{
	struct sched_domain *child;
	struct sched_group *group;

	WARN_ON(!sd || !sd->groups);

	if (cpu != first_cpu(sd->groups->cpumask))
		return;

	child = sd->child;

	sd->groups->__cpu_power = 0;

	/*
	 * For perf policy, if the groups in child domain share resources
	 * (for example cores sharing some portions of the cache hierarchy
	 * or SMT), then set this domain groups cpu_power such that each group
	 * can handle only one task, when there are other idle groups in the
	 * same sched domain.
	 */
	if (!child || (!(sd->flags & SD_POWERSAVINGS_BALANCE) &&
		       (child->flags &
			(SD_SHARE_CPUPOWER | SD_SHARE_PKG_RESOURCES)))) {
		sg_inc_cpu_power(sd->groups, SCHED_LOAD_SCALE);
		return;
	}

	/*
	 * add cpu_power of each child group to this groups cpu_power
	 */
	group = child->groups;
	do {
		sg_inc_cpu_power(sd->groups, group->__cpu_power);
		group = group->next;
	} while (group != child->groups);
}

/*
 * Initialisers for schedule domains
 * Non-inlined to reduce accumulated stack pressure in build_sched_domains()
 */

#define	SD_INIT(sd, type)	sd_init_##type(sd)
#define SD_INIT_FUNC(type)	\
static noinline void sd_init_##type(struct sched_domain *sd)	\
{								\
	memset(sd, 0, sizeof(*sd));				\
	*sd = SD_##type##_INIT;					\
	sd->level = SD_LV_##type;				\
}

SD_INIT_FUNC(CPU)
#ifdef CONFIG_NUMA
 SD_INIT_FUNC(ALLNODES)
 SD_INIT_FUNC(NODE)
#endif
#ifdef CONFIG_SCHED_SMT
 SD_INIT_FUNC(SIBLING)
#endif
#ifdef CONFIG_SCHED_MC
 SD_INIT_FUNC(MC)
#endif

/*
 * To minimize stack usage kmalloc room for cpumasks and share the
 * space as the usage in build_sched_domains() dictates.  Used only
 * if the amount of space is significant.
 */
struct allmasks {
	cpumask_t tmpmask;			/* make this one first */
	union {
		cpumask_t nodemask;
		cpumask_t this_sibling_map;
		cpumask_t this_core_map;
	};
	cpumask_t send_covered;

#ifdef CONFIG_NUMA
	cpumask_t domainspan;
	cpumask_t covered;
	cpumask_t notcovered;
#endif
};

#if	NR_CPUS > 128
#define	SCHED_CPUMASK_ALLOC		1
#define	SCHED_CPUMASK_FREE(v)		kfree(v)
#define	SCHED_CPUMASK_DECLARE(v)	struct allmasks *v
#else
#define	SCHED_CPUMASK_ALLOC		0
#define	SCHED_CPUMASK_FREE(v)
#define	SCHED_CPUMASK_DECLARE(v)	struct allmasks _v, *v = &_v
#endif

#define	SCHED_CPUMASK_VAR(v, a) 	cpumask_t *v = (cpumask_t *) \
			((unsigned long)(a) + offsetof(struct allmasks, v))

static int default_relax_domain_level = -1;

static int __init setup_relax_domain_level(char *str)
{
	unsigned long val;

	val = simple_strtoul(str, NULL, 0);
	if (val < SD_LV_MAX)
		default_relax_domain_level = val;

	return 1;
}
__setup("relax_domain_level=", setup_relax_domain_level);

static void set_domain_attribute(struct sched_domain *sd,
				 struct sched_domain_attr *attr)
{
	int request;

	if (!attr || attr->relax_domain_level < 0) {
		if (default_relax_domain_level < 0)
			return;
		else
			request = default_relax_domain_level;
	} else
		request = attr->relax_domain_level;
	if (request < sd->level) {
		/* turn off idle balance on this domain */
		sd->flags &= ~(SD_WAKE_IDLE|SD_BALANCE_NEWIDLE);
	} else {
		/* turn on idle balance on this domain */
		sd->flags |= (SD_WAKE_IDLE_FAR|SD_BALANCE_NEWIDLE);
	}
}

/*
 * Build sched domains for a given set of cpus and attach the sched domains
 * to the individual cpus
 */
static int __build_sched_domains(const cpumask_t *cpu_map,
				 struct sched_domain_attr *attr)
{
	int i;
	struct root_domain *rd;
	SCHED_CPUMASK_DECLARE(allmasks);
	cpumask_t *tmpmask;
#ifdef CONFIG_NUMA
	struct sched_group **sched_group_nodes = NULL;
	int sd_allnodes = 0;

	/*
	 * Allocate the per-node list of sched groups
	 */
	sched_group_nodes = kcalloc(nr_node_ids, sizeof(struct sched_group *),
				    GFP_KERNEL);
	if (!sched_group_nodes) {
		printk(KERN_WARNING "Can not alloc sched group node list\n");
		return -ENOMEM;
	}
#endif

	rd = alloc_rootdomain();
	if (!rd) {
		printk(KERN_WARNING "Cannot alloc root domain\n");
#ifdef CONFIG_NUMA
		kfree(sched_group_nodes);
#endif
		return -ENOMEM;
	}

#if SCHED_CPUMASK_ALLOC
	/* get space for all scratch cpumask variables */
	allmasks = kmalloc(sizeof(*allmasks), GFP_KERNEL);
	if (!allmasks) {
		printk(KERN_WARNING "Cannot alloc cpumask array\n");
		kfree(rd);
#ifdef CONFIG_NUMA
		kfree(sched_group_nodes);
#endif
		return -ENOMEM;
	}
#endif
	tmpmask = (cpumask_t *)allmasks;


#ifdef CONFIG_NUMA
	sched_group_nodes_bycpu[first_cpu(*cpu_map)] = sched_group_nodes;
#endif

	/*
	 * Set up domains for cpus specified by the cpu_map.
	 */
	for_each_cpu_mask_nr(i, *cpu_map) {
		struct sched_domain *sd = NULL, *p;
		SCHED_CPUMASK_VAR(nodemask, allmasks);

		*nodemask = node_to_cpumask(cpu_to_node(i));
		cpus_and(*nodemask, *nodemask, *cpu_map);

#ifdef CONFIG_NUMA
		if (cpus_weight(*cpu_map) >
				SD_NODES_PER_DOMAIN*cpus_weight(*nodemask)) {
			sd = &per_cpu(allnodes_domains, i);
			SD_INIT(sd, ALLNODES);
			set_domain_attribute(sd, attr);
			sd->span = *cpu_map;
			cpu_to_allnodes_group(i, cpu_map, &sd->groups, tmpmask);
			p = sd;
			sd_allnodes = 1;
		} else
			p = NULL;

		sd = &per_cpu(node_domains, i);
		SD_INIT(sd, NODE);
		set_domain_attribute(sd, attr);
		sched_domain_node_span(cpu_to_node(i), &sd->span);
		sd->parent = p;
		if (p)
			p->child = sd;
		cpus_and(sd->span, sd->span, *cpu_map);
#endif

		p = sd;
		sd = &per_cpu(phys_domains, i);
		SD_INIT(sd, CPU);
		set_domain_attribute(sd, attr);
		sd->span = *nodemask;
		sd->parent = p;
		if (p)
			p->child = sd;
		cpu_to_phys_group(i, cpu_map, &sd->groups, tmpmask);

#ifdef CONFIG_SCHED_MC
		p = sd;
		sd = &per_cpu(core_domains, i);
		SD_INIT(sd, MC);
		set_domain_attribute(sd, attr);
		sd->span = cpu_coregroup_map(i);
		cpus_and(sd->span, sd->span, *cpu_map);
		sd->parent = p;
		p->child = sd;
		cpu_to_core_group(i, cpu_map, &sd->groups, tmpmask);
#endif

#ifdef CONFIG_SCHED_SMT
		p = sd;
		sd = &per_cpu(cpu_domains, i);
		SD_INIT(sd, SIBLING);
		set_domain_attribute(sd, attr);
		sd->span = per_cpu(cpu_sibling_map, i);
		cpus_and(sd->span, sd->span, *cpu_map);
		sd->parent = p;
		p->child = sd;
		cpu_to_cpu_group(i, cpu_map, &sd->groups, tmpmask);
#endif
	}

#ifdef CONFIG_SCHED_SMT
	/* Set up CPU (sibling) groups */
	for_each_cpu_mask_nr(i, *cpu_map) {
		SCHED_CPUMASK_VAR(this_sibling_map, allmasks);
		SCHED_CPUMASK_VAR(send_covered, allmasks);

		*this_sibling_map = per_cpu(cpu_sibling_map, i);
		cpus_and(*this_sibling_map, *this_sibling_map, *cpu_map);
		if (i != first_cpu(*this_sibling_map))
			continue;

		init_sched_build_groups(this_sibling_map, cpu_map,
					&cpu_to_cpu_group,
					send_covered, tmpmask);
	}
#endif

#ifdef CONFIG_SCHED_MC
	/* Set up multi-core groups */
	for_each_cpu_mask_nr(i, *cpu_map) {
		SCHED_CPUMASK_VAR(this_core_map, allmasks);
		SCHED_CPUMASK_VAR(send_covered, allmasks);

		*this_core_map = cpu_coregroup_map(i);
		cpus_and(*this_core_map, *this_core_map, *cpu_map);
		if (i != first_cpu(*this_core_map))
			continue;

		init_sched_build_groups(this_core_map, cpu_map,
					&cpu_to_core_group,
					send_covered, tmpmask);
	}
#endif

	/* Set up physical groups */
	for (i = 0; i < nr_node_ids; i++) {
		SCHED_CPUMASK_VAR(nodemask, allmasks);
		SCHED_CPUMASK_VAR(send_covered, allmasks);

		*nodemask = node_to_cpumask(i);
		cpus_and(*nodemask, *nodemask, *cpu_map);
		if (cpus_empty(*nodemask))
			continue;

		init_sched_build_groups(nodemask, cpu_map,
					&cpu_to_phys_group,
					send_covered, tmpmask);
	}

#ifdef CONFIG_NUMA
	/* Set up node groups */
	if (sd_allnodes) {
		SCHED_CPUMASK_VAR(send_covered, allmasks);

		init_sched_build_groups(cpu_map, cpu_map,
					&cpu_to_allnodes_group,
					send_covered, tmpmask);
	}

	for (i = 0; i < nr_node_ids; i++) {
		/* Set up node groups */
		struct sched_group *sg, *prev;
		SCHED_CPUMASK_VAR(nodemask, allmasks);
		SCHED_CPUMASK_VAR(domainspan, allmasks);
		SCHED_CPUMASK_VAR(covered, allmasks);
		int j;

		*nodemask = node_to_cpumask(i);
		cpus_clear(*covered);

		cpus_and(*nodemask, *nodemask, *cpu_map);
		if (cpus_empty(*nodemask)) {
			sched_group_nodes[i] = NULL;
			continue;
		}

		sched_domain_node_span(i, domainspan);
		cpus_and(*domainspan, *domainspan, *cpu_map);

		sg = kmalloc_node(sizeof(struct sched_group), GFP_KERNEL, i);
		if (!sg) {
			printk(KERN_WARNING "Can not alloc domain group for "
				"node %d\n", i);
			goto error;
		}
		sched_group_nodes[i] = sg;
		for_each_cpu_mask_nr(j, *nodemask) {
			struct sched_domain *sd;

			sd = &per_cpu(node_domains, j);
			sd->groups = sg;
		}
		sg->__cpu_power = 0;
		sg->cpumask = *nodemask;
		sg->next = sg;
		cpus_or(*covered, *covered, *nodemask);
		prev = sg;

		for (j = 0; j < nr_node_ids; j++) {
			SCHED_CPUMASK_VAR(notcovered, allmasks);
			int n = (i + j) % nr_node_ids;
			node_to_cpumask_ptr(pnodemask, n);

			cpus_complement(*notcovered, *covered);
			cpus_and(*tmpmask, *notcovered, *cpu_map);
			cpus_and(*tmpmask, *tmpmask, *domainspan);
			if (cpus_empty(*tmpmask))
				break;

			cpus_and(*tmpmask, *tmpmask, *pnodemask);
			if (cpus_empty(*tmpmask))
				continue;

			sg = kmalloc_node(sizeof(struct sched_group),
					  GFP_KERNEL, i);
			if (!sg) {
				printk(KERN_WARNING
				"Can not alloc domain group for node %d\n", j);
				goto error;
			}
			sg->__cpu_power = 0;
			sg->cpumask = *tmpmask;
			sg->next = prev->next;
			cpus_or(*covered, *covered, *tmpmask);
			prev->next = sg;
			prev = sg;
		}
	}
#endif

	/* Calculate CPU power for physical packages and nodes */
#ifdef CONFIG_SCHED_SMT
	for_each_cpu_mask_nr(i, *cpu_map) {
		struct sched_domain *sd = &per_cpu(cpu_domains, i);

		init_sched_groups_power(i, sd);
	}
#endif
#ifdef CONFIG_SCHED_MC
	for_each_cpu_mask_nr(i, *cpu_map) {
		struct sched_domain *sd = &per_cpu(core_domains, i);

		init_sched_groups_power(i, sd);
	}
#endif

	for_each_cpu_mask_nr(i, *cpu_map) {
		struct sched_domain *sd = &per_cpu(phys_domains, i);

		init_sched_groups_power(i, sd);
	}

#ifdef CONFIG_NUMA
	for (i = 0; i < nr_node_ids; i++)
		init_numa_sched_groups_power(sched_group_nodes[i]);

	if (sd_allnodes) {
		struct sched_group *sg;

		cpu_to_allnodes_group(first_cpu(*cpu_map), cpu_map, &sg,
								tmpmask);
		init_numa_sched_groups_power(sg);
	}
#endif

	/* Attach the domains */
	for_each_cpu_mask_nr(i, *cpu_map) {
		struct sched_domain *sd;
#ifdef CONFIG_SCHED_SMT
		sd = &per_cpu(cpu_domains, i);
#elif defined(CONFIG_SCHED_MC)
		sd = &per_cpu(core_domains, i);
#else
		sd = &per_cpu(phys_domains, i);
#endif
		cpu_attach_domain(sd, rd, i);
	}

	SCHED_CPUMASK_FREE((void *)allmasks);
	return 0;

#ifdef CONFIG_NUMA
error:
	free_sched_groups(cpu_map, tmpmask);
	SCHED_CPUMASK_FREE((void *)allmasks);
	return -ENOMEM;
#endif
}

static int build_sched_domains(const cpumask_t *cpu_map)
{
	return __build_sched_domains(cpu_map, NULL);
}

static cpumask_t *doms_cur;	/* current sched domains */
static int ndoms_cur;		/* number of sched domains in 'doms_cur' */
static struct sched_domain_attr *dattr_cur;
				/* attribues of custom domains in 'doms_cur' */

/*
 * Special case: If a kmalloc of a doms_cur partition (array of
 * cpumask_t) fails, then fallback to a single sched domain,
 * as determined by the single cpumask_t fallback_doms.
 */
static cpumask_t fallback_doms;

void __attribute__((weak)) arch_update_cpu_topology(void)
{
}

/*
 * Set up scheduler domains and groups. Callers must hold the hotplug lock.
 * For now this just excludes isolated cpus, but could be used to
 * exclude other special cases in the future.
 */
static int arch_init_sched_domains(const cpumask_t *cpu_map)
{
	int err;

	arch_update_cpu_topology();
	ndoms_cur = 1;
	doms_cur = kmalloc(sizeof(cpumask_t), GFP_KERNEL);
	if (!doms_cur)
		doms_cur = &fallback_doms;
	cpus_andnot(*doms_cur, *cpu_map, cpu_isolated_map);
	dattr_cur = NULL;
	err = build_sched_domains(doms_cur);
	register_sched_domain_sysctl();

	return err;
}

static void arch_destroy_sched_domains(const cpumask_t *cpu_map,
				       cpumask_t *tmpmask)
{
	free_sched_groups(cpu_map, tmpmask);
}

/*
 * Detach sched domains from a group of cpus specified in cpu_map
 * These cpus will now be attached to the NULL domain
 */
static void detach_destroy_domains(const cpumask_t *cpu_map)
{
	cpumask_t tmpmask;
	int i;

	unregister_sched_domain_sysctl();

	for_each_cpu_mask_nr(i, *cpu_map)
		cpu_attach_domain(NULL, &def_root_domain, i);
	synchronize_sched();
	arch_destroy_sched_domains(cpu_map, &tmpmask);
}

/* handle null as "default" */
static int dattrs_equal(struct sched_domain_attr *cur, int idx_cur,
			struct sched_domain_attr *new, int idx_new)
{
	struct sched_domain_attr tmp;

	/* fast path */
	if (!new && !cur)
		return 1;

	tmp = SD_ATTR_INIT;
	return !memcmp(cur ? (cur + idx_cur) : &tmp,
			new ? (new + idx_new) : &tmp,
			sizeof(struct sched_domain_attr));
}

/*
 * Partition sched domains as specified by the 'ndoms_new'
 * cpumasks in the array doms_new[] of cpumasks. This compares
 * doms_new[] to the current sched domain partitioning, doms_cur[].
 * It destroys each deleted domain and builds each new domain.
 *
 * 'doms_new' is an array of cpumask_t's of length 'ndoms_new'.
 * The masks don't intersect (don't overlap.) We should setup one
 * sched domain for each mask. CPUs not in any of the cpumasks will
 * not be load balanced. If the same cpumask appears both in the
 * current 'doms_cur' domains and in the new 'doms_new', we can leave
 * it as it is.
 *
 * The passed in 'doms_new' should be kmalloc'd. This routine takes
 * ownership of it and will kfree it when done with it. If the caller
 * failed the kmalloc call, then it can pass in doms_new == NULL,
 * and partition_sched_domains() will fallback to the single partition
 * 'fallback_doms', it also forces the domains to be rebuilt.
 *
 * If doms_new==NULL it will be replaced with cpu_online_map.
 * ndoms_new==0 is a special case for destroying existing domains.
 * It will not create the default domain.
 *
 * Call with hotplug lock held
 */
void partition_sched_domains(int ndoms_new, cpumask_t *doms_new,
			     struct sched_domain_attr *dattr_new)
{
	int i, j, n;

	mutex_lock(&sched_domains_mutex);

	/* always unregister in case we don't destroy any domains */
	unregister_sched_domain_sysctl();

	n = doms_new ? ndoms_new : 0;

	/* Destroy deleted domains */
	for (i = 0; i < ndoms_cur; i++) {
		for (j = 0; j < n; j++) {
			if (cpus_equal(doms_cur[i], doms_new[j])
			    && dattrs_equal(dattr_cur, i, dattr_new, j))
				goto match1;
		}
		/* no match - a current sched domain not in new doms_new[] */
		detach_destroy_domains(doms_cur + i);
match1:
		;
	}

	if (doms_new == NULL) {
		ndoms_cur = 0;
		doms_new = &fallback_doms;
		cpus_andnot(doms_new[0], cpu_online_map, cpu_isolated_map);
		dattr_new = NULL;
	}

	/* Build new domains */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < ndoms_cur; j++) {
			if (cpus_equal(doms_new[i], doms_cur[j])
			    && dattrs_equal(dattr_new, i, dattr_cur, j))
				goto match2;
		}
		/* no match - add a new doms_new */
		__build_sched_domains(doms_new + i,
					dattr_new ? dattr_new + i : NULL);
match2:
		;
	}

	/* Remember the new sched domains */
	if (doms_cur != &fallback_doms)
		kfree(doms_cur);
	kfree(dattr_cur);	/* kfree(NULL) is safe */
	doms_cur = doms_new;
	dattr_cur = dattr_new;
	ndoms_cur = ndoms_new;

	register_sched_domain_sysctl();

	mutex_unlock(&sched_domains_mutex);
}

#if defined(CONFIG_SCHED_MC) || defined(CONFIG_SCHED_SMT)
int arch_reinit_sched_domains(void)
{
	get_online_cpus();

	/* Destroy domains first to force the rebuild */
	partition_sched_domains(0, NULL, NULL);

	rebuild_sched_domains();
	put_online_cpus();

	return 0;
}

static ssize_t sched_power_savings_store(const char *buf, size_t count, int smt)
{
	int ret;

	if (buf[0] != '0' && buf[0] != '1')
		return -EINVAL;

	if (smt)
		sched_smt_power_savings = (buf[0] == '1');
	else
		sched_mc_power_savings = (buf[0] == '1');

	ret = arch_reinit_sched_domains();

	return ret ? ret : count;
}

#ifdef CONFIG_SCHED_MC
static ssize_t sched_mc_power_savings_show(struct sysdev_class *class,
					   char *page)
{
	return sprintf(page, "%u\n", sched_mc_power_savings);
}
static ssize_t sched_mc_power_savings_store(struct sysdev_class *class,
					    const char *buf, size_t count)
{
	return sched_power_savings_store(buf, count, 0);
}
static SYSDEV_CLASS_ATTR(sched_mc_power_savings, 0644,
			 sched_mc_power_savings_show,
			 sched_mc_power_savings_store);
#endif

#ifdef CONFIG_SCHED_SMT
static ssize_t sched_smt_power_savings_show(struct sysdev_class *dev,
					    char *page)
{
	return sprintf(page, "%u\n", sched_smt_power_savings);
}
static ssize_t sched_smt_power_savings_store(struct sysdev_class *dev,
					     const char *buf, size_t count)
{
	return sched_power_savings_store(buf, count, 1);
}
static SYSDEV_CLASS_ATTR(sched_smt_power_savings, 0644,
		   sched_smt_power_savings_show,
		   sched_smt_power_savings_store);
#endif

int sched_create_sysfs_power_savings_entries(struct sysdev_class *cls)
{
	int err = 0;

#ifdef CONFIG_SCHED_SMT
	if (smt_capable())
		err = sysfs_create_file(&cls->kset.kobj,
					&attr_sched_smt_power_savings.attr);
#endif
#ifdef CONFIG_SCHED_MC
	if (!err && mc_capable())
		err = sysfs_create_file(&cls->kset.kobj,
					&attr_sched_mc_power_savings.attr);
#endif
	return err;
}
#endif /* CONFIG_SCHED_MC || CONFIG_SCHED_SMT */

#ifndef CONFIG_CPUSETS
/*
 * Add online and remove offline CPUs from the scheduler domains.
 * When cpusets are enabled they take over this function.
 */
static int update_sched_domains(struct notifier_block *nfb,
				unsigned long action, void *hcpu)
{
	switch (action) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		partition_sched_domains(1, NULL, NULL);
		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}
#endif

static int update_runtime(struct notifier_block *nfb,
				unsigned long action, void *hcpu)
{
	switch (action) {
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		return NOTIFY_OK;

	case CPU_DOWN_FAILED:
	case CPU_DOWN_FAILED_FROZEN:
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}

void __init sched_init_smp(void)
{
	struct sched_domain *sd;
	int cpu;

	cpumask_t non_isolated_cpus;

#if defined(CONFIG_NUMA)
	sched_group_nodes_bycpu = kzalloc(nr_cpu_ids * sizeof(void **),
								GFP_KERNEL);
	BUG_ON(sched_group_nodes_bycpu == NULL);
#endif
	get_online_cpus();
	mutex_lock(&sched_domains_mutex);
	arch_init_sched_domains(&cpu_online_map);
	cpus_andnot(non_isolated_cpus, cpu_possible_map, cpu_isolated_map);
	if (cpus_empty(non_isolated_cpus))
		cpu_set(smp_processor_id(), non_isolated_cpus);
	mutex_unlock(&sched_domains_mutex);
	put_online_cpus();

#ifndef CONFIG_CPUSETS
	/* XXX: Theoretical race here - CPU may be hotplugged now */
	hotcpu_notifier(update_sched_domains, 0);
#endif

	/* RT runtime code needs to handle some hotplug events */
	hotcpu_notifier(update_runtime, 0);

	/* Move init over to a non-isolated CPU */
	if (set_cpus_allowed_ptr(current, &non_isolated_cpus) < 0)
		BUG();

	/*
	 * Assume that every added cpu gives us slightly less overall latency
	 * allowing us to increase the base rr_interval, but in a non linear
	 * fashion.
	 */
	rr_interval *= 1 + ilog2(num_online_cpus());

	/*
	 * Set up the relative cache distance of each online cpu from each
	 * other in a simple array for quick lookup. Locality is determined
	 * by the closest sched_domain that CPUs are separated by. CPUs with
	 * shared cache in SMT and MC are treated as local. Separate CPUs
	 * (within the same package or physically) within the same node are
	 * treated as not local. CPUs not even in the same domain (different
	 * nodes) are treated as very distant.
	 */
	for_each_online_cpu(cpu) {
		for_each_domain(cpu, sd) {
			struct rq *rq = cpu_rq(cpu);
			unsigned long locality;
			int other_cpu;

			if (sd->level <= SD_LV_MC)
				locality = 0;
			else if (sd->level <= SD_LV_NODE)
				locality = 1;
			else
				continue;

			for_each_cpu_mask_nr(other_cpu, sd->span) {
				if (locality < rq->cpu_locality[other_cpu])
					rq->cpu_locality[other_cpu] = locality;
			}
		}
	}
}
#else
void __init sched_init_smp(void)
{
}
#endif /* CONFIG_SMP */

int in_sched_functions(unsigned long addr)
{
	return in_lock_functions(addr) ||
		(addr >= (unsigned long)__sched_text_start
		&& addr < (unsigned long)__sched_text_end);
}

void __init sched_init(void)
{
	int i;
	struct rq *rq;

	spin_lock_init(&grq.lock);
#ifdef CONFIG_SMP
	init_defrootdomain();
#endif
	for_each_possible_cpu(i) {
		rq = cpu_rq(i);
		rq->cpu = i;
		rq->user_pc = rq->nice_pc = rq->softirq_pc = rq->system_pc =
			      rq->iowait_pc = rq->idle_pc = 0;
#ifdef CONFIG_SMP
		rq->sd = NULL;
		rq->rd = NULL;
		rq->online = 0;
		rq_attach_root(rq, &def_root_domain);
#endif
		atomic_set(&rq->nr_iowait, 0);
	}

#ifdef CONFIG_SMP
	nr_cpu_ids = i;
	/*
	 * Set the base locality for cpu cache distance calculation to
	 * "distant" (3). Make sure the distance from a CPU to itself is 0.
	 */
	for_each_possible_cpu(i) {
		int j;

		rq = cpu_rq(i);
		rq->cpu_locality = alloc_bootmem(nr_cpu_ids * sizeof(unsigned long));
		for_each_possible_cpu(j) {
			if (i == j)
				rq->cpu_locality[j] = 0;
			else
				rq->cpu_locality[j] = 3;
		}
	}
#endif

	for (i = 0; i < PRIO_LIMIT; i++)
		INIT_LIST_HEAD(grq.queue + i);
	/* delimiter for bitsearch */
	__set_bit(PRIO_LIMIT, grq.prio_bitmap);

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&init_task.preempt_notifiers);
#endif

#ifdef CONFIG_RT_MUTEXES
	plist_head_init(&init_task.pi_waiters, &init_task.pi_lock);
#endif

	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	atomic_inc(&init_mm.mm_count);
	enter_lazy_tlb(&init_mm, current);

	/*
	 * Make us the idle thread. Technically, schedule() should not be
	 * called from this thread, however somewhere below it might be,
	 * but because we are the idle thread, we just pick up running again
	 * when this runqueue becomes "idle".
	 */
	init_idle(current, smp_processor_id());
}

#ifdef CONFIG_DEBUG_SPINLOCK_SLEEP
void __might_sleep(char *file, int line)
{
#ifdef in_atomic
	static unsigned long prev_jiffy;	/* ratelimiting */

	if ((in_atomic() || irqs_disabled()) &&
	    system_state == SYSTEM_RUNNING && !oops_in_progress) {
		if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
			return;
		prev_jiffy = jiffies;
		printk(KERN_ERR "BUG: sleeping function called from invalid"
				" context at %s:%d\n", file, line);
		printk("in_atomic():%d, irqs_disabled():%d\n",
			in_atomic(), irqs_disabled());
		debug_show_held_locks(current);
		if (irqs_disabled())
			print_irqtrace_events(current);
		dump_stack();
	}
#endif
}
EXPORT_SYMBOL(__might_sleep);
#endif

#ifdef CONFIG_MAGIC_SYSRQ
void normalize_rt_tasks(void)
{
	struct task_struct *g, *p;
	unsigned long flags;
	struct rq *rq;
	int queued;

	read_lock_irq(&tasklist_lock);

	do_each_thread(g, p) {
		if (!rt_task(p) && !iso_task(p))
			continue;

		spin_lock_irqsave(&p->pi_lock, flags);
		rq = __task_grq_lock(p);
		update_rq_clock(rq);

		queued = task_queued(p);
		if (queued)
			dequeue_task(p);
		__setscheduler(p, rq, SCHED_NORMAL, 0);
		if (queued) {
			enqueue_task(p);
			try_preempt(p, rq);
		}

		__task_grq_unlock();
		spin_unlock_irqrestore(&p->pi_lock, flags);
	} while_each_thread(g, p);

	read_unlock_irq(&tasklist_lock);
}
#endif /* CONFIG_MAGIC_SYSRQ */

#ifdef CONFIG_IA64
/*
 * These functions are only useful for the IA64 MCA handling.
 *
 * They can only be called when the whole system has been
 * stopped - every CPU needs to be quiescent, and no scheduling
 * activity can take place. Using them for anything else would
 * be a serious bug, and as a result, they aren't even visible
 * under any other configuration.
 */

/**
 * curr_task - return the current task for a given cpu.
 * @cpu: the processor in question.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 */
struct task_struct *curr_task(int cpu)
{
	return cpu_curr(cpu);
}

/**
 * set_curr_task - set the current task for a given cpu.
 * @cpu: the processor in question.
 * @p: the task pointer to set.
 *
 * Description: This function must only be used when non-maskable interrupts
 * are serviced on a separate stack.  It allows the architecture to switch the
 * notion of the current task on a cpu in a non-blocking manner.  This function
 * must be called with all CPU's synchronised, and interrupts disabled, the
 * and caller must save the original value of the current task (see
 * curr_task() above) and restore that value before reenabling interrupts and
 * re-starting the system.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 */
void set_curr_task(int cpu, struct task_struct *p)
{
	cpu_curr(cpu) = p;
}

#endif

/*
 * Use precise platform statistics if available:
 */
#ifdef CONFIG_VIRT_CPU_ACCOUNTING
cputime_t task_utime(struct task_struct *p)
{
	return p->utime;
}

cputime_t task_stime(struct task_struct *p)
{
	return p->stime;
}
#else
cputime_t task_utime(struct task_struct *p)
{
	clock_t utime = cputime_to_clock_t(p->utime),
		total = utime + cputime_to_clock_t(p->stime);
	u64 temp;

	temp = (u64)nsec_to_clock_t(p->sched_time);

	if (total) {
		temp *= utime;
		do_div(temp, total);
	}
	utime = (clock_t)temp;

	p->prev_utime = max(p->prev_utime, clock_t_to_cputime(utime));
	return p->prev_utime;
}

cputime_t task_stime(struct task_struct *p)
{
	clock_t stime;

	stime = nsec_to_clock_t(p->sched_time) -
			cputime_to_clock_t(task_utime(p));

	if (stime >= 0)
		p->prev_stime = max(p->prev_stime, clock_t_to_cputime(stime));

	return p->prev_stime;
}
#endif

inline cputime_t task_gtime(struct task_struct *p)
{
	return p->gtime;
}

void __cpuinit init_idle_bootup_task(struct task_struct *idle)
{}

#ifdef CONFIG_SCHED_DEBUG
void proc_sched_show_task(struct task_struct *p, struct seq_file *m)
{}

void proc_sched_set_task(struct task_struct *p)
{}
#endif
