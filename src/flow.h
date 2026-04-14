#pragma once

#include "util.h"

namespace cx {

struct ChiLifetime;

namespace ast {
struct Node;

enum class SinkKind { Definite, Maybe };

struct SinkEdge {
    Node *target = nullptr;
    SinkKind kind = SinkKind::Definite;
};

// FlowState tracks the dataflow analysis state within a function.
// Contains move tracking (sink edges), borrow tracking (ref edges),
// and terminal/escape tracking.
struct FlowState {
    map<Node *, array<Node *>> ref_edges = {}; // escape analysis: dependency graph
    map<Node *, array<long>> ref_edge_offsets = {}; // creation offset for each ref edge
    map<Node *, array<Node *>> copy_edges = {}; // by-value copy graph: A copies borrow deps from B
    map<Node *, SinkEdge> sink_edges = {};     // move: a → {target, kind}
    map<Node *, SinkEdge> invalidate_edges = {}; // exclusive-access invalidation: a → {target, kind}
    map<Node *, array<Node *>> invalidate_exempt_terminals = {}; // roots whose invalidation does not kill specific borrow values
    map<Node *, size_t> edge_offsets = {};     // per-variable: index where current edges start
    map<Node *, long> terminal_last_use = {};  // per-terminal: offset of last reference
    map<Node *, Node *> terminal_last_use_node = {}; // per-terminal: node of last reference
    array<Node *> terminals = {};              // nodes whose lifetimes extend beyond the function
    map<Node *, ChiLifetime *> terminal_lifetimes = {}; // per-terminal: required lifetime constraint

    // Register a terminal node (return statement, this with field assignments, etc.)
    void add_terminal(Node *terminal) {
        if (!terminal)
            return;
        for (size_t i = 0; i < terminals.size(); i++) {
            if (terminals[i] == terminal)
                return;
        }
        terminals.add(terminal);
    }

    // Direct reference: A points to B's memory (from & operator)
    void add_ref_edge(Node *from, Node *to, long edge_offset = -1) {
        if (!from || !to || from == to)
            return;
        ref_edges[from].add(to);
        ref_edge_offsets[from].add(edge_offset);
    }

    void add_copy_edge(Node *from, Node *to) {
        if (!from || !to || from == to)
            return;
        auto &edges = copy_edges[from];
        for (auto *existing : edges) {
            if (existing == to)
                return;
        }
        edges.add(to);
    }

    // Move/sink: A's ownership was transferred to B (A is dead after this)
    void add_sink_edge(Node *from, Node *to, SinkKind kind = SinkKind::Definite) {
        if (!from || !to)
            return;
        auto *existing = sink_edges.get(from);
        if (existing) {
            existing->target = to;
            if (existing->kind == SinkKind::Maybe || kind == SinkKind::Maybe) {
                existing->kind = SinkKind::Maybe;
            } else {
                existing->kind = kind;
            }
            return;
        }
        sink_edges[from] = {to, kind};
    }

    void add_invalidate_edge(Node *from, Node *to) {
        if (!from || !to)
            return;
        invalidate_edges[from] = {to, SinkKind::Definite};
    }

    void add_invalidate_exempt_terminal(Node *root, Node *terminal) {
        if (!root || !terminal) {
            return;
        }
        auto &items = invalidate_exempt_terminals[root];
        for (auto *existing : items) {
            if (existing == terminal) {
                return;
            }
        }
        items.add(terminal);
    }

    bool is_sunk(Node *node) { return sink_edges.has_key(node); }

    bool is_maybe_sunk(Node *node) {
        auto *edge = sink_edges.get(node);
        return edge && edge->kind == SinkKind::Maybe;
    }

    Node *sink_target(Node *node) {
        auto *edge = sink_edges.get(node);
        return edge ? edge->target : nullptr;
    }

    // Get the offset where current (non-stale) edges start for a variable
    size_t current_edge_offset(Node *node) {
        auto *offset = edge_offsets.get(node);
        return offset ? *offset : 0;
    }

    // Mark current edges as stale (called before adding new edges on reassignment)
    void bump_edge_offset(Node *node) {
        auto *edges = ref_edges.get(node);
        edge_offsets[node] = edges ? edges->size() : 0;
    }

