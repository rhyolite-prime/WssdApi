//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  RuntimeContext — the workflow variable arena.
//
//  v2 (implementation_plan_2.md T1.5/T2.4/T4.1) adds:
//    • dotted-path access that understands both flat keys ("a.b") and nested
//      documents, so HTTP output extraction and `$a.b` expressions agree
//    • write-tracking so parallel joins can merge only what a branch touched
//      (instead of blind last-writer-wins)
//    • scoped overlays (loop bodies / subflow isolation)
//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/JsonPath.hpp"

namespace sapo::runtime {

    class RuntimeContext {
    public:
        using json = nlohmann::json;

        RuntimeContext() = default;

        // Prevent accidental copying to ensure thread/state integrity across
        // execution workers; use `fork()` when an isolated branch is wanted.
        RuntimeContext(const RuntimeContext&) = delete;
        RuntimeContext& operator=(const RuntimeContext&) = delete;

        /** Creates an isolated copy (used by parallel branches and subflows). */
        [[nodiscard]] std::unique_ptr<RuntimeContext> fork() const {
            auto child = std::make_unique<RuntimeContext>();
            child->m_variables = snapshot();
            return child;
        }

        void setVariable(const std::string &key, const json &value) {
            std::unique_lock lock(m_context_mutex);
            if (value.is_null() && m_delete_on_null) {
                m_variables.erase(key);
            } else {
                m_variables[key] = value;
            }
            recordWrite(key);
        }

        /** Null writes erase the key (legacy behaviour used by jump signals). */
        void setDeleteOnNull(bool enabled) { m_delete_on_null = enabled; }

        [[nodiscard]] std::optional<json> getVariable(const std::string &key) const {
            std::shared_lock lock(m_context_mutex);
            return lookupLocked(key);
        }

        [[nodiscard]] bool hasVariable(const std::string &key) const {
            std::shared_lock lock(m_context_mutex);
            return lookupLocked(key).has_value();
        }

        /**
         * @brief Dotted/bracketed path read: exact key first (so keys written by
         *        the HTTP extractor still resolve), then nested traversal.
         */
        [[nodiscard]] std::optional<json> getByPath(const std::string &path) const {
            std::shared_lock lock(m_context_mutex);
            if (auto direct = lookupLocked(path); direct.has_value()) return direct;

            const auto steps = util::parsePath(path);
            if (!steps.has_value() || steps->empty()) return std::nullopt;
            if (auto root = lookupLocked(steps->front().field); root.has_value()) {
                json document = *root;
                std::vector<util::PathStep> rest(steps->begin() + 1, steps->end());
                if (const auto *found = util::walk(document, rest)) return *found;
            }
            if (const auto *found = util::walk(m_variables, *steps)) return *found;
            return std::nullopt;
        }

        /// Writes through a dotted path, creating nested objects as needed. When
        /// the base key already exists as a flat key, the flat key wins.
        void setByPath(const std::string &path, const json &value) {
            std::unique_lock lock(m_context_mutex);
            if (m_variables.contains(path)) {
                m_variables[path] = value;
                recordWrite(path);
                return;
            }
            const auto steps = util::parsePath(path);
            if (steps.has_value() && !steps->empty() && m_variables.contains(steps->front().field)) {
                json &root = m_variables[steps->front().field];
                std::vector<util::PathStep> rest(steps->begin() + 1, steps->end());
                json *cursor = &root;
                for (size_t i = 0; i < rest.size(); ++i) {
                    const bool last = (i + 1 == rest.size());
                    const auto &step = rest[i];
                    if (step.kind == util::PathStep::Kind::Index) {
                        if (!cursor->is_array()) *cursor = json::array();
                        while (cursor->size() <= static_cast<size_t>(step.index)) cursor->push_back(nullptr);
                        cursor = &cursor->at(static_cast<size_t>(step.index));
                    } else {
                        if (!cursor->is_object()) *cursor = json::object();
                        cursor = &(*cursor)[step.field];
                    }
                    if (last) *cursor = value;
                }
                recordWrite(steps->front().field);
                return;
            }
            if (steps.has_value() && !steps->empty()) {
                json document = json::object();
                util::setPath(document, path, value);
                m_variables[steps->front().field] = document[steps->front().field];
                recordWrite(steps->front().field);
                return;
            }
            m_variables[path] = value;
            recordWrite(path);
        }

