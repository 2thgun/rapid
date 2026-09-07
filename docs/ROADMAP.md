# Delivery backlog

New runtime work should use C++ wherever practical. These are local issue drafts;
no GitHub issues, assignees, dates or remote milestones have been created.

## Milestones

| Milestone | Status | Exit condition |
| --- | --- | --- |
| M0: repository and clean dashboard | Delivered locally/on Pi | Preserved originals, documented source tree, native log notification, offline checks |
| M1: dependable live recording | Open | Per-lap publication, recovery and four-simulator acceptance |
| M2: native Pi parity | Deployed; live acceptance open | Debug/Release checks and cutover complete; verify live simulator recordings |
| M3: archive acceptance | Native recovery tests pass; end-to-end check pending | Resume, crash recovery, authentication and limits pass end-to-end tests |

## Prioritized issue drafts

| ID | Priority | Outcome | Required acceptance evidence |
| --- | --- | --- | --- |
| RPD-001 | P1 | Publish completed laps during continued driving | New valid `.ld` visible on SMB before session end; inspect in MoTeC |
| RPD-002 | P1 | Preserve data if bundle publication fails | Inject write/rename failure, restart, recover the original samples |
| RPD-003 | P1 | Deploy and validate v4 with a live simulator | Six Pi suites and Windows-to-Pi UDP round trip pass; production key pairing and live acceptance pending |
| RPD-004 | P1 | Respect configured runtime controls | ACC/upload disable switches work; upload policy, retention and local-recording flags tested |
| RPD-005 | P1 | Keep receiver alive on malformed input | Reject non-object JSON and invalid fields, then accept a valid frame |
| RPD-006 | P1 | Reliable sessions, laps and recovered upload queue | New sessions reset identity; equal-time distinct laps persist; recovered bundles enqueue on first start |
| RPD-007 | P1 | Four-simulator live acceptance | ACC/AC/ACE/iRacing values, multiple laps, MoTeC units and switching without daemon restart; depends on RPD-001/003/006 |
| RPD-008 | P2 | Complete dashboard data wiring | Real recorder state, consistent wheel-speed/ABS units, session-reset timing and source sectors |
| RPD-009 | P1 | Archive crash consistency | Restart between file append and DB commit resumes without duplicated bytes |
| RPD-010 | P2 | Archive limits and producer compatibility | Bounded uploads, correct artifact roles and blank-metadata handling; depends on RPD-009 |
| RPD-011 | P2 | Native Pi production replacement | Parallel parity checks and on-device recording acceptance before changing `rapid.service` |
| RPD-012 | P2 | Finish boot/display review | Explain legacy `rc.local` seven-second sleep and framebuffer helpers before modifying them; verify physical cold boot |

Use Backlog, Ready, In progress, Blocked and Done when importing these into GitHub.
Record evidence separately from status. Native Linux and Windows companion CI passed
for revision `47860ec`: [verified run](https://github.com/2thgun/rapid/actions/runs/34064690027).

## Evidence checkpoint — 2026-09-07

- RPD-011: native production replacement completed; live recording acceptance remains open.
- RPD-005: automated checks reject malformed JSON and fields, then accept valid telemetry.
- RPD-002/006: automated checks cover publication-failure spool preservation, crash recovery and v3 session reset; remaining acceptance criteria stay open.
- RPD-012: removed the seven-second rc.local delay and failed fbcp launch, preserving the original and console mapping. Xorg uses /dev/fb0 directly. Cold-boot measurement remains pending.
