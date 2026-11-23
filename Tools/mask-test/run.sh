#!/bin/bash
# QEMU runtime test of the critical-section mask change of the Cortex-M ports.
#
#   Tools/mask-test/run.sh [PORT...]      PORT = cm4f cm7 cm0 cm33 (default: all four)
#
# Builds the Docker image dsf-firmware-build-qemu (Dockerfile here, FROM dsf-firmware-build, the firmware build
# image of DuetSoftwareFramework, firmware/Dockerfile), then in it, per port: the test against this tree (src/,
# its FreeRTOSConfig.h with the hooks) and against the kernel before the change (negative control), runs each
# case in QEMU with semihosting and compares.
#
# Machines: cm4f mps2-an386 (Cortex-M4), cm7 mps2-an500 (Cortex-M7, port ARM_CM7/r0p1), cm0 microbit
# (Cortex-M0, nRF51), cm33 mps2-an505 (Cortex-M33, secure state, port ARM_CM33_NTZ secure-only).
#
# Negative control: `git archive MASK_TEST_CONTROL_REF src` copies the kernel before the change (default
# 980af10, the 3.7-dev commit that the change is based on) from this repository to build/control-kernel. Its own
# src/include/FreeRTOSConfig.h is this tree's configuration without the xPortTaskCanBlock() hooks (run.sh
# prints the diff), so the control is built with that file unchanged and with -DCONTROL=1, which drops the
# xPortTaskCanBlock() calls of the test and switches the expectations to the old behaviour.
#
# Priority bits: QEMU 8.2 implements 8 NVIC priority bits on ARMv7-M/ARMv8-M and 2 on ARMv6-M (probe.c
# prints them). With __NVIC_PRIO_BITS=8 the configuration gives configMAX_SYSCALL_INTERRUPT_PRIORITY 3,
# whose sub-priority bit xPortStartScheduler rejects (ARM_CM4F/port.c "configMAX_SYSCALL_INTERRUPT_PRIORITY
# & 0x1U"), so cm4f/cm7 use 3 bits (SAME51/SAME70), cm33 4 bits (STM32H5) and cm0 2 bits (SAMC21 = QEMU).
# The test checks at start that the NVIC implements at least configPRIO_BITS.
#
# Timing: the test runs use -icount shift=4 (virtual time = instruction count, 16 ns per instruction), so
# a tick lasts a fixed number of instructions whatever the host load. Without it the first vTaskDelay(2) of a run
# sometimes returned before the spinner task had run once ("vTaskDelay.spinner_ran", 4 of 15 runs on
# mps2-an500, 1 of 15 on microbit; 0 of 15 with -icount). The likely cause, not verified, is that the
# virtual clock then follows the host clock, so two ticks can pass while QEMU is busy elsewhere.
set -euo pipefail
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMAGE=${MASK_TEST_IMAGE:-dsf-firmware-build-qemu}

if [ "${1:-}" != "--in-container" ]; then
    CONTROL_REF=${MASK_TEST_CONTROL_REF:-980af1090f98f1c6cfbf682918194e9d4fc1a7e5}
    docker build -q -t "$IMAGE" - < "$HERE/Dockerfile" > /dev/null

    # Copy of the kernel before the change (never built in place)
    rm -rf "$HERE/build/control-kernel"
    mkdir -p "$HERE/build/control-kernel"
    git -C "$REPO" archive "$CONTROL_REF" src | tar -x -C "$HERE/build/control-kernel"
    echo "Control kernel: $(git -C "$REPO" log -1 --format='%h %s' "$CONTROL_REF") (git archive src)"
    echo "Configuration of the tree under test vs. the control's own (the control is built with its own):"
    diff "$HERE/build/control-kernel/src/include/FreeRTOSConfig.h" "$REPO/src/include/FreeRTOSConfig.h" || true
    echo

    exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$REPO:/work" \
        -w /work/Tools/mask-test "$IMAGE" ./run.sh --in-container "${@}"
fi
shift

# ---------------------------------------------------------------------------------------------- in the container
cd "$HERE"
PORTS=("$@")
[ ${#PORTS[@]} -gt 0 ] || PORTS=(cm4f cm7 cm0 cm33)
MODIFIED_KERNEL=../../src
CONTROL_KERNEL=build/control-kernel/src
LOGS=build/logs
mkdir -p "$LOGS"

machine_of() {
    case "$1" in
        cm4f) echo mps2-an386 ;;
        cm7) echo mps2-an500 ;;
        cm0) echo microbit ;;
        cm33) echo mps2-an505 ;;
    esac
}