        void erase(const std::string &key) {
            std::unique_lock lock(m_context_mutex);
            m_variables.erase(key);
            recordWrite(key);
        }

        [[nodiscard]] json getAllVariables() const {
            std::shared_lock lock(m_context_mutex);
            return m_variables;
        }

        [[nodiscard]] size_t size() const {
            std::shared_lock lock(m_context_mutex);
            return m_variables.size();
        }

        std::string serializeState() const {
            std::shared_lock lock(m_context_mutex);
            return m_variables.dump();
        }

        void deserializeState(const std::string &json_state) {
            if (json_state.empty()) return;
            std::unique_lock lock(m_context_mutex);
            m_variables = json::parse(json_state);
            if (!m_variables.is_object()) m_variables = json::object();
        }

        void loadState(const std::string &json_str) { deserializeState(json_str); }

        [[nodiscard]] json snapshot() const {
            std::shared_lock lock(m_context_mutex);
            return m_variables;
        }

        void restore(const json &state) {
            std::unique_lock lock(m_context_mutex);
            m_variables = state.is_object() ? state : json::object();
        }

        void clear() {
            std::unique_lock lock(m_context_mutex);
            m_variables.clear();
            m_journal.clear();
            m_sequence = 0;
        }

        /**
         * @brief Overlay a read-through layer on top of the shared store.
         *
         * Loop bodies and predicate lambdas push a layer with `item`/`index`
         * bindings; lookups check layers top-down, writes to layered names stay
         * in the layer so they disappear with the scope.
         */
        void pushLayer(json layer) {
            std::unique_lock lock(m_context_mutex);
            m_layers.push_back(std::move(layer));
        }

        void popLayer() {
            std::unique_lock lock(m_context_mutex);
            if (!m_layers.empty()) m_layers.pop_back();
        }

        /// RAII helper for scoped layers.
        class LayerGuard {
        public:
            LayerGuard(RuntimeContext &context, json layer) : m_context(context) {
                m_context.pushLayer(std::move(layer));
            }
            ~LayerGuard() { m_context.popLayer(); }
            LayerGuard(const LayerGuard &) = delete;
            LayerGuard &operator=(const LayerGuard &) = delete;

        private:
            RuntimeContext &m_context;
        };

        /** Sets a value in the topmost layer (or the shared store if none). */
        void setLayered(const std::string &key, const json &value) {
            std::unique_lock lock(m_context_mutex);
            if (!m_layers.empty()) {
                m_layers.back()[key] = value;
                return;
            }
            m_variables[key] = value;
            recordWrite(key);
        }

        // -----------------------------------------------------------------
        // Change tracking (parallel join / subflow output projection)
        // -----------------------------------------------------------------
        [[nodiscard]] uint64_t mark() const {
            std::unique_lock lock(m_context_mutex);
            return m_sequence;
        }

        /// Keys written (or erased) after `sequence`, newest first de-duplicated.
        [[nodiscard]] std::set<std::string> changedSince(uint64_t sequence) const {
            std::unique_lock lock(m_context_mutex);
            std::set<std::string> changed;
            for (auto it = m_journal.rbegin(); it != m_journal.rend(); ++it) {
                if (it->second <= sequence) break;
                changed.insert(it->first);
            }
            return changed;
        }

