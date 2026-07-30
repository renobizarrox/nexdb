/* nexdb.h - a small database engine with an associative memory layer.
 *
 * Everything is hand-rolled C11 with no external dependencies:
 *   pager.c   paged file storage + slotted heap pages + persisted catalog
 *   memory.c  per-row memory strength (reinforce + decay) and Hebbian links
 *   lexer.c   tokenizer for the T-SQL-ish dialect
 *   parser.c  recursive-descent parser producing an AST
 *   exec.c    executor + result formatting
 *   main.c    interactive shell / script runner
 */
#ifndef NEXDB_H
#define NEXDB_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* ---------------------------------------------------------------- limits */

#define NEXDB_VERSION    "0.2"

#define PAGE_SIZE        4096
#define MAX_NAME         128
#define MAX_COLS         32
#define MAX_TABLES       128
#define MAX_INDEXES      32
#define MAX_ERR          256
#define MAX_SELECT_ITEMS 128
#define MAX_SET_ITEMS    64
#define MAX_IN_ITEMS     128
#define MAX_RECALL_TERMS 128   /* distinct search words RECALL considers */
#define MAX_INSERT_ROWS  128   /* rows per INSERT statement */
#define UNDO_MAX         256  /* max pages in a single transaction */

/* An entry in the in-memory undo log: the page number and its content before
 * the write that is part of an explicit transaction. */
typedef struct {
    uint32_t pno;
    uint8_t  old[PAGE_SIZE];
} UndoEntry;

/* ------------------------------------------------------- memory tuning
 * These constants define how the "brain" behaves. They are the knobs worth
 * playing with: a shorter half-life forgets faster, a bigger boost means a
 * single access matters more.
 */
#define MEM_HALFLIFE_SECS   (7.0 * 86400.0)  /* strength halves every week   */
#define MEM_INIT_STRENGTH   1.0              /* strength of a brand new row  */
#define MEM_BOOST           1.0              /* added on each access         */
#define MEM_MAX_STRENGTH    1000.0           /* saturation ceiling           */
#define MEM_LINK_BOOST      0.30             /* co-access edge reinforcement */
#define MEM_LINK_MAX        50.0
#define MEM_COACT_MAX       12               /* rows linked per statement    */
#define MEM_SPREAD_FACTOR   0.45             /* activation passed to a peer  */
#define MEM_LINK_CAP        (1u << 17)       /* max stored associations      */

/* ---------------------------------------------------------------- values */

typedef enum {
    T_NULL  = 0,
    T_INT   = 1,
    T_FLOAT = 2,
    T_TEXT  = 3,
    T_BIT   = 4,
    T_UUID  = 5      /* 16 packed bytes, rendered canonically */
} TypeTag;

typedef struct {
    uint8_t  tag;
    int64_t  i;      /* T_INT, T_BIT           */
    double   f;      /* T_FLOAT                */
    char    *s;      /* T_TEXT, heap-allocated */
    uint32_t slen;
    uint8_t  uu[16]; /* T_UUID                 */
} Value;

Value val_uuid(const uint8_t bytes[16]);
int   uuid_parse(const char *s, uint8_t out[16]);
void  uuid_format(const uint8_t in[16], char *out, size_t cap);
void  uuid_generate(uint8_t out[16]);

void        val_clear(Value *v);
Value       val_null(void);
Value       val_int(int64_t i);
Value       val_float(double f);
Value       val_bit(int b);
Value       val_text(const char *s);          /* copies */
Value       val_text_n(const char *s, size_t n);
Value       val_copy(const Value *v);
int         val_truthy(const Value *v);
int         val_compare(const Value *a, const Value *b, int *ok);
void        val_format(const Value *v, char *out, size_t cap);
const char *type_name(uint8_t tag);

/* --------------------------------------------------------------- catalog */

/* Integer width, so a declared INT actually rejects 5 billion. Values match
 * T-SQL's ranges. */
