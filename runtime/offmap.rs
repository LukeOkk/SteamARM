//! Reclaim host pages that contain only the edges of 4 KiB guest file maps.
//!
//! Darwin maps in 16 KiB pages. A guest mapping at a 4 KiB file offset may
//! leave an unnameable head or tail page after guest munmap. Track host pages
//! owned by this module separately from guest bytes still mapped inside them;
//! reclaim an edge only after its final guest piece is gone.

use std::ffi::{c_char, c_int, c_ulonglong, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Mutex, MutexGuard, Once};

include!(concat!(env!("OUT_DIR"), "/timer_constants.rs"));
include!(concat!(env!("OUT_DIR"), "/tls_constants.rs"));

const HOST_PAGE: u64 = 16_384;

#[repr(transparent)]
#[derive(Clone, Copy)]
struct DarwinSigSet(u32);

#[derive(Clone, Copy)]
struct Range {
    start: u64,
    end: u64,
}

#[derive(Default)]
struct RangeSet {
    entries: Vec<Range>,
}

impl RangeSet {
    fn lower(&self, address: u64) -> usize {
        let mut lo = 0;
        let mut hi = self.entries.len();
        while lo < hi {
            let mid = (lo + hi) / 2;
            if self.entries[mid].end <= address {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        lo
    }

    fn reserve(&mut self, extra: usize) -> bool {
        let Some(needed) = self.entries.len().checked_add(extra) else {
            return false;
        };
        if needed <= self.entries.capacity() {
            return true;
        }
        let mut capacity = if self.entries.capacity() == 0 {
            64
        } else {
            self.entries.capacity().saturating_mul(2)
        };
        while capacity < needed {
            let next = capacity.saturating_mul(2);
            if next == capacity {
                return false;
            }
            capacity = next;
        }
        self.entries
            .try_reserve_exact(capacity - self.entries.len())
            .is_ok()
    }

    fn intersects(&self, start: u64, end: u64) -> bool {
        let index = self.lower(start);
        index < self.entries.len() && self.entries[index].start < end
    }

    // Sorted, disjoint half-open ranges; touching ranges are merged.
    fn add(&mut self, mut start: u64, mut end: u64) -> bool {
        let mut index = self.lower(start);
        if index > 0 && self.entries[index - 1].end == start {
            index -= 1;
        }
        let mut next = index;
        while next < self.entries.len() && self.entries[next].start <= end {
            start = start.min(self.entries[next].start);
            end = end.max(self.entries[next].end);
            next += 1;
        }
        if next > index {
            self.entries[index] = Range { start, end };
            self.entries.drain(index + 1..next);
            return true;
        }
        if !self.reserve(1) {
            return false;
        }
        self.entries.insert(index, Range { start, end });
        true
    }

    // If a split cannot allocate, drop the whole range only when requested;
    // this keeps allocation failures from unmapping a live guest page.
    fn remove(&mut self, start: u64, end: u64, drop_whole: bool) {
        let mut index = self.lower(start);
        while index < self.entries.len() && self.entries[index].start < end {
            let range = self.entries[index];
            if range.start < start && range.end > end {
                if !self.reserve(1) {
                    if drop_whole {
                        self.entries.remove(index);
                    }
                    return;
                }
                self.entries.insert(
                    index + 1,
                    Range {
                        start: end,
                        end: range.end,
                    },
                );
                self.entries[index].end = start;
                return;
            }
            if range.start < start {
                self.entries[index].end = start;
                index += 1;
            } else if range.end > end {
                self.entries[index].start = end;
                return;
            } else {
                self.entries.remove(index);
            }
        }
    }
}

struct State {
    owned: RangeSet,
    pieces: RangeSet,
}

static STATE: Mutex<State> = Mutex::new(State {
    owned: RangeSet {
        entries: Vec::new(),
    },
    pieces: RangeSet {
        entries: Vec::new(),
    },
});
static LIVE: AtomicI32 = AtomicI32::new(0);
static FORK_HOOKS: Once = Once::new();
type StateGuard = MutexGuard<'static, State>;
static mut FORK_GUARD: MaybeUninit<StateGuard> = MaybeUninit::uninit();

#[link(name = "System")]
unsafe extern "C" {
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn sigfillset(set: *mut c_void) -> c_int;
    fn sigdelset(set: *mut c_void, signal: c_int) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, old: *mut c_void) -> c_int;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
}

fn lock_state() -> StateGuard {
    STATE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

extern "C" fn fork_prepare() {
    let guard = lock_state();
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<StateGuard>()
            .write(guard);
    }
}

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<StateGuard>());
    }
}

