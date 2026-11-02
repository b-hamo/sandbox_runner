# Output acquisition before termination

`artifact.output.v1` adds a bounded, locked Output manifest to the existing
`artifact.export.v1` HTTPS upload contract. Both capabilities must be negotiated.
The integrated Host requires both; an old Runner fails startup verification.

`OUTPUT_SNAPSHOT_REQUEST` has no task/action ID and uses normal authenticated
Control session/generation/connection binding. Payload:

```json
{"snapshot_id":"0a50d134-3e37-46e5-bf12-a17b51b5c4ae","operation":"CAPTURE","max_file_bytes":52428800,"max_total_bytes":209715200}
```

Operations are `CAPTURE`, `VERIFY`, `RELEASE`. CAPTURE walks the whole Output tree,
including existing files and directories moved into it. It rejects reparse
points, alternate streams, hardlinks, unavailable files and exceeded limits.
It holds source file handles denying writers/deletion until RELEASE or shutdown.
Work runs on the bounded artifact worker, not the Control receive thread.

The correlated `OUTPUT_SNAPSHOT_RESULT` has status OK, null error, and payload:

```json
{"snapshot_id":"0a50d134-3e37-46e5-bf12-a17b51b5c4ae","operation":"CAPTURE","complete":true,"problem":null,"files":[{"candidate_event_id":"b2a25dde-29fd-4acd-9ea9-0cf372264a21","relative_path":"result.bin","size_bytes":5,"sha256":"a7937b64b8caa58f03721bb6bacf5c78cb235febe0e70b1b84cd99541461a08e"}]}
```

Domain failures set `complete=false`, a nonempty `problem`, and an empty `files`
array. They must never be interpreted as a successfully acquired empty folder.
Only CAPTURE returns files. VERIFY compares the complete current path list and
held identities; RELEASE unlocks the matching snapshot. Wrong snapshot IDs,
overlapping uploads/snapshots, missing channels and repeated CAPTURE are refused.
No implicit retry, grant reuse or snapshot replacement occurs.

Limits: 4,096 files/directories during traversal, 50 MiB per file, 200 MiB per
manifest, 48 KiB serialized result payload (within the 64 KiB Control frame).
An overlong manifest fails explicitly rather than truncating its file list.

The Runner hashes each pinned file while writers are denied. The Host may reuse
an identically named/sized/hashed verified receipt or a matching prior disposition;
the Guest digest is never a scan verdict. Otherwise it uploads the candidate through
ordinary one-use HTTPS grants, checks received bytes against the manifest, then requests
VERIFY before normal TERMINATE. Metadata and a snapshot response are not malware
inspection or publication approval. Host scanner and user dialogs remain required.
Files created after the final verification point cannot be promised by this
snapshot; forced VM destruction cannot recover bytes never sent to the Host.

The Runner also creates `Desktop/Output.lnk`, targeting the actual session Output.
It is a shortcut, not another directory or writable Host mapping. A pre-existing
unrelated shortcut is refused rather than overwritten.
