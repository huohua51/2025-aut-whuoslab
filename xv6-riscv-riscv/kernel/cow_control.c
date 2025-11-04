// ============================================================================
// kernel/cow_control.c - COW控制位实现
// ============================================================================

#include "types.h"
#include "cow_control.h"
#include "printf.h"

// COW控制寄存器
uint32 cow_control_flags = 0;

/**
 * 设置COW控制位
 * @param flag 要设置的位标志
 */
void cow_set_flag(uint32 flag) {
    cow_control_flags |= flag;
    printf("COW control: flag 0x%x set, current flags: 0x%x\n", flag, cow_control_flags);
}

/**
 * 清除COW控制位
 * @param flag 要清除的位标志
 */
void cow_clear_flag(uint32 flag) {
    cow_control_flags &= ~flag;
    printf("COW control: flag 0x%x cleared, current flags: 0x%x\n", flag, cow_control_flags);
}

/**
 * 检查COW功能是否启用
 * @return 1表示启用，0表示禁用
 */
int cow_is_enabled(void) {
    return (cow_control_flags & COW_ENABLE_BIT) != 0;
}

/**
 * 检查COW调试功能是否启用
 * @return 1表示启用，0表示禁用
 */
int cow_is_debug_enabled(void) {
    return (cow_control_flags & COW_DEBUG_BIT) != 0;
}

/**
 * 检查COW统计功能是否启用
 * @return 1表示启用，0表示禁用
 */
int cow_is_stats_enabled(void) {
    return (cow_control_flags & COW_STATS_BIT) != 0;
}