        /**
         * @brief Merges `other` into this context.
         * @param keys   when non-empty, only these keys are considered.
         * @param policy "last_writer_wins" | "skip_conflicts" | "fail_conflicts"
         * @return the list of keys where both sides had different values.
         */
        enum class MergePolicy { LastWriterWins, SkipConflicts, FailConflicts };
        std::vector<std::string> mergeFrom(const RuntimeContext &other, const std::set<std::string> &keys = {},
                                           MergePolicy policy = MergePolicy::LastWriterWins) {
            std::vector<std::string> conflicts;
            const json other_state = other.snapshot();
            std::unique_lock lock(m_context_mutex);
            for (auto it = other_state.begin(); it != other_state.end(); ++it) {
                if (!keys.empty() && !keys.count(it.key())) continue;
                if (m_variables.contains(it.key()) && m_variables[it.key()] != it.value()) {
                    conflicts.push_back(it.key());
                    if (policy == MergePolicy::SkipConflicts) continue;
                }
                m_variables[it.key()] = it.value();
            }
            return conflicts;
        }

        // -----------------------------------------------------------------
        // Iteration counters used by the interpreter's loop frames
        // -----------------------------------------------------------------
        struct LoopState {
            int64_t index{0};
            int64_t total{0};
            json items = json::array();
            bool active{false};
        };

        void setLoopState(const std::string &loop_id, LoopState state) {
            std::unique_lock lock(m_context_mutex);
            m_loop_states[loop_id] = std::move(state);
        }

        [[nodiscard]] std::optional<LoopState> loopState(const std::string &loop_id) const {
            std::shared_lock lock(m_context_mutex);
            auto it = m_loop_states.find(loop_id);
            if (it == m_loop_states.end()) return std::nullopt;
            return it->second;
        }

        void clearLoopState(const std::string &loop_id) {
            std::unique_lock lock(m_context_mutex);
            m_loop_states.erase(loop_id);
        }

        /// Frame stack persisted with the session so suspend/resume keeps loops.
        struct FrameState {
            std::string kind;         // loop | try | subflow
            std::string node_id;      // owning node
            int64_t position{0};      // cursor within the frame body
            std::string resume_target;
            json state = json::object();
        };

        void setFrames(std::vector<FrameState> frames) {
            std::unique_lock lock(m_context_mutex);
            m_frames = std::move(frames);
        }

        [[nodiscard]] std::vector<FrameState> frames() const {
            std::shared_lock lock(m_context_mutex);
            return m_frames;
        }

        [[nodiscard]] static json framesToJson(const std::vector<FrameState> &frames) {
            json out = json::array();
            for (const auto &frame : frames) {
                out.push_back({{"kind", frame.kind},
                               {"node", frame.node_id},
                               {"position", frame.position},
                               {"resume", frame.resume_target},
                               {"state", frame.state}});
            }
            return out;
        }

        [[nodiscard]] static std::vector<FrameState> framesFromJson(const json &value) {
            std::vector<FrameState> out;
            if (!value.is_array()) return out;
            for (const auto &item : value) {
                FrameState frame;
                frame.kind = item.value("kind", "");
                frame.node_id = item.value("node", "");
                frame.position = item.value("position", 0);
                frame.resume_target = item.value("resume", "");
                frame.state = item.value("state", json::object());
                out.push_back(std::move(frame));
            }
            return out;
        }

        [[nodiscard]] std::vector<std::string> keys() const {
            std::shared_lock lock(m_context_mutex);
            std::vector<std::string> out;
            out.reserve(m_variables.size());
            for (auto it = m_variables.begin(); it != m_variables.end(); ++it) out.push_back(it.key());
            return out;
        }

    private:
        [[nodiscard]] std::optional<json> lookupLocked(const std::string &key) const {
            for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
                if (it->is_object() && it->contains(key)) return it->at(key);
            }
            auto found = m_variables.find(key);
            if (found != m_variables.end()) return found.value();
            return std::nullopt;
        }

        void recordWrite(const std::string &key) {
            m_journal[key] = ++m_sequence;
        }

        json m_variables = json::object();
        std::vector<json> m_layers;
        std::map<std::string, uint64_t> m_journal;
        uint64_t m_sequence{0};
        bool m_delete_on_null{false};
        std::map<std::string, LoopState> m_loop_states;
        std::vector<FrameState> m_frames;
        mutable std::shared_mutex m_context_mutex;
    };

} // namespace sapo::runtime
