#include <stdexcept>
#include <utility>
#include <vector>

#include "yaml-cpp/eventtape.h"
#include "yaml-cpp/parser.h"

namespace YAML {
namespace {

using Kind = ArchiveEvent::Kind;

ArchiveEvent MakeEvent(Kind kind, const Mark& mark, anchor_t anchor,
                       const std::string& tag, const std::string& value,
                       EmitterStyle::value style) {
  ArchiveEvent event;
  event.kind = kind;
  event.mark = mark;
  event.anchor = anchor;
  event.tag = tag;
  event.value = value;
  event.style = style;
  return event;
}

/** Records every callback of one HandleNextDocument call, verbatim. */
class TapeRecorder : public EventHandler {
 public:
  explicit TapeRecorder(std::vector<ArchiveEvent>& records)
      : records_(records) {}

  void OnDocumentStart(const Mark& mark) override {
    records_.push_back(MakeEvent(Kind::DocumentStart, mark, NullAnchor,
                                 std::string(), std::string(),
                                 EmitterStyle::Default));
  }

  void OnDocumentEnd() override {
    records_.push_back(MakeEvent(Kind::DocumentEnd, Mark::null_mark(),
                                 NullAnchor, std::string(), std::string(),
                                 EmitterStyle::Default));
  }

  void OnNull(const Mark& mark, anchor_t anchor) override {
    records_.push_back(MakeEvent(Kind::Null, mark, anchor, std::string(),
                                 std::string(), EmitterStyle::Default));
  }

  void OnAlias(const Mark& mark, anchor_t anchor) override {
    records_.push_back(MakeEvent(Kind::Alias, mark, anchor, std::string(),
                                 std::string(), EmitterStyle::Default));
  }

  void OnScalar(const Mark& mark, const std::string& tag, anchor_t anchor,
                const std::string& value) override {
    records_.push_back(MakeEvent(Kind::Scalar, mark, anchor, tag, value,
                                 EmitterStyle::Default));
  }

  void OnSequenceStart(const Mark& mark, const std::string& tag,
                       anchor_t anchor, EmitterStyle::value style) override {
    records_.push_back(
        MakeEvent(Kind::SequenceStart, mark, anchor, tag, std::string(), style));
  }

  void OnSequenceEnd() override {
    records_.push_back(MakeEvent(Kind::SequenceEnd, Mark::null_mark(),
                                 NullAnchor, std::string(), std::string(),
                                 EmitterStyle::Default));
  }

  void OnMapStart(const Mark& mark, const std::string& tag, anchor_t anchor,
                  EmitterStyle::value style) override {
    records_.push_back(
        MakeEvent(Kind::MapStart, mark, anchor, tag, std::string(), style));
  }

  void OnMapEnd() override {
    records_.push_back(MakeEvent(Kind::MapEnd, Mark::null_mark(), NullAnchor,
                                 std::string(), std::string(),
                                 EmitterStyle::Default));
  }

  void OnAnchor(const Mark& mark, const std::string& anchor_name) override {
    records_.push_back(MakeEvent(Kind::Anchor, mark, NullAnchor, std::string(),
                                 anchor_name, EmitterStyle::Default));
  }

