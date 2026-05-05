/*
 * Zeos — std.btree (in-memory B-tree)
 *
 * Real B-tree with split / merge.  Pages allocated from a fixed pool
 * (BTREE_MAX_NODES) so the OS doesn't need a malloc storm under load.
 *
 * Algorithm: Cormen et al. "Introduction to Algorithms" 3rd ed., ch 18.
 *   - Insert via "split-as-we-descend" so the parent is never full.
 *   - Delete via "ensure-at-least-min-keys" (merge with sibling, or
 *     borrow) before recursing.
 */

#include "std_btree.h"
#include "kprint.h"

struct btree_node {
    int32_t key  [BTREE_FANOUT - 1];
    int32_t val  [BTREE_FANOUT - 1];
    int32_t child[BTREE_FANOUT];     /* node indices into g_nodes; -1 = none */
    int32_t n;                       /* number of keys */
    int32_t leaf;                    /* 1 if leaf */
    int32_t in_use;
};

struct btree {
    int32_t in_use;
    int32_t root;        /* node index */
};

static struct btree_node g_nodes[BTREE_MAX_NODES];
static struct btree      g_trees[BTREE_MAX_TREES];
static int g_initialized = 0;

void btree_init(void)
{
    if (g_initialized) return;
    for (int i = 0; i < BTREE_MAX_NODES; i++) g_nodes[i].in_use = 0;
    for (int i = 0; i < BTREE_MAX_TREES; i++) g_trees[i].in_use = 0;
    g_initialized = 1;
}

static int alloc_node(int leaf)
{
    for (int i = 0; i < BTREE_MAX_NODES; i++) {
        if (!g_nodes[i].in_use) {
            g_nodes[i].in_use = 1;
            g_nodes[i].n = 0;
            g_nodes[i].leaf = leaf ? 1 : 0;
            for (int j = 0; j < BTREE_FANOUT; j++) g_nodes[i].child[j] = -1;
            return i;
        }
    }
    return -1;
}

static void free_node(int idx)
{
    if (idx < 0 || idx >= BTREE_MAX_NODES) return;
    g_nodes[idx].in_use = 0;
}

btree_handle_t btree_new(void)
{
    btree_init();
    for (int i = 0; i < BTREE_MAX_TREES; i++) {
        if (!g_trees[i].in_use) {
            int r = alloc_node(1);
            if (r < 0) return -1;
            g_trees[i].in_use = 1;
            g_trees[i].root = r;
            return i;
        }
    }
    return -1;
}

int btree_tree_count(void)
{
    int n = 0;
    for (int i = 0; i < BTREE_MAX_TREES; i++) if (g_trees[i].in_use) n++;
    return n;
}

/* Split the i-th child of x.  Pre: x is not full, x->child[i] is full. */
static int split_child(int x_idx, int i)
{
    struct btree_node *x = &g_nodes[x_idx];
    int y_idx = x->child[i];
    struct btree_node *y = &g_nodes[y_idx];
    int z_idx = alloc_node(y->leaf);
    if (z_idx < 0) return -1;
    struct btree_node *z = &g_nodes[z_idx];

    int t = BTREE_FANOUT / 2;        /* min degree */
    z->n = t - 1;
    for (int j = 0; j < t - 1; j++) {
        z->key[j] = y->key[j + t];
        z->val[j] = y->val[j + t];
    }
    if (!y->leaf) {
        for (int j = 0; j < t; j++) z->child[j] = y->child[j + t];
    }
    y->n = t - 1;
    /* Shift x's children right and insert z. */
    for (int j = x->n; j >= i + 1; j--) x->child[j + 1] = x->child[j];
    x->child[i + 1] = z_idx;
    for (int j = x->n - 1; j >= i; j--) {
        x->key[j + 1] = x->key[j];
        x->val[j + 1] = x->val[j];
    }
    x->key[i] = y->key[t - 1];
    x->val[i] = y->val[t - 1];
    x->n++;
    return 0;
}

static int insert_nonfull(int x_idx, int32_t key, int32_t value);

