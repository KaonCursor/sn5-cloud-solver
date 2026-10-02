#ifndef YAML_CPP_EVENTARCHIVE_H_
#define YAML_CPP_EVENTARCHIVE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "yaml-cpp/dll.h"
#include "yaml-cpp/emitterstyle.h"
#include "yaml-cpp/eventtape.h"
#include "yaml-cpp/mark.h"
#include "yaml-cpp/node/type.h"

namespace YAML {

class Node;
class Parser;

/**
 * A flattened, allocation-free description of one node of one retained
 * document.
 *
 * The fields are the ones a caller needs to reason about a path without
 * building a Node graph: the node kind, the position it started at, its tag,
 * its scalar payload (empty unless the node is a scalar) and the style its
 * container was opened with.
 */
struct YAML_CPP_API ArchiveNodeInfo {
  NodeType::value type = NodeType::Undefined;
  Mark mark = Mark::null_mark();
  std::string tag;
  std::string scalar;
  EmitterStyle::value style = EmitterStyle::Default;
};

/**
 * A bounded, transactional store of immutable event tapes.
 *
 * The archive owns a FIFO of complete documents.  Each document costs the sum
 * of the byte lengths of its scalar payloads (see ScalarBytes() in
 * eventarchive_store.cpp); the archive never retains more than `max_scalar_bytes`
 * payload bytes at once, evicting the oldest complete documents to make room
 * for a new one.  Document identifiers start at zero, are handed out in
 * increasing order and are never reused, so an identifier keeps naming the same
 * document for as long as that document is retained.
 *
 * Every mutating operation is transactional: a document is captured and priced
 * in full before any archive state changes, so a failed append leaves both the
 * retained set and the identifier counter untouched.
 *
 * The quota counts scalar payload bytes only; it is not a bound on total memory
 * use.  A limit of zero is legal and accepts documents that contain no scalar
 * payloads at all.
 */
class YAML_CPP_API EventArchive {
 public:
  /**
   * Creates an empty archive that retains at most `max_scalar_bytes` scalar
   * payload bytes.
   */
  explicit EventArchive(std::size_t max_scalar_bytes);

  /**
   * Captures the next document of `parser` and, if it is retained, stores it.
   *
   * @return false when the parser had no more documents; `assigned_id` is then
   *         left unchanged.
   * @throw std::runtime_error("event archive: document exceeds scalar byte
   *        limit") when the document alone does not fit the limit.  Nothing is
   *        evicted and no identifier is consumed.
   * @throw std::runtime_error on identifier or cost arithmetic overflow; the
   *        archive state is unchanged.
   * @throw ParserException when the parser fails; the archive state is
   *        unchanged.
   */
  bool AppendNext(Parser& parser, std::uint64_t& assigned_id);

  /** The identifiers of the retained documents, oldest first. */
  std::vector<std::uint64_t> Documents() const;

  /** The scalar payload bytes currently retained. */
  std::size_t UsedScalarBytes() const;

  /**
   * A cursor positioned at the start of the named document.
   *
   * @throw std::runtime_error("event archive: document not retained: <id>")
   *        when `id` is not retained.
   */
  EventCursor Replay(std::uint64_t id) const;

  /**
   * Replays the named document into a NodeBuilder.
   *
   * Alias identity is preserved, so shared and recursive structures come back
   * as aliases rather than as copies.
   */
  Node Build(std::uint64_t id) const;

  /**
   * Emits the named document as one YAML document.
   *
   * Comments, original formatting and textual anchor names are not promised by
   * the emitted text.
   *
   * @throw std::runtime_error when the emitter rejects the document.
   */
  std::string Emit(std::uint64_t id) const;

  /**
   * A portable, human readable checkpoint of the whole archive.
   *
   * The archive state is not changed.
   *
   * @throw std::runtime_error when the emitter rejects the checkpoint.
   */
  std::string Save() const;

  /**
   * Rebuilds an archive from Save() output, restoring the exact retained
   * identifiers, their events and the retention limit.
   *
   * @throw std::runtime_error("event archive: invalid checkpoint") when
   *        `checkpoint` is not a valid archive checkpoint.
   */
  static EventArchive Restore(const std::string& checkpoint);

  /**
   * Describes the node at `pointer` (a JSON-style YAML path such as
   * "/a/0/b") of the named document, following aliases.
   *
   * @throw std::runtime_error("event archive: document not retained: <id>")
   *        when `id` is not retained.
   */
  ArchiveNodeInfo Inspect(std::uint64_t id, const std::string& pointer) const;

 private:
  /** The retained tape of `id`, shared with any existing cursor. */
  std::shared_ptr<const EventTape> GetTape(std::uint64_t id) const;

  /** One retained, completely priced document. */
  struct Entry {
    std::uint64_t id = 0;
    std::shared_ptr<const EventTape> tape;
    std::size_t scalar_bytes = 0;
  };

  std::size_t max_scalar_bytes_;
  std::size_t used_scalar_bytes_ = 0;
  std::uint64_t next_id_ = 0;
  std::deque<Entry> entries_;  // oldest first
};

}  // namespace YAML

#endif  // YAML_CPP_EVENTARCHIVE_H_
