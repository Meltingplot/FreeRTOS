/* Prints what the QEMU NVIC implements: priority bits (NVIC_IPR0, SHPR3), BASEPRI read-back and
 * BASEPRI_MAX behaviour, CPUID. No FreeRTOS. Part of Tools/mask-test (run.sh runs it first). */
#include <stdint.h>
#include "semihost.h"

#define NVIC_IPR0 ( *( volatile uint32_t * ) 0xE000E400 )
#define SCB_SHPR3 ( *( volatile uint32_t * ) 0xE000ED20 )
#define SCB_CPUID ( *( volatile uint32_t * ) 0xE000ED00 )

static unsigned bits_of( uint32_t byte )
{
    unsigned n = 0;
    while( byte & 0x80u )
    {
        n++;
        byte = ( byte << 1 ) & 0xffu;
    }
    return n;
}

int main( void )
{
    uint32_t ipr, shpr3;

    sh_puts( "PROBE cpuid=" );
    sh_puthex( SCB_CPUID );
    NVIC_IPR0 = 0xffffffffu;
    ipr = NVIC_IPR0;
    NVIC_IPR0 = 0;
    sh_puts( " ipr0_readback=" );
    sh_puthex( ipr );
    sh_puts( " nvic_prio_bits=" );
    sh_putdec( bits_of( ipr & 0xffu ) );
    shpr3 = SCB_SHPR3;
    SCB_SHPR3 = 0xffff0000u;
    sh_puts( " shpr3_readback=" );
    sh_puthex( SCB_SHPR3 );
    SCB_SHPR3 = shpr3;
#if !defined( __ARM_ARCH_6M__ )
    {
        uint32_t a, b, c, v = 0xff;
        __asm volatile ( "msr basepri, %0" :: "r" ( v ) : "memory" );
        __asm volatile ( "mrs %0, basepri" : "=r" ( a ) );
        v = 0x60;
        __asm volatile ( "msr basepri, %0" :: "r" ( v ) : "memory" );
        v = 0x20;
        __asm volatile ( "msr basepri_max, %0" :: "r" ( v ) : "memory" );
        __asm volatile ( "mrs %0, basepri" : "=r" ( b ) );
        v = 0x80;
        __asm volatile ( "msr basepri_max, %0" :: "r" ( v ) : "memory" );
        __asm volatile ( "mrs %0, basepri" : "=r" ( c ) );
        v = 0;
        __asm volatile ( "msr basepri, %0" :: "r" ( v ) : "memory" );
        sh_puts( " basepri_ff_readback=" );
        sh_puthex( a );
        sh_puts( " basepri_max(0x60,0x20)=" );
        sh_puthex( b );
        sh_puts( " basepri_max(0x20,0x80)=" );
        sh_puthex( c );
    }
#endif
    sh_puts( "\n" );
    sh_exit( 0 );
}
