/* Minimal Arm semihosting (BKPT 0xAB on M-profile) for the QEMU mask test. */
#ifndef MASK_TEST_SEMIHOST_H
#define MASK_TEST_SEMIHOST_H

#include <stdint.h>

#define SH_SYS_WRITE0           0x04u
#define SH_SYS_GET_CMDLINE      0x15u
#define SH_SYS_EXIT_EXTENDED    0x20u
#define SH_ADP_APPLICATION_EXIT 0x20026u

static inline uint32_t sh_call( uint32_t op, const void * arg )
{
    register uint32_t r0 __asm__( "r0" ) = op;
    register const void * r1 __asm__( "r1" ) = arg;
    __asm__ volatile ( "bkpt 0xab" : "+r" ( r0 ) : "r" ( r1 ) : "memory" );
    return r0;
}

static inline void sh_puts( const char * s )
{
    ( void ) sh_call( SH_SYS_WRITE0, s );
}

/* QEMU passes the subcode of SYS_EXIT_EXTENDED on as its exit status. */
static inline __attribute__( ( noreturn ) ) void sh_exit( uint32_t code )
{
    const uint32_t block[ 2 ] = { SH_ADP_APPLICATION_EXIT, code };
    for( ; ; )
    {
        ( void ) sh_call( SH_SYS_EXIT_EXTENDED, block );
    }
}

/* Copies the semihosting command line (QEMU -semihosting-config arg=...) into buf. */
static inline int sh_cmdline( char * buf, uint32_t len )
{
    uint32_t block[ 2 ] = { ( uint32_t ) buf, len };
    return ( int ) sh_call( SH_SYS_GET_CMDLINE, block );
}

void sh_puthex( uint32_t v );
void sh_putdec( uint32_t v );

#endif
