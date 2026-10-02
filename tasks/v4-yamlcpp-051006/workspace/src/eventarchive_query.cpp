// Graph-aware path inspection of retained documents (wp3).
//
// EventArchive::Inspect answers "what lives at this path?" straight from the
// immutable event tape of a document.  It never materializes a Node graph and
// never expands an alias: a local structural index maps every collection start
// to its matching end, its direct children in callback order and every
// document-local numeric anchor to the offset of the node that defines it.
// Building that index is one linear pass over the tape, so a call costs one
// linear scan plus the work of the requested path itself, and it terminates on
// recursive documents because every traversal step consumes one path token.
//
// Pointer syntax follows JSON Pointer (RFC 6901) tokenization; see
// docs/Event-Archives.md for the exact contract.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "yaml-cpp/eventarchive.h"
#include "yaml-cpp/eventtape.h"

namespace YAML {
namespace {

using Kind = ArchiveEvent::Kind;

// Offsets are record indices inside one tape.
using Offset = std::size_t;

const char kInvalidPointer[] = "invalid pointer";
const char kMissingKey[] = "missing key";
const char kAmbiguousKey[] = "ambiguous key";
const char kInvalidSequenceIndex[] = "invalid sequence index";
const char kIndexOutOfRange[] = "index out of range";
const char kNotACollection[] = "not a collection";
const char kAliasCycle[] = "alias cycle";

[[noreturn]] void ThrowAt(const char* reason, const std::string& pointer) {
  throw std::runtime_error(std::string("event archive: ") + reason + " at " +
                           pointer);
}

bool IsNodeKind(Kind kind) {
  return kind == Kind::Null || kind == Kind::Alias || kind == Kind::Scalar ||
         kind == Kind::SequenceStart || kind == Kind::MapStart;
}

bool IsCollectionStart(Kind kind) {
  return kind == Kind::SequenceStart || kind == Kind::MapStart;
}

bool IsCollectionEnd(Kind kind) {
  return kind == Kind::SequenceEnd || kind == Kind::MapEnd;
}

// ---------------------------------------------------------------------------
// Tokenization
// ---------------------------------------------------------------------------

// Decodes one token in place: ~0 becomes ~ and ~1 becomes /, exactly once.
// Any other tilde escape, including a trailing ~, is an invalid pointer.  The
// token is split on '/' before decoding, so a decoded '/' never creates a new
// token.
void DecodeToken(std::string& token, const std::string& pointer) {
  std::string decoded;
  decoded.reserve(token.size());
  for (std::size_t i = 0; i < token.size(); ++i) {
    if (token[i] != '~') {
      decoded.push_back(token[i]);
      continue;
    }
    if (i + 1 >= token.size())
      ThrowAt(kInvalidPointer, pointer);
    if (token[i + 1] == '0') {
      decoded.push_back('~');
      ++i;
    } else if (token[i + 1] == '1') {
      decoded.push_back('/');
      ++i;
    } else {
      ThrowAt(kInvalidPointer, pointer);
    }
  }
  token.swap(decoded);
}

// Splits `pointer` into decoded tokens.  An empty pointer selects the root and
// yields no tokens at all.
std::vector<std::string> Tokenize(const std::string& pointer) {
  if (pointer.empty())
    return std::vector<std::string>();
  if (pointer[0] != '/')
    ThrowAt(kInvalidPointer, pointer);

  // Split first, decode afterwards.
  std::vector<std::string> raw;
  std::size_t begin = 1;
  for (std::size_t i = 1; i <= pointer.size(); ++i) {
    if (i == pointer.size() || pointer[i] == '/') {
      raw.push_back(pointer.substr(begin, i - begin));
      begin = i + 1;
    }
  }

  for (std::size_t i = 0; i < raw.size(); ++i)
    DecodeToken(raw[i], pointer);
  return raw;
}

// ---------------------------------------------------------------------------
// Structural index
// ---------------------------------------------------------------------------

/**
 * A document-local map from collection starts to their structure.
 *
 * Everything is keyed by record offset, so the index never copies payload and
 * never depends on textual anchor names.
 */
class TapeIndex {
 public:
  explicit TapeIndex(const std::vector<ArchiveEvent>& records) : records_(records) {
    match_end_.assign(records_.size(), 0);
    children_.resize(records_.size());
    root_ = 0;

    std::vector<Offset> open;
    bool have_root = false;

    for (Offset i = 0; i < records_.size(); ++i) {
      const Kind kind = records_[i].kind;

      // Anchor records are metadata for the definition that follows them; they
      // are not children of anything and are ignored when counting.
      if (kind == Kind::Anchor)
        continue;

      if (IsNodeKind(kind)) {
        // Aliases carry the anchor they reference; only definitions count.
        if (kind != Kind::Alias && records_[i].anchor != 0)
          anchors_.insert(std::make_pair(records_[i].anchor, i));
        if (open.empty()) {
          root_ = i;  // exactly one root node per validated tape
          have_root = true;
        } else {
          children_[open.back()].push_back(i);
        }
        if (IsCollectionStart(kind))
          open.push_back(i);
        continue;
      }

      if (IsCollectionEnd(kind)) {
        const Offset start = open.back();
        open.pop_back();
        match_end_[start] = i;
      }
    }

    (void)have_root;  // validated tapes always define exactly one root
  }