    // Deep copy of this flow state for branching
    FlowState fork() const { return *this; }

    // Merge another branch's flow state into this one.
    // Sinks present in only one branch become Maybe (need runtime drop flag).
    void merge(const FlowState &other) {
        // Sink edges: sinks in this only → Maybe
        for (auto &[key, edge] : sink_edges.data) {
            if (other.sink_edges.data.find(key) == other.sink_edges.data.end()) {
                edge.kind = SinkKind::Maybe;
            }
        }
        // Sink edges: add from other (both → keep min kind, other only → Maybe)
        for (auto &[key, edge] : other.sink_edges.data) {
            auto *existing = sink_edges.get(key);
            if (existing) {
                // In both: Maybe if either is Maybe
                if (existing->kind == SinkKind::Maybe || edge.kind == SinkKind::Maybe)
                    existing->kind = SinkKind::Maybe;
            } else {
                sink_edges[key] = {edge.target, SinkKind::Maybe};
            }
        }

        // Invalidation edges: same merge rules as sink edges
        for (auto &[key, edge] : invalidate_edges.data) {
            if (other.invalidate_edges.data.find(key) == other.invalidate_edges.data.end()) {
                edge.kind = SinkKind::Maybe;
            }
        }
        for (auto &[key, edge] : other.invalidate_edges.data) {
            auto *existing = invalidate_edges.get(key);
            if (existing) {
                if (existing->kind == SinkKind::Maybe || edge.kind == SinkKind::Maybe)
                    existing->kind = SinkKind::Maybe;
            } else {
                invalidate_edges[key] = {edge.target, SinkKind::Maybe};
            }
        }

        for (auto &[key, items] : other.invalidate_exempt_terminals.data) {
            auto *existing = invalidate_exempt_terminals.get(key);
            if (!existing) {
                invalidate_exempt_terminals[key] = items;
                continue;
            }
            for (size_t i = 0; i < items.size(); i++) {
                auto *item = items[i];
                bool found = false;
                for (auto *cur : *existing) {
                    if (cur == item) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    existing->add(item);
                }
            }
        }

        // Merge ref edges: union of edge lists
        for (auto &[key, edges] : other.ref_edges.data) {
            auto *existing = ref_edges.get(key);
            auto *existing_pos = ref_edge_offsets.get(key);
            auto other_pos_it = other.ref_edge_offsets.data.find(key);
            array<long> *other_pos = other_pos_it != other.ref_edge_offsets.data.end()
                                         ? const_cast<array<long> *>(&other_pos_it->second)
                                         : nullptr;
            if (!existing) {
                ref_edges[key] = edges;
                if (other_pos) {
                    ref_edge_offsets[key] = *other_pos;
                }
            } else {
                for (size_t i = 0; i < edges.size(); i++) {
                    bool found = false;
                    for (size_t j = 0; j < existing->size(); j++) {
                        if ((*existing)[j] == edges[i]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        existing->add(edges[i]);
                        if (!existing_pos) {
                            ref_edge_offsets[key] = {};
                            existing_pos = ref_edge_offsets.get(key);
                        }
                        existing_pos->add(other_pos && i < other_pos->size() ? (*other_pos)[i] : -1);
                    }
                }
            }
        }

        // Merge copy edges: union of edge lists
        for (auto &[key, edges] : other.copy_edges.data) {
            auto *existing = copy_edges.get(key);
            if (!existing) {
                copy_edges[key] = edges;
            } else {
                for (size_t i = 0; i < edges.size(); i++) {
                    bool found = false;
                    for (size_t j = 0; j < existing->size(); j++) {
                        if ((*existing)[j] == edges[i]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        existing->add(edges[i]);
                    }
                }
            }
        }

        // Merge edge offsets: take the max
        for (auto &[key, val] : other.edge_offsets.data) {
            auto *existing = edge_offsets.get(key);
            if (!existing || val > *existing)
                edge_offsets[key] = val;
        }

        // Merge terminal last use: take the max
        for (auto &[key, val] : other.terminal_last_use.data) {
            auto *existing = terminal_last_use.get(key);
            if (!existing || val > *existing)
                terminal_last_use[key] = val;
        }
        for (auto &[key, val] : other.terminal_last_use_node.data) {
            auto other_use_it = other.terminal_last_use.data.find(key);
            if (other_use_it == other.terminal_last_use.data.end())
                continue;
            auto *existing = terminal_last_use.get(key);
            auto *existing_node = terminal_last_use_node.get(key);
            if (!existing || other_use_it->second >= *existing || !existing_node) {
                terminal_last_use_node[key] = val;
            }
        }

        // Merge terminals: union
        for (size_t i = 0; i < other.terminals.size(); i++) {
            add_terminal(other.terminals[i]);
        }

        // Merge terminal lifetimes: union
        for (auto &[key, val] : other.terminal_lifetimes.data) {
            if (!terminal_lifetimes.has_key(key))
                terminal_lifetimes[key] = val;
        }
    }

    // By-value copy: A inherits B's leaf terminals (the actual memory targets)
    // Traverses B's edges to find leaves, then creates edges from A to each leaf.
    // fallback_to_source: if true (default), treat `from` itself as a leaf when it
    //   has no edges. If false, do nothing when `from` has no edges.
    void copy_ref_edges(Node *to, Node *from, bool fallback_to_source = true, long edge_offset = -1) {
        if (!to || !from || to == from)
            return;
        add_copy_edge(to, from);
        copy_existing_ref_edges(to, from, fallback_to_source, edge_offset);
    }

    // Copy only the current leaf borrow dependencies of `from` into `to`.
    // Unlike copy_ref_edges(), this does not record an abstract copy-edge relation.
    void copy_existing_ref_edges(Node *to, Node *from, bool fallback_to_source = true,
                                 long edge_offset = -1) {
        if (!to || !from || to == from)
            return;
        auto *deps = ref_edges.get(from);
        if (!deps || deps->size() == 0) {
            if (fallback_to_source)
                add_ref_edge(to, from, edge_offset);
            return;
        }
        // Follow edges to find leaf terminals (nodes with no outgoing edges)
        array<Node *> stack;
        for (size_t i = 0; i < deps->size(); i++)
            stack.add((*deps)[i]);
        map<Node *, bool> visited;
        while (stack.size() > 0) {
            auto *node = stack.pop();
            if (visited.has_key(node))
                continue;
            visited[node] = true;
            auto *next = ref_edges.get(node);
            if (next && next->size() > 0) {
                for (size_t i = 0; i < next->size(); i++)
                    stack.add((*next)[i]);
            } else {
                // Leaf terminal — add direct edge
                add_ref_edge(to, node, edge_offset);
            }
        }
    }

    // Follow both borrow and copy edges from `start` and collect only the leaf nodes that are
    // reached through at least one borrow edge. Pure value-copy chains with no borrow relation
    // produce no leaves.
    void collect_borrow_leaves(Node *start, array<Node *> &leaves) {
        struct TraverseState {
            Node *node = nullptr;
            bool has_borrow = false;
        };

        if (!start)
            return;

        array<TraverseState> stack;
        map<Node *, uint8_t> visited;

        if (auto *deps = ref_edges.get(start)) {
            for (size_t i = 0; i < deps->size(); i++) {
                stack.add({(*deps)[i], true});
            }
        }
        if (auto *deps = copy_edges.get(start)) {
            for (size_t i = 0; i < deps->size(); i++) {
                stack.add({(*deps)[i], false});
            }
        }

        while (stack.size() > 0) {
            auto state = stack.pop();

            uint8_t mask = state.has_borrow ? 0x2 : 0x1;
            auto *seen = visited.get(state.node);
            if (seen && ((*seen & mask) == mask)) {
                continue;
            }
            visited[state.node] = seen ? static_cast<uint8_t>(*seen | mask) : mask;

            if (auto *next = ref_edges.get(state.node); next && next->size() > 0) {
                for (size_t i = 0; i < next->size(); i++) {
                    stack.add({(*next)[i], true});
                }
                continue;
            }

            if (state.has_borrow) {
                bool found = false;
                for (auto *leaf : leaves) {
                    if (leaf == state.node) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    leaves.add(state.node);
                }
                continue;
            }

            if (auto *next = copy_edges.get(state.node)) {
                for (size_t i = 0; i < next->size(); i++) {
                    stack.add({(*next)[i], false});
                }
            }
        }
    }
};

} // namespace ast
} // namespace cx