 private:
  std::vector<ArchiveEvent>& records_;
};

bool IsNodeKind(Kind kind) {
  switch (kind) {
    case Kind::Null:
    case Kind::Alias:
    case Kind::Scalar:
    case Kind::SequenceStart:
    case Kind::MapStart:
      return true;
    default:
      return false;
  }
}

bool IsCollectionStart(Kind kind) {
  return kind == Kind::SequenceStart || kind == Kind::MapStart;
}

bool IsCollectionEnd(Kind kind) {
  return kind == Kind::SequenceEnd || kind == Kind::MapEnd;
}

bool IsEndKind(Kind kind) {
  return kind == Kind::DocumentEnd || IsCollectionEnd(kind);
}

bool IsValidKind(int kind) { return kind >= 0 && kind <= static_cast<int>(Kind::Anchor); }

bool IsValidStyle(EmitterStyle::value style) {
  return style == EmitterStyle::Default || style == EmitterStyle::Block ||
         style == EmitterStyle::Flow;
}

void Reject() { throw std::runtime_error("event archive: invalid tape"); }

void Require(bool condition) {
  if (!condition)
    Reject();
}

/** Rejects records whose unused fields were not left at their defaults. */
void ValidateFieldDefaults(const ArchiveEvent& event) {
  Require(IsValidKind(static_cast<int>(event.kind)));

  switch (event.kind) {
    case Kind::DocumentStart:
      Require(event.anchor == 0 && event.tag.empty() && event.value.empty() &&
              event.style == EmitterStyle::Default);
      break;
    case Kind::DocumentEnd:
    case Kind::SequenceEnd:
    case Kind::MapEnd:
      Require(event.mark.is_null() && event.anchor == 0 && event.tag.empty() &&
              event.value.empty() && event.style == EmitterStyle::Default);
      break;
    case Kind::Null:
    case Kind::Alias:
      Require(event.tag.empty() && event.value.empty() &&
              event.style == EmitterStyle::Default);
      break;
    case Kind::Scalar:
      Require(event.style == EmitterStyle::Default);
      break;
    case Kind::SequenceStart:
    case Kind::MapStart:
      Require(IsValidStyle(event.style) && event.value.empty());
      break;
    case Kind::Anchor:
      Require(event.anchor == 0 && event.tag.empty() && !event.value.empty() &&
              event.style == EmitterStyle::Default);
      break;
    default:
      Reject();
      break;
  }
}

/**
 * Rejects anything that is not exactly one well-formed document: one document
 * start, one root node, one document end, nothing else at the top level,
 * balanced and correctly typed collection ends, an even number of children for
 * every map, anchors numbered consecutively from one and aliases that only
 * point at definitions that have already started.
 */
void ValidateRecords(const std::vector<ArchiveEvent>& records) {
  Require(!records.empty());
  Require(records.front().kind == Kind::DocumentStart);
  Require(records.back().kind == Kind::DocumentEnd);

  // Numbers of open collections; parallel to `open_kinds`.
  std::vector<std::size_t> children;
  std::vector<Kind> open_kinds;
  std::size_t root_nodes = 0;
  std::size_t defined_anchors = 0;
  bool expecting_definition = false;

  for (std::size_t i = 0; i < records.size(); ++i) {
    const ArchiveEvent& event = records[i];
    ValidateFieldDefaults(event);

    if (expecting_definition) {
      // An Anchor record must immediately precede the scalar, null or
      // collection start it names; an alias is never a definition.
      Require(event.kind == Kind::Null || event.kind == Kind::Scalar ||
              IsCollectionStart(event.kind));
      Require(event.anchor == defined_anchors);
      expecting_definition = false;
    } else if (event.kind == Kind::Anchor) {
      Require(i + 1 < records.size());
      ++defined_anchors;
      expecting_definition = true;
      continue;
    } else if (IsNodeKind(event.kind) && event.kind != Kind::Alias) {
      // A definition carrying a numeric anchor without a preceding Anchor
      // record is meaningless; aliases are checked through the anchor table.
      Require(event.anchor == 0);
    }

    if (event.kind == Kind::DocumentStart) {
      Require(i == 0);
      continue;
    }

    if (event.kind == Kind::DocumentEnd) {
      Require(i + 1 == records.size());
      Require(children.empty());
      Require(root_nodes == 1);
      Require(!expecting_definition);
      continue;
    }

    if (IsEndKind(event.kind)) {
      Require(!children.empty());
      const Kind open = open_kinds.back();
      Require(IsCollectionStart(open) &&
              (open == Kind::SequenceStart) == (event.kind == Kind::SequenceEnd));
      if (open == Kind::MapStart)
        Require(children.back() % 2 == 0);
      children.pop_back();
      open_kinds.pop_back();
      continue;
    }

    Require(IsNodeKind(event.kind));

    if (event.kind == Kind::Alias)
      Require(event.anchor >= 1 && event.anchor <= defined_anchors);

    if (children.empty()) {
      Require(root_nodes == 0);
      ++root_nodes;
    } else {
      ++children.back();
    }

    if (IsCollectionStart(event.kind)) {
      children.push_back(0);
      open_kinds.push_back(event.kind);
    }
  }

  Require(!expecting_definition);
  Require(root_nodes == 1);
  Require(children.empty());
}

/**
 * The single budget-boundary predicate of the replay.
 *
 * `emitted` node callbacks have already been produced by the current Pump and
 * the budget allows `max_nodes`.  While the budget still has room every record
 * may be emitted.  At the boundary only the free records that close the node
 * group just finished may be drained: collection ends and the document end.
 * An Anchor record names the node that follows it, so it is never split from
 * that node and is therefore refused at a boundary.
 */
bool CanEmitAtBoundary(const ArchiveEvent& event, std::size_t emitted,
                       std::size_t max_nodes) {
  if (emitted < max_nodes)
    return true;
  if (IsNodeKind(event.kind))
    return false;
  if (event.kind == Kind::Anchor)
    return false;
  return IsEndKind(event.kind) || event.kind == Kind::DocumentStart;
}

void ReplayEvent(EventHandler& handler, const ArchiveEvent& event) {
  switch (event.kind) {
    case Kind::DocumentStart:
      handler.OnDocumentStart(event.mark);
      return;
    case Kind::DocumentEnd:
      handler.OnDocumentEnd();
      return;
    case Kind::Null:
      handler.OnNull(event.mark, event.anchor);
      return;
    case Kind::Alias:
      handler.OnAlias(event.mark, event.anchor);
      return;
    case Kind::Scalar:
      handler.OnScalar(event.mark, event.tag, event.anchor, event.value);
      return;
    case Kind::SequenceStart:
      handler.OnSequenceStart(event.mark, event.tag, event.anchor, event.style);
      return;
    case Kind::SequenceEnd:
      handler.OnSequenceEnd();
      return;
    case Kind::MapStart:
      handler.OnMapStart(event.mark, event.tag, event.anchor, event.style);
      return;
    case Kind::MapEnd:
      handler.OnMapEnd();
      return;
    case Kind::Anchor:
      handler.OnAnchor(event.mark, event.value);
      return;
    default:
      throw std::runtime_error("event archive: invalid tape");
  }
}

}  // namespace

std::shared_ptr<const EventTape> EventTape::Capture(Parser& parser) {
  // Exactly one HandleNextDocument call; if it throws, nothing is retained and
  // the exception reaches the caller without a partial tape.
  std::vector<ArchiveEvent> records;
  TapeRecorder recorder(records);
  if (!parser.HandleNextDocument(recorder))
    return std::shared_ptr<const EventTape>();

  return std::shared_ptr<const EventTape>(new EventTape(std::move(records)));
}

std::shared_ptr<const EventTape> EventTape::FromRecords(
    std::vector<ArchiveEvent> records) {
  ValidateRecords(records);
  return std::shared_ptr<const EventTape>(new EventTape(std::move(records)));
}

EventCursor::EventCursor(std::shared_ptr<const EventTape> tape)
    : tape_(std::move(tape)), position_(0) {
  if (!tape_)
    throw std::runtime_error("event archive: null tape");
}

EventCursor::~EventCursor() = default;

bool EventCursor::Done() const {
  return !tape_ || position_ >= tape_->records().size();
}

bool EventCursor::Pump(EventHandler& handler, std::size_t max_nodes) {
  const std::vector<ArchiveEvent>& records = tape_->records();
  if (position_ >= records.size())
    return true;
  if (max_nodes == 0)
    return false;

  std::size_t emitted = 0;
  while (position_ < records.size()) {
    const ArchiveEvent& event = records[position_];
    if (!CanEmitAtBoundary(event, emitted, max_nodes))
      break;
    ReplayEvent(handler, event);
    if (IsNodeKind(event.kind))
      ++emitted;
    ++position_;
  }

  return position_ >= records.size();
}

}  // namespace YAML