  /** The root node record. */
  Offset Root() const { return root_; }

  /** The record a numeric anchor is defined at. */
  bool FindAnchor(anchor_t anchor, Offset& definition) const {
    const auto it = anchors_.find(anchor);
    if (it == anchors_.end())
      return false;
    definition = it->second;
    return true;
  }

  /** The direct child node records of a collection start, in order. */
  const std::vector<Offset>& Children(Offset start) const {
    return children_[start];
  }

  /** The matching end record of a collection start. */
  Offset MatchingEnd(Offset start) const { return match_end_[start]; }

 private:
  const std::vector<ArchiveEvent>& records_;
  std::vector<Offset> match_end_;
  std::vector<std::vector<Offset>> children_;
  std::unordered_map<anchor_t, Offset> anchors_;
  Offset root_;
};

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

// Identity of a resolution state: where we stand and how much of the path we
// have already consumed.  Reaching the same pair twice can only mean that the
// document recurses without consuming tokens, which is reported as a cycle.
struct StateKey {
  Offset offset;
  std::size_t consumed;

  bool operator==(const StateKey& other) const {
    return offset == other.offset && consumed == other.consumed;
  }
};

struct StateKeyHash {
  std::size_t operator()(const StateKey& key) const {
    // Offsets are record indices and consumed counts are token counts; both are
    // far below 2^32 for any realizable tape, but the mix below is exact for
    // anything that fits in 32 bits and still good beyond that.
    std::size_t hash = static_cast<std::size_t>(key.offset) * 0x9E3779B97F4A7C15ULL;
    hash ^= static_cast<std::size_t>(key.consumed) + 0x9E3779B97F4A7C15ULL +
            (hash << 6) + (hash >> 2);
    return hash;
  }
};

class PathResolver {
 public:
  PathResolver(const EventTape& tape, const std::string& pointer)
      : records_(tape.records()),
        pointer_(pointer),
        tokens_(Tokenize(pointer)),
        index_(records_) {}

  ArchiveNodeInfo Resolve() {
    Offset current = index_.Root();

    // The root itself may be an alias.
    current = Deref(current, 0);
    visited_.insert(StateKey{current, 0});

    for (std::size_t consumed = 0; consumed < tokens_.size(); ++consumed) {
      const std::string& token = tokens_[consumed];
      const ArchiveEvent& event = records_[current];

      if (!IsCollectionStart(event.kind))
        ThrowAt(kNotACollection, pointer_);

      const std::vector<Offset>& children = index_.Children(current);
      Offset next = 0;
      if (event.kind == Kind::SequenceStart) {
        next = SequenceChild(children, token);
      } else {
        next = MapValue(children, token);
      }

      // Every step consumes exactly one token, which is what keeps recursion
      // bounded.
      current = Deref(next, consumed + 1);
    }

    return InfoAt(current);
  }

 private:
  /** The node a scalar payload or null resolves to; nothing else is expected. */
  ArchiveNodeInfo InfoAt(Offset offset) const {
    const ArchiveEvent& event = records_[offset];
    ArchiveNodeInfo info;
    info.mark = event.mark;
    switch (event.kind) {
      case Kind::Null:
        info.type = NodeType::Null;
        break;
      case Kind::Scalar:
        info.type = NodeType::Scalar;
        info.tag = event.tag;
        info.scalar = event.value;
        break;
      case Kind::SequenceStart:
        info.type = NodeType::Sequence;
        info.tag = event.tag;
        info.style = event.style;
        break;
      case Kind::MapStart:
        info.type = NodeType::Map;
        info.tag = event.tag;
        info.style = event.style;
        break;
      default:
        break;  // aliases are dereferenced before they are described
    }
    return info;
  }