typedef enum {
    SUB_NONE = 0,
    SUB_TINYINT,    /* 0 .. 255                  */
    SUB_SMALLINT,   /* -32768 .. 32767           */
    SUB_INT,        /* -2^31 .. 2^31-1           */
    SUB_BIGINT      /* full 64-bit               */
} IntSub;

/* A DEFAULT has to survive in the catalog, so it is stored as data rather than
 * as a parse tree: either literal text to be coerced, or a marker for one of
 * the two functions worth allowing here. */
typedef enum {
    DFLT_NONE = 0,
    DFLT_LITERAL,
    DFLT_GETDATE,
    DFLT_NEWID
} DefaultKind;

typedef struct {
    char     name[MAX_NAME];
    uint8_t  type;         /* TypeTag                                     */
    uint8_t  not_null;
    uint8_t  is_pk;        /* PRIMARY KEY: implies not_null and unique    */
    uint8_t  unique;
    uint8_t  sub;          /* IntSub, for T_INT columns                   */
    uint8_t  is_datetime;  /* declared DATETIME: validate the text format */
    uint32_t maxlen;       /* declared character limit, 0 = unlimited     */

    uint8_t  dflt;         /* DefaultKind                                 */
    char     dflt_text[96];
    uint8_t  identity;     /* IDENTITY(seed, step)                        */
    int64_t  id_next;
    int64_t  id_step;
} Column;

int         int_range(uint8_t sub, int64_t *lo, int64_t *hi);
const char *int_sub_name(uint8_t sub);
int         valid_datetime(const char *s);
int         text_to_number(const char *s, double *out, int *is_int, int64_t *ival);

typedef struct {
    uint32_t root;
    int8_t   col;
    uint8_t  valid;
    uint8_t  _pad[2];
} Index;

typedef struct {
    char     name[MAX_NAME];
    int32_t  ncols;
    Column   cols[MAX_COLS];
    uint32_t first_page;
    uint32_t last_page;
    int64_t  nrows;
    int32_t  nindexes;
    Index    indexes[MAX_INDEXES];
} Table;

typedef struct {
    int32_t ntables;
    Table   tables[MAX_TABLES];
} Catalog;

/* ----------------------------------------------------------------- links */

typedef struct {
    uint64_t a, b;   /* a < b; a == 0 means empty slot */
    float    w;
} Link;

typedef struct {
    Link    *e;
    uint32_t cap;
    uint32_t n;
    int      dirty;
} LinkStore;

/* -------------------------------------------------------------- database */

typedef struct {
    int       fd;
    char      path[512];
    uint32_t  page_count;
    uint32_t  free_list;    /* head of singly-linked free page list */
    uint32_t  catalog_page;
    uint32_t  links_page;
    uint64_t  next_rid;
    Catalog   cat;
    LinkStore links;
    int       txn_active;   /* 1 between BEGIN and COMMIT/ROLLBACK */
    UndoEntry undo[UNDO_MAX];
    int       undo_depth;
    int       wal_fd;       /* WAL file descriptor, -1 if not open */
    char      wal_path[520];
    char      err[MAX_ERR];
} DB;

typedef struct {
    uint32_t page;
    uint16_t slot;
} RowRef;

typedef struct {
    RowRef   ref;
    uint64_t rid;
    uint32_t access_count;
    int64_t  last_access;
    float    strength;
    int32_t  ncols;
    Value    v[MAX_COLS];
} Row;

void row_clear(Row *r);

/* pager.c */
int      db_open(DB *db, const char *path);
void     db_close(DB *db);
int      pager_read(DB *db, uint32_t pno, void *buf);
int      pager_write(DB *db, uint32_t pno, const void *buf);
uint32_t pager_alloc(DB *db);
void     pager_free(DB *db, uint32_t pno);
int      pager_undo_capture(DB *db, uint32_t pno);
void     pager_undo_rollback(DB *db);
void     pager_undo_commit(DB *db);
int      db_flush_catalog(DB *db);
int      db_sync(DB *db);
int      db_write_links_blob(DB *db, const uint8_t *blob, size_t len);
int      db_read_links_blob(DB *db, uint8_t **out, size_t *len);
int      heap_update_meta(DB *db, RowRef ref, uint32_t access, int64_t last,
                          float strength);
