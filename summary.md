| Scenario | base | new |
|---|---|---|
| basic | OK | OK |
| swap | FAIL (1) | OK |
| restart_audio | FAIL (2) | OK |
| restart_in_active | CRASH 0xC00000FD | OK |
| init_retry | FAIL (1) | OK |
| nan | FAIL (1) | OK |
| state | OK | OK |
| state_retry | FAIL (3) | OK |
| state_reject | FAIL (2) | OK |
| rescan | OK | OK |
| stress | FAIL (2) | OK |
| crash_process | CRASH 0xC0000005 | OK |
| throw_process | CRASH 0xC0000409 | OK |
| crash_init | CRASH 0xC0000005 | OK |
| crash_setstate | CRASH 0xC0000005 | OK |
| crash_initdll | CRASH 0xC0000005 | OK |
| quality_1 | OK | OK |
| quality_16 | OK | OK |
| soak | FAIL (2) | OK |
| shell_rescan | FAIL (1) | OK |
| shell_next_start | FAIL (1) | OK |
| shell_cached | OK | OK |
