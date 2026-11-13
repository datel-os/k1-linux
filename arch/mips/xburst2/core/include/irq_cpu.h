#ifndef IRQ_CPU_H
#define IRQ_CPU_H

void ingenic_irq_migration(int lock);
void ingenic_irq_cpumask_idle(int idle);
int ingenic_percpu_irq_init(int cpu_num);
void ingenic_percpu_irq_deinit(void);
void ingenic_percpu_timerevent_init(unsigned int cpu_num);
void ingenic_percpu_timerevent_deinit(void);
void nebulaos_core_ost_dump(void);
#endif /* IRQ_CPU_H */