int      heap_read_meta(DB *db, RowRef ref, uint32_t *access, int64_t *last,
                        float *strength);

Table *cat_find(DB *db, const char *name);
Table *cat_create(DB *db, const char *name, const Column *cols, int ncols);
int    cat_drop(DB *db, const char *name);
int    table_col_index(const Table *t, const char *name);

/* heap access */
int heap_insert(DB *db, Table *t, Row *r);
int heap_delete(DB *db, Table *t, RowRef ref);
int heap_replace(DB *db, Table *t, RowRef ref, Row *r);
void heap_free_pages(DB *db, Table *t);
int heap_read_row(DB *db, RowRef ref, Row *out);

typedef struct {
    DB      *db;
    Table   *t;
    uint32_t page;
    int      slot;
    int      loaded;
    uint8_t  buf[PAGE_SIZE];
} Scan;

void scan_init(Scan *s, DB *db, Table *t);
int  scan_next(Scan *s, Row *out);   /* 1 = row produced, 0 = exhausted */

/* memory.c */
int64_t mem_now(void);
double  mem_strength_at(double stored, int64_t last_access, int64_t now);
double  mem_row_strength(const Row *r, int64_t now);
int     mem_touch(DB *db, Table *t, RowRef ref, double boost);
void    mem_associate(DB *db, const uint64_t *rids, int n, double boost);
int     mem_neighbors(DB *db, uint64_t rid, uint64_t *out_rid, float *out_w, int max);
int     links_flush(DB *db);
int     links_load(DB *db);
void    links_free(DB *db);
double  mem_link_weight(DB *db, uint64_t a, uint64_t b);
int     mem_link_count(DB *db);
void    mem_link_set(DB *db, uint64_t a, uint64_t b, float w);
int  mem_top_links(DB *db, Link *out, int max);
void mem_forget_row(DB *db, uint64_t rid);

/* btree.c */
int  btree_create(Index *idx);
int  btree_find(DB *db, uint32_t root, const Value *key, RowRef *ref, char *err);
int  btree_insert(DB *db, uint32_t *root, const Value *key, RowRef ref, char *err);
int  btree_delete(DB *db, uint32_t *root, const Value *key, char *err);
int  btree_destroy(DB *db, uint32_t root);
int  btree_has_key(DB *db, uint32_t root, const Value *key, char *err);

/* index helpers in exec.c */
int  table_ensure_index(DB *db, Table *t, int col);
int  table_find_index(const Table *t, int col);

/* ----------------------------------------------------------------- lexer */

typedef enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_PUNCT
} TokKind;

/* Big enough to hold any literal that could fit in a row, since a row has to
 * fit inside one page. Overflowing this is reported as an error rather than
 * silently truncated - quietly losing the tail of someone's data is worse
 * than refusing it. */
#define MAX_TOKEN     4096   /* max length of a single token (string literal, identifier, etc.) */
#define MAX_ROW_SIZE (64 * 1024)  /* max serialised row; may span multiple pages via overflow chain */

typedef struct {
    TokKind kind;
    char    text[MAX_TOKEN];
    int64_t ival;
    double  fval;
    int     is_float;
    int     line;
} Token;

typedef struct {
    const char *src;
    size_t      pos;
    int         line;
    Token       cur;
    Token       ahead;
    int         has_ahead;
    char        err[MAX_ERR];
} Lexer;

void lex_init(Lexer *lx, const char *src);
int  lex_next(Lexer *lx);
Token *lex_peek(Lexer *lx);
int  tok_is_kw(const Token *t, const char *kw);
int  tok_is_punct(const Token *t, const char *p);

/* ------------------------------------------------------------------- AST */

typedef enum {
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
    OP_LIKE, OP_NOT_LIKE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD
} BinOp;

/* Aggregate functions are not evaluated per row like everything else, so they
 * get their own expression kind and are collected before execution. */
