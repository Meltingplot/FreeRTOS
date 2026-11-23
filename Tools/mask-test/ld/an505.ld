/* QEMU mps2-an505 (Cortex-M33, boots secure with VTOR_S 0x10000000): secure aliases of ZBT SSRAM1
 * (code, 0x10000000) and ZBT SSRAM2 (data, 0x38000000). */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x38000000, LENGTH = 2M
}
INCLUDE common.ld