static int insert_nonfull(int x_idx, int32_t key, int32_t value)
{
    struct btree_node *x = &g_nodes[x_idx];
    int i = x->n - 1;
    if (x->leaf) {
        /* Replace if key already present. */
        for (int j = 0; j < x->n; j++) {
            if (x->key[j] == key) { x->val[j] = value; return 0; }
        }
        while (i >= 0 && key < x->key[i]) {
            x->key[i + 1] = x->key[i];
            x->val[i + 1] = x->val[i];
            i--;
        }
        x->key[i + 1] = key;
        x->val[i + 1] = value;
        x->n++;
        return 0;
    }
    while (i >= 0 && key < x->key[i]) i--;
    if (i >= 0 && x->key[i] == key) { x->val[i] = value; return 0; }
    i++;
    int c_idx = x->child[i];
    if (g_nodes[c_idx].n == BTREE_FANOUT - 1) {
        if (split_child(x_idx, i) < 0) return -1;
        if (key > x->key[i]) i++;
        else if (key == x->key[i]) { x->val[i] = value; return 0; }
    }
    return insert_nonfull(x->child[i], key, value);
}

int btree_insert(btree_handle_t h, int32_t key, int32_t value)
{
    if (h < 0 || h >= BTREE_MAX_TREES || !g_trees[h].in_use) return -1;
    int r_idx = g_trees[h].root;
    if (g_nodes[r_idx].n == BTREE_FANOUT - 1) {
        int s_idx = alloc_node(0);
        if (s_idx < 0) return -1;
        g_nodes[s_idx].child[0] = r_idx;
        g_trees[h].root = s_idx;
        if (split_child(s_idx, 0) < 0) return -1;
        return insert_nonfull(s_idx, key, value);
    }
    return insert_nonfull(r_idx, key, value);
}

static int search_node(int n_idx, int32_t key, int32_t *out)
{
    if (n_idx < 0) return 0;
    struct btree_node *x = &g_nodes[n_idx];
    int i = 0;
    while (i < x->n && key > x->key[i]) i++;
    if (i < x->n && key == x->key[i]) { if (out) *out = x->val[i]; return 1; }
    if (x->leaf) return 0;
    return search_node(x->child[i], key, out);
}

int32_t btree_get(btree_handle_t h, int32_t key)
{
    if (h < 0 || h >= BTREE_MAX_TREES || !g_trees[h].in_use) return 0;
    int32_t v = 0;
    search_node(g_trees[h].root, key, &v);
    return v;
}

int btree_contains(btree_handle_t h, int32_t key)
{
    if (h < 0 || h >= BTREE_MAX_TREES || !g_trees[h].in_use) return 0;
    return search_node(g_trees[h].root, key, 0);
}

/* ─── Delete (Cormen ch 18.3) ────────────────────────────────────── */

static int t_min(void) { return BTREE_FANOUT / 2; }

static void merge_children(int x_idx, int i)
{
    struct btree_node *x = &g_nodes[x_idx];
    int y_idx = x->child[i];
    int z_idx = x->child[i + 1];
    struct btree_node *y = &g_nodes[y_idx];
    struct btree_node *z = &g_nodes[z_idx];
    int t = t_min();
    y->key[t - 1] = x->key[i];
    y->val[t - 1] = x->val[i];
    for (int j = 0; j < z->n; j++) {
        y->key[t + j] = z->key[j];
        y->val[t + j] = z->val[j];
    }
    if (!y->leaf) {
        for (int j = 0; j <= z->n; j++) y->child[t + j] = z->child[j];
    }
    y->n = t - 1 + z->n + 1;
    for (int j = i; j < x->n - 1; j++) {
        x->key[j] = x->key[j + 1];
        x->val[j] = x->val[j + 1];
    }
    for (int j = i + 1; j < x->n; j++) x->child[j] = x->child[j + 1];
    x->n--;
    free_node(z_idx);
}

static void borrow_from_prev(int x_idx, int i)
{
    struct btree_node *x = &g_nodes[x_idx];
    struct btree_node *child = &g_nodes[x->child[i]];
    struct btree_node *sib   = &g_nodes[x->child[i - 1]];
    for (int j = child->n - 1; j >= 0; j--) {
        child->key[j + 1] = child->key[j];
        child->val[j + 1] = child->val[j];
    }
    if (!child->leaf) {
        for (int j = child->n; j >= 0; j--)
            child->child[j + 1] = child->child[j];
    }
    child->key[0] = x->key[i - 1];
    child->val[0] = x->val[i - 1];
    if (!child->leaf) child->child[0] = sib->child[sib->n];
    x->key[i - 1] = sib->key[sib->n - 1];
    x->val[i - 1] = sib->val[sib->n - 1];
    child->n++;
    sib->n--;
}