typedef enum {
    AGG_NONE = 0,
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX
} AggKind;

typedef enum {
    EX_LIT,
    EX_COL,
    EX_BIN,
    EX_NOT,
    EX_IS_NULL,
    EX_IS_NOT_NULL,
    EX_IN,
    EX_NEG,
    EX_FUNC,      /* scalar function call        */
    EX_CAST,      /* CAST(x AS type)             */
    EX_CASE,      /* CASE [x] WHEN..THEN..ELSE   */
    EX_BETWEEN,
    EX_AGG,       /* COUNT/SUM/AVG/MIN/MAX       */
    EX_SUBQUERY,  /* (SELECT ...) subquery       */
    EX_ANY,       /* = ANY / > ALL (subquery)    */
    EX_ALL        /* = ALL / > ALL (subquery)    */
} ExprKind;

#define MAX_FUNC_ARGS  16

/* Subquery types */
#define SUBQ_SCALAR 0
#define SUBQ_EXISTS 1

struct Stmt;

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    Value    lit;
    char     col[MAX_NAME];
    char     col_table[MAX_NAME];   /* table qualifier, empty if unqualified */
    BinOp    op;
    Expr    *l, *r;
    Expr    *items[MAX_IN_ITEMS];   /* IN list, or CASE WHEN/THEN pairs */
    int      nitems;
    int      negated;   /* NOT IN / NOT LIKE / NOT BETWEEN / NOT EXISTS */
    char     esc;       /* LIKE ... ESCAPE 'c', 0 = none    */

    /* EX_FUNC / EX_AGG / EX_BETWEEN */
    char     fname[MAX_NAME];
    Expr    *args[MAX_FUNC_ARGS];
    int      nargs;
    uint8_t  agg;        /* AggKind                      */
    uint8_t  agg_star;   /* COUNT(*)                     */
    uint8_t  agg_distinct; /* COUNT(DISTINCT x)          */
    int      agg_slot;   /* index into the group's states */

    /* EX_CAST */
    uint8_t  cast_type;
    uint8_t  cast_sub;
    uint32_t cast_len;

    /* EX_SUBQUERY / EX_IN (subquery) */
    struct Stmt *sub;
    uint8_t      subq_type;   /* SUBQ_SCALAR or SUBQ_EXISTS */
};

void expr_free(Expr *e);

/* A select-list entry is either '*' or an arbitrary expression. */
typedef struct {
    int   is_star;
    int   col_star;       /* t.* — star qualified by a table name */
    char  col_table[MAX_NAME];  /* table name for col_star */
    Expr *e;
    char  alias[MAX_NAME];
    char  label[MAX_NAME];   /* heading to print */
} SelItem;

typedef struct {
    Expr *e;
    int   desc;
} OrderKey;

typedef enum {
    JOIN_INNER,
    JOIN_LEFT
} JoinType;

typedef struct {
    JoinType type;
    char     table[MAX_NAME];
    char     alias[MAX_NAME];
    Expr    *on;
} Join;

#define MAX_ORDER_KEYS  16
#define MAX_GROUP_KEYS  16
#define MAX_JOINS       16
#define MAX_AGGS        32   /* aggregate functions (COUNT, SUM, etc) per query */

typedef struct {
    char  col[MAX_NAME];
    Expr *val;
} SetItem;

typedef enum {
    ST_NOOP = 0,
    ST_CREATE,
    ST_DROP,
    ST_ALTER_ADD,
    ST_ALTER_DROP,
    ST_ALTER_RENAME,
    ST_ALTER_TYPE,
    ST_TRUNCATE,
    ST_INSERT,
    ST_SELECT,
    ST_UPDATE,
    ST_DELETE,
    ST_RECALL,
    ST_REMEMBER,
    ST_FORGET,
    ST_SHOW_TABLES,
    ST_SHOW_MEMORY,
    ST_SHOW_LINKS,
    ST_PRINT,
    ST_CHECKPOINT,
    ST_BEGIN,
    ST_COMMIT,
    ST_ROLLBACK,
    ST_EXPLAIN,
    ST_VACUUM
} StKind;

