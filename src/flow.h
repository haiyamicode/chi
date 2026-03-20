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
        for (size_t i = 0; i < terminals.len; i++) {
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
        edge_offsets[node] = edges ? edges->len : 0;
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
            for (size_t i = 0; i < items.len; i++) {
                auto *item = items.items[i];
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
                for (size_t i = 0; i < edges.len; i++) {
                    bool found = false;
                    for (size_t j = 0; j < existing->len; j++) {
                        if (existing->items[j] == edges.items[i]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        existing->add(edges.items[i]);
                        if (!existing_pos) {
                            ref_edge_offsets[key] = {};
                            existing_pos = ref_edge_offsets.get(key);
                        }
                        existing_pos->add(other_pos && i < other_pos->len ? other_pos->items[i] : -1);
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
        for (size_t i = 0; i < other.terminals.len; i++) {
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
        auto *deps = ref_edges.get(from);
        if (!deps || deps->len == 0) {
            if (fallback_to_source)
                add_ref_edge(to, from, edge_offset);
            return;
        }
        // Follow edges to find leaf terminals (nodes with no outgoing edges)
        array<Node *> stack;
        for (size_t i = 0; i < deps->len; i++)
            stack.add(deps->items[i]);
        map<Node *, bool> visited;
        while (stack.len > 0) {
            auto *node = stack.last();
            stack.len--;
            if (visited.has_key(node))
                continue;
            visited[node] = true;
            auto *next = ref_edges.get(node);
            if (next && next->len > 0) {
                for (size_t i = 0; i < next->len; i++)
                    stack.add(next->items[i]);
            } else {
                // Leaf terminal — add direct edge
                add_ref_edge(to, node, edge_offset);
            }
        }
    }
};

} // namespace ast
} // namespace cx
