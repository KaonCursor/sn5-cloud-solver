# Event Archives

yaml-cpp can record parsed documents as immutable *event tapes*. A tape is the
exact list of `EventHandler` callbacks that `Parser::HandleNextDocument`
produced for one document. An `EventArchive` keeps complete tapes under a
scalar-payload quota. It can save and restore portable checkpoints, replay the
callbacks in bounded chunks, rebuild `Node` graphs, emit YAML, and inspect paths
through aliases without building a `Node` tree.

Both headers are included by `yaml-cpp/yaml.h`:

```cpp
#include "yaml-cpp/eventtape.h"     // ArchiveEvent, EventTape, EventCursor
#include "yaml-cpp/eventarchive.h"  // ArchiveNodeInfo, EventArchive
```

## Event records

```cpp
struct ArchiveEvent {
  enum class Kind { DocumentStart = 0, DocumentEnd = 1, Null = 2, Alias = 3,
                    Scalar = 4, SequenceStart = 5, SequenceEnd = 6,
                    MapStart = 7, MapEnd = 8, Anchor = 9 };
  Kind kind;
  Mark mark = Mark::null_mark();
  anchor_t anchor = 0;
  std::string tag;
  std::string value;
  EmitterStyle::value style = EmitterStyle::Default;
};
```

Each record describes one callback. Fields the callback does not use keep their
defaults:

| Kind | Meaningful fields |
| --- | --- |
| DocumentStart | `mark` |
| DocumentEnd, SequenceEnd, MapEnd | none (`mark` is `null_mark()`) |
| Null | `mark`, `anchor` |
| Alias | `mark`, `anchor` (the numeric anchor it references) |
| Scalar | `mark`, `anchor`, `tag`, `value` |
| SequenceStart, MapStart | `mark`, `anchor`, `tag`, `style` |
| Anchor | `mark`, `value` (the original textual anchor name) |

**Anchor names and anchor IDs.** The textual name from `OnAnchor` (for
example `&base`) is callback metadata. It is kept so replays deliver it
unchanged. Reference identity comes from the *numeric* `anchor_t`. Within one
document, definitions are numbered consecutively from one, and an alias names
the numeric ID of its definition. Two definitions may use the same textual
name. Every consumer here (NodeBuilder, the emitter, and `Inspect`) resolves
aliases through the numeric ID only.

## EventTape

```cpp
static std::shared_ptr<const EventTape> EventTape::Capture(Parser& parser);
static std::shared_ptr<const EventTape> EventTape::FromRecords(std::vector<ArchiveEvent> records);
const std::vector<ArchiveEvent>& records() const;
std::size_t size() const;
```

* `Capture` calls `parser.HandleNextDocument` exactly once, using an internal
  recorder. At end of input it returns a null pointer. If the parser throws,
  the exception propagates and no partial tape is returned. The recorder keeps
  every callback, including `OnAnchor`, with its original marks, tags, values,
  styles, numeric anchors, and map entry order.
* `FromRecords` validates the records and throws
  `std::runtime_error("event archive: invalid tape")` unless they describe
  exactly one document. That means:
  * one DocumentStart first, one root node, and one DocumentEnd last, with
    nothing after it;
  * collection ends that match their starts and are properly nested;
  * an even number of children in every map;
  * valid kind and style values, and default values in every unused field;
  * anchor definitions numbered 1, 2, 3, … in document order;
  * each Anchor record has a nonempty name and comes immediately before the
    Null, Scalar, SequenceStart, or MapStart that defines it;
  * each alias references a definition that has already started. A
    collection may therefore contain an alias to itself (a recursive alias is
    legal).
* Construction is private, so a validated tape can never be modified. Tapes are
  shared through `std::shared_ptr` and live as long as any owner holds them.

## EventCursor: resumable replay

```cpp
explicit EventCursor(std::shared_ptr<const EventTape> tape);  // null throws
bool Pump(EventHandler& handler, std::size_t max_nodes);     // true when done
bool Done() const;
```

Each cursor holds its own reference to the tape and its own position, so
several cursors can replay one tape independently. Callback payloads are
delivered unchanged.

Budget semantics:

* Scalar, Null, Alias, SequenceStart, and MapStart each cost **one node**.
  DocumentStart, DocumentEnd, SequenceEnd, MapEnd, and Anchor are free.
* `Pump(h, 0)` performs no callbacks at all.
* A positive `Pump(h, n)` emits DocumentStart (on the first call), then at
  most `n` node callbacks. It then *drains* any SequenceEnd, MapEnd, and
  DocumentEnd callbacks that follow, stopping at the next node group.
* **An Anchor callback and the definition that follows it are always delivered
  in the same positive Pump.** An Anchor is never emitted unless the same call
  can also emit its defining node. At a budget boundary, a pending Anchor waits
  for the next call.
