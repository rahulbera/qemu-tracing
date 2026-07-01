# kvmclock Patch — Technical Reference

## Problem Statement

QEMU snapshots taken under KVM include a `kvmclock` device state section.
When restoring under TCG (needed for our tracing plugin), QEMU fails because
the kvmclock device is not instantiated under TCG, so no VMState handler
exists to parse the section data from the snapshot stream.

## Error Message

```
qemu-system-x86_64: Unknown savevm section or instance 'kvmclock' 0.
Make sure that your current VM setup matches your saved VM setup,
including any hotplugged devices
qemu-system-x86_64: Error -22 while loading VM state
```

## Root Cause Analysis

### Snapshot Save Path (KVM)

1. Machine init calls `kvmclock_create()` which checks `kvm_enabled()`
2. Under KVM: creates a `kvmclock` SysBus device
3. Device registration automatically registers VMState handler `kvmclock_vmsd`
4. `savevm` serializes all registered VMState handlers, including kvmclock

### Snapshot Load Path (TCG) — FAILS

1. Machine init calls `kvmclock_create()` which checks `kvm_enabled()`
2. Under TCG: `kvm_enabled()` returns false → device NOT created
3. No VMState handler registered for "kvmclock"
4. `loadvm` encounters "kvmclock" section in stream → `find_se()` returns NULL → abort

### Why We Can't Skip Unknown Sections

The snapshot stream format:
```
[QEMU_VM_SECTION_FULL]
  [section_id: 4 bytes]
  [idstr_len: 1 byte] [idstr: variable]
  [instance_id: 4 bytes]
  [version_id: 4 bytes]
  [device data: VARIABLE LENGTH, NO LENGTH PREFIX]
  [section footer: 4 bytes]
```

The device data length is determined by the VMStateDescription — it's not
stored in the stream header. Without the VMStateDescription, we don't know
how many bytes to skip. Reading too few or too many bytes corrupts the
position for all subsequent sections.

## kvmclock VMState Format

From `hw/i386/kvm/clock.c`:

```c
typedef struct KVMClockState {
    SysBusDevice busdev;
    uint64_t clock;
    bool runstate_paused;
    bool clock_is_reliable;
    /* ... other non-migrated fields ... */
} KVMClockState;

static const VMStateDescription kvmclock_vmsd = {
    .name = "kvmclock",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = kvmclock_pre_load,
    .pre_save = kvmclock_pre_save,
    .post_load = kvmclock_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(clock, KVMClockState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &kvmclock_reliable_get_clock,  // optional: VMSTATE_BOOL
        NULL
    }
};
```

The on-wire format for this section is:
- 8 bytes: `clock` value (big-endian uint64_t)
- Possibly followed by subsection `kvmclock/clock_is_reliable`: 1 byte bool
- Section footer: 4 bytes

Total: ~9-14 bytes of device data. Tiny.

## Fix Strategy

### Approach: Make kvmclock Instantiable Under TCG

**Goal:** The kvmclock device should be created under TCG so that its
VMState handler is registered and the snapshot loader can parse (and
discard) the kvmclock data correctly.

**Two changes needed:**

1. **`kvmclock_create()` or its call site:** Remove or relax the
   `kvm_enabled()` guard so the device is created under TCG too.

2. **`kvmclock_realize()`:** Add an early return when `!kvm_enabled()`
   to skip KVM-specific initialization (KVM ioctls, VM state change
   handlers, etc.) that would fail under TCG.

**The result:** Under TCG, a kvmclock device exists with a registered
VMState handler but no KVM functionality. The snapshot loader finds the
handler, reads the 8-byte clock value (discarding it since there's no
KVM clock to set), reads the optional subsection, and continues to the
next device section.

### Steps for Implementation

1. **Read the source files:**
   - `~/qemu-9.2.4/hw/i386/kvm/clock.c` — device implementation
   - Search for `kvmclock_create` across the tree to find call site(s)
   - Check the `meson.build` in `hw/i386/kvm/` to understand build conditions

2. **Patch `kvmclock_realize()`:**
   Add early return for non-KVM:
   ```c
   static void kvmclock_realize(DeviceState *dev, Error **errp)
   {
       KVMClockState *s = KVM_CLOCK(dev);
   
       if (!kvm_enabled()) {
           /* Under TCG: device exists only for VMState load compat */
           return;
       }
   
       /* ... existing KVM init code ... */
   }
   ```

3. **Patch `kvmclock_create()` or call site:**
   Relax the `kvm_enabled()` check. The exact change depends on the code
   structure — it might be:
   ```c
   // Before:
   if (kvm_enabled() && ...) {
       sysbus_create_simple("kvmclock", -1, NULL);
   }
   
   // After:
   if (kvm_enabled() && ...) {
       sysbus_create_simple("kvmclock", -1, NULL);
   } else {
       /* Create dummy device for TCG snapshot compatibility */
       sysbus_create_simple("kvmclock", -1, NULL);
   }
   ```
   Or more simply, remove the condition entirely if the realize function
   handles the non-KVM case.

4. **Check pre_save/pre_load/post_load callbacks:**
   The VMState has `pre_save`, `pre_load`, and `post_load` callbacks.
   Under TCG, `post_load` would try to push the clock value to KVM
   (which doesn't exist). This callback needs to be guarded:
   ```c
   static int kvmclock_post_load(void *opaque, int version_id)
   {
       if (!kvm_enabled()) {
           return 0;  /* nothing to do under TCG */
       }
       /* ... existing code ... */
   }
   ```
   Similarly check `pre_save` and `pre_load`.

5. **Check meson.build:**
   `hw/i386/kvm/clock.c` is likely in a KVM-only source set. If so,
   it's already compiled (we build with `--enable-kvm`). If it's
   conditionally compiled only when KVM is the sole accelerator,
   we may need to adjust the build.

6. **Rebuild:**
   ```bash
   cd ~/qemu-9.2.4/build
   make -j$(nproc)
   make install
   ```

7. **Test** (see CLAUDE.md for test commands)

## Potential Additional Issues

After fixing kvmclock, there may be other KVM-specific sections in the
snapshot that cause similar errors. Common ones for x86 KVM:

| Section name | Device | Likely fix |
|-------------|--------|-----------|
| `kvmclock` | KVM paravirt clock | **This patch** |
| `kvm-tpr-opt` | TPR optimization | Same approach if encountered |
| `apic-msi` | KVM APIC | Same approach if encountered |
| `fw_cfg` | Firmware config | Usually exists under TCG too |

If additional sections fail, apply the same strategy: find the device,
make it instantiable under TCG with a no-op realize.

## Safety Assessment

**Risk: Low.** The kvmclock device under TCG does nothing — it's a
data sink for the VMState loader. The guest was already running without
kvmclock (since it's under TCG with no KVM), so the loaded clock value
is simply discarded. No functional impact on guest behavior.

The only risk is if `pre_load` or `post_load` callbacks assume KVM
is available and crash. Hence the need to guard those callbacks.
