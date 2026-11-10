// database.c -- flat text-file storage for the banking server (replaces SQLite)
//
// Files (created in the directory the server is started from):
//   users.txt         user_id|username|password|role_id|is_active|is_logged_in
//   accounts.txt      account_id|user_id|balance
//   transactions.txt  txn_id|account_id|type|amount|details|timestamp
//   loans.txt         loan_id|account_id|amount|status|assigned_employee_id
//   feedback.txt      feedback_id|user_id|message|timestamp
//   bank.lock         empty file used only for flock()
//
// Concurrency: every server child process calls store_open(), which takes a
// flock() on bank.lock (shared for reads, exclusive for writes), loads the
// files into memory, and store_save() rewrites them via temp file + rename().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/file.h>

#define USERS_FILE     "users.txt"
#define ACCOUNTS_FILE  "accounts.txt"
#define TXNS_FILE      "transactions.txt"
#define LOANS_FILE     "loans.txt"
#define FEEDBACK_FILE  "feedback.txt"
#define LOCK_FILE      "bank.lock"

#define STORE_ERROR (-1)
#define STORE_BUSY  (-2)

typedef struct {
    int  user_id;
    char username[64];
    char password[64];
    int  role_id;        // 1=Customer, 2=Employee, 3=Manager, 4=Admin
    int  is_active;
    int  is_logged_in;
} User;

typedef struct {
    int    account_id;
    int    user_id;
    double balance;
} Account;

typedef struct {
    int    txn_id;
    int    account_id;
    char   type[24];     // DEPOSIT, WITHDRAW, TRANSFER, LOAN_CREDIT
    double amount;
    char   details[128];
    char   timestamp[24];
} Txn;

typedef struct {
    int    loan_id;
    int    account_id;
    double amount;
    char   status[16];   // PENDING, ASSIGNED, APPROVED, REJECTED
    int    assigned_employee_id;
} Loan;

typedef struct {
    int  feedback_id;
    int  user_id;
    char message[300];
    char timestamp[24];
} Feedback;

typedef struct {
    User     *users;     int n_users;
    Account  *accounts;  int n_accounts;
    Txn      *txns;      int n_txns;
    Loan     *loans;     int n_loans;
    Feedback *feedback;  int n_feedback;
    int       lock_fd;
} Store;

/* ---------- small helpers ---------- */

static void *grow_array(void *arr, int count, size_t elem) {
    void *p = realloc(arr, elem * (size_t)(count + 1));
    if (!p) { perror("realloc"); exit(1); }
    return p;
}
// append one zero-filled slot to arr and return a pointer to it
#define PUSH(arr, count) \
    (((arr) = grow_array((arr), (count), sizeof(*(arr)))), \
     memset(&(arr)[(count)], 0, sizeof(*(arr))), &(arr)[(count)++])

// copy text, turning field separators / newlines into spaces so records stay on one line
void store_copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++) {
        char c = src[i];
        dst[i] = (c == '|' || c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[i] = '\0';
}

static void stamp_now(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

// split "a|b|c" in place; the last field keeps any remaining text
static int split_fields(char *line, char **f, int max) {
    int n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    while (n < max) {
        f[n++] = line;
        if (n == max) break;
        char *p = strchr(line, '|');
        if (!p) break;
        *p = '\0';
        line = p + 1;
    }
    return n;
}

/* ---------- loading ---------- */

static void load_users(Store *s) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) return;
    char line[512], *f[6];
    while (fgets(line, sizeof line, fp)) {
        if (split_fields(line, f, 6) < 6) continue;
        User *u = PUSH(s->users, s->n_users);
        u->user_id = atoi(f[0]);
        store_copy_text(u->username, sizeof u->username, f[1]);
        store_copy_text(u->password, sizeof u->password, f[2]);
        u->role_id = atoi(f[3]);
        u->is_active = atoi(f[4]);
        u->is_logged_in = atoi(f[5]);
    }
    fclose(fp);
}

