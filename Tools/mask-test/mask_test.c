/*
 * QEMU runtime test of the critical-section mask change of the Cortex-M ports
 * (vPortEnterCritical/vPortExitCritical restore the mask the outermost critical
 * section found, ulPortRaiseBASEPRI uses basepri_max, xPortTaskCanBlock() and the
 * blocking trace hooks in src/include/FreeRTOSConfig.h).
 *
 * The case to run comes from the semihosting command line (QEMU
 * -semihosting-config arg=<case>), see run.sh for the list and the expectations.
 * Output goes through semihosting; the exit status says how the case ended:
 *   0  PASS (all checks matched)
 *   1  FAIL (a check did not match)
 *   2  configASSERT reached although no assertion was armed
 *   3  fault or unexpected exception
 *   10 configASSERT reached while armed (run.sh checks file:line against the hook)
 *   11 the armed call returned without an assertion
 *
 * Build macros: PORT_CM4F, PORT_CM7, PORT_CM0 or PORT_CM33 (one of them);
 * CONTROL=1 for the unmodified kernel (old behaviour is expected, no hooks, no
 * xPortTaskCanBlock); SYSCLK_HZ.
 */
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "freertos_task_additions.h"
#include "semihost.h"

#ifndef CONTROL
    #define CONTROL 0
#endif

#if defined( __ARM_ARCH_6M__ )
    #define HAS_BASEPRI 0
#else
    #define HAS_BASEPRI 1
#endif

#if ( defined( PORT_CM4F ) + defined( PORT_CM7 ) + defined( PORT_CM0 ) + defined( PORT_CM33 ) ) != 1
    #error Define exactly one of PORT_CM4F, PORT_CM7, PORT_CM0, PORT_CM33
#endif

uint32_t SystemCoreClock = SYSCLK_HZ;

/* -------------------------------------------------------------------------- */
/* Register access */

static inline uint32_t get_basepri( void )
{
#if HAS_BASEPRI
    uint32_t v;
    __asm volatile ( "mrs %0, basepri" : "=r" ( v )::"memory" );
    return v;
#else
    return 0;
#endif
}

static inline void set_basepri( uint32_t v )
{
#if HAS_BASEPRI
    __asm volatile ( "msr basepri, %0\n isb" ::"r" ( v ) : "memory" );
#else
    ( void ) v;
#endif
}

static inline uint32_t get_primask( void )
{
    uint32_t v;
    __asm volatile ( "mrs %0, primask" : "=r" ( v )::"memory" );
    return v & 1u;
}

static inline void primask_set( void )
{
    __asm volatile ( "cpsid i" ::: "memory" );
}

static inline void primask_clear( void )
{
    __asm volatile ( "cpsie i" ::: "memory" );
}

/* -------------------------------------------------------------------------- */
/* Output and checks */

static uint32_t g_failures;
static volatile uint32_t g_armed;

static void put( const char * s )
{
    sh_puts( s );
}

static void check( const char * group, const char * id, uint32_t obs, uint32_t exp )
{
    put( "CHECK " );
    put( group );
    put( " " );
    put( id );
    put( " obs=" );
    sh_puthex( obs );
    put( " exp=" );
    sh_puthex( exp );
    put( obs == exp ? " OK\n" : " BAD\n" );

    if( obs != exp )
    {
        g_failures++;
    }
}

static void check_true( const char * group, const char * id, int cond )
{
    check( group, id, cond ? 1u : 0u, 1u );
}

static __attribute__( ( noreturn ) ) void finish( void )
{
    primask_set();

    if( g_failures == 0 )
    {
        put( "MASKTEST RESULT PASS\n" );
        sh_exit( 0 );
    }

    put( "MASKTEST RESULT FAIL: " );
    sh_putdec( g_failures );
    put( " check(s) did not match\n" );
    sh_exit( 1 );
}

/* -------------------------------------------------------------------------- */
/* Hooks the configuration requires */

void vAssertCalled( uint32_t ulLine, const char * pcFile )
{
    primask_set();
    put( "MASKTEST ASSERT " );
    put( pcFile );
    put( ":" );
    sh_putdec( ulLine );

    if( g_armed != 0 )
    {
        put( " (armed)\nMASKTEST RESULT ASSERTED\n" );
        sh_exit( 10 );
    }

    put( " (not armed)\nMASKTEST RESULT FAIL: unexpected assertion\n" );
    sh_exit( 2 );
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;
    primask_set();
    put( "MASKTEST RESULT FAIL: stack overflow in " );
    put( pcTaskName );
    put( "\n" );
    sh_exit( 3 );
}

void vApplicationMallocFailedHook( void )
{
    put( "MASKTEST RESULT FAIL: malloc failed\n" );
    sh_exit( 3 );
}