typedef struct Stmt {
    StKind kind;
    char   table[MAX_NAME];
    int    if_exists;         /* DROP TABLE IF EXISTS */

    /* CREATE */
    Column cols[MAX_COLS];
    int    ncols;

    /* INSERT */
    char   ins_cols[MAX_COLS][MAX_NAME];
    int    n_ins_cols;
    Expr  *rows[MAX_INSERT_ROWS][MAX_COLS];
    int    nrows;
    int    row_width[MAX_INSERT_ROWS];

    /* SELECT / RECALL / SHOW */
    SelItem  items[MAX_SELECT_ITEMS];
    int      nitems;
    Expr    *where;
    int      distinct;
    int      has_from;
    char     alias[MAX_NAME];     /* table alias for the primary table */
    Expr    *group[MAX_GROUP_KEYS];
    int      ngroup;
    Expr    *having;
    OrderKey order[MAX_ORDER_KEYS];
    int      norder;
    struct Stmt *sub;         /* INSERT INTO ... SELECT */
    struct Stmt *derived;     /* FROM (SELECT ...) derived table */
    int      top;             /* -1 = unlimited */
    Join     joins[MAX_JOINS];
    int      njoins;
    /* sized to match the lexer's literal limit, so a RECALL phrase can never
     * be silently clipped on its way in */
    char    recall_text[MAX_TOKEN];

    /* UPDATE */
    SetItem sets[MAX_SET_ITEMS];
    int     nsets;

    /* PRINT */
    char msg[512];
} Stmt;

void stmt_free(Stmt *s);

/* parser.c */
int parse_stmt(Lexer *lx, Stmt *out, char *err);   /* 1 = stmt, 0 = end, -1 = error */

/* exec.c */
int exec_stmt(DB *db, Stmt *s, char *err);
int row_matches(const Expr *where, const Row *row, const Table *t,
                int64_t now, int *match, char *err);
void exec_set_reinforce(int on);
int  exec_reinforce_enabled(void);
int  row_matches_join(const Expr *where, const Table **tables,
                      const Row **rows, const char **aliases,
                      int ntables, int64_t now, int *match, char *err);
extern DB *g_db;

/* result formatting, shared between exec.c and select.c */
#define MAX_OUT_COLS 64

typedef struct {
    int    ncols;
    char   head[MAX_OUT_COLS][MAX_NAME];
    char **cells;
    int    nrows, cap;
} Grid;

void grid_init(Grid *g, int ncols);
int  grid_row(Grid *g, char **vals);
void grid_print(Grid *g);
void grid_free(Grid *g);

int  pseudo_col_index(const char *name);
void exec_set_agg_context(const Value *vals, int n);
void exec_set_join_ctx(const Table **tables, const Row **rows,
                       const char **aliases, int n);

/* select.c
 *
 * A Capture makes a SELECT hand its rows to the caller instead of printing
 * them, which is what INSERT ... SELECT needs. */
typedef struct {
    Value *cells;          /* nrows * ncols */
    int    ncols, nrows, cap;
    char   colnames[MAX_OUT_COLS][MAX_NAME]; /* column headings */
} Capture;

int  exec_select(DB *db, Stmt *s, char *err);
int  exec_select_into(DB *db, Stmt *s, Capture *cap, char *err);
void capture_free(Capture *c);

/* func.c */
int func_exists(const char *name);
int func_call(const char *name, Value *args, int n, Value *out, char *err);
int cast_value(const Value *in, uint8_t type, uint8_t sub, uint32_t len,
               Value *out, char *err);

/* parser.c */
int agg_kind_of(const char *name);
int exec_script(DB *db, const char *sql, int echo);
int eval_expr(const Expr *e, const Row *row, const Table *t, int64_t now,
              Value *out, char *err);
int like_match(const char *text, const char *pat);
int like_match_esc(const char *text, const char *pat, char esc);

#endif /* NEXDB_H */