extern "C" fn register_fork_hooks() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(Some(fork_prepare), Some(fork_release), Some(fork_release));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn block_async_signals() -> DarwinSigSet {
    let mut all = DarwinSigSet(0);
    let mut old = DarwinSigSet(0);
    unsafe {
        sigfillset((&mut all as *mut DarwinSigSet).cast());
        sigdelset((&mut all as *mut DarwinSigSet).cast(), D_SIGBUS);
        sigdelset((&mut all as *mut DarwinSigSet).cast(), D_SIGSEGV);
        sigdelset((&mut all as *mut DarwinSigSet).cast(), D_SIGILL);
        sigdelset((&mut all as *mut DarwinSigSet).cast(), D_SIGTRAP);
        pthread_sigmask(
            D_SIG_BLOCK,
            (&all as *const DarwinSigSet).cast(),
            (&mut old as *mut DarwinSigSet).cast(),
        );
    }
    old
}

fn lock_nosig() -> (StateGuard, DarwinSigSet) {
    let old = block_async_signals();
    (lock_state(), old)
}

fn unlock_nosig(guard: StateGuard, old: DarwinSigSet) {
    LIVE.store(
        (guard.owned.entries.len() + guard.pieces.entries.len()) as c_int,
        Ordering::SeqCst,
    );
    drop(guard);
    unsafe {
        pthread_sigmask(
            D_SIG_SETMASK,
            (&old as *const DarwinSigSet).cast(),
            ptr::null_mut(),
        );
    }
}

fn align_down(address: u64) -> u64 {
    address & !(HOST_PAGE - 1)
}

fn align_up(address: u64) -> u64 {
    address.wrapping_add(HOST_PAGE - 1) & !(HOST_PAGE - 1)
}

#[no_mangle]
pub extern "C" fn lxrt_offmap_note(
    host_start: u64,
    host_end: u64,
    guest_start: u64,
    guest_len: u64,
) {
    if host_start >= host_end || guest_len == 0 {
        return;
    }
    let guest_end = guest_start.wrapping_add(guest_len);
    let (mut state, old) = lock_nosig();
    // Pieces first: an owned page without its pieces would look dead.
    if state.pieces.add(guest_start, guest_end) && !state.owned.add(host_start, host_end) {
        state.pieces.remove(guest_start, guest_end, false);
    }
    unlock_nosig(state, old);
}

#[no_mangle]
pub extern "C" fn lxrt_offmap_unmapped(address: u64, len: u64, dead: *mut u64) -> c_int {
    if LIVE.load(Ordering::SeqCst) == 0 || len == 0 || dead.is_null() {
        return 0;
    }
    let end = address.wrapping_add(len);
    let host_start = align_up(address);
    let host_end = align_down(end);

    // Partly covered host pages at the two edges (one if both edges share a page).
    let mut candidates = [0u64; 2];
    let mut candidate_count = 0usize;
    if address % HOST_PAGE != 0 {
        candidates[candidate_count] = align_down(address);
        candidate_count += 1;
    }
    let tail = align_down(end);
    if end % HOST_PAGE != 0 && (candidate_count == 0 || candidates[0] != tail) {
        candidates[candidate_count] = tail;
        candidate_count += 1;
    }

    let (mut state, old) = lock_nosig();
    state.pieces.remove(address, end, false);
    if host_start < host_end {
        state.owned.remove(host_start, host_end, true);
    }
    let mut dead_count = 0usize;
    for page in candidates.iter().take(candidate_count).copied() {
        if !state.owned.intersects(page, page + HOST_PAGE)
            || state.pieces.intersects(page, page + HOST_PAGE)
        {
            continue;
        }
        if unsafe { munmap(page as usize as *mut c_void, HOST_PAGE as usize) } != 0 {
            continue;
        }
        state.owned.remove(page, page + HOST_PAGE, true);
        unsafe { dead.add(dead_count).write(page) };
        dead_count += 1;
    }
    unlock_nosig(state, old);

    if dead_count != 0 {
        unsafe {
            if lxrt_trace_on() {
                let first = dead.read();
                fprintf(
                    lxrt_trace_stream(),
                    b"[lxrt] munmap 0x%llx+0x%llx: %d host page(s) of a 4 KiB-offset file mapping freed with their last guest page (0x%llx%s)\n\0"
                        .as_ptr()
                        .cast(),
                    address as c_ulonglong,
                    len as c_ulonglong,
                    dead_count as c_int,
                    first as c_ulonglong,
                    if dead_count > 1 {
                        b" and one more\0".as_ptr().cast::<c_char>()
                    } else {
                        b"\0".as_ptr().cast::<c_char>()
                    },
                );
            }
        }
    }
    dead_count as c_int
}

#[no_mangle]
pub extern "C" fn lxrt_offmap_disown(address: u64, len: u64) {
    if LIVE.load(Ordering::SeqCst) == 0 || len == 0 {
        return;
    }
    let end = address.wrapping_add(len);
    let (mut state, old) = lock_nosig();
    state.pieces.remove(address, end, false);
    state.owned.remove(align_down(address), align_up(end), true);
    unlock_nosig(state, old);
}
