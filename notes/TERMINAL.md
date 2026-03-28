# CosmoRT Terminal Audit

**Status:** 2026-03-28  
**Scope:** VT/PTY/TTY implementation audit against Linux behavior  
**Recommendation:** Significant revisions needed for interactive shell compatibility

---

## Executive Summary

CosmoRT's terminal implementation provides basic functionality but deviates from Linux in critical areas affecting interactive shell workflows. The implementation stores only `canon` and `echo` flags, lacks full `termios` structure support, hardcodes control characters, and does not properly enforce foreground/background process group semantics. These gaps prevent correct job control and signal delivery in shell pipelines.

**Critical Issue:** Interactive shell pattern (fork → setpgid → TIOCSPGRP → exec) will fail because:
1. `pty_master_write()` sends signals to `fg_pgid` OR `slave_pid` (fallback), not proper process group
2. No SIGTTIN/SIGTTOU for background process group access protection
3. Control characters (^C, ^Z) are hardcoded, not configurable via `c_cc[]`
4. `termios` flags (`c_iflag`, `c_oflag`, `c_cflag`) are reported statically, not stored

---

## Deviations from Linux TTY

### CRITICAL (blocks interactive shell)

#### 1. Missing `termios` Structure Storage
**Linux behavior:**  
- Stores complete `struct termios` (36 bytes): `c_iflag`, `c_oflag`, `c_cflag`, `c_lflag`, `c_line`, `c_cc[19]`
- Each terminal has independent termios config
- Applications can query/set any flag via TCGETS/TCSETS

**CosmoRT behavior:**  
- `pty_t` stores only `int echo` and `int canon` (2 fields)
- `c_cc[]` array not stored; all 19 control characters ignored
- TCGETS returns hardcoded flags for all PTYs
  - `c_iflag = 0x0500` (always ICRNL | IXON)
  - `c_oflag = 0x0005` (always OPOST | ONLCR)
  - `c_cflag = 0x00BF` (always B38400 | CS8 | CREAD)
  - `c_lflag` built from stored canon/echo, but missing ISIG enforcement

**File/line:**  
- `/home/cosmo/Git/CosmoRT/include/kernel/vt/pty.h:17-46` (pty_t struct)
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:679-694` (TCGETS hardcoding)
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:740-766` (TCSETS ignores most flags)

**Impact:**  
- Applications expecting `c_cc[VMIN]`, `c_cc[VTIME]` (raw mode read behavior) will hang or return immediately with wrong semantics
- Shell cannot configure terminal after `exec` (e.g., `sh -i` relies on TCSETS to set canonical mode)
- Escape sequence timing (VTIME timeout) not supported

**Fix priority:** HIGH — affects any terminal-aware application

---

#### 2. Hardcoded Control Characters
**Linux behavior:**  
- Control characters (VINTR=^C, VQUIT=^\, VSUSP=^Z, VEOF=^D, etc.) stored in `termios.c_cc[0..19]`
- Applications call `tcgetattr()` → read `c_cc[]` → customize (e.g., stty, emacs)
- Configurable per-PTY at runtime