static void load_accounts(Store *s) {
    FILE *fp = fopen(ACCOUNTS_FILE, "r");
    if (!fp) return;
    char line[256], *f[3];
    while (fgets(line, sizeof line, fp)) {
        if (split_fields(line, f, 3) < 3) continue;
        Account *a = PUSH(s->accounts, s->n_accounts);
        a->account_id = atoi(f[0]);
        a->user_id = atoi(f[1]);
        a->balance = atof(f[2]);
    }
    fclose(fp);
}

static void load_txns(Store *s) {
    FILE *fp = fopen(TXNS_FILE, "r");
    if (!fp) return;
    char line[512], *f[6];
    while (fgets(line, sizeof line, fp)) {
        if (split_fields(line, f, 6) < 6) continue;
        Txn *t = PUSH(s->txns, s->n_txns);
        t->txn_id = atoi(f[0]);
        t->account_id = atoi(f[1]);
        store_copy_text(t->type, sizeof t->type, f[2]);
        t->amount = atof(f[3]);
        store_copy_text(t->details, sizeof t->details, f[4]);
        store_copy_text(t->timestamp, sizeof t->timestamp, f[5]);
    }
    fclose(fp);
}

static void load_loans(Store *s) {
    FILE *fp = fopen(LOANS_FILE, "r");
    if (!fp) return;
    char line[256], *f[5];
    while (fgets(line, sizeof line, fp)) {
        if (split_fields(line, f, 5) < 5) continue;
        Loan *l = PUSH(s->loans, s->n_loans);
        l->loan_id = atoi(f[0]);
        l->account_id = atoi(f[1]);
        l->amount = atof(f[2]);
        store_copy_text(l->status, sizeof l->status, f[3]);
        l->assigned_employee_id = atoi(f[4]);
    }
    fclose(fp);
}

static void load_feedback(Store *s) {
    FILE *fp = fopen(FEEDBACK_FILE, "r");
    if (!fp) return;
    char line[1024], *f[4];
    while (fgets(line, sizeof line, fp)) {
        if (split_fields(line, f, 4) < 4) continue;
        Feedback *fb = PUSH(s->feedback, s->n_feedback);
        fb->feedback_id = atoi(f[0]);
        fb->user_id = atoi(f[1]);
        store_copy_text(fb->message, sizeof fb->message, f[2]);
        store_copy_text(fb->timestamp, sizeof fb->timestamp, f[3]);
    }
    fclose(fp);
}

/* ---------- saving (temp file + atomic rename) ---------- */

static FILE *tmp_open(const char *name, char *tmp, size_t cap) {
    snprintf(tmp, cap, "%s.tmp", name);
    return fopen(tmp, "w");
}

static int tmp_commit(FILE *fp, const char *tmp, const char *name) {
    int ok = (fflush(fp) == 0 && fsync(fileno(fp)) == 0);
    if (fclose(fp) != 0) ok = 0;
    if (!ok) { unlink(tmp); return -1; }
    return rename(tmp, name) == 0 ? 0 : -1;
}

