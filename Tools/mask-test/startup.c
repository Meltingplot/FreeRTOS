/* Vector table and reset code for the QEMU mask test (mps2-an386, mps2-an500, microbit, mps2-an505). */
#include <stdint.h>
#include "semihost.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main( void );

void Reset_Handler( void );
void Default_Handler( void );
void HardFault_Handler( void );
void NMI_Handler( void ) __attribute__( ( weak, alias( "Default_Handler" ) ) );
void MemManage_Handler( void ) __attribute__( ( weak, alias( "HardFault_Handler" ) ) );
void BusFault_Handler( void ) __attribute__( ( weak, alias( "HardFault_Handler" ) ) );
void UsageFault_Handler( void ) __attribute__( ( weak, alias( "HardFault_Handler" ) ) );
void SecureFault_Handler( void ) __attribute__( ( weak, alias( "HardFault_Handler" ) ) );
void SVC_Handler( void ) __attribute__( ( weak, alias( "Default_Handler" ) ) );
void PendSV_Handler( void ) __attribute__( ( weak, alias( "Default_Handler" ) ) );
void SysTick_Handler( void ) __attribute__( ( weak, alias( "Default_Handler" ) ) );
void TestIRQ_Handler( void ) __attribute__( ( weak, alias( "Default_Handler" ) ) );

typedef void ( * vector_t )( void );

__attribute__( ( section( ".isr_vector" ), used ) ) const vector_t vectors[ 16 + 32 ] =
{
    ( vector_t ) &_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    SecureFault_Handler,
    0, 0, 0,
    SVC_Handler,
    0, 0,
    PendSV_Handler,
    SysTick_Handler,
    [ 16 ... 16 + 31 ] = Default_Handler,
};

void sh_puthex( uint32_t v )
{
    char buf[ 11 ];
    int i;
    buf[ 0 ] = '0';
    buf[ 1 ] = 'x';
    for( i = 0; i < 8; i++ )
    {
        uint32_t n = ( v >> ( 28 - 4 * i ) ) & 0xfu;
        buf[ 2 + i ] = ( char ) ( n < 10 ? '0' + n : 'a' + n - 10 );
    }
    buf[ 10 ] = 0;
    sh_puts( buf );
}

void sh_putdec( uint32_t v )
{
    char buf[ 11 ];
    int i = 10;
    buf[ i ] = 0;
    do
    {
        buf[ --i ] = ( char ) ( '0' + v % 10 );
        v /= 10;
    } while( v != 0 );
    sh_puts( &buf[ i ] );
}

void Reset_Handler( void )
{
    uint32_t * src = &_sidata, * dst = &_sdata;
    while( dst < &_edata )
    {
        *dst++ = *src++;
    }
    for( dst = &_sbss; dst < &_ebss; )
    {
        *dst++ = 0;
    }
#if defined( __ARM_FP )
    /* Full access to CP10/CP11 before any floating point instruction. */
    *( volatile uint32_t * ) 0xE000ED88 |= ( 0xFu << 20 );
    __asm volatile ( "dsb\n isb" ::: "memory" );
#endif
    ( void ) main();
    sh_puts( "MASKTEST main returned\n" );
    sh_exit( 4 );
}

static void fault_exit( const char * what )
{
    uint32_t ipsr;
    __asm volatile ( "mrs %0, ipsr" : "=r" ( ipsr ) );
    sh_puts( "MASKTEST RESULT FAIL: " );
    sh_puts( what );
    sh_puts( " ipsr=" );
    sh_puthex( ipsr );
#if !defined( __ARM_ARCH_6M__ )
    sh_puts( " cfsr=" );
    sh_puthex( *( volatile uint32_t * ) 0xE000ED28 );
    sh_puts( " hfsr=" );
    sh_puthex( *( volatile uint32_t * ) 0xE000ED2C );
#endif
    sh_puts( "\n" );
    sh_exit( 3 );
}

void HardFault_Handler( void )
{
    fault_exit( "fault" );
}

void Default_Handler( void )
{
    fault_exit( "unexpected exception" );
}
