Experimental CREATE Cache Tokens
===============================

This prototype adds an opt-in CREATE context to the Linux SMB client and
ksmbd. It is not an assigned SMB extension or an Azure Files feature.
Do not deploy it on production data before completing the validation below.

Contract
--------

The server returns a token for a particular file incarnation and data version,
captured while read-caching protection is granted. Equal tokens must identify
equal file contents and length. Tokens may change for metadata-only updates;
this costs a cache miss but does not compromise correctness.

Every permitted mutation must participate in both version tracking and SMB
lease coordination. A storage service supporting REST must cover REST range
writes, clears, resize, copy, replacement, and restore, as well as SMB writes.
The Linux client does not poll REST. Azure Files requires service-side support
for this contract; these ksmbd changes do not implement that support.

After an actual close, no lease protects retained bytes. A new successful
open must supply a matching token and fresh read-caching rights before those
bytes can be reused. A matching token never overrides a write, lease-epoch
purge, reconnect invalidation, or an existing invalid-mapping condition.
Existing strict-cache read and lease-break processing remain in force.

Wire Format
-----------

The experimental context name is the following 16-byte sequence::

  a8 31 2d 77 45 9b 4f 02 91 c8 6a 34 e5 70 b2 19

The request has no payload: ``DataOffset=0`` and ``DataLength=0``. The
response has ``DataLength=32`` and contains only ``__u8 Token[32]``. There
is no payload version, flags, or duplicate length. The client compares all
32 bytes without interpreting them. Every value, including all-zero bytes,
is valid; presence is tracked separately from the token bytes.

Absent and unknown contexts use ordinary caching. Malformed or duplicate
recognized response contexts fail parsing; partially parsed tokens are not
published. Unsolicited tokens are not used. No existing CREATE fields are
repurposed, and no dialect, READ, WRITE, CLOSE, or lease-break format changes.

The earlier 80-byte experiment used a different context name and is not
recognized as this extension. Old clients do not request the new context;
old servers can ignore it. Incompatible future formats or semantics require
a new identifier, not reinterpretation of these 32 opaque bytes.

This identifier and layout require protocol review and coordination before
interoperable deployment. They are intentionally experimental.

ksmbd Requirements
------------------

Load the patched ksmbd module with ``cache_tokens_smb_only=1`` in a dedicated
test environment. This read-only module parameter defaults to false. Enabling
it asserts that all mutations of every exported filesystem go through this
ksmbd instance. Local writes, writable mmap, NFS, another SMB server instance,
and storage rollback without restarting ksmbd violate that assertion.

Set ``smb2 leases = yes`` in the global ksmbd configuration. Some versions of
ksmbd-tools disable leases by default; ordinary oplocks are not sufficient
for this prototype's server-side token eligibility check.

The backing filesystem must expose ``STATX_CHANGE_COOKIE`` and a 16-byte
filesystem UUID. Kernel-managed inode versioning must be enabled where the
filesystem requires it; for example, ext4 may require the ``iversion`` mount
option. Merely supporting timestamps is not sufficient. Unsupported backing
filesystems receive no token, even when the module option is enabled.

The server returns tokens only for existing regular, non-stream files opened
without write/append data access and granted a read-caching lease. Durable
reconnect/replay does not establish a token in this prototype. The random
epoch changes on ksmbd initialization/reset and therefore after a server
reboot. Persistent continuity across server restart, failover, filesystem
rollback, or a cloned filesystem is not provided. Restart ksmbd after changing
its backing storage or restoring a snapshot.

ksmbd computes SHA-256 over a packed metadata snapshot: the 16-byte server
epoch, 16-byte filesystem UUID, little-endian 64-bit inode number, change
cookie, ctime seconds, and size, followed by little-endian 32-bit ctime
nanoseconds and inode generation. It does not hash file contents. Token
generation is server-specific; clients must not depend on this construction.
The post-lease snapshot must agree with the CREATE response EOF, otherwise
ksmbd omits the token. Legacy SMB writers still change the metadata snapshot
and participate in lease coordination without knowing about this extension.