int store_save(Store *s) {
    FILE *fp;
    char tmp[64];
    int i;

    if (!(fp = tmp_open(USERS_FILE, tmp, sizeof tmp))) return -1;
    for (i = 0; i < s->n_users; i++) {
        User *u = &s->users[i];
        fprintf(fp, "%d|%s|%s|%d|%d|%d\n", u->user_id, u->username, u->password,
                u->role_id, u->is_active, u->is_logged_in);
    }
    if (tmp_commit(fp, tmp, USERS_FILE) != 0) return -1;

    if (!(fp = tmp_open(ACCOUNTS_FILE, tmp, sizeof tmp))) return -1;
    for (i = 0; i < s->n_accounts; i++) {
        Account *a = &s->accounts[i];
        fprintf(fp, "%d|%d|%.6f\n", a->account_id, a->user_id, a->balance);
    }
    if (tmp_commit(fp, tmp, ACCOUNTS_FILE) != 0) return -1;

    if (!(fp = tmp_open(TXNS_FILE, tmp, sizeof tmp))) return -1;
    for (i = 0; i < s->n_txns; i++) {
        Txn *t = &s->txns[i];
        fprintf(fp, "%d|%d|%s|%.6f|%s|%s\n", t->txn_id, t->account_id, t->type,
                t->amount, t->details, t->timestamp);
    }
    if (tmp_commit(fp, tmp, TXNS_FILE) != 0) return -1;

    if (!(fp = tmp_open(LOANS_FILE, tmp, sizeof tmp))) return -1;
    for (i = 0; i < s->n_loans; i++) {
        Loan *l = &s->loans[i];
        fprintf(fp, "%d|%d|%.6f|%s|%d\n", l->loan_id, l->account_id, l->amount,
                l->status, l->assigned_employee_id);
    }
    if (tmp_commit(fp, tmp, LOANS_FILE) != 0) return -1;

    if (!(fp = tmp_open(FEEDBACK_FILE, tmp, sizeof tmp))) return -1;
    for (i = 0; i < s->n_feedback; i++) {
        Feedback *fb = &s->feedback[i];
        fprintf(fp, "%d|%d|%s|%s\n", fb->feedback_id, fb->user_id, fb->message, fb->timestamp);
    }
    if (tmp_commit(fp, tmp, FEEDBACK_FILE) != 0) return -1;

    return 0;
}

/* ---------- open / close with locking ---------- */

// writable=1 -> exclusive lock (read-modify-write); writable=0 -> shared lock (read only)
// returns 0, STORE_BUSY (lock held by someone else too long) or STORE_ERROR
int store_open(Store *s, int writable) {
    memset(s, 0, sizeof *s);
    s->lock_fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0666);
    if (s->lock_fd < 0) {
        perror("Error opening lock file");
        return STORE_ERROR;
    }

    if (writable) {
        int tries = 0;
        while (flock(s->lock_fd, LOCK_EX | LOCK_NB) == -1) {
            if (errno != EWOULDBLOCK) {
                perror("flock");
                close(s->lock_fd);
                return STORE_ERROR;
            }
            if (++tries >= 100) {
                fprintf(stderr, "Storage locked: concurrent operation in progress.\n");
                close(s->lock_fd);
                return STORE_BUSY;
            }
            usleep(20000);
        }
    } else if (flock(s->lock_fd, LOCK_SH) == -1) {
        perror("flock");
        close(s->lock_fd);
        return STORE_ERROR;
    }

    load_users(s);
    load_accounts(s);
    load_txns(s);
    load_loans(s);
    load_feedback(s);
    return 0;
}

void store_close(Store *s) {
    free(s->users);
    free(s->accounts);
    free(s->txns);
    free(s->loans);
    free(s->feedback);
    flock(s->lock_fd, LOCK_UN);
    close(s->lock_fd);
    memset(s, 0, sizeof *s);
}

/* ---------- lookups ---------- */
// NOTE: returned pointers stay valid until a record is added to the same table.

User *store_user_by_id(Store *s, int id) {
    for (int i = 0; i < s->n_users; i++)
        if (s->users[i].user_id == id) return &s->users[i];
    return NULL;
}

User *store_user_by_name(Store *s, const char *name) {
    for (int i = 0; i < s->n_users; i++)
        if (strcmp(s->users[i].username, name) == 0) return &s->users[i];
    return NULL;
}

Account *store_account_of_user(Store *s, int user_id) {
    for (int i = 0; i < s->n_accounts; i++)
        if (s->accounts[i].user_id == user_id) return &s->accounts[i];
    return NULL;
}

Account *store_account_by_id(Store *s, int account_id) {
    for (int i = 0; i < s->n_accounts; i++)
        if (s->accounts[i].account_id == account_id) return &s->accounts[i];
    return NULL;
}

Loan *store_loan_by_id(Store *s, int loan_id) {
    for (int i = 0; i < s->n_loans; i++)
        if (s->loans[i].loan_id == loan_id) return &s->loans[i];
    return NULL;
}

/* ---------- inserts ---------- */

