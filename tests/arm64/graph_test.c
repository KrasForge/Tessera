/* tests/arm64/graph_test.c - host unit tests for the audio graph (Issue #27).
 *
 * Covers the acceptance criteria: a two-node (synth + DAC) graph, a three-node
 * (synth + effect + DAC) graph sorted in the correct order with the DAC last,
 * and rejection of an invalid PID without corrupting the graph - plus cycle
 * detection, duplicate edges, and the DAC-is-a-sink rule.
 *
 * Build/run via:  make test-arm-graph
 */

#include "audio_graph.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Validator: PIDs 1..99 are "live" processes; anything else is invalid. */
static int valid_pid(uint32_t pid) { return pid >= 1 && pid <= 99; }

static int index_of(const int *order, int n, int node)
{
    for (int i = 0; i < n; i++) if (order[i] == node) return i;
    return -1;
}

static void test_two_node(void)
{
    printf("- two nodes: synth -> DAC (M5 shape)\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int synth = audio_graph_add_node(&g, 1);
    int dac   = audio_graph_add_dac(&g);
    CHECK(synth >= 0 && dac >= 0, "synth and DAC added");
    CHECK(audio_graph_connect(&g, synth, dac) >= 0, "synth -> DAC connected");

    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g, order, GRAPH_MAX_NODES);
    CHECK(n == 2, "two nodes sorted");
    CHECK(order[0] == synth && order[1] == dac, "synth before DAC, DAC last");
}

static void test_three_node(void)
{
    printf("- three nodes: synth -> effect -> DAC (correct order)\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int synth  = audio_graph_add_node(&g, 1);
    int effect = audio_graph_add_node(&g, 2);
    int dac    = audio_graph_add_dac(&g);
    audio_graph_connect(&g, synth, effect);
    audio_graph_connect(&g, effect, dac);

    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g, order, GRAPH_MAX_NODES);
    CHECK(n == 3, "three nodes sorted");
    int is = index_of(order, n, synth), ie = index_of(order, n, effect),
        id = index_of(order, n, dac);
    CHECK(is < ie && ie < id, "synth before effect before DAC");
    CHECK(order[n - 1] == dac, "DAC is last");
}

static void test_dac_forced_last(void)
{
    printf("- DAC sorted last even if added first\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int dac   = audio_graph_add_dac(&g);          /* added first */
    int synth = audio_graph_add_node(&g, 7);
    audio_graph_connect(&g, synth, dac);
    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g, order, GRAPH_MAX_NODES);
    CHECK(n == 2 && order[1] == dac, "DAC still ends up last");
}