# Expected exit status and, for an assertion, the hook in src/tasks.c
CASES_MODIFIED=(
    "mask 0"
    "block 0"
    "A-delay-mask 10 traceTASK_DELAY();"
    "A-semtake-primask 10 traceENTER_vTaskPlaceOnEventList( pxEventList, xTicksToWait );"
    "A-notifytake-mask 10 traceTASK_NOTIFY_TAKE_BLOCK( uxIndexToWaitOn );"
    "A-suspendself-mask 10 traceTASK_SUSPEND( pxTCB );"
    "A-delay-critical 10 traceTASK_DELAY();"
    "A-notifywait-mask 10 traceTASK_NOTIFY_WAIT_BLOCK( uxIndexToWaitOn );"
    "A-delayuntil-mask 10 traceTASK_DELAY_UNTIL( xTimeToWake );"
    "A-deleteself-mask 10 traceTASK_DELETE( pxTCB );"
    "N-semtake0-mask 0"
    "N-semgive-mask 0"
    "N-suspendother-mask 0"
    "N-suspendother-critical 0"
    "N-block-after-restore 0"
    "N-suspend-before-start 0"
)
CASES_CONTROL=(
    "mask 0"
    "block 0"
)

FAILED=0
SUMMARY=()

run_case() {  # variant port elf case expected_exit [hook...]
    local variant=$1 port=$2 elf=$3 name=$4 expect=$5
    shift 5
    local hook="$*" log="$LOGS/$variant-$port-$name.log" status=0 verdict detail=""
    timeout 60 qemu-system-arm -M "$(machine_of "$port")" -icount shift=4 -nographic -monitor none -serial none \
        -semihosting-config "enable=on,target=native,arg=$name" -kernel "$elf" > "$log" 2>&1 || status=$?
    verdict=PASS
    [ "$status" = "$expect" ] || verdict=FAIL
    if [ "$expect" = 10 ]; then
        local kernel=$MODIFIED_KERNEL lines at
        lines=$(grep -nF "$hook" "$kernel/tasks.c" | cut -d: -f1 | tr '\n' ' ')
        at=$(sed -n 's/^MASKTEST ASSERT .*tasks\.c:\([0-9]*\) (armed)$/\1/p' "$log")
        if [ -n "$at" ] && [[ " $lines " == *" $at "* ]]; then
            detail="asserted at tasks.c:$at = $hook"
        else
            verdict=FAIL
            detail="assert at '${at:-none}' ($(grep '^MASKTEST ASSERT' "$log" || true)), hook '$hook' is at tasks.c:$lines"
        fi
    fi
    if [ "$verdict" = PASS ] && [ "$expect" = 0 ] && ! grep -q '^MASKTEST RESULT PASS$' "$log"; then
        verdict=FAIL
    fi
    if [ "$verdict" = FAIL ]; then
        FAILED=1
        [ -n "$detail" ] || detail=$(grep -E '^MASKTEST RESULT|^MASKTEST ASSERT|BAD$' "$log" | head -5 | tr '\n' ';')
    fi
    printf '  %-6s %-9s %-26s exit=%-3s expected=%-3s %s %s\n' "$port" "$variant" "$name" "$status" "$expect" "$verdict" "$detail"
    SUMMARY+=("$(printf '%-6s %-9s %-26s %s' "$port" "$variant" "$name" "$verdict")")
}

for port in "${PORTS[@]}"; do
    echo "=== $port on $(machine_of "$port")"
    make -s PORT="$port" CROSS_COMPILE="$CROSS_COMPILE" BUILD="build/probe-$port" probe
    timeout 20 qemu-system-arm -M "$(machine_of "$port")" -nographic -monitor none -serial none \
        -semihosting-config enable=on,target=native -kernel "build/probe-$port/probe.elf" | sed 's/^/  /'
    make -s PORT="$port" VARIANT=modified KERNEL="$MODIFIED_KERNEL" CROSS_COMPILE="$CROSS_COMPILE"
    make -s PORT="$port" VARIANT=control KERNEL="$CONTROL_KERNEL" CROSS_COMPILE="$CROSS_COMPILE"

    for c in "${CASES_MODIFIED[@]}"; do
        # shellcheck disable=SC2086
        run_case modified "$port" "build/modified-$port/mask_test.elf" $c
    done
    for c in "${CASES_CONTROL[@]}"; do
        # shellcheck disable=SC2086
        run_case control "$port" "build/control-$port/mask_test.elf" $c
    done
    sed -n 's/^MASKTEST \(modified\|control\) /  /p' "$LOGS/modified-$port-mask.log"

    # Checks of the "mask" case whose observed value differs between the two kernels
    echo "  mask case, observed values that differ between the kernels (id: modified -> control):"
    join -j1 -o 1.1,1.2,2.2 \
        <(sed -n 's/^CHECK \([^ ]*\) \([^ ]*\) obs=\([^ ]*\) .*/\1:\2 \3/p' "$LOGS/modified-$port-mask.log" | sort) \
        <(sed -n 's/^CHECK \([^ ]*\) \([^ ]*\) obs=\([^ ]*\) .*/\1:\2 \3/p' "$LOGS/control-$port-mask.log" | sort) |
        awk '$2 != $3 { printf "    %-44s %s -> %s\n", $1, $2, $3; n++ } END { if (!n) print "    (none)" }'
    echo
done

echo "=== Summary"
printf '%s\n' "${SUMMARY[@]}"
if [ "$FAILED" = 0 ]; then
    echo "ALL PASS"
else
    echo "FAILURES (logs in Tools/mask-test/$LOGS)"
fi
exit "$FAILED"