// creates a user (and returns it) or NULL if the username is already taken
User *store_add_user(Store *s, const char *username, const char *password, int role_id, int logged_in) {
    if (store_user_by_name(s, username)) return NULL;
    int next = 1;
    for (int i = 0; i < s->n_users; i++)
        if (s->users[i].user_id >= next) next = s->users[i].user_id + 1;
    User *u = PUSH(s->users, s->n_users);
    u->user_id = next;
    store_copy_text(u->username, sizeof u->username, username);
    store_copy_text(u->password, sizeof u->password, password);
    u->role_id = role_id;
    u->is_active = 1;
    u->is_logged_in = logged_in;
    return u;
}

Account *store_add_account(Store *s, int user_id, double balance) {
    int next = 10001;
    for (int i = 0; i < s->n_accounts; i++)
        if (s->accounts[i].account_id >= next) next = s->accounts[i].account_id + 1;
    Account *a = PUSH(s->accounts, s->n_accounts);
    a->account_id = next;
    a->user_id = user_id;
    a->balance = balance;
    return a;
}

Txn *store_add_txn(Store *s, int account_id, const char *type, double amount, const char *details) {
    int next = 1;
    for (int i = 0; i < s->n_txns; i++)
        if (s->txns[i].txn_id >= next) next = s->txns[i].txn_id + 1;
    Txn *t = PUSH(s->txns, s->n_txns);
    t->txn_id = next;
    t->account_id = account_id;
    store_copy_text(t->type, sizeof t->type, type);
    t->amount = amount;
    store_copy_text(t->details, sizeof t->details, details);
    stamp_now(t->timestamp, sizeof t->timestamp);
    return t;
}

Loan *store_add_loan(Store *s, int account_id, double amount) {
    int next = 1;
    for (int i = 0; i < s->n_loans; i++)
        if (s->loans[i].loan_id >= next) next = s->loans[i].loan_id + 1;
    Loan *l = PUSH(s->loans, s->n_loans);
    l->loan_id = next;
    l->account_id = account_id;
    l->amount = amount;
    strcpy(l->status, "PENDING");
    l->assigned_employee_id = 0;
    return l;
}

Feedback *store_add_feedback(Store *s, int user_id, const char *message) {
    int next = 1;
    for (int i = 0; i < s->n_feedback; i++)
        if (s->feedback[i].feedback_id >= next) next = s->feedback[i].feedback_id + 1;
    Feedback *fb = PUSH(s->feedback, s->n_feedback);
    fb->feedback_id = next;
    fb->user_id = user_id;
    store_copy_text(fb->message, sizeof fb->message, message);
    stamp_now(fb->timestamp, sizeof fb->timestamp);
    return fb;
}

/* ---------- startup / shutdown ---------- */

// creates the data files with sample users on first run, and clears stale
// login flags (sessions do not survive a server restart)
int db_init() {
    Store st;
    if (store_open(&st, 1) != 0) {
        fprintf(stderr, "Cannot open data files.\n");
        return -1;
    }

    if (st.n_users == 0) {
        store_add_user(&st, "cust1",  "pass",  1, 0);
        store_add_user(&st, "cust2",  "pass2", 1, 0);
        store_add_user(&st, "emp1",   "pass",  2, 0);
        store_add_user(&st, "mgr1",   "pass",  3, 0);
        store_add_user(&st, "admin1", "pass",  4, 0);
        // fixed ids so the sample data matches the documented logins
        st.users[0].user_id = 101;
        st.users[1].user_id = 102;
        st.users[2].user_id = 201;
        st.users[3].user_id = 301;
        st.users[4].user_id = 401;
        store_add_account(&st, 101, 5000.00);
        store_add_account(&st, 102, 200.00);
    }

    for (int i = 0; i < st.n_users; i++) st.users[i].is_logged_in = 0;

    int rc = store_save(&st);
    store_close(&st);
    if (rc != 0) {
        perror("Cannot write data files");
        return -1;
    }
    printf("DB: Data files checked/created and sample users loaded.\n");
    return 0;
}

void db_close() {
    // nothing to release: every operation opens and closes its own Store
}
