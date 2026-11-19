// Darwin errno values differ from Linux errno values, including the swapped
// EAGAIN/EDEADLK pair. Raw host errors can become unrelated guest failures:
// Darwin ENOSYS is 78, while Linux ENOSYS is 38. Returning 78 kept glibc's
// clone3-to-clone fallback from firing and surfaced as "pthread_create: Remote
// address changed". Keep Linux numbers explicit and derive host values from
// the active macOS SDK at build time.

include!(concat!(env!("OUT_DIR"), "/errno_map_generated.rs"));

#[no_mangle]
pub extern "C" fn lxrt_errno_to_linux(error: i32) -> i32 {
    map_errno(error)
}
