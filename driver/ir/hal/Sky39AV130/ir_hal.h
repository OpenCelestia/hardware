#ifndef HARDWARE_IR_HAL_SKY39AV130_H
#define HARDWARE_IR_HAL_SKY39AV130_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int hal_ir_init(const char *cfg_path);
int hal_ir_set_ircut(int status);
void hal_ir_set_ircut_time(int time_ms);
int hal_ir_get_ircut_time(int *time_ms);

typedef struct {
    int working_level;
} hal_irled_param_t;

int hal_irled_init(const hal_irled_param_t *param);
int hal_irled_get_status(int *status);
int hal_irled_set_status(int status);

#ifdef __cplusplus
}
#endif

#endif