**CosmoRT behavior:**  
- Control characters hardcoded in `pty_master_write()`:
  - `c == 3` → SIGINT (^C, hardcoded)
  - `c == 26` → SIGTSTP (^Z, hardcoded)
  - `c == 4` → EOF (^D, hardcoded)
  - `c == 28` → SIGQUIT (^\, hardcoded)
  - `c == 12` → clear screen (^L, Linux doesn't do this)

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/vt/pty.c:143-178` (hardcoded signal dispatch)
- Control character values are C literals (`== 3`, `== 4`, etc.), not fetched from `c_cc[]`

**Impact:**  
- Applications that set custom control characters (e.g., `stty intr ^G`) will be ignored
- `termios` queries report hardcoded values; actual behavior differs
- Job control tools (e.g., `fg`, `bg` with custom suspend key) will break
- Ctrl+L will clear screen even if VDISCARD/VLNEXT is configured to that key

**Fix priority:** CRITICAL — breaks job control and shell customization

---

#### 3. Foreground Process Group Logic Flawed
**Linux behavior:**  
- Terminal has independent `fg_pgid` per controlling terminal
- Read/write syscalls check:
  - **Background read from terminal:** if `proc->pgid != pty->fg_pgid` AND not in session leader, raise SIGTTIN (block)
  - **Background write to terminal:** if `proc->pgid != pty->fg_pgid` AND not in session leader, raise SIGTTOU (block)
  - Signal delivery (SIGINT, SIGTSTP) goes to `pty->fg_pgid`, not all processes

**CosmoRT behavior:**  
- `pty_t` stores `fg_pgid` (initialized to 0)
- Signal delivery: `send_signal_to_fg()` in pty.c:99-119 uses:
  ```c
  int pgid = p->fg_pgid;
  if (pgid <= 0) pgid = p->slave_pid;  // FALLBACK: wrong!
  ```
  This allows signals to reach wrong processes if `fg_pgid` not set

- No SIGTTIN/SIGTTOU enforcement on read/write:
  - `pty_slave_read()` (sys_file.c:266-317) will return data to any process, even if background
  - `pty_slave_write()` (sys_file.c:143-158) will drain to output buffer for any process
  - Should raise SIGTTOU if `proc->pgid != pty->fg_pgid` and not session leader

- TIOCSPGRP (sys_file.c:772-779) stores value but no validation:
  - Should check `pgid` is in same session
  - Should validate `pgid` is a live process group

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/vt/pty.c:99-119` (signal dispatch, fallback to slave_pid)
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:266-318` (pty_slave_read, no SIGTTIN)
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:143-158` (pty_slave_write, no SIGTTOU)
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:772-779` (TIOCSPGRP, no validation)

**Impact:**  
- Job control broken: backgrounded job can read from terminal (hangs), can write to terminal (pollutes foreground)
- Shell pattern `fg %1` → restore foreground process group fails because:
  1. TIOCSPGRP not validated
  2. Background writes not blocked with SIGTTOU
  3. Signal delivery falls back to `slave_pid` if `fg_pgid` misconfigured

**Fix priority:** CRITICAL — breaks job control

---

#### 4. Input Processing Flags Not Applied
**Linux behavior:**  
- ICRNL (0x0100): CR (0x0D) → NL (0x0A) on input, unless disabled
- INLCR (0x0040): NL → CR on input
- IGNCR (0x0080): ignore CR on input
- ISTRIP (0x0020): mask to 7 bits
- IXON/IXOFF (0x0400/0x1000): XON/XOFF flow control
- Applied in canonical mode AND in raw mode if enabled

**CosmoRT behavior:**  
- All input goes directly to input buffer in `pty_master_write()` or line discipline
- No ICRNL processing: CR stays as CR
- No ISTRIP: high bit not masked
- No IXON/IXOFF: flow control not implemented
- ONLCR not applied to process output → NL not converted to CR+NL

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/vt/pty.c:133-228` (pty_master_write, no c_iflag processing)
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:673-676` (TCGETS reports flags as if set, but not enforced)

**Impact:**  
- Applications reading from terminal will see CR as CR, not NL (breaks line parsing)
- XON/XOFF not supported; slow terminals cannot throttle input
- Programs expecting ISTRIP will see garbage if 8th bit set

**Fix priority:** MEDIUM — affects terminal behavior but less common in modern systems

---

### HIGH (breaks common programs)

#### 5. VMIN/VTIME Not Implemented
**Linux behavior:**  
- `c_cc[VMIN]` (element 6): minimum characters to return from read
  - VMIN=0: non-blocking read (return immediately)
  - VMIN>0: blocking read until N chars available
- `c_cc[VTIME]` (element 5): timeout in deciseconds (0.1s units)
  - VTIME=0 with VMIN>0: wait forever
  - VTIME>0: return after timeout even if < VMIN chars
- Separate logic for canonical vs. raw mode

**CosmoRT behavior:**  
- `pty_slave_read()` (sys_file.c:266-318) ignores VMIN/VTIME
- Always blocks until data available OR returns all available data
- No timeout support
- No way to get non-blocking raw mode read behavior

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:266-318` (pty_slave_read always blocking)
- `c_cc[]` array never read from termios structure (not stored)

**Impact:**  
- Raw mode programs (vi, less, top, tmux) cannot implement timeout reads
- Single-character input (getc equiv) will block indefinitely
- Terminal multiplexers cannot poll multiple PTYs efficiently

**Fix priority:** HIGH — affects interactive applications

---

#### 6. Canonical Mode Line Editing Incomplete
**Linux behavior:**  
- ICANON: line-buffered input with editing
  - Backspace (VERASE): move left one char
  - ECHOE: echo backspace as "BS SP BS" to erase on display
  - ECHOK: echo NL after VKILL
  - ECHOCTL: echo control characters as ^X
  - Word erase (VWERASE=^W): delete word, echo spaces
  - Line kill (VKILL=^U): delete line, echo erase sequence

**CosmoRT behavior:**  
- ICANON implemented (boolean in pty_t)
- Backspace supported: `c == '\b' || c == 127` (pty.c:183-189)
- Backspace echo: hardcoded as "\b \b" (not ECHOE via c_cc)
- No VWERASE (^W) support
- No VKILL (^U) support
- No word deletion or line kill

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/vt/pty.c:181-200` (line editing, backspace only)
- Backspace hardcoded, not referenced from `c_cc[VERASE]`

**Impact:**  
- Shells work (basic editing), but ^U and ^W not available
- Users cannot customize erase/kill keys
- Less critical than VMIN/VTIME, but incomplete line discipline

**Fix priority:** MEDIUM — shells work, but power users lose features

---

#### 7. No TIOCSCTTY Controlling Terminal Assignment
**Linux behavior:**  
- TIOCSCTTY: Process acquires controlling terminal
  - First process in session (sid == pid) can claim terminal
  - Gives that process a "controlling terminal" for job control and signals
  - SIGHUP, SIGCONT, SIGTERM sent to controlling process on hangup/logout
- TIOCNOTTY: disassociate from controlling terminal

**CosmoRT behavior:**  
- TIOCSCTTY/TIOCNOTTY handled as no-op (sys_file.c:769-770)
- No `controlling_terminal` field in `process_t`
- SIGHUP/SIGTERM on terminal close not implemented
- Session leader cannot be identified as having control

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:769-770` (TIOCSCTTY returns 0, no-op)
- `/home/cosmo/Git/CosmoRT/include/kernel/proc/process.h:20-85` (no controlling_terminal field)

**Impact:**  
- Logout/VT exit does not kill child processes
- Session-wide signals (SIGHUP) not sent
- Less critical for single-user system but breaks daemon behavior

**Fix priority:** LOW — single-user system, but affects daemon/background job cleanup

---

### MEDIUM (edge cases)

#### 8. Output Processing (OPOST, ONLCR) Not Applied
**Linux behavior:**  
- OPOST (0x0001): enable output processing
- ONLCR (0x0004): NL → CR+NL on output
- OCRNL (0x0008): CR → NL
- ONOCR (0x0010): do not output CR
- ONLRET (0x0020): NL performs carriage return

**CosmoRT behavior:**  
- `pty_slave_write()` (sys_file.c:249-264) copies bytes directly to output buffer
- No ONLCR processing: NL stays as NL, not CR+NL
- No OPOST enforcement

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:249-264` (slave_write, no processing)

**Impact:**  
- Programs writing bare NL might not position cursor to column 0 on some terminals
- Less critical on modern VTs that handle both NL and CR+LF
- VT rendering handles both, so visual impact minimal

**Fix priority:** LOW — mostly cosmetic

---

#### 9. Missing ICANON→raw Mode Transition Handling
**Linux behavior:**  
- Switching from canonical to raw mode should flush line buffer
- Already-typed characters in line buffer should be discarded or transferred

**CosmoRT behavior:**  
- TCSETS (sys_file.c:750-761) flushes line buffer when switching canon→raw:
  ```c
  if (was_canon && !pt->canon && pt->line_pos > 0) {
      for (int li = 0; li < pt->line_pos; li++)
          pt->input_buf[...] = pt->line_buf[li];
      pt->line_pos = 0;
  }
  ```
- **Bug:** Transfers line buffer characters to input ring, but Linux **discards** them
- Programs expecting clean input after mode switch will see stale characters

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/sys/sys_file.c:750-761` (TCSETS mode switch)

**Impact:**  
- Rare case (most programs don't switch modes), but breaks some terminal editors
- Stale characters in input cause unexpected behavior

**Fix priority:** LOW → MEDIUM (depends on application)

---

#### 10. PTY Output Echo Not Respecting OPOST/ONLCR
**Linux behavior:**  
- Master write (`pty_master_write`) echoes character back to output buffer
- Echo should respect OPOST/ONLCR flags
- Backspace echo should be "\b \b" (not counting display columns)

**CosmoRT behavior:**  
- Echo hardcoded in `pty_master_write()` (pty.c:192, 199, 205-206)
- Backspace echo: hardcoded "\b \b" (correct)
- NL echo: hardcoded "\n" (should check ONLCR)

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/vt/pty.c:147, 154, 186, 192, 199`

**Impact:**  
- Programs with custom echo settings will see standard echo anyway
- Mostly cosmetic since NL → CR+NL is expected on terminals

**Fix priority:** LOW

---

## Signal Generation Issues

#### 11. Signal Delivery Not Respecting ISIG Flag
**Linux behavior:**  
- ISIG (0x0001): enable signal generation from control characters
- If ISIG disabled, ^C/^Z/^\ are passed as regular input
- If ISIG enabled, signals sent to foreground process group

**CosmoRT behavior:**  
- Signal generation always enabled: `pty_master_write()` line 143-178
- Only checks `p->canon` (canonical mode), not c_lflag ISIG flag
- Cannot disable signal generation

**File/line:**  
- `/home/cosmo/Git/CosmoRT/src/kernel/vt/pty.c:143-158` (always generates signals)

**Impact:**  
- Programs that disable ISIG (some interactive editors) will still get SIGINT on ^C
- Rare case, but breaks full termios compatibility

**Fix priority:** LOW

---

## Recommended Fix Order

### Phase 1: Critical (blocks job control) — Priority NOW
1. **Extend `pty_t` structure** to store full `termios`:
   - Add `struct termios term_ios` (36 bytes)
   - Add `uint8_t c_cc[19]` array for control characters
   - Add `uint32_t controlling_terminal` field to process_t

2. **Implement signal routing correctly**:
   - Remove fallback to `slave_pid` in `send_signal_to_fg()`
   - Always use `fg_pgid` (validate before TIOCSPGRP)
   - Add SIGTTIN/SIGTTOU enforcement in read/write paths

3. **Implement VMIN/VTIME**:
   - Read `c_cc[VMIN]` and `c_cc[VTIME]` in `pty_slave_read()`
   - Support timeout waits using timer infrastructure
   - Return early if VMIN=0

4. **Make control characters configurable**:
   - Store and query `c_cc[VINTR]`, `c_cc[VSUSP]`, etc.
   - Dispatch signals based on configurable values

### Phase 2: Important (affects many programs) — Priority HIGH
5. **Implement ICRNL, ISTRIP, IXON/IXOFF**:
   - Read `c_iflag` in `pty_master_write()`
   - Apply CR→NL translation if ICRNL
   - Strip 8th bit if ISTRIP
   - Implement flow control stubs

6. **Implement full canonical mode**:
   - Add VKILL (^U) and VWERASE (^W) support
   - Reference `c_cc[]` for VERASE, VKILL, VWERASE keys
   - Honor ECHOE, ECHOK, ECHOCTL flags from `c_lflag`

7. **Implement OPOST/ONLCR**:
   - Apply NL→CR+NL conversion in `pty_slave_write()` if enabled
   - Read `c_oflag` to check OPOST flag

### Phase 3: Desirable — Priority MEDIUM/LOW
8. **Implement TIOCSCTTY**:
   - Store `controlling_terminal` in process struct
   - Validate session on terminal assignment
   - Send SIGHUP/SIGTERM to controlling process on terminal close

9. **Fix mode transition handling**:
   - Discard (not transfer) line buffer on canon→raw switch
   - Match Linux semantics exactly

10. **Minor improvements**:
    - Remove ^L hardcoding (treat as regular char unless VDISCARD)
    - Respect ISIG flag
    - Validate TIOCSPGRP pgid

---

## Files Requiring Changes

| File | Changes Required | Priority |
|------|------------------|----------|
| `include/kernel/vt/pty.h` | Extend `pty_t`: add `struct termios`, `c_cc[]` | NOW |
| `include/kernel/proc/process.h` | Add `controlling_terminal` field | HIGH |
| `src/kernel/vt/pty.c` | Replace hardcoded control chars, implement flow control | NOW |
| `src/kernel/sys/sys_file.c` | TCGETS/TCSETS full implementation, SIGTTIN/SIGTTOU enforcement | NOW |
| `src/kernel/sys/sys_signal.c` | (no changes needed — framework exists) | — |

---

## Testing Strategy

Before fixing, ensure:
1. Interactive shell works: `sh -i`, job control (`fg`, `bg`, ^Z)
2. vi/less exit on SIGINT
3. Terminal-aware programs (tmux, screen) run without hanging
4. `stty` reads and sets termios correctly
5. Timeout reads work: `read -t 1 VAR`
6. Background job blocks on read (SIGTTIN)

---

## Restructure: src/kernel/vt/ → src/kernel/tty/

Aktuell liegt alles unter `src/kernel/vt/`. Linux hat `drivers/tty/` — aber
TTY braucht Kernel-Interna (Scheduler, Signals, Prozesse), passt nicht unter
die Driver-Isolation (`src/drivers/` = nur `include/public/`).

Ziel: `src/kernel/tty/` mit Linux-konformer Benennung:

```
src/kernel/tty/
  tty_io.c       Core TTY I/O Dispatch (open, read, write, close)
  tty_ioctl.c    termios, TIOC* ioctls (TCGETS/TCSETS/TIOCSPGRP/...)
  n_tty.c        Line Discipline (canonical/raw, c_cc[], VMIN/VTIME)
  pty.c          PTY master/slave
  vt/
    vt.c         VT State, ANSI Parser, Cursor, Scrollback
    keyboard.c   Keyboard Input, Keymap
    fb.c         Framebuffer Rendering (Glyph, Dirty Tracking)
```

Aktuell: `pty.c` mischt Line Discipline + PTY I/O + termios.
Linux trennt: `pty.c` (PTY), `n_tty.c` (Line Discipline), `tty_ioctl.c` (termios).

---

## References

- Linux kernel: `drivers/tty/tty_ioctl.c` (TCGETS/TCSETS)
- Linux kernel: `drivers/tty/n_tty.c` (line discipline, canonica mode)
- POSIX termios spec: `man 3 termios`
- Job control: `man 7 credentials` (session/process group semantics)