static void borrow_from_next(int x_idx, int i)
{
    struct btree_node *x = &g_nodes[x_idx];
    struct btree_node *child = &g_nodes[x->child[i]];
    struct btree_node *sib   = &g_nodes[x->child[i + 1]];
    child->key[child->n] = x->key[i];
    child->val[child->n] = x->val[i];
    if (!child->leaf) child->child[child->n + 1] = sib->child[0];
    x->key[i] = sib->key[0];
    x->val[i] = sib->val[0];
    for (int j = 1; j < sib->n; j++) {
        sib->key[j - 1] = sib->key[j];
        sib->val[j - 1] = sib->val[j];
    }
    if (!sib->leaf) {
        for (int j = 1; j <= sib->n; j++) sib->child[j - 1] = sib->child[j];
    }
    child->n++;
    sib->n--;
}

static int delete_from(int x_idx, int32_t key);

static int delete_from(int x_idx, int32_t key)
{
    struct btree_node *x = &g_nodes[x_idx];
    int i = 0;
    while (i < x->n && key > x->key[i]) i++;

    if (i < x->n && x->key[i] == key) {
        if (x->leaf) {
            for (int j = i; j < x->n - 1; j++) {
                x->key[j] = x->key[j + 1];
                x->val[j] = x->val[j + 1];
            }
            x->n--;
            return 0;
        }
        /* Internal node: replace with predecessor or successor. */
        int t = t_min();
        int pred_idx = x->child[i];
        int succ_idx = x->child[i + 1];
        if (g_nodes[pred_idx].n >= t) {
            int p = pred_idx;
            while (!g_nodes[p].leaf) p = g_nodes[p].child[g_nodes[p].n];
            int32_t pk = g_nodes[p].key[g_nodes[p].n - 1];
            int32_t pv = g_nodes[p].val[g_nodes[p].n - 1];
            x->key[i] = pk;
            x->val[i] = pv;
            return delete_from(pred_idx, pk);
        }
        if (g_nodes[succ_idx].n >= t) {
            int p = succ_idx;
            while (!g_nodes[p].leaf) p = g_nodes[p].child[0];
            int32_t sk = g_nodes[p].key[0];
            int32_t sv = g_nodes[p].val[0];
            x->key[i] = sk;
            x->val[i] = sv;
            return delete_from(succ_idx, sk);
        }
        merge_children(x_idx, i);
        return delete_from(pred_idx, key);
    }
    if (x->leaf) return -1;

    int t = t_min();
    int child_idx = x->child[i];
    if (g_nodes[child_idx].n < t) {
        if (i > 0 && g_nodes[x->child[i - 1]].n >= t) {
            borrow_from_prev(x_idx, i);
        } else if (i < x->n && g_nodes[x->child[i + 1]].n >= t) {
            borrow_from_next(x_idx, i);
        } else {
            if (i < x->n) merge_children(x_idx, i);
            else { merge_children(x_idx, i - 1); i--; }
        }
    }
    return delete_from(x->child[i], key);
}

int btree_delete(btree_handle_t h, int32_t key)
{
    if (h < 0 || h >= BTREE_MAX_TREES || !g_trees[h].in_use) return -1;
    int r_idx = g_trees[h].root;
    int rc = delete_from(r_idx, key);
    /* If root has 0 keys and is internal, collapse. */
    if (g_nodes[r_idx].n == 0 && !g_nodes[r_idx].leaf) {
        int new_root = g_nodes[r_idx].child[0];
        free_node(r_idx);
        g_trees[h].root = new_root;
    }
    return rc;
}

/* ─── Range scan ─────────────────────────────────────────────────── */

static void range_walk(int n_idx, int32_t lo, int32_t hi,
                       void (*visit)(int32_t, int32_t, void *), void *ud,
                       int *count)
{
    if (n_idx < 0) return;
    struct btree_node *x = &g_nodes[n_idx];
    int i = 0;
    while (i < x->n && x->key[i] < lo) i++;
    while (i < x->n && x->key[i] <= hi) {
        if (!x->leaf) range_walk(x->child[i], lo, hi, visit, ud, count);
        if (visit) visit(x->key[i], x->val[i], ud);
        (*count)++;
        i++;
    }
    if (!x->leaf && i <= x->n) range_walk(x->child[i], lo, hi, visit, ud, count);
}

int btree_range(btree_handle_t h, int32_t lo, int32_t hi,
                void (*visit)(int32_t, int32_t, void *), void *ud)
{
    if (h < 0 || h >= BTREE_MAX_TREES || !g_trees[h].in_use) return 0;
    int count = 0;
    range_walk(g_trees[h].root, lo, hi, visit, ud, &count);
    return count;
}
