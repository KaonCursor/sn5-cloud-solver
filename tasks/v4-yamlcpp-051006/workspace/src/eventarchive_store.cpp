// Transactional retention, checkpoints and the existing node/emitter adapters
// of the EventArchive subsystem.
//
// wp3 (eventarchive_query.cpp) supplies EventArchive::Inspect and relies on
// GetTape() and on ScalarBytes() below.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "yaml-cpp/eventarchive.h"
#include "yaml-cpp/eventtape.h"

#include "yaml-cpp/emitter.h"
#include "yaml-cpp/emitfromevents.h"
#include "yaml-cpp/exceptions.h"
#include "yaml-cpp/node/convert.h"
#include "yaml-cpp/node/detail/impl.h"
#include "yaml-cpp/node/emit.h"
#include "yaml-cpp/node/iterator.h"
#include "yaml-cpp/node/node.h"
#include "yaml-cpp/node/parse.h"
#include "yaml-cpp/parser.h"

#include "nodebuilder.h"

namespace YAML {
namespace {

using Kind = ArchiveEvent::Kind;

const char kInvalidCheckpoint[] = "event archive: invalid checkpoint";
const char kExceedsLimit[] = "event archive: document exceeds scalar byte limit";

[[noreturn]] void ThrowInvalidCheckpoint() {
  throw std::runtime_error(kInvalidCheckpoint);
}

void Require(bool condition) {
  if (!condition)
    ThrowInvalidCheckpoint();
}

// Checkpoint style codes: Default = 0, Block = 1, Flow = 2.
int StyleCode(EmitterStyle::value style) {
  switch (style) {
    case EmitterStyle::Default:
      return 0;
    case EmitterStyle::Block:
      return 1;
    case EmitterStyle::Flow:
      return 2;
    default:
      ThrowInvalidCheckpoint();
  }
}

EmitterStyle::value StyleFromCode(std::uint64_t code) {
  switch (code) {
    case 0:
      return EmitterStyle::Default;
    case 1:
      return EmitterStyle::Block;
    case 2:
      return EmitterStyle::Flow;
    default:
      ThrowInvalidCheckpoint();
  }
}

bool IsDigits(const std::string& text, std::size_t begin) {
  if (begin >= text.size())
    return false;
  for (std::size_t i = begin; i < text.size(); ++i)
    if (text[i] < '0' || text[i] > '9')
      return false;
  return true;
}

// Reads a non-negative decimal integer.  Anything else - a negative number, a
// float, a string that only looks numeric, a value that overflows - is an
// invalid checkpoint rather than a silently wrapped value.
std::uint64_t ParseUnsigned(const Node& node) {
  Require(node.IsScalar());
  const std::string& text = node.Scalar();
  Require(IsDigits(text, 0));

  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  Require(errno != ERANGE && end == text.c_str() + text.size());
  return static_cast<std::uint64_t>(value);
}

// Reads a signed decimal integer that fits in `int`; marks are signed.
int ParseInt(const Node& node) {
  Require(node.IsScalar());
  const std::string& text = node.Scalar();

  std::size_t begin = 0;
  if (!text.empty() && (text[0] == '-' || text[0] == '+'))
    begin = 1;
  Require(IsDigits(text, begin));

  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  Require(errno != ERANGE && end == text.c_str() + text.size());
  Require(value >= std::numeric_limits<int>::min() &&
          value <= std::numeric_limits<int>::max());
  return static_cast<int>(value);
}

std::string ParseString(const Node& node) {
  Require(node.IsScalar());
  return node.Scalar().c_str();
}

// The four checkpoint fields of a mapping, in a fixed order.
enum Field { kFieldVersion = 0, kFieldLimit, kFieldNextId, kFieldDocuments, kFieldCount };

// Rejects unknown, duplicate or missing fields of a checkpoint mapping.
void CheckFields(const Node& map, const char* const* names, std::size_t count,
                 bool* seen) {
  Require(map.IsMap());
  for (std::size_t i = 0; i < count; ++i)
    seen[i] = false;

  for (const_iterator it = map.begin(); it != map.end(); ++it) {
    const std::string key = ParseString(it->first);
    int index = -1;
    for (std::size_t i = 0; i < count; ++i) {
      if (key == names[i]) {
        index = static_cast<int>(i);
        break;
      }
    }
    Require(index >= 0);
    Require(!seen[index]);
    seen[index] = true;
  }
  for (std::size_t i = 0; i < count; ++i)
    Require(seen[i]);
}

// Rebuilds one tape from an `events` sequence.
std::shared_ptr<const EventTape> ReadTape(const Node& events) {
  Require(events.IsSequence());
  Require(events.size() > 0);

  std::vector<ArchiveEvent> records;
  records.reserve(events.size());

  for (const_iterator it = events.begin(); it != events.end(); ++it) {
    const Node& record = *it;
    Require(record.IsSequence());
    Require(record.size() == 8);

    ArchiveEvent event;
    event.kind = static_cast<Kind>(ParseUnsigned(record[0]));
    event.mark.pos = ParseInt(record[1]);
    event.mark.line = ParseInt(record[2]);
    event.mark.column = ParseInt(record[3]);
    event.anchor = static_cast<anchor_t>(ParseUnsigned(record[4]));
    event.tag = ParseString(record[5]);
    event.value = ParseString(record[6]);
    event.style = StyleFromCode(ParseUnsigned(record[7]));

    records.push_back(std::move(event));
  }

  return EventTape::FromRecords(std::move(records));
}

}  // namespace

// The single cost model of the archive: the checked sum of the byte lengths of
// every scalar payload, including mapping keys, repeated equal scalars, tags
// and aliases.  Aliases, nulls, names and structural events cost nothing.
std::size_t ScalarBytes(const EventTape& tape) {
  const std::vector<ArchiveEvent>& records = tape.records();
  std::size_t total = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].kind != Kind::Scalar)
      continue;
    const std::size_t bytes = records[i].value.size();
    if (bytes > std::numeric_limits<std::size_t>::max() - total)
      throw std::runtime_error("event archive: scalar byte overflow");
    total += bytes;
  }
  return total;
}

