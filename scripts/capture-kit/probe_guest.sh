#!/bin/sh
# probe_guest.sh — run INSIDE the guest VM. Collects the facts the
# capture pipeline and the offline modeling track need. Never fails:
# every key gets a value or UNKNOWN=<reason>.
#
# Usage:  sh probe_guest.sh > guest_config.txt
# Transfer to the guest with scp, or serve from the host:
#   host$ python3 -m http.server 8000
#   guest$ wget http://10.0.2.2:8000/probe_guest.sh

emit() { printf '%s=%s\n' "$1" "$2"; }

emit PROBE_VERSION 1
emit ARCH "$(uname -m 2>/dev/null || echo 'UNKNOWN=uname-failed')"
emit KERNEL_VERSION "$(uname -r 2>/dev/null || echo 'UNKNOWN=uname-failed')"

# --- VA_BITS (arm64) ---
VA=""
CFG="/boot/config-$(uname -r 2>/dev/null)"
if [ -r "$CFG" ]; then
    VA=$(grep '^CONFIG_ARM64_VA_BITS=' "$CFG" 2>/dev/null | cut -d= -f2)
fi
if [ -z "$VA" ] && [ -r /proc/config.gz ]; then
    VA=$(zcat /proc/config.gz 2>/dev/null | grep '^CONFIG_ARM64_VA_BITS=' | cut -d= -f2)
fi
if [ -z "$VA" ]; then
    # Fallback: infer from the top of the process stack mapping.
    TOP=$(grep '\[stack\]' /proc/self/maps 2>/dev/null | head -1 \
          | cut -d' ' -f1 | cut -d- -f2)
    case "$TOP" in
        0000ffff*|ffff*) VA=48 ;;
        0000007f*|7f*)   VA=39 ;;
        000fffff*)       VA=52 ;;
        *) VA="UNKNOWN=no-kernel-config-and-unrecognized-stack-top-$TOP" ;;
    esac
fi
emit VA_BITS "$VA"

# --- Page size ---
PS=$(getconf PAGE_SIZE 2>/dev/null) || PS="UNKNOWN=getconf-missing"
emit PAGE_SIZE "${PS:-UNKNOWN=getconf-empty}"

# --- SVE ---
FEAT=$(grep -m1 '^Features' /proc/cpuinfo 2>/dev/null)
case "$FEAT" in
    *" sve"*|*" sve "*|*sve2*) emit SVE yes ;;
    "") emit SVE "UNKNOWN=no-Features-line" ;;
    *)  emit SVE no ;;
esac
if [ -r /proc/sys/abi/sve_default_vector_length ]; then
    emit SVE_VL "$(cat /proc/sys/abi/sve_default_vector_length)"
else
    emit SVE_VL "UNKNOWN=no-sve-sysctl"
fi

# --- CPUs ---
emit NCPU "$(nproc 2>/dev/null || grep -c '^processor' /proc/cpuinfo 2>/dev/null || echo 'UNKNOWN=no-nproc')"
MODEL=$(grep -m1 -E '^(model name|CPU part)' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')
emit CPU_MODEL_GUEST "${MODEL:-UNKNOWN=no-cpuinfo-model}"

# --- Accelerator (informational provenance only) ---
ACCEL=""
if command -v systemd-detect-virt >/dev/null 2>&1; then
    V=$(systemd-detect-virt 2>/dev/null)
    case "$V" in
        kvm)  ACCEL=KVM ;;
        qemu) ACCEL=TCG ;;
    esac
fi
if [ -z "$ACCEL" ]; then
    if dmesg 2>/dev/null | grep -qiE 'kvm-clock|Hypervisor detected: KVM'; then
        ACCEL=KVM
    fi
fi
emit ACCEL "${ACCEL:-UNKNOWN=no-systemd-detect-virt-and-no-dmesg-signal}"