* The return value says whether replay is complete. A finished cursor stays
  finished: later calls return `true` and emit nothing.
* Keep using the same compatible handler for a cursor. If a handler callback
  throws, discard the cursor.

Partial replay example:

```cpp
YAML::EventCursor cursor = archive.Replay(id);
MyHandler handler;
while (!cursor.Pump(handler, 64)) {
  // do other work between chunks of at most 64 nodes
}
```

For `[a, [b, c]]` and a budget of 2, the first Pump delivers DocumentStart,
SequenceStart, and `a`. The second delivers SequenceStart and `b`. The third
delivers `c`, then drains SequenceEnd, SequenceEnd, and DocumentEnd.

## EventArchive

```cpp
explicit EventArchive(std::size_t max_scalar_bytes);
bool AppendNext(Parser& parser, std::uint64_t& assigned_id);
std::vector<std::uint64_t> Documents() const;   // retained IDs, oldest first
std::size_t UsedScalarBytes() const;
EventCursor Replay(std::uint64_t id) const;
Node Build(std::uint64_t id) const;
std::string Emit(std::uint64_t id) const;
std::string Save() const;
static EventArchive Restore(const std::string& checkpoint);
ArchiveNodeInfo Inspect(std::uint64_t id, const std::string& pointer) const;
```

### Stable IDs and FIFO retention

Document IDs start at zero and increase by one for each admitted document. An
ID never changes when other documents are evicted, and it is never reused.
Retained documents form a FIFO queue. To make room, the archive evicts the
oldest *complete* documents. It never truncates a document.

`AppendNext` works as a transaction:

1. It captures one complete document from the parser before changing any
   archive state. At end of input it returns `false` and leaves `assigned_id`
   unchanged. If the parser throws, the exception propagates and the archive is
   unchanged.
2. It computes the document cost. If the cost exceeds `max_scalar_bytes`, it
   throws `std::runtime_error("event archive: document exceeds scalar byte
   limit")`. Nothing is evicted and no ID is consumed. **The parser has still
   consumed the rejected document**, so the next `AppendNext` reads the
   following document.
3. If the cost or ID arithmetic would overflow, it throws `std::runtime_error`
   and leaves the state unchanged.
4. Otherwise it evicts the oldest documents until `used + cost <= limit`, then
   stores the tape, assigns `next_id`, and returns `true`.

A cursor obtained from `Replay` keeps its tape alive even after that document is
evicted. Operations on an ID that is not retained throw
`std::runtime_error("event archive: document not retained: <decimal id>")`.

### Quota arithmetic

A document's cost is the checked sum of `value.size()` over **every Scalar
event** in its tape:

* Mapping keys count too.
* **Repeated equal scalar occurrences are charged separately**, even if an
  implementation interns or shares their storage.
* **Aliases have no expanded payload cost.** An alias costs zero no matter how
  large its target is.
* Nulls, anchor names, tags, and structural events cost zero.
* Bytes are the bytes of the decoded string, including embedded NUL and every
  UTF-8 byte. Source spelling (quotes, escapes) and Unicode character counts
  don't matter. For example, `"\0é"` costs 3 bytes.

A document whose cost equals the limit fits. A zero limit accepts documents with
no scalar payload, such as `[~, ~]` or `{}`. The quota bounds scalar payload
bytes only. **It is not a total-memory bound.**

Bounded retention example:

```cpp
YAML::EventArchive archive(40);
std::istringstream in("a: 1111111111\n---\nb: 2222222222\n---\nc: 3333333333333333333333333\n");
YAML::Parser parser(in);
std::uint64_t id;
archive.AppendNext(parser, id);   // id 0, cost 11
archive.AppendNext(parser, id);   // id 1, cost 11, used 22
archive.AppendNext(parser, id);   // id 2, cost 26: 22 + 26 > 40, evicts 0
// archive.Documents() == {1, 2}, archive.UsedScalarBytes() == 37
```

### Build and Emit

`Build` replays the whole document into a fresh `NodeBuilder`. It preserves
alias identity, so shared and recursive structures come back as shared nodes.
`Emit` replays the whole document into a fresh `Emitter` through
`EmitFromEvents`, checks `emitter.good()`, and returns one YAML document. The
emitted text does not promise to keep comments, original formatting, or
textual anchor names.

### Checkpoints

`Save` returns a YAML mapping with exactly these keys and does not change the
archive. The example below is illustrative and uses flow sequences for
brevity. `Save` itself emits block style, and `Restore` accepts either:

```yaml
version: 1
max_scalar_bytes: 40
next_id: 3
documents:
  - id: 2
    events:
      - [0, 0, 0, 0, 0, "", "", 0]         # DocumentStart
      - [7, 0, 0, 0, 0, "?", "", 1]        # MapStart, block style
      - [4, 0, 0, 0, 0, "?", "c", 0]       # Scalar
      - [4, 3, 0, 3, 0, "?", "333…", 0]    # Scalar
      - [8, -1, -1, -1, 0, "", "", 0]      # MapEnd (null mark)
      - [1, -1, -1, -1, 0, "", "", 0]      # DocumentEnd
```