static volatile uint64_t g_runTimeTicks;
static volatile uint32_t g_tickHookCount;

void vApplicationTickHook( void )
{
    g_tickHookCount++;
    g_runTimeTicks++;
}

configRUN_TIME_COUNTER_TYPE TaskGetRunTimeTicks( void )
{
    return g_runTimeTicks;
}

configRUN_TIME_COUNTER_TYPE TaskResetRunTimeCounter( void )
{
    const configRUN_TIME_COUNTER_TYPE r = g_runTimeTicks;
    g_runTimeTicks = 0;
    return r;
}

static StaticTask_t xIdleTCB;
static StackType_t xIdleStack[ configMINIMAL_STACK_SIZE ];

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * puxIdleTaskStackSize )
{
    *ppxIdleTaskTCBBuffer = &xIdleTCB;
    *ppxIdleTaskStackBuffer = xIdleStack;
    *puxIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* -------------------------------------------------------------------------- */
/* Tasks */

#define PRIO_SPIN      1
#define PRIO_HELPER    2
#define PRIO_MAIN      3
#define PRIO_DELETER   4

#define HELPER_DELAY   3

enum
{
    CMD_NONE,
    CMD_GIVE_SEM,
    CMD_NOTIFY_GIVE,
    CMD_NOTIFY_VALUE,
    CMD_RESUME_MAIN
};

#define NOTIFY_VALUE    0x1234abcdu

static StaticTask_t xMainTCB, xHelperTCB, xSpinTCB, xDeleterTCB;
static StackType_t xMainStack[ 320 ], xHelperStack[ 192 ], xSpinStack[ configMINIMAL_STACK_SIZE ], xDeleterStack[ configMINIMAL_STACK_SIZE ];
static TaskHandle_t xMain, xHelper, xSpin;
static StaticSemaphore_t xSemBuffer, xEmptySemBuffer;
static SemaphoreHandle_t xSem, xEmptySem;

static volatile uint32_t g_spin, g_helperRuns, g_helperCmd, g_deleterRan;
static char g_case[ 64 ];

static void spin_task( void * p )
{
    ( void ) p;

    for( ; ; )
    {
        g_spin++;
    }
}

static void helper_task( void * p )
{
    ( void ) p;

    for( ; ; )
    {
        ( void ) ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
        g_helperRuns++;
        vTaskDelay( HELPER_DELAY );

        switch( g_helperCmd )
        {
            case CMD_GIVE_SEM:
                ( void ) xSemaphoreGive( xSem );
                break;

            case CMD_NOTIFY_GIVE:
                ( void ) xTaskNotifyGive( xMain );
                break;

            case CMD_NOTIFY_VALUE:
                ( void ) xTaskNotify( xMain, NOTIFY_VALUE, eSetValueWithOverwrite );
                break;

            case CMD_RESUME_MAIN:
                vTaskResume( xMain );
                break;

            default:
                break;
        }
    }
}

#if !CONTROL
#define PRIO_PARKED    5

static StaticTask_t xParkedTCB;
static StackType_t xParkedStack[ configMINIMAL_STACK_SIZE ];
static TaskHandle_t xParked;
static volatile uint32_t g_parkedRan, g_parkedWasCurrent, g_mainCurrentAfter;

static void parked_task( void * p )
{
    ( void ) p;

    for( ; ; )
    {
        g_parkedRan++;
        vTaskDelay( 1 );
    }
}
#endif

static void deleter_task( void * p )
{
    ( void ) p;
    g_deleterRan = 1;
    vTaskDelete( NULL );
}

static void helper_command( uint32_t cmd )
{
    g_helperCmd = cmd;
    ( void ) xTaskNotifyGive( xHelper );
}

/* -------------------------------------------------------------------------- */
/* Test 1: the mask a task holds survives the critical sections */

typedef enum
{
    MASK_NONE,
    MASK_BASEPRI,
    MASK_PRIMASK
} MaskKind;

#define SYSCALL_LEVEL    ( ( uint32_t ) configMAX_SYSCALL_INTERRUPT_PRIORITY )
#define PRIO_LEVEL( n )    ( ( uint32_t ) ( n ) << ( 8 - configPRIO_BITS ) )

static void mask_apply( MaskKind k, uint32_t v )
{
    if( k == MASK_BASEPRI )
    {
        set_basepri( v );
    }
    else if( k == MASK_PRIMASK )
    {
        primask_set();
    }
}

static void mask_release( void )
{
    set_basepri( 0 );
    primask_clear();
}

#if !CONTROL
/* BASEPRI after msr basepri_max of v on top of cur. */
static uint32_t basepri_max_of( uint32_t cur, uint32_t v )
{
    return ( v != 0 && ( cur == 0 || v < cur ) ) ? v : cur;
}
#endif

/* Expected BASEPRI inside a critical section entered under (k, v). */
static uint32_t exp_in_basepri( MaskKind k, uint32_t v )
{
#if CONTROL
    ( void ) k;
    ( void ) v;
    return SYSCALL_LEVEL;   /* msr basepri overwrites */
#else
    return basepri_max_of( k == MASK_BASEPRI ? v : 0, SYSCALL_LEVEL );
#endif
}

/* Expected PRIMASK inside a critical section entered under (k, v). */
static uint32_t exp_in_primask( MaskKind k )
{
#if defined( PORT_CM0 )
    ( void ) k;
    return 1;               /* cpsid i */
#elif CONTROL && defined( PORT_CM7 )
    ( void ) k;
    return 0;               /* errata workaround: cpsid i ... cpsie i */
#else
    return k == MASK_PRIMASK ? 1 : 0;
#endif
}

/* Expected BASEPRI after the outermost exit. */
static uint32_t exp_out_basepri( MaskKind k, uint32_t v )
{
#if CONTROL
    ( void ) k;
    ( void ) v;
    return 0;               /* vPortSetBASEPRI( 0 ) */
#else
    return k == MASK_BASEPRI ? v : 0;
#endif
}

/* Expected PRIMASK after the outermost exit. */
static uint32_t exp_out_primask( MaskKind k )
{
#if CONTROL && ( defined( PORT_CM7 ) || defined( PORT_CM0 ) )
    ( void ) k;
    return 0;               /* CM7 r0p1: cpsie i on entry; CM0: cpsie i on exit */
#else
    return k == MASK_PRIMASK ? 1 : 0;
#endif
}

/* Expected PRIMASK while portSET_INTERRUPT_MASK_FROM_ISR is in force, and after the clear. */
static uint32_t exp_isr_in_primask( MaskKind k )
{
#if defined( PORT_CM0 )
    ( void ) k;
    return 1;               /* ulSetInterruptMask: cpsid i */
#elif CONTROL && defined( PORT_CM7 )
    ( void ) k;
    return 0;
#else
    return k == MASK_PRIMASK ? 1 : 0;
#endif
}

static uint32_t exp_isr_out_primask( MaskKind k )
{
#if CONTROL && defined( PORT_CM7 )
    ( void ) k;
    return 0;
#else
    return k == MASK_PRIMASK ? 1 : 0;   /* CM0: vClearInterruptMask restores PRIMASK */
#endif
}

static void check_regs( const char * group, const char * where, uint32_t basepri, uint32_t primask,
                        uint32_t expBasepri, uint32_t expPrimask )
{
    char id[ 40 ];
    size_t n = strlen( where );

    memcpy( id, where, n );
#if HAS_BASEPRI
    memcpy( id + n, ".BASEPRI", 9 );
    check( group, id, basepri, expBasepri );
#else
    ( void ) basepri;
    ( void ) expBasepri;
#endif
    memcpy( id + n, ".PRIMASK", 9 );
    check( group, id, primask, expPrimask );
}

static void mask_sequences( const char * group, MaskKind k, uint32_t v )
{
    uint32_t b1, p1, b2, p2, b3, p3, bo, po, ret;

    put( "GROUP " );
    put( group );
    put( k == MASK_BASEPRI ? " mask=BASEPRI value=" : k == MASK_PRIMASK ? " mask=PRIMASK value=" : " mask=none value=" );
    sh_puthex( v );
    put( "\n" );

    /* One critical section. */
    mask_apply( k, v );
    taskENTER_CRITICAL();
    b1 = get_basepri();
    p1 = get_primask();
    taskEXIT_CRITICAL();
    bo = get_basepri();
    po = get_primask();
    mask_release();
    check_regs( group, "crit.in", b1, p1, exp_in_basepri( k, v ), exp_in_primask( k ) );
    check_regs( group, "crit.after", bo, po, exp_out_basepri( k, v ), exp_out_primask( k ) );

    /* Nested critical sections. */
    mask_apply( k, v );
    taskENTER_CRITICAL();
    b1 = get_basepri();
    p1 = get_primask();
    taskENTER_CRITICAL();
    b2 = get_basepri();
    p2 = get_primask();
    taskEXIT_CRITICAL();
    b3 = get_basepri();
    p3 = get_primask();
    taskEXIT_CRITICAL();
    bo = get_basepri();
    po = get_primask();
    mask_release();
    check_regs( group, "nest.in1", b1, p1, exp_in_basepri( k, v ), exp_in_primask( k ) );
    check_regs( group, "nest.in2", b2, p2, exp_in_basepri( k, v ), exp_in_primask( k ) );
    check_regs( group, "nest.in1again", b3, p3, exp_in_basepri( k, v ), exp_in_primask( k ) );
    check_regs( group, "nest.after", bo, po, exp_out_basepri( k, v ), exp_out_primask( k ) );

    /* vTaskSuspendAll, a critical section, xTaskResumeAll (which has a critical section of its own). */
    mask_apply( k, v );
    vTaskSuspendAll();
    taskENTER_CRITICAL();
    b1 = get_basepri();
    p1 = get_primask();
    taskEXIT_CRITICAL();
    b2 = get_basepri();
    p2 = get_primask();
    ( void ) xTaskResumeAll();
    bo = get_basepri();
    po = get_primask();
    mask_release();
    check_regs( group, "suspall.crit.in", b1, p1, exp_in_basepri( k, v ), exp_in_primask( k ) );
    check_regs( group, "suspall.crit.after", b2, p2, exp_out_basepri( k, v ), exp_out_primask( k ) );
    check_regs( group, "suspall.resumed", bo, po, exp_out_basepri( k, v ), exp_out_primask( k ) );

    /* vTaskSuspendAll/xTaskResumeAll inside a critical section. */
    mask_apply( k, v );
    taskENTER_CRITICAL();
    vTaskSuspendAll();
    ( void ) xTaskResumeAll();
    b1 = get_basepri();
    p1 = get_primask();
    taskEXIT_CRITICAL();
    bo = get_basepri();
    po = get_primask();
    mask_release();
    check_regs( group, "crit.suspall.in", b1, p1, exp_in_basepri( k, v ), exp_in_primask( k ) );
    check_regs( group, "crit.suspall.after", bo, po, exp_out_basepri( k, v ), exp_out_primask( k ) );

    /* portSET_INTERRUPT_MASK_FROM_ISR / portCLEAR_INTERRUPT_MASK_FROM_ISR. */
    mask_apply( k, v );
    ret = portSET_INTERRUPT_MASK_FROM_ISR();
    b1 = get_basepri();
    p1 = get_primask();
    portCLEAR_INTERRUPT_MASK_FROM_ISR( ret );
    bo = get_basepri();
    po = get_primask();
    mask_release();
#if HAS_BASEPRI
    check( group, "isrmask.ret", ret, k == MASK_BASEPRI ? v : 0 );
#else
    check( group, "isrmask.ret", ret, k == MASK_PRIMASK ? 1 : 0 );
#endif
    check_regs( group, "isrmask.in", b1, p1, exp_in_basepri( k, v ), exp_isr_in_primask( k ) );
    check_regs( group, "isrmask.after", bo, po, k == MASK_BASEPRI ? v : 0, exp_isr_out_primask( k ) );
}

#if !CONTROL
/* xPortTaskCanBlock( uxKernelNesting ) directly. */
static void canblock_checks( void )
{
    BaseType_t r0, r1, r2;

    put( "GROUP canblock\n" );

    r0 = xPortTaskCanBlock( 0 );
    r1 = xPortTaskCanBlock( 1 );
    check( "canblock", "none.n0", ( uint32_t ) r0, pdTRUE );
    check( "canblock", "none.n1", ( uint32_t ) r1, pdFALSE );

    taskENTER_CRITICAL();
    r0 = xPortTaskCanBlock( 0 );
    r1 = xPortTaskCanBlock( 1 );
    taskENTER_CRITICAL();
    r2 = xPortTaskCanBlock( 1 );
    taskEXIT_CRITICAL();
    taskEXIT_CRITICAL();
    check( "canblock", "crit.n0", ( uint32_t ) r0, pdFALSE );
    check( "canblock", "crit.n1", ( uint32_t ) r1, pdTRUE );
    check( "canblock", "crit2.n1", ( uint32_t ) r2, pdFALSE );

    #if HAS_BASEPRI
    {
        const uint32_t values[ 3 ] = { SYSCALL_LEVEL, PRIO_LEVEL( 1 ), PRIO_LEVEL( 4 ) };
        const char * const ids0[ 3 ] = { "basepri_sys.n0", "basepri_strict.n0", "basepri_weak.n0" };
        const char * const ids1[ 3 ] = { "basepri_sys.crit.n1", "basepri_strict.crit.n1", "basepri_weak.crit.n1" };
        unsigned i;

        for( i = 0; i < 3; i++ )
        {
            set_basepri( values[ i ] );
            r0 = xPortTaskCanBlock( 0 );
            taskENTER_CRITICAL();
            r1 = xPortTaskCanBlock( 1 );
            taskEXIT_CRITICAL();
            set_basepri( 0 );
            check( "canblock", ids0[ i ], ( uint32_t ) r0, pdFALSE );
            check( "canblock", ids1[ i ], ( uint32_t ) r1, pdFALSE );
        }
    }
    #endif /* if HAS_BASEPRI */

    primask_set();
    r0 = xPortTaskCanBlock( 0 );
    taskENTER_CRITICAL();
    r1 = xPortTaskCanBlock( 1 );
    taskEXIT_CRITICAL();
    primask_clear();
    check( "canblock", "primask.n0", ( uint32_t ) r0, pdFALSE );
    check( "canblock", "primask.crit.n1", ( uint32_t ) r1, pdFALSE );

    r0 = xPortTaskCanBlock( 0 );
    check( "canblock", "none.again.n0", ( uint32_t ) r0, pdTRUE );
}
#endif /* if !CONTROL */

/* The task still blocks and the other tasks still run. */
static void check_switches( const char * group )
{
    const uint32_t spin = g_spin;
    const TickType_t t0 = xTaskGetTickCount();

    vTaskDelay( 2 );
    check_true( group, "delay.spinner_ran", g_spin != spin );
    check_true( group, "delay.ticks>=2", ( xTaskGetTickCount() - t0 ) >= 2 );
    check( group, "delay.final.BASEPRI", get_basepri(), 0 );
    check( group, "delay.final.PRIMASK", get_primask(), 0 );
}

/* taskDISABLE_INTERRUPTS() under PRIMASK. The change leaves portDISABLE_INTERRUPTS alone; on ARM_CM7/r0p1
 * it is vPortRaiseBASEPRI with the errata workaround cpsid i ... cpsie i, which clears PRIMASK. */
static void disable_interrupts_check( void )
{
    uint32_t b, p;

    put( "GROUP info.taskDISABLE_INTERRUPTS mask=PRIMASK value=0x00000001\n" );
    primask_set();
    taskDISABLE_INTERRUPTS();
    b = get_basepri();
    p = get_primask();
    set_basepri( 0 );
    primask_clear();
#if defined( PORT_CM7 )
    check_regs( "info.taskDISABLE_INTERRUPTS", "in", b, p, SYSCALL_LEVEL, 0 );
#else
    check_regs( "info.taskDISABLE_INTERRUPTS", "in", b, p, HAS_BASEPRI ? SYSCALL_LEVEL : 0, 1 );
#endif
}

static void case_mask( void )
{
    mask_sequences( "1c.none", MASK_NONE, 0 );
#if HAS_BASEPRI
    mask_sequences( "1a.basepri_syscall", MASK_BASEPRI, SYSCALL_LEVEL );
    mask_sequences( "1a.basepri_stricter", MASK_BASEPRI, PRIO_LEVEL( 1 ) );
    mask_sequences( "1a.basepri_weaker", MASK_BASEPRI, PRIO_LEVEL( 4 ) );
#endif
    mask_sequences( "1b.primask", MASK_PRIMASK, 1 );
    disable_interrupts_check();
#if !CONTROL
    canblock_checks();
#endif
    check_switches( "1.after" );
}

/* -------------------------------------------------------------------------- */
/* Test 2: blocking without a mask */

static void case_block( void )
{
    TickType_t t0, xLast;
    uint32_t spin, runs, v;
    BaseType_t r;

    put( "GROUP 2.block\n" );

    spin = g_spin;
    t0 = xTaskGetTickCount();
    vTaskDelay( 5 );
    check_true( "2.block", "vTaskDelay.ticks>=5", ( xTaskGetTickCount() - t0 ) >= 5 );
    check_true( "2.block", "vTaskDelay.spinner_ran", g_spin != spin );

    spin = g_spin;
    xLast = xTaskGetTickCount();
    r = xTaskDelayUntil( &xLast, 4 );
    check( "2.block", "xTaskDelayUntil.ret", ( uint32_t ) r, pdTRUE );
    check_true( "2.block", "xTaskDelayUntil.spinner_ran", g_spin != spin );

    runs = g_helperRuns;
    helper_command( CMD_GIVE_SEM );
    t0 = xTaskGetTickCount();
    r = xSemaphoreTake( xSem, 100 );
    check( "2.block", "xSemaphoreTake(given).ret", ( uint32_t ) r, pdTRUE );
    check_true( "2.block", "xSemaphoreTake(given).helper_ran", g_helperRuns == runs + 1 );
    check_true( "2.block", "xSemaphoreTake(given).ticks>=3", ( xTaskGetTickCount() - t0 ) >= HELPER_DELAY );

    t0 = xTaskGetTickCount();
    r = xSemaphoreTake( xEmptySem, 3 );
    check( "2.block", "xSemaphoreTake(timeout).ret", ( uint32_t ) r, pdFALSE );
    check_true( "2.block", "xSemaphoreTake(timeout).ticks>=3", ( xTaskGetTickCount() - t0 ) >= 3 );

    runs = g_helperRuns;
    helper_command( CMD_NOTIFY_GIVE );
    v = ulTaskNotifyTake( pdTRUE, 100 );
    check( "2.block", "ulTaskNotifyTake.ret", v, 1 );
    check_true( "2.block", "ulTaskNotifyTake.helper_ran", g_helperRuns == runs + 1 );

    runs = g_helperRuns;
    helper_command( CMD_NOTIFY_VALUE );
    v = 0;
    r = xTaskNotifyWait( 0, 0xffffffffu, &v, 100 );
    check( "2.block", "xTaskNotifyWait.ret", ( uint32_t ) r, pdTRUE );
    check( "2.block", "xTaskNotifyWait.value", v, NOTIFY_VALUE );
    check_true( "2.block", "xTaskNotifyWait.helper_ran", g_helperRuns == runs + 1 );

    runs = g_helperRuns;
    helper_command( CMD_RESUME_MAIN );
    vTaskSuspend( NULL );
    check_true( "2.block", "vTaskSuspend(self).helper_resumed", g_helperRuns == runs + 1 );

    ( void ) xTaskCreateStatic( deleter_task, "deleter", configMINIMAL_STACK_SIZE, NULL, PRIO_DELETER, xDeleterStack, &xDeleterTCB );
    check_true( "2.block", "vTaskDelete(self).ran", g_deleterRan == 1 );

    check( "2.block", "final.BASEPRI", get_basepri(), 0 );
    check( "2.block", "final.PRIMASK", get_primask(), 0 );
}

#if !CONTROL

/* -------------------------------------------------------------------------- */
/* Test 3: blocking under a mask asserts (one case per run) */

/* The application mask of the "-mask" cases: BASEPRI at the syscall level, PRIMASK on ARMv6-M. */
static void app_mask_on( void )
{
#if HAS_BASEPRI
    put( "MASK BASEPRI=" );
    sh_puthex( SYSCALL_LEVEL );
    put( "\n" );
    set_basepri( SYSCALL_LEVEL );
#else
    put( "MASK PRIMASK=1\n" );
    primask_set();
#endif
}

static void app_primask_on( void )
{
    put( "MASK PRIMASK=1\n" );
    primask_set();
}

static void arm( const char * call )
{
    put( "ARMED " );
    put( call );
    put( "\n" );
    g_armed = 1;
}

static __attribute__( ( noreturn ) ) void armed_returned( void )
{
    g_armed = 0;
    primask_set();
    put( "MASKTEST RESULT NO-ASSERT: the armed call returned\n" );
    sh_exit( 11 );
}

/* -------------------------------------------------------------------------- */
/* Test 3: cases that must not assert */

typedef struct
{
    const char * name;
    MaskKind kind;
    uint32_t value;
} MaskVariant;

static const MaskVariant xVariants[] =
{
#if HAS_BASEPRI
    { "basepri_syscall",  MASK_BASEPRI, SYSCALL_LEVEL   },
    { "basepri_stricter", MASK_BASEPRI, PRIO_LEVEL( 1 ) },
#endif
    { "primask",          MASK_PRIMASK, 1               },
};

#define NUM_VARIANTS    ( sizeof( xVariants ) / sizeof( xVariants[ 0 ] ) )

static uint32_t task_state( TaskHandle_t t )
{
    const void * res;
    BaseType_t idx = 0;

    return ( uint32_t ) eTaskGetExtendedState( t, &res, &idx );
}

static void case_n_semtake0( void )
{
    unsigned i;

    for( i = 0; i < NUM_VARIANTS; i++ )
    {
        BaseType_t r;
        mask_apply( xVariants[ i ].kind, xVariants[ i ].value );
        r = xSemaphoreTake( xEmptySem, 0 );
        mask_release();
        check( xVariants[ i ].name, "xSemaphoreTake(empty,0).ret", ( uint32_t ) r, pdFALSE );
    }
}

static void case_n_semgive( void )
{
    unsigned i;

    for( i = 0; i < NUM_VARIANTS; i++ )
    {
        BaseType_t r, r2;
        mask_apply( xVariants[ i ].kind, xVariants[ i ].value );
        r = xSemaphoreGive( xEmptySem );
        mask_release();
        r2 = xSemaphoreTake( xEmptySem, 0 );
        check( xVariants[ i ].name, "xSemaphoreGive.ret", ( uint32_t ) r, pdTRUE );
        check( xVariants[ i ].name, "xSemaphoreTake(given,0).ret", ( uint32_t ) r2, pdTRUE );
    }
}

static void case_n_suspendother( void )
{
    unsigned i;

    for( i = 0; i < NUM_VARIANTS; i++ )
    {
        uint32_t s1, s2;
        mask_apply( xVariants[ i ].kind, xVariants[ i ].value );
        vTaskSuspend( xHelper );
        mask_release();
        s1 = task_state( xHelper );
        vTaskResume( xHelper );
        s2 = task_state( xHelper );
        check( xVariants[ i ].name, "vTaskSuspend(helper).state", s1, esSuspended );
        check( xVariants[ i ].name, "vTaskResume(helper).state", s2, esReady );
    }
}

static void case_n_suspendother_critical( void )
{
    uint32_t s1, s2;

    taskENTER_CRITICAL();
    vTaskSuspend( xHelper );
    taskEXIT_CRITICAL();
    s1 = task_state( xHelper );
    vTaskResume( xHelper );
    s2 = task_state( xHelper );
    check( "critical", "vTaskSuspend(helper).state", s1, esSuspended );
    check( "critical", "vTaskResume(helper).state", s2, esReady );
}

static void case_n_block_after_restore( void )
{
    unsigned i;

    for( i = 0; i < NUM_VARIANTS; i++ )
    {
        const MaskVariant * const m = &xVariants[ i ];
        uint32_t b, p, spin, runs;
        TickType_t t0;
        BaseType_t r;

        mask_apply( m->kind, m->value );
        taskENTER_CRITICAL();
        taskEXIT_CRITICAL();
        b = get_basepri();
        p = get_primask();
        mask_release();
        check( m->name, "masked.BASEPRI", b, m->kind == MASK_BASEPRI ? m->value : 0 );
        check( m->name, "masked.PRIMASK", p, m->kind == MASK_PRIMASK ? 1 : 0 );

        spin = g_spin;
        t0 = xTaskGetTickCount();
        vTaskDelay( 2 );
        check_true( m->name, "vTaskDelay.spinner_ran", g_spin != spin );
        check_true( m->name, "vTaskDelay.ticks>=2", ( xTaskGetTickCount() - t0 ) >= 2 );

        runs = g_helperRuns;
        helper_command( CMD_GIVE_SEM );
        r = xSemaphoreTake( xSem, 100 );
        check( m->name, "xSemaphoreTake(given).ret", ( uint32_t ) r, pdTRUE );
        check_true( m->name, "xSemaphoreTake(given).helper_ran", g_helperRuns == runs + 1 );
    }
}

#endif /* if !CONTROL */

/* -------------------------------------------------------------------------- */

static void main_task( void * p )
{
    ( void ) p;

    put( "CASE " );
    put( g_case );
    put( "\n" );

    if( strcmp( g_case, "mask" ) == 0 )
    {
        case_mask();
    }
    else if( strcmp( g_case, "block" ) == 0 )
    {
        case_block();
    }
#if !CONTROL
    else if( strcmp( g_case, "A-delay-mask" ) == 0 )
    {
        app_mask_on();
        arm( "vTaskDelay(1)" );
        vTaskDelay( 1 );
        armed_returned();
    }
    else if( strcmp( g_case, "A-semtake-primask" ) == 0 )
    {
        app_primask_on();
        arm( "xSemaphoreTake(empty, 10)" );
        ( void ) xSemaphoreTake( xEmptySem, 10 );
        armed_returned();
    }
    else if( strcmp( g_case, "A-notifytake-mask" ) == 0 )
    {
        app_mask_on();
        arm( "ulTaskNotifyTake(pdTRUE, 10)" );
        ( void ) ulTaskNotifyTake( pdTRUE, 10 );
        armed_returned();
    }
    else if( strcmp( g_case, "A-suspendself-mask" ) == 0 )
    {
        app_mask_on();
        arm( "vTaskSuspend(NULL)" );
        vTaskSuspend( NULL );
        armed_returned();
    }
    else if( strcmp( g_case, "A-delay-critical" ) == 0 )
    {
        put( "MASK none, taskENTER_CRITICAL\n" );
        taskENTER_CRITICAL();
        arm( "vTaskDelay(1)" );
        vTaskDelay( 1 );
        armed_returned();
    }
    else if( strcmp( g_case, "A-notifywait-mask" ) == 0 )
    {
        uint32_t v;
        app_mask_on();
        arm( "xTaskNotifyWait(0, 0, &v, 10)" );
        ( void ) xTaskNotifyWait( 0, 0, &v, 10 );
        armed_returned();
    }
    else if( strcmp( g_case, "A-delayuntil-mask" ) == 0 )
    {
        TickType_t xLast = xTaskGetTickCount();
        app_mask_on();
        arm( "xTaskDelayUntil(&last, 5)" );
        ( void ) xTaskDelayUntil( &xLast, 5 );
        armed_returned();
    }
    else if( strcmp( g_case, "A-deleteself-mask" ) == 0 )
    {
        app_mask_on();
        arm( "vTaskDelete(NULL)" );
        vTaskDelete( NULL );
        armed_returned();
    }
    else if( strcmp( g_case, "N-semtake0-mask" ) == 0 )
    {
        case_n_semtake0();
    }
    else if( strcmp( g_case, "N-semgive-mask" ) == 0 )
    {
        case_n_semgive();
    }
    else if( strcmp( g_case, "N-suspendother-mask" ) == 0 )
    {
        case_n_suspendother();
    }
    else if( strcmp( g_case, "N-suspendother-critical" ) == 0 )
    {
        case_n_suspendother_critical();
    }
    else if( strcmp( g_case, "N-block-after-restore" ) == 0 )
    {
        case_n_block_after_restore();
    }
    else if( strcmp( g_case, "N-suspend-before-start" ) == 0 )
    {
        check( "before-start", "parked.was_pxCurrentTCB", g_parkedWasCurrent, 1 );
        check( "before-start", "main.pxCurrentTCB_after_suspend", g_mainCurrentAfter, 1 );
        check( "before-start", "parked.state", task_state( xParked ), esSuspended );
        check( "before-start", "parked.ran", g_parkedRan, 0 );
    }
#endif /* if !CONTROL */
    else
    {
        put( "MASKTEST RESULT FAIL: unknown case\n" );
        sh_exit( 1 );
    }

    /* The system still schedules after the case. */
    check_switches( "end" );
    finish();
}

/* Number of priority bits the NVIC implements (IPR of IRQ 0, word access for ARMv6-M). */
static unsigned nvic_prio_bits( void )
{
    volatile uint32_t * const ipr0 = ( volatile uint32_t * ) 0xE000E400;
    const uint32_t saved = *ipr0;
    uint32_t b;
    unsigned n = 0;

    *ipr0 = 0xffffffffu;
    b = *ipr0 & 0xffu;
    *ipr0 = saved;

    while( b & 0x80u )
    {
        n++;
        b = ( b << 1 ) & 0xffu;
    }

    return n;
}

int main( void )
{
    unsigned hwBits;

    if( sh_cmdline( g_case, sizeof( g_case ) ) != 0 )
    {
        g_case[ 0 ] = 0;
    }

    hwBits = nvic_prio_bits();
    put( "MASKTEST " );
    put( CONTROL ? "control" : "modified" );
    put( " port=" );
    put( PORT_NAME );
    put( " cpuid=" );
    sh_puthex( *( volatile uint32_t * ) 0xE000ED00 );
    put( " nvic_prio_bits=" );
    sh_putdec( hwBits );
    put( " configPRIO_BITS=" );
    sh_putdec( configPRIO_BITS );
#if HAS_BASEPRI
    put( " syscall=" );
    sh_puthex( SYSCALL_LEVEL );
    put( " stricter=" );
    sh_puthex( PRIO_LEVEL( 1 ) );
    put( " weaker=" );
    sh_puthex( PRIO_LEVEL( 4 ) );
#endif
    put( "\n" );

    /* The configured priorities must use implemented bits only. */
    if( hwBits < configPRIO_BITS )
    {
        put( "MASKTEST RESULT FAIL: configPRIO_BITS exceeds the NVIC\n" );
        sh_exit( 1 );
    }

    xSem = xSemaphoreCreateBinaryStatic( &xSemBuffer );
    xEmptySem = xSemaphoreCreateBinaryStatic( &xEmptySemBuffer );
    xMain = xTaskCreateStatic( main_task, "main", sizeof( xMainStack ) / sizeof( xMainStack[ 0 ] ), NULL, PRIO_MAIN, xMainStack, &xMainTCB );
    xHelper = xTaskCreateStatic( helper_task, "helper", sizeof( xHelperStack ) / sizeof( xHelperStack[ 0 ] ), NULL, PRIO_HELPER, xHelperStack, &xHelperTCB );
    xSpin = xTaskCreateStatic( spin_task, "spin", configMINIMAL_STACK_SIZE, NULL, PRIO_SPIN, xSpinStack, &xSpinTCB );
    ( void ) xSpin;

#if !CONTROL
    if( strcmp( g_case, "N-suspend-before-start" ) == 0 )
    {
        /* The highest priority task created so far is pxCurrentTCB until the scheduler starts, so this
         * reaches traceTASK_SUSPEND with pxTaskToSuspend == pxCurrentTCB and xSchedulerRunning == pdFALSE. */
        xParked = xTaskCreateStatic( parked_task, "parked", configMINIMAL_STACK_SIZE, NULL, PRIO_PARKED, xParkedStack, &xParkedTCB );
        g_parkedWasCurrent = ( xTaskGetCurrentTaskHandle() == xParked ) ? 1 : 0;
        vTaskSuspend( xParked );
        g_mainCurrentAfter = ( xTaskGetCurrentTaskHandle() == xMain ) ? 1 : 0;
    }
#endif

    vTaskStartScheduler();
    put( "MASKTEST RESULT FAIL: scheduler returned\n" );
    sh_exit( 3 );
}
