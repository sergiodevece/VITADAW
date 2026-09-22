# VitaDAW 0.8.2A — Recovery Inventory Foundation

This phase adds a synchronous, control-side inventory for explicitly authorized
filesystem roots. It is discovery only: it does not delete, rename, move,
adopt, publish, repair, or modify media or projects.

`audio::RecoveryCandidate` is the portable description of a discovery snapshot.
The platform-files scanner supplies pathname, metadata, and a stable
device/inode-style identity where the platform exposes one. A pathname is not
the candidate's logical identity. Discovery is not authorization for a later
mutation: any future adopt/delete operation must reacquire and verify identity
immediately before it acts.

The scanner only walks roots supplied by the caller, rejects a symlink root,
does not recurse through symlink entries, and records partial scan failures as
diagnostics. Each WAV probe is tied to a read-only, no-follow descriptor and
the pathname identity is checked again afterwards; media payloads are never
loaded into the inventory. The structural header probe is bounded to 64 KiB.

## Containment contract and follow-up

Authorized roots are supplied explicitly by the caller and must be trusted. A
root that is itself a symlink is rejected, as are discovered symlink/reparse
entries. This is deliberately a read-only discovery contract: 0.8.2A does not
claim canonical physical containment against every possible symlink in an
ancestor component through which an authorized root may have been reached.

A `RecoveryCandidate` remains informative snapshot data, never permission to
mutate a pathname. A future Recover, Delete, Adopt, or GC operation must reopen
and revalidate file identity immediately before acting, and must define and
apply a containment policy appropriate to that mutating operation. Ancestor
symlink containment testing is intentionally deferred to that mutating phase;
it is not a 0.8.2A discovery feature.

Current recognition evidence is deliberately narrow:

- recording candidates require a valid existing VitaDAW recording-recovery
  marker, whose media names are already constrained to the marker directory;
- export temporaries require the exact retained 0.8.1 naming form
  `.<final>.export.<nonce>.part.wav`;
- a retained temporary and final with equal stable identity are reported as a
  published-temporary alias, not as two independent files;
- all other WAV files remain `provenanceUnknown`, even if their names look
  temporary. Unknown provenance never licenses future automatic deletion.

The only WAV validation is RIFF/WAVE recognition, bounded `fmt`/`data` header
inspection, basic mono/stereo PCM/float format coherence, and evident truncation
checks. It is not a decoder or repair facility.

0.8.2A intentionally has no UI, watcher, background work, recovery/adoption,
garbage collection, automatic cleanup, project mutation, or post-crash media
repair. Those capabilities belong to later phases built on identity revalidation
and explicit user authority.
