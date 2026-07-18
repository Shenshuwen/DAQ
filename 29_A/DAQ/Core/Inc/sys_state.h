#ifndef __SYS_STATE_H
#define __SYS_STATE_H


#include <stdint.h>

typedef enum {
    SYS_ERROR_NONE = 0x00,
    SYS_ERROR_SensorTask = 0x01,
    SYS_ERROR_CommTask = 0x02
} SysErrorFlag;


extern volatile SysErrorFlag sys_error_flag ;
extern volatile uint32_t sys_sensor_heartbeat ;
extern volatile uint32_t sys_comm_heartbeat ;

#endif /* __SYS_STATE_H */

