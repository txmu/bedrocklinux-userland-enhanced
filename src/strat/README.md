strat
=====

strat runs the specified Bedrock Linux stratum's instance of an executable.

Usage
-----
    strat <stratum-name> <command-to-run>

Ad-Hoc Mode & Security Policy
-----------------------------
In addition to registered strata, `strat` accepts absolute paths. To prevent privilege escalation, 
three security levels are available via `brl adhoc`:

### 1. Disabled (Mode 0)
Ad-Hoc execution is completely forbidden. Best for high-security production servers.

### 2. Secure / Verified (Mode 1) - DEFAULT
Protects against the "Malicious Rootfs" attack.
- The caller must be the real root user (UID 0).
- The target directory must be owned by root.
This prevents a non-privileged user from tricking root into running a subverted environment.

### 3. Permissive / Unchecked (Mode 2)
The original Bedrock Enhanced behavior. Bypasses all ownership checks.
- **Benefit:** Zero-setup entry. Run user-owned rootfs (e.g., extracted tarballs in /home) as root.
- **Benefit:** Allows fixing foreign filesystems (FAT32/NTFS/Network) that lack POSIX ownership.
- **Risk:** If Root is induced to run `strat` on a user-controlled directory, the user can 
  gain full system persistence or execute arbitrary code via subverted binaries like `/bin/sh`.

Switching Policies
------------------
To switch modes, run as real root:
    # brl adhoc [disabled|secure|permissive]
