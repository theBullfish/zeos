"""Shared QEMU acceleration helper for Zeos test harnesses.
Returns KVM flags when /dev/kvm is usable (near-native speed, ~3x faster boot
and near-real-hardware timing on this Ryzen host), else empty (TCG fallback)."""
def accel_args():
    try:
        with open("/dev/kvm", "rb"):
            return ["-enable-kvm", "-cpu", "host"]
    except Exception:
        return []
