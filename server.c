#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include "database.c"   // flat-file storage layer (users.txt, accounts.txt, ...)

// compile:  gcc server.c -o server
#define PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 2048
#define MAX_TOKENS 10

typedef struct {
    int user_id;
    int role_id;
    int is_active;
} UserData;

/* ---------- helpers ---------- */

// append formatted text to buf (bounded by cap)
static void buf_add(char *buf, size_t cap, const char *fmt, ...) {
    size_t len = strlen(buf);
    if (len >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

// true while there is still room for one more list row
static int has_room(const char *buf) {
    return strlen(buf) + 300 < BUFFER_SIZE - strlen("SUCCESS|");
}

static int valid_amount(double v) {
    return v > 0.0 && v < 1e12;   // also rejects NaN
}

// opens the store; on failure fills response and returns 0
static int open_store(Store *st, int writable, char *response) {
    int rc = store_open(st, writable);
    if (rc == 0) return 1;
    if (rc == STORE_BUSY) strcpy(response, "ERROR|System busy. Try again.");
    else strcpy(response, "ERROR|Storage error.");
    return 0;
}

// saves and closes; returns 1 on success, otherwise fills response and returns 0
static int finish_store(Store *st, char *response) {
    int rc = store_save(st);
    store_close(st);
    if (rc != 0) {
        strcpy(response, "ERROR|Failed to save data.");
        return 0;
    }
    return 1;
}

/* ---------- sessions ---------- */

void clear_session_lock(int user_id);

void handle_logout(char *response, int user_id) {
    clear_session_lock(user_id);
    strcpy(response, "SUCCESS|Session terminated.");
}

// reap finished children so they don't stay as zombies
void sigchld_handler(int s) {
    (void)s;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void handle_admin_action(const char *action, const char *args[], int user_id, char *response);
void handle_manager_action(const char *action, const char *args[], int user_id, char *response);
void handle_employee_action(const char *action, const char *args[], int user_id, char *response);

void handle_create_customer(const char *username, const char *password, char *response) {
    Store st;
    if (!open_store(&st, 1, response)) return;

    // new customers are logged in straight away
    User *u = store_add_user(&st, username, password, 1, 1);
    if (!u) {
        store_close(&st);
        strcpy(response, "ERROR|Account creation failed (Username likely exists).");
        return;
    }
    int new_user_id = u->user_id;
    store_add_account(&st, new_user_id, 0.0);

    if (finish_store(&st, response)) snprintf(response, BUFFER_SIZE, "SUCCESS|%d", new_user_id);
}

// returns 1 ok, -2 role mismatch, -3 deactivated, -4 already logged in,
// -5 storage busy, 0 invalid credentials
int handle_login(const char *username, const char *password, int requested_role, UserData *result) {
    Store st;
    int rc = store_open(&st, 1);
    result->user_id = -1;
    if (rc == STORE_BUSY) return -5;
    if (rc != 0) return 0;

    int status = 0;
    User *u = store_user_by_name(&st, username);
    if (u && strcmp(u->password, password) == 0) {
        result->user_id = u->user_id;
        result->role_id = u->role_id;
        result->is_active = u->is_active;

        if (u->is_logged_in == 1) status = -4;
        else if (u->role_id != requested_role) status = -2;
        else if (u->is_active == 0) status = -3;
        else {
            u->is_logged_in = 1;
            status = (store_save(&st) == 0) ? 1 : 0;
        }
    }
    store_close(&st);
    return status;
}

void clear_session_lock(int user_id) {
    if (user_id <= 0) return;
    Store st;
    if (store_open(&st, 1) != 0) return;
    User *u = store_user_by_id(&st, user_id);
    if (u) {
        u->is_logged_in = 0;
        store_save(&st);
    }
    store_close(&st);
}

// 1 if the caller has a live session under the role it claims; otherwise fills response
static int session_ok(int user_id, int role_id, char *response) {
    Store st;
    if (!open_store(&st, 0, response)) return 0;
    User *u = store_user_by_id(&st, user_id);
    int ok = 0;
    if (!u || u->is_logged_in == 0) strcpy(response, "ERROR|SESSION_EXPIRED");
    else if (u->role_id != role_id) strcpy(response, "ERROR|Role mismatch.");
    else ok = 1;
    store_close(&st);
    return ok;
}

/* ---------- customer ---------- */

void handle_view_balance(int user_id, char *response) {
    Store st;
    if (!open_store(&st, 0, response)) return;
    User *u = store_user_by_id(&st, user_id);
    if (!u) {
        strcpy(response, "ERROR|Account not found.");
    } else {
        Account *a = store_account_of_user(&st, user_id);
        snprintf(response, BUFFER_SIZE, "SUCCESS|%.2f", a ? a->balance : 0.0);
    }
    store_close(&st);
}

void handle_deposit(int user_id, const char *amount_str, char *response) {
    double amount = atof(amount_str);
    if (!valid_amount(amount)) {
        strcpy(response, "ERROR|Invalid deposit amount.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    Account *a = store_account_of_user(&st, user_id);
    if (!a) {
        store_close(&st);
        strcpy(response, "ERROR|Deposit failed (DB error).");
        return;
    }
    a->balance += amount;
    store_add_txn(&st, a->account_id, "DEPOSIT", amount, "Self Deposit");

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Deposit successful.");
}

void handle_withdraw(int user_id, const char *amount_str, char *response) {
    double amount = atof(amount_str);
    if (!valid_amount(amount)) {
        strcpy(response, "ERROR|Invalid amount or Insufficient funds.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    Account *a = store_account_of_user(&st, user_id);
    if (!a || a->balance < amount) {
        store_close(&st);
        strcpy(response, "ERROR|Invalid amount or Insufficient funds.");
        return;
    }
    a->balance -= amount;
    store_add_txn(&st, a->account_id, "WITHDRAW", amount, "Self Withdrawal");

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Withdrawal successful.");
}

void handle_transfer(int user_id, const char *target_account_id_str, const char *amount_str, char *response) {
    double amount = atof(amount_str);
    int target_account_id = atoi(target_account_id_str);
    if (!valid_amount(amount)) {
        strcpy(response, "ERROR|Invalid amount or Insufficient funds.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    Account *src = store_account_of_user(&st, user_id);
    Account *dst = store_account_by_id(&st, target_account_id);

    if (!src || src->balance < amount) {
        store_close(&st);
        strcpy(response, "ERROR|Invalid amount or Insufficient funds.");
        return;
    }
    if (!dst || dst == src) {
        store_close(&st);
        strcpy(response, "ERROR|Transfer failed (DB error/Target not found).");
        return;
    }

    char details[128];
    src->balance -= amount;
    dst->balance += amount;
    snprintf(details, sizeof details, "Transfer OUT to Acc %d", target_account_id);
    store_add_txn(&st, src->account_id, "TRANSFER", amount, details);
    snprintf(details, sizeof details, "Transfer IN from user %d", user_id);
    store_add_txn(&st, dst->account_id, "TRANSFER", amount, details);

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Transfer successful.");
}

// newest-first transaction list for one user's account, at most `limit` rows
static void list_transactions(Store *st, int user_id, int limit, const char *header,
                              const char *empty_msg, char *response) {
    char details[BUFFER_SIZE];
    details[0] = '\0';
    buf_add(details, sizeof details, "%s", header);

    Account *a = store_account_of_user(st, user_id);
    int shown = 0;
    if (a) {
        for (int i = st->n_txns - 1; i >= 0 && shown < limit && has_room(details); i--) {
            Txn *t = &st->txns[i];
            if (t->account_id != a->account_id) continue;
            buf_add(details, sizeof details, "%s|%s|%.2f|%s\n", t->details, t->type, t->amount, t->timestamp);
            shown++;
        }
    }

    if (shown > 0) {
        strcpy(response, "SUCCESS|");
        strncat(response, details, BUFFER_SIZE - strlen(response) - 1);
    } else {
        snprintf(response, BUFFER_SIZE, "SUCCESS|%s", empty_msg);
    }
}

void handle_view_history(int user_id, char *response) {
    Store st;
    if (!open_store(&st, 0, response)) return;
    list_transactions(&st, user_id, 10, "History:\n", "No transactions found.", response);
    store_close(&st);
}

void handle_change_password(int user_id, const char *new_password, char *response) {
    Store st;
    if (!open_store(&st, 1, response)) return;

    User *u = store_user_by_id(&st, user_id);
    if (!u) {
        store_close(&st);
        strcpy(response, "ERROR|Failed to change password.");
        return;
    }
    store_copy_text(u->password, sizeof u->password, new_password);

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Password changed successfully.");
}

void handle_apply_loan(int user_id, const char *amount_str, char *response) {
    double amount = atof(amount_str);
    if (!valid_amount(amount)) {
        strcpy(response, "ERROR|Invalid loan amount.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    Account *a = store_account_of_user(&st, user_id);
    if (!a) {
        store_close(&st);
        strcpy(response, "ERROR|Loan submission failed (DB error).");
        return;
    }
    store_add_loan(&st, a->account_id, amount);

    if (finish_store(&st, response))
        strcpy(response, "SUCCESS|Loan application submitted successfully. Status: PENDING.");
}

void handle_add_feedback(int user_id, const char *message, char *response) {
    if (strlen(message) < 5) {
        strcpy(response, "ERROR|Feedback message is too short.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;
    store_add_feedback(&st, user_id, message);

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Feedback submitted. Thank you.");
}

/* ---------- manager ---------- */

void handle_view_pending_loans(char *response) {
    Store st;
    if (!open_store(&st, 0, response)) return;

    char details[BUFFER_SIZE];
    details[0] = '\0';
    buf_add(details, sizeof details, "Pending Loan Applications:\n");

    int shown = 0;
    for (int i = 0; i < st.n_loans && has_room(details); i++) {
        Loan *l = &st.loans[i];
        if (strcmp(l->status, "PENDING") != 0 && strcmp(l->status, "ASSIGNED") != 0) continue;
        buf_add(details, sizeof details, "[ID:%d] Acc:%d - $%.2f - Status:%s\n",
                l->loan_id, l->account_id, l->amount, l->status);
        shown++;
    }
    store_close(&st);

    if (shown > 0) {
        strcpy(response, "SUCCESS|");
        strncat(response, details, BUFFER_SIZE - strlen(response) - 1);
    } else {
        strcpy(response, "SUCCESS|No pending loan applications found.");
    }
}

void handle_activate_deactivate(const char *target_user_id_str, const char *status_str, char *response) {
    int target_user_id = atoi(target_user_id_str);
    int activate = (strcmp(status_str, "ACTIVATE") == 0);
    if (!activate && strcmp(status_str, "DEACTIVATE") != 0) {
        strcpy(response, "ERROR|Status must be ACTIVATE or DEACTIVATE.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    User *u = store_user_by_id(&st, target_user_id);
    if (!u || u->role_id != 1) {
        store_close(&st);
        strcpy(response, "ERROR|Failed to modify account status.");
        return;
    }
    u->is_active = activate ? 1 : 0;

    if (finish_store(&st, response))
        snprintf(response, BUFFER_SIZE, "SUCCESS|Account %d status set to %s.", target_user_id, status_str);
}

void handle_assign_loan(const char *loan_id_str, const char *employee_id_str, char *response) {
    int loan_id = atoi(loan_id_str);
    int employee_id = atoi(employee_id_str);
    Store st;
    if (!open_store(&st, 1, response)) return;

    Loan *l = store_loan_by_id(&st, loan_id);
    User *emp = store_user_by_id(&st, employee_id);
    if (!l || strcmp(l->status, "PENDING") != 0 || !emp || emp->role_id != 2) {
        store_close(&st);
        strcpy(response, "ERROR|Failed to assign loan (Loan may not be pending or IDs invalid).");
        return;
    }
    l->assigned_employee_id = employee_id;
    strcpy(l->status, "ASSIGNED");

    if (finish_store(&st, response))
        snprintf(response, BUFFER_SIZE, "SUCCESS|Loan %d assigned to Employee %d.", loan_id, employee_id);
}

void handle_review_feedback(char *response) {
    Store st;
    if (!open_store(&st, 0, response)) return;

    char details[BUFFER_SIZE];
    details[0] = '\0';
    buf_add(details, sizeof details, "Customer Feedback:\n");

    int shown = 0;
    for (int i = st.n_feedback - 1; i >= 0 && has_room(details); i--) {
        Feedback *fb = &st.feedback[i];
        buf_add(details, sizeof details, "[ID:%d, User:%d] %s (Time: %s)\n",
                fb->feedback_id, fb->user_id, fb->message, fb->timestamp);
        shown++;
    }
    store_close(&st);

    if (shown > 0) {
        strcpy(response, "SUCCESS|");
        strncat(response, details, BUFFER_SIZE - strlen(response) - 1);
    } else {
        strcpy(response, "SUCCESS|No feedback found.");
    }
}

/* ---------- employee ---------- */

void handle_add_new_customer(const char *username, const char *password, const char *initial_balance_str, char *response) {
    double initial_balance = atof(initial_balance_str);
    if (!(initial_balance >= 0.0) || initial_balance >= 1e12) {
        strcpy(response, "ERROR|Initial balance cannot be negative.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    User *u = store_add_user(&st, username, password, 1, 0);
    if (!u) {
        store_close(&st);
        strcpy(response, "ERROR|Creation failed (Username likely exists).");
        return;
    }
    store_add_account(&st, u->user_id, initial_balance);

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|New customer and account created.");
}

void handle_modify_customer(const char *target_user_id_str, const char *new_password, char *response) {
    int target_user_id = atoi(target_user_id_str);
    Store st;
    if (!open_store(&st, 1, response)) return;

    User *u = store_user_by_id(&st, target_user_id);
    if (!u || u->role_id != 1) {
        store_close(&st);
        strcpy(response, "ERROR|Failed to modify details (User may not exist or is not a Customer).");
        return;
    }
    store_copy_text(u->password, sizeof u->password, new_password);

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Customer details modified.");
}

void handle_view_passbook(const char *target_user_id_str, char *response) {
    int target_user_id = atoi(target_user_id_str);
    Store st;
    if (!open_store(&st, 0, response)) return;
    list_transactions(&st, target_user_id, 20, "Passbook History:\n",
                      "No transactions found for that user.", response);
    store_close(&st);
}

void handle_view_assigned_loans(int employee_id, char *response) {
    Store st;
    if (!open_store(&st, 0, response)) return;

    char details[BUFFER_SIZE];
    details[0] = '\0';
    buf_add(details, sizeof details, "Loans Assigned to You:\n");

    int shown = 0;
    for (int i = 0; i < st.n_loans && has_room(details); i++) {
        Loan *l = &st.loans[i];
        if (l->assigned_employee_id != employee_id) continue;
        if (strcmp(l->status, "ASSIGNED") != 0 && strcmp(l->status, "PROCESSING") != 0) continue;
        buf_add(details, sizeof details, "[ID:%d] Acc:%d - $%.2f - Status:%s\n",
                l->loan_id, l->account_id, l->amount, l->status);
        shown++;
    }
    store_close(&st);

    if (shown > 0) {
        strcpy(response, "SUCCESS|");
        strncat(response, details, BUFFER_SIZE - strlen(response) - 1);
    } else {
        strcpy(response, "SUCCESS|No loans currently assigned to you.");
    }
}

void handle_process_loan_application(const char *loan_id_str, const char *status_str, int employee_id, char *response) {
    int loan_id = atoi(loan_id_str);

    if (loan_id <= 0 || (strcmp(status_str, "APPROVED") != 0 && strcmp(status_str, "REJECTED") != 0)) {
        strcpy(response, "ERROR|Invalid Loan ID or status (Use APPROVED or REJECTED).");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    Loan *l = store_loan_by_id(&st, loan_id);
    if (!l || strcmp(l->status, "APPROVED") == 0) {
        store_close(&st);
        strcpy(response, "ERROR|Loan not found or already processed.");
        return;
    }

    if (strcmp(status_str, "APPROVED") == 0) {
        Account *a = store_account_by_id(&st, l->account_id);
        if (!a || l->amount <= 0) {
            store_close(&st);
            strcpy(response, "ERROR|Loan not found or already processed.");
            return;
        }
        a->balance += l->amount;
        strcpy(l->status, "APPROVED");
        l->assigned_employee_id = employee_id;
        store_add_txn(&st, a->account_id, "LOAN_CREDIT", l->amount, "Loan Approval");
    } else {
        strcpy(l->status, "REJECTED");
        l->assigned_employee_id = employee_id;
    }

    if (finish_store(&st, response))
        snprintf(response, BUFFER_SIZE, "SUCCESS|Loan %d processed: %s.", loan_id, status_str);
}

/* ---------- admin ---------- */

void handle_add_new_employee(const char *username, const char *password, char *response) {
    Store st;
    if (!open_store(&st, 1, response)) return;

    if (!store_add_user(&st, username, password, 2, 0)) {
        store_close(&st);
        strcpy(response, "ERROR|Creation failed (Username likely exists).");
        return;
    }
    if (finish_store(&st, response)) strcpy(response, "SUCCESS|New Bank Employee account created.");
}

void handle_admin_modify_user(const char *target_user_id_str, const char *new_password, char *response) {
    int target_user_id = atoi(target_user_id_str);
    if (target_user_id <= 0) {
        strcpy(response, "ERROR|Invalid target User ID.");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    User *u = store_user_by_id(&st, target_user_id);
    if (!u) {
        store_close(&st);
        strcpy(response, "ERROR|Failed to modify details (User ID invalid or DB error).");
        return;
    }
    store_copy_text(u->password, sizeof u->password, new_password);

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|Details (Password) modified for user.");
}

void handle_manage_user_roles(const char *target_user_id_str, const char *new_role_id_str, char *response) {
    int target_user_id = atoi(target_user_id_str);
    int new_role_id = atoi(new_role_id_str);
    if (target_user_id <= 0 || new_role_id < 1 || new_role_id > 4) {
        strcpy(response, "ERROR|Invalid User ID or Role ID (must be 1-4).");
        return;
    }
    Store st;
    if (!open_store(&st, 1, response)) return;

    User *u = store_user_by_id(&st, target_user_id);
    if (!u) {
        store_close(&st);
        strcpy(response, "ERROR|Failed to update user role.");
        return;
    }
    u->role_id = new_role_id;

    if (finish_store(&st, response)) strcpy(response, "SUCCESS|User role updated successfully.");
}

void handle_admin_action(const char *action, const char *args[], int user_id, char *response) {
    (void)args;
    snprintf(response, BUFFER_SIZE, "SUCCESS|Admin action '%s' by user %d acknowledged.", action, user_id);
}
void handle_manager_action(const char *action, const char *args[], int user_id, char *response) {
    (void)args;
    snprintf(response, BUFFER_SIZE, "SUCCESS|Manager action '%s' by user %d acknowledged.", action, user_id);
}
void handle_employee_action(const char *action, const char *args[], int user_id, char *response) {
    (void)args;
    snprintf(response, BUFFER_SIZE, "SUCCESS|Employee action '%s' by user %d acknowledged.", action, user_id);
}

/* ---------- request routing ---------- */

static void dispatch_request(const char *action, int role_id, int user_id,
                             char *tokens[], int num_tokens, char *response) {
    if (strcmp(action, "LOGOUT") == 0 && num_tokens == 3) {
        handle_logout(response, user_id);
    }
    else if (strcmp(action, "CREATE_CUSTOMER") == 0 && num_tokens == 4 && role_id == 1) {
        handle_create_customer(tokens[2], tokens[3], response);
    }
    else if (strcmp(action, "LOGIN") == 0 && num_tokens == 4) {
        UserData u_data;
        int login_status = handle_login(tokens[2], tokens[3], role_id, &u_data);
        if (login_status == 1) snprintf(response, BUFFER_SIZE, "SUCCESS|%d|%d", u_data.user_id, u_data.role_id);
        else if (login_status == -2) strcpy(response, "ERROR|Role mismatch.");
        else if (login_status == -3) strcpy(response, "ERROR|Account deactivated.");
        else if (login_status == -4) strcpy(response, "ERROR|ALREADY_LOGGED_IN");
        else if (login_status == -5) strcpy(response, "ERROR|System busy. Try again.");
        else strcpy(response, "ERROR|Invalid credentials or storage error.");
    }
    else if (role_id == 1) {
        if (strcmp(action, "VIEW_BALANCE") == 0) handle_view_balance(user_id, response);
        else if (strcmp(action, "DEPOSIT") == 0 && num_tokens == 4) handle_deposit(user_id, tokens[3], response);
        else if (strcmp(action, "WITHDRAW") == 0 && num_tokens == 4) handle_withdraw(user_id, tokens[3], response);
        else if (strcmp(action, "TRANSFER") == 0 && num_tokens == 5) handle_transfer(user_id, tokens[3], tokens[4], response);
        else if (strcmp(action, "VIEW_HISTORY") == 0) handle_view_history(user_id, response);
        else if (strcmp(action, "CHANGE_PASS") == 0 && num_tokens == 4) handle_change_password(user_id, tokens[3], response);
        else if (strcmp(action, "APPLY_LOAN") == 0 && num_tokens == 4) handle_apply_loan(user_id, tokens[3], response);
        else if (strcmp(action, "ADD_FEEDBACK") == 0 && num_tokens == 4) handle_add_feedback(user_id, tokens[3], response);
    }
    else if (role_id == 2) {
        if (strcmp(action, "ADD_CUSTOMER") == 0 && num_tokens == 6) handle_add_new_customer(tokens[3], tokens[4], tokens[5], response);
        else if (strcmp(action, "MOD_CUSTOMER") == 0 && num_tokens == 5) handle_modify_customer(tokens[3], tokens[4], response);
        else if (strcmp(action, "VIEW_PASSBOOK") == 0 && num_tokens == 4) handle_view_passbook(tokens[3], response);
        else if (strcmp(action, "VIEW_ASSIGNED_LOANS") == 0 && num_tokens == 3) handle_view_assigned_loans(user_id, response);
        else if (strcmp(action, "PROCESS_LOAN") == 0 && num_tokens == 5) handle_process_loan_application(tokens[3], tokens[4], user_id, response);
        else if (strcmp(action, "CHANGE_PASS") == 0 && num_tokens == 4) handle_change_password(user_id, tokens[3], response);
        else handle_employee_action(action, (const char **)&tokens[3], user_id, response);
    }
    else if (role_id == 3) {
        if (strcmp(action, "ACT_DEACT_ACC") == 0 && num_tokens == 5) handle_activate_deactivate(tokens[3], tokens[4], response);
        else if (strcmp(action, "VIEW_LOANS") == 0 && num_tokens == 3) handle_view_pending_loans(response);
        else if (strcmp(action, "ASSIGN_LOAN") == 0 && num_tokens == 5) handle_assign_loan(tokens[3], tokens[4], response);
        else if (strcmp(action, "REVIEW_FEEDBACK") == 0 && num_tokens == 3) handle_review_feedback(response);
        else if (strcmp(action, "CHANGE_PASS") == 0 && num_tokens == 4) handle_change_password(user_id, tokens[3], response);
        else handle_manager_action(action, (const char **)&tokens[3], user_id, response);
    }
    else if (role_id == 4) {
        if (strcmp(action, "ADD_EMPLOYEE") == 0 && num_tokens == 5) handle_add_new_employee(tokens[3], tokens[4], response);
        else if (strcmp(action, "MOD_USER_DETAILS") == 0 && num_tokens == 5) handle_admin_modify_user(tokens[3], tokens[4], response);
        else if (strcmp(action, "MANAGE_ROLES") == 0 && num_tokens == 5) handle_manage_user_roles(tokens[3], tokens[4], response);
        else if (strcmp(action, "CHANGE_PASS") == 0 && num_tokens == 4) handle_change_password(user_id, tokens[3], response);
        else handle_admin_action(action, (const char **)&tokens[3], user_id, response);
    }
}

void handle_child_process(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};
    char response[BUFFER_SIZE] = "ERROR|Unknown Request.";

    ssize_t bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("PID %d: Received: '%s'\n", getpid(), buffer);

        char *tokens[MAX_TOKENS] = {0};
        int i = 0;
        tokens[i] = strtok(buffer, "|");
        while (tokens[i] != NULL && i < MAX_TOKENS - 1) {
            tokens[++i] = strtok(NULL, "|");
        }
        int num_tokens = i;

        if (num_tokens >= 3) {
            const char *action = tokens[0];
            int role_id = atoi(tokens[1]);
            int user_id = atoi(tokens[2]);

            int needs_session = strcmp(action, "LOGIN") != 0 &&
                                strcmp(action, "LOGOUT") != 0 &&
                                strcmp(action, "CREATE_CUSTOMER") != 0;

            if (!needs_session || session_ok(user_id, role_id, response)) {
                dispatch_request(action, role_id, user_id, tokens, num_tokens, response);
            }
        }
    }

    send(client_fd, response, strlen(response), 0);
    close(client_fd);

    printf("Child PID %d finished and exiting.\n", getpid());
    exit(0);
}

int main() {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) { perror("sigaction failed"); return 1; }

    if (db_init() != 0) return 1;

    int sockfd, new_fd;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t sin_size;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) { perror("socket failed"); return 1; }
    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(server_addr.sin_zero), '\0', 8);
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind failed"); close(sockfd); return 1;
    }

    if (listen(sockfd, BACKLOG) == -1) { perror("listen failed"); close(sockfd); return 1; }

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        sin_size = sizeof(struct sockaddr_in);
        if ((new_fd = accept(sockfd, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
            perror("accept failed"); continue;
        }

        printf("Connection from %s:%d. Forking process (PID %d)...\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), getpid());

        if (fork() == 0) {
            close(sockfd);
            handle_child_process(new_fd);
        } else {
            close(new_fd);
        }
    }

    db_close();
    close(sockfd);
    return 0;
}