EventArchive::EventArchive(std::size_t max_scalar_bytes)
    : max_scalar_bytes_(max_scalar_bytes) {}

bool EventArchive::AppendNext(Parser& parser, std::uint64_t& assigned_id) {
  // Capture the complete document first: nothing below can observe a partially
  // recorded document, and a parser failure above this line changes nothing.
  const std::shared_ptr<const EventTape> tape = EventTape::Capture(parser);
  if (!tape)
    return false;

  const std::size_t cost = ScalarBytes(*tape);

  // Identifier overflow: identifiers are never reused, so the counter running
  // out is a hard error, not a wrap-around.
  if (next_id_ == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("event archive: identifier overflow");

  // Admission is decided before any eviction or identifier assignment.
  if (cost > max_scalar_bytes_)
    throw std::runtime_error(kExceedsLimit);

  // Evict the oldest complete documents until the new document fits.  Equality
  // with the limit fits, and `cost <= max_scalar_bytes_` keeps the subtraction
  // below well defined.
  while (used_scalar_bytes_ > max_scalar_bytes_ - cost) {
    used_scalar_bytes_ -= entries_.front().scalar_bytes;
    entries_.pop_front();  // the tape itself survives in any existing cursor
  }

  const std::uint64_t id = next_id_;
  Entry entry;
  entry.id = id;
  entry.tape = tape;
  entry.scalar_bytes = cost;
  entries_.push_back(entry);
  used_scalar_bytes_ += cost;
  ++next_id_;
  assigned_id = id;
  return true;
}

std::vector<std::uint64_t> EventArchive::Documents() const {
  std::vector<std::uint64_t> ids;
  ids.reserve(entries_.size());
  for (std::size_t i = 0; i < entries_.size(); ++i)
    ids.push_back(entries_[i].id);
  return ids;
}

std::size_t EventArchive::UsedScalarBytes() const { return used_scalar_bytes_; }

EventCursor EventArchive::Replay(std::uint64_t id) const {
  return EventCursor(GetTape(id));
}

Node EventArchive::Build(std::uint64_t id) const {
  NodeBuilder builder;
  EventCursor cursor(GetTape(id));
  // A complete document always replays in one pump of unlimited budget.
  cursor.Pump(builder, std::numeric_limits<std::size_t>::max());
  return builder.Root();
}

std::string EventArchive::Emit(std::uint64_t id) const {
  Emitter emitter;
  {
    EmitFromEvents emit_from_events(emitter);
    EventCursor cursor(GetTape(id));
    if (!cursor.Pump(emit_from_events, std::numeric_limits<std::size_t>::max()))
      throw std::runtime_error("event archive: incomplete replay");
  }
  if (!emitter.good())
    throw std::runtime_error("event archive: emit failed: " +
                             emitter.GetLastError());
  return std::string(emitter.c_str(), emitter.size());
}

std::string EventArchive::Save() const {
  Node out;

  out["version"] = 1;
  out["max_scalar_bytes"] = static_cast<std::uint64_t>(max_scalar_bytes_);
  out["next_id"] = next_id_;

  Node documents(NodeType::Sequence);
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];

    Node document;
    document["id"] = entry.id;

    Node events(NodeType::Sequence);
    const std::vector<ArchiveEvent>& records = entry.tape->records();
    for (std::size_t j = 0; j < records.size(); ++j) {
      const ArchiveEvent& event = records[j];
      Node record(NodeType::Sequence);
      record.push_back(static_cast<int>(event.kind));  // numeric Kind
      record.push_back(event.mark.pos);
      record.push_back(event.mark.line);
      record.push_back(event.mark.column);
      record.push_back(static_cast<std::uint64_t>(event.anchor));
      record.push_back(event.tag);   // string field
      record.push_back(event.value); // string field
      record.push_back(StyleCode(event.style));
      events.push_back(record);
    }

    document["events"] = events;
    documents.push_back(document);
  }

  out["documents"] = documents;

  Emitter emitter;
  emitter << out;
  if (!emitter.good())
    throw std::runtime_error("event archive: save failed: " +
                             emitter.GetLastError());
  return std::string(emitter.c_str(), emitter.size());
}

