#ifndef IRQ_CPU_H
#define IRQ_CPU_H

void ingenic_percpu_timerevent_init(unsigned int cpu_num);
void ingenic_percpu_timerevent_deinit(void);
#endif /* IRQ_CPU_H */
