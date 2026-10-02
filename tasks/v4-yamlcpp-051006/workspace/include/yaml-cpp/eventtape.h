#ifndef YAML_CPP_EVENTTAPE_H_
#define YAML_CPP_EVENTTAPE_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "yaml-cpp/anchor.h"
#include "yaml-cpp/dll.h"
#include "yaml-cpp/emitterstyle.h"
#include "yaml-cpp/eventhandler.h"
#include "yaml-cpp/mark.h"

namespace YAML {

class Parser;

/**
 * A single, self-contained record of one event handler callback.
 *
 * Every field that a given callback does not use keeps its default value, so a
 * record is byte-for-byte reproducible from the callback it describes:
 *   - DocumentStart: only `mark` is meaningful.
 *   - DocumentEnd, SequenceEnd, MapEnd: nothing is meaningful; `mark` is
 *     null_mark().
 *   - Null: `mark` and `anchor`.
 *   - Alias: `mark` and `anchor` (the numeric id of the referenced anchor).
 *   - Scalar: `mark`, `anchor`, `tag` and `value`.
 *   - SequenceStart, MapStart: `mark`, `anchor`, `tag` and `style`.
 *   - Anchor: `mark` and `value` (the original textual anchor name).
 */
struct YAML_CPP_API ArchiveEvent {
  /// Stable, serialized identifiers for the recorded callback kinds.
  enum class Kind {
    DocumentStart = 0,
    DocumentEnd = 1,
    Null = 2,
    Alias = 3,
    Scalar = 4,
    SequenceStart = 5,
    SequenceEnd = 6,
    MapStart = 7,
    MapEnd = 8,
    Anchor = 9,
  };

  Kind kind = Kind::Null;
  Mark mark = Mark::null_mark();
  anchor_t anchor = 0;
  std::string tag;
  std::string value;
  EmitterStyle::value style = EmitterStyle::Default;
};

/**
 * An immutable tape of the callbacks produced by exactly one parsed document.
 *
 * A tape can only be created through Capture() or FromRecords(); construction
 * is private and no accessor mutates the record list, so once a tape has been
 * validated it can never change.  Tapes are shared between cursors through
 * std::shared_ptr.
 */
class YAML_CPP_API EventTape {
 public:
  /**
   * Captures the next document of `parser` as a tape, using exactly one
   * HandleNextDocument call.  Returns a null pointer when the parser has no
   * more documents.  Parser exceptions propagate; no partial tape is returned.
   */
  static std::shared_ptr<const EventTape> Capture(Parser& parser);

  /**
   * Validates `records` as one complete, well-formed document and returns an
   * immutable tape holding them.
   *
   * @throw std::runtime_error("event archive: invalid tape") if the records do
   *        not describe exactly one valid document.
   */
  static std::shared_ptr<const EventTape> FromRecords(
      std::vector<ArchiveEvent> records);

  /** The recorded callbacks, in original callback order. */
  const std::vector<ArchiveEvent>& records() const { return records_; }

  /** Number of recorded callbacks. */
  std::size_t size() const { return records_.size(); }

 private:
  explicit EventTape(std::vector<ArchiveEvent> records)
      : records_(std::move(records)) {}

  std::vector<ArchiveEvent> records_;
};

/**
 * A resumable, budgeted replay position inside one EventTape.
 *
 * A cursor owns its own position, so any number of cursors may replay the same
 * shared tape independently.  Callbacks are handed to a caller supplied handler
 * unchanged.  A cursor must always be driven with the same kind of handler it
 * was started with, and it must be discarded if a handler callback throws.
 */
class YAML_CPP_API EventCursor {
 public:
  /**
   * @throw std::runtime_error if `tape` is null.
   */
  explicit EventCursor(std::shared_ptr<const EventTape> tape);

  EventCursor(const EventCursor&) = default;
  EventCursor& operator=(const EventCursor&) = default;
  ~EventCursor();

  /**
   * Replays at most `max_nodes` node callbacks into `handler`.
   *
   * Scalar, Null, Alias, SequenceStart and MapStart callbacks each cost one
   * node; DocumentStart, DocumentEnd, SequenceEnd, MapEnd and Anchor are free.
   * A budget of zero performs no callbacks at all.  After the budget is spent
   * the cursor drains the collection-end and document-end callbacks that close
   * the node it stopped at, and never splits an Anchor callback from the node
   * it names.
   *
   * @return true when the whole tape has been replayed.
   */
  bool Pump(EventHandler& handler, std::size_t max_nodes);

  /** True when every callback of the tape has been replayed. */
  bool Done() const;

  /** The tape being replayed. */
  const std::shared_ptr<const EventTape>& tape() const { return tape_; }

 private:
  std::shared_ptr<const EventTape> tape_;
  std::size_t position_ = 0;
};

}  // namespace YAML

#endif  // YAML_CPP_EVENTTAPE_H_