EventArchive EventArchive::Restore(const std::string& checkpoint) {
  Node root;
  try {
    root = YAML::Load(checkpoint);
  } catch (const std::exception&) {
    ThrowInvalidCheckpoint();
  }

  static const char* kNames[kFieldCount] = {"version", "max_scalar_bytes",
                                            "next_id", "documents"};
  bool seen[kFieldCount];
  CheckFields(root, kNames, kFieldCount, seen);

  Require(ParseUnsigned(root["version"]) == 1);

  const std::uint64_t limit = ParseUnsigned(root["max_scalar_bytes"]);
  Require(limit <= std::numeric_limits<std::size_t>::max());

  const std::uint64_t next_id = ParseUnsigned(root["next_id"]);

  const Node& documents = root["documents"];
  Require(documents.IsSequence());

  std::deque<Entry> entries;
  std::size_t used = 0;
  std::uint64_t previous_id = 0;
  bool first = true;

  for (const_iterator it = documents.begin(); it != documents.end(); ++it) {
    static const char* kDocumentNames[2] = {"id", "events"};
    bool document_seen[2];
    CheckFields(*it, kDocumentNames, 2, document_seen);

    const std::uint64_t id = ParseUnsigned((*it)["id"]);
    Require(first || id > previous_id);  // strictly increasing, never reused
    Require(id < next_id);
    previous_id = id;
    first = false;

    std::shared_ptr<const EventTape> tape;
    try {
      tape = ReadTape((*it)["events"]);
    } catch (const std::exception&) {
      ThrowInvalidCheckpoint();
    }

    const std::size_t cost = ScalarBytes(*tape);
    if (cost > std::numeric_limits<std::size_t>::max() - used)
      ThrowInvalidCheckpoint();
    used += cost;
    // Retained documents are never silently evicted; the whole checkpoint must
    // fit the saved limit.
    Require(used <= static_cast<std::size_t>(limit));

    Entry entry;
    entry.id = id;
    entry.tape = std::move(tape);
    entry.scalar_bytes = cost;
    entries.push_back(entry);
  }

  EventArchive archive(static_cast<std::size_t>(limit));
  archive.entries_ = std::move(entries);
  archive.used_scalar_bytes_ = used;
  archive.next_id_ = next_id;
  return archive;
}

std::shared_ptr<const EventTape> EventArchive::GetTape(
    std::uint64_t id) const {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].id == id)
      return entries_[i].tape;
  }
  throw std::runtime_error("event archive: document not retained: " +
                           std::to_string(id));
}

}  // namespace YAML