static void test_invalid_pid(void)
{
    printf("- invalid PID rejected without corrupting the graph\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int a = audio_graph_add_node(&g, 5);
    int before = g.n_nodes;
    CHECK(a >= 0 && before == 1, "one valid node added");

    CHECK(audio_graph_add_node(&g, 0) == -1, "PID 0 rejected");
    CHECK(audio_graph_add_node(&g, 1000) == -1, "out-of-range PID rejected");
    CHECK(g.n_nodes == before, "node count unchanged after rejected adds");

    /* The good node and a fresh add still work, proving no corruption. */
    int b = audio_graph_add_node(&g, 6);
    CHECK(b >= 0 && b != a && g.n_nodes == 2, "graph still usable after bad adds");
}

static void test_rules(void)
{
    printf("- connection rules and cycle detection\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int s = audio_graph_add_node(&g, 1);
    int e = audio_graph_add_node(&g, 2);
    int dac = audio_graph_add_dac(&g);

    CHECK(audio_graph_connect(&g, s, e) >= 0, "first edge ok");
    CHECK(audio_graph_connect(&g, s, e) == -1, "duplicate edge rejected");
    CHECK(audio_graph_connect(&g, s, s) == -1, "self-edge rejected");
    CHECK(audio_graph_connect(&g, dac, e) == -1, "DAC cannot be a source");
    CHECK(audio_graph_add_dac(&g) == -1, "second DAC rejected");

    /* Make a cycle: e -> s as well as s -> e. */
    audio_graph_connect(&g, e, s);
    int order[GRAPH_MAX_NODES];
    CHECK(audio_graph_toposort(&g, order, GRAPH_MAX_NODES) == -1, "cycle detected");
}

static void test_full(void)
{
    printf("- node table capacity\n");
    audio_graph_t g;
    audio_graph_init(&g, 0);             /* no validator: pid != 0 suffices */
    int added = 0;
    for (uint32_t i = 1; i <= GRAPH_MAX_NODES; i++)
        if (audio_graph_add_node(&g, i) >= 0) added++;
    CHECK(added == GRAPH_MAX_NODES, "filled to capacity");
    CHECK(audio_graph_add_node(&g, 999) == -1, "add beyond capacity rejected");
}

static void test_input_node(void)
{
    printf("- capture input node (issue #84)\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int in  = audio_graph_add_input(&g);
    int eff = audio_graph_add_node(&g, 1);
    int dac = audio_graph_add_dac(&g);
    CHECK(in >= 0 && g.input_node == in, "input node added as a singleton");
    CHECK(audio_graph_add_input(&g) == -1, "second input rejected");
    CHECK(audio_graph_node_by_pid(&g, GRAPH_INPUT_PID) == in,
          "input found by reserved pid");

    CHECK(audio_graph_connect(&g, in, eff) >= 0, "input -> effect ok");
    CHECK(audio_graph_connect(&g, eff, in) == -1, "input cannot be a destination");
    CHECK(audio_graph_connect(&g, eff, dac) >= 0, "effect -> DAC ok");

    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g, order, GRAPH_MAX_NODES);
    int pi = -1, pe = -1, pd = -1;
    for (int i = 0; i < n; i++) {
        if (order[i] == in)  pi = i;
        if (order[i] == eff) pe = i;
        if (order[i] == dac) pd = i;
    }
    CHECK(n == 3 && pi < pe && pe < pd, "input before effect before DAC");

    /* input -> DAC directly (the bypass path) is a legal edge. */
    audio_graph_disconnect(&g, in, eff);
    audio_graph_disconnect(&g, eff, dac);
    CHECK(audio_graph_connect(&g, in, dac) >= 0, "input -> DAC bypass ok");

    /* Removing the input clears the singleton slot. */
    audio_graph_remove_node(&g, in);
    CHECK(g.input_node == -1, "input_node cleared on removal");
    CHECK(audio_graph_add_input(&g) >= 0, "a fresh input can be re-added");
}

static void test_feedback_edges(void)
{
    printf("- feedback edges: a one-block delay breaks a cycle for scheduling (#117)\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);
    int a   = audio_graph_add_node(&g, 1);
    int b   = audio_graph_add_node(&g, 2);
    int dac = audio_graph_add_dac(&g);

    CHECK(audio_graph_connect(&g, a, b) >= 0, "a -> b (forward) ok");
    int fe = audio_graph_connect_feedback(&g, b, a);   /* closes the cycle */
    CHECK(fe >= 0, "b -> a (feedback) accepted");
    CHECK(audio_graph_edge_is_feedback(&g, fe) == 1, "the edge is marked feedback");
    CHECK(audio_graph_edge_is_feedback(&g, audio_graph_find_edge(&g, a, b)) == 0,
          "the forward edge is not feedback");
    audio_graph_connect(&g, b, dac);

    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g, order, GRAPH_MAX_NODES);
    CHECK(n == 3, "a graph with a feedback cycle still sorts (no error)");
    int ia = index_of(order, n, a), ib = index_of(order, n, b);
    CHECK(ia < ib, "the forward edge orders a before b; the feedback edge is ignored");
    CHECK(order[n - 1] == dac, "DAC still last");

    /* Contrast: the same cycle with a *normal* back-edge is still rejected. */
    audio_graph_t g2;
    audio_graph_init(&g2, valid_pid);
    int x = audio_graph_add_node(&g2, 1), y = audio_graph_add_node(&g2, 2);
    audio_graph_connect(&g2, x, y);
    audio_graph_connect(&g2, y, x);                    /* normal back-edge */
    CHECK(audio_graph_toposort(&g2, order, GRAPH_MAX_NODES) == -1,
          "a normal back-edge is still a rejected cycle");

    /* find + disconnect work on a feedback edge. */
    CHECK(audio_graph_find_edge(&g, b, a) == fe, "feedback edge found by src/dst");
    audio_graph_disconnect(&g, b, a);
    CHECK(audio_graph_find_edge(&g, b, a) == -1, "feedback edge disconnected");
    CHECK(audio_graph_toposort(&g, order, GRAPH_MAX_NODES) == 3,
          "graph still sorts after the feedback edge is removed");
}

static void test_channels(void)
{
    printf("- multi-channel buses: connect requires matching widths (#119)\n");
    audio_graph_t g;
    audio_graph_init(&g, valid_pid);

    /* Nodes default to stereo, so a plain stereo graph is unaffected. */
    int s = audio_graph_add_node(&g, 1);
    int e = audio_graph_add_node(&g, 2);
    int dac = audio_graph_add_dac(&g);
    int se = audio_graph_connect(&g, s, e);
    CHECK(se >= 0, "stereo default: synth -> effect connects");
    CHECK(audio_graph_edge_channels(&g, se) == 2, "stereo edge carries 2 channels");
    CHECK(audio_graph_connect(&g, e, dac) >= 0, "stereo effect -> DAC connects");

    /* A mismatched pair is rejected and leaves the edge table untouched. */
    audio_graph_t g2;
    audio_graph_init(&g2, valid_pid);
    int a = audio_graph_add_node(&g2, 1);   /* 6-out surround source */
    int b = audio_graph_add_node(&g2, 2);   /* stereo consumer       */
    audio_graph_set_channels(&g2, a, 2, 6);
    int before = g2.n_edges;
    CHECK(audio_graph_connect(&g2, a, b) == -1, "6ch -> 2ch mismatch rejected");
    CHECK(g2.n_edges == before, "rejected connect adds no edge");

    /* Widen the consumer's input to 6 and the same edge now connects. */
    audio_graph_set_channels(&g2, b, 6, 2);
    int ab = audio_graph_connect(&g2, a, b);
    CHECK(ab >= 0, "6ch -> 6ch matching connects");
    CHECK(audio_graph_edge_channels(&g2, ab) == 6, "surround edge carries 6 channels");

    /* A 0 count leaves that side unchanged (set only out_ch here). */
    int c = audio_graph_add_node(&g2, 3);
    audio_graph_set_channels(&g2, c, 0, 4);
    CHECK(g2.nodes[c].in_ch == 2 && g2.nodes[c].out_ch == 4,
          "zero count leaves in_ch, sets out_ch");

    CHECK(audio_graph_edge_channels(&g2, 99) == 0, "out-of-range edge -> 0 channels");
}

int main(void)
{
    printf("=== Tessera audio-graph tests (issue #27) ===\n");
    test_two_node();
    test_three_node();
    test_dac_forced_last();
    test_invalid_pid();
    test_rules();
    test_full();
    test_input_node();
    test_feedback_edges();
    test_channels();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