Each event is an eight-element sequence
`[kind, pos, line, column, anchor, tag, value, style]`. Tag and value are
always written as strings.

| kind | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| event | DocumentStart | DocumentEnd | Null | Alias | Scalar | SequenceStart | SequenceEnd | MapStart | MapEnd | Anchor |

| style | 0 | 1 | 2 |
| --- | --- | --- | --- |
| EmitterStyle | Default | Block | Flow |

`Restore` loads the text with `YAML::Load` and then:

* validates the schema and numeric ranges, rejecting unknown or missing fields;
* rebuilds each tape through `EventTape::FromRecords`;
* recomputes each cost with the same formula (cached usage is not
  serialized);
* restores the exact retained IDs and `next_id`.

The IDs must be strictly increasing and less than `next_id`, and the total
retained cost must fit the saved limit. Checkpoint documents are never evicted
silently. An empty archive may have a nonzero `next_id`. Any violation throws
`std::runtime_error("event archive: invalid checkpoint")`. After a round trip,
every event field and all later admission behavior are unchanged.

Checkpoint restart example:

```cpp
std::string saved = archive.Save();
// ... process restarts ...
YAML::EventArchive resumed = YAML::EventArchive::Restore(saved);
// resumed.Documents() == archive.Documents(); the next AppendNext assigns
// the same ID the original archive would have assigned.
```

## Inspect: path queries without a Node graph

```cpp
struct ArchiveNodeInfo {
  NodeType::value type;  // Null, Scalar, Sequence or Map
  Mark mark;
  std::string tag;       // scalar and collection events only
  std::string scalar;    // Scalar only
  EmitterStyle::value style;  // collections only, otherwise Default
};
```

`Inspect(id, pointer)` works directly on the tape's records. It builds a local
structural index that maps each collection start to its matching end, its
direct children, and each numeric anchor to its definition. Anchor records are
ignored when counting children. The cost is linear in the tape size plus the
work of the path itself. Inspect never expands aliases, so it terminates on
recursive documents.

**Pointer syntax** follows JSON Pointer (RFC 6901) tokenization:

* The empty string selects the root. Any other pointer must start with `/`.
* The pointer is split on `/` *before* decoding. Each token is then decoded
  once: `~0` becomes `~` and `~1` becomes `/`. Any other tilde escape,
  including a trailing `~`, is an invalid pointer. So `/~01` selects the key
  `~1`, not `/`, and `/a~1b` selects the single key `a/b`. A lone `/` selects
  the empty key.
* In a sequence, a token must be the canonical decimal `0` or `[1-9][0-9]*`,
  converted with overflow checks. An index too large to represent is out of
  range.
* In a map, a token is compared byte for byte with decoded scalar key values,
  regardless of tag. An alias key is followed to its definition and matches if
  that definition is a scalar. Null keys and collection keys are ignored. If
  several scalar keys match, the request is ambiguous, but other unique keys in
  the same map can still be inspected.

**Aliases** are dereferenced at every step and for the final result, always
through the numeric anchor ID. Metadata comes from the resolved *defining*
event, including its mark. **A finite pointer may pass through a recursive alias
as often as it likes, as long as each revisit consumes path tokens.** As a
defensive check, resolution records each visited (definition offset, tokens
consumed) pair and reports `alias cycle` if a pair repeats. There is no
arbitrary alias-depth limit.

Escaped-token example, for the document `{"a/b": 1, "~1": 2, "": 3}`:

```cpp
archive.Inspect(id, "/a~1b").scalar;  // "1"
archive.Inspect(id, "/~01").scalar;   // "2"
archive.Inspect(id, "/").scalar;      // "3"
```

Recursive-reference example, for the document `&r {value: ok, next: *r}`:

```cpp
archive.Inspect(id, "/next").type;         // NodeType::Map (the root itself)
archive.Inspect(id, "/next/value").scalar; // "ok"
```

**Errors.** An ID that is not retained raises the `document not retained` error
described above. Every other failure throws
`std::runtime_error("event archive: <reason> at <original pointer>")`. The
pointer is quoted exactly as passed in. The possible reasons are:

| reason | cause |
| --- | --- |
| `invalid pointer` | missing leading `/`, or a bad tilde escape |
| `missing key` | no scalar key in the map equals the token |
| `ambiguous key` | more than one scalar key equals the token |
| `invalid sequence index` | token is not a canonical decimal index |
| `index out of range` | index is at or past the sequence length |
| `not a collection` | tokens remain after reaching a scalar or null |
| `alias cycle` | defensive: a resolution state repeated |