  /**
   * Follows alias records to the node that defines the referenced numeric
   * anchor.  `consumed` tokens have been read on the way to `offset`.
   */
  Offset Deref(Offset offset, std::size_t consumed) {
    // Definitions are never aliases in a validated tape, so this loop runs
    // once; it is written as a loop so that a hostile tape cannot recurse.
    while (records_[offset].kind == Kind::Alias) {
      const anchor_t anchor = records_[offset].anchor;
      Offset definition = 0;
      if (!index_.FindAnchor(anchor, definition))
        ThrowAt(kAliasCycle, pointer_);
      const StateKey key{definition, consumed};
      if (!visited_.insert(key).second)
        ThrowAt(kAliasCycle, pointer_);
      offset = definition;
    }
    return offset;
  }

  /** Canonical decimal sequence index: 0, or [1-9][0-9]*, checked. */
  std::size_t SequenceChild(const std::vector<Offset>& children,
                            const std::string& token) const {
    if (token.empty())
      ThrowAt(kInvalidSequenceIndex, pointer_);
    if (token[0] == '0') {
      if (token.size() != 1)
        ThrowAt(kInvalidSequenceIndex, pointer_);
    } else if (token[0] < '1' || token[0] > '9') {
      ThrowAt(kInvalidSequenceIndex, pointer_);
    } else {
      for (std::size_t i = 1; i < token.size(); ++i)
        if (token[i] < '0' || token[i] > '9')
          ThrowAt(kInvalidSequenceIndex, pointer_);
    }

    std::size_t index = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
      const unsigned digit = static_cast<unsigned>(token[i] - '0');
      if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10)
        ThrowAt(kIndexOutOfRange, pointer_);
      index = index * 10 + digit;
    }
    if (index >= children.size())
      ThrowAt(kIndexOutOfRange, pointer_);
    return children[index];
  }

  /** Follows an alias key to the scalar it names, if it names one. */
  bool AliasKeyMatches(Offset key_offset, const std::string& token) const {
    Offset definition = key_offset;
    // Alias keys are resolved independently of the traversal state below; a
    // key probe consumes no path token and never recurses.
    while (records_[definition].kind == Kind::Alias) {
      if (!index_.FindAnchor(records_[definition].anchor, definition))
        return false;
    }
    if (records_[definition].kind != Kind::Scalar)
      return false;  // null and collection keys are ignored
    return records_[definition].value == token.c_str();
  }

  /** The value record of `token` in a mapping, or a missing/ambiguous error. */
  Offset MapValue(const std::vector<Offset>& children,
                  const std::string& token) const {
    Offset value = 0;
    bool found = false;
    for (std::size_t i = 0; i + 1 < children.size(); i += 2) {
      const Offset key_offset = children[i];
      const Kind key_kind = records_[key_offset].kind;
      bool matches = false;
      if (key_kind == Kind::Scalar) {
        // Compared byte for byte; the tag of the key never matters.
        matches = records_[key_offset].value == token;
      } else if (key_kind == Kind::Alias) {
        matches = AliasKeyMatches(key_offset, token);
      }
      if (!matches)
        continue;
      if (found)
        ThrowAt(kAmbiguousKey, pointer_);
      found = true;
      value = children[i + 1];
    }
    if (!found)
      ThrowAt(kMissingKey, pointer_);
    return value;
  }

  const std::vector<ArchiveEvent>& records_;
  const std::string& pointer_;
  std::vector<std::string> tokens_;
  TapeIndex index_;
  std::unordered_set<StateKey, StateKeyHash> visited_;
};

}  // namespace

ArchiveNodeInfo EventArchive::Inspect(std::uint64_t id,
                                      const std::string& pointer) const {
  const std::shared_ptr<const EventTape> tape = GetTape(id);
  // A missing document already reported itself through GetTape(); from here on
  // every failure is a path failure, and every message quotes the pointer
  // exactly as it was given.
  PathResolver resolver(*tape, pointer);
  return resolver.Resolve();
}

}  // namespace YAML