Client Setup
------------

Use a patched kernel/module built for the running kernel. For persistent
caching, enable ``CONFIG_CIFS_FSCACHE``, ``CONFIG_FSCACHE`` and
``CONFIG_CACHEFILES`` and configure cachefilesd with a persistent local cache
directory. Start cachefilesd before populating the SMB cache. Do not put its
cache directory on the SMB mount or in temporary storage.

Example mount options for a reader are::

  vers=3.1.1,cache=strict,fsc,cachetokens,serverino,closetimeo=0

Use normal credentials and authorization. ``closetimeo=0`` is useful for
testing actual CLOSE/reopen behavior; a nonzero timeout remains supported.
Never disable signing or encryption just to use this extension.

``cachetokens`` is optional and may be negated with ``nocachetokens`` on the
initial mount. It cannot be toggled on remount, and token mounts cannot switch
away from strict caching. Token and ordinary mounts do not share a superblock.
Without ``fsc`` the feature applies only to retained memory pages.

The disk-cache lookup is delayed until the first open. A token-bearing first
open uses a separate inode-cache namespace and persists the token as coherency
data with a separate validity marker. The compact-token key uses a different
namespace from both timestamp-only objects and the earlier 80-byte tokens;
old objects cannot validate a new-format open. An invalid record is distinct
from a valid all-zero token. Ordinary mounts retain their existing coherency
format.

This first implementation optimizes read-only handles. A local writable open
disables token reuse for that inode until it is evicted; normal writes remain
supported. If the first open cannot establish a token, the inode keeps the
ordinary disk-cache format until eviction. Subsequent opens still validate
tokens before retaining memory pages. CacheFiles is opportunistic: culling,
partial population, errors, and changed volume identity can all cause misses.
This is not offline access or a guarantee that every byte survives reboot.

Validation
----------

Use a disposable share, a separately mounted writer, and a reader with
``closetimeo=0``. The writer must use a distinct client/superblock, for example
a second mount with ``nosharesock``, so the test exercises server lease breaks
rather than merely invalidating the reader's inode through local writes.

Required scenarios are:

* Read, actual CLOSE, and reopen unchanged data. Expect identical bytes and
  no new SMB READ for ranges retained in memory.
* Evict the client inode or unmount/remount without deleting the local cache.
  Reopen unchanged data and verify CacheFiles reads, rather than SMB READs,
  for populated ranges.
* Populate the cache, close the file, and cleanly reboot only the client.
  Start cachefilesd, remount, and verify unchanged bytes and disk-cache hits.
* Overwrite with different same-length contents through the writer and restore
  mtime. The reader must see the new contents, both after close and while a
  reader handle remains open.
* Truncate, extend, replace the file, and read previously unpopulated ranges.
* Change data while the client is down, then verify new data after reboot.
* Restart/reset ksmbd. Old epoch objects must not be reused.
* Disable server tokens, remove read-cache rights, use a writable local handle,
  force reconnect, and exercise malformed contexts. No token-based reuse is
  permitted without the required conditions.

The ``smb2_cachetoken`` KUnit suite covers nonzero and all-zero tokens,
absent and old-format contexts, duplicates, and malformed payload lengths
and bounds. Enable ``CONFIG_KUNIT`` and
``CONFIG_SMB_KUNIT_TESTS`` to run it with the SMB client.

Content checks alone do not prove a cache hit. Compare SMB READ counters in
``/proc/fs/cifs/Stats`` and use the existing FS-Cache/CacheFiles trace events
to distinguish memory hits, disk hits and network reads. With CIFS debug
logging enabled, ``cache token matched/changed/unavailable`` reports the
in-memory token decision; an initial ``changed`` message does not rule out
a subsequent successful persistent-cache lookup.
